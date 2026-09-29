// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * media.c - library / search result entries
 */
#include "media.h"

G_DEFINE_FINAL_TYPE(PdMediaItem, pd_media_item, G_TYPE_OBJECT)

static GQuark haystack_quark;

static void pd_media_item_finalize(GObject *obj)
{
	PdMediaItem *m = PD_MEDIA_ITEM(obj);

	g_free(m->key);
	g_free(m->location);
	g_free(m->title);
	g_free(m->artist);
	g_free(m->album);
	g_free(m->genre);
	g_free(m->permalink);
	g_free(m->track_auth);
	G_OBJECT_CLASS(pd_media_item_parent_class)->finalize(obj);
}

static void pd_media_item_class_init(PdMediaItemClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pd_media_item_finalize;
	haystack_quark = g_quark_from_static_string("pd-haystack");
}

static void pd_media_item_init(PdMediaItem *m)
{
}

PdMediaItem *pd_media_item_new(enum media_source source, const char *key)
{
	PdMediaItem *m = g_object_new(PD_TYPE_MEDIA_ITEM, NULL);

	m->source = source;
	m->key = g_strdup(key);
	return m;
}

const char *pd_media_item_haystack(PdMediaItem *m)
{
	char *s = g_object_get_qdata(G_OBJECT(m), haystack_quark);
	char *raw;

	if (s)
		return s;
	raw = g_strjoin(" ", m->title ? m->title : "",
			m->artist ? m->artist : "",
			m->album ? m->album : "",
			m->genre ? m->genre : "", NULL);
	s = g_utf8_casefold(raw, -1);
	g_free(raw);
	g_object_set_qdata_full(G_OBJECT(m), haystack_quark, s, g_free);
	return s;
}

char *format_duration(double seconds)
{
	int s = (int)(seconds + 0.5);

	if (seconds <= 0.0)
		return g_strdup("");
	if (s >= 3600)
		return g_strdup_printf("%d:%02d:%02d", s / 3600,
				       s / 60 % 60, s % 60);
	return g_strdup_printf("%d:%02d", s / 60, s % 60);
}
