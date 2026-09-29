// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * net.c - minimal libcurl wrapper
 */
#include <curl/curl.h>

#include "net.h"
#include "pd-build.h"

#define MAX_BODY	(16 * 1024 * 1024)
/*
 * SoundCloud's bot detection flags clients that do not look like the
 * web player, per IP address, which then also blocks the embedded
 * login.  Send what a browser sends.
 */
#define USER_AGENT \
	"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 " \
	"(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"

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

char *net_get(const char *url, const char *auth, long *status,
	      GError **err)
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
	hdr = curl_slist_append(hdr, "Accept-Language: en-US,en;q=0.9");
	hdr = curl_slist_append(hdr, "Origin: https://soundcloud.com");
	hdr = curl_slist_append(hdr, "Referer: https://soundcloud.com/");
	hdr = curl_slist_append(hdr, "Sec-Fetch-Site: same-site");
	hdr = curl_slist_append(hdr, "Sec-Fetch-Mode: cors");
	hdr = curl_slist_append(hdr, "Sec-Fetch-Dest: empty");
	if (auth) {
		line = g_strdup_printf("Authorization: %s", auth);
		hdr = curl_slist_append(hdr, line);
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
	if (code >= 400) {
		g_set_error(err, NET_ERROR, NET_ERROR_HTTP,
			    "Server answered with HTTP %ld", code);
		g_string_free(body, TRUE);
		return NULL;
	}
	return g_string_free(body, FALSE);
}
