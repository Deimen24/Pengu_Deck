/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * automix.h - hands free playback of the queue on decks A and B
 *
 * While enabled the queue is played alternately on decks A and B: the
 * next track is loaded into the idle deck ahead of time, started at its
 * cue point when the playing track has "fade" seconds left (tempo synced
 * when possible) and the crossfader glides over.  Everything runs on the
 * main loop and only pokes the same atomics the buttons and faders use,
 * so the audio thread is never disturbed and the user can still touch
 * anything mid transition.
 */
#ifndef PD_AUTOMIX_H
#define PD_AUTOMIX_H

#include <stdbool.h>

#include "app.h"

void automix_init(struct app *a);
void automix_shutdown(void);

void automix_set_enabled(bool on);
bool automix_enabled(void);

/* The crossfader is being moved by automix right now. */
bool automix_fading(void);

/* Start the transition to the next queued track immediately. */
void automix_next(void);

/* Human readable state for the panel, static string. */
const char *automix_status(void);

#endif /* PD_AUTOMIX_H */
