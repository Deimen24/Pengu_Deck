// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * scauth.c - SoundCloud OAuth 2.1 login with PKCE
 */
#include <string.h>

#include <gio/gio.h>

#include "net.h"
#include "scauth.h"
#include "soundcloud.h"

#define AUTHORIZE_URL	"https://secure.soundcloud.com/authorize"
#define TOKEN_URL	"https://secure.soundcloud.com/oauth/token"

struct sc_login {
	GSocketService *service;
	char *verifier;
	char *state;
	void (*done)(const char *message, gpointer data);
	gpointer done_data;
	gboolean finished;
};

/* base64url without padding, as PKCE wants it */
static char *base64url(const guchar *data, gsize len)
{
	char *s = g_base64_encode(data, len);
	char *p;

	for (p = s; *p; p++) {
		if (*p == '+')
			*p = '-';
		else if (*p == '/')
			*p = '_';
	}
	p = strchr(s, '=');
	if (p)
		*p = '\0';
	return s;
}

char *sc_pkce_challenge(const char *verifier)
{
	GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
	guchar digest[32];
	gsize n = sizeof(digest);
	char *out;

	g_checksum_update(c, (const guchar *)verifier, -1);
	g_checksum_get_digest(c, digest, &n);
	g_checksum_free(c);
	out = base64url(digest, n);
	return out;
}

static char *random_token(gsize bytes)
{
	guchar *buf = g_malloc(bytes);
	char *s;
	gsize i;

	for (i = 0; i < bytes; i++)
		buf[i] = (guchar)g_random_int_range(0, 256);
	s = base64url(buf, bytes);
	g_free(buf);
	return s;
}

/* ---- finishing --------------------------------------------------- */

struct finish {
	struct sc_login *l;
	char *code;
	char *verifier;
	char *message;
};

static gboolean finish_idle(gpointer data)
{
	struct finish *f = data;
	struct sc_login *l = f->l;

	if (!l->finished) {
		l->finished = TRUE;
		if (l->done)
			l->done(f->message, l->done_data);
	}
	g_free(f->message);
	g_free(f->code);
	g_free(f->verifier);
	g_free(f);
	return G_SOURCE_REMOVE;
}

/* Trade the code for tokens off the main loop. */
static gpointer exchange_thread(gpointer data)
{
	struct finish *f = data;
	struct sc_tokens t = { 0 };
	GError *err = NULL;
	char *code = g_uri_escape_string(f->code, NULL, FALSE);
	char *form, *body, *reply = NULL;
	long status = 0;

	form = g_strdup_printf("grant_type=authorization_code&code=%s"
			       "&redirect_uri=%s&code_verifier=%s",
			       code, SC_REDIRECT_URI, f->verifier);
	body = sc_session_post_grant(form, &reply, &status, &err);
	if (body && sc_parse_token_reply(body, &t, &err)) {
		t.user = TRUE;
		sc_session_set_tokens(&t);
		sc_tokens_clear(&t);
	} else if (err) {
		f->message = g_strdup(err->message);
		g_error_free(err);
	} else {
		f->message = g_strdup("SoundCloud sent no tokens");
	}
	g_free(body);
	g_free(reply);
	g_free(form);
	g_free(code);
	g_idle_add(finish_idle, f);
	return NULL;
}

/* ---- loopback redirect ------------------------------------------- */

static const char page_ok[] =
	"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
	"Connection: close\r\n\r\n"
	"<!doctype html><html><head><meta charset=\"utf-8\">"
	"<title>Pengu Deck</title><style>body{font-family:sans-serif;"
	"background:#0d0f13;color:#eef1f5;display:flex;height:100vh;"
	"align-items:center;justify-content:center;margin:0}"
	"div{text-align:center}h1{font-weight:600}</style></head><body>"
	"<div><h1>Logged in to SoundCloud</h1>"
	"<p>You can close this tab and go back to Pengu Deck.</p></div>"
	"</body></html>";

static const char page_fail[] =
	"HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; "
	"charset=utf-8\r\nConnection: close\r\n\r\n"
	"Pengu Deck: the login did not complete. Go back to the app.";

static char *query_param(const char *query, const char *name)
{
	char **parts = g_strsplit(query, "&", -1);
	char *val = NULL;
	int i;

	for (i = 0; parts && parts[i] && !val; i++) {
		char *eq = strchr(parts[i], '=');

		if (eq && strncmp(parts[i], name, eq - parts[i]) == 0 &&
		    strlen(name) == (size_t)(eq - parts[i]))
			val = g_uri_unescape_string(eq + 1, NULL);
	}
	g_strfreev(parts);
	return val;
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn,
			    GObject *src, gpointer data)
{
	struct sc_login *l = data;
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	GDataInputStream *din = g_data_input_stream_new(in);
	char *line = g_data_input_stream_read_line(din, NULL, NULL, NULL);
	char *code = NULL, *state = NULL, *err_param = NULL;
	const char *reply = page_fail;

	/* "GET /callback?code=...&state=... HTTP/1.1" */
	if (line && g_str_has_prefix(line, "GET ")) {
		char *path = line + 4, *sp = strchr(path, ' ');
		char *q;

		if (sp)
			*sp = '\0';
		q = strchr(path, '?');
		if (q) {
			code = query_param(q + 1, "code");
			state = query_param(q + 1, "state");
			err_param = query_param(q + 1, "error");
		}
	}
	if (code && state && g_strcmp0(state, l->state) == 0 && !l->finished)
		reply = page_ok;
	g_output_stream_write_all(out, reply, strlen(reply), NULL, NULL, NULL);
	g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	g_object_unref(din);

	if (reply == page_ok) {
		struct finish *f = g_new0(struct finish, 1);

		f->l = l;
		f->code = code;
		f->verifier = g_strdup(l->verifier);
		code = NULL;
		g_socket_service_stop(l->service);
		g_thread_unref(g_thread_new("pd-sc-exchange", exchange_thread,
					    f));
	} else if (!l->finished && err_param) {
		/* the user declined; anything else is noise, keep waiting */
		struct finish *f = g_new0(struct finish, 1);

		f->l = l;
		f->message = g_strdup_printf("SoundCloud reported: %s",
					     err_param);
		g_socket_service_stop(l->service);
		g_idle_add(finish_idle, f);
	}
	g_free(code);
	g_free(state);
	g_free(err_param);
	g_free(line);
	return TRUE;
}

struct sc_login *sc_login_start(void (*done)(const char *message,
					     gpointer data),
				gpointer data, char **authorize_url,
				GError **err)
{
	struct sc_login *l = g_new0(struct sc_login, 1);
	GInetAddress *lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	GSocketAddress *addr = g_inet_socket_address_new(lo, SC_REDIRECT_PORT);
	char *challenge, *client_id = NULL;

	if (!sc_session_has_app()) {
		g_set_error_literal(err, SC_ERROR, SC_ERROR_NO_APP,
				    "Enter your SoundCloud app's client ID "
				    "and secret in Preferences → SoundCloud "
				    "first.");
		goto fail;
	}
	l->done = done;
	l->done_data = data;
	l->verifier = random_token(48);
	l->state = random_token(16);
	l->service = g_socket_service_new();
	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(l->service), addr,
					   G_SOCKET_TYPE_STREAM,
					   G_SOCKET_PROTOCOL_TCP, NULL, NULL,
					   err)) {
		g_prefix_error(err, "Cannot listen on %s: ", SC_REDIRECT_URI);
		goto fail;
	}
	g_signal_connect(l->service, "incoming", G_CALLBACK(on_incoming), l);

	challenge = sc_pkce_challenge(l->verifier);
	client_id = sc_session_client_id();
	*authorize_url = g_strdup_printf(AUTHORIZE_URL "?client_id=%s"
					 "&redirect_uri=%s&response_type=code"
					 "&code_challenge=%s"
					 "&code_challenge_method=S256"
					 "&state=%s", client_id,
					 SC_REDIRECT_URI, challenge, l->state);
	g_free(challenge);
	g_free(client_id);
	g_object_unref(addr);
	g_object_unref(lo);
	return l;

fail:
	g_object_unref(addr);
	g_object_unref(lo);
	sc_login_cancel(l);
	return NULL;
}

static gboolean free_login(gpointer data)
{
	g_free(data);
	return G_SOURCE_REMOVE;
}

void sc_login_cancel(struct sc_login *l)
{
	if (!l)
		return;
	l->finished = TRUE;
	l->done = NULL;
	if (l->service) {
		g_socket_service_stop(l->service);
		g_socket_listener_close(G_SOCKET_LISTENER(l->service));
		g_object_unref(l->service);
	}
	g_free(l->verifier);
	g_free(l->state);
	l->verifier = NULL;
	l->state = NULL;
	/* a finish already queued on the main loop may still name it */
	g_timeout_add_seconds(90, free_login, l);
}
