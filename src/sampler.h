/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sampler.h - eight sample pads mixed into the master
 */
#ifndef PD_SAMPLER_H
#define PD_SAMPLER_H

#include <stdatomic.h>
#include <stdbool.h>

#include "track.h"

#define SAMPLER_PADS	8

enum sample_mode {
	SAMPLE_ONESHOT,		/* play to the end, retrigger restarts */
	SAMPLE_LOOP,		/* loop until stopped */
	SAMPLE_HOLD,		/* play while held */
};

struct sample_pad {
	/* shared */
	_Atomic(struct track *) track;
	atomic_bool playing;
	atomic_int mode;
	_Atomic float volume;		/* 0..1 */
	_Atomic double pos;		/* frames, for the progress bar */
	atomic_bool retrigger;		/* GUI sets, audio clears */

	/* audio */
	double apos;
};

struct sampler {
	struct sample_pad pad[SAMPLER_PADS];
	_Atomic float volume;		/* sampler master */
	atomic_int in_use;
	unsigned int rate;
};

void sampler_init(struct sampler *s);
void sampler_fini(struct sampler *s);
void sampler_set_rate(struct sampler *s, unsigned int rate);

/* Audio thread: add @n stereo frames to @out. */
void sampler_render(struct sampler *s, float *out, unsigned int n);

/* Main thread */
void sampler_load(struct sampler *s, int pad, struct track *t);
void sampler_clear(struct sampler *s, int pad);
void sampler_trigger(struct sampler *s, int pad, bool press);
void sampler_stop_all(struct sampler *s);

#endif /* PD_SAMPLER_H */
