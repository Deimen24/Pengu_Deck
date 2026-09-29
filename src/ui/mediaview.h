/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mediaview.h - sortable, filterable column view over PdMediaItems
 *
 * Rows can be dragged onto a deck; double click loads into the deck that
 * is not playing (or A) and the "load-a" / "load-b" context entries pick
 * a deck explicitly.
 */
#ifndef PD_UI_MEDIAVIEW_H
#define PD_UI_MEDIAVIEW_H

#include <gtk/gtk.h>

#include "app.h"
#include "playlists.h"

G_BEGIN_DECLS

#define PD_TYPE_MEDIA_VIEW (pd_media_view_get_type())
G_DECLARE_FINAL_TYPE(PdMediaView, pd_media_view, PD, MEDIA_VIEW, GtkBox)

GtkWidget *pd_media_view_new(struct app *app, GListStore *store,
			     const char *placeholder);
void pd_media_view_set_filter(PdMediaView *v, const char *text);
/* Show another collection; @playlist enables "remove from playlist". */
void pd_media_view_set_store(PdMediaView *v, GListStore *store,
			     struct playlist *playlist);
GListStore *pd_media_view_store(PdMediaView *v);
GtkWidget *pd_media_view_search_entry(PdMediaView *v);
void pd_media_view_load_selected(PdMediaView *v, int idx);
void pd_media_view_queue_selected(PdMediaView *v);
/* A "+ Queue" button bound to this view. */
GtkWidget *pd_media_view_queue_button(PdMediaView *v);
guint pd_media_view_count(PdMediaView *v);

G_END_DECLS

#endif /* PD_UI_MEDIAVIEW_H */
