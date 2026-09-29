// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * soundcloud.c - SoundCloud public API client
 */
#define G_LOG_DOMAIN "pengu-deck"

#include <string.h>

#include <json-glib/json-glib.h>

#include "net.h"
#include "soundcloud.h"

#define API		"https://api.soundcloud.com"
#define TOKEN_URL	"https://secure.soundcloud.com/oauth/token"

/* PENGU_DECK_SC_API / PENGU_DECK_SC_TOKEN_URL point the client at a
 * stand in server for testing. */
static const char *api_base(void)
{
	const char *e = g_getenv("PENGU_DECK_SC_API");

	return e && *e ? e : API;
}

static const char *token_url(void)
{
	const char *e = g_getenv("PENGU_DECK_SC_TOKEN_URL");

	return e && *e ? e : TOKEN_URL;
}
#define PAGE_LIMIT	50
#define MAX_TRACKS	200
/* Search results: full tracks only, or previews too (the default). */
static gboolean full_only;

void sc_set_full_only(gboolean on)
{
	full_only = on;
}

static const char *access_filter(void)
{
	return full_only ? "access=playable" : "access=playable,preview";
}
/* refresh this long before the token actually expires */
#define EXPIRY_MARGIN	60
/* after a failed token exchange, wait this long before asking again:
 * the exchange is rate limited per app and per address */
#define TOKEN_COOLDOWN	60
/* HTTP 429: back off 1, 2, 4 seconds, then give up */
#define BACKOFF_TRIES	3

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
	gint64 failed_at;	/* last failed exchange, unix seconds */
	char *failed_why;
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

	/* the environment wins, so credentials need not live in a file */
	if (g_getenv("SOUNDCLOUD_CLIENT_ID") && *g_getenv("SOUNDCLOUD_CLIENT_ID"))
		client_id = g_getenv("SOUNDCLOUD_CLIENT_ID");
	if (g_getenv("SOUNDCLOUD_CLIENT_SECRET") &&
	    *g_getenv("SOUNDCLOUD_CLIENT_SECRET"))
		client_secret = g_getenv("SOUNDCLOUD_CLIENT_SECRET");
	g_mutex_lock(&ses.lock);
	ses.failed_at = 0;
	g_clear_pointer(&ses.failed_why, g_free);
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
	char *form, *basic = NULL, *reply = NULL, *body;
	struct sc_tokens t = { 0 };
	long status = 0;
	gboolean ok = FALSE;
	int try;

	if (g_str_has_prefix(grant, "grant_type=client_credentials")) {
		/* this grant takes the credentials as HTTP Basic only */
		char *pair = g_strdup_printf("%s:%s", ses.client_id,
					     ses.client_secret);
		char *b64 = g_base64_encode((const guchar *)pair,
					    strlen(pair));

		basic = g_strdup_printf("Basic %s", b64);
		form = g_strdup(grant);
		g_free(b64);
		g_free(pair);
	} else {
		form = g_strdup_printf("%s&client_id=%s&client_secret=%s",
				       grant, id, secret);
	}
	for (try = 0; ; try++) {
		g_clear_error(err);
		g_clear_pointer(&reply, g_free);
		body = net_post_form(token_url(), form, basic, &status, &reply,
				     err);
		if (body || status != 429 || try + 1 >= BACKOFF_TRIES)
			break;
		g_usleep((gulong)(1 << try) * G_USEC_PER_SEC);
	}
	if (!body && status == 429) {
		g_clear_error(err);
		g_set_error_literal(err, SC_ERROR, SC_ERROR_AUTH,
				    "SoundCloud is rate limiting token "
				    "requests, try again in a while");
	}
	if (!body) {
		if (reply && sc_parse_token_reply(reply, &t, NULL)) {
			/* not reachable: an error status with a token */
			sc_tokens_clear(&t);
		} else if (reply) {
			JsonNode *r = parse_json(reply, NULL);
			JsonObject *o = r && JSON_NODE_HOLDS_OBJECT(r) ?
					json_node_get_object(r) : NULL;
			const char *why = str_member(o, "error_description");
			const char *code = str_member(o, "error");

			if (!why && g_strcmp0(code, "invalid_client") == 0)
				why = "the client ID or secret is wrong, "
				      "check Preferences → SoundCloud";
			else if (!why && g_strcmp0(code, "invalid_grant") == 0)
				why = "the login has expired, please log in "
				      "again";
			else if (!why)
				why = code;
			if (why) {
				g_clear_error(err);
				g_set_error(err, SC_ERROR, SC_ERROR_AUTH,
					    "%s", why);
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
	if (!ok) {
		ses.failed_at = g_get_real_time() / G_USEC_PER_SEC;
		g_free(ses.failed_why);
		ses.failed_why = g_strdup(err && *err ? (*err)->message :
					  "token request failed");
	} else {
		ses.failed_at = 0;
		g_clear_pointer(&ses.failed_why, g_free);
	}
	sc_tokens_clear(&t);
	g_free(body);
	g_free(reply);
	g_free(form);
	g_free(basic);
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
	body = net_post_form(token_url(), form, NULL, status, reply, err);
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
	if (ses.failed_at &&
	    g_get_real_time() / G_USEC_PER_SEC < ses.failed_at + TOKEN_COOLDOWN) {
		g_set_error(err, SC_ERROR, SC_ERROR_AUTH, "%s (retrying in a "
			    "minute)", ses.failed_why);
		goto out;
	}
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
	if ((status == 401 || status == 403) && !sc_session_logged_in())
		g_set_error_literal(err, SC_ERROR, SC_ERROR_NO_TOKEN,
				    "SoundCloud allows this only for a signed "
				    "in user: press \"Log in to SoundCloud\"");
	else if (status == 401 || status == 403)
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
		{
			int try;

			for (try = 0; ; try++) {
				g_clear_error(err);
				body = net_get(url, hdr, &status, err);
				if (body || status != 429 ||
				    try + 1 >= BACKOFF_TRIES)
					break;
				g_usleep((gulong)(1 << try) * G_USEC_PER_SEC);
			}
		}
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
	char *url = g_strdup_printf("%s/tracks?q=%s&%s&limit=%d"
				    "&linked_partitioning=true", api_base(),
				    q, access_filter(), PAGE_LIMIT);
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
	url = g_strdup_printf("%s/resolve?url=%s", api_base(), q);
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
		url = g_strdup_printf("%s/playlists/%s/tracks?%s"
				      "&linked_partitioning=true", api_base(),
				      urn, access_filter());
		res = fetch_tracks(url, err);
		g_free(url);
	} else if (kind && strcmp(kind, "user") == 0 && urn) {
		url = g_strdup_printf("%s/users/%s/tracks?%s"
				      "&limit=%d&linked_partitioning=true",
				      api_base(), urn, access_filter(),
				      PAGE_LIMIT);
		res = fetch_tracks(url, err);
		g_free(url);
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
	char *url = g_strdup_printf("%s/me/likes/tracks?%s&limit=200"
				    "&linked_partitioning=true", api_base(),
				    access_filter());
	GPtrArray *res = fetch_tracks(url, err);

	g_free(url);
	return res;
}

gboolean sc_url_is_api(const char *url)
{
	const char *base = api_base();
	size_t n = strlen(base);

	return url && strncmp(url, base, n) == 0 && url[n] == '/';
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
	char *url, *body, *res = NULL;
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
	/* the spec puts the urn in the path as is: colons and all */
	url = g_strdup_printf("%s/tracks/%s/streams", api_base(),
			      m->location);
	body = api_get(url, err);
	g_free(url);
	if (!body)
		return NULL;

	root = parse_json(body, err);
	g_free(body);
	if (!root)
		return NULL;
	o = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
	if (o) {
		GList *members = json_object_get_members(o), *l;

		for (l = members; l; l = l->next)
			g_debug("  streams: %s", (const char *)l->data);
		g_list_free(members);
	}
	for (i = 0; i < G_N_ELEMENTS(stream_keys) && !res; i++) {
		const char *s = str_member(o, stream_keys[i]);

		if (s && *s)
			res = g_strdup(s);
	}
	json_node_unref(root);
	if (!res) {
		g_set_error(err, SC_ERROR, SC_ERROR_NOT_STREAMABLE,
			    "SoundCloud did not return a stream");
		return NULL;
	}
	/*
	 * The Streams URLs sit on the API host and "need to keep using
	 * authentication" (spec).  Follow them with the token so the
	 * player gets the media host address; when the host serves the
	 * audio itself, the decoder sends the token (sc_url_is_api()).
	 */
	if (sc_url_is_api(res)) {
		char *token = sc_session_token(err);
		char *hdr, *final;
		long status = 0;

		if (!token) {
			g_free(res);
			return NULL;
		}
		hdr = g_strdup_printf("OAuth %s", token);
		final = net_final_url(res, hdr, &status, err);
		g_free(hdr);
		g_free(token);
		g_free(res);
		if (!final && status)
			refuse(err, status);
		return final;
	}
	return res;
}
