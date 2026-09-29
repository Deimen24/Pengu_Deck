/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fx.h - per channel effect unit
 *
 * One effect at a time, beat synced where that makes sense, with a
 * dry/wet mix.  Parameters are atomics written by the GUI; everything
 * else belongs to the audio thread.
 */
#ifndef PD_FX_H
#define PD_FX_H

#include <stdatomic.h>
#include <stdbool.h>

enum fx_type {
	FX_NONE,
	FX_ECHO,	/* beat synced delay with feedback */
	FX_REVERB,
	FX_FLANGER,	/* LFO synced to beats */
	FX_PHASER,
	FX_CRUSH,	/* bit and sample rate reduction */
	FX_GATE,	/* trance gate synced to beats */
	FX_COUNT,
};

const char *fx_name(enum fx_type t);

/* Beat lengths the BEATS button cycles through. */
#define FX_BEAT_STEPS	6
extern const float fx_beat_steps[FX_BEAT_STEPS];

struct fx_state;

struct fx {
	/* shared */
	atomic_int type;
	_Atomic float wet;		/* 0..1 */
	_Atomic float beats;		/* 0.25..4 */
	_Atomic float param;		/* 0..1, effect specific */
	atomic_bool on;

	/* audio */
	struct fx_state *st;
	unsigned int rate;
};

void fx_init(struct fx *f);
void fx_fini(struct fx *f);
/* Called with the audio stopped. */
void fx_set_rate(struct fx *f, unsigned int rate);
/* Process @n interleaved stereo frames in place; @bpm may be 0. */
void fx_process(struct fx *f, float *buf, unsigned int n, double bpm);

#endif /* PD_FX_H */
