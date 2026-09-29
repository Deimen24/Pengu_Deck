/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cuestore.h - per track cue points and analysis results
 *
 * Positions are stored in seconds so they survive sample rate changes.
 */
#ifndef PD_CUESTORE_H
#define PD_CUESTORE_H

#include <stdbool.h>

#include "deck.h"

struct track_info {
	double bpm;		/* 0 when unknown */
	double beat_offset;	/* seconds */
	double cue;		/* seconds */
	double hotcue[DECK_HOTCUES];	/* seconds, < 0 when unset */
};

void cuestore_open(void);
void cuestore_close(void);

bool cuestore_get(const char *key, struct track_info *info);
void cuestore_put(const char *key, const struct track_info *info);

/* Stored bpm only, 0 if unknown. */
double cuestore_bpm(const char *key);

#endif /* PD_CUESTORE_H */
