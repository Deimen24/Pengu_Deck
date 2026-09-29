// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * mixerview.c - mixer section
 */
#include <math.h>

#include "automix.h"
#include "knob.h"
#include "meter.h"
#include "mixerview.h"

struct _PdMixerView {
	GtkBox parent;
	struct app *app;

	GtkWidget *strip[ENGINE_DECKS];
	GtkWidget *meter[ENGINE_DECKS];
	GtkWidget *master_meter;
	GtkWidget *xfader;
	GtkWidget *pfl[ENGINE_DECKS];
	GtkWidget *record;
	GtkWidget *record_label;
	GtkWidget *fader[ENGINE_DECKS];
	GPtrArray *knobs;		/* PdKnob, target in "target" data */
	guint tick;
	gboolean updating;
};

G_DEFINE_FINAL_TYPE(PdMixerView, pd_mixer_view, GTK_TYPE_BOX)

/* Knob callbacks store into the atomic the knob was created for. */
static void on_knob_float(PdKnob *k, gpointer data)
{
	_Atomic float *target = data;

	atomic_store(target, (float)pd_knob_get_value(k));
}

static GtkWidget *knob_for(const char *label, double min, double max,
			   double def, _Atomic float *target,
			   const char *accent)
{
	GtkWidget *k = pd_knob_new(label, min, max, def);

	g_signal_connect(k, "changed", G_CALLBACK(on_knob_float), target);
	pd_knob_set_accent(PD_KNOB(k), accent);
	g_object_set_data(G_OBJECT(k), "target", target);
	return k;
}

/* Knobs and faders show values changed from MIDI or automix. */
static void follow_atomics(PdMixerView *v)
{
	struct engine *e = &v->app->engine;
	guint i;

	v->updating = TRUE;
	for (i = 0; i < v->knobs->len; i++) {
		PdKnob *k = v->knobs->pdata[i];
		_Atomic float *t = g_object_get_data(G_OBJECT(k), "target");
		float val = atomic_load(t);

		if (fabsf(val - (float)pd_knob_get_value(k)) > 1e-4f)
			pd_knob_set_value(k, val);
	}
	for (i = 0; i < ENGINE_DECKS; i++) {
		float val = atomic_load(&e->deck[i].volume);
		GtkRange *r = GTK_RANGE(v->fader[i]);

		if (fabsf(val - (float)gtk_range_get_value(r)) > 1e-3f)
			gtk_range_set_value(r, val);
	}
	if (!automix_fading()) {
		float x = atomic_load(&e->xfader);

		if (fabsf(x - (float)gtk_range_get_value(
				GTK_RANGE(v->xfader))) > 1e-3f)
			gtk_range_set_value(GTK_RANGE(v->xfader), x);
	}
	v->updating = FALSE;
}

/* Crossfader assignment cycles left → thru → right. */
static void on_xf_side(GtkButton *b, gpointer data)
{
	atomic_int *side = data;
	static const char *const labels[] = { "XF A", "XF –", "XF B" };
	int s = atomic_load(side) + 1;

	s = (s + 1) % 3;
	atomic_store(side, s - 1);
	gtk_button_set_label(b, labels[s]);
}

static void on_kill(GtkToggleButton *b, gpointer data)
{
	atomic_bool *target = data;

	atomic_store(target, gtk_toggle_button_get_active(b));
}

static void on_fader(GtkRange *r, gpointer data)
{
	_Atomic float *target = data;

	/* Also fires when follow_atomics() moves it: same value, harmless. */
	atomic_store(target, (float)gtk_range_get_value(r));
}

static void track_knob(PdMixerView *v, GtkWidget *k)
{
	g_ptr_array_add(v->knobs, k);
}

static void on_xfader(GtkRange *r, PdMixerView *v)
{
	if (!v->updating)
		atomic_store(&v->app->engine.xfader,
			     (float)gtk_range_get_value(r));
}

static void on_pfl(GtkToggleButton *b, gpointer data)
{
	atomic_bool *target = data;

	atomic_store(target, gtk_toggle_button_get_active(b));
}

static void on_record(GtkToggleButton *b, PdMixerView *v)
{
	struct app *a = v->app;
	GDateTime *now;
	char *stamp, *path, *err = NULL;

	if (v->updating)
		return;
	if (!gtk_toggle_button_get_active(b)) {
		engine_record_stop(&a->engine);
		app_toast(a, "Recording saved to %s", a->cfg.record_dir);
		return;
	}
	g_mkdir_with_parents(a->cfg.record_dir, 0755);
	now = g_date_time_new_now_local();
	stamp = g_date_time_format(now, "%Y-%m-%d %H-%M-%S");
	path = g_strdup_printf("%s/Mix %s.wav", a->cfg.record_dir, stamp);
	if (engine_record_start(&a->engine, path, &err) != 0) {
		app_toast(a, "Recording failed: %s", err);
		g_free(err);
		v->updating = TRUE;
		gtk_toggle_button_set_active(b, FALSE);
		v->updating = FALSE;
	}
	g_free(path);
	g_free(stamp);
	g_date_time_unref(now);
}

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	PdMixerView *v = PD_MIXER_VIEW(w);
	struct engine *e = &v->app->engine;
	int i;

	for (i = 0; i < ENGINE_DECKS; i++) {
		struct deck *d = &e->deck[i];

		pd_meter_update(PD_METER(v->meter[i]),
				atomic_exchange(&d->peak_l, 0.0f),
				atomic_exchange(&d->peak_r, 0.0f));
	}
	pd_meter_update(PD_METER(v->master_meter),
			atomic_exchange(&e->peak_l, 0.0f),
			atomic_exchange(&e->peak_r, 0.0f));

	if (automix_fading()) {
		v->updating = TRUE;
		gtk_range_set_value(GTK_RANGE(v->xfader),
				    atomic_load(&e->xfader));
		v->updating = FALSE;
	}
	follow_atomics(v);

	if (engine_recording(e)) {
		double s = engine_record_seconds(e);
		char *t = g_strdup_printf("● %d:%02d", (int)s / 60,
					  (int)s % 60);

		gtk_label_set_text(GTK_LABEL(v->record_label), t);
		g_free(t);
	} else {
		gtk_label_set_text(GTK_LABEL(v->record_label), "REC");
	}
	return G_SOURCE_CONTINUE;
}

static GtkWidget *section_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_widget_add_css_class(l, "section-label");
	return l;
}

static GtkWidget *build_strip(PdMixerView *v, int i)
{
	struct deck *d = &v->app->engine.deck[i];
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *knobs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *row, *fader, *b, *k, *l;
	static const char *const names[EQ_BANDS] = { "LOW", "MID", "HIGH" };
	static const char *const band_col[EQ_BANDS] = {
		"#ff7043", "#ffd54f", "#4fc3f7",
	};
	char letter[2] = { app_deck_letter(i), '\0' };
	const char *color = app_deck_color(i);
	int band;

	gtk_widget_add_css_class(box, "strip");
	gtk_widget_add_css_class(box, app_deck_class(i));
	l = section_label(letter);
	gtk_widget_add_css_class(l, "deck-letter");
	gtk_box_append(GTK_BOX(box), l);
	k = knob_for("TRIM", -12.0, 12.0, 0.0, &d->trim_db, color);
	track_knob(v, k);
	gtk_box_append(GTK_BOX(knobs), k);

	for (band = EQ_BANDS - 1; band >= 0; band--) {
		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		k = knob_for(names[band], -26.0, 9.0, 0.0, &d->eq_db[band],
			     band_col[band]);
		track_knob(v, k);
		gtk_box_append(GTK_BOX(row), k);
		b = gtk_toggle_button_new_with_label("K");
		gtk_widget_add_css_class(b, "kill");
		gtk_widget_set_focusable(b, FALSE);
		gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(b, "Kill this band");
		g_signal_connect(b, "toggled", G_CALLBACK(on_kill),
				 &d->eq_kill[band]);
		gtk_box_append(GTK_BOX(row), b);
		gtk_box_append(GTK_BOX(knobs), row);
	}

	k = knob_for("FILTER", -1.0, 1.0, 0.0, &d->filter, "#c792ea");
	gtk_widget_set_tooltip_text(k, "Left: low pass, right: high pass");
	track_knob(v, k);
	gtk_box_append(GTK_BOX(knobs), k);

	b = gtk_button_new_with_label(i % 2 ? "XF B" : "XF A");
	gtk_widget_add_css_class(b, "xf-side");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_tooltip_text(b, "Crossfader side: A side, bypass, "
				    "B side");
	g_signal_connect(b, "clicked", G_CALLBACK(on_xf_side), &d->xf_side);
	gtk_box_append(GTK_BOX(knobs), b);

	v->pfl[i] = gtk_toggle_button_new_with_label("🎧");
	gtk_widget_set_focusable(v->pfl[i], FALSE);
	gtk_widget_add_css_class(v->pfl[i], "pfl");
	gtk_widget_set_tooltip_text(v->pfl[i], "Pre fader listen (headphone "
				    "cue)");
	g_signal_connect(v->pfl[i], "toggled", G_CALLBACK(on_pfl), &d->pfl);
	gtk_box_append(GTK_BOX(knobs), v->pfl[i]);

	fader = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0.0, 1.0,
					 0.005);
	gtk_range_set_inverted(GTK_RANGE(fader), TRUE);
	gtk_range_set_value(GTK_RANGE(fader), 1.0);
	gtk_scale_set_draw_value(GTK_SCALE(fader), FALSE);
	gtk_widget_set_vexpand(fader, TRUE);
	gtk_widget_set_focusable(fader, FALSE);
	gtk_widget_add_css_class(fader, "channel-fader");
	g_signal_connect(fader, "value-changed", G_CALLBACK(on_fader),
			 &d->volume);
	v->fader[i] = fader;
	v->meter[i] = pd_meter_new();
	gtk_widget_set_margin_top(v->meter[i], 12);
	gtk_widget_set_margin_bottom(v->meter[i], 12);
	gtk_widget_set_margin_start(v->meter[i], 2);
	gtk_widget_set_margin_end(v->meter[i], 2);

	/* Knobs on the outside, fader and meter towards the master. */
	if (i % 2 == 0) {
		gtk_box_append(GTK_BOX(cols), knobs);
		gtk_box_append(GTK_BOX(cols), fader);
		gtk_box_append(GTK_BOX(cols), v->meter[i]);
	} else {
		gtk_box_append(GTK_BOX(cols), v->meter[i]);
		gtk_box_append(GTK_BOX(cols), fader);
		gtk_box_append(GTK_BOX(cols), knobs);
	}
	gtk_widget_set_vexpand(cols, TRUE);
	gtk_box_append(GTK_BOX(box), cols);
	return box;
}

static GtkWidget *build_master(PdMixerView *v)
{
	struct engine *e = &v->app->engine;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *knobs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *k;

	gtk_widget_add_css_class(box, "strip");
	gtk_box_append(GTK_BOX(box), section_label("MASTER"));
	k = knob_for("LEVEL", 0.0, 1.5, 1.0, &e->master, "#ffd54f");
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	track_knob(v, k);
	gtk_box_append(GTK_BOX(knobs), k);

	gtk_box_append(GTK_BOX(knobs), section_label("CUE"));
	k = knob_for("MIX", 0.0, 1.0, 0.0, &e->cue_mix, "#4dd0e1");
	gtk_widget_set_tooltip_text(k, "Headphone mix: cue ↔ master");
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	track_knob(v, k);
	gtk_box_append(GTK_BOX(knobs), k);
	k = knob_for("VOL", 0.0, 1.5, 1.0, &e->cue_vol, "#4dd0e1");
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	track_knob(v, k);
	gtk_box_append(GTK_BOX(knobs), k);

	v->record = gtk_toggle_button_new();
	v->record_label = gtk_label_new("REC");
	gtk_button_set_child(GTK_BUTTON(v->record), v->record_label);
	gtk_widget_add_css_class(v->record, "record");
	gtk_widget_set_focusable(v->record, FALSE);
	gtk_widget_set_tooltip_text(v->record, "Record the master output "
				    "to a WAV file");
	g_signal_connect(v->record, "toggled", G_CALLBACK(on_record), v);
	gtk_box_append(GTK_BOX(knobs), v->record);

	v->master_meter = pd_meter_new();
	gtk_widget_set_margin_top(v->master_meter, 12);
	gtk_widget_set_margin_bottom(v->master_meter, 12);
	gtk_box_append(GTK_BOX(cols), knobs);
	gtk_box_append(GTK_BOX(cols), v->master_meter);
	gtk_widget_set_vexpand(cols, TRUE);
	gtk_box_append(GTK_BOX(box), cols);
	return box;
}

static GtkWidget *build_xfader(PdMixerView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *l;

	l = gtk_label_new("A");
	gtk_widget_add_css_class(l, "section-label");
	gtk_box_append(GTK_BOX(box), l);
	v->xfader = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
					     -1.0, 1.0, 0.005);
	gtk_range_set_value(GTK_RANGE(v->xfader), 0.0);
	gtk_scale_set_draw_value(GTK_SCALE(v->xfader), FALSE);
	gtk_scale_set_has_origin(GTK_SCALE(v->xfader), FALSE);
	gtk_scale_add_mark(GTK_SCALE(v->xfader), 0.0, GTK_POS_BOTTOM, NULL);
	gtk_widget_set_hexpand(v->xfader, TRUE);
	gtk_widget_set_focusable(v->xfader, FALSE);
	gtk_widget_add_css_class(v->xfader, "crossfader");
	g_signal_connect(v->xfader, "value-changed", G_CALLBACK(on_xfader),
			 v);
	gtk_box_append(GTK_BOX(box), v->xfader);
	l = gtk_label_new("B");
	gtk_widget_add_css_class(l, "section-label");
	gtk_box_append(GTK_BOX(box), l);
	return box;
}

static void pd_mixer_view_dispose(GObject *obj)
{
	PdMixerView *v = PD_MIXER_VIEW(obj);

	if (v->tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(v), v->tick);
		v->tick = 0;
	}
	g_clear_pointer(&v->knobs, g_ptr_array_unref);
	G_OBJECT_CLASS(pd_mixer_view_parent_class)->dispose(obj);
}

static void pd_mixer_view_class_init(PdMixerViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_mixer_view_dispose;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "mixer");
}

static void pd_mixer_view_init(PdMixerView *v)
{
}

GtkWidget *pd_mixer_view_new(struct app *app)
{
	PdMixerView *v = g_object_new(PD_TYPE_MIXER_VIEW,
				      "orientation", GTK_ORIENTATION_VERTICAL,
				      "spacing", 6, NULL);
	GtkWidget *strips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
	int i;

	v->app = app;
	v->knobs = g_ptr_array_new();
	for (i = 0; i < ENGINE_DECKS; i++)
		v->strip[i] = build_strip(v, i);
	/* Channel order C A | master | B D, like a four channel mixer. */
	gtk_box_append(GTK_BOX(strips), v->strip[2]);
	gtk_box_append(GTK_BOX(strips), v->strip[0]);
	gtk_box_append(GTK_BOX(strips), build_master(v));
	gtk_box_append(GTK_BOX(strips), v->strip[1]);
	gtk_box_append(GTK_BOX(strips), v->strip[3]);
	gtk_widget_set_vexpand(strips, TRUE);
	gtk_box_append(GTK_BOX(v), strips);
	gtk_box_append(GTK_BOX(v), build_xfader(v));
	pd_mixer_view_set_decks(v, app->cfg.ndecks);

	v->tick = gtk_widget_add_tick_callback(GTK_WIDGET(v), tick, NULL,
					       NULL);
	return GTK_WIDGET(v);
}

void pd_mixer_view_nudge_xfader(PdMixerView *v, double delta)
{
	gtk_range_set_value(GTK_RANGE(v->xfader),
			    gtk_range_get_value(GTK_RANGE(v->xfader)) + delta);
}

void pd_mixer_view_set_xfader(PdMixerView *v, double value)
{
	gtk_range_set_value(GTK_RANGE(v->xfader), CLAMP(value, -1.0, 1.0));
}

void pd_mixer_view_set_decks(PdMixerView *v, int n)
{
	int i;

	for (i = 0; i < ENGINE_DECKS; i++)
		gtk_widget_set_visible(v->strip[i], i < n);
}

void pd_mixer_view_apply_config(PdMixerView *v)
{
	atomic_store(&v->app->engine.xf_curve, v->app->cfg.xf_curve);
}
