// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * analyze.c - tempo and beat grid detection
 *
 * The signal is reduced to an onset strength envelope (half wave rectified
 * derivative of log energy, with extra weight on the bass band where kick
 * drums live).  A coarse tempo is the lag maximising a comb of
 * autocorrelation values at one to four beats, with a mild preference for
 * common dance music tempos.  The estimate is then refined by folding the
 * whole envelope onto a beat period: only the exact tempo keeps the onsets
 * stacked in one phase bin over several minutes of music.  The winning
 * phase bin gives the beat grid offset.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "analyze.h"

#define ENV_HOP		128
#define MAX_SECONDS	360
#define PHASE_BINS	96

double bpm_fold(double bpm)
{
	if (bpm <= 0.0)
		return 0.0;
	while (bpm < BPM_MIN)
		bpm *= 2.0;
	while (bpm >= BPM_MAX)
		bpm *= 0.5;
	return bpm;
}

static void remove_local_mean(float *env, size_t n)
{
	const size_t w = 16;
	float *mean = malloc(n * sizeof(*mean));
	double acc = 0.0;
	size_t i;

	if (!mean)
		return;
	for (i = 0; i < n; i++) {
		acc += env[i];
		if (i >= 2 * w)
			acc -= env[i - 2 * w];
		mean[i] = (float)(acc / (double)(2 * w));
	}
	for (i = 0; i < n; i++) {
		size_t k = i + w < n ? i + w : n - 1;
		float v = env[i] - mean[k];

		env[i] = v > 0.0f ? v : 0.0f;
	}
	free(mean);
}

/* Returns the envelope and stores its length and first frame. */
static float *onset_envelope(const struct track *t, size_t *len,
			     size_t *first)
{
	size_t frames = track_frames(t);
	size_t max = (size_t)MAX_SECONDS * t->rate;
	size_t start = 0, n, i, j;
	float a = 1.0f - expf(-2.0f * (float)M_PI * 150.0f / t->rate);
	float lp = 0.0f, prev = 0.0f;
	float *env;

	/* Analyse the middle of long tracks, intros are often beatless. */
	if (frames > max) {
		start = (frames - max) / 2;
		frames = max;
	}
	n = frames / ENV_HOP;
	if (n < 256)
		return NULL;

	env = malloc(n * sizeof(*env));
	if (!env)
		return NULL;

	for (i = 0; i < n; i++) {
		float e_low = 0.0f, e_all = 0.0f, cur;

		for (j = 0; j < ENV_HOP; j++) {
			const int16_t *f = track_frame(t, start +
						       i * ENV_HOP + j);
			float m = (f[0] + f[1]) * (0.5f / 32768.0f);

			lp += a * (m - lp);
			e_low += lp * lp;
			e_all += m * m;
		}
		cur = logf(1e-7f + 4.0f * e_low + e_all);
		env[i] = cur > prev ? cur - prev : 0.0f;
		prev = cur;
	}
	env[0] = 0.0f;
	remove_local_mean(env, n);

	*len = n;
	*first = start;
	return env;
}

static double *autocorr(const float *env, size_t n, size_t max_lag)
{
	double *acf = calloc(max_lag + 2, sizeof(*acf));
	size_t lag, i;

	if (!acf)
		return NULL;
	for (lag = 1; lag <= max_lag + 1 && lag < n; lag++) {
		double s = 0.0;

		for (i = 0; i + lag < n; i++)
			s += (double)env[i] * env[i + lag];
		acf[lag] = s / (double)(n - lag);
	}
	return acf;
}

static double acf_at(const double *acf, size_t max_lag, double lag)
{
	size_t i = (size_t)lag;
	double f = lag - (double)i;

	if (i + 1 > max_lag + 1)
		return 0.0;
	return acf[i] * (1.0 - f) + acf[i + 1] * f;
}

static double tempo_prior(double bpm)
{
	double x = log2(bpm / 124.0) / 0.9;

	return exp(-0.5 * x * x);
}

static double coarse_tempo(const float *env, size_t n, double env_rate)
{
	size_t max_lag = (size_t)(4.0 * 60.0 * env_rate / BPM_MIN) + 2;
	double best = 0.0, best_score = -1.0, bpm;
	double *acf = autocorr(env, n, max_lag);

	if (!acf)
		return 0.0;
	for (bpm = BPM_MIN; bpm < BPM_MAX; bpm += 0.05) {
		double lag = 60.0 * env_rate / bpm;
		double s = 0.0;
		int k;

		for (k = 1; k <= 4; k++)
			s += acf_at(acf, max_lag, lag * k);
		s *= tempo_prior(bpm);
		if (s > best_score) {
			best_score = s;
			best = bpm;
		}
	}
	free(acf);
	return best_score > 0.0 ? best : 0.0;
}

/*
 * Fold the envelope onto a grid of @period hops.  Returns the height of
 * the strongest (lightly smoothed) phase bin and stores the bin index.
 */
static double fold_score(const float *env, size_t n, double period,
			 int *phase)
{
	double hist[PHASE_BINS] = { 0 };
	double best = -1.0, scale = PHASE_BINS / period;
	size_t i;
	int b;

	for (i = 0; i < n; i++) {
		double p = fmod((double)i, period) * scale;

		hist[(int)p % PHASE_BINS] += env[i];
	}
	for (b = 0; b < PHASE_BINS; b++) {
		double s = hist[b] * 2.0 +
			   hist[(b + 1) % PHASE_BINS] +
			   hist[(b + PHASE_BINS - 1) % PHASE_BINS];

		if (s > best) {
			best = s;
			*phase = b;
		}
	}
	return best;
}

int analyze_tempo(const struct track *t, double *bpm, double *offset)
{
	double env_rate = (double)t->rate / ENV_HOP;
	double coarse, fine = 0.0, best = -1.0, b, period, beat;
	size_t n, first;
	int phase = 0, p;
	float *env = onset_envelope(t, &n, &first);

	if (!env)
		return -1;

	coarse = coarse_tempo(env, n, env_rate);
	if (coarse <= 0.0) {
		free(env);
		return -1;
	}

	for (b = coarse - 0.4; b <= coarse + 0.4; b += 0.0025) {
		double s = fold_score(env, n, 60.0 * env_rate / b, &p);

		if (s > best) {
			best = s;
			fine = b;
			phase = p;
		}
	}

	/* Most produced music sits on an integer or half integer tempo. */
	if (fabs(fine - round(fine)) < 0.04)
		fine = round(fine);
	else if (fabs(fine * 2.0 - round(fine * 2.0)) < 0.04)
		fine = round(fine * 2.0) / 2.0;

	period = 60.0 * env_rate / fine;
	fold_score(env, n, period, &phase);
	free(env);

	beat = 60.0 * t->rate / fine;
	*bpm = fine;
	*offset = fmod((double)first +
		       (phase + 0.5) * period / PHASE_BINS * ENV_HOP, beat);
	return 0;
}

/* ---- key detection ----------------------------------------------- */

#define KEY_FFT_LOG	12
#define KEY_FFT		(1 << KEY_FFT_LOG)
#define KEY_HOP		(KEY_FFT / 2)
#define KEY_RATE	11025

struct cpx {
	float re, im;
};

static void fft(struct cpx *x, int n)
{
	int i, j, k, m;

	for (i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			struct cpx t = x[i];

			x[i] = x[j];
			x[j] = t;
		}
	}
	for (m = 2; m <= n; m <<= 1) {
		float ang = -2.0f * (float)M_PI / m;
		struct cpx wm = { cosf(ang), sinf(ang) };

		for (k = 0; k < n; k += m) {
			struct cpx w = { 1.0f, 0.0f };

			for (j = 0; j < m / 2; j++) {
				struct cpx *a = &x[k + j], *b = &x[k + j + m / 2];
				struct cpx t = { w.re * b->re - w.im * b->im,
						 w.re * b->im + w.im * b->re };
				struct cpx nw = { w.re * wm.re - w.im * wm.im,
						  w.re * wm.im + w.im * wm.re };

				b->re = a->re - t.re;
				b->im = a->im - t.im;
				a->re += t.re;
				a->im += t.im;
				w = nw;
			}
		}
	}
}

/* Krumhansl-Kessler key profiles. */
static const float major_profile[12] = {
	6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
	2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f,
};
static const float minor_profile[12] = {
	6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
	2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f,
};

static float correlate(const float *chroma, const float *profile, int shift)
{
	float mc = 0.0f, mp = 0.0f, num = 0.0f, da = 0.0f, db = 0.0f;
	int i;

	for (i = 0; i < 12; i++) {
		mc += chroma[i];
		mp += profile[i];
	}
	mc /= 12.0f;
	mp /= 12.0f;
	for (i = 0; i < 12; i++) {
		float a = chroma[(i + shift) % 12] - mc;
		float b = profile[i] - mp;

		num += a * b;
		da += a * a;
		db += b * b;
	}
	return da > 0.0f && db > 0.0f ? num / sqrtf(da * db) : 0.0f;
}

int analyze_key(const struct track *t)
{
	size_t frames = track_frames(t), n, i, pos = 0;
	unsigned int step = t->rate / KEY_RATE;
	double chroma[12] = { 0 };
	float chromaf[12];
	struct cpx *buf;
	float *win;
	int k, best = -1;
	float best_c = -1.0f;

	if (step < 1)
		step = 1;
	n = frames / step;
	if (n < 4 * KEY_FFT)
		return -1;
	buf = malloc(KEY_FFT * sizeof(*buf));
	win = malloc(KEY_FFT * sizeof(*win));
	if (!buf || !win) {
		free(buf);
		free(win);
		return -1;
	}
	for (i = 0; i < KEY_FFT; i++)
		win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / KEY_FFT);

	/* Analyse up to four minutes from the middle of the track. */
	if (n > (size_t)240 * KEY_RATE) {
		pos = (n - (size_t)240 * KEY_RATE) / 2;
		n = pos + (size_t)240 * KEY_RATE;
	}
	for (; pos + KEY_FFT <= n; pos += KEY_HOP) {
		float fs = (float)t->rate / step;

		for (i = 0; i < KEY_FFT; i++) {
			const int16_t *f = track_frame(t, (pos + i) * step);

			buf[i].re = (f[0] + f[1]) * (0.5f / 32768.0f) * win[i];
			buf[i].im = 0.0f;
		}
		fft(buf, KEY_FFT);
		for (i = 2; i < KEY_FFT / 2; i++) {
			float hz = i * fs / KEY_FFT;
			float mag, midi;
			int pc;

			if (hz < 55.0f || hz > 2000.0f)
				continue;
			mag = buf[i].re * buf[i].re + buf[i].im * buf[i].im;
			midi = 69.0f + 12.0f * log2f(hz / 440.0f);
			pc = ((int)lrintf(midi) % 12 + 12) % 12;
			chroma[pc] += log1pf(mag * 1e3f);
		}
	}
	free(buf);
	free(win);

	for (k = 0; k < 12; k++)
		chromaf[k] = (float)chroma[k];
	for (k = 0; k < 12; k++) {
		float cm = correlate(chromaf, major_profile, k);
		float cn = correlate(chromaf, minor_profile, k);

		if (cm > best_c) {
			best_c = cm;
			best = k;
		}
		if (cn > best_c) {
			best_c = cn;
			best = 12 + k;
		}
	}
	return best_c > 0.3f ? best : -1;
}

const char *key_name(int key)
{
	static const char *const names[24] = {
		"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#",
		"B", "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m",
		"Am", "A#m", "Bm",
	};

	return key >= 0 && key < 24 ? names[key] : "";
}

const char *key_camelot(int key)
{
	static const char *const major[12] = {
		"8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B",
		"6B", "1B",
	};
	static const char *const minor[12] = {
		"5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A",
		"3A", "10A",
	};

	if (key < 0 || key >= 24)
		return "";
	return key < 12 ? major[key] : minor[key - 12];
}

int key_distance(int key, int target)
{
	int d;

	if (key < 0 || target < 0 || (key < 12) != (target < 12))
		return 0;
	d = (target % 12) - (key % 12);
	if (d > 6)
		d -= 12;
	if (d < -6)
		d += 12;
	return d;
}

/* ---- loudness ---------------------------------------------------- */

float analyze_gain(const struct track *t)
{
	size_t frames = track_frames(t), i;
	double sum = 0.0;
	float rms_db;

	if (frames == 0)
		return 0.0f;
	for (i = 0; i < frames; i += 4) {
		const int16_t *f = track_frame(t, i);
		double m = (f[0] + f[1]) * (0.5 / 32768.0);

		sum += m * m;
	}
	rms_db = (float)(10.0 * log10(sum / (double)(frames / 4 + 1) + 1e-12));
	return CLAMP(-18.0f - rms_db, -12.0f, 12.0f);
}

/* ---- energy profile ---------------------------------------------- */

#define QUIET_RATIO	0.45
#define MAX_QUIET_SECS	60.0

static int cmp_double_asc(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* Mean peak level per second from the waveform bins. */
static double *seconds_profile(const struct track *t, size_t *n,
			       double *threshold)
{
	size_t bins = track_bins(t), per_sec = t->rate / WAVE_BIN_FRAMES;
	size_t secs, i, j;
	double *e, *sorted;

	if (!per_sec || bins < per_sec * 4)
		return NULL;
	secs = bins / per_sec;
	e = malloc(secs * sizeof(*e));
	sorted = malloc(secs * sizeof(*sorted));
	if (!e || !sorted) {
		free(e);
		free(sorted);
		return NULL;
	}
	for (i = 0; i < secs; i++) {
		double acc = 0.0;

		for (j = 0; j < per_sec; j++)
			acc += track_bin(t, i * per_sec + j)->peak;
		e[i] = acc / per_sec;
	}
	memcpy(sorted, e, secs * sizeof(*e));
	qsort(sorted, secs, sizeof(*sorted), cmp_double_asc);
	*threshold = sorted[secs / 2] * QUIET_RATIO;
	free(sorted);
	*n = secs;
	return e;
}

/* Mean peak level (0..1) of the frames @from..@to, 0 without bins. */
double analyze_level(const struct track *t, size_t from, size_t to)
{
	size_t bins = track_bins(t), i, a = from / WAVE_BIN_FRAMES;
	size_t b = to / WAVE_BIN_FRAMES;
	double acc = 0.0;

	if (b > bins)
		b = bins;
	if (a >= b)
		return 0.0;
	for (i = a; i < b; i++)
		acc += track_bin(t, i)->peak;
	return acc / (double)(b - a) / 255.0;
}

#define SILENCE_PEAK	2	/* of 255: about -42 dBFS */
#define MAX_SILENCE_SECS 300.0

/* Seconds of silence (not just quiet) at the end of a decoded track. */
double analyze_silence_tail(const struct track *t)
{
	size_t bins = track_bins(t), i;
	double per_bin = (double)WAVE_BIN_FRAMES / t->rate, secs = 0.0;

	if (!track_done(t))
		return 0.0;
	for (i = bins; i > 0 && secs < MAX_SILENCE_SECS; i--) {
		if (track_bin(t, i - 1)->peak > SILENCE_PEAK)
			break;
		secs += per_bin;
	}
	return secs;
}

/* Seconds of silence from frame @from on. */
double analyze_silence_head(const struct track *t, size_t from)
{
	size_t bins = track_bins(t), i;
	double per_bin = (double)WAVE_BIN_FRAMES / t->rate, secs = 0.0;

	for (i = from / WAVE_BIN_FRAMES; i < bins && secs < MAX_SILENCE_SECS;
	     i++) {
		if (track_bin(t, i)->peak > SILENCE_PEAK)
			break;
		secs += per_bin;
	}
	return secs;
}

double analyze_quiet_tail(const struct track *t)
{
	size_t n, i;
	double thr, *e;
	double quiet = 0.0;

	if (!track_done(t))
		return 0.0;
	e = seconds_profile(t, &n, &thr);
	if (!e)
		return 0.0;
	for (i = n; i > 0 && e[i - 1] < thr && quiet < MAX_QUIET_SECS; i--)
		quiet += 1.0;
	free(e);
	return quiet;
}

double analyze_quiet_head(const struct track *t, size_t from)
{
	size_t n, i, start;
	double thr, *e;
	double quiet = 0.0;

	e = seconds_profile(t, &n, &thr);
	if (!e)
		return 0.0;
	start = from / t->rate;
	for (i = start; i < n && e[i] < thr && quiet < MAX_QUIET_SECS; i++)
		quiet += 1.0;
	free(e);
	return quiet;
}
