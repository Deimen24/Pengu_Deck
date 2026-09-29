/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PD_DECODER_H
#define PD_DECODER_H

#include "track.h"

/*
 * Start decoding @t->uri on a background thread.  The thread holds its
 * own reference to the track and stops early when t->cancel is set.  Once
 * all audio is decoded the tempo is analysed unless t->analysed is
 * already true.
 */
void decoder_start(struct track *t);

/* Synchronous variant, used by tests and the library scanner. */
int decoder_run(struct track *t);

struct media_tags {
	char *title;
	char *artist;
	char *album;
	char *genre;
	double duration;	/* seconds, 0 if unknown */
	double bpm;		/* from tags, 0 if absent */
};

/* Read metadata without decoding audio.  Returns 0 on success. */
int decoder_probe(const char *path, struct media_tags *tags);
void media_tags_clear(struct media_tags *tags);

#endif /* PD_DECODER_H */
