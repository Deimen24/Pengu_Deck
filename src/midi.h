/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * midi.h - MIDI controllers through the ALSA sequencer
 *
 * Any class compliant controller (Pioneer DDJ, CDJs in MIDI mode, ...)
 * shows up as a sequencer client; all of them are connected to our input
 * port, including ones plugged in later.  Messages are matched against a
 * mapping file (~/.config/pengu-deck/midi.ini) that "learn" mode fills.
 */
#ifndef PD_MIDI_H
#define PD_MIDI_H

#include <stdbool.h>

#include "app.h"

enum midi_kind {
	MIDI_BUTTON,		/* note on, or cc > 63 */
	MIDI_ABSOLUTE,		/* 0..127 mapped onto a range */
	MIDI_RELATIVE,		/* jog wheels and endless encoders */
};

struct midi_control {
	const char *name;	/* "deck0.play", "xfader" */
	const char *label;	/* "Deck A: Play" */
	enum midi_kind kind;
	int deck;		/* -1 for global controls */
};

void midi_init(struct app *a);
void midi_shutdown(void);

bool midi_available(void);
/* Names of the connected input clients, free with g_strfreev(). */
char **midi_devices(void);

const struct midi_control *midi_controls(unsigned int *n);

/* Human readable binding of a control ("CC 12 ch 1"), NULL if unbound. */
char *midi_binding(const char *control);
void midi_unbind(const char *control);

/* The next message will be bound to @control (NULL cancels). */
void midi_learn(const char *control);
bool midi_learning(void);

/* Last received message as text, for the preferences page. */
const char *midi_last_message(void);

#endif /* PD_MIDI_H */
