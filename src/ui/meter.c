// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * meter.c - stereo peak level meter
 */
#include <math.h>

#include "meter.h"

#define METER_W		14
#define DECAY		0.92f
#define HOLD_DECAY	0.985f
#define MIN_DB		-40.0f

struct _PdMeter {
	GtkWidget parent;
	float level[2];
	float hold[2];
};

G_DEFINE_FINAL_TYPE(PdMeter, pd_meter, GTK_TYPE_WIDGET)

static float to_frac(float lin)
{
	float db = 20.0f * log10f(lin > 1e-5f ? lin : 1e-5f);

	if (db < MIN_DB)
		return 0.0f;
	if (db > 0.0f)
		db = 0.0f;
	return (db - MIN_DB) / -MIN_DB;
}

static void meter_measure(GtkWidget *w, GtkOrientation o, int for_size,
			  int *min, int *nat, int *base_min, int *base_nat)
{
	if (o == GTK_ORIENTATION_HORIZONTAL)
		*min = *nat = METER_W;
	else {
		*min = 40;
		*nat = 120;
	}
}

static void meter_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
	PdMeter *m = PD_METER(w);
	float width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
	float bw = (width - 2) / 2.0f;
	int c, i;

	for (c = 0; c < 2; c++) {
		float x = c * (bw + 2);
		float frac = to_frac(m->level[c]);
		float hold = to_frac(m->hold[c]);
		graphene_rect_t r;
		GdkRGBA col;

		gdk_rgba_parse(&col, "#1d1f24");
		graphene_rect_init(&r, x, 0, bw, height);
		gtk_snapshot_append_color(snap, &col, &r);

		/* segments: green, amber above -12 dB, red above -3 dB */
		for (i = 0; i < 3; i++) {
			static const float bound[] = { 0.0f, 0.7f, 0.925f, 1.0f };
			static const char *const cols[] = {
				"#43c26d", "#e5b542", "#e0463b",
			};
			float lo = bound[i], hi = bound[i + 1];
			float top = height * (1.0f - (frac < hi ? frac : hi));
			float bot = height * (1.0f - lo);

			if (frac <= lo)
				continue;
			gdk_rgba_parse(&col, cols[i]);
			graphene_rect_init(&r, x, top, bw, bot - top);
			gtk_snapshot_append_color(snap, &col, &r);
		}

		if (hold > 0.01f) {
			gdk_rgba_parse(&col, hold > 0.925f ? "#ff6b5e" :
					     "#e8e8e8");
			graphene_rect_init(&r, x, height * (1.0f - hold) - 1,
					   bw, 2);
			gtk_snapshot_append_color(snap, &col, &r);
		}
	}
}

static void pd_meter_class_init(PdMeterClass *klass)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

	wc->measure = meter_measure;
	wc->snapshot = meter_snapshot;
	gtk_widget_class_set_css_name(wc, "meter");
}

static void pd_meter_init(PdMeter *m)
{
	gtk_widget_set_vexpand(GTK_WIDGET(m), TRUE);
}

GtkWidget *pd_meter_new(void)
{
	return g_object_new(PD_TYPE_METER, NULL);
}

void pd_meter_update(PdMeter *m, float left, float right)
{
	float in[2] = { left, right };
	int c;

	for (c = 0; c < 2; c++) {
		m->level[c] = in[c] > m->level[c] ? in[c] :
			      m->level[c] * DECAY;
		m->hold[c] = in[c] > m->hold[c] ? in[c] :
			     m->hold[c] * HOLD_DECAY;
	}
	gtk_widget_queue_draw(GTK_WIDGET(m));
}
