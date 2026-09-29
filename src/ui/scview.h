/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * scview.h - SoundCloud search panel
 */
#ifndef PD_UI_SCVIEW_H
#define PD_UI_SCVIEW_H

#include <gtk/gtk.h>

#include "app.h"
#include "mediaview.h"

G_BEGIN_DECLS

#define PD_TYPE_SC_VIEW (pd_sc_view_get_type())
G_DECLARE_FINAL_TYPE(PdScView, pd_sc_view, PD, SC_VIEW, GtkBox)

GtkWidget *pd_sc_view_new(struct app *app);
PdMediaView *pd_sc_view_media(PdScView *v);

G_END_DECLS

#endif /* PD_UI_SCVIEW_H */
