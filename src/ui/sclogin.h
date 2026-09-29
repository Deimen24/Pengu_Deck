/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sclogin.h - the SoundCloud login window
 *
 * Signs the user in through their browser with OAuth 2.1 and PKCE; see
 * scauth.h for the flow.
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
