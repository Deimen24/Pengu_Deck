// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * fx.c - per channel effect unit
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "dsp.h"
#include "fx.h"

#define MAX_DELAY_SECS	4.0
#define COMBS		4
#define ALLPASSES	2
#define PHASER_STAGES	4
#define DEFAULT_BPM	128.0

const float fx_beat_steps[FX_BEAT_STEPS] = {
	0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f,
};

struct delay_line {
	float *buf;
	unsigned int len;
	unsigned int pos;
};

struct fx_state {
	enum fx_type cur;
	float wet_cur;

	/* echo, flanger */
	struct delay_line dl[2];
	/* reverb */
	struct delay_line comb[2][COMBS];
	float comb_lp[2][COMBS];
	struct delay_line ap[2][ALLPASSES];
	/* phaser */
	float ap_z[2][PHASER_STAGES];
	/* crusher */
	float hold[2];
	float phase_acc;
	/* lfo / gate */
	double phase;
	/* smoothing */
	float delay_cur;
};

const char *fx_name(enum fx_type t)
{
	static const char *const names[FX_COUNT] = {
		"No FX", "Echo", "Reverb", "Flanger", "Phaser", "Crush", "Gate",
	};

	return t >= 0 && t < FX_COUNT ? names[t] : "";
}

static void dl_alloc(struct delay_line *d, unsigned int len)
{
	g_free(d->buf);
	d->buf = g_new0(float, len);
	d->len = len;
	d->pos = 0;
}

static void dl_free(struct delay_line *d)
{
	g_free(d->buf);
	d->buf = NULL;
	d->len = 0;
}

static inline float dl_read(const struct delay_line *d, float delay)
{
	float p = (float)d->pos - delay;
	int i;
	float f;

	while (p < 0.0f)
		p += d->len;
	i = (int)p;
	f = p - i;
	return d->buf[i % d->len] * (1.0f - f) +
	       d->buf[(i + 1) % d->len] * f;
}

static inline void dl_write(struct delay_line *d, float v)
{
	d->buf[d->pos] = v;
	if (++d->pos >= d->len)
		d->pos = 0;
}

void fx_init(struct fx *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->wet, 0.5f);
	atomic_init(&f->beats, 0.5f);
	atomic_init(&f->param, 0.5f);
}

static void state_free(struct fx_state *s)
{
	int c, i;

	if (!s)
		return;
	for (c = 0; c < 2; c++) {
		dl_free(&s->dl[c]);
		for (i = 0; i < COMBS; i++)
			dl_free(&s->comb[c][i]);
		for (i = 0; i < ALLPASSES; i++)
			dl_free(&s->ap[c][i]);
	}
	g_free(s);
}

void fx_fini(struct fx *f)
{
	state_free(f->st);
	f->st = NULL;
}

void fx_set_rate(struct fx *f, unsigned int rate)
{
	static const float comb_ms[COMBS] = { 29.7f, 37.1f, 41.1f, 43.7f };
	static const float ap_ms[ALLPASSES] = { 5.0f, 1.7f };
	struct fx_state *s = g_new0(struct fx_state, 1);
	int c, i;

	state_free(f->st);
	f->rate = rate;
	for (c = 0; c < 2; c++) {
		dl_alloc(&s->dl[c], (unsigned int)(MAX_DELAY_SECS * rate));
		for (i = 0; i < COMBS; i++)
			dl_alloc(&s->comb[c][i], (unsigned int)(comb_ms[i] *
				 rate / 1000.0f * (c ? 1.07f : 1.0f)) + 1);
		for (i = 0; i < ALLPASSES; i++)
			dl_alloc(&s->ap[c][i],
				 (unsigned int)(ap_ms[i] * rate / 1000.0f) + 1);
	}
	f->st = s;
}

static void reset_state(struct fx_state *s)
{
	int c, i;

	for (c = 0; c < 2; c++) {
		memset(s->dl[c].buf, 0, s->dl[c].len * sizeof(float));
		for (i = 0; i < COMBS; i++) {
			memset(s->comb[c][i].buf, 0,
			       s->comb[c][i].len * sizeof(float));
			s->comb_lp[c][i] = 0.0f;
		}
		for (i = 0; i < ALLPASSES; i++)
			memset(s->ap[c][i].buf, 0,
			       s->ap[c][i].len * sizeof(float));
		memset(s->ap_z[c], 0, sizeof(s->ap_z[c]));
		s->hold[c] = 0.0f;
	}
	s->phase = 0.0;
	s->phase_acc = 0.0f;
	s->delay_cur = 0.0f;
}

/* ---- effects ----------------------------------------------------- */

static void echo(struct fx *f, struct fx_state *s, float *buf,
		 unsigned int n, float beat_frames, float wet, float param)
{
	float target = beat_frames * atomic_load(&f->beats);
	float fb = 0.3f + 0.6f * param;
	unsigned int i, c;

	if (target > s->dl[0].len - 2)
		target = s->dl[0].len - 2;
	if (s->delay_cur <= 0.0f)
		s->delay_cur = target;
	for (i = 0; i < n; i++) {
		s->delay_cur += (target - s->delay_cur) * 0.0005f;
		for (c = 0; c < 2; c++) {
			float in = buf[2 * i + c];
			float d = dl_read(&s->dl[c], s->delay_cur);

			dl_write(&s->dl[c], in + d * fb);
			buf[2 * i + c] = in + d * wet;
		}
	}
}

static void reverb(struct fx *f, struct fx_state *s, float *buf,
		   unsigned int n, float wet, float param)
{
	float fb = 0.7f + 0.27f * param;
	float damp = 0.3f;
	unsigned int i, c;
	int k;

	for (i = 0; i < n; i++) {
		for (c = 0; c < 2; c++) {
			float in = buf[2 * i + c];
			float acc = 0.0f, v;

			for (k = 0; k < COMBS; k++) {
				struct delay_line *d = &s->comb[c][k];
				float out = d->buf[d->pos];

				s->comb_lp[c][k] += damp * (out - s->comb_lp[c][k]);
				dl_write(d, in + s->comb_lp[c][k] * fb);
				acc += out;
			}
			v = acc * 0.25f;
			for (k = 0; k < ALLPASSES; k++) {
				struct delay_line *d = &s->ap[c][k];
				float out = d->buf[d->pos];
				float w = v + out * 0.5f;

				dl_write(d, w);
				v = out - w * 0.5f;
			}
			buf[2 * i + c] = in * (1.0f - wet * 0.5f) + v * wet;
		}
	}
}

static void flanger(struct fx *f, struct fx_state *s, float *buf,
		    unsigned int n, float beat_frames, float wet, float param)
{
	double period = beat_frames * atomic_load(&f->beats) * 2.0;
	float depth = (0.5f + 4.0f * param) * f->rate / 1000.0f;
	float base = 1.0f * f->rate / 1000.0f;
	float fb = 0.5f;
	unsigned int i, c;

	if (period < 1.0)
		period = 1.0;
	for (i = 0; i < n; i++) {
		float lfo = 0.5f - 0.5f * (float)cos(2.0 * M_PI * s->phase);
		float delay = base + depth * lfo;

		s->phase += 1.0 / period;
		if (s->phase >= 1.0)
			s->phase -= 1.0;
		for (c = 0; c < 2; c++) {
			float in = buf[2 * i + c];
			float d = dl_read(&s->dl[c], delay);

			dl_write(&s->dl[c], in + d * fb);
			buf[2 * i + c] = in + d * wet;
		}
	}
}

static void phaser(struct fx *f, struct fx_state *s, float *buf,
		   unsigned int n, float beat_frames, float wet, float param)
{
	double period = beat_frames * atomic_load(&f->beats) * 2.0;
	float depth = 0.3f + 0.6f * param;
	unsigned int i, c;
	int k;

	if (period < 1.0)
		period = 1.0;
	for (i = 0; i < n; i++) {
		float lfo = 0.5f - 0.5f * (float)cos(2.0 * M_PI * s->phase);
		float fc = 300.0f * powf(8.0f, lfo * depth);
		float a = (1.0f - (float)M_PI * fc / f->rate) /
			  (1.0f + (float)M_PI * fc / f->rate);

		s->phase += 1.0 / period;
		if (s->phase >= 1.0)
			s->phase -= 1.0;
		for (c = 0; c < 2; c++) {
			float in = buf[2 * i + c], v = in;

			for (k = 0; k < PHASER_STAGES; k++) {
				float y = -a * v + s->ap_z[c][k];

				s->ap_z[c][k] = v + a * y;
				v = y;
			}
			buf[2 * i + c] = in * (1.0f - wet * 0.5f) + v * wet * 0.5f;
		}
	}
}

static void crush(struct fx *f, struct fx_state *s, float *buf,
		  unsigned int n, float wet, float param)
{
	float bits = 12.0f - 8.0f * param;
	float steps = powf(2.0f, bits);
	float ratio = 1.0f + 15.0f * param;	/* sample rate divisor */
	unsigned int i, c;

	for (i = 0; i < n; i++) {
		s->phase_acc += 1.0f;
		if (s->phase_acc >= ratio) {
			s->phase_acc -= ratio;
			for (c = 0; c < 2; c++)
				s->hold[c] = roundf(buf[2 * i + c] * steps) /
					     steps;
		}
		for (c = 0; c < 2; c++)
			buf[2 * i + c] = buf[2 * i + c] * (1.0f - wet) +
					 s->hold[c] * wet;
	}
}

static void gate(struct fx *f, struct fx_state *s, float *buf,
		 unsigned int n, float beat_frames, float wet, float param)
{
	double period = beat_frames * atomic_load(&f->beats);
	float duty = 0.2f + 0.6f * param;
	unsigned int i, c;

	if (period < 1.0)
		period = 1.0;
	for (i = 0; i < n; i++) {
		float g = s->phase < duty ? 1.0f : 0.0f;
		float env;

		/* short ramps to avoid clicks */
		if (s->phase < 0.01)
			g = (float)(s->phase / 0.01);
		else if (s->phase > duty - 0.01 && s->phase < duty)
			g = (float)((duty - s->phase) / 0.01);
		env = 1.0f - wet + wet * g;
		s->phase += 1.0 / period;
		if (s->phase >= 1.0)
			s->phase -= 1.0;
		for (c = 0; c < 2; c++)
			buf[2 * i + c] *= env;
	}
}

void fx_process(struct fx *f, float *buf, unsigned int n, double bpm)
{
	struct fx_state *s = f->st;
	enum fx_type type = atomic_load(&f->type);
	float wet = atomic_load(&f->wet);
	float param = atomic_load(&f->param);
	float beat_frames;

	if (!s || !atomic_load(&f->on) || type == FX_NONE) {
		if (s)
			s->cur = FX_NONE;
		return;
	}
	if (type != s->cur) {
		reset_state(s);
		s->cur = type;
		s->wet_cur = 0.0f;
	}
	/* Fade the wet amount in over the first blocks after switching. */
	s->wet_cur += (wet - s->wet_cur) * 0.2f;
	wet = s->wet_cur;
	beat_frames = (float)(60.0 * f->rate / (bpm > 0.0 ? bpm : DEFAULT_BPM));

	switch (type) {
	case FX_ECHO:
		echo(f, s, buf, n, beat_frames, wet, param);
		break;
	case FX_REVERB:
		reverb(f, s, buf, n, wet, param);
		break;
	case FX_FLANGER:
		flanger(f, s, buf, n, beat_frames, wet, param);
		break;
	case FX_PHASER:
		phaser(f, s, buf, n, beat_frames, wet, param);
		break;
	case FX_CRUSH:
		crush(f, s, buf, n, wet, param);
		break;
	case FX_GATE:
		gate(f, s, buf, n, beat_frames, wet, param);
		break;
	default:
		break;
	}
}
