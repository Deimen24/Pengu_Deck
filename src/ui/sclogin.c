// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sclogin.c - log in to SoundCloud inside the app
 */
#include "pd-build.h"
#include "sclogin.h"
#include "window.h"
#include "soundcloud.h"

#ifdef HAVE_WEBKIT
#include <webkit/webkit.h>
#endif

#define SIGNIN_URL	"https://soundcloud.com/signin"
#define COOKIE_URL	"https://soundcloud.com/"
#define POLL_MS		1500

bool sclogin_available(void)
{
#ifdef HAVE_WEBKIT
	return true;
#else
	return false;
#endif
}

#ifdef HAVE_WEBKIT
struct login {
	struct app *app;
	GtkWindow *win;
	WebKitWebView *view;
	GtkWidget *status;
	guint poll;
	gboolean found;
	void (*done)(gpointer data);
	gpointer done_data;
};

static WebKitNetworkSession *session(void)
{
	static WebKitNetworkSession *s;
	char *dir;

	if (s)
		return s;
	/* Persistent so a login survives restarts. */
	dir = g_build_filename(g_get_user_data_dir(), "pengu-deck",
			       "webkit", NULL);
	s = webkit_network_session_new(dir, dir);
	g_free(dir);
	return s;
}

/*
 * SoundCloud's bot check refuses WebKit's stock identity; present the
 * view as Safari, the mainstream browser built on the same engine, so
 * what the page can probe matches what the identity claims.
 */
#define USER_AGENT \
	"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) " \
	"AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15"

static WebKitSettings *view_settings(void)
{
	static WebKitSettings *s;

	if (s)
		return s;
	s = webkit_settings_new();
	webkit_settings_set_user_agent(s, USER_AGENT);
	webkit_settings_set_enable_javascript(s, TRUE);
	webkit_settings_set_javascript_can_open_windows_automatically(s,
								     TRUE);
	webkit_settings_set_enable_developer_extras(s, FALSE);
	return s;
}

static void on_reload(GtkButton *b, struct login *l)
{
	webkit_web_view_load_uri(l->view, SIGNIN_URL);
}

static void on_open_browser(GtkButton *b, struct login *l)
{
	GtkUriLauncher *u = gtk_uri_launcher_new(SIGNIN_URL);

	gtk_uri_launcher_launch(u, l->win, NULL, NULL, NULL);
	g_object_unref(u);
	gtk_label_set_text(GTK_LABEL(l->status),
			   "Signed in in your browser? Copy the oauth_token "
			   "and datadome cookies from soundcloud.com into "
			   "Preferences → SoundCloud.");
}

/* When the embedded page misbehaves: retry, or use the system browser. */
static GtkWidget *fallback_row(struct login *l)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b;

	gtk_widget_set_margin_top(row, 6);
	gtk_widget_set_margin_bottom(row, 6);
	gtk_widget_set_margin_start(row, 6);
	gtk_widget_set_margin_end(row, 6);
	b = gtk_button_new_with_label("Reload");
	g_signal_connect(b, "clicked", G_CALLBACK(on_reload), l);
	gtk_box_append(GTK_BOX(row), b);
	b = gtk_button_new_with_label("Open in browser instead");
	gtk_widget_set_hexpand(b, TRUE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_open_browser), l);
	gtk_box_append(GTK_BOX(row), b);
	return row;
}

struct detect {
	struct login *l;
	char *id;
};

static gboolean detect_done(gpointer data)
{
	struct detect *d = data;
	struct login *l = d->l;

	if (d->id) {
		g_free(l->app->cfg.sc_client_id);
		l->app->cfg.sc_client_id = d->id;
		d->id = NULL;
	}
	config_save(&l->app->cfg);
	app_toast(l->app, "Logged in to SoundCloud");
	if (l->done)
		l->done(l->done_data);
	if (l->win)
		gtk_window_close(l->win);
	g_free(d->id);
	g_free(d);
	return G_SOURCE_REMOVE;
}

static gpointer detect_thread(gpointer data)
{
	struct detect *d = data;

	d->id = sc_detect_client_id(NULL);
	g_idle_add(detect_done, d);
	return NULL;
}

static void cookies_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct login *l = data;
	GList *cookies, *c;
	const char *token = NULL;

	cookies = webkit_cookie_manager_get_cookies_finish(
			WEBKIT_COOKIE_MANAGER(src), res, NULL);
	for (c = cookies; c; c = c->next) {
		SoupCookie *ck = c->data;

		if (g_str_equal(soup_cookie_get_name(ck), "oauth_token"))
			token = soup_cookie_get_value(ck);
	}
	if (token && *token && !l->found && l->win) {
		struct detect *d = g_new0(struct detect, 1);

		l->found = TRUE;
		g_free(l->app->cfg.sc_token);
		l->app->cfg.sc_token = g_strdup(token);
		gtk_label_set_text(GTK_LABEL(l->status),
				   "Logged in, fetching the client id…");
		d->l = l;
		g_thread_unref(g_thread_new("pd-sc-detect", detect_thread, d));
	}
	g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
}

static gboolean poll(gpointer data)
{
	struct login *l = data;
	WebKitCookieManager *mgr = webkit_network_session_get_cookie_manager(
					session());

	webkit_cookie_manager_get_cookies(mgr, COOKIE_URL, NULL, cookies_cb,
					  l);
	return G_SOURCE_CONTINUE;
}

static gboolean free_later(gpointer data)
{
	g_free(data);
	return G_SOURCE_REMOVE;
}

static void on_destroy(GtkWidget *w, struct login *l)
{
	if (l->poll)
		g_source_remove(l->poll);
	l->poll = 0;
	l->win = NULL;
	/* Outlives any cookie callback still in flight. */
	g_timeout_add_seconds(30, free_later, l);
}

void sclogin_show(struct app *a, void (*done)(gpointer data), gpointer data)
{
	struct login *l = g_new0(struct login, 1);
	GtkWidget *win = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *view;

	l->app = a;
	l->win = GTK_WINDOW(win);
	l->done = done;
	l->done_data = data;
	gtk_window_set_title(l->win, "Log in to SoundCloud");
	gtk_window_set_transient_for(l->win, a->win);
	gtk_window_set_default_size(l->win, 520, 720);
	pd_window_close_on_escape(l->win);

	l->status = gtk_label_new("Sign in with your SoundCloud account. "
				  "The session stays on this computer.");
	gtk_widget_add_css_class(l->status, "dim-label");
	gtk_widget_set_margin_top(l->status, 6);
	gtk_widget_set_margin_bottom(l->status, 6);
	gtk_box_append(GTK_BOX(box), l->status);

	view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session(),
			    "settings", view_settings(), NULL);
	l->view = WEBKIT_WEB_VIEW(view);
	gtk_widget_set_vexpand(view, TRUE);
	webkit_web_view_load_uri(l->view, SIGNIN_URL);
	gtk_box_append(GTK_BOX(box), view);
	gtk_box_append(GTK_BOX(box), fallback_row(l));
	gtk_window_set_child(l->win, box);

	l->poll = g_timeout_add(POLL_MS, poll, l);
	g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), l);
	gtk_window_present(l->win);
}

void sclogin_logout(struct app *a)
{
	WebKitWebsiteDataManager *mgr =
		webkit_network_session_get_website_data_manager(session());

	webkit_website_data_manager_clear(mgr, WEBKIT_WEBSITE_DATA_COOKIES |
					  WEBKIT_WEBSITE_DATA_LOCAL_STORAGE,
					  0, NULL, NULL, NULL);
	g_clear_pointer(&a->cfg.sc_token, g_free);
	config_save(&a->cfg);
	app_toast(a, "Logged out of SoundCloud");
}
#else
void sclogin_show(struct app *a, void (*done)(gpointer data), gpointer data)
{
	app_toast(a, "Built without WebKitGTK: paste an OAuth token in "
		  "Preferences → SoundCloud instead");
}

void sclogin_logout(struct app *a)
{
	g_clear_pointer(&a->cfg.sc_token, g_free);
	config_save(&a->cfg);
}
#endif
