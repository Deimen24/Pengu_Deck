// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * mixerview.c - mixer section
 */
#include <math.h>

#include "knob.h"
#include "meter.h"
#include "mixerview.h"

struct _PdMixerView {
	GtkBox parent;
	struct app *app;

	GtkWidget *meter[2];
	GtkWidget *master_meter;
	GtkWidget *xfader;
	GtkWidget *pfl[2];
	GtkWidget *record;
	GtkWidget *record_label;
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
			   double def, _Atomic float *target)
{
	GtkWidget *k = pd_knob_new(label, min, max, def);

	g_signal_connect(k, "changed", G_CALLBACK(on_knob_float), target);
	return k;
}

static void on_kill(GtkToggleButton *b, gpointer data)
{
	atomic_bool *target = data;

	atomic_store(target, gtk_toggle_button_get_active(b));
}

static void on_fader(GtkRange *r, gpointer data)
{
	_Atomic float *target = data;

	atomic_store(target, (float)gtk_range_get_value(r));
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

	for (i = 0; i < 2; i++) {
		struct deck *d = &e->deck[i];

		pd_meter_update(PD_METER(v->meter[i]),
				atomic_exchange(&d->peak_l, 0.0f),
				atomic_exchange(&d->peak_r, 0.0f));
	}
	pd_meter_update(PD_METER(v->master_meter),
			atomic_exchange(&e->peak_l, 0.0f),
			atomic_exchange(&e->peak_r, 0.0f));

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
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *knobs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *row, *fader, *b, *k;
	static const char *const names[EQ_BANDS] = { "LOW", "MID", "HIGH" };
	int band;

	gtk_widget_add_css_class(box, "strip");
	gtk_box_append(GTK_BOX(box), section_label(i == 0 ? "A" : "B"));
	gtk_box_append(GTK_BOX(knobs),
		       knob_for("TRIM", -12.0, 12.0, 0.0, &d->trim_db));

	for (band = EQ_BANDS - 1; band >= 0; band--) {
		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		k = knob_for(names[band], -26.0, 9.0, 0.0, &d->eq_db[band]);
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

	k = knob_for("FILTER", -1.0, 1.0, 0.0, &d->filter);
	gtk_widget_set_tooltip_text(k, "Left: low pass, right: high pass");
	gtk_box_append(GTK_BOX(knobs), k);

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
	v->meter[i] = pd_meter_new();
	gtk_widget_set_margin_top(v->meter[i], 12);
	gtk_widget_set_margin_bottom(v->meter[i], 12);

	/* Knobs on the outside, fader and meter towards the master. */
	if (i == 0) {
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
	k = knob_for("LEVEL", 0.0, 1.5, 1.0, &e->master);
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	gtk_box_append(GTK_BOX(knobs), k);

	gtk_box_append(GTK_BOX(knobs), section_label("CUE"));
	k = knob_for("MIX", 0.0, 1.0, 0.0, &e->cue_mix);
	gtk_widget_set_tooltip_text(k, "Headphone mix: cue ↔ master");
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	gtk_box_append(GTK_BOX(knobs), k);
	k = knob_for("VOL", 0.0, 1.5, 1.0, &e->cue_vol);
	pd_knob_set_detent(PD_KNOB(k), FALSE);
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
	GtkWidget *strips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	v->app = app;
	gtk_box_append(GTK_BOX(strips), build_strip(v, 0));
	gtk_box_append(GTK_BOX(strips), build_master(v));
	gtk_box_append(GTK_BOX(strips), build_strip(v, 1));
	gtk_widget_set_vexpand(strips, TRUE);
	gtk_box_append(GTK_BOX(v), strips);
	gtk_box_append(GTK_BOX(v), build_xfader(v));

	v->tick = gtk_widget_add_tick_callback(GTK_WIDGET(v), tick, NULL,
					       NULL);
	return GTK_WIDGET(v);
}

void pd_mixer_view_nudge_xfader(PdMixerView *v, double delta)
{
	gtk_range_set_value(GTK_RANGE(v->xfader),
			    gtk_range_get_value(GTK_RANGE(v->xfader)) + delta);
}

void pd_mixer_view_apply_config(PdMixerView *v)
{
	atomic_store(&v->app->engine.xf_curve, v->app->cfg.xf_curve);
}
