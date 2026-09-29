/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * soundcloud.h - SoundCloud search, links, likes and stream resolution
 *
 * All functions block and are meant to run on worker threads.  Result
 * arrays hold PdMediaItem references and free them with the array.
 */
#ifndef PD_SOUNDCLOUD_H
#define PD_SOUNDCLOUD_H

#include <glib.h>

#include "media.h"

#define SC_ERROR (sc_error_quark())
GQuark sc_error_quark(void);

enum sc_error_code {
	SC_ERROR_NO_CLIENT_ID,
	SC_ERROR_NO_TOKEN,
	SC_ERROR_PARSE,
	SC_ERROR_NOT_FOUND,
	SC_ERROR_NOT_STREAMABLE,
};

struct sc_auth {
	char *client_id;
	char *token;		/* optional OAuth token */
};

GPtrArray *sc_search(const struct sc_auth *a, const char *query,
		     GError **err);
GPtrArray *sc_resolve(const struct sc_auth *a, const char *url,
		      GError **err);
GPtrArray *sc_likes(const struct sc_auth *a, GError **err);

/* Returns a URL FFmpeg can open (progressive MP3 or an HLS playlist). */
char *sc_stream_url(const struct sc_auth *a, PdMediaItem *item,
		    GError **err);

/* Scrape the public client id the SoundCloud web player uses. */
char *sc_detect_client_id(GError **err);

/* Exposed for the tests. */
GPtrArray *sc_parse_tracks(const char *json, GError **err);

#endif /* PD_SOUNDCLOUD_H */
