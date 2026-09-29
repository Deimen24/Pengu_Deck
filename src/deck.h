/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * deck.h - one playback deck plus its mixer channel strip
 *
 * Fields marked "shared" are written by the GUI and read by the audio
 * thread (or the other way round) and are therefore atomic.  Fields marked
 * "audio" are private to the audio thread, fields marked "gui" are private
 * to the main thread.
 */
#ifndef PD_DECK_H
#define PD_DECK_H

#include <stdatomic.h>
#include <stdbool.h>

#include "dsp.h"
#include "track.h"

#define DECK_HOTCUES		4
#define DECK_MAX_BLOCK		2048
#define DECK_NO_SEEK		(-1e18)

enum eq_band {
	EQ_LOW,
	EQ_MID,
	EQ_HIGH,
	EQ_BANDS,
};

struct rb_state;

struct deck {
	int index;

	/* shared: transport */
	_Atomic(struct track *) track;
	atomic_int in_use;		/* audio thread is using track */
	_Atomic double pos;		/* frames in track rate */
	_Atomic double seek;		/* pending seek or DECK_NO_SEEK */
	atomic_bool playing;
	_Atomic float pitch;		/* tempo change, 0.08 = +8% */
	_Atomic float bend;		/* temporary nudge */
	atomic_bool keylock;
	atomic_bool scratch;
	_Atomic double scratch_target;

	/* shared: markers, written by the gui, read by the audio thread */
	_Atomic double loop_in;
	_Atomic double loop_out;
	atomic_bool loop_on;

	/* shared: channel strip */
	_Atomic float trim_db;
	_Atomic float eq_db[EQ_BANDS];
	atomic_bool eq_kill[EQ_BANDS];
	_Atomic float filter;		/* -1 low pass .. 0 off .. 1 high pass */
	_Atomic float volume;		/* channel fader 0..1 */
	atomic_bool pfl;
	_Atomic float peak_l;		/* max since last read by the gui */
	_Atomic float peak_r;

	/* gui */
	double cue;
	double hotcue[DECK_HOTCUES];	/* < 0 when unset */
	bool cue_preview;
	double loop_beats;

	/* audio */
	unsigned int out_rate;
	struct biquad eq[EQ_BANDS][2];
	struct biquad flt[2];
	float eq_cur[EQ_BANDS];
	float flt_cur;
	float fade;
	double scr_rate;
	struct rb_state *rb;
	float *tmp[2];
};

void deck_init(struct deck *d, int index);
void deck_fini(struct deck *d);

/* Called with the audio device stopped. */
void deck_set_rate(struct deck *d, unsigned int rate);

/* Audio thread: render @n stereo frames, post EQ, pre fader. */
void deck_render(struct deck *d, float *out, unsigned int n);

/* Main thread API */
void deck_load(struct deck *d, struct track *t);
struct track *deck_track(struct deck *d);
double deck_position(struct deck *d);
void deck_seek(struct deck *d, double frame);
void deck_play(struct deck *d, bool play);
void deck_cue_press(struct deck *d);
void deck_cue_release(struct deck *d);
void deck_hotcue(struct deck *d, int i);
void deck_hotcue_clear(struct deck *d, int i);
void deck_loop_beats(struct deck *d, double beats);
void deck_loop_set_in(struct deck *d);
void deck_loop_set_out(struct deck *d);
void deck_loop_toggle(struct deck *d);
void deck_loop_scale(struct deck *d, double factor);
double deck_rate(struct deck *d);
double deck_bpm(struct deck *d);
double deck_sync_pitch(struct deck *d, struct deck *master);
void deck_sync_phase(struct deck *d, struct deck *master);
bool deck_has_keylock(void);

#endif /* PD_DECK_H */
