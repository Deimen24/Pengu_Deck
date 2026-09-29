// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * track.c - progressively decoded PCM storage
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "track.h"

/*
 * Waveform band splitting state.  Only the single decoder thread writing
 * the track touches it.
 */
struct wave_state {
	float lp_low;
	float lp_high;
	float max_low;
	float max_mid;
	float max_high;
	float max_peak;
	unsigned int count;
	float a_low;
	float a_high;
};

static struct wave_state *wave_state_get(struct track *t)
{
	struct wave_state *ws = t->ws;

	if (ws)
		return ws;
	ws = g_new0(struct wave_state, 1);
	/* one-pole low pass coefficients at 200 Hz and 2.5 kHz */
	ws->a_low = 1.0f - expf(-2.0f * (float)M_PI * 200.0f / t->rate);
	ws->a_high = 1.0f - expf(-2.0f * (float)M_PI * 2500.0f / t->rate);
	t->ws = ws;
	return ws;
}

struct track *track_new(const char *uri, const char *key, unsigned int rate)
{
	struct track *t = g_new0(struct track, 1);

	atomic_init(&t->refcount, 1);
	t->uri = g_strdup(uri);
	t->key = g_strdup(key ? key : uri);
	t->rate = rate;
	g_mutex_init(&t->lock);
	atomic_init(&t->state, TRACK_LOADING);
	return t;
}

struct track *track_ref(struct track *t)
{
	if (t)
		atomic_fetch_add(&t->refcount, 1);
	return t;
}

void track_unref(struct track *t)
{
	size_t i;

	if (!t || atomic_fetch_sub(&t->refcount, 1) != 1)
		return;

	g_free(t->ws);
	for (i = 0; i < TRACK_MAX_CHUNKS && t->pcm[i]; i++) {
		g_free(t->pcm[i]);
		g_free(t->wave[i]);
	}
	g_mutex_clear(&t->lock);
	g_free(t->uri);
	g_free(t->key);
	g_free(t->title);
	g_free(t->artist);
	g_free(t->error);
	g_free(t);
}

static inline float fmaxabs(float m, float v)
{
	v = fabsf(v);
	return v > m ? v : m;
}

static inline uint8_t to_u8(float v)
{
	int x = (int)(v * 255.0f + 0.5f);

	return x > 255 ? 255 : (uint8_t)x;
}

static void wave_feed(struct track *t, struct wave_state *ws, size_t idx,
		      const int16_t *f)
{
	float m = (f[0] + f[1]) * (0.5f / 32768.0f);
	struct wave_bin *b;

	ws->lp_low += ws->a_low * (m - ws->lp_low);
	ws->lp_high += ws->a_high * (m - ws->lp_high);

	ws->max_low = fmaxabs(ws->max_low, ws->lp_low);
	ws->max_mid = fmaxabs(ws->max_mid, ws->lp_high - ws->lp_low);
	ws->max_high = fmaxabs(ws->max_high, m - ws->lp_high);
	ws->max_peak = fmaxabs(ws->max_peak, m);

	if (++ws->count < WAVE_BIN_FRAMES)
		return;

	b = &t->wave[idx >> TRACK_CHUNK_SHIFT]
		    [(idx & TRACK_CHUNK_MASK) >> WAVE_BIN_SHIFT];
	b->low = to_u8(ws->max_low);
	b->mid = to_u8(ws->max_mid * 1.4f);
	b->high = to_u8(ws->max_high * 2.0f);
	b->peak = to_u8(ws->max_peak);
	ws->max_low = ws->max_mid = ws->max_high = ws->max_peak = 0.0f;
	ws->count = 0;
}

int track_append(struct track *t, const int16_t *frames, size_t n)
{
	struct wave_state *ws = wave_state_get(t);
	size_t pos = atomic_load_explicit(&t->frames, memory_order_relaxed);

	while (n) {
		size_t c = pos >> TRACK_CHUNK_SHIFT;
		size_t off = pos & TRACK_CHUNK_MASK;
		size_t take = TRACK_CHUNK_FRAMES - off;
		size_t i;

		if (c >= TRACK_MAX_CHUNKS)
			return -1;
		if (!t->pcm[c]) {
			t->pcm[c] = g_malloc(TRACK_CHUNK_FRAMES * 4);
			t->wave[c] = g_malloc0(WAVE_BINS_PER_CHUNK *
					       sizeof(struct wave_bin));
		}
		if (take > n)
			take = n;

		memcpy(t->pcm[c] + 2 * off, frames, take * 4);
		for (i = 0; i < take; i++)
			wave_feed(t, ws, pos + i, frames + 2 * i);

		frames += 2 * take;
		pos += take;
		n -= take;
		atomic_store_explicit(&t->frames, pos, memory_order_release);
	}
	return 0;
}

void track_set_meta(struct track *t, const char *title, const char *artist)
{
	g_mutex_lock(&t->lock);
	if (title && *title) {
		g_free(t->title);
		t->title = g_strdup(title);
	}
	if (artist && *artist) {
		g_free(t->artist);
		t->artist = g_strdup(artist);
	}
	g_mutex_unlock(&t->lock);
}

void track_fail(struct track *t, const char *msg)
{
	g_mutex_lock(&t->lock);
	g_free(t->error);
	t->error = g_strdup(msg);
	g_mutex_unlock(&t->lock);
	atomic_store(&t->state, TRACK_FAILED);
}

static char *dup_locked(struct track *t, char **field)
{
	char *s;

	g_mutex_lock(&t->lock);
	s = g_strdup(*field);
	g_mutex_unlock(&t->lock);
	return s;
}

char *track_title(struct track *t)
{
	return dup_locked(t, &t->title);
}

char *track_artist(struct track *t)
{
	return dup_locked(t, &t->artist);
}

char *track_error(struct track *t)
{
	return dup_locked(t, &t->error);
}
