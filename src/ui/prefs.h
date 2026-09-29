/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * prefs.h - preferences window
 */
#ifndef PD_UI_PREFS_H
#define PD_UI_PREFS_H

#include <gtk/gtk.h>

#include "app.h"

/*
 * Opens the preferences window.  @applied is called on the main thread
 * after settings changed, so the views can pick up new values.
 */
void prefs_show(struct app *app, GCallback applied, gpointer data);

#endif /* PD_UI_PREFS_H */
