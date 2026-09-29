// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * main.c - application entry point
 */
#include <locale.h>
#include <stdio.h>

#include "app.h"
#include "automix.h"
#include "midi.h"
#include "pd-build.h"
#include "pd-vcs.h"
#include "ui/window.h"

static struct app app;

static void load_css(void)
{
	GtkCssProvider *css = gtk_css_provider_new();
	const char *extra = g_getenv("PENGU_DECK_CSS");

	/* the UI is designed for the dark variant only */
	g_object_set(gtk_settings_get_default(),
		     "gtk-application-prefer-dark-theme", TRUE, NULL);
	gtk_css_provider_load_from_resource(css, "/io/github/deimen24/"
					    "PenguDeck/style.css");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(css),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	if (!extra)
		return;
	/* user overrides on top of the built in theme */
	css = gtk_css_provider_new();
	gtk_css_provider_load_from_path(css, extra);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(css),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(css);
}

static GtkWidget *ensure_window(GtkApplication *gtk)
{
	GtkWindow *win = gtk_application_get_active_window(gtk);
	char *warn = NULL;

	if (win)
		return GTK_WIDGET(win);

	load_css();
	app_open_audio(&app, &warn);
	win = GTK_WINDOW(pd_window_new(&app));
	gtk_window_present(win);
	if (warn) {
		app_toast(&app, "%s", warn);
		g_free(warn);
	}
	return GTK_WIDGET(win);
}

static void on_activate(GtkApplication *gtk, gpointer data)
{
	ensure_window(gtk);
}

static void on_open(GtkApplication *gtk, GFile **files, int n,
		    const char *hint, gpointer data)
{
	PdWindow *win = PD_WINDOW(ensure_window(gtk));
	int i;

	for (i = 0; i < n && i < 2; i++) {
		char *path = g_file_get_path(files[i]);

		if (path)
			pd_window_load_file(win, path);
		g_free(path);
	}
}

static void on_startup(GtkApplication *gtk, gpointer data)
{
	const char *quit_accels[] = { "<Control>q", NULL };
	const char *prefs_accels[] = { "<Control>comma", NULL };
	GSimpleAction *quit = g_simple_action_new("quit", NULL);

	g_signal_connect_swapped(quit, "activate",
				 G_CALLBACK(g_application_quit), gtk);
	g_action_map_add_action(G_ACTION_MAP(gtk), G_ACTION(quit));
	gtk_application_set_accels_for_action(gtk, "app.quit", quit_accels);
	gtk_application_set_accels_for_action(gtk, "win.prefs", prefs_accels);
	g_object_unref(quit);

	g_set_prgname("pengu-deck");
	app_init(&app, gtk);
	automix_init(&app);
	midi_init(&app);
}

static void on_shutdown(GtkApplication *gtk, gpointer data)
{
	midi_shutdown();
	automix_shutdown();
	app_shutdown(&app);
}

int main(int argc, char **argv)
{
	GtkApplication *gtk;
	int ret;

	setlocale(LC_ALL, "");
	if (argc > 1 && (g_str_equal(argv[1], "--version") ||
			 g_str_equal(argv[1], "-v"))) {
		printf("Pengu Deck %s (%s)\n", PD_VERSION, PD_COMMIT);
		return 0;
	}
	gtk = gtk_application_new(PD_APP_ID, G_APPLICATION_HANDLES_OPEN);
	g_signal_connect(gtk, "startup", G_CALLBACK(on_startup), NULL);
	g_signal_connect(gtk, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(gtk, "open", G_CALLBACK(on_open), NULL);
	g_signal_connect(gtk, "shutdown", G_CALLBACK(on_shutdown), NULL);
	ret = g_application_run(G_APPLICATION(gtk), argc, argv);
	g_object_unref(gtk);
	return ret;
}
