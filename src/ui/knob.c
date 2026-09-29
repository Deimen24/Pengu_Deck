// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * knob.c - rotary control
 */
#include <math.h>

#include "knob.h"

#define KNOB_SIZE	44
#define LABEL_H		14
#define DRAG_RANGE	160.0	/* pixels for the full range */
#define SWEEP		(1.5 * M_PI)

struct _PdKnob {
	GtkWidget parent;
	char *label;
	double min, max, def, value;
	double drag_start;
	gboolean detent;
	gboolean bipolar;
	GdkRGBA accent;
};

enum {
	SIG_CHANGED,
	N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PdKnob, pd_knob, GTK_TYPE_WIDGET)

static void set_value_internal(PdKnob *k, double v, gboolean notify)
{
	if (v < k->min)
		v = k->min;
	if (v > k->max)
		v = k->max;
	if (k->detent && fabs(v - k->def) < (k->max - k->min) * 0.03)
		v = k->def;
	if (v == k->value)
		return;
	k->value = v;
	gtk_widget_queue_draw(GTK_WIDGET(k));
	if (notify)
		g_signal_emit(k, signals[SIG_CHANGED], 0);
}

static void knob_measure(GtkWidget *w, GtkOrientation o, int for_size,
			 int *min, int *nat, int *base_min, int *base_nat)
{
	*min = *nat = o == GTK_ORIENTATION_HORIZONTAL ?
		      KNOB_SIZE : KNOB_SIZE + LABEL_H;
}

static void knob_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
	PdKnob *k = PD_KNOB(w);
	int width = gtk_widget_get_width(w);
	double cx = width / 2.0, cy = KNOB_SIZE / 2.0, r = KNOB_SIZE / 2.0 - 4;
	double frac = (k->value - k->min) / (k->max - k->min);
	double a0 = 0.75 * M_PI, a = a0 + frac * SWEEP;
	double zero = a0 + (k->def - k->min) / (k->max - k->min) * SWEEP;
	graphene_rect_t bounds;
	PangoLayout *layout;
	cairo_t *cr;
	GdkRGBA fg, accent;
	int tw, th;

	graphene_rect_init(&bounds, 0, 0, width, KNOB_SIZE + LABEL_H);
	cr = gtk_snapshot_append_cairo(snap, &bounds);
	gtk_widget_get_color(w, &fg);
	accent = k->accent;

	/* track */
	cairo_set_line_width(cr, 3.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.2);
	cairo_arc(cr, cx, cy, r, a0, a0 + SWEEP);
	cairo_stroke(cr);

	/* value arc, from the default position for bipolar knobs */
	gdk_cairo_set_source_rgba(cr, &accent);
	if (k->bipolar) {
		if (a >= zero)
			cairo_arc(cr, cx, cy, r, zero, a);
		else
			cairo_arc_negative(cr, cx, cy, r, zero, a);
	} else {
		cairo_arc(cr, cx, cy, r, a0, a);
	}
	cairo_stroke(cr);

	/* body */
	cairo_set_source_rgb(cr, 0.16, 0.17, 0.19);
	cairo_arc(cr, cx, cy, r - 5, 0, 2 * M_PI);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	/* pointer */
	cairo_set_line_width(cr, 2.5);
	gdk_cairo_set_source_rgba(cr, &fg);
	cairo_move_to(cr, cx + cos(a) * (r - 12), cy + sin(a) * (r - 12));
	cairo_line_to(cr, cx + cos(a) * (r - 5), cy + sin(a) * (r - 5));
	cairo_stroke(cr);

	/* label */
	if (k->label) {
		layout = gtk_widget_create_pango_layout(w, k->label);
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_move_to(cr, cx - tw / 2.0, KNOB_SIZE);
		cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.7);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}
	cairo_destroy(cr);
}

static void drag_begin(GtkGestureDrag *g, double x, double y, PdKnob *k)
{
	k->drag_start = k->value;
}

static void drag_update(GtkGestureDrag *g, double dx, double dy, PdKnob *k)
{
	GdkModifierType mod = gtk_event_controller_get_current_event_state(
					GTK_EVENT_CONTROLLER(g));
	double range = k->max - k->min;
	double scale = mod & GDK_SHIFT_MASK ? 8.0 : 1.0;

	set_value_internal(k, k->drag_start - dy / (DRAG_RANGE * scale) *
			   range, TRUE);
}

static void reset(GtkGestureClick *g, int n, double x, double y, PdKnob *k)
{
	if (n == 2)
		set_value_internal(k, k->def, TRUE);
}

static gboolean scroll(GtkEventControllerScroll *c, double dx, double dy,
		       PdKnob *k)
{
	set_value_internal(k, k->value - dy * (k->max - k->min) / 40.0,
			   TRUE);
	return TRUE;
}

static void pd_knob_finalize(GObject *obj)
{
	g_free(PD_KNOB(obj)->label);
	G_OBJECT_CLASS(pd_knob_parent_class)->finalize(obj);
}

static void pd_knob_class_init(PdKnobClass *klass)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = pd_knob_finalize;
	wc->measure = knob_measure;
	wc->snapshot = knob_snapshot;
	gtk_widget_class_set_css_name(wc, "knob");
	signals[SIG_CHANGED] = g_signal_new("changed", PD_TYPE_KNOB,
					    G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					    NULL, G_TYPE_NONE, 0);
}

static void pd_knob_init(PdKnob *k)
{
	GtkGesture *drag = gtk_gesture_drag_new();
	GtkGesture *click = gtk_gesture_click_new();
	GtkEventController *sc = gtk_event_controller_scroll_new(
			GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
			GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);

	g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), k);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), k);
	g_signal_connect(click, "pressed", G_CALLBACK(reset), k);
	g_signal_connect(sc, "scroll", G_CALLBACK(scroll), k);
	gtk_widget_add_controller(GTK_WIDGET(k), GTK_EVENT_CONTROLLER(drag));
	gtk_widget_add_controller(GTK_WIDGET(k), GTK_EVENT_CONTROLLER(click));
	gtk_widget_add_controller(GTK_WIDGET(k), sc);
	gtk_widget_set_cursor_from_name(GTK_WIDGET(k), "ns-resize");
}

GtkWidget *pd_knob_new(const char *label, double min, double max, double def)
{
	PdKnob *k = g_object_new(PD_TYPE_KNOB, NULL);

	k->label = g_strdup(label);
	k->min = min;
	k->max = max;
	k->def = def;
	k->value = def;
	k->bipolar = def > min && def < max;
	k->detent = k->bipolar;
	gdk_rgba_parse(&k->accent, "#4fc3f7");
	gtk_widget_set_tooltip_text(GTK_WIDGET(k), label);
	return GTK_WIDGET(k);
}

double pd_knob_get_value(PdKnob *k)
{
	return k->value;
}

void pd_knob_set_value(PdKnob *k, double v)
{
	set_value_internal(k, v, FALSE);
}

void pd_knob_set_detent(PdKnob *k, gboolean on)
{
	k->detent = on;
}

void pd_knob_set_accent(PdKnob *k, const char *hex)
{
	gdk_rgba_parse(&k->accent, hex);
	gtk_widget_queue_draw(GTK_WIDGET(k));
}
