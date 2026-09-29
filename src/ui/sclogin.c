// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sclogin.c - the SoundCloud login window
 *
 * Opens the OAuth authorization page in the user's browser and waits
 * for SoundCloud to send the browser back to the app (scauth.c).
 */
#include "app.h"
#include "config.h"
#include "scauth.h"
#include "sclogin.h"
#include "soundcloud.h"
#include "window.h"

struct login {
	struct app *app;
	GtkWindow *win;
	GtkWidget *status;
	struct sc_login *flow;
	char *url;
	void (*done)(gpointer data);
	gpointer done_data;
};

bool sclogin_available(void)
{
	return true;
}

static void open_browser(struct login *l)
{
	GtkUriLauncher *u = gtk_uri_launcher_new(l->url);

	gtk_uri_launcher_launch(u, l->win, NULL, NULL, NULL);
	g_object_unref(u);
}

static void on_open_again(GtkButton *b, struct login *l)
{
	open_browser(l);
}

static void on_copy(GtkButton *b, struct login *l)
{
	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(l->win)),
			       l->url);
	gtk_label_set_text(GTK_LABEL(l->status), "Link copied. Open it in "
			   "any browser on this computer.");
}

static void on_flow_done(const char *message, gpointer data)
{
	struct login *l = data;

	l->flow = NULL;
	if (message) {
		gtk_label_set_text(GTK_LABEL(l->status), message);
		return;
	}
	app_toast(l->app, "Logged in to SoundCloud");
	if (l->done)
		l->done(l->done_data);
	if (l->win)
		gtk_window_close(l->win);
}

static void on_destroy(GtkWidget *w, struct login *l)
{
	sc_login_cancel(l->flow);
	l->flow = NULL;
	l->win = NULL;
	g_free(l->url);
	g_free(l);
}

void sclogin_show(struct app *a, void (*done)(gpointer data), gpointer data)
{
	struct login *l = g_new0(struct login, 1);
	GtkWidget *win = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b, *l1, *spinner;
	GError *err = NULL;

	l->app = a;
	l->win = GTK_WINDOW(win);
	l->done = done;
	l->done_data = data;
	l->flow = sc_login_start(on_flow_done, l, &l->url, &err);
	if (!l->flow) {
		app_toast(a, "%s", err->message);
		g_error_free(err);
		g_free(l);
		g_object_ref_sink(win);
		g_object_unref(win);
		return;
	}

	gtk_window_set_title(l->win, "Log in to SoundCloud");
	gtk_window_set_transient_for(l->win, a->win);
	gtk_window_set_default_size(l->win, 460, -1);
	pd_window_close_on_escape(l->win);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);

	l1 = gtk_label_new("Your browser has opened SoundCloud. Sign in "
			   "there and allow Pengu Deck; this window closes "
			   "by itself when SoundCloud sends you back.");
	gtk_label_set_wrap(GTK_LABEL(l1), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l1), 0.0f);
	gtk_box_append(GTK_BOX(box), l1);

	spinner = gtk_spinner_new();
	gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
	l->status = gtk_label_new("Waiting for SoundCloud…");
	gtk_label_set_wrap(GTK_LABEL(l->status), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l->status), 0.0f);
	gtk_widget_add_css_class(l->status, "dim-label");
	gtk_widget_set_hexpand(l->status, TRUE);
	gtk_box_append(GTK_BOX(row), spinner);
	gtk_box_append(GTK_BOX(row), l->status);
	gtk_box_append(GTK_BOX(box), row);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	b = gtk_button_new_with_label("Open the browser again");
	g_signal_connect(b, "clicked", G_CALLBACK(on_open_again), l);
	gtk_box_append(GTK_BOX(row), b);
	b = gtk_button_new_with_label("Copy link");
	g_signal_connect(b, "clicked", G_CALLBACK(on_copy), l);
	gtk_box_append(GTK_BOX(row), b);
	b = gtk_button_new_with_label("Cancel");
	gtk_widget_set_hexpand(b, TRUE);
	gtk_widget_set_halign(b, GTK_ALIGN_END);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_window_close),
				 win);
	gtk_box_append(GTK_BOX(row), b);
	gtk_box_append(GTK_BOX(box), row);

	gtk_window_set_child(l->win, box);
	g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), l);
	gtk_window_present(l->win);
	open_browser(l);
}

void sclogin_logout(struct app *a)
{
	sc_session_logout();
	app_toast(a, "Logged out of SoundCloud");
}
