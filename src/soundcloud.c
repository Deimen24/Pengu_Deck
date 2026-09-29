// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * soundcloud.c - SoundCloud api-v2 client
 */
#include <stdbool.h>
#include <string.h>

#include <json-glib/json-glib.h>

#include "net.h"
#include "soundcloud.h"

#define API		"https://api-v2.soundcloud.com"
#define PAGE_LIMIT	50
#define MAX_TRACKS	200
#define IDS_PER_CALL	50

G_DEFINE_QUARK(pd-soundcloud-error-quark, sc_error)

static const char *str_member(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	if (JSON_NODE_HOLDS_VALUE(n) &&
	    json_node_get_value_type(n) == G_TYPE_STRING)
		return json_node_get_string(n);
	return NULL;
}

static gint64 int_member(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return 0;
	n = json_object_get_member(o, name);
	if (!JSON_NODE_HOLDS_VALUE(n))
		return 0;
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return json_node_get_int(n);
	if (json_node_get_value_type(n) == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(n);
	return 0;
}

static JsonObject *obj_member(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	return JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

static JsonArray *arr_member(JsonObject *o, const char *name)
{
	JsonNode *n;

	if (!o || !json_object_has_member(o, name))
		return NULL;
	n = json_object_get_member(o, name);
	return JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}

/*
 * Rank a transcoding: plain progressive MP3 first, then HLS MP3, Opus and
 * AAC.  Encrypted (DRM) streams cannot be played and score zero.
 */
static int transcoding_score(JsonObject *tc)
{
	JsonObject *fmt = obj_member(tc, "format");
	const char *proto = str_member(fmt, "protocol");
	const char *mime = str_member(fmt, "mime_type");
	int score;

	if (!proto || !mime || !str_member(tc, "url"))
		return 0;
	if (strstr(proto, "encrypted"))
		return 0;
	if (g_str_has_prefix(mime, "audio/mpeg"))
		score = 30;
	else if (g_str_has_prefix(mime, "audio/ogg"))
		score = 20;
	else if (g_str_has_prefix(mime, "audio/mp4") ||
		 g_str_has_prefix(mime, "audio/aac"))
		score = 10;
	else
		return 0;
	if (strcmp(proto, "progressive") == 0)
		score += 5;
	else if (strcmp(proto, "hls") != 0)
		return 0;
	if (json_object_has_member(tc, "snipped") &&
	    json_object_get_boolean_member(tc, "snipped"))
		score -= 8;
	return score;
}

static PdMediaItem *parse_track(JsonObject *o)
{
	JsonArray *tcs = arr_member(obj_member(o, "media"), "transcodings");
	JsonObject *best = NULL;
	int best_score = 0;
	const char *genre, *policy;
	PdMediaItem *m;
	char *key;
	guint i;

	if (!o || !str_member(o, "title"))
		return NULL;

	for (i = 0; tcs && i < json_array_get_length(tcs); i++) {
		JsonObject *tc = json_array_get_object_element(tcs, i);
		int s = tc ? transcoding_score(tc) : 0;

		if (s > best_score) {
			best_score = s;
			best = tc;
		}
	}

	key = g_strdup_printf("soundcloud:%" G_GINT64_FORMAT,
			      int_member(o, "id"));
	m = pd_media_item_new(MEDIA_SOUNDCLOUD, key);
	g_free(key);

	m->title = g_strdup(str_member(o, "title"));
	m->artist = g_strdup(str_member(obj_member(o, "user"), "username"));
	genre = str_member(o, "genre");
	m->genre = g_strdup(genre ? genre : "");
	m->album = g_strdup("");
	m->duration = int_member(o, "full_duration") / 1000.0;
	if (m->duration <= 0.0)
		m->duration = int_member(o, "duration") / 1000.0;
	m->permalink = g_strdup(str_member(o, "permalink_url"));
	m->track_auth = g_strdup(str_member(o, "track_authorization"));
	policy = str_member(o, "policy");
	m->preview = policy && strcmp(policy, "SNIP") == 0;
	if (best) {
		m->location = g_strdup(str_member(best, "url"));
		if (json_object_has_member(best, "snipped") &&
		    json_object_get_boolean_member(best, "snipped"))
			m->preview = TRUE;
	}
	return m;
}

static JsonNode *parse_json(const char *text, GError **err)
{
	JsonParser *p = json_parser_new();
	JsonNode *root = NULL;

	if (json_parser_load_from_data(p, text, -1, err))
		root = json_node_copy(json_parser_get_root(p));
	g_object_unref(p);
	if (!root && err && !*err)
		g_set_error(err, SC_ERROR, SC_ERROR_PARSE, "Empty response");
	return root;
}

/* Tracks either are the element or sit in its "track" member (likes). */
static void add_track_node(GPtrArray *out, JsonNode *n)
{
	JsonObject *o, *inner;
	PdMediaItem *m;

	if (!JSON_NODE_HOLDS_OBJECT(n))
		return;
	o = json_node_get_object(n);
	inner = obj_member(o, "track");
	m = parse_track(inner ? inner : o);
	if (m)
		g_ptr_array_add(out, m);
}

GPtrArray *sc_parse_tracks(const char *json, GError **err)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	JsonNode *root = parse_json(json, err);
	JsonArray *arr = NULL;
	guint i;

	if (!root) {
		g_ptr_array_unref(out);
		return NULL;
	}
	if (JSON_NODE_HOLDS_ARRAY(root))
		arr = json_node_get_array(root);
	else if (JSON_NODE_HOLDS_OBJECT(root))
		arr = arr_member(json_node_get_object(root), "collection");

	if (arr) {
		for (i = 0; i < json_array_get_length(arr); i++)
			add_track_node(out, json_array_get_element(arr, i));
	} else if (JSON_NODE_HOLDS_OBJECT(root)) {
		add_track_node(out, root);
	}
	json_node_unref(root);
	return out;
}

static char *auth_header(const struct sc_auth *a)
{
	if (!a->token || !*a->token)
		return NULL;
	if (g_str_has_prefix(a->token, "OAuth "))
		return g_strdup(a->token);
	return g_strdup_printf("OAuth %s", a->token);
}

static bool check_auth(const struct sc_auth *a, GError **err)
{
	if (a->client_id && *a->client_id)
		return true;
	g_set_error(err, SC_ERROR, SC_ERROR_NO_CLIENT_ID,
		    "No SoundCloud client ID set. Open Preferences and press "
		    "\"Detect\" or paste one.");
	return false;
}

/* GET @url with client_id (and OAuth header) added. */
static char *api_get(const struct sc_auth *a, const char *url, GError **err)
{
	char *full, *hdr, *body;
	long status = 0;

	full = g_strdup_printf("%s%sclient_id=%s", url,
			       strchr(url, '?') ? "&" : "?", a->client_id);
	hdr = auth_header(a);
	body = net_get(full, hdr, &status, err);
	if (!body && (status == 401 || status == 403) && err && *err) {
		g_clear_error(err);
		g_set_error(err, SC_ERROR, SC_ERROR_NO_CLIENT_ID,
			    "SoundCloud refused the request (HTTP %ld). The "
			    "client ID may have expired, press \"Detect\" in "
			    "Preferences.", status);
	}
	g_free(hdr);
	g_free(full);
	return body;
}

GPtrArray *sc_search(const struct sc_auth *a, const char *query,
		     GError **err)
{
	char *q, *url, *body;
	GPtrArray *res;

	if (!check_auth(a, err))
		return NULL;
	q = g_uri_escape_string(query, NULL, FALSE);
	url = g_strdup_printf(API "/search/tracks?q=%s&limit=%d", q,
			      PAGE_LIMIT);
	body = api_get(a, url, err);
	res = body ? sc_parse_tracks(body, err) : NULL;
	g_free(body);
	g_free(url);
	g_free(q);
	return res;
}

/* Fill in playlist entries that only carry an id. */
static void fetch_stubs(const struct sc_auth *a, GPtrArray *out,
			GArray *ids)
{
	guint i, j;

	for (i = 0; i < ids->len; i += IDS_PER_CALL) {
		GString *url = g_string_new(API "/tracks?ids=");
		GPtrArray *part;
		char *body;

		for (j = i; j < ids->len && j < i + IDS_PER_CALL; j++)
			g_string_append_printf(url, "%s%" G_GINT64_FORMAT,
					       j > i ? "%2C" : "",
					       g_array_index(ids, gint64, j));
		body = api_get(a, url->str, NULL);
		part = body ? sc_parse_tracks(body, NULL) : NULL;
		for (j = 0; part && j < part->len; j++)
			g_ptr_array_add(out, g_object_ref(part->pdata[j]));
		if (part)
			g_ptr_array_unref(part);
		g_free(body);
		g_string_free(url, TRUE);
	}
}

static GPtrArray *parse_playlist(const struct sc_auth *a, JsonObject *o)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	JsonArray *tracks = arr_member(o, "tracks");
	GArray *stubs = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;

	for (i = 0; tracks && i < json_array_get_length(tracks); i++) {
		JsonObject *t = json_array_get_object_element(tracks, i);
		gint64 id = int_member(t, "id");

		if (str_member(t, "title"))
			add_track_node(out, json_array_get_element(tracks, i));
		else if (id)
			g_array_append_val(stubs, id);
	}
	fetch_stubs(a, out, stubs);
	g_array_unref(stubs);
	return out;
}

static GPtrArray *user_tracks(const struct sc_auth *a, gint64 id,
			      const char *what, GError **err)
{
	GPtrArray *res = NULL;
	char *url, *body;

	url = g_strdup_printf(API "/users/%" G_GINT64_FORMAT "/%s?limit=%d",
			      id, what, MAX_TRACKS);
	body = api_get(a, url, err);
	if (body)
		res = sc_parse_tracks(body, err);
	g_free(body);
	g_free(url);
	return res;
}

GPtrArray *sc_resolve(const struct sc_auth *a, const char *link,
		      GError **err)
{
	GPtrArray *res = NULL;
	char *q, *url, *body;
	JsonNode *root;
	JsonObject *o;
	const char *kind;

	if (!check_auth(a, err))
		return NULL;
	q = g_uri_escape_string(link, NULL, FALSE);
	url = g_strdup_printf(API "/resolve?url=%s", q);
	body = api_get(a, url, err);
	g_free(url);
	g_free(q);
	if (!body)
		return NULL;

	root = parse_json(body, err);
	g_free(body);
	if (!root)
		return NULL;
	o = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
	kind = str_member(o, "kind");

	if (kind && strcmp(kind, "track") == 0) {
		res = g_ptr_array_new_with_free_func(g_object_unref);
		add_track_node(res, root);
	} else if (kind && strcmp(kind, "playlist") == 0) {
		res = parse_playlist(a, o);
	} else if (kind && strcmp(kind, "user") == 0) {
		res = user_tracks(a, int_member(o, "id"), "tracks", err);
	} else {
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_FOUND,
			    "That link is not a SoundCloud track, playlist "
			    "or artist");
	}
	json_node_unref(root);
	return res;
}

GPtrArray *sc_likes(const struct sc_auth *a, GError **err)
{
	JsonNode *root;
	char *body;
	gint64 id;

	if (!check_auth(a, err))
		return NULL;
	if (!a->token || !*a->token) {
		g_set_error(err, SC_ERROR, SC_ERROR_NO_TOKEN,
			    "Your likes need an OAuth token, see "
			    "Preferences.");
		return NULL;
	}
	body = api_get(a, API "/me", err);
	if (!body)
		return NULL;
	root = parse_json(body, err);
	g_free(body);
	if (!root)
		return NULL;
	id = JSON_NODE_HOLDS_OBJECT(root) ?
	     int_member(json_node_get_object(root), "id") : 0;
	json_node_unref(root);
	if (!id) {
		g_set_error(err, SC_ERROR, SC_ERROR_PARSE,
			    "Could not read your SoundCloud account");
		return NULL;
	}
	return user_tracks(a, id, "track_likes", err);
}

char *sc_stream_url(const struct sc_auth *a, PdMediaItem *m, GError **err)
{
	char *url, *body, *res = NULL;
	JsonNode *root;
	const char *u;

	if (!check_auth(a, err))
		return NULL;
	if (!m->location) {
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_STREAMABLE,
			    "\"%s\" has no playable stream (it may be DRM "
			    "protected or not available in your country)",
			    m->title);
		return NULL;
	}
	if (m->track_auth)
		url = g_strdup_printf("%s%strack_authorization=%s",
				      m->location,
				      strchr(m->location, '?') ? "&" : "?",
				      m->track_auth);
	else
		url = g_strdup(m->location);
	body = api_get(a, url, err);
	g_free(url);
	if (!body)
		return NULL;

	root = parse_json(body, err);
	g_free(body);
	if (!root)
		return NULL;
	u = JSON_NODE_HOLDS_OBJECT(root) ?
	    str_member(json_node_get_object(root), "url") : NULL;
	if (u)
		res = g_strdup(u);
	else
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_STREAMABLE,
			    "SoundCloud did not return a stream");
	json_node_unref(root);
	return res;
}

static char *find_client_id(const char *js)
{
	GRegex *re = g_regex_new("client_id\\s*[:=]\\s*\"?([0-9a-zA-Z]{32})",
				 0, 0, NULL);
	GMatchInfo *mi = NULL;
	char *id = NULL;

	if (g_regex_match(re, js, 0, &mi))
		id = g_match_info_fetch(mi, 1);
	g_match_info_free(mi);
	g_regex_unref(re);
	return id;
}

char *sc_detect_client_id(GError **err)
{
	GRegex *re;
	GMatchInfo *mi = NULL;
	GPtrArray *scripts = g_ptr_array_new_with_free_func(g_free);
	char *html, *id = NULL;
	int i;

	html = net_get("https://soundcloud.com/", NULL, NULL, err);
	if (!html) {
		g_ptr_array_unref(scripts);
		return NULL;
	}
	re = g_regex_new("<script[^>]+src=\"(https://[^\"]+sndcdn\\.com/"
			 "assets/[^\"]+\\.js)\"", 0, 0, NULL);
	g_regex_match(re, html, 0, &mi);
	while (g_match_info_matches(mi)) {
		g_ptr_array_add(scripts, g_match_info_fetch(mi, 1));
		g_match_info_next(mi, NULL);
	}
	g_match_info_free(mi);
	g_regex_unref(re);
	g_free(html);

	/* The id lives in one of the last bundles, walk backwards. */
	for (i = (int)scripts->len - 1; i >= 0 && !id; i--) {
		char *js = net_get(scripts->pdata[i], NULL, NULL, NULL);

		if (js)
			id = find_client_id(js);
		g_free(js);
	}
	g_ptr_array_unref(scripts);

	if (!id && err && !*err)
		g_set_error(err, SC_ERROR, SC_ERROR_NO_CLIENT_ID,
			    "Could not find a client ID on soundcloud.com");
	return id;
}
