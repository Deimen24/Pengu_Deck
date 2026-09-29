/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * queueview.h - automix panel: queue list and transition settings
 */
#ifndef PD_UI_QUEUEVIEW_H
#define PD_UI_QUEUEVIEW_H

#include <gtk/gtk.h>

#include "app.h"

G_BEGIN_DECLS

#define PD_TYPE_QUEUE_VIEW (pd_queue_view_get_type())
G_DECLARE_FINAL_TYPE(PdQueueView, pd_queue_view, PD, QUEUE_VIEW, GtkBox)

GtkWidget *pd_queue_view_new(struct app *app);

G_END_DECLS

#endif /* PD_UI_QUEUEVIEW_H */
