/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * scauth.h - SoundCloud OAuth 2.1 login with PKCE
 *
 * The user signs in with their everyday browser: the app opens the
 * authorization page, SoundCloud redirects the browser to a loopback
 * address the app listens on, and the app trades the code it receives
 * for tokens.  Nothing of the user's password ever passes through the
 * app, and no embedded browser is involved.
 */
#ifndef PD_SCAUTH_H
#define PD_SCAUTH_H

#include <glib.h>

/* The redirect URI to register with the SoundCloud app. */
#define SC_REDIRECT_URI	"http://127.0.0.1:38472/callback"
#define SC_REDIRECT_PORT	38472

struct sc_login;

/*
 * Start listening for the redirect and return the authorization URL to
 * open in the browser (free it).  @done runs on the main loop with
 * @message NULL on success, else the reason.  NULL with @err when the
 * loopback port is taken or no app is configured.
 */
struct sc_login *sc_login_start(void (*done)(const char *message,
					     gpointer data),
				gpointer data, char **authorize_url,
				GError **err);

/* Stop listening; @done is not called. */
void sc_login_cancel(struct sc_login *l);

/* RFC 7636: S256 challenge for @verifier, base64url without padding. */
char *sc_pkce_challenge(const char *verifier);

#endif /* PD_SCAUTH_H */
