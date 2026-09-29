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

#endif /* PD_ANALYZE_H */
