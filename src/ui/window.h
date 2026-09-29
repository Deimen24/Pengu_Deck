/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PD_UI_WINDOW_H
#define PD_UI_WINDOW_H

#include <gtk/gtk.h>

#include "app.h"

G_BEGIN_DECLS

#define PD_TYPE_WINDOW (pd_window_get_type())
G_DECLARE_FINAL_TYPE(PdWindow, pd_window, PD, WINDOW, GtkApplicationWindow)

GtkWidget *pd_window_new(struct app *app);
void pd_window_load_file(PdWindow *w, const char *path);

G_END_DECLS

/* Close a secondary window with the Escape key. */
void pd_window_close_on_escape(GtkWindow *win);

#endif /* PD_UI_WINDOW_H */
