/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * app.h - state shared by all views
 */
#ifndef PD_APP_H
#define PD_APP_H

#include <gtk/gtk.h>

#include "config.h"
#include "engine.h"
#include "media.h"
#include "soundcloud.h"

struct app {
	GtkApplication *gtk;
	GtkWindow *win;
	struct engine engine;
	struct config cfg;

	/* main thread only */
	GListStore *library;		/* PdMediaItem */
	GListStore *sc_results;		/* PdMediaItem */
	gboolean loading[2];		/* stream url being resolved */

	/* installed by the window */
	void (*toast)(gpointer data, const char *msg);
	gpointer toast_data;
};

void app_init(struct app *a, GtkApplication *gtk);
void app_shutdown(struct app *a);

/* Open (or reopen) the audio device with the current settings. */
gboolean app_open_audio(struct app *a, char **warn);

/* Load @item into deck @idx, resolving SoundCloud streams as needed. */
void app_load_item(struct app *a, int idx, PdMediaItem *item);
void app_load_path(struct app *a, int idx, const char *path);

/* Persist cue points and analysis of the track in deck @idx. */
void app_save_cues(struct app *a, int idx);

/* Show a transient message at the bottom of the window. */
void app_toast(struct app *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* Copy of the SoundCloud credentials, free with sc_auth_free(). */
struct sc_auth *app_sc_auth(struct app *a);
void sc_auth_free(struct sc_auth *auth);

#endif /* PD_APP_H */
