// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sampler.c - eight sample pads
 */
#include <string.h>

#include <glib.h>

#include "sampler.h"

void sampler_init(struct sampler *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	atomic_init(&s->volume, 0.8f);
	for (i = 0; i < SAMPLER_PADS; i++)
		atomic_init(&s->pad[i].volume, 1.0f);
}

void sampler_fini(struct sampler *s)
{
	int i;

	for (i = 0; i < SAMPLER_PADS; i++)
		sampler_clear(s, i);
}

void sampler_set_rate(struct sampler *s, unsigned int rate)
{
	s->rate = rate;
}

static void render_pad(struct sampler *s, struct sample_pad *p,
		       struct track *t, float *out, unsigned int n)
{
	size_t frames = track_frames(t);
	double step = (double)t->rate / s->rate;
	float vol = atomic_load(&p->volume) * atomic_load(&s->volume);
	bool loop = atomic_load(&p->mode) == SAMPLE_LOOP;
	unsigned int i;

	if (atomic_exchange(&p->retrigger, false))
		p->apos = 0.0;
	for (i = 0; i < n; i++) {
		size_t idx = (size_t)p->apos;
		const int16_t *f;

		if (idx >= frames) {
			if (loop && track_done(t) && frames > 0) {
				p->apos = 0.0;
				idx = 0;
			} else {
				if (track_done(t))
					atomic_store(&p->playing, false);
				break;
			}
		}
		f = track_frame(t, idx);
		out[2 * i] += f[0] * (vol / 32768.0f);
		out[2 * i + 1] += f[1] * (vol / 32768.0f);
		p->apos += step;
	}
	atomic_store(&p->pos, p->apos);
}

void sampler_render(struct sampler *s, float *out, unsigned int n)
{
	int i;

	if (!s->rate)
		return;
	atomic_store(&s->in_use, 1);
	for (i = 0; i < SAMPLER_PADS; i++) {
		struct sample_pad *p = &s->pad[i];
		struct track *t = atomic_load(&p->track);

		if (!t || !atomic_load(&p->playing))
			continue;
		render_pad(s, p, t, out, n);
	}
	atomic_store(&s->in_use, 0);
}

void sampler_load(struct sampler *s, int pad, struct track *t)
{
	struct sample_pad *p;
	struct track *old;

	if (pad < 0 || pad >= SAMPLER_PADS)
		return;
	p = &s->pad[pad];
	atomic_store(&p->playing, false);
	old = atomic_exchange(&p->track, track_ref(t));
	while (atomic_load(&s->in_use))
		g_thread_yield();
	p->apos = 0.0;
	atomic_store(&p->pos, 0.0);
	if (old) {
		atomic_store(&old->cancel, true);
		track_unref(old);
	}
}

void sampler_clear(struct sampler *s, int pad)
{
	struct sample_pad *p;
	struct track *old;

	if (pad < 0 || pad >= SAMPLER_PADS)
		return;
	p = &s->pad[pad];
	atomic_store(&p->playing, false);
	old = atomic_exchange(&p->track, NULL);
	while (atomic_load(&s->in_use))
		g_thread_yield();
	if (old) {
		atomic_store(&old->cancel, true);
		track_unref(old);
	}
}

void sampler_trigger(struct sampler *s, int pad, bool press)
{
	struct sample_pad *p;

	if (pad < 0 || pad >= SAMPLER_PADS)
		return;
	p = &s->pad[pad];
	if (!atomic_load(&p->track))
		return;
	switch (atomic_load(&p->mode)) {
	case SAMPLE_HOLD:
		if (press)
			atomic_store(&p->retrigger, true);
		atomic_store(&p->playing, press);
		break;
	case SAMPLE_LOOP:
		if (!press)
			break;
		if (atomic_load(&p->playing)) {
			atomic_store(&p->playing, false);
		} else {
			atomic_store(&p->retrigger, true);
			atomic_store(&p->playing, true);
		}
		break;
	case SAMPLE_ONESHOT:
	default:
		if (press) {
			atomic_store(&p->retrigger, true);
			atomic_store(&p->playing, true);
		}
		break;
	}
}

void sampler_stop_all(struct sampler *s)
{
	int i;

	for (i = 0; i < SAMPLER_PADS; i++)
		atomic_store(&s->pad[i].playing, false);
}
