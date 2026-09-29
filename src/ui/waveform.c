// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * waveform.c - scrolling coloured waveform
 */
#include <math.h>

#include "waveform.h"

#define ZOOM_DEFAULT	120.0
#define ZOOM_MIN	20.0
#define ZOOM_MAX	1200.0
#define ZOOMED_H	110
#define OVERVIEW_H	46

struct _PdWaveform {
	GtkWidget parent;
	struct deck *deck;
	gboolean overview;
	double zoom;		/* pixels per second */
	guint tick;
	double last_pos;
	gboolean last_ready;
	GdkRGBA accent;

	/* drag state */
	double drag_pos;
	gboolean was_playing;
};

G_DEFINE_FINAL_TYPE(PdWaveform, pd_waveform, GTK_TYPE_WIDGET)

static void wave_measure(GtkWidget *w, GtkOrientation o, int for_size,
			 int *min, int *nat, int *base_min, int *base_nat)
{
	PdWaveform *wf = PD_WAVEFORM(w);

	if (o == GTK_ORIENTATION_HORIZONTAL) {
		*min = 200;
		*nat = 420;
	} else {
		*min = *nat = wf->overview ? OVERVIEW_H : ZOOMED_H;
	}
}

static void fill(GtkSnapshot *snap, const char *hex, float alpha,
		 float x, float y, float w, float h)
{
	graphene_rect_t r;
	GdkRGBA c;

	if (w <= 0 || h <= 0)
		return;
	gdk_rgba_parse(&c, hex);
	c.alpha = alpha;
	graphene_rect_init(&r, x, y, w, h);
	gtk_snapshot_append_color(snap, &c, &r);
}

/* Draw one column of the three band waveform, bins [b0, b1). */
static void draw_column(GtkSnapshot *snap, const struct track *t,
			size_t b0, size_t b1, float x, float w, float mid,
			float half)
{
	unsigned int lo = 0, mi = 0, hi = 0;
	size_t b;
	float hl, hm, hh;

	for (b = b0; b < b1; b++) {
		const struct wave_bin *bin = track_bin(t, b);

		if (bin->low > lo)
			lo = bin->low;
		if (bin->mid > mi)
			mi = bin->mid;
		if (bin->high > hi)
			hi = bin->high;
	}
	hl = half * lo / 255.0f;
	hm = half * mi / 255.0f;
	hh = half * hi / 255.0f;
	fill(snap, "#3d7ef5", 0.95f, x, mid - hl, w, 2 * hl);
	fill(snap, "#e9a03b", 0.85f, x, mid - hm, w, 2 * hm);
	fill(snap, "#f3f3f3", 0.75f, x, mid - hh, w, 2 * hh);
}

static void draw_markers(PdWaveform *wf, GtkSnapshot *snap,
			 double frame_at_x0, double frames_per_px, float h)
{
	struct deck *d = wf->deck;
	struct track *t = deck_track(d);
	double lin = atomic_load(&d->loop_in), lout = atomic_load(&d->loop_out);
	gboolean loop = atomic_load(&d->loop_on);
	float x;
	int i;

	if (lout > lin) {
		x = (float)((lin - frame_at_x0) / frames_per_px);
		fill(snap, "#7bd88f", loop ? 0.22f : 0.1f, x, 0,
		     (float)((lout - lin) / frames_per_px), h);
	}
	x = (float)((d->cue - frame_at_x0) / frames_per_px);
	fill(snap, "#ffa726", 1.0f, x - 1, 0, 2, h);
	for (i = 0; i < DECK_HOTCUES; i++) {
		if (d->hotcue[i] < 0.0)
			continue;
		x = (float)((d->hotcue[i] - frame_at_x0) / frames_per_px);
		fill(snap, "#ef5da8", 1.0f, x - 1, 0, 2, h);
		fill(snap, "#ef5da8", 1.0f, x - 1, 0, 10, 10);
	}
	(void)t;
}

static void draw_beats(PdWaveform *wf, GtkSnapshot *snap, struct track *t,
		       double frame_at_x0, double frames_per_px, float w,
		       float h)
{
	double beat = track_beat_len(t);
	double off = atomic_load(&t->beat_offset);
	double first, f;
	long n;

	if (beat <= 0.0 || beat / frames_per_px < 4.0)
		return;
	first = off + floor((frame_at_x0 - off) / beat) * beat;
	n = lround((first - off) / beat);
	for (f = first; (f - frame_at_x0) / frames_per_px < w; f += beat, n++) {
		float x = (float)((f - frame_at_x0) / frames_per_px);
		gboolean bar = ((n % 4) + 4) % 4 == 0;

		fill(snap, "#ffffff", bar ? 0.5f : 0.18f, x, 0, 1, h);
	}
}

static void wave_snapshot(GtkWidget *w, GtkSnapshot *snap)
{
	PdWaveform *wf = PD_WAVEFORM(w);
	struct deck *d = wf->deck;
	struct track *t = deck_track(d);
	float width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
	float mid = height / 2.0f, half = mid - 2;
	double pos = deck_position(d);
	double frames_per_px, frame_at_x0, playhead_x;
	size_t bins, len;
	float x;

	fill(snap, "#101215", 1.0f, 0, 0, width, height);
	if (!t) {
		fill(snap, "#ffffff", 0.1f, 0, mid, width, 1);
		return;
	}
	bins = track_bins(t);
	len = track_length(t);

	if (wf->overview) {
		frames_per_px = (double)(len > 0 ? len : 1) / width;
		frame_at_x0 = 0.0;
		playhead_x = pos / frames_per_px;
	} else {
		frames_per_px = t->rate / wf->zoom;
		playhead_x = width / 2.0;
		frame_at_x0 = pos - playhead_x * frames_per_px;
	}

	/* the waveform itself, one column per pixel */
	for (x = 0; x < width; x += 1.0f) {
		double f0 = frame_at_x0 + x * frames_per_px;
		double f1 = f0 + frames_per_px;
		size_t b0, b1;

		if (f1 <= 0.0)
			continue;
		if (f0 < 0.0)
			f0 = 0.0;
		b0 = (size_t)f0 >> WAVE_BIN_SHIFT;
		b1 = ((size_t)f1 >> WAVE_BIN_SHIFT) + 1;
		if (b0 >= bins)
			break;
		if (b1 > bins)
			b1 = bins;
		draw_column(snap, t, b0, b1, x, 1.0f, mid, half);
	}

	/* Loading progress on the overview. */
	if (wf->overview && !track_done(t) && len > 0) {
		float lx = (float)(track_frames(t) / frames_per_px);

		fill(snap, "#ffffff", 0.06f, lx, 0, width - lx, height);
	}

	if (!wf->overview)
		draw_beats(wf, snap, t, frame_at_x0, frames_per_px, width,
			   height);
	draw_markers(wf, snap, frame_at_x0, frames_per_px, height);

	/* playhead */
	fill(snap, "#ffffff", 1.0f, (float)playhead_x - 1, 0, 2, height);
	if (!wf->overview) {
		fill(snap, "#ffffff", 0.35f, (float)playhead_x - 3, 0, 6, 6);
		fill(snap, "#ffffff", 0.35f, (float)playhead_x - 3, height - 6,
		     6, 6);
	}
}

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	PdWaveform *wf = PD_WAVEFORM(w);
	struct track *t = deck_track(wf->deck);
	double pos = deck_position(wf->deck);
	gboolean ready = t && track_done(t) && atomic_load(&t->analysed);

	/*
	 * Redraw while the position moves, while the track is still
	 * loading or analysing, and once more when that finishes so the
	 * beat grid appears on a paused deck too.
	 */
	if (pos != wf->last_pos || !ready || !wf->last_ready) {
		wf->last_pos = pos;
		wf->last_ready = ready;
		gtk_widget_queue_draw(w);
	}
	return G_SOURCE_CONTINUE;
}

/* ---- interaction ------------------------------------------------- */

static void overview_press(GtkGestureClick *g, int n, double x, double y,
			   PdWaveform *wf)
{
	struct track *t = deck_track(wf->deck);
	double width = gtk_widget_get_width(GTK_WIDGET(wf));
	size_t len;

	if (!t)
		return;
	len = track_length(t);
	deck_seek(wf->deck, x / width * (double)len);
}

static void zoom_drag_begin(GtkGestureDrag *g, double x, double y,
			    PdWaveform *wf)
{
	struct deck *d = wf->deck;

	if (!deck_track(d))
		return;
	wf->drag_pos = deck_position(d);
	wf->was_playing = atomic_load(&d->playing);
	atomic_store(&d->scratch_target, wf->drag_pos);
	atomic_store(&d->scratch, true);
}

static void zoom_drag_update(GtkGestureDrag *g, double dx, double dy,
			     PdWaveform *wf)
{
	struct deck *d = wf->deck;
	struct track *t = deck_track(d);

	if (!t)
		return;
	atomic_store(&d->scratch_target,
		     wf->drag_pos - dx * t->rate / wf->zoom);
}

static void zoom_drag_end(GtkGestureDrag *g, double dx, double dy,
			  PdWaveform *wf)
{
	struct deck *d = wf->deck;

	if (!deck_track(d))
		return;
	atomic_store(&d->scratch, false);
	atomic_store(&d->playing, wf->was_playing);
}

static gboolean zoom_scroll(GtkEventControllerScroll *c, double dx,
			    double dy, PdWaveform *wf)
{
	GdkModifierType mod = gtk_event_controller_get_current_event_state(
					GTK_EVENT_CONTROLLER(c));
	struct track *t = deck_track(wf->deck);

	if (mod & GDK_CONTROL_MASK) {
		pd_waveform_zoom_by(wf, dy > 0 ? 0.8 : 1.25);
		return TRUE;
	}
	if (t)
		deck_seek(wf->deck, deck_position(wf->deck) +
			  dy * t->rate * 0.5);
	return TRUE;
}

static void pd_waveform_dispose(GObject *obj)
{
	PdWaveform *wf = PD_WAVEFORM(obj);

	if (wf->tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(wf), wf->tick);
		wf->tick = 0;
	}
	G_OBJECT_CLASS(pd_waveform_parent_class)->dispose(obj);
}

static void pd_waveform_class_init(PdWaveformClass *klass)
{
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

	G_OBJECT_CLASS(klass)->dispose = pd_waveform_dispose;
	wc->measure = wave_measure;
	wc->snapshot = wave_snapshot;
	gtk_widget_class_set_css_name(wc, "waveform");
}

static void pd_waveform_init(PdWaveform *wf)
{
	wf->zoom = ZOOM_DEFAULT;
	wf->last_pos = -1.0;
	gtk_widget_set_hexpand(GTK_WIDGET(wf), TRUE);
	gtk_widget_set_overflow(GTK_WIDGET(wf), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *pd_waveform_new(struct deck *d, gboolean overview)
{
	PdWaveform *wf = g_object_new(PD_TYPE_WAVEFORM, NULL);
	GtkWidget *w = GTK_WIDGET(wf);

	wf->deck = d;
	wf->overview = overview;
	wf->tick = gtk_widget_add_tick_callback(w, tick, NULL, NULL);

	if (overview) {
		GtkGesture *click = gtk_gesture_click_new();

		g_signal_connect(click, "pressed", G_CALLBACK(overview_press),
				 wf);
		gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(click));
	} else {
		GtkGesture *drag = gtk_gesture_drag_new();
		GtkEventController *sc = gtk_event_controller_scroll_new(
				GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
				GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);

		g_signal_connect(drag, "drag-begin",
				 G_CALLBACK(zoom_drag_begin), wf);
		g_signal_connect(drag, "drag-update",
				 G_CALLBACK(zoom_drag_update), wf);
		g_signal_connect(drag, "drag-end", G_CALLBACK(zoom_drag_end),
				 wf);
		g_signal_connect(sc, "scroll", G_CALLBACK(zoom_scroll), wf);
		gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));
		gtk_widget_add_controller(w, sc);
		gtk_widget_set_cursor_from_name(w, "grab");
	}
	return w;
}

void pd_waveform_set_zoom(PdWaveform *wf, double px_per_sec)
{
	wf->zoom = CLAMP(px_per_sec, ZOOM_MIN, ZOOM_MAX);
	gtk_widget_queue_draw(GTK_WIDGET(wf));
}

void pd_waveform_zoom_by(PdWaveform *wf, double factor)
{
	pd_waveform_set_zoom(wf, wf->zoom * factor);
}

void pd_waveform_set_color(PdWaveform *wf, const char *hex)
{
	gdk_rgba_parse(&wf->accent, hex);
}
