// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * deckview.c - one deck panel
 */
#include <math.h>

#include "cuestore.h"
#include "deckview.h"
#include "waveform.h"

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
	GtkWidget *loop;
	GtkWidget *hotcue[DECK_HOTCUES];
	GtkWidget *pitch;
	GtkWidget *pitch_label;

	guint tick;
	struct track *saved_for;	/* track whose analysis was saved */
	gboolean updating;
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

	s = g_strdup_printf("%+.2f%%  ±%d", pitch, (int)range);
	gtk_label_set_text(GTK_LABEL(v->pitch_label), s);
	g_free(s);
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
	gtk_button_set_label(GTK_BUTTON(v->play), playing ? "⏸" : "▶");
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->loop)) !=
	    atomic_load(&v->deck->loop_on))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->loop),
					     atomic_load(&v->deck->loop_on));
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

static void on_sync(GtkButton *b, PdDeckView *v)
{
	struct deck *master = &v->app->engine.deck[1 - v->idx];
	double p = deck_sync_pitch(v->deck, master);
	double range = v->app->cfg.pitch_range / 100.0;

	if (isnan(p)) {
		app_toast(v->app, "Sync needs a tempo on both decks");
		return;
	}
	if (fabs(p) > range) {
		app_toast(v->app, "Sync needs %+.1f%%, outside the ±%d%% "
			  "pitch range", p * 100.0, v->app->cfg.pitch_range);
		return;
	}
	atomic_store(&v->deck->pitch, (float)p);
	gtk_range_set_value(GTK_RANGE(v->pitch), -p * 100.0);
	deck_sync_phase(v->deck, master);
}

static void on_keylock(GtkToggleButton *b, PdDeckView *v)
{
	atomic_store(&v->deck->keylock, gtk_toggle_button_get_active(b));
}

static void on_pitch(GtkRange *r, PdDeckView *v)
{
	/* Fader is inverted: up = slower, like on a CDJ. */
	atomic_store(&v->deck->pitch, (float)(-gtk_range_get_value(r) / 100.0));
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

static void on_zoom_in(GtkButton *b, PdDeckView *v)
{
	pd_waveform_zoom_by(PD_WAVEFORM(v->wave), 1.5);
}

static void on_zoom_out(GtkButton *b, PdDeckView *v)
{
	pd_waveform_zoom_by(PD_WAVEFORM(v->wave), 1.0 / 1.5);
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
	char *deck_name = g_strdup_printf("%c", 'A' + v->idx);
	GtkWidget *badge = gtk_label_new(deck_name);

	g_free(deck_name);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_widget_add_css_class(badge, "deck-badge");
	gtk_widget_add_css_class(badge, v->idx == 0 ? "deck-a" : "deck-b");
	gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

	v->title = gtk_label_new("");
	v->artist = gtk_label_new("");
	v->bpm = gtk_label_new("");
	v->time = gtk_label_new("");
	v->status = gtk_label_new("");
	gtk_widget_add_css_class(v->title, "track-title");
	gtk_widget_add_css_class(v->artist, "dim-label");
	gtk_widget_add_css_class(v->bpm, "bpm");
	gtk_widget_add_css_class(v->time, "time");
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
	gtk_grid_attach(GTK_GRID(grid), v->bpm, 2, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v->time, 2, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v->status, 1, 2, 2, 1);
	return grid;
}

static GtkWidget *build_loops(PdDeckView *v)
{
	static const struct {
		const char *label;
		int quarter_beats;
	} sizes[] = {
		{ "¼", 1 }, { "½", 2 }, { "1", 4 }, { "2", 8 }, { "4", 16 },
		{ "8", 32 }, { "16", 64 },
	};
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *b;
	size_t i;

	b = gtk_label_new("LOOP");
	gtk_widget_add_css_class(b, "section-label");
	gtk_box_append(GTK_BOX(box), b);
	for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
		b = button(sizes[i].label, "loop-size");
		g_object_set_data(G_OBJECT(b), "view", v);
		g_signal_connect(b, "clicked", G_CALLBACK(on_loop_beats),
				 GINT_TO_POINTER(sizes[i].quarter_beats));
		gtk_box_append(GTK_BOX(box), b);
	}
	b = button("½×", NULL);
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_half), v);
	gtk_box_append(GTK_BOX(box), b);
	b = button("2×", NULL);
	g_signal_connect(b, "clicked", G_CALLBACK(on_loop_double), v);
	gtk_box_append(GTK_BOX(box), b);

	return box;
}

static GtkWidget *build_hotcues(PdDeckView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *b;
	int i;

	b = gtk_label_new("HOT CUES");
	gtk_widget_add_css_class(b, "section-label");
	gtk_box_append(GTK_BOX(box), b);
	for (i = 0; i < DECK_HOTCUES; i++) {
		char label[4];
		GtkGesture *g = gtk_gesture_click_new();

		g_snprintf(label, sizeof(label), "%d", i + 1);
		b = button(label, "hotcue");
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
	gtk_widget_set_margin_start(b, 12);
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
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b;
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

	b = button("SYNC", "sync-button");
	gtk_widget_set_tooltip_text(b, "Match tempo and beat phase to the "
				    "other deck");
	g_signal_connect(b, "clicked", G_CALLBACK(on_sync), v);
	gtk_widget_set_margin_start(b, 10);
	gtk_box_append(GTK_BOX(box), b);

	v->keylock = toggle("KEY", NULL);
	gtk_widget_set_tooltip_text(v->keylock, "Keylock: change tempo "
				    "without changing pitch");
	gtk_widget_set_sensitive(v->keylock, deck_has_keylock());
	g_signal_connect(v->keylock, "toggled", G_CALLBACK(on_keylock), v);
	gtk_box_append(GTK_BOX(box), v->keylock);

	b = button("−", NULL);
	gtk_widget_set_tooltip_text(b, "Zoom waveform out (Ctrl+scroll)");
	gtk_widget_set_margin_start(b, 10);
	g_signal_connect(b, "clicked", G_CALLBACK(on_zoom_out), v);
	gtk_box_append(GTK_BOX(box), b);
	b = button("+", NULL);
	gtk_widget_set_tooltip_text(b, "Zoom waveform in");
	g_signal_connect(b, "clicked", G_CALLBACK(on_zoom_in), v);
	gtk_box_append(GTK_BOX(box), b);
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
	gtk_widget_add_css_class(GTK_WIDGET(v), idx == 0 ? "deck-a" :
				 "deck-b");
	gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);

	gtk_box_append(GTK_BOX(main), build_header(v));
	v->wave = pd_waveform_new(v->deck, FALSE);
	v->overview = pd_waveform_new(v->deck, TRUE);
	gtk_box_append(GTK_BOX(main), v->wave);
	gtk_box_append(GTK_BOX(main), v->overview);

	gtk_box_append(GTK_BOX(main), build_transport(v));
	gtk_box_append(GTK_BOX(main), build_hotcues(v));
	gtk_box_append(GTK_BOX(main), build_loops(v));

	gtk_widget_set_hexpand(main, TRUE);
	gtk_box_append(GTK_BOX(v), main);
	gtk_box_append(GTK_BOX(v), build_pitch(v));

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

	gtk_range_set_range(GTK_RANGE(v->pitch), -range, range);
	gtk_range_set_value(GTK_RANGE(v->pitch), CLAMP(cur, -range, range));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->keylock),
				     atomic_load(&v->deck->keylock));
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
			on_sync(NULL, v);
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
