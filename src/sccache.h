/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sccache.h - on disk cache of SoundCloud streams
 *
 * Streams are remuxed (not re-encoded) into Matroska files under
 * $XDG_CACHE_HOME/pengu-deck/soundcloud/<track id>.mka by a small worker
 * pool.  The decoder opens the file like any local track once complete.
 */
#ifndef PD_SCCACHE_H
#define PD_SCCACHE_H

#include <stdbool.h>

#include "app.h"

void sccache_init(struct app *a);
void sccache_shutdown(void);

/* Path of the complete cached file, or NULL.  Free with g_free(). */
char *sccache_lookup(PdMediaItem *m);

/* Start downloading @m unless cached or already in flight. */
void sccache_fetch(PdMediaItem *m);

/* -1: not cached, 0..99: downloading, 100: complete, -2: failed. */
int sccache_progress(PdMediaItem *m);

guint64 sccache_size(void);
void sccache_clear(void);

#endif /* PD_SCCACHE_H */
