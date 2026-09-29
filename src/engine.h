/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * engine.h - audio device, two channel mixer, headphone cue and recorder
 */
#ifndef PD_ENGINE_H
#define PD_ENGINE_H

#include <stdatomic.h>
#include <stdbool.h>

#include "deck.h"

enum hp_mode {
	HP_OFF,
	HP_SPLIT,	/* left = master (mono), right = cue (mono) */
	HP_CH34,	/* 4 channel interface: 1/2 master, 3/4 cue */
};

enum xf_curve {
	XF_DIPLESS,	/* both sides at full level in the centre */
	XF_POWER,	/* constant power */
	XF_CUT,		/* scratch curve, sharp cut in */
};

enum audio_backend {
	BACKEND_AUTO,
	BACKEND_PULSE,	/* also PipeWire via pipewire-pulse */
	BACKEND_ALSA,
	BACKEND_JACK,	/* also PipeWire via pipewire-jack */
	BACKEND_NULL,	/* no output, for testing */
};

struct engine_opts {
	enum audio_backend backend;
	const char *device;	/* NULL or "" for the default device */
	unsigned int rate;	/* 0 for the device default */
	unsigned int period;	/* frames, 0 for the backend default */
	enum hp_mode hp_mode;
};

struct engine_priv;

struct engine {
	struct deck deck[2];

	/* shared, written by the gui */
	_Atomic float xfader;		/* -1 = deck A .. 1 = deck B */
	_Atomic float master;		/* master gain */
	_Atomic float cue_mix;		/* 0 = cue only .. 1 = master only */
	_Atomic float cue_vol;
	atomic_int xf_curve;

	/* shared, written by the audio thread */
	_Atomic float peak_l;
	_Atomic float peak_r;
	atomic_uint xruns;

	/* set by engine_open() */
	unsigned int rate;
	unsigned int channels;
	enum hp_mode hp_mode;
	bool running;
	char *device_name;

	struct engine_priv *priv;
};

void engine_init(struct engine *e);
void engine_fini(struct engine *e);

/* Returns 0 on success, otherwise fills @err with a message. */
int engine_open(struct engine *e, const struct engine_opts *o, char **err);
void engine_close(struct engine *e);

/* NULL terminated list of playback device names, free with g_strfreev. */
char **engine_list_devices(enum audio_backend backend);

int engine_record_start(struct engine *e, const char *path, char **err);
void engine_record_stop(struct engine *e);
bool engine_recording(struct engine *e);
double engine_record_seconds(struct engine *e);

/* Mix @n frames into @out (engine->channels interleaved), for tests. */
void engine_process(struct engine *e, float *out, unsigned int n);

#endif /* PD_ENGINE_H */
