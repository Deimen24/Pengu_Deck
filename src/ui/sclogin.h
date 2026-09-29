/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sclogin.h - log in to SoundCloud inside the app
 *
 * Opens soundcloud.com in an embedded WebKit view; once the site has set
 * its oauth_token cookie the token is copied into the settings, the
 * public client id is detected and the window closes.
 */
#ifndef PD_UI_SCLOGIN_H
#define PD_UI_SCLOGIN_H

#include <gtk/gtk.h>

#include "app.h"

bool sclogin_available(void);

/* @done runs on the main thread after a successful login. */
void sclogin_show(struct app *a, void (*done)(gpointer data), gpointer data);

/* Forget the token and the browser session. */
void sclogin_logout(struct app *a);

#endif /* PD_UI_SCLOGIN_H */
