// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * deck.c - track playback, varispeed, keylock, scratching and EQ
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "pd-build.h"
#include "deck.h"

#ifdef HAVE_RUBBERBAND
#include <rubberband/rubberband-c.h>
#endif

#define FADE_STEP	(1.0f / 256.0f)
#define SCRATCH_MAX	12.0
#define CUE_TOLERANCE	0.02	/* seconds */

struct rb_state {
#ifdef HAVE_RUBBERBAND
	RubberBandState rb;
#endif
	bool active;
	float scale;
	int skip;		/* output frames still to discard */
	float *in[2];
	float *out[2];
	float *trash[2];
};

bool deck_has_keylock(void)
{
#ifdef HAVE_RUBBERBAND
	return true;
#else
	return false;
#endif
}

void deck_init(struct deck *d, int index)
{
	int i;

	memset(d, 0, sizeof(*d));
	d->index = index;
	atomic_init(&d->seek, DECK_NO_SEEK);
	atomic_init(&d->volume, 1.0f);
	/* A and C on the left of the crossfader, B and D on the right. */
	atomic_init(&d->xf_side, index % 2 ? XF_RIGHT : XF_LEFT);
	d->loop_beats = 4.0;
	for (i = 0; i < DECK_HOTCUES; i++)
		d->hotcue[i] = -1.0;
	d->tmp[0] = g_new0(float, DECK_MAX_BLOCK);
	d->tmp[1] = g_new0(float, DECK_MAX_BLOCK);
}

static void rb_free(struct rb_state *s)
{
	int c;

	if (!s)
		return;
#ifdef HAVE_RUBBERBAND
	if (s->rb)
		rubberband_delete(s->rb);
#endif
	for (c = 0; c < 2; c++) {
		g_free(s->in[c]);
		g_free(s->out[c]);
		g_free(s->trash[c]);
	}
	g_free(s);
}

void deck_fini(struct deck *d)
{
	struct track *t = atomic_exchange(&d->track, NULL);

	track_unref(t);
	rb_free(d->rb);
	d->rb = NULL;
	g_free(d->tmp[0]);
	g_free(d->tmp[1]);
}

void deck_set_rate(struct deck *d, unsigned int rate)
{
	int b, c;

	d->out_rate = rate;
	for (b = 0; b < EQ_BANDS; b++) {
		d->eq_cur[b] = NAN;
		for (c = 0; c < 2; c++)
			biquad_reset(&d->eq[b][c]);
	}
	d->flt_cur = NAN;
	biquad_reset(&d->flt[0]);
	biquad_reset(&d->flt[1]);

	rb_free(d->rb);
	d->rb = NULL;
#ifdef HAVE_RUBBERBAND
	d->rb = g_new0(struct rb_state, 1);
	d->rb->rb = rubberband_new(rate, 2,
				   RubberBandOptionProcessRealTime |
				   RubberBandOptionPitchHighConsistency |
				   RubberBandOptionTransientsMixed,
				   1.0, 1.0);
	rubberband_set_max_process_size(d->rb->rb, DECK_MAX_BLOCK);
	for (c = 0; c < 2; c++) {
		d->rb->in[c] = g_new0(float, DECK_MAX_BLOCK);
		d->rb->out[c] = g_new0(float, DECK_MAX_BLOCK);
		d->rb->trash[c] = g_new0(float, DECK_MAX_BLOCK);
	}
#endif
}

/* ---- audio thread ------------------------------------------------ */

static inline void get_frame(const struct track *t, long long i, size_t n,
			     float *l, float *r)
{
	const int16_t *f;

	if (i < 0 || (size_t)i >= n) {
		*l = *r = 0.0f;
		return;
	}
	f = track_frame(t, (size_t)i);
	*l = f[0] * (1.0f / 32768.0f);
	*r = f[1] * (1.0f / 32768.0f);
}

static inline float hermite(float x, float y0, float y1, float y2, float y3)
{
	float c1 = 0.5f * (y2 - y0);
	float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
	float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

	return ((c3 * x + c2) * x + c1) * x + y1;
}

static void sample_at(const struct track *t, double pos, size_t n,
		      float *l, float *r)
{
	long long i = (long long)floor(pos);
	float x = (float)(pos - (double)i);
	float l0, r0, l1, r1, l2, r2, l3, r3;

	get_frame(t, i - 1, n, &l0, &r0);
	get_frame(t, i, n, &l1, &r1);
	get_frame(t, i + 1, n, &l2, &r2);
	get_frame(t, i + 2, n, &l3, &r3);
	*l = hermite(x, l0, l1, l2, l3);
	*r = hermite(x, r0, r1, r2, r3);
}

/*
 * Read @n frames at @rate track frames per output frame into @l and @r,
 * advancing *pos, wrapping active loops and stopping at the end.
 */
static void read_varispeed(struct deck *d, const struct track *t,
			   double *pos, double rate, float *l, float *r,
			   unsigned int n)
{
	size_t frames = track_frames(t);
	bool done = track_done(t);
	bool loop = atomic_load(&d->loop_on);
	double lin = atomic_load(&d->loop_in);
	double lout = atomic_load(&d->loop_out);
	double p = *pos;
	unsigned int i;

	if (lout <= lin)
		loop = false;

	for (i = 0; i < n; i++) {
		if (p >= (double)frames && rate > 0.0) {
			/* Out of decoded data: end of track or buffering. */
			l[i] = r[i] = 0.0f;
			if (done) {
				atomic_store(&d->playing, false);
				p = (double)frames;
			}
			continue;
		}
		sample_at(t, p, frames, &l[i], &r[i]);
		p += rate;
		if (loop && rate > 0.0 && p >= lout && p - rate < lout)
			p -= lout - lin;
		if (p < 0.0)
			p = 0.0;
	}
	*pos = p;
}

#ifdef HAVE_RUBBERBAND
static void keylock_prime(struct deck *d, float scale)
{
	struct rb_state *s = d->rb;
	int pad, k;

	rubberband_reset(s->rb);
	rubberband_set_pitch_scale(s->rb, scale);
	s->scale = scale;

	memset(s->in[0], 0, DECK_MAX_BLOCK * sizeof(float));
	memset(s->in[1], 0, DECK_MAX_BLOCK * sizeof(float));
	pad = (int)rubberband_get_preferred_start_pad(s->rb);
	while (pad > 0) {
		k = pad > DECK_MAX_BLOCK ? DECK_MAX_BLOCK : pad;
		rubberband_process(s->rb, (const float *const *)s->in, k, 0);
		pad -= k;
	}
	/* The start delay is discarded as output becomes available. */
	s->skip = (int)rubberband_get_start_delay(s->rb);
	s->active = true;
}

/*
 * Keylock: read at varispeed and pitch shift the result back by the
 * inverse ratio, so the tempo changes but the key does not.
 */
static bool keylock_render(struct deck *d, const struct track *t,
			   double *pos, double rate, double tempo,
			   unsigned int n)
{
	struct rb_state *s = d->rb;
	float scale = (float)(1.0 / tempo);
	int guard = 64;

	if (!s)
		return false;
	if (!s->active)
		keylock_prime(d, scale);
	else if (fabsf(scale - s->scale) > 1e-5f) {
		rubberband_set_pitch_scale(s->rb, scale);
		s->scale = scale;
	}

	while (guard-- > 0) {
		int avail = rubberband_available(s->rb);
		int need;

		if (s->skip > 0 && avail > 0) {
			int k = avail < s->skip ? avail : s->skip;

			if (k > DECK_MAX_BLOCK)
				k = DECK_MAX_BLOCK;
			rubberband_retrieve(s->rb, s->trash, k);
			s->skip -= k;
			continue;
		}
		if (s->skip <= 0 && avail >= (int)n)
			break;
		need = (int)rubberband_get_samples_required(s->rb);
		if (need <= 0)
			need = 256;
		if (need > DECK_MAX_BLOCK)
			need = DECK_MAX_BLOCK;
		read_varispeed(d, t, pos, rate, s->in[0], s->in[1],
			       (unsigned int)need);
		rubberband_process(s->rb, (const float *const *)s->in, need, 0);
	}
	if (rubberband_available(s->rb) < (int)n) {
		memset(d->tmp[0], 0, n * sizeof(float));
		memset(d->tmp[1], 0, n * sizeof(float));
		return true;
	}
	rubberband_retrieve(s->rb, d->tmp, n);
	return true;
}
#endif

static void update_eq(struct deck *d)
{
	static const double freq[EQ_BANDS] = { 220.0, 1000.0, 3200.0 };
	static const enum biquad_kind kind[EQ_BANDS] = {
		BQ_LOWSHELF, BQ_PEAK, BQ_HIGHSHELF,
	};
	int b;

	for (b = 0; b < EQ_BANDS; b++) {
		float db = atomic_load(&d->eq_db[b]);

		if (atomic_load(&d->eq_kill[b]))
			db = -60.0f;
		if (db == d->eq_cur[b])
			continue;
		d->eq_cur[b] = db;
		/* Designing only touches coefficients, not filter state. */
		biquad_design(&d->eq[b][0], kind[b], d->out_rate, freq[b],
			      b == EQ_MID ? 0.6 : 0.7, db);
		biquad_design(&d->eq[b][1], kind[b], d->out_rate, freq[b],
			      b == EQ_MID ? 0.6 : 0.7, db);
	}
}

static void update_filter(struct deck *d)
{
	float f = atomic_load(&d->filter);
	enum biquad_kind kind;
	double hz;
	int c;

	if (fabsf(f - d->flt_cur) < 0.002f)
		return;
	d->flt_cur = f;
	for (c = 0; c < 2; c++) {
		if (fabsf(f) < 0.02f) {
			biquad_bypass(&d->flt[c]);
			continue;
		}
		if (f < 0.0f) {
			kind = BQ_LOWPASS;
			hz = 20000.0 * pow(120.0 / 20000.0, -f);
		} else {
			kind = BQ_HIGHPASS;
			hz = 20.0 * pow(8000.0 / 20.0, f);
		}
		biquad_design(&d->flt[c], kind, d->out_rate, hz, 1.0, 0.0);
	}
}

static void channel_strip(struct deck *d, float *out, unsigned int n)
{
	float trim = db_to_gain(atomic_load(&d->trim_db));
	float pl = 0.0f, pr = 0.0f;
	unsigned int i;
	int b;

	update_eq(d);
	update_filter(d);

	for (i = 0; i < n; i++) {
		float l = d->tmp[0][i] * trim;
		float r = d->tmp[1][i] * trim;

		for (b = 0; b < EQ_BANDS; b++) {
			l = biquad_run(&d->eq[b][0], l);
			r = biquad_run(&d->eq[b][1], r);
		}
		l = biquad_run(&d->flt[0], l);
		r = biquad_run(&d->flt[1], r);

		out[2 * i] = l;
		out[2 * i + 1] = r;
		if (fabsf(l) > pl)
			pl = fabsf(l);
		if (fabsf(r) > pr)
			pr = fabsf(r);
	}
	if (pl > atomic_load(&d->peak_l))
		atomic_store(&d->peak_l, pl);
	if (pr > atomic_load(&d->peak_r))
		atomic_store(&d->peak_r, pr);
}

static void apply_fade(struct deck *d, bool on, unsigned int n)
{
	unsigned int i;

	if (on && d->fade >= 1.0f)
		return;
	for (i = 0; i < n; i++) {
		d->fade += on ? FADE_STEP : -FADE_STEP;
		if (d->fade > 1.0f)
			d->fade = 1.0f;
		if (d->fade < 0.0f)
			d->fade = 0.0f;
		d->tmp[0][i] *= d->fade;
		d->tmp[1][i] *= d->fade;
	}
}

static void render_track(struct deck *d, struct track *t, unsigned int n)
{
	double pos = atomic_load(&d->pos);
	double seek = atomic_exchange(&d->seek, DECK_NO_SEEK);
	bool playing = atomic_load(&d->playing);
	bool scratch = atomic_load(&d->scratch);
	double tempo = 1.0 + atomic_load(&d->pitch) + atomic_load(&d->bend);
	double base = (double)t->rate / d->out_rate;
	bool locked = false;

	if (seek != DECK_NO_SEEK) {
		pos = seek;
		if (d->rb)
			d->rb->active = false;
	}

	if (scratch) {
		double want = (atomic_load(&d->scratch_target) - pos) / n;

		if (want > SCRATCH_MAX)
			want = SCRATCH_MAX;
		if (want < -SCRATCH_MAX)
			want = -SCRATCH_MAX;
		d->scr_rate += 0.35 * (want - d->scr_rate);
		read_varispeed(d, t, &pos, d->scr_rate, d->tmp[0], d->tmp[1],
			       n);
		if (d->rb)
			d->rb->active = false;
		d->fade = 1.0f;
		goto out;
	}

	if (!playing && d->fade <= 0.0f) {
		memset(d->tmp[0], 0, n * sizeof(float));
		memset(d->tmp[1], 0, n * sizeof(float));
		if (d->rb)
			d->rb->active = false;
		goto out;
	}

#ifdef HAVE_RUBBERBAND
	if (atomic_load(&d->keylock) && fabs(tempo - 1.0) > 1e-4)
		locked = keylock_render(d, t, &pos, base * tempo, tempo, n);
	else if (d->rb)
		d->rb->active = false;
#endif
	if (!locked)
		read_varispeed(d, t, &pos, base * tempo, d->tmp[0], d->tmp[1],
			       n);
	apply_fade(d, playing, n);
out:
	/* Do not clobber a seek that arrived while rendering. */
	if (atomic_load(&d->seek) == DECK_NO_SEEK)
		atomic_store(&d->pos, pos);
}

void deck_render(struct deck *d, float *out, unsigned int n)
{
	struct track *t;

	if (!d->out_rate) {
		memset(out, 0, n * 2 * sizeof(float));
		return;
	}
	atomic_store(&d->in_use, 1);
	t = atomic_load(&d->track);
	if (t && track_frames(t) > 0) {
		render_track(d, t, n);
	} else {
		memset(d->tmp[0], 0, n * sizeof(float));
		memset(d->tmp[1], 0, n * sizeof(float));
	}
	atomic_store(&d->in_use, 0);
	channel_strip(d, out, n);
}

/* ---- main thread ------------------------------------------------- */

void deck_load(struct deck *d, struct track *t)
{
	struct track *old;
	int i;

	atomic_store(&d->playing, false);
	atomic_store(&d->scratch, false);
	atomic_store(&d->loop_on, false);
	atomic_store(&d->loop_in, 0.0);
	atomic_store(&d->loop_out, 0.0);
	atomic_store(&d->seek, 0.0);
	d->cue = 0.0;
	d->cue_preview = false;
	for (i = 0; i < DECK_HOTCUES; i++)
		d->hotcue[i] = -1.0;

	old = atomic_exchange(&d->track, track_ref(t));
	while (atomic_load(&d->in_use))
		g_thread_yield();
	if (old) {
		atomic_store(&old->cancel, true);
		track_unref(old);
	}
	atomic_store(&d->pos, 0.0);
}

struct track *deck_track(struct deck *d)
{
	return atomic_load(&d->track);
}

double deck_position(struct deck *d)
{
	double s = atomic_load(&d->seek);

	return s != DECK_NO_SEEK ? s : atomic_load(&d->pos);
}

void deck_seek(struct deck *d, double frame)
{
	struct track *t = deck_track(d);
	double len;

	if (!t)
		return;
	len = (double)track_length(t);
	/* While loading the length is only a hint, let the seek through. */
	if (track_done(t) && frame > len)
		frame = len;
	if (frame < 0.0)
		frame = 0.0;
	atomic_store(&d->seek, frame);
}

void deck_play(struct deck *d, bool play)
{
	struct track *t = deck_track(d);

	d->cue_preview = false;
	if (!t) {
		atomic_store(&d->playing, false);
		return;
	}
	/* Restart from the top when play is hit at the very end. */
	if (play && track_done(t) &&
	    deck_position(d) >= (double)track_frames(t) - 1.0)
		deck_seek(d, d->cue);
	atomic_store(&d->playing, play);
}

static bool at_cue(struct deck *d, const struct track *t)
{
	return fabs(deck_position(d) - d->cue) < CUE_TOLERANCE * t->rate;
}

/* CDJ style cue: return and stop, set when paused, preview while held. */
void deck_cue_press(struct deck *d)
{
	struct track *t = deck_track(d);

	if (!t)
		return;
	if (atomic_load(&d->playing) && !d->cue_preview) {
		atomic_store(&d->playing, false);
		deck_seek(d, d->cue);
		return;
	}
	if (!at_cue(d, t)) {
		d->cue = deck_position(d);
		return;
	}
	d->cue_preview = true;
	atomic_store(&d->playing, true);
}

void deck_cue_release(struct deck *d)
{
	if (!d->cue_preview)
		return;
	d->cue_preview = false;
	atomic_store(&d->playing, false);
	deck_seek(d, d->cue);
}

void deck_hotcue(struct deck *d, int i)
{
	if (!deck_track(d) || i < 0 || i >= DECK_HOTCUES)
		return;
	if (d->hotcue[i] < 0.0) {
		d->hotcue[i] = deck_position(d);
		return;
	}
	d->cue_preview = false;
	deck_seek(d, d->hotcue[i]);
}

void deck_hotcue_clear(struct deck *d, int i)
{
	if (i >= 0 && i < DECK_HOTCUES)
		d->hotcue[i] = -1.0;
}

static double beat_len(struct track *t)
{
	double b = track_beat_len(t);

	return b > 0.0 ? b : t->rate * 0.5;
}

static double snap_to_beat(struct track *t, double pos)
{
	double b = track_beat_len(t);
	double off = atomic_load(&t->beat_offset);

	if (b <= 0.0)
		return pos;
	return off + round((pos - off) / b) * b;
}

void deck_loop_beats(struct deck *d, double beats)
{
	struct track *t = deck_track(d);
	double in;

	if (!t)
		return;
	d->loop_beats = beats;
	if (atomic_load(&d->loop_on)) {
		in = atomic_load(&d->loop_in);
	} else {
		in = snap_to_beat(t, deck_position(d));
		if (in < 0.0)
			in = 0.0;
	}
	atomic_store(&d->loop_in, in);
	atomic_store(&d->loop_out, in + beats * beat_len(t));
	atomic_store(&d->loop_on, true);
}

void deck_loop_set_in(struct deck *d)
{
	if (!deck_track(d))
		return;
	atomic_store(&d->loop_on, false);
	atomic_store(&d->loop_in, deck_position(d));
	atomic_store(&d->loop_out, 0.0);
}

void deck_loop_set_out(struct deck *d)
{
	double pos = deck_position(d);

	if (!deck_track(d) || pos <= atomic_load(&d->loop_in))
		return;
	atomic_store(&d->loop_out, pos);
	atomic_store(&d->loop_on, true);
	deck_seek(d, atomic_load(&d->loop_in));
}

void deck_loop_toggle(struct deck *d)
{
	double in = atomic_load(&d->loop_in);
	double out = atomic_load(&d->loop_out);

	if (atomic_load(&d->loop_on)) {
		atomic_store(&d->loop_on, false);
		return;
	}
	if (out <= in)
		return;
	atomic_store(&d->loop_on, true);
	/* Reloop: jump back in when we are already past the loop. */
	if (deck_position(d) >= out)
		deck_seek(d, in);
}

void deck_loop_scale(struct deck *d, double factor)
{
	double in = atomic_load(&d->loop_in);
	double out = atomic_load(&d->loop_out);
	double len = (out - in) * factor;

	if (out <= in || len < 16.0)
		return;
	d->loop_beats *= factor;
	atomic_store(&d->loop_out, in + len);
}

double deck_rate(struct deck *d)
{
	return 1.0 + atomic_load(&d->pitch) + atomic_load(&d->bend);
}

double deck_bpm(struct deck *d)
{
	struct track *t = deck_track(d);

	if (!t)
		return 0.0;
	return atomic_load(&t->bpm) * (1.0 + atomic_load(&d->pitch));
}

/* Returns the pitch that matches @master's tempo, or NAN. */
double deck_sync_pitch(struct deck *d, struct deck *master)
{
	struct track *t = deck_track(d);
	double target = deck_bpm(master);
	double bpm, ratio, best = NAN;
	static const double mult[] = { 1.0, 2.0, 0.5 };
	size_t i;

	if (!t || target <= 0.0)
		return NAN;
	bpm = atomic_load(&t->bpm);
	if (bpm <= 0.0)
		return NAN;
	for (i = 0; i < G_N_ELEMENTS(mult); i++) {
		ratio = target * mult[i] / bpm;
		if (isnan(best) || fabs(ratio - 1.0) < fabs(best))
			best = ratio - 1.0;
	}
	return best;
}

void deck_sync_phase(struct deck *d, struct deck *master)
{
	struct track *t = deck_track(d), *mt = deck_track(master);
	double b, mb, off, moff, mphase, cur, want;

	if (!t || !mt || !atomic_load(&master->playing))
		return;
	b = track_beat_len(t);
	mb = track_beat_len(mt);
	if (b <= 0.0 || mb <= 0.0)
		return;
	off = atomic_load(&t->beat_offset);
	moff = atomic_load(&mt->beat_offset);

	mphase = (deck_position(master) - moff) / mb;
	mphase -= floor(mphase);
	cur = (deck_position(d) - off) / b;
	want = floor(cur) + mphase;
	if (want - cur > 0.5)
		want -= 1.0;
	else if (cur - want > 0.5)
		want += 1.0;
	deck_seek(d, off + want * b);
}
