// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * deckview.c - one deck panel
 */
#include <math.h>
#include <string.h>

#include "analyze.h"
#include "cuestore.h"
#include "deckview.h"
#include "platter.h"
#include "waveform.h"

#define PLATTER_SIZE	92

#define BEND_AMOUNT	0.04f

struct _PdDeckView {
	GtkBox parent;
	struct app *app;
	struct deck *deck;
	int idx;

	GtkWidget *title;
	GtkWidget *artist;
	GtkWidget *bpm;
	GtkWidget *time;
	GtkWidget *status;
	GtkWidget *wave;
	GtkWidget *overview;
	GtkWidget *play;
	GtkWidget *cue;
	GtkWidget *keylock;
	GtkWidget *sync;
	GtkWidget *quant;
	GtkWidget *rev;
	GtkWidget *slip;
	GtkWidget *key;
	GtkWidget *loop;
	gint64 taps[8];
	int ntaps;
	GtkWidget *hotcue[DECK_HOTCUES];
	GtkWidget *pitch;
	GtkWidget *pitch_label;
	GtkWidget *pitch_box;
	GtkWidget *platter;
	GtkWidget *transport_row;
	GtkWidget *modes_row;
	GtkWidget *hotcue_row;
	GtkWidget *loop_row;
	int compact;

	guint tick;
	struct track *saved_for;	/* track whose analysis was saved */
	gboolean updating;
	gboolean shown_playing;
};

G_DEFINE_FINAL_TYPE(PdDeckView, pd_deck_view, GTK_TYPE_BOX)

/* ---- helpers ----------------------------------------------------- */

static GtkWidget *button(const char *label, const char *css)
{
	GtkWidget *b = gtk_button_new_with_label(label);

	gtk_widget_set_focusable(b, FALSE);
	if (css)
		gtk_widget_add_css_class(b, css);
	return b;
}

static GtkWidget *toggle(const char *label, const char *css)
{
	GtkWidget *b = gtk_toggle_button_new_with_label(label);

	gtk_widget_set_focusable(b, FALSE);
	if (css)
		gtk_widget_add_css_class(b, css);
	return b;
}

static void set_time_label(PdDeckView *v, struct track *t)
{
	double pos = deck_position(v->deck) / t->rate;
	double len = (double)track_length(t) / t->rate;
	double rem = len - pos;
	char *s;

	if (rem < 0.0)
		rem = 0.0;
	s = g_strdup_printf("%d:%02d.%d  <span alpha='55%%'>-%d:%02d</span>",
			    (int)pos / 60, (int)pos % 60,
			    (int)(fmod(pos, 1.0) * 10),
			    (int)rem / 60, (int)rem % 60);
	gtk_label_set_markup(GTK_LABEL(v->time), s);
	g_free(s);
}

static void set_bpm_label(PdDeckView *v, struct track *t)
{
	double bpm = deck_bpm(v->deck);
	double range = v->app->cfg.pitch_range;
	double pitch = atomic_load(&v->deck->pitch) * 100.0;
	char *s;

	if (bpm > 0.0)
		s = g_strdup_printf("%.2f BPM", bpm);
	else if (t && !atomic_load(&t->analysed))
		s = g_strdup("analysing…");
	else
		s = g_strdup("– BPM");
	gtk_label_set_text(GTK_LABEL(v->bpm), s);
	g_free(s);

	s = g_strdup_printf("%+.2f%%", pitch);
	gtk_label_set_text(GTK_LABEL(v->pitch_label), s);
	g_free(s);
	(void)range;
}

static void set_status(PdDeckView *v, struct track *t)
{
	const char *msg = "";
	char *err = NULL;

	if (t) {
		switch (atomic_load(&t->state)) {
		case TRACK_LOADING:
			msg = track_frames(t) ? "buffering" : "loading";
			break;
		case TRACK_DECODED:
			msg = "analysing";
			break;
		case TRACK_FAILED:
			err = track_error(t);
			msg = err;
			break;
		default:
			break;
		}
	}
	gtk_label_set_text(GTK_LABEL(v->status), msg ? msg : "");
	if (err)
		gtk_widget_add_css_class(v->status, "error");
	else
		gtk_widget_remove_css_class(v->status, "error");
	g_free(err);
}

static gboolean do_sync(PdDeckView *v, gboolean phase, gboolean loud);
static void sync_toggle(PdDeckView *v, GtkWidget *b, atomic_bool *flag);
static void set_key_label(PdDeckView *v, struct track *t);

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	PdDeckView *v = PD_DECK_VIEW(w);
	struct track *t = deck_track(v->deck);
	gboolean playing = atomic_load(&v->deck->playing);
	int i;

	v->updating = TRUE;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->play)) !=
	    playing)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->play),
					     playing);
	if (playing != v->shown_playing) {
		v->shown_playing = playing;
		gtk_button_set_label(GTK_BUTTON(v->play), playing ? "⏸" : "▶");
	}
	{
		double want = atomic_load(&v->deck->pitch) * 100.0;

		if (fabs(want - gtk_range_get_value(GTK_RANGE(v->pitch))) >
		    0.005)
			gtk_range_set_value(GTK_RANGE(v->pitch), want);
	}
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->keylock)) !=
	    atomic_load(&v->deck->keylock))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->keylock),
					     atomic_load(&v->deck->keylock));
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->loop)) !=
	    atomic_load(&v->deck->loop_on))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->loop),
					     atomic_load(&v->deck->loop_on));
	sync_toggle(v, v->sync, &v->deck->sync_lock);
	sync_toggle(v, v->quant, &v->deck->quantize);
	sync_toggle(v, v->rev, &v->deck->reverse);
	sync_toggle(v, v->slip, &v->deck->slip);
	if (atomic_load(&v->deck->sync_lock) && !do_sync(v, FALSE, FALSE))
		atomic_store(&v->deck->sync_lock, false);
	set_key_label(v, t);
	for (i = 0; i < DECK_HOTCUES; i++) {
		if (v->deck->hotcue[i] >= 0.0)
			gtk_widget_add_css_class(v->hotcue[i], "set");
		else
			gtk_widget_remove_css_class(v->hotcue[i], "set");
	}
	v->updating = FALSE;

	if (t) {
		char *title = track_title(t), *artist = track_artist(t);

		gtk_label_set_text(GTK_LABEL(v->title),
				   title ? title : "Untitled");
		gtk_label_set_text(GTK_LABEL(v->artist),
				   artist ? artist : "");
		g_free(title);
		g_free(artist);
		set_time_label(v, t);

		/* Persist analysis results once they arrive. */
		if (atomic_load(&t->analysed) && v->saved_for != t) {
			v->saved_for = t;
			app_save_cues(v->app, v->idx);
		}
	} else {
		gtk_label_set_text(GTK_LABEL(v->title), "No track loaded");
		gtk_label_set_text(GTK_LABEL(v->artist),
				   "Drop a file here or double click the "
				   "library");
		gtk_label_set_text(GTK_LABEL(v->time), "0:00.0");
	}
	set_bpm_label(v, t);
	set_status(v, t);
	return G_SOURCE_CONTINUE;
}

/* ---- transport callbacks ----------------------------------------- */

static void on_play(GtkToggleButton *b, PdDeckView *v)
{
	if (v->updating)
		return;
	deck_play(v->deck, gtk_toggle_button_get_active(b));
}

static void on_cue_press(GtkGestureClick *g, int n, double x, double y,
			 PdDeckView *v)
{
	deck_cue_press(v->deck);
}

static void on_cue_release(GtkGestureClick *g, int n, double x, double y,
			   PdDeckView *v)
{
	deck_cue_release(v->deck);
}

/* One shot sync; returns false when it is not possible. */
static gboolean do_sync(PdDeckView *v, gboolean phase, gboolean loud)
{
	struct deck *master = &v->app->engine.deck[app_sync_master(v->app,
								   v->idx)];
	double p = deck_sync_pitch(v->deck, master);
	double range = v->app->cfg.pitch_range / 100.0;

	if (isnan(p)) {
		if (loud)
			app_toast(v->app, "Sync needs a tempo on this and "
				  "another deck");
		return FALSE;
	}
	if (fabs(p) > range) {
		if (loud)
			app_toast(v->app, "Sync needs %+.1f%%, outside the "
				  "±%d%% pitch range", p * 100.0,
				  v->app->cfg.pitch_range);
		return FALSE;
	}
	if (fabs(p - atomic_load(&v->deck->pitch)) > 1e-4)
		gtk_range_set_value(GTK_RANGE(v->pitch), p * 100.0);
	if (phase)
		deck_sync_phase(v->deck, master);
	return TRUE;
}

/* SYNC toggled on locks the tempo to the master until switched off. */
static void on_sync_lock(GtkToggleButton *b, PdDeckView *v)
{
	gboolean on = gtk_toggle_button_get_active(b);

	if (v->updating)
		return;
	if (on && !do_sync(v, TRUE, TRUE)) {
		v->updating = TRUE;
		gtk_toggle_button_set_active(b, FALSE);
		v->updating = FALSE;
		return;
	}
	atomic_store(&v->deck->sync_lock, on);
}

static void on_flag(GtkToggleButton *b, gpointer data)
{
	atomic_bool *flag = data;
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");

	if (!v->updating)
		atomic_store(flag, gtk_toggle_button_get_active(b));
}

static GtkWidget *flag_toggle(PdDeckView *v, const char *label,
			      const char *tip, atomic_bool *flag,
			      const char *css)
{
	GtkWidget *b = toggle(label, css);

	gtk_widget_set_tooltip_text(b, tip);
	g_object_set_data(G_OBJECT(b), "view", v);
	g_signal_connect(b, "toggled", G_CALLBACK(on_flag), flag);
	return b;
}

static void sync_toggle(PdDeckView *v, GtkWidget *b, atomic_bool *flag)
{
	gboolean want = atomic_load(flag);

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b)) != want)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), want);
}

static void on_key_shift(GtkButton *b, gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");
	int ks = atomic_load(&v->deck->key_shift) + GPOINTER_TO_INT(data);

	atomic_store(&v->deck->key_shift, CLAMP(ks, -12, 12));
}

static void on_key_reset(GtkGestureClick *g, int n, double x, double y,
			 PdDeckView *v)
{
	atomic_store(&v->deck->key_shift, 0);
}

static void set_key_label(PdDeckView *v, struct track *t)
{
	int k = t ? atomic_load(&t->mkey) : -1;
	int ks = atomic_load(&v->deck->key_shift);
	char *s;

	if (k >= 0 && ks)
		k = (k / 12) * 12 + ((k % 12 + ks) % 12 + 12) % 12;
	if (k >= 0)
		s = g_strdup_printf("%s %s%s%+d", key_camelot(k), key_name(k),
				    ks ? " " : "", ks);
	else
		s = g_strdup(t && !atomic_load(&t->analysed) ? "…" : "–");
	if (!ks && k >= 0) {
		g_free(s);
		s = g_strdup_printf("%s %s", key_camelot(k), key_name(k));
	}
	gtk_label_set_text(GTK_LABEL(v->key), s);
	g_free(s);
}

static void on_jump(GtkButton *b, gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");

	deck_beat_jump(v->deck, GPOINTER_TO_INT(data) * v->deck->loop_beats);
}

static void roll_press(GtkGestureClick *g, int n, double x, double y,
		       gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(g), "view");

	deck_roll_start(v->deck, GPOINTER_TO_INT(data) / 8.0);
}

static void roll_release(GtkGestureClick *g, int n, double x, double y,
			 gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(g), "view");

	deck_roll_end(v->deck);
}

static void censor_press(GtkGestureClick *g, int n, double x, double y,
			 PdDeckView *v)
{
	deck_censor(v->deck, true);
}

static void censor_release(GtkGestureClick *g, int n, double x, double y,
			   PdDeckView *v)
{
	deck_censor(v->deck, false);
}

/* ---- beat grid popover ------------------------------------------- */

static void grid_changed(PdDeckView *v)
{
	app_save_cues(v->app, v->idx);
}

static void on_tap(GtkButton *b, PdDeckView *v)
{
	gint64 now = g_get_monotonic_time();
	double sum = 0.0;
	int i, n;

	/* A pause of two seconds starts a new tap sequence. */
	if (v->ntaps && now - v->taps[v->ntaps - 1] > 2 * G_USEC_PER_SEC)
		v->ntaps = 0;
	if (v->ntaps == (int)G_N_ELEMENTS(v->taps)) {
		memmove(v->taps, v->taps + 1, sizeof(v->taps) - sizeof(gint64));
		v->ntaps--;
	}
	v->taps[v->ntaps++] = now;
	if (v->ntaps < 3)
		return;
	n = v->ntaps - 1;
	for (i = 0; i < n; i++)
		sum += (double)(v->taps[i + 1] - v->taps[i]);
	deck_grid_set_bpm(v->deck, 60.0 * G_USEC_PER_SEC / (sum / n));
	if (v->ntaps == 3)
		deck_grid_set_downbeat(v->deck);
	grid_changed(v);
}

static void on_downbeat(GtkButton *b, PdDeckView *v)
{
	deck_grid_set_downbeat(v->deck);
	grid_changed(v);
}

static void on_grid_nudge(GtkButton *b, gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");

	deck_grid_nudge(v->deck, GPOINTER_TO_INT(data) / 1000.0);
	grid_changed(v);
}

static void on_grid_scale(GtkButton *b, gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");

	deck_grid_scale_bpm(v->deck, GPOINTER_TO_INT(data) > 0 ? 2.0 : 0.5);
	grid_changed(v);
}

static gpointer reanalyse_thread(gpointer data)
{
	struct track *t = data;
	double bpm, offset;

	if (analyze_tempo(t, &bpm, &offset) == 0) {
		atomic_store(&t->beat_offset, offset);
		atomic_store(&t->bpm, bpm);
	}
	atomic_store(&t->mkey, analyze_key(t));
	atomic_store(&t->analysed, true);
	track_unref(t);
	return NULL;
}

static void on_reanalyse(GtkButton *b, PdDeckView *v)
{
	struct track *t = deck_track(v->deck);

	if (!t || !track_done(t))
		return;
	atomic_store(&t->analysed, false);
	v->saved_for = NULL;	/* the tick saves the fresh result */
	g_thread_unref(g_thread_new("pd-reanalyse", reanalyse_thread,
				    track_ref(t)));
}

static GtkWidget *grid_button(PdDeckView *v, const char *label,
			      const char *tip, GCallback cb, gpointer data)
{
	GtkWidget *b = gtk_button_new_with_label(label);

	gtk_widget_set_tooltip_text(b, tip);
	g_object_set_data(G_OBJECT(b), "view", v);
	g_signal_connect(b, "clicked", cb, data);
	return b;
}

static GtkWidget *build_grid_menu(PdDeckView *v)
{
	GtkWidget *mb = gtk_menu_button_new();
	GtkWidget *pop = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *row, *l;

	gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), gtk_label_new("GRID"));
	gtk_widget_add_css_class(mb, "grid-button");
	gtk_widget_set_tooltip_text(mb, "Edit the beat grid");

	l = gtk_label_new("BEAT GRID");
	gtk_widget_add_css_class(l, "section-label");
	gtk_box_append(GTK_BOX(box), l);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_append(GTK_BOX(row), grid_button(v, "TAP", "Tap the tempo "
			"(3+ taps); the first tap sets the downbeat",
			G_CALLBACK(on_tap), v));
	gtk_box_append(GTK_BOX(row), grid_button(v, "Set downbeat",
			"Move the grid so a beat sits at the play head",
			G_CALLBACK(on_downbeat), v));
	gtk_box_append(GTK_BOX(box), row);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_append(GTK_BOX(row), grid_button(v, "◀ 10 ms", "Nudge the "
			"grid earlier", G_CALLBACK(on_grid_nudge),
			GINT_TO_POINTER(-10)));
	gtk_box_append(GTK_BOX(row), grid_button(v, "10 ms ▶", "Nudge the "
			"grid later", G_CALLBACK(on_grid_nudge),
			GINT_TO_POINTER(10)));
	gtk_box_append(GTK_BOX(box), row);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_append(GTK_BOX(row), grid_button(v, "BPM ÷ 2", "Halve the "
			"tempo", G_CALLBACK(on_grid_scale),
			GINT_TO_POINTER(-1)));
	gtk_box_append(GTK_BOX(row), grid_button(v, "BPM × 2", "Double the "
			"tempo", G_CALLBACK(on_grid_scale), GINT_TO_POINTER(1)));
	gtk_box_append(GTK_BOX(box), row);
	gtk_box_append(GTK_BOX(box), grid_button(v, "Re-analyse",
			"Detect tempo, grid and key again",
			G_CALLBACK(on_reanalyse), v));
	gtk_popover_set_child(GTK_POPOVER(pop), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
	return mb;
}

static void on_keylock(GtkToggleButton *b, PdDeckView *v)
{
	if (!v->updating)
		atomic_store(&v->deck->keylock,
			     gtk_toggle_button_get_active(b));
}

static void on_pitch(GtkRange *r, PdDeckView *v)
{
	/* Vertical scales put the minimum at the top: up = slower. */
	atomic_store(&v->deck->pitch, (float)(gtk_range_get_value(r) / 100.0));
}

static void on_pitch_reset(GtkButton *b, PdDeckView *v)
{
	gtk_range_set_value(GTK_RANGE(v->pitch), 0.0);
}

static void bend_press(GtkGestureClick *g, int n, double x, double y,
		       gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(g), "view");
	float amount = GPOINTER_TO_INT(data) > 0 ? BEND_AMOUNT : -BEND_AMOUNT;

	atomic_store(&v->deck->bend, amount);
}

static void bend_release(GtkGestureClick *g, int n, double x, double y,
			 gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(g), "view");

	atomic_store(&v->deck->bend, 0.0f);
}

static void on_hotcue(GtkGestureClick *g, int n, double x, double y,
		      gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(g), "view");
	int i = GPOINTER_TO_INT(data);
	guint btn = gtk_gesture_single_get_current_button(
					GTK_GESTURE_SINGLE(g));

	if (btn == GDK_BUTTON_SECONDARY)
		deck_hotcue_clear(v->deck, i);
	else
		deck_hotcue(v->deck, i);
}

static void on_loop_beats(GtkButton *b, gpointer data)
{
	PdDeckView *v = g_object_get_data(G_OBJECT(b), "view");

	deck_loop_beats(v->deck, GPOINTER_TO_INT(data) / 4.0);
}

static void on_loop_in(GtkButton *b, PdDeckView *v)
{
	deck_loop_set_in(v->deck);
}

static void on_loop_out(GtkButton *b, PdDeckView *v)
{
	deck_loop_set_out(v->deck);
}

static void on_loop_toggle(GtkToggleButton *b, PdDeckView *v)
{
	if (v->updating)
		return;
	deck_loop_toggle(v->deck);
}

static void on_loop_half(GtkButton *b, PdDeckView *v)
{
	deck_loop_scale(v->deck, 0.5);
}

static void on_loop_double(GtkButton *b, PdDeckView *v)
{
	deck_loop_scale(v->deck, 2.0);
}

/* ---- drag and drop ----------------------------------------------- */

static gboolean on_drop(GtkDropTarget *t, const GValue *val, double x,
			double y, PdDeckView *v)
{
	if (G_VALUE_HOLDS(val, PD_TYPE_MEDIA_ITEM)) {
		app_load_item(v->app, v->idx, g_value_get_object(val));
		return TRUE;
	}
	if (G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) {
		GSList *files = g_value_get_boxed(val);
		char *path;

		if (!files)
			return FALSE;
		path = g_file_get_path(files->data);
		if (path)
			app_load_path(v->app, v->idx, path);
		g_free(path);
		return path != NULL;
	}
	return FALSE;
}

/* ---- construction ------------------------------------------------ */

static GtkWidget *build_header(PdDeckView *v)
{
	GtkWidget *grid = gtk_grid_new();
	char deck_name[2] = { app_deck_letter(v->idx), '\0' };
	GtkWidget *badge = gtk_label_new(deck_name);

	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_widget_add_css_class(badge, "deck-badge");
	gtk_widget_add_css_class(badge, app_deck_class(v->idx));
	gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

	v->title = gtk_label_new("");
	v->artist = gtk_label_new("");
	v->bpm = gtk_label_new("");
	v->time = gtk_label_new("");
	v->status = gtk_label_new("");
	gtk_widget_add_css_class(v->title, "track-title");
	gtk_widget_add_css_class(v->artist, "dim-label");
	gtk_widget_add_css_class(v->bpm, "bpm");
	gtk_widget_add_css_class(v->bpm, "readout");
	gtk_widget_add_css_class(v->time, "time");
	gtk_widget_add_css_class(v->time, "readout");
	gtk_widget_add_css_class(v->status, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(v->title), 0.0f);
	gtk_label_set_xalign(GTK_LABEL(v->artist), 0.0f);
	gtk_label_set_xalign(GTK_LABEL(v->status), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(v->title), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize(GTK_LABEL(v->artist), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize(GTK_LABEL(v->status), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(v->title, TRUE);
	gtk_label_set_xalign(GTK_LABEL(v->bpm), 1.0f);
	gtk_label_set_xalign(GTK_LABEL(v->time), 1.0f);

	gtk_grid_attach(GTK_GRID(grid), badge, 0, 0, 1, 2);
	gtk_grid_attach(GTK_GRID(grid), v->title, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v->artist, 1, 1, 1, 1);
	v->key = gtk_label_new("–");
	gtk_widget_add_css_class(v->key, "readout");
	gtk_widget_add_css_class(v->key, "key-readout");
	gtk_widget_set_tooltip_text(v->key, "Musical key (Camelot); click "
				    "to reset the key shift");
	gtk_widget_set_valign(v->key, GTK_ALIGN_CENTER);
	{
		GtkGesture *g = gtk_gesture_click_new();

		g_signal_connect(g, "pressed", G_CALLBACK(on_key_reset), v);
		gtk_widget_add_controller(v->key, GTK_EVENT_CONTROLLER(g));
	}
	gtk_grid_attach(GTK_GRID(grid), v->bpm, 2, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v->time, 2, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v->key, 3, 0, 1, 2);
	gtk_grid_attach(GTK_GRID(grid), v->status, 1, 2, 2, 1);
	return grid;
}

static GtkWidget *build_loops(PdDeckView *v)
{
	static const struct {
		const char *label;
		int quarter_beats;
	} sizes[] = {
		{ "½", 2 }, { "1", 4 }, { "2", 8 }, { "4", 16 }, { "8", 32 },
	};
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *b;
	size_t i;

	gtk_widget_add_css_class(group, "linked");
	for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
		b = button(sizes[i].label, "loop-size");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_loop_beats),
				 GINT_TO_POINTER(sizes[i].quarter_beats));
		gtk_box_append(GTK_BOX(group), b);
	}
	gtk_box_append(GTK_BOX(box), group);

	group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(group, "linked");
	b = button("½×", "loop-size");
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_half), v);
	gtk_box_append(GTK_BOX(group), b);
	b = button("2×", "loop-size");
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_double), v);
	gtk_box_append(GTK_BOX(group), b);
	gtk_box_append(GTK_BOX(box), group);

	{
		static const struct {
			const char *label;
			int eighths;
		} rolls[] = {
			{ "⅛", 1 }, { "¼", 2 }, { "½", 4 }, { "1", 8 },
		};
		GtkGesture *g;

		group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_margin_start(group, 6);
		gtk_widget_add_css_class(group, "linked");
		for (i = 0; i < G_N_ELEMENTS(rolls); i++) {
			b = button(rolls[i].label, "roll");
			gtk_widget_set_tooltip_text(b, "Hold for a loop roll; "
						    "the track keeps running "
						    "underneath");
			g = gtk_gesture_click_new();
			g_object_set_data(G_OBJECT(g), "view", v);
			g_signal_connect(g, "pressed", G_CALLBACK(roll_press),
					 GINT_TO_POINTER(rolls[i].eighths));
			g_signal_connect(g, "released",
					 G_CALLBACK(roll_release), NULL);
			g_signal_connect(g, "cancel",
					 G_CALLBACK(roll_release), NULL);
			gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
			gtk_box_append(GTK_BOX(group), b);
		}
		gtk_box_append(GTK_BOX(box), group);

		b = button("CENSOR", "censor");
		gtk_widget_set_tooltip_text(b, "Hold to play backwards "
					    "(bleep), release to slip back");
		g = gtk_gesture_click_new();
		g_signal_connect(g, "pressed", G_CALLBACK(censor_press), v);
		g_signal_connect(g, "released", G_CALLBACK(censor_release), v);
		g_signal_connect(g, "cancel", G_CALLBACK(censor_release), v);
		gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
		gtk_box_append(GTK_BOX(box), b);
	}
	return box;
}

static GtkWidget *build_hotcues(PdDeckView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	GtkWidget *b;
	int i;

	for (i = 0; i < DECK_HOTCUES; i++) {
		char label[4];
		GtkGesture *g = gtk_gesture_click_new();

		g_snprintf(label, sizeof(label), "%d", i + 1);
		b = button(label, "hotcue");
		gtk_widget_add_css_class(b, "pad");
		gtk_widget_set_tooltip_text(b, "Click: set or jump, "
					    "right click: clear");
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
		g_object_set_data(G_OBJECT(g), "view", v);
		g_signal_connect(g, "pressed", G_CALLBACK(on_hotcue),
				 GINT_TO_POINTER(i));
		gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
		gtk_box_append(GTK_BOX(box), b);
		v->hotcue[i] = b;
	}

	b = button("In", NULL);
	gtk_widget_set_margin_start(b, 6);
	gtk_widget_set_tooltip_text(b, "Manual loop in point");
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_in), v);
	gtk_box_append(GTK_BOX(box), b);
	b = button("Out", NULL);
	gtk_widget_set_tooltip_text(b, "Manual loop out point, starts the "
				    "loop");
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_out), v);
	gtk_box_append(GTK_BOX(box), b);
	v->loop = toggle("Loop", "loop-toggle");
	gtk_widget_set_tooltip_text(v->loop, "Loop on/off (reloop)");
	g_signal_connect(v->loop, "toggled", G_CALLBACK(on_loop_toggle), v);
	gtk_box_append(GTK_BOX(box), v->loop);

	return box;
}

static GtkWidget *bend_button(PdDeckView *v, const char *label, int dir)
{
	GtkWidget *b = button(label, "bend");
	GtkGesture *g = gtk_gesture_click_new();

	gtk_widget_set_tooltip_text(b, dir > 0 ? "Hold to nudge forward" :
				    "Hold to nudge back");
	g_object_set_data(G_OBJECT(g), "view", v);
	g_signal_connect(g, "pressed", G_CALLBACK(bend_press),
			 GINT_TO_POINTER(dir));
	g_signal_connect(g, "released", G_CALLBACK(bend_release), NULL);
	g_signal_connect(g, "cancel", G_CALLBACK(bend_release), NULL);
	gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
	return b;
}

static GtkWidget *build_transport(PdDeckView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkGesture *g;

	v->cue = button("CUE", "cue-button");
	g = gtk_gesture_click_new();
	g_signal_connect(g, "pressed", G_CALLBACK(on_cue_press), v);
	g_signal_connect(g, "released", G_CALLBACK(on_cue_release), v);
	g_signal_connect(g, "cancel", G_CALLBACK(on_cue_release), v);
	gtk_widget_add_controller(v->cue, GTK_EVENT_CONTROLLER(g));
	gtk_widget_set_tooltip_text(v->cue, "Cue: set while paused, hold to "
				    "preview, press while playing to return");
	gtk_box_append(GTK_BOX(box), v->cue);

	v->play = toggle("▶", "play-button");
	g_signal_connect(v->play, "toggled", G_CALLBACK(on_play), v);
	gtk_box_append(GTK_BOX(box), v->play);

	gtk_box_append(GTK_BOX(box), bend_button(v, "◀", -1));
	gtk_box_append(GTK_BOX(box), bend_button(v, "▶", 1));

	v->sync = toggle("SYNC", "sync-button");
	gtk_widget_set_tooltip_text(v->sync, "Sync lock: match tempo and "
				    "beat phase to the master deck and follow "
				    "it; click again to release");
	g_signal_connect(v->sync, "toggled", G_CALLBACK(on_sync_lock), v);
	gtk_widget_set_margin_start(v->sync, 8);
	gtk_box_append(GTK_BOX(box), v->sync);

	v->keylock = toggle("KEY", NULL);
	gtk_widget_set_tooltip_text(v->keylock, "Keylock: change tempo "
				    "without changing pitch");
	gtk_widget_set_sensitive(v->keylock, deck_has_keylock());
	g_signal_connect(v->keylock, "toggled", G_CALLBACK(on_keylock), v);
	gtk_box_append(GTK_BOX(box), v->keylock);

	return box;
}

static GtkWidget *build_modes(PdDeckView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *b;

	v->quant = flag_toggle(v, "Q", "Quantize: cues, loops and jumps "
			       "snap to the beat grid", &v->deck->quantize,
			       "quant-button");
	gtk_box_append(GTK_BOX(box), v->quant);
	v->rev = flag_toggle(v, "REV", "Play backwards", &v->deck->reverse,
			     "rev-button");
	gtk_box_append(GTK_BOX(box), v->rev);
	v->slip = flag_toggle(v, "SLIP", "Slip mode: loops, scratches and "
			      "reverse play while the track keeps running "
			      "underneath", &v->deck->slip, "slip-button");
	gtk_box_append(GTK_BOX(box), v->slip);
	{
		GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

		gtk_widget_add_css_class(group, "linked");
		gtk_widget_set_margin_start(group, 6);
		b = button("⇤", "jump");
		gtk_widget_set_tooltip_text(b, "Beat jump back by the loop "
					    "length");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_jump),
				 GINT_TO_POINTER(-1));
		gtk_box_append(GTK_BOX(group), b);
		b = button("⇥", "jump");
		gtk_widget_set_tooltip_text(b, "Beat jump forward by the "
					    "loop length");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_jump),
				 GINT_TO_POINTER(1));
		gtk_box_append(GTK_BOX(group), b);
		gtk_box_append(GTK_BOX(box), group);

		group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_add_css_class(group, "linked");
		gtk_widget_set_margin_start(group, 6);
		b = button("♭", "keyshift");
		gtk_widget_set_tooltip_text(b, "Key down one semitone");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_key_shift),
				 GINT_TO_POINTER(-1));
		gtk_widget_set_sensitive(b, deck_has_keylock());
		gtk_box_append(GTK_BOX(group), b);
		b = button("♯", "keyshift");
		gtk_widget_set_tooltip_text(b, "Key up one semitone");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_key_shift),
				 GINT_TO_POINTER(1));
		gtk_widget_set_sensitive(b, deck_has_keylock());
		gtk_box_append(GTK_BOX(group), b);
		gtk_box_append(GTK_BOX(box), group);
	}
	gtk_box_append(GTK_BOX(box), build_grid_menu(v));
	return box;
}

static GtkWidget *build_pitch(PdDeckView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *b;
	int range = v->app->cfg.pitch_range;

	b = gtk_label_new("PITCH");
	gtk_widget_add_css_class(b, "section-label");
	gtk_box_append(GTK_BOX(box), b);

	v->pitch = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
					    -range, range, 0.01);
	gtk_range_set_value(GTK_RANGE(v->pitch), 0.0);
	gtk_scale_set_draw_value(GTK_SCALE(v->pitch), FALSE);
	gtk_scale_set_has_origin(GTK_SCALE(v->pitch), FALSE);
	gtk_scale_add_mark(GTK_SCALE(v->pitch), 0.0, GTK_POS_LEFT, NULL);
	gtk_widget_set_size_request(v->pitch, -1, 170);
	gtk_widget_set_focusable(v->pitch, FALSE);
	gtk_widget_add_css_class(v->pitch, "pitch-fader");
	g_signal_connect(v->pitch, "value-changed", G_CALLBACK(on_pitch), v);
	gtk_box_append(GTK_BOX(box), v->pitch);

	v->pitch_label = gtk_label_new("");
	gtk_widget_add_css_class(v->pitch_label, "dim-label");
	gtk_widget_add_css_class(v->pitch_label, "mono");
	gtk_box_append(GTK_BOX(box), v->pitch_label);

	b = button("0", NULL);
	gtk_widget_set_tooltip_text(b, "Reset pitch");
	g_signal_connect(b, "clicked", G_CALLBACK(on_pitch_reset), v);
	gtk_box_append(GTK_BOX(box), b);
	return box;
}

static void pd_deck_view_dispose(GObject *obj)
{
	PdDeckView *v = PD_DECK_VIEW(obj);

	if (v->tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(v), v->tick);
		v->tick = 0;
	}
	G_OBJECT_CLASS(pd_deck_view_parent_class)->dispose(obj);
}

static void pd_deck_view_class_init(PdDeckViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_deck_view_dispose;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "deck");
}

static void pd_deck_view_init(PdDeckView *v)
{
}

GtkWidget *pd_deck_view_new(struct app *app, int idx)
{
	PdDeckView *v = g_object_new(PD_TYPE_DECK_VIEW,
				     "orientation", GTK_ORIENTATION_HORIZONTAL,
				     "spacing", 8, NULL);
	GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkDropTarget *drop;
	GType types[] = { PD_TYPE_MEDIA_ITEM, GDK_TYPE_FILE_LIST };

	v->app = app;
	v->idx = idx;
	v->deck = &app->engine.deck[idx];
	gtk_widget_add_css_class(GTK_WIDGET(v), app_deck_class(idx));
	gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);

	gtk_box_append(GTK_BOX(main), build_header(v));
	v->wave = pd_waveform_new(v->deck, FALSE);
	v->overview = pd_waveform_new(v->deck, TRUE);
	gtk_widget_set_tooltip_text(v->wave, "Drag to scratch, scroll to "
				    "seek, Ctrl+scroll to zoom");
	pd_waveform_set_color(PD_WAVEFORM(v->wave), app_deck_color(idx));
	pd_waveform_set_color(PD_WAVEFORM(v->overview), app_deck_color(idx));
	gtk_box_append(GTK_BOX(main), v->wave);
	gtk_box_append(GTK_BOX(main), v->overview);

	{
		GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		GtkWidget *pads = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

		v->transport_row = build_transport(v);
		v->modes_row = build_modes(v);
		v->hotcue_row = build_hotcues(v);
		v->loop_row = build_loops(v);
		gtk_box_append(GTK_BOX(pads), v->transport_row);
		gtk_box_append(GTK_BOX(pads), v->modes_row);
		gtk_box_append(GTK_BOX(pads), v->hotcue_row);
		gtk_box_append(GTK_BOX(pads), v->loop_row);
		gtk_widget_set_valign(pads, GTK_ALIGN_CENTER);
		v->platter = pd_platter_new(v->deck, app_deck_color(idx),
					    PLATTER_SIZE);
		gtk_box_append(GTK_BOX(bottom), v->platter);
		gtk_box_append(GTK_BOX(bottom), pads);
		gtk_box_append(GTK_BOX(main), bottom);
	}

	gtk_widget_set_hexpand(main, TRUE);
	gtk_box_append(GTK_BOX(v), main);
	v->pitch_box = build_pitch(v);
	gtk_box_append(GTK_BOX(v), v->pitch_box);

	drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
	gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), v);
	gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(drop));

	pd_deck_view_apply_config(v);
	v->tick = gtk_widget_add_tick_callback(GTK_WIDGET(v), tick, NULL,
					       NULL);
	return GTK_WIDGET(v);
}

void pd_deck_view_apply_config(PdDeckView *v)
{
	int range = v->app->cfg.pitch_range;
	double cur = gtk_range_get_value(GTK_RANGE(v->pitch));
	char *tip = g_strdup_printf("Pitch ±%d %% (Preferences → Decks)",
				    range);

	gtk_widget_set_tooltip_text(v->pitch, tip);
	g_free(tip);
	gtk_range_set_range(GTK_RANGE(v->pitch), -range, range);
	gtk_range_set_value(GTK_RANGE(v->pitch), CLAMP(cur, -range, range));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->keylock),
				     atomic_load(&v->deck->keylock));
}

void pd_deck_view_set_compact(PdDeckView *v, int level)
{
	level = CLAMP(level, 0, PD_COMPACT_MAX);
	if (level == v->compact)
		return;
	v->compact = level;
	/* the full track overview outlives every button but transport */
	gtk_widget_set_visible(v->loop_row, level < 1);
	gtk_widget_set_visible(v->hotcue_row, level < 2);
	gtk_widget_set_visible(v->platter, level < 2);
	gtk_widget_set_visible(v->modes_row, level < 3);
	gtk_widget_set_visible(v->pitch_box, level < 3);
	gtk_widget_set_visible(v->status, level < 3);
	gtk_widget_set_visible(v->wave, level < 4);
}

void pd_deck_view_action(PdDeckView *v, const char *action, gboolean press)
{
	if (g_str_equal(action, "play")) {
		if (press)
			deck_play(v->deck, !atomic_load(&v->deck->playing));
	} else if (g_str_equal(action, "cue")) {
		if (press)
			deck_cue_press(v->deck);
		else
			deck_cue_release(v->deck);
	} else if (g_str_equal(action, "sync")) {
		if (press)
			do_sync(v, TRUE, TRUE);
	} else if (g_str_equal(action, "bend-")) {
		atomic_store(&v->deck->bend, press ? -BEND_AMOUNT : 0.0f);
	} else if (g_str_equal(action, "bend+")) {
		atomic_store(&v->deck->bend, press ? BEND_AMOUNT : 0.0f);
	} else if (g_str_equal(action, "loop")) {
		if (press)
			deck_loop_toggle(v->deck);
	} else if (g_str_equal(action, "loop4")) {
		if (press)
			deck_loop_beats(v->deck, 4.0);
	} else if (g_str_has_prefix(action, "hotcue")) {
		if (press)
			deck_hotcue(v->deck, action[6] - '1');
	}
}
