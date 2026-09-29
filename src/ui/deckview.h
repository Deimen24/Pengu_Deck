/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * deckview.h - transport, pitch, loops and hot cues for one deck
 */
#ifndef PD_UI_DECKVIEW_H
#define PD_UI_DECKVIEW_H

#include <gtk/gtk.h>

#include "app.h"

G_BEGIN_DECLS

#define PD_TYPE_DECK_VIEW (pd_deck_view_get_type())
G_DECLARE_FINAL_TYPE(PdDeckView, pd_deck_view, PD, DECK_VIEW, GtkBox)

GtkWidget *pd_deck_view_new(struct app *app, int idx);

/* Keyboard shortcut entry points, see window.c. */
void pd_deck_view_action(PdDeckView *v, const char *action, gboolean press);
void pd_deck_view_apply_config(PdDeckView *v);

G_END_DECLS

#endif /* PD_UI_DECKVIEW_H */
