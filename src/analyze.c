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
