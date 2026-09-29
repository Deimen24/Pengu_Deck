/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * app.h - state shared by all views
 */
#ifndef PD_APP_H
#define PD_APP_H

#include <stdbool.h>

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
	GListStore *queue;		/* PdMediaItem, automix order */
	GHashTable *played;		/* keys of tracks loaded this session */
	gboolean loading[ENGINE_DECKS];	/* stream url being resolved */
	gboolean preview_loading;
	char *preview_key;		/* item on the preview deck */

	/* installed by the window */
	void (*toast)(gpointer data, const char *msg);
	gpointer toast_data;
	void (*sync_deck)(gpointer data, int idx);
	void (*load_selected)(gpointer data, int idx);
	gpointer ui_data;
};

void app_init(struct app *a, GtkApplication *gtk);
void app_shutdown(struct app *a);

/* Open (or reopen) the audio device with the current settings. */
gboolean app_open_audio(struct app *a, char **warn);

/* Load @item into deck @idx, resolving SoundCloud streams as needed. */
void app_load_item(struct app *a, int idx, PdMediaItem *item);
void app_unload(struct app *a, int idx);

/* Headphone pre-listen on the preview deck; toggles when @m is playing. */
void app_preview(struct app *a, PdMediaItem *m);
void app_preview_stop(struct app *a);
bool app_previewing(struct app *a, PdMediaItem *m);
void app_load_path(struct app *a, int idx, const char *path);

/* Tell every list showing @m to rebind its row. */
void app_item_changed(struct app *a, PdMediaItem *m);

/* Played marks: set on load, kept until reset or the app closes. */
bool app_item_played(struct app *a, PdMediaItem *m);
void app_reset_played(struct app *a);

/* Persist cue points and analysis of the track in deck @idx. */
void app_save_cues(struct app *a, int idx);

/* Deck to sync @idx to: a playing deck, else its crossfader neighbour. */
int app_sync_master(struct app *a, int idx);

/* Number of decks shown; changing it never touches the audio. */
void app_set_deck_count(struct app *a, int n);

/* Per deck accent colour as a CSS hex string and css class. */
const char *app_deck_color(int idx);
const char *app_deck_class(int idx);
char app_deck_letter(int idx);

/* Show a transient message at the bottom of the window. */
void app_toast(struct app *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* Push the app credentials from the config into the SoundCloud client. */
void app_sc_configure(struct app *a);

#endif /* PD_APP_H */
