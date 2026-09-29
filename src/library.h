/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * library.h - scanning music folders into PdMediaItems
 */
#ifndef PD_LIBRARY_H
#define PD_LIBRARY_H

#include <stdbool.h>

#include <gio/gio.h>

#include "media.h"

bool library_is_audio(const char *path);

/*
 * Scan @folders recursively on a worker thread.  Tags are read with
 * FFmpeg and cached by path and modification time.  @done receives a
 * GPtrArray of PdMediaItem via library_scan_finish().
 */
void library_scan_async(char **folders, GCancellable *cancel,
			GAsyncReadyCallback done, gpointer data);
GPtrArray *library_scan_finish(GAsyncResult *res, GError **err);

/* Synchronous single file, used for drag and drop. */
PdMediaItem *library_probe_file(const char *path);

#endif /* PD_LIBRARY_H */
