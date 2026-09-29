/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * track.h - progressively decoded PCM storage for one loaded track
 *
 * The decoder thread appends interleaved stereo s16 frames into fixed
 * size chunks and publishes the new frame count with release semantics.
 * The audio thread and the GUI only ever read frames below the published
 * count, so no locking is needed on the sample data itself.
 */
#ifndef PD_TRACK_H
#define PD_TRACK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <glib.h>

#define TRACK_CHUNK_SHIFT	16
#define TRACK_CHUNK_FRAMES	(1u << TRACK_CHUNK_SHIFT)
#define TRACK_CHUNK_MASK	(TRACK_CHUNK_FRAMES - 1)
#define TRACK_MAX_CHUNKS	8192	/* ~3 hours at 48 kHz */

#define WAVE_BIN_SHIFT		8	/* 256 frames per waveform bin */
#define WAVE_BIN_FRAMES		(1u << WAVE_BIN_SHIFT)
#define WAVE_BINS_PER_CHUNK	(TRACK_CHUNK_FRAMES >> WAVE_BIN_SHIFT)

enum track_state {
	TRACK_LOADING,
	TRACK_DECODED,		/* all audio present, analysis may run */
	TRACK_READY,		/* decoded and analysed */
	TRACK_FAILED,
};

/* Per-bin peak levels of three frequency bands, 0..255. */
struct wave_bin {
	uint8_t low;
	uint8_t mid;
	uint8_t high;
	uint8_t peak;
};

struct wave_state;

struct track {
	atomic_int refcount;

	char *uri;		/* what the decoder opens */
	char *key;		/* stable identity for the cue store */
	unsigned int rate;	/* sample rate of the stored PCM */

	GMutex lock;		/* protects the strings below */
	char *title;
	char *artist;
	char *error;

	int16_t *pcm[TRACK_MAX_CHUNKS];
	struct wave_bin *wave[TRACK_MAX_CHUNKS];

	atomic_size_t frames;		/* published decoded frames */
	atomic_size_t length_hint;	/* expected total frames, or 0 */
	atomic_int state;
	atomic_bool cancel;

	_Atomic double bpm;		/* 0 when unknown */
	_Atomic double beat_offset;	/* frame of the first beat */
	atomic_bool analysed;		/* bpm came from cache or analysis */
	atomic_int mkey;		/* musical key, see analyze.h, -1 unknown */
	_Atomic float gain_db;		/* replay gain to reach the target */
	atomic_bool gain_known;

	struct wave_state *ws;		/* decoder private */
};

struct track *track_new(const char *uri, const char *key, unsigned int rate);
struct track *track_ref(struct track *t);
void track_unref(struct track *t);

/* Decoder side */
int track_append(struct track *t, const int16_t *frames, size_t n);
void track_set_meta(struct track *t, const char *title, const char *artist);
/* True once a title is known, from the caller or the file's tags. */
bool track_has_title(struct track *t);
void track_fail(struct track *t, const char *msg);

/* Reader side; returned strings must be freed with g_free(). */
char *track_title(struct track *t);
char *track_artist(struct track *t);
char *track_error(struct track *t);

static inline size_t track_frames(const struct track *t)
{
	return atomic_load_explicit(&t->frames, memory_order_acquire);
}

static inline size_t track_length(const struct track *t)
{
	size_t n = track_frames(t);
	size_t hint = atomic_load_explicit(&t->length_hint,
					   memory_order_relaxed);

	if (atomic_load(&t->state) != TRACK_LOADING)
		return n;
	return hint > n ? hint : n;
}

static inline bool track_done(const struct track *t)
{
	int s = atomic_load(&t->state);

	return s == TRACK_DECODED || s == TRACK_READY;
}

/* Caller guarantees idx < track_frames(t). */
static inline const int16_t *track_frame(const struct track *t, size_t idx)
{
	return t->pcm[idx >> TRACK_CHUNK_SHIFT] + 2 * (idx & TRACK_CHUNK_MASK);
}

static inline const struct wave_bin *track_bin(const struct track *t,
					       size_t bin)
{
	size_t f = bin << WAVE_BIN_SHIFT;

	return t->wave[f >> TRACK_CHUNK_SHIFT] +
	       ((f & TRACK_CHUNK_MASK) >> WAVE_BIN_SHIFT);
}

/* Number of waveform bins whose data is complete. */
static inline size_t track_bins(const struct track *t)
{
	return track_frames(t) >> WAVE_BIN_SHIFT;
}

static inline double track_beat_len(const struct track *t)
{
	double bpm = atomic_load(&t->bpm);

	return bpm > 0.0 ? 60.0 * t->rate / bpm : 0.0;
}

#endif /* PD_TRACK_H */
