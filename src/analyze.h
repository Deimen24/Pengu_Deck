/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PD_ANALYZE_H
#define PD_ANALYZE_H

#include <stddef.h>
#include <stdint.h>

#include "track.h"

#define BPM_MIN		70.0
#define BPM_MAX		180.0

/*
 * Estimate the tempo and the first beat position (in frames) of a fully
 * decoded track.  Returns 0 on success, -1 if no tempo was found.
 */
int analyze_tempo(const struct track *t, double *bpm, double *offset);

/* Fold a tempo into [BPM_MIN, BPM_MAX) by doubling or halving. */
double bpm_fold(double bpm);

/*
 * Musical key: 0..11 major C..B, 12..23 minor C..B (natural minor), or
 * -1 when undetermined.
 */
int analyze_key(const struct track *t);
const char *key_name(int key);		/* "Am", "F#" */
const char *key_camelot(int key);	/* "8A" */
/* Semitones to shift @key so it matches @target (0 if unrelated). */
int key_distance(int key, int target);

/* Replay gain in dB that brings the track to -18 dBFS RMS. */
float analyze_gain(const struct track *t);

/*
 * Quiet outro / intro in seconds: how long the level stays well below
 * the track's typical level at the end (needs a fully decoded track) or
 * from frame @from on.  Used by automix to size transitions.
 */
double analyze_quiet_tail(const struct track *t);
double analyze_quiet_head(const struct track *t, size_t from);

#endif /* PD_ANALYZE_H */
