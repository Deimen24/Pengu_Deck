// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dsp.c - biquad design after the RBJ audio EQ cookbook
 */
#include <math.h>

#include "dsp.h"

void biquad_design(struct biquad *bq, enum biquad_kind kind, double rate,
		   double freq, double q, double gain_db)
{
	double a = pow(10.0, gain_db / 40.0);
	double w0 = 2.0 * M_PI * freq / rate;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * q);
	double sa = 2.0 * sqrt(a) * alpha;
	double b0, b1, b2, a0, a1, a2;

	switch (kind) {
	case BQ_LOWSHELF:
		b0 = a * ((a + 1) - (a - 1) * cw + sa);
		b1 = 2 * a * ((a - 1) - (a + 1) * cw);
		b2 = a * ((a + 1) - (a - 1) * cw - sa);
		a0 = (a + 1) + (a - 1) * cw + sa;
		a1 = -2 * ((a - 1) + (a + 1) * cw);
		a2 = (a + 1) + (a - 1) * cw - sa;
		break;
	case BQ_HIGHSHELF:
		b0 = a * ((a + 1) + (a - 1) * cw + sa);
		b1 = -2 * a * ((a - 1) + (a + 1) * cw);
		b2 = a * ((a + 1) + (a - 1) * cw - sa);
		a0 = (a + 1) - (a - 1) * cw + sa;
		a1 = 2 * ((a - 1) - (a + 1) * cw);
		a2 = (a + 1) - (a - 1) * cw - sa;
		break;
	case BQ_PEAK:
		b0 = 1 + alpha * a;
		b1 = -2 * cw;
		b2 = 1 - alpha * a;
		a0 = 1 + alpha / a;
		a1 = -2 * cw;
		a2 = 1 - alpha / a;
		break;
	case BQ_LOWPASS:
		b0 = (1 - cw) / 2;
		b1 = 1 - cw;
		b2 = (1 - cw) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cw;
		a2 = 1 - alpha;
		break;
	case BQ_HIGHPASS:
	default:
		b0 = (1 + cw) / 2;
		b1 = -(1 + cw);
		b2 = (1 + cw) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cw;
		a2 = 1 - alpha;
		break;
	}

	bq->b0 = (float)(b0 / a0);
	bq->b1 = (float)(b1 / a0);
	bq->b2 = (float)(b2 / a0);
	bq->a1 = (float)(a1 / a0);
	bq->a2 = (float)(a2 / a0);
}

void biquad_bypass(struct biquad *bq)
{
	bq->b0 = 1.0f;
	bq->b1 = bq->b2 = bq->a1 = bq->a2 = 0.0f;
}

void biquad_reset(struct biquad *bq)
{
	bq->z1 = bq->z2 = 0.0f;
}
