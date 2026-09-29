// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sclogin.c - log in to SoundCloud
 *
 * SoundCloud's bot protection refuses an embedded WebKit view on many
 * networks while the user's everyday browser passes, so the login
 * leads with the system browser: sign in there, paste the session
 * here.  The embedded view stays as a second option where it works.
 */
#include "app.h"
#include "config.h"
#include "net.h"
#include "pd-build.h"
#include "sclogin.h"
#include "soundcloud.h"
#include "window.h"

#ifdef HAVE_WEBKIT
#include <webkit/webkit.h>
#endif

#define SIGNIN_URL	"https://soundcloud.com/signin"
#define COOKIE_URL	"https://soundcloud.com/"
#define POLL_MS		1500

bool sclogin_available(void)
{
	return true;
}

struct login {
	struct app *app;
	GtkWindow *win;
	GtkWidget *status;
	GtkWidget *paste;
	GtkWidget *done_btn;
	GtkWidget *stack;
	char *token;
	char *datadome;
	gboolean found;
	void (*done)(gpointer data);
	gpointer done_data;
#ifdef HAVE_WEBKIT
	WebKitWebView *view;
	guint poll;
#endif
};

/* ---- finishing a login ------------------------------------------ */

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

/* Store the session and fetch the client id the web player uses now. */
static void finish(struct login *l, const char *token, const char *datadome)
{
	struct detect *d = g_new0(struct detect, 1);
	struct config *c = &l->app->cfg;

	if (l->found)
		return;
	l->found = TRUE;
	g_free(c->sc_token);
	c->sc_token = g_strdup(token);
	if (datadome) {
		g_free(c->sc_cookies);
		c->sc_cookies = g_strdup_printf("datadome=%s", datadome);
		net_set_soundcloud_cookies(c->sc_cookies);
	}
	gtk_label_set_text(GTK_LABEL(l->status),
			   "Logged in, fetching the client id…");
	d->l = l;
	g_thread_unref(g_thread_new("pd-sc-detect", detect_thread, d));
}

/* ---- system browser + paste -------------------------------------- */

#ifdef HAVE_WEBKIT
static void show_embedded_page(GtkButton *b, struct login *l);
#endif

static void on_open_browser(GtkButton *b, struct login *l)
{
	GtkUriLauncher *u = gtk_uri_launcher_new(SIGNIN_URL);

	gtk_uri_launcher_launch(u, l->win, NULL, NULL, NULL);
	g_object_unref(u);
}

static void on_paste_changed(GtkTextBuffer *buf, struct login *l)
{
	GtkTextIter a, b;
	char *text;

	gtk_text_buffer_get_bounds(buf, &a, &b);
	text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
	g_clear_pointer(&l->token, g_free);
	g_clear_pointer(&l->datadome, g_free);
	sc_parse_session(text, &l->token, &l->datadome);
	g_free(text);

	gtk_widget_set_sensitive(l->done_btn, l->token != NULL);
	if (l->token && l->datadome)
		gtk_label_set_text(GTK_LABEL(l->status),
				   "Found the login token and the bot "
				   "protection cookie. Press Done.");
	else if (l->token)
		gtk_label_set_text(GTK_LABEL(l->status),
				   "Found the login token. Press Done. (No "
				   "datadome cookie in the paste: fine unless "
				   "SoundCloud shows \"Verification "
				   "Required\".)");
	else
		gtk_label_set_text(GTK_LABEL(l->status),
				   "No token found yet. Paste the cookies or "
				   "a \"Copy as cURL\" from the browser.");
}

static void on_done(GtkButton *b, struct login *l)
{
	if (l->token)
		finish(l, l->token, l->datadome);
}

static GtkWidget *step(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	return l;
}

static GtkWidget *build_browser_page(struct login *l)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	GtkWidget *b, *scroll, *row;
	GtkTextBuffer *buf;

	gtk_widget_set_margin_top(box, 12);
	gtk_widget_set_margin_bottom(box, 12);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);

	gtk_box_append(GTK_BOX(box), step("1.  Sign in to SoundCloud in your "
					  "browser."));
	b = gtk_button_new_with_label("Open soundcloud.com in your browser");
	gtk_widget_add_css_class(b, "suggested-action");
	g_signal_connect(b, "clicked", G_CALLBACK(on_open_browser), l);
	gtk_box_append(GTK_BOX(box), b);

	gtk_box_append(GTK_BOX(box), step("2.  Copy your session from the "
		"browser and paste it below. Press F12, open the Network "
		"tab, click any request to api-v2.soundcloud.com, then "
		"right click it → Copy → Copy as cURL. The cookies "
		"oauth_token and datadome from the Storage / Application "
		"tab work as well, and so does the bare token."));

	l->paste = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(l->paste), GTK_WRAP_CHAR);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(l->paste), TRUE);
	gtk_widget_add_css_class(l->paste, "paste-area");
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(l->paste));
	g_signal_connect(buf, "changed", G_CALLBACK(on_paste_changed), l);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), l->paste);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, -1, 140);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	l->done_btn = gtk_button_new_with_label("Done");
	gtk_widget_add_css_class(l->done_btn, "suggested-action");
	gtk_widget_set_sensitive(l->done_btn, FALSE);
	gtk_widget_set_hexpand(l->done_btn, TRUE);
	g_signal_connect(l->done_btn, "clicked", G_CALLBACK(on_done), l);
	gtk_box_append(GTK_BOX(row), l->done_btn);
#ifdef HAVE_WEBKIT
	b = gtk_button_new_with_label("Try the embedded browser");
	g_signal_connect(b, "clicked", G_CALLBACK(show_embedded_page), l);
	gtk_box_append(GTK_BOX(row), b);
#endif
	gtk_box_append(GTK_BOX(box), row);
	return box;
}

/* ---- embedded browser (where the bot protection lets it through) --- */

#ifdef HAVE_WEBKIT
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
 * Present the view as Safari, the mainstream browser built on the same
 * engine, so what the page can probe matches what the identity claims.
 */
#define USER_AGENT \
	"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 " \
	"(KHTML, like Gecko) Version/17.6 Safari/605.1.15"

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

static void cookies_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct login *l = data;
	GList *cookies, *c;
	const char *token = NULL, *dd = NULL;

	cookies = webkit_cookie_manager_get_cookies_finish(
			WEBKIT_COOKIE_MANAGER(src), res, NULL);
	for (c = cookies; c; c = c->next) {
		SoupCookie *ck = c->data;

		if (g_str_equal(soup_cookie_get_name(ck), "oauth_token"))
			token = soup_cookie_get_value(ck);
		else if (g_str_equal(soup_cookie_get_name(ck), "datadome"))
			dd = soup_cookie_get_value(ck);
	}
	if (token && *token && l->win)
		finish(l, token, dd);
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

static void on_reload(GtkButton *b, struct login *l)
{
	webkit_web_view_load_uri(l->view, SIGNIN_URL);
}

static void show_browser_page(GtkButton *b, struct login *l)
{
	gtk_stack_set_visible_child_name(GTK_STACK(l->stack), "browser");
}

static void show_embedded_page(GtkButton *b, struct login *l)
{
	gtk_stack_set_visible_child_name(GTK_STACK(l->stack), "embedded");
	if (!webkit_web_view_get_uri(l->view))
		webkit_web_view_load_uri(l->view, SIGNIN_URL);
}

static GtkWidget *build_embedded_page(struct login *l)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *view, *row, *b;

	view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session(),
			    "settings", view_settings(), NULL);
	l->view = WEBKIT_WEB_VIEW(view);
	gtk_widget_set_vexpand(view, TRUE);
	gtk_box_append(GTK_BOX(box), view);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(row, 6);
	gtk_widget_set_margin_bottom(row, 6);
	gtk_widget_set_margin_start(row, 6);
	gtk_widget_set_margin_end(row, 6);
	b = gtk_button_new_with_label("Reload");
	g_signal_connect(b, "clicked", G_CALLBACK(on_reload), l);
	gtk_box_append(GTK_BOX(row), b);
	b = gtk_button_new_with_label("Back to the browser login");
	gtk_widget_set_hexpand(b, TRUE);
	g_signal_connect(b, "clicked", G_CALLBACK(show_browser_page), l);
	gtk_box_append(GTK_BOX(row), b);
	gtk_box_append(GTK_BOX(box), row);
	return box;
}
#endif /* HAVE_WEBKIT */

/* ---- window ------------------------------------------------------ */

static gboolean free_later(gpointer data)
{
	struct login *l = data;

	g_free(l->token);
	g_free(l->datadome);
	g_free(l);
	return G_SOURCE_REMOVE;
}

static void on_destroy(GtkWidget *w, struct login *l)
{
#ifdef HAVE_WEBKIT
	if (l->poll)
		g_source_remove(l->poll);
	l->poll = 0;
#endif
	l->win = NULL;
	/* Outlives any callback still in flight. */
	g_timeout_add_seconds(30, free_later, l);
}

void sclogin_show(struct app *a, void (*done)(gpointer data), gpointer data)
{
	struct login *l = g_new0(struct login, 1);
	GtkWidget *win = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *page;

	l->app = a;
	l->win = GTK_WINDOW(win);
	l->done = done;
	l->done_data = data;
	gtk_window_set_title(l->win, "Log in to SoundCloud");
	gtk_window_set_transient_for(l->win, a->win);
	gtk_window_set_default_size(l->win, 560, 520);
	pd_window_close_on_escape(l->win);

	l->status = gtk_label_new("Sign in with your SoundCloud account. "
				  "The session stays on this computer.");
	gtk_label_set_wrap(GTK_LABEL(l->status), TRUE);
	gtk_widget_add_css_class(l->status, "dim-label");
	gtk_widget_set_margin_top(l->status, 8);
	gtk_widget_set_margin_start(l->status, 12);
	gtk_widget_set_margin_end(l->status, 12);
	gtk_box_append(GTK_BOX(box), l->status);

	l->stack = gtk_stack_new();
	gtk_widget_set_vexpand(l->stack, TRUE);
	page = build_browser_page(l);
	gtk_stack_add_named(GTK_STACK(l->stack), page, "browser");
#ifdef HAVE_WEBKIT
	gtk_stack_add_named(GTK_STACK(l->stack), build_embedded_page(l),
			    "embedded");
	l->poll = g_timeout_add(POLL_MS, poll, l);
#endif
	gtk_stack_set_visible_child_name(GTK_STACK(l->stack), "browser");
	gtk_box_append(GTK_BOX(box), l->stack);
	gtk_window_set_child(l->win, box);

	g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), l);
	gtk_window_present(l->win);
	/* straight to the browser: that is the path that works */
	on_open_browser(NULL, l);
}

void sclogin_logout(struct app *a)
{
#ifdef HAVE_WEBKIT
	WebKitWebsiteDataManager *mgr =
		webkit_network_session_get_website_data_manager(session());

	webkit_website_data_manager_clear(mgr, WEBKIT_WEBSITE_DATA_COOKIES |
					  WEBKIT_WEBSITE_DATA_LOCAL_STORAGE,
					  0, NULL, NULL, NULL);
#endif
	g_clear_pointer(&a->cfg.sc_token, g_free);
	g_clear_pointer(&a->cfg.sc_cookies, g_free);
	net_set_soundcloud_cookies(NULL);
	config_save(&a->cfg);
	app_toast(a, "Logged out of SoundCloud");
}
