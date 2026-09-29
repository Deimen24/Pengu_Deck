/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * media.h - one entry of the local library or of SoundCloud results
 *
 * A GObject so it can live in GListModels; the fields are plain public
 * members that are filled in before the item is shown and then only
 * changed from the main thread.
 */
#ifndef PD_MEDIA_H
#define PD_MEDIA_H

#include <glib-object.h>

G_BEGIN_DECLS

enum media_source {
	MEDIA_LOCAL,
	MEDIA_SOUNDCLOUD,
};

#define PD_TYPE_MEDIA_ITEM (pd_media_item_get_type())
G_DECLARE_FINAL_TYPE(PdMediaItem, pd_media_item, PD, MEDIA_ITEM, GObject)

struct _PdMediaItem {
	GObject parent;

	enum media_source source;
	char *key;		/* path, or "soundcloud:<id>" */
	char *location;		/* path, or SoundCloud transcoding api url */
	char *title;
	char *artist;
	char *album;
	char *genre;
	double duration;	/* seconds */
	double bpm;
	gint64 mtime;		/* local files: modification time */

	/* SoundCloud only */
	char *permalink;
	char *track_auth;
	gboolean preview;	/* only a 30 second snippet is available */
};

PdMediaItem *pd_media_item_new(enum media_source source, const char *key);

/* Lower case "title artist album genre" for searching. */
const char *pd_media_item_haystack(PdMediaItem *item);

char *format_duration(double seconds);

G_END_DECLS

#endif /* PD_MEDIA_H */
