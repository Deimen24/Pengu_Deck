// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * engine.c - audio device handling and the mixer
 */
#include <math.h>
#include <string.h>

#include <glib.h>

#include "engine.h"
#include "miniaudio.h"

#define REC_SECONDS	4
#define LIMIT		0.98f
#define LIM_RELEASE	0.99995f

struct engine_priv {
	ma_context ctx;
	ma_device dev;
	bool ctx_ok;
	bool dev_ok;

	float gain[2];		/* smoothed channel gains */
	float lim;		/* limiter gain */
	float deck_buf[2][DECK_MAX_BLOCK * 2];
	float mix[DECK_MAX_BLOCK * 2];
	float cue[DECK_MAX_BLOCK * 2];

	/* recorder */
	ma_pcm_rb rec_rb;
	ma_encoder rec_enc;
	GThread *rec_thread;
	atomic_bool rec_on;
	atomic_bool rec_stop;
	atomic_ullong rec_frames;
	atomic_uint rec_dropped;
};

void engine_init(struct engine *e)
{
	memset(e, 0, sizeof(*e));
	deck_init(&e->deck[0], 0);
	deck_init(&e->deck[1], 1);
	atomic_init(&e->master, 1.0f);
	atomic_init(&e->cue_mix, 0.0f);
	atomic_init(&e->cue_vol, 1.0f);
	e->priv = g_new0(struct engine_priv, 1);
	e->priv->lim = 1.0f;
}

void engine_fini(struct engine *e)
{
	engine_record_stop(e);
	engine_close(e);
	deck_fini(&e->deck[0]);
	deck_fini(&e->deck[1]);
	g_free(e->priv);
	e->priv = NULL;
}

/* ---- mixing ------------------------------------------------------ */

static void xfade_gains(int curve, float x, float *ga, float *gb)
{
	float p = (x + 1.0f) * 0.5f;

	switch (curve) {
	case XF_POWER:
		*ga = cosf(p * (float)M_PI_2);
		*gb = sinf(p * (float)M_PI_2);
		break;
	case XF_CUT:
		*ga = fminf(1.0f, (1.0f - p) * 16.0f);
		*gb = fminf(1.0f, p * 16.0f);
		break;
	case XF_DIPLESS:
	default:
		*ga = fminf(1.0f, (1.0f - p) * 2.0f);
		*gb = fminf(1.0f, p * 2.0f);
		break;
	}
}

static void mix_block(struct engine *e, unsigned int n)
{
	struct engine_priv *p = e->priv;
	float target[2], xa, xb, step[2];
	float master = atomic_load(&e->master);
	float pk_l = 0.0f, pk_r = 0.0f;
	unsigned int i;
	int k;

	xfade_gains(atomic_load(&e->xf_curve), atomic_load(&e->xfader),
		    &xa, &xb);
	for (k = 0; k < 2; k++) {
		float v = atomic_load(&e->deck[k].volume);

		target[k] = v * v * (k == 0 ? xa : xb);
		step[k] = (target[k] - p->gain[k]) / (float)n;
	}

	for (i = 0; i < 2 * n; i += 2) {
		float l, r, a;

		p->gain[0] += step[0];
		p->gain[1] += step[1];
		l = p->deck_buf[0][i] * p->gain[0] +
		    p->deck_buf[1][i] * p->gain[1];
		r = p->deck_buf[0][i + 1] * p->gain[0] +
		    p->deck_buf[1][i + 1] * p->gain[1];
		l *= master;
		r *= master;

		/* Peak limiter with instant attack and slow release. */
		a = fmaxf(fabsf(l), fabsf(r));
		if (a * p->lim > LIMIT)
			p->lim = LIMIT / a;
		else
			p->lim = fminf(1.0f, p->lim / LIM_RELEASE);
		l *= p->lim;
		r *= p->lim;

		p->mix[i] = l;
		p->mix[i + 1] = r;
		pk_l = fmaxf(pk_l, fabsf(l));
		pk_r = fmaxf(pk_r, fabsf(r));
	}
	p->gain[0] = target[0];
	p->gain[1] = target[1];

	if (pk_l > atomic_load(&e->peak_l))
		atomic_store(&e->peak_l, pk_l);
	if (pk_r > atomic_load(&e->peak_r))
		atomic_store(&e->peak_r, pk_r);
}

static void cue_block(struct engine *e, unsigned int n)
{
	struct engine_priv *p = e->priv;
	float mix = atomic_load(&e->cue_mix);
	float vol = atomic_load(&e->cue_vol);
	bool pfl[2];
	unsigned int i;

	pfl[0] = atomic_load(&e->deck[0].pfl);
	pfl[1] = atomic_load(&e->deck[1].pfl);

	for (i = 0; i < 2 * n; i++) {
		float c = 0.0f;

		if (pfl[0])
			c += p->deck_buf[0][i];
		if (pfl[1])
			c += p->deck_buf[1][i];
		c = (c * (1.0f - mix) + p->mix[i] * mix) * vol;
		p->cue[i] = fmaxf(-1.0f, fminf(1.0f, c));
	}
}

static void record_block(struct engine *e, unsigned int n)
{
	struct engine_priv *p = e->priv;
	ma_uint32 done = 0;

	if (!atomic_load(&p->rec_on))
		return;
	while (done < n) {
		ma_uint32 k = n - done;
		void *dst;

		if (ma_pcm_rb_acquire_write(&p->rec_rb, &k, &dst) != MA_SUCCESS
		    || k == 0) {
			atomic_fetch_add(&p->rec_dropped, n - done);
			return;
		}
		memcpy(dst, p->mix + 2 * done, k * 2 * sizeof(float));
		ma_pcm_rb_commit_write(&p->rec_rb, k);
		done += k;
	}
}

static void output_block(struct engine *e, float *out, unsigned int n)
{
	struct engine_priv *p = e->priv;
	unsigned int i, ch = e->channels;

	for (i = 0; i < n; i++) {
		float *o = out + i * ch;
		float ml = p->mix[2 * i], mr = p->mix[2 * i + 1];

		switch (e->hp_mode) {
		case HP_SPLIT:
			o[0] = 0.5f * (ml + mr);
			o[1] = 0.5f * (p->cue[2 * i] + p->cue[2 * i + 1]);
			break;
		case HP_CH34:
			o[0] = ml;
			o[1] = mr;
			if (ch >= 4) {
				o[2] = p->cue[2 * i];
				o[3] = p->cue[2 * i + 1];
			}
			break;
		case HP_OFF:
		default:
			o[0] = ml;
			o[1] = mr;
			break;
		}
	}
}

void engine_process(struct engine *e, float *out, unsigned int n)
{
	struct engine_priv *p = e->priv;

	while (n) {
		unsigned int k = n > DECK_MAX_BLOCK ? DECK_MAX_BLOCK : n;

		deck_render(&e->deck[0], p->deck_buf[0], k);
		deck_render(&e->deck[1], p->deck_buf[1], k);
		mix_block(e, k);
		if (e->hp_mode != HP_OFF)
			cue_block(e, k);
		record_block(e, k);
		output_block(e, out, k);

		out += k * e->channels;
		n -= k;
	}
}

static void data_cb(ma_device *dev, void *out, const void *in, ma_uint32 n)
{
	(void)in;
	engine_process(dev->pUserData, out, n);
}

static void notify_cb(const ma_device_notification *note)
{
	struct engine *e = note->pDevice->pUserData;

	if (note->type == ma_device_notification_type_rerouted ||
	    note->type == ma_device_notification_type_interruption_began)
		atomic_fetch_add(&e->xruns, 1);
}

/* ---- device management ------------------------------------------- */

static int backend_list(enum audio_backend b, ma_backend *list)
{
	switch (b) {
	case BACKEND_PULSE:
		list[0] = ma_backend_pulseaudio;
		return 1;
	case BACKEND_ALSA:
		list[0] = ma_backend_alsa;
		return 1;
	case BACKEND_JACK:
		list[0] = ma_backend_jack;
		return 1;
	case BACKEND_NULL:
		list[0] = ma_backend_null;
		return 1;
	case BACKEND_AUTO:
	default:
		list[0] = ma_backend_pulseaudio;
		list[1] = ma_backend_alsa;
		list[2] = ma_backend_jack;
		return 3;
	}
}

static int context_init(ma_context *ctx, enum audio_backend b)
{
	ma_context_config cfg = ma_context_config_init();
	ma_backend list[3];
	int n = backend_list(b, list);

	cfg.pulse.pApplicationName = "Pengu Deck";
	cfg.jack.pClientName = "pengu-deck";
	return ma_context_init(list, n, &cfg, ctx) == MA_SUCCESS ? 0 : -1;
}

char **engine_list_devices(enum audio_backend backend)
{
	GPtrArray *names = g_ptr_array_new();
	ma_device_info *info;
	ma_uint32 count, i;
	ma_context ctx;

	if (context_init(&ctx, backend) == 0) {
		if (ma_context_get_devices(&ctx, &info, &count, NULL, NULL) ==
		    MA_SUCCESS) {
			for (i = 0; i < count; i++)
				g_ptr_array_add(names, g_strdup(info[i].name));
		}
		ma_context_uninit(&ctx);
	}
	g_ptr_array_add(names, NULL);
	return (char **)g_ptr_array_free(names, FALSE);
}

static bool find_device(ma_context *ctx, const char *name, ma_device_id *id)
{
	ma_device_info *info;
	ma_uint32 count, i;

	if (!name || !*name)
		return false;
	if (ma_context_get_devices(ctx, &info, &count, NULL, NULL) !=
	    MA_SUCCESS)
		return false;
	for (i = 0; i < count; i++) {
		if (strcmp(info[i].name, name) == 0) {
			*id = info[i].id;
			return true;
		}
	}
	return false;
}

int engine_open(struct engine *e, const struct engine_opts *o, char **err)
{
	struct engine_priv *p = e->priv;
	ma_device_config cfg;
	ma_device_id id;
	char name[MA_MAX_DEVICE_NAME_LENGTH + 1];

	engine_close(e);
	if (context_init(&p->ctx, o->backend) != 0) {
		*err = g_strdup("Could not initialise any audio backend");
		return -1;
	}
	p->ctx_ok = true;

	cfg = ma_device_config_init(ma_device_type_playback);
	cfg.playback.format = ma_format_f32;
	cfg.playback.channels = o->hp_mode == HP_CH34 ? 4 : 2;
	cfg.sampleRate = o->rate;
	cfg.periodSizeInFrames = o->period;
	cfg.performanceProfile = ma_performance_profile_low_latency;
	cfg.noPreSilencedOutputBuffer = MA_TRUE;
	cfg.noClip = MA_TRUE;
	cfg.dataCallback = data_cb;
	cfg.notificationCallback = notify_cb;
	cfg.pUserData = e;
	if (find_device(&p->ctx, o->device, &id))
		cfg.playback.pDeviceID = &id;

	if (ma_device_init(&p->ctx, &cfg, &p->dev) != MA_SUCCESS) {
		*err = g_strdup("Could not open the audio output device");
		goto fail;
	}
	p->dev_ok = true;

	e->rate = p->dev.sampleRate;
	e->channels = p->dev.playback.channels;
	e->hp_mode = o->hp_mode;
	deck_set_rate(&e->deck[0], e->rate);
	deck_set_rate(&e->deck[1], e->rate);

	ma_device_get_name(&p->dev, ma_device_type_playback, name,
			   sizeof(name), NULL);
	g_free(e->device_name);
	e->device_name = g_strdup_printf("%s (%s)", name,
			ma_get_backend_name(p->ctx.backend));

	if (ma_device_start(&p->dev) != MA_SUCCESS) {
		*err = g_strdup("Could not start the audio output device");
		goto fail;
	}
	e->running = true;
	if (o->hp_mode == HP_CH34 && p->dev.playback.internalChannels < 4)
		*err = g_strdup_printf("The output device has only %u "
				       "channels, headphone cue on channels "
				       "3/4 is not available",
				       p->dev.playback.internalChannels);
	return 0;

fail:
	engine_close(e);
	return -1;
}

void engine_close(struct engine *e)
{
	struct engine_priv *p = e->priv;

	if (p->dev_ok) {
		ma_device_uninit(&p->dev);
		p->dev_ok = false;
	}
	if (p->ctx_ok) {
		ma_context_uninit(&p->ctx);
		p->ctx_ok = false;
	}
	e->running = false;
}

/* ---- recorder ---------------------------------------------------- */

static gpointer rec_thread(gpointer data)
{
	struct engine_priv *p = data;
	int16_t out[4096 * 2];

	for (;;) {
		bool stop = atomic_load(&p->rec_stop);
		ma_uint32 k = 4096, i;
		void *src;

		if (ma_pcm_rb_acquire_read(&p->rec_rb, &k, &src) !=
		    MA_SUCCESS)
			k = 0;
		if (k == 0) {
			if (stop)
				break;
			g_usleep(20000);
			continue;
		}
		for (i = 0; i < 2 * k; i++) {
			float v = ((float *)src)[i] * 32767.0f;

			out[i] = (int16_t)fmaxf(-32768.0f,
						fminf(32767.0f, v));
		}
		ma_pcm_rb_commit_read(&p->rec_rb, k);
		ma_encoder_write_pcm_frames(&p->rec_enc, out, k, NULL);
		atomic_fetch_add(&p->rec_frames, k);
	}
	return NULL;
}

int engine_record_start(struct engine *e, const char *path, char **err)
{
	struct engine_priv *p = e->priv;
	ma_encoder_config cfg;

	if (atomic_load(&p->rec_on))
		return 0;
	if (!e->running) {
		*err = g_strdup("The audio device is not running");
		return -1;
	}
	cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16,
				     2, e->rate);
	if (ma_encoder_init_file(path, &cfg, &p->rec_enc) != MA_SUCCESS) {
		*err = g_strdup_printf("Could not create %s", path);
		return -1;
	}
	if (ma_pcm_rb_init(ma_format_f32, 2, e->rate * REC_SECONDS, NULL,
			   NULL, &p->rec_rb) != MA_SUCCESS) {
		ma_encoder_uninit(&p->rec_enc);
		*err = g_strdup("Out of memory");
		return -1;
	}
	atomic_store(&p->rec_frames, 0);
	atomic_store(&p->rec_dropped, 0);
	atomic_store(&p->rec_stop, false);
	p->rec_thread = g_thread_new("pd-recorder", rec_thread, p);
	atomic_store(&p->rec_on, true);
	return 0;
}

void engine_record_stop(struct engine *e)
{
	struct engine_priv *p = e->priv;

	if (!p || !atomic_load(&p->rec_on))
		return;
	atomic_store(&p->rec_on, false);
	/* Let a callback that saw rec_on finish its write. */
	g_usleep(50000);
	atomic_store(&p->rec_stop, true);
	g_thread_join(p->rec_thread);
	p->rec_thread = NULL;
	ma_encoder_uninit(&p->rec_enc);
	ma_pcm_rb_uninit(&p->rec_rb);
}

bool engine_recording(struct engine *e)
{
	return atomic_load(&e->priv->rec_on);
}

double engine_record_seconds(struct engine *e)
{
	if (!e->rate)
		return 0.0;
	return (double)atomic_load(&e->priv->rec_frames) / e->rate;
}
