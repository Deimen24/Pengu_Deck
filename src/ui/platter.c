// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * platter.c - jog wheel
 *
 * One revolution is 1.8 s of audio (33⅓ rpm).  Dragging around the
 * centre scratches through the deck's scratch_target, so it behaves
 * exactly like dragging the waveform, only circular.
 */
#include <math.h>

#include "platter.h"

#define REV_SECONDS	1.8

struct _PdPlatter {
	GtkWidget parent;
	struct deck *deck;
	GdkRGBA color;
	int size;
	guint tick;
	double last_pos;
	gboolean last_playing;

	/* drag state */
	double drag_angle;
	double drag_pos;
	gboolean was_playing;
};

G_DEFINE_FINAL_TYPE(PdPlatter, pd_platter, GTK_TYPE_WIDGET)

static void platter_measure(GtkWidget *w, GtkOrientation o, int for_size,
			    int *min, int *nat, int *base_min, int *base_nat)
{
	*min = *nat = PD_PLATTER(w)->size;
}

static double angle_of(PdPlatter *p)
{
	struct track *t = deck_track(p->deck);

	if (!t)
		return 0.0;
	return fmod(deck_position(p->deck) / t->rate / REV_SECONDS, 1.0) *
	       2.0 * M_PI;
}

static void platter_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
	PdPlatter *p = PD_PLATTER(w);
	struct track *t = deck_track(p->deck);
	double s = p->size, c = s / 2.0, r = c - 3;
	double a = angle_of(p) - M_PI_2;
	double frac = 0.0;
	graphene_rect_t bounds;
	cairo_pattern_t *grad;
	cairo_t *cr;
	int i;

	graphene_rect_init(&bounds, 0, 0, (float)s, (float)s);
	cr = gtk_snapshot_append_cairo(snap, &bounds);

	/* outer ring in the deck colour, lit when playing */
	cairo_set_line_width(cr, 3.0);
	cairo_set_source_rgba(cr, p->color.red, p->color.green,
			      p->color.blue,
			      t && atomic_load(&p->deck->playing) ? 1.0 : 0.4);
	cairo_arc(cr, c, c, r, 0, 2 * M_PI);
	cairo_stroke(cr);

	/* progress arc */
	if (t && track_length(t) > 0)
		frac = deck_position(p->deck) / (double)track_length(t);
	cairo_set_line_width(cr, 5.0);
	cairo_set_source_rgba(cr, p->color.red, p->color.green,
			      p->color.blue, 0.85);
	cairo_arc(cr, c, c, r - 6, -M_PI_2, -M_PI_2 + frac * 2 * M_PI);
	cairo_stroke(cr);

	/* vinyl body */
	grad = cairo_pattern_create_radial(c - r * 0.3, c - r * 0.3, r * 0.1,
					   c, c, r);
	cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.22, 0.24, 0.28);
	cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.08, 0.09, 0.11);
	cairo_set_source(cr, grad);
	cairo_arc(cr, c, c, r - 11, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(grad);

	/* grooves */
	cairo_set_line_width(cr, 1.0);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
	for (i = 0; i < 5; i++) {
		cairo_arc(cr, c, c, (r - 14) * (0.55 + 0.09 * i), 0, 2 * M_PI);
		cairo_stroke(cr);
	}

	/* label with a position marker that rotates */
	cairo_save(cr);
	cairo_translate(cr, c, c);
	cairo_rotate(cr, a);
	cairo_set_source_rgba(cr, p->color.red, p->color.green,
			      p->color.blue, 0.25);
	cairo_arc(cr, 0, 0, r * 0.42, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, 3.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
	cairo_move_to(cr, r * 0.12, 0);
	cairo_line_to(cr, r * 0.40, 0);
	cairo_stroke(cr);
	cairo_restore(cr);

	/* spindle */
	cairo_set_source_rgb(cr, 0.85, 0.87, 0.9);
	cairo_arc(cr, c, c, 3.5, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_destroy(cr);
}

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	PdPlatter *p = PD_PLATTER(w);
	double pos = deck_position(p->deck);
	gboolean playing = atomic_load(&p->deck->playing);

	if (pos != p->last_pos || playing != p->last_playing) {
		p->last_pos = pos;
		p->last_playing = playing;
		gtk_widget_queue_draw(w);
	}
	return G_SOURCE_CONTINUE;
}

static double pointer_angle(PdPlatter *p, double x, double y)
{
	return atan2(y - p->size / 2.0, x - p->size / 2.0);
}

static void drag_begin(GtkGestureDrag *g, double x, double y, PdPlatter *p)
{
	struct deck *d = p->deck;

	if (!deck_track(d))
		return;
	p->drag_angle = pointer_angle(p, x, y);
	p->drag_pos = deck_position(d);
	p->was_playing = atomic_load(&d->playing);
	atomic_store(&d->scratch_target, p->drag_pos);
	atomic_store(&d->scratch, true);
}

static void drag_update(GtkGestureDrag *g, double dx, double dy,
			PdPlatter *p)
{
	struct deck *d = p->deck;
	struct track *t = deck_track(d);
	double sx, sy, a, delta;

	if (!t)
		return;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	a = pointer_angle(p, sx + dx, sy + dy);
	delta = a - p->drag_angle;
	/* unwrap so a full turn accumulates instead of jumping back */
	while (delta > M_PI)
		delta -= 2 * M_PI;
	while (delta < -M_PI)
		delta += 2 * M_PI;
	p->drag_angle = a;
	p->drag_pos += delta / (2 * M_PI) * REV_SECONDS * t->rate;
	if (p->drag_pos < 0)
		p->drag_pos = 0;
	atomic_store(&d->scratch_target, p->drag_pos);
}

static void drag_end(GtkGestureDrag *g, double dx, double dy, PdPlatter *p)
{
	struct deck *d = p->deck;

	if (!deck_track(d))
		return;
	atomic_store(&d->scratch, false);
	atomic_store(&d->playing, p->was_playing);
}

static void pd_platter_dispose(GObject *obj)
{
	PdPlatter *p = PD_PLATTER(obj);

	if (p->tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(p), p->tick);
		p->tick = 0;
	}
	G_OBJECT_CLASS(pd_platter_parent_class)->dispose(obj);
}

static void pd_platter_class_init(PdPlatterClass *klass)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

	G_OBJECT_CLASS(klass)->dispose = pd_platter_dispose;
	wc->measure = platter_measure;
	wc->snapshot = platter_snapshot;
	gtk_widget_class_set_css_name(wc, "platter");
}

static void pd_platter_init(PdPlatter *p)
{
	p->last_pos = -1.0;
}

GtkWidget *pd_platter_new(struct deck *d, const char *color, int size)
{
	PdPlatter *p = g_object_new(PD_TYPE_PLATTER, NULL);
	GtkGesture *drag = gtk_gesture_drag_new();

	p->deck = d;
	p->size = size;
	gdk_rgba_parse(&p->color, color);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), p);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), p);
	g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), p);
	gtk_widget_add_controller(GTK_WIDGET(p), GTK_EVENT_CONTROLLER(drag));
	gtk_widget_set_cursor_from_name(GTK_WIDGET(p), "grab");
	gtk_widget_set_tooltip_text(GTK_WIDGET(p), "Jog wheel: drag to "
				    "scratch or nudge");
	gtk_widget_set_valign(GTK_WIDGET(p), GTK_ALIGN_CENTER);
	p->tick = gtk_widget_add_tick_callback(GTK_WIDGET(p), tick, NULL,
					       NULL);
	return GTK_WIDGET(p);
}
