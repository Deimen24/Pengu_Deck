// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * net.c - minimal libcurl wrapper
 */
#define G_LOG_DOMAIN "pengu-deck"

#include <curl/curl.h>

#include "net.h"
#include "pd-build.h"

#define MAX_BODY	(16 * 1024 * 1024)
#define USER_AGENT	"PenguDeck/" PD_VERSION " (Linux; +https://github.com/" \
			"deimen24/Pengu_Deck)"

G_DEFINE_QUARK(pd-net-error-quark, net_error)

void net_init(void)
{
	static gsize once;

	if (g_once_init_enter(&once)) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		g_once_init_leave(&once, 1);
	}
}

static size_t write_cb(char *data, size_t size, size_t nmemb, void *user)
{
	GString *body = user;
	size_t n = size * nmemb;

	if (body->len + n > MAX_BODY)
		return 0;
	g_string_append_len(body, data, (gssize)n);
	return n;
}

/*
 * Run one request.  @form NULL means GET, else a form encoded POST.
 * The body is always returned to the caller through @out; a NULL result
 * with @err set marks a failed request.
 */
static char *request(const char *url, const char *form, const char *auth,
		     long *status, char **reply, GError **err)
{
	struct curl_slist *hdr = NULL;
	GString *body = g_string_new(NULL);
	char *line = NULL;
	long code = 0;
	CURLcode rc;
	CURL *c;

	net_init();
	c = curl_easy_init();
	if (!c) {
		g_set_error(err, NET_ERROR, NET_ERROR_TRANSPORT,
			    "Could not create an HTTP handle");
		g_string_free(body, TRUE);
		return NULL;
	}

	hdr = curl_slist_append(hdr, "Accept: application/json, */*");
	if (auth) {
		line = g_strdup_printf("Authorization: %s", auth);
		hdr = curl_slist_append(hdr, line);
	}
	if (form) {
		hdr = curl_slist_append(hdr, "Content-Type: "
					"application/x-www-form-urlencoded");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, form);
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, body);

	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	/* G_MESSAGES_DEBUG=pengu-deck shows every request and its answer */
	g_debug("%s %s -> %ld%s%s", form ? "POST" : "GET", url, code,
		rc != CURLE_OK ? " " : "",
		rc != CURLE_OK ? curl_easy_strerror(rc) : "");
	if (code >= 400 && body->len > 0 && body->len < 400)
		g_debug("  reply: %s", body->str);
	curl_slist_free_all(hdr);
	g_free(line);

	if (status)
		*status = code;
	if (rc != CURLE_OK) {
		g_set_error(err, NET_ERROR, NET_ERROR_TRANSPORT, "%s",
			    curl_easy_strerror(rc));
		g_string_free(body, TRUE);
		return NULL;
	}
	if (code < 200 || code >= 300) {
		g_set_error(err, NET_ERROR, NET_ERROR_HTTP, "HTTP %ld", code);
		if (reply)
			*reply = g_string_free(body, FALSE);
		else
			g_string_free(body, TRUE);
		return NULL;
	}
	return g_string_free(body, FALSE);
}

char *net_get(const char *url, const char *auth, long *status,
	      GError **err)
{
	return request(url, NULL, auth, status, NULL, err);
}

char *net_final_url(const char *url, const char *auth, long *status,
		    GError **err)
{
	struct curl_slist *hdr = NULL;
	GString *body = g_string_new(NULL);
	char *line = NULL, *final = NULL, *eff = NULL;
	long code = 0;
	CURLcode rc;
	CURL *c;

	net_init();
	c = curl_easy_init();
	if (!c) {
		g_set_error(err, NET_ERROR, NET_ERROR_TRANSPORT,
			    "Could not create an HTTP handle");
		g_string_free(body, TRUE);
		return NULL;
	}
	if (auth) {
		line = g_strdup_printf("Authorization: %s", auth);
		hdr = curl_slist_append(hdr, line);
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	/* the auth header is for the API only, not the media host */
	curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);
	curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
	g_debug("RESOLVE %s -> %ld %s", url, code, eff ? eff : "");
	if (status)
		*status = code;
	if (rc != CURLE_OK)
		g_set_error(err, NET_ERROR, NET_ERROR_TRANSPORT, "%s",
			    curl_easy_strerror(rc));
	else if (code >= 400)
		g_set_error(err, NET_ERROR, NET_ERROR_HTTP, "HTTP %ld", code);
	else
		final = g_strdup(eff ? eff : url);
	curl_easy_cleanup(c);
	curl_slist_free_all(hdr);
	g_free(line);
	g_string_free(body, TRUE);
	return final;
}

char *net_post_form(const char *url, const char *form, const char *auth,
		    long *status, char **reply, GError **err)
{
	return request(url, form, auth, status, reply, err);
}
