/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * playlists.h - crates, play history and Rekordbox import
 *
 * Playlists are GListStores of PdMediaItem kept in
 * $XDG_DATA_HOME/pengu-deck/playlists.tsv; every entry stores enough
 * metadata to show the row without the library.
 */
#ifndef PD_PLAYLISTS_H
#define PD_PLAYLISTS_H

#include <gio/gio.h>

#include "media.h"

struct playlist {
	char *name;
	GListStore *items;	/* PdMediaItem */
};

void playlists_open(void);
void playlists_close(void);

/* Array of struct playlist, owned by the module. */
GPtrArray *playlists_all(void);
struct playlist *playlists_find(const char *name);
struct playlist *playlists_create(const char *name);
void playlists_delete(const char *name);
void playlists_rename(struct playlist *p, const char *name);
void playlists_add(struct playlist *p, PdMediaItem *m);
void playlists_remove(struct playlist *p, guint pos);
/* Emitted through this callback whenever the set of playlists changes. */
void playlists_set_changed_cb(void (*cb)(gpointer data), gpointer data);
/* Write a playlist as M3U8; returns FALSE with @err on failure. */
gboolean playlists_export_m3u(struct playlist *p, const char *path,
			      GError **err);

/* History: every load is appended, in memory and to history.tsv. */
GListStore *history_items(void);
void history_add(PdMediaItem *m);
gboolean history_export(const char *path, GError **err);

/*
 * Rekordbox XML import: cue points, tempo and grid go to the cue store,
 * playlists become playlists, and tracks are returned so the library
 * can show them.  Runs synchronously, call it from a worker thread.
 */
struct rb_import {
	GPtrArray *tracks;	/* PdMediaItem */
	unsigned int cues;
	unsigned int playlists;
};

gboolean rekordbox_import(const char *xml_path, struct rb_import *out,
			  GError **err);
void rb_import_clear(struct rb_import *out);

#endif /* PD_PLAYLISTS_H */
