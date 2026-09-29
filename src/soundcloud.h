/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * soundcloud.h - SoundCloud public API client
 *
 * Talks to https://api.soundcloud.com with the OAuth 2.1 credentials of
 * a registered SoundCloud app.  Without a signed in user the app's own
 * client credentials token serves public content; after the PKCE login
 * (scauth.h) the user's token serves their likes and playlists too.
 * Tokens are kept in one process wide session, refreshed on demand and
 * handed back to the app for saving through a change hook.
 */
#ifndef PD_SOUNDCLOUD_H
#define PD_SOUNDCLOUD_H

#include <glib.h>

#include "media.h"

#define SC_ERROR (sc_error_quark())
GQuark sc_error_quark(void);

enum sc_error_code {
	SC_ERROR_NO_APP,	/* client id / secret missing */
	SC_ERROR_NO_TOKEN,	/* needs a signed in user */
	SC_ERROR_AUTH,		/* SoundCloud refused the credentials */
	SC_ERROR_PARSE,
	SC_ERROR_NOT_FOUND,
	SC_ERROR_NOT_STREAMABLE,
};

/* ---- session ----------------------------------------------------- */

struct sc_tokens {
	char *access;
	char *refresh;
	gint64 expires_at;	/* unix seconds, 0 = unknown */
	gboolean user;		/* belongs to a signed in user */
};

/* App credentials from the SoundCloud developer registration. */
void sc_session_set_app(const char *client_id, const char *client_secret);
gboolean sc_session_has_app(void);

/* Replace the tokens (from the saved config or a fresh login). */
void sc_session_set_tokens(const struct sc_tokens *t);
/* Copy of the current tokens, free with sc_tokens_clear(). */
void sc_session_get_tokens(struct sc_tokens *out);
void sc_tokens_clear(struct sc_tokens *t);
gboolean sc_session_logged_in(void);
/* Drop the user's tokens (public content keeps working). */
void sc_session_logout(void);

/* Called on the main loop whenever the tokens change, e.g. a refresh. */
void sc_session_set_changed(void (*cb)(gpointer data), gpointer data);

/*
 * A valid access token, refreshing or requesting one as needed.  Safe
 * from any thread; blocks on the network.  NULL with @err set.
 */
char *sc_session_token(GError **err);

/* For the login: the app's client id (free it). */
char *sc_session_client_id(void);
/*
 * For the login: POST @grant to the token endpoint with the app's
 * credentials appended, returning the reply body; on failure NULL with
 * @err, and the error body in @reply when the server sent one.
 */
char *sc_session_post_grant(const char *grant, char **reply, long *status,
			    GError **err);

/* ---- content ----------------------------------------------------- */

GPtrArray *sc_search(const char *query, GError **err);
GPtrArray *sc_resolve(const char *url, GError **err);
GPtrArray *sc_likes(GError **err);

/* Returns a URL FFmpeg can open (progressive MP3 or an HLS playlist). */
char *sc_stream_url(PdMediaItem *item, GError **err);

/* Exposed for the tests. */
GPtrArray *sc_parse_tracks(const char *json, GError **err);
gboolean sc_parse_token_reply(const char *json, struct sc_tokens *out,
			      GError **err);

#endif /* PD_SOUNDCLOUD_H */
