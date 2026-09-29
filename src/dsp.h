/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dsp.h - small audio building blocks (RBJ biquads)
 */
#ifndef PD_DSP_H
#define PD_DSP_H

#include <math.h>

enum biquad_kind {
	BQ_LOWSHELF,
	BQ_PEAK,
	BQ_HIGHSHELF,
	BQ_LOWPASS,
	BQ_HIGHPASS,
};

struct biquad {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

void biquad_design(struct biquad *bq, enum biquad_kind kind, double rate,
		   double freq, double q, double gain_db);
void biquad_bypass(struct biquad *bq);
void biquad_reset(struct biquad *bq);

/* Transposed direct form II */
static inline float biquad_run(struct biquad *bq, float x)
{
	float y = bq->b0 * x + bq->z1;

	bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
	bq->z2 = bq->b2 * x - bq->a2 * y;
	return y;
}

static inline float db_to_gain(float db)
{
	return powf(10.0f, db / 20.0f);
}

#endif /* PD_DSP_H */
