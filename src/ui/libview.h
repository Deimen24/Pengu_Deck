/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * libview.h - local music library panel
 */
#ifndef PD_UI_LIBVIEW_H
#define PD_UI_LIBVIEW_H

#include <gtk/gtk.h>

#include "app.h"
#include "mediaview.h"

G_BEGIN_DECLS

#define PD_TYPE_LIB_VIEW (pd_lib_view_get_type())
G_DECLARE_FINAL_TYPE(PdLibView, pd_lib_view, PD, LIB_VIEW, GtkBox)

GtkWidget *pd_lib_view_new(struct app *app);
void pd_lib_view_rescan(PdLibView *v);
PdMediaView *pd_lib_view_media(PdLibView *v);

G_END_DECLS

#endif /* PD_UI_LIBVIEW_H */
