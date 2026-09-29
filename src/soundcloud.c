// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * soundcloud.c - SoundCloud public API client
 */
#include <string.h>

#include <json-glib/json-glib.h>

#include "net.h"
#include "soundcloud.h"

#define API		"https://api.soundcloud.com"
#define TOKEN_URL	"https://secure.soundcloud.com/oauth/token"
#define PAGE_LIMIT	50
#define MAX_TRACKS	200
#define ACCESS		"access=playable,preview"
/* refresh this long before the token actually expires */
#define EXPIRY_MARGIN	60

G_DEFINE_QUARK(pd-soundcloud-error-quark, sc_error)

/* ---- json helpers ------------------------------------------------ */

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

/* ---- session ----------------------------------------------------- */

static struct {
	GMutex lock;
	char *client_id;
	char *client_secret;
	struct sc_tokens t;
	void (*changed)(gpointer data);
	gpointer changed_data;
} ses;

static gboolean changed_idle(gpointer data)
{
	if (ses.changed)
		ses.changed(ses.changed_data);
	return G_SOURCE_REMOVE;
}

/* Call with the lock held. */
static void notify_changed(void)
{
	g_idle_add(changed_idle, NULL);
}

static void tokens_copy(struct sc_tokens *dst, const struct sc_tokens *src)
{
	dst->access = g_strdup(src->access);
	dst->refresh = g_strdup(src->refresh);
	dst->expires_at = src->expires_at;
	dst->user = src->user;
}

void sc_tokens_clear(struct sc_tokens *t)
{
	g_clear_pointer(&t->access, g_free);
	g_clear_pointer(&t->refresh, g_free);
	t->expires_at = 0;
	t->user = FALSE;
}

void sc_session_set_app(const char *client_id, const char *client_secret)
{
	gboolean same;

	g_mutex_lock(&ses.lock);
	same = g_strcmp0(client_id, ses.client_id) == 0 &&
	       g_strcmp0(client_secret, ses.client_secret) == 0;
	g_free(ses.client_id);
	g_free(ses.client_secret);
	ses.client_id = client_id && *client_id ? g_strdup(client_id) : NULL;
	ses.client_secret = client_secret && *client_secret ?
			    g_strdup(client_secret) : NULL;
	/* tokens belong to the app that issued them */
	if (!same)
		sc_tokens_clear(&ses.t);
	g_mutex_unlock(&ses.lock);
}

gboolean sc_session_has_app(void)
{
	gboolean has;

	g_mutex_lock(&ses.lock);
	has = ses.client_id && ses.client_secret;
	g_mutex_unlock(&ses.lock);
	return has;
}

void sc_session_set_tokens(const struct sc_tokens *t)
{
	g_mutex_lock(&ses.lock);
	sc_tokens_clear(&ses.t);
	if (t)
		tokens_copy(&ses.t, t);
	g_mutex_unlock(&ses.lock);
}

void sc_session_get_tokens(struct sc_tokens *out)
{
	g_mutex_lock(&ses.lock);
	tokens_copy(out, &ses.t);
	g_mutex_unlock(&ses.lock);
}

gboolean sc_session_logged_in(void)
{
	gboolean in;

	g_mutex_lock(&ses.lock);
	in = ses.t.user && ses.t.access;
	g_mutex_unlock(&ses.lock);
	return in;
}

void sc_session_logout(void)
{
	g_mutex_lock(&ses.lock);
	sc_tokens_clear(&ses.t);
	notify_changed();
	g_mutex_unlock(&ses.lock);
}

void sc_session_set_changed(void (*cb)(gpointer data), gpointer data)
{
	ses.changed = cb;
	ses.changed_data = data;
}

gboolean sc_parse_token_reply(const char *json, struct sc_tokens *out,
			      GError **err)
{
	JsonNode *root = parse_json(json, err);
	JsonObject *o;
	const char *access;

	if (!root)
		return FALSE;
	o = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
	access = str_member(o, "access_token");
	if (!access || !*access) {
		const char *why = str_member(o, "error_description");

		g_set_error(err, SC_ERROR, SC_ERROR_AUTH, "%s", why ? why :
			    str_member(o, "error") ? str_member(o, "error") :
			    "SoundCloud sent no access token");
		json_node_unref(root);
		return FALSE;
	}
	memset(out, 0, sizeof(*out));
	out->access = g_strdup(access);
	out->refresh = g_strdup(str_member(o, "refresh_token"));
	if (int_member(o, "expires_in") > 0)
		out->expires_at = g_get_real_time() / G_USEC_PER_SEC +
				  int_member(o, "expires_in");
	json_node_unref(root);
	return TRUE;
}

/*
 * One token request against the OAuth endpoint.  @grant is the form
 * body without the client credentials, which are appended here.  Call
 * with the lock held; the result replaces the session tokens.
 */
static gboolean token_request(const char *grant, gboolean user, GError **err)
{
	char *id = g_uri_escape_string(ses.client_id, NULL, FALSE);
	char *secret = g_uri_escape_string(ses.client_secret, NULL, FALSE);
	char *form = g_strdup_printf("%s&client_id=%s&client_secret=%s",
				     grant, id, secret);
	char *reply = NULL, *body;
	struct sc_tokens t = { 0 };
	long status = 0;
	gboolean ok = FALSE;

	body = net_post_form(TOKEN_URL, form, NULL, &status, &reply, err);
	if (!body) {
		if (reply && sc_parse_token_reply(reply, &t, NULL)) {
			/* not reachable: an error status with a token */
			sc_tokens_clear(&t);
		} else if (reply) {
			JsonNode *r = parse_json(reply, NULL);
			const char *why = r && JSON_NODE_HOLDS_OBJECT(r) ?
				str_member(json_node_get_object(r),
					   "error_description") : NULL;

			if (why) {
				g_clear_error(err);
				g_set_error(err, SC_ERROR, SC_ERROR_AUTH,
					    "SoundCloud: %s", why);
			}
			if (r)
				json_node_unref(r);
		}
		goto out;
	}
	if (!sc_parse_token_reply(body, &t, err))
		goto out;
	t.user = user;
	sc_tokens_clear(&ses.t);
	ses.t = t;
	memset(&t, 0, sizeof(t));
	notify_changed();
	ok = TRUE;
out:
	sc_tokens_clear(&t);
	g_free(body);
	g_free(reply);
	g_free(form);
	g_free(secret);
	g_free(id);
	return ok;
}

char *sc_session_client_id(void)
{
	char *id;

	g_mutex_lock(&ses.lock);
	id = g_strdup(ses.client_id);
	g_mutex_unlock(&ses.lock);
	return id;
}

char *sc_session_post_grant(const char *grant, char **reply, long *status,
			    GError **err)
{
	char *id, *secret, *form, *body;

	g_mutex_lock(&ses.lock);
	id = g_uri_escape_string(ses.client_id ? ses.client_id : "", NULL,
				 FALSE);
	secret = g_uri_escape_string(ses.client_secret ? ses.client_secret :
				     "", NULL, FALSE);
	g_mutex_unlock(&ses.lock);
	form = g_strdup_printf("%s&client_id=%s&client_secret=%s", grant, id,
			       secret);
	body = net_post_form(TOKEN_URL, form, NULL, status, reply, err);
	g_free(form);
	g_free(secret);
	g_free(id);
	return body;
}

/* Call with the lock held. */
static gboolean token_valid(void)
{
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	return ses.t.access && (ses.t.expires_at == 0 ||
				ses.t.expires_at > now + EXPIRY_MARGIN);
}

char *sc_session_token(GError **err)
{
	char *token = NULL;

	g_mutex_lock(&ses.lock);
	if (!ses.client_id || !ses.client_secret) {
		g_set_error_literal(err, SC_ERROR, SC_ERROR_NO_APP,
				    "No SoundCloud app configured. Enter the "
				    "client ID and secret of your registered "
				    "app in Preferences → SoundCloud.");
		goto out;
	}
	if (token_valid())
		goto done;
	/* refresh tokens are single use: one refresh at a time, in here */
	if (ses.t.refresh) {
		char *r = g_uri_escape_string(ses.t.refresh, NULL, FALSE);
		char *grant = g_strdup_printf("grant_type=refresh_token"
					      "&refresh_token=%s", r);
		gboolean user = ses.t.user;
		GError *e = NULL;

		if (token_request(grant, user, &e)) {
			g_free(grant);
			g_free(r);
			goto done;
		}
		g_free(grant);
		g_free(r);
		if (user) {
			/* the login has lapsed for good */
			sc_tokens_clear(&ses.t);
			notify_changed();
			g_set_error(err, SC_ERROR, SC_ERROR_NO_TOKEN,
				    "Your SoundCloud login has expired (%s). "
				    "Please log in again.", e->message);
			g_error_free(e);
			goto out;
		}
		g_error_free(e);
		sc_tokens_clear(&ses.t);
	}
	/* no user: the app's own credentials serve public content */
	if (!token_request("grant_type=client_credentials", FALSE, err))
		goto out;
done:
	token = g_strdup(ses.t.access);
out:
	g_mutex_unlock(&ses.lock);
	return token;
}

/* ---- requests ---------------------------------------------------- */

static void refuse(GError **err, long status)
{
	g_clear_error(err);
	if (status == 401 || status == 403)
		g_set_error(err, SC_ERROR, SC_ERROR_AUTH,
			    "SoundCloud refused the request (HTTP %ld)",
			    status);
	else if (status == 404)
		g_set_error_literal(err, SC_ERROR, SC_ERROR_NOT_FOUND,
				    "Not found on SoundCloud");
	else if (status == 429)
		g_set_error_literal(err, SC_ERROR, SC_ERROR_AUTH,
				    "SoundCloud rate limit reached, try "
				    "again later");
}

/* GET @url with the session token; a 401 gets one refresh and retry. */
static char *api_get(const char *url, GError **err)
{
	int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		char *token = sc_session_token(err);
		char *hdr, *body;
		long status = 0;

		if (!token)
			return NULL;
		hdr = g_strdup_printf("OAuth %s", token);
		body = net_get(url, hdr, &status, err);
		g_free(hdr);
		if (body) {
			g_free(token);
			return body;
		}
		if (status == 401 && attempt == 0) {
			struct sc_tokens t;

			/* expire it so the next call refreshes */
			g_mutex_lock(&ses.lock);
			ses.t.expires_at = 1;
			tokens_copy(&t, &ses.t);
			g_mutex_unlock(&ses.lock);
			sc_tokens_clear(&t);
			g_clear_error(err);
			g_free(token);
			continue;
		}
		g_free(token);
		if (status)
			refuse(err, status);
		return NULL;
	}
	return NULL;
}

/* ---- tracks ------------------------------------------------------ */

static gint64 id_from_urn(const char *urn)
{
	const char *p = urn ? strrchr(urn, ':') : NULL;

	return p ? g_ascii_strtoll(p + 1, NULL, 10) : 0;
}

static PdMediaItem *parse_track(JsonObject *o)
{
	const char *access = str_member(o, "access");
	const char *urn = str_member(o, "urn");
	const char *genre;
	gint64 id = int_member(o, "id");
	PdMediaItem *m;
	char *key;

	if (!o || !str_member(o, "title"))
		return NULL;
	if (!id)
		id = id_from_urn(urn);
	if (!id)
		return NULL;

	key = g_strdup_printf("soundcloud:%" G_GINT64_FORMAT, id);
	m = pd_media_item_new(MEDIA_SOUNDCLOUD, key);
	g_free(key);

	m->title = g_strdup(str_member(o, "title"));
	m->artist = g_strdup(str_member(obj_member(o, "user"), "username"));
	genre = str_member(o, "genre");
	m->genre = g_strdup(genre ? genre : "");
	m->album = g_strdup("");
	m->duration = int_member(o, "duration") / 1000.0;
	m->permalink = g_strdup(str_member(o, "permalink_url"));
	/* the streams endpoint wants the urn */
	m->location = urn ? g_strdup(urn) :
		      g_strdup_printf("soundcloud:tracks:%" G_GINT64_FORMAT,
				      id);
	m->preview = g_strcmp0(access, "preview") == 0;
	if (g_strcmp0(access, "blocked") == 0)
		g_clear_pointer(&m->location, g_free);
	return m;
}

static void add_track_node(GPtrArray *out, JsonNode *n)
{
	PdMediaItem *m;

	if (!JSON_NODE_HOLDS_OBJECT(n))
		return;
	m = parse_track(json_node_get_object(n));
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

/* Fetch a paginated track collection, following next_href. */
static GPtrArray *fetch_tracks(const char *first_url, GError **err)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	char *url = g_strdup(first_url);

	while (url && out->len < MAX_TRACKS) {
		char *body = api_get(url, err);
		GPtrArray *page;
		JsonNode *root;
		const char *next = NULL;
		guint i;

		g_free(url);
		url = NULL;
		if (!body) {
			if (out->len == 0) {
				g_ptr_array_unref(out);
				return NULL;
			}
			g_clear_error(err);
			break;
		}
		page = sc_parse_tracks(body, NULL);
		for (i = 0; page && i < page->len; i++)
			g_ptr_array_add(out, g_object_ref(page->pdata[i]));
		if (page)
			g_ptr_array_unref(page);
		root = parse_json(body, NULL);
		if (root && JSON_NODE_HOLDS_OBJECT(root))
			next = str_member(json_node_get_object(root),
					  "next_href");
		if (next && *next)
			url = g_strdup(next);
		if (root)
			json_node_unref(root);
		g_free(body);
	}
	g_free(url);
	return out;
}

GPtrArray *sc_search(const char *query, GError **err)
{
	char *q = g_uri_escape_string(query, NULL, FALSE);
	char *url = g_strdup_printf(API "/tracks?q=%s&" ACCESS "&limit=%d"
				    "&linked_partitioning=true", q,
				    PAGE_LIMIT);
	char *body = api_get(url, err);
	GPtrArray *res = body ? sc_parse_tracks(body, err) : NULL;

	g_free(body);
	g_free(url);
	g_free(q);
	return res;
}

GPtrArray *sc_resolve(const char *link, GError **err)
{
	GPtrArray *res = NULL;
	char *q, *url, *body;
	JsonNode *root;
	JsonObject *o;
	const char *kind, *urn;

	q = g_uri_escape_string(link, NULL, FALSE);
	url = g_strdup_printf(API "/resolve?url=%s", q);
	body = api_get(url, err);
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
	urn = str_member(o, "urn");

	if (kind && strcmp(kind, "track") == 0) {
		res = g_ptr_array_new_with_free_func(g_object_unref);
		add_track_node(res, root);
	} else if (kind && strcmp(kind, "playlist") == 0 && urn) {
		char *u = g_uri_escape_string(urn, NULL, FALSE);

		url = g_strdup_printf(API "/playlists/%s/tracks?" ACCESS
				      "&linked_partitioning=true", u);
		res = fetch_tracks(url, err);
		g_free(url);
		g_free(u);
	} else if (kind && strcmp(kind, "user") == 0 && urn) {
		char *u = g_uri_escape_string(urn, NULL, FALSE);

		url = g_strdup_printf(API "/users/%s/tracks?" ACCESS
				      "&limit=%d&linked_partitioning=true",
				      u, PAGE_LIMIT);
		res = fetch_tracks(url, err);
		g_free(url);
		g_free(u);
	} else {
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_FOUND,
			    "That link is not a SoundCloud track, playlist "
			    "or artist");
	}
	json_node_unref(root);
	return res;
}

GPtrArray *sc_likes(GError **err)
{
	if (!sc_session_logged_in()) {
		g_set_error_literal(err, SC_ERROR, SC_ERROR_NO_TOKEN,
				    "Log in to SoundCloud to see your likes.");
		return NULL;
	}
	return fetch_tracks(API "/me/likes/tracks?" ACCESS "&limit=200"
			    "&linked_partitioning=true", err);
}

/* Progressive MP3 first: FFmpeg seeks it freely; then the HLS variants. */
static const char *const stream_keys[] = {
	"http_mp3_128_url",
	"hls_mp3_128_url",
	"hls_aac_160_url",
	"hls_opus_64_url",
	"preview_mp3_128_url",
};

char *sc_stream_url(PdMediaItem *m, GError **err)
{
	char *u, *url, *body, *res = NULL;
	JsonNode *root;
	JsonObject *o;
	guint i;

	if (!m->location) {
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_STREAMABLE,
			    "\"%s\" cannot be streamed (blocked by the "
			    "rights holder or not available in your "
			    "country)", m->title);
		return NULL;
	}
	u = g_uri_escape_string(m->location, NULL, FALSE);
	url = g_strdup_printf(API "/tracks/%s/streams", u);
	body = api_get(url, err);
	g_free(url);
	g_free(u);
	if (!body)
		return NULL;

	root = parse_json(body, err);
	g_free(body);
	if (!root)
		return NULL;
	o = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
	for (i = 0; i < G_N_ELEMENTS(stream_keys) && !res; i++) {
		const char *s = str_member(o, stream_keys[i]);

		if (s && *s)
			res = g_strdup(s);
	}
	if (!res)
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_STREAMABLE,
			    "SoundCloud did not return a stream");
	json_node_unref(root);
	return res;
}
