// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * queueview.c - automix panel
 *
 * A status line with the two decks automix plays on, the transition
 * settings, and the queue: each row shows key, tempo and the pitch the
 * track would need to follow what is playing.  Rows are dragged to
 * reorder; tracks and files dropped anywhere are queued, dropped on a
 * row they go in at that spot.
 */
#include <math.h>

#include "analyze.h"
#include "automix.h"
#include "cuestore.h"
#include "library.h"
#include "queueview.h"

struct _PdQueueView {
	GtkBox parent;
	struct app *app;
	GtkWidget *list;
	GtkSingleSelection *selection;
	GtkWidget *toggle;
	GtkWidget *stack;
	GtkWidget *status;
	GtkWidget *now;
	GtkWidget *count;
	guint timer;
};

G_DEFINE_FINAL_TYPE(PdQueueView, pd_queue_view, GTK_TYPE_BOX)

/* ---- queue edits ------------------------------------------------- */

static guint queue_index(struct app *a, PdMediaItem *m)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(a->queue)), i;

	for (i = 0; i < n; i++) {
		PdMediaItem *x = g_list_model_get_item(G_LIST_MODEL(a->queue),
						       i);

		g_object_unref(x);
		if (x == m)
			return i;
	}
	return GTK_INVALID_LIST_POSITION;
}

/* Put @m at @pos: moved when it is queued already, else inserted. */
static void queue_place(struct app *a, PdMediaItem *m, guint pos)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(a->queue));
	guint cur = queue_index(a, m);

	g_object_ref(m);
	if (cur != GTK_INVALID_LIST_POSITION) {
		g_list_store_remove(a->queue, cur);
		n--;
		if (pos > cur)
			pos--;
	}
	g_list_store_insert(a->queue, MIN(pos, n), m);
	g_object_unref(m);
}

static void queue_files(struct app *a, GSList *files, guint pos)
{
	for (; files; files = files->next) {
		char *path = g_file_get_path(files->data);
		PdMediaItem *m = path ? library_probe_file(path) : NULL;

		if (m) {
			queue_place(a, m, pos++);
			g_object_unref(m);
		}
		g_free(path);
	}
}

/* Value from a drop: a track, or files from the file manager. */
static gboolean queue_drop_value(struct app *a, const GValue *val, guint pos)
{
	if (G_VALUE_HOLDS(val, PD_TYPE_MEDIA_ITEM)) {
		queue_place(a, g_value_get_object(val), pos);
		return TRUE;
	}
	if (G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) {
		queue_files(a, g_value_get_boxed(val), pos);
		return TRUE;
	}
	return FALSE;
}

/* ---- rows -------------------------------------------------------- */

struct row {
	GtkWidget *num;
	GtkWidget *title;
	GtkWidget *sub;
	GtkWidget *key;
	GtkWidget *bpm;
	GtkWidget *pitch;
	GtkWidget *len;
};

static void row_free(gpointer data)
{
	g_free(data);
}

static void set_number(GtkListItem *li, GtkWidget *num)
{
	guint pos = gtk_list_item_get_position(li);
	char *s = pos == GTK_INVALID_LIST_POSITION ? g_strdup("") :
		  g_strdup_printf("%u", pos + 1);

	gtk_label_set_text(GTK_LABEL(num), s);
	g_free(s);
}

static void on_position(GtkListItem *li, GParamSpec *ps, GtkWidget *num)
{
	set_number(li, num);
}

/* The pitch the track needs to follow what plays: the mixability at a glance. */
static void bind_pitch(struct row *r, PdMediaItem *m)
{
	double p = automix_pitch_for(m);
	double range = 0.0;
	char *s;

	gtk_widget_remove_css_class(r->pitch, "far");
	gtk_widget_remove_css_class(r->pitch, "near");
	if (isnan(p)) {
		gtk_label_set_text(GTK_LABEL(r->pitch), "");
		return;
	}
	s = g_strdup_printf("%+.1f%%", p * 100.0);
	gtk_label_set_text(GTK_LABEL(r->pitch), s);
	g_free(s);
	{
		PdQueueView *v = g_object_get_data(G_OBJECT(r->pitch), "view");

		if (v)
			range = v->app->cfg.pitch_range / 100.0;
	}
	gtk_widget_add_css_class(r->pitch, fabs(p) <= range ? "near" :
				 fabs(p) <= 2.0 * range ? "" : "far");
}

static void bind_row_state(GtkListItem *li)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	struct row *r = g_object_get_data(G_OBJECT(li), "row");

	if (m && r)
		bind_pitch(r, m);
}

static void on_item_changed(GObject *watch, const char *key, GtkListItem *li)
{
	PdMediaItem *m = gtk_list_item_get_item(li);

	if (m && (g_str_equal(key, "*") || g_strcmp0(key, m->key) == 0))
		bind_row_state(li);
}

/* ---- row drag and drop ------------------------------------------- */

static GdkContentProvider *row_drag_prepare(GtkDragSource *src, double x,
					    double y, GtkListItem *li)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	GValue val = G_VALUE_INIT;
	GdkContentProvider *p;

	if (!m)
		return NULL;
	g_value_init(&val, PD_TYPE_MEDIA_ITEM);
	g_value_set_object(&val, m);
	p = gdk_content_provider_new_for_value(&val);
	g_value_unset(&val);
	return p;
}

static void row_drag_begin(GtkDragSource *src, GdkDrag *drag, GtkListItem *li)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	GtkWidget *l;
	GdkPaintable *icon;

	if (!m)
		return;
	l = gtk_label_new(m->title);
	gtk_widget_add_css_class(l, "drag-label");
	g_object_ref_sink(l);
	icon = gtk_widget_paintable_new(l);
	gtk_drag_source_set_icon(src, icon, 0, 0);
	g_object_unref(icon);
	g_object_set_data_full(G_OBJECT(drag), "drag-widget", l,
			       g_object_unref);
}

static gboolean row_drop(GtkDropTarget *t, const GValue *val, double x,
			 double y, GtkListItem *li)
{
	PdQueueView *v = g_object_get_data(G_OBJECT(t), "view");
	guint pos = gtk_list_item_get_position(li);
	GtkWidget *row = gtk_list_item_get_child(li);

	gtk_widget_remove_css_class(row, "drop-here");
	if (pos == GTK_INVALID_LIST_POSITION)
		return FALSE;
	/* below the middle of the row means after it */
	if (y > gtk_widget_get_height(row) / 2.0)
		pos++;
	return queue_drop_value(v->app, val, pos);
}

static GdkDragAction row_drop_enter(GtkDropTarget *t, double x, double y,
				    GtkListItem *li)
{
	gtk_widget_add_css_class(gtk_list_item_get_child(li), "drop-here");
	return GDK_ACTION_COPY;
}

static void row_drop_leave(GtkDropTarget *t, GtkListItem *li)
{
	gtk_widget_remove_css_class(gtk_list_item_get_child(li), "drop-here");
}

static GtkWidget *mono(const char *css, float xalign, int chars)
{
	GtkWidget *l = gtk_label_new("");

	gtk_widget_add_css_class(l, "mono");
	if (css)
		gtk_widget_add_css_class(l, css);
	gtk_label_set_xalign(GTK_LABEL(l), xalign);
	if (chars)
		gtk_label_set_width_chars(GTK_LABEL(l), chars);
	gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
	return l;
}

static void setup_row(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	PdQueueView *v = data;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	struct row *r = g_new0(struct row, 1);
	GtkDragSource *src = gtk_drag_source_new();
	GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INVALID,
						  GDK_ACTION_COPY);
	GType types[] = { PD_TYPE_MEDIA_ITEM, GDK_TYPE_FILE_LIST };

	r->num = mono("queue-number", 1.0f, 3);
	r->title = gtk_label_new("");
	gtk_widget_add_css_class(r->title, "row-title");
	gtk_label_set_xalign(GTK_LABEL(r->title), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_END);
	r->sub = gtk_label_new("");
	gtk_widget_add_css_class(r->sub, "row-sub");
	gtk_label_set_xalign(GTK_LABEL(r->sub), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(r->sub), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(text), r->title);
	gtk_box_append(GTK_BOX(text), r->sub);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

	r->key = gtk_label_new("");
	gtk_widget_add_css_class(r->key, "pill");
	gtk_widget_add_css_class(r->key, "pill-key");
	gtk_widget_set_valign(r->key, GTK_ALIGN_CENTER);
	r->bpm = gtk_label_new("");
	gtk_widget_add_css_class(r->bpm, "pill");
	gtk_widget_add_css_class(r->bpm, "pill-bpm");
	gtk_widget_set_valign(r->bpm, GTK_ALIGN_CENTER);
	r->pitch = mono("queue-pitch", 1.0f, 7);
	gtk_widget_set_tooltip_text(r->pitch, "Pitch this track needs to "
				    "follow the playing deck");
	g_object_set_data(G_OBJECT(r->pitch), "view", v);
	r->len = mono("row-len", 1.0f, 6);
	gtk_widget_add_css_class(r->len, "dim-label");

	gtk_box_append(GTK_BOX(box), r->num);
	gtk_box_append(GTK_BOX(box), text);
	gtk_box_append(GTK_BOX(box), r->key);
	gtk_box_append(GTK_BOX(box), r->bpm);
	gtk_box_append(GTK_BOX(box), r->pitch);
	gtk_box_append(GTK_BOX(box), r->len);
	gtk_widget_add_css_class(box, "queue-row");
	gtk_list_item_set_child(li, box);
	g_object_set_data_full(G_OBJECT(li), "row", r, row_free);
	/* rows keep their item when earlier ones go, only the position moves */
	g_signal_connect(li, "notify::position", G_CALLBACK(on_position),
			 r->num);
	g_signal_connect_object(pd_media_watch(), "changed",
				G_CALLBACK(on_item_changed), li, 0);

	gtk_drag_source_set_actions(src, GDK_ACTION_COPY);
	g_signal_connect(src, "prepare", G_CALLBACK(row_drag_prepare), li);
	g_signal_connect(src, "drag-begin", G_CALLBACK(row_drag_begin), li);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(src));

	gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
	g_object_set_data(G_OBJECT(drop), "view", v);
	g_signal_connect(drop, "drop", G_CALLBACK(row_drop), li);
	g_signal_connect(drop, "enter", G_CALLBACK(row_drop_enter), li);
	g_signal_connect(drop, "leave", G_CALLBACK(row_drop_leave), li);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
}

static void bind_row(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	struct row *r = g_object_get_data(G_OBJECT(li), "row");
	double bpm;
	int key;
	char *s;

	set_number(li, r->num);
	gtk_label_set_text(GTK_LABEL(r->title), m->title ? m->title : "");
	gtk_label_set_text(GTK_LABEL(r->sub), m->artist && *m->artist ?
			   m->artist : "Unknown artist");

	key = cuestore_key(m->key);
	gtk_widget_set_visible(r->key, key >= 0);
	if (key >= 0) {
		int n = atoi(key_camelot(key));
		char cls[16];

		gtk_label_set_text(GTK_LABEL(r->key), key_camelot(key));
		g_snprintf(cls, sizeof(cls), "camelot-%d", n);
		gtk_widget_add_css_class(r->key, cls);
	}
	bpm = m->bpm > 0.0 ? m->bpm : cuestore_bpm(m->key);
	gtk_widget_set_visible(r->bpm, bpm > 0.0);
	s = g_strdup_printf("%.0f", bpm);
	gtk_label_set_text(GTK_LABEL(r->bpm), s);
	g_free(s);
	s = format_duration(m->duration);
	gtk_label_set_text(GTK_LABEL(r->len), s);
	g_free(s);
	bind_pitch(r, m);
}

static void unbind_row(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	struct row *r = g_object_get_data(G_OBJECT(li), "row");
	int n;

	/* the Camelot colour class must not carry over to the next item */
	for (n = 1; n <= 12; n++) {
		char cls[16];

		g_snprintf(cls, sizeof(cls), "camelot-%d", n);
		gtk_widget_remove_css_class(r->key, cls);
	}
}

/* ---- controls ---------------------------------------------------- */

static void on_toggle(GtkToggleButton *b, PdQueueView *v)
{
	automix_set_enabled(gtk_toggle_button_get_active(b));
}

static void on_next(GtkButton *b, PdQueueView *v)
{
	automix_next();
}

static void on_remove(GtkButton *b, PdQueueView *v)
{
	guint i = gtk_single_selection_get_selected(v->selection);

	if (i != GTK_INVALID_LIST_POSITION)
		g_list_store_remove(v->app->queue, i);
}

static void on_clear(GtkButton *b, PdQueueView *v)
{
	g_list_store_remove_all(v->app->queue);
}

static void move_selected(PdQueueView *v, int dir)
{
	guint i = gtk_single_selection_get_selected(v->selection);
	guint n = g_list_model_get_n_items(G_LIST_MODEL(v->app->queue));
	PdMediaItem *m;

	if (i == GTK_INVALID_LIST_POSITION)
		return;
	if ((dir < 0 && i == 0) || (dir > 0 && i + 1 >= n))
		return;
	m = g_list_model_get_item(G_LIST_MODEL(v->app->queue), i);
	g_list_store_remove(v->app->queue, i);
	g_list_store_insert(v->app->queue, i + dir, m);
	gtk_single_selection_set_selected(v->selection, i + dir);
	g_object_unref(m);
}

static void on_up(GtkButton *b, PdQueueView *v)
{
	move_selected(v, -1);
}

static void on_down(GtkButton *b, PdQueueView *v)
{
	move_selected(v, 1);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
		       GdkModifierType mods, PdQueueView *v)
{
	if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) {
		on_remove(NULL, v);
		return TRUE;
	}
	return FALSE;
}

static void on_fade(GtkSpinButton *s, PdQueueView *v)
{
	v->app->cfg.automix_fade = gtk_spin_button_get_value_as_int(s);
	config_save(&v->app->cfg);
}

static void on_auto(GtkCheckButton *c, PdQueueView *v)
{
	v->app->cfg.automix_auto = gtk_check_button_get_active(c);
	config_save(&v->app->cfg);
}

static void on_smart(GtkCheckButton *c, PdQueueView *v)
{
	v->app->cfg.automix_smart = gtk_check_button_get_active(c);
	config_save(&v->app->cfg);
}

static void on_sync(GtkCheckButton *c, PdQueueView *v)
{
	v->app->cfg.automix_sync = gtk_check_button_get_active(c);
	config_save(&v->app->cfg);
}

/* Anywhere on the panel that is not a row: append. */
static gboolean on_drop(GtkDropTarget *t, const GValue *val, double x,
			double y, PdQueueView *v)
{
	return queue_drop_value(v->app, val, G_MAXUINT);
}

static void on_items_changed(GListModel *m, guint pos, guint rm, guint add,
			     PdQueueView *v)
{
	gtk_stack_set_visible_child_name(GTK_STACK(v->stack),
					 g_list_model_get_n_items(m) ?
					 "list" : "empty");
}

/* "A  Artist – Title  →  B  Artist – Title" */
static char *deck_line(struct app *a, int i)
{
	struct track *t = i >= 0 ? deck_track(&a->engine.deck[i]) : NULL;
	char *title, *artist, *s;

	if (!t)
		return NULL;
	title = track_title(t);
	artist = track_artist(t);
	s = g_strdup_printf("%c  %s%s%s", app_deck_letter(i),
			    artist && *artist ? artist : "",
			    artist && *artist ? " – " : "",
			    title ? title : "Untitled");
	g_free(title);
	g_free(artist);
	return s;
}

static gboolean refresh(gpointer data)
{
	PdQueueView *v = data;
	static int ticks;
	guint n = g_list_model_get_n_items(G_LIST_MODEL(v->app->queue));

	/* the pitch column follows the playing deck, which glides */
	if (automix_enabled() && ++ticks % 4 == 0)
		pd_media_item_changed("*");
	char *s = g_strdup_printf("%u queued", n);
	char *now = deck_line(v->app, automix_active_deck());
	char *next = deck_line(v->app, automix_next_deck());
	char *line;

	gtk_label_set_text(GTK_LABEL(v->status), automix_status());
	gtk_label_set_text(GTK_LABEL(v->count), s);
	g_free(s);
	if (now && next)
		line = g_strdup_printf("%s   →   %s", now, next);
	else if (now)
		line = g_strdup(now);
	else
		line = g_strdup(automix_enabled() ? "" :
				"Press AUTOMIX to play the queue on decks "
				"A and B");
	gtk_label_set_text(GTK_LABEL(v->now), line);
	g_free(line);
	g_free(now);
	g_free(next);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->toggle)) !=
	    automix_enabled())
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->toggle),
					     automix_enabled());
	return G_SOURCE_CONTINUE;
}

static void pd_queue_view_dispose(GObject *obj)
{
	PdQueueView *v = PD_QUEUE_VIEW(obj);

	g_signal_handlers_disconnect_by_data(v->app->queue, v);
	if (v->timer) {
		g_source_remove(v->timer);
		v->timer = 0;
	}
	G_OBJECT_CLASS(pd_queue_view_parent_class)->dispose(obj);
}

static void pd_queue_view_class_init(PdQueueViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_queue_view_dispose;
}

static void pd_queue_view_init(PdQueueView *v)
{
}

static GtkWidget *tool(const char *label, const char *tip, GCallback cb,
		       gpointer data)
{
	GtkWidget *b = gtk_button_new_with_label(label);

	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", cb, data);
	return b;
}

static GtkWidget *option(const char *label, const char *tip, gboolean on,
			 GCallback cb, gpointer data)
{
	GtkWidget *c = gtk_check_button_new_with_label(label);

	gtk_check_button_set_active(GTK_CHECK_BUTTON(c), on);
	gtk_widget_set_tooltip_text(c, tip);
	gtk_widget_set_focusable(c, FALSE);
	g_signal_connect(c, "toggled", cb, data);
	return c;
}

GtkWidget *pd_queue_view_new(struct app *app)
{
	PdQueueView *v = g_object_new(PD_TYPE_QUEUE_VIEW,
				      "orientation", GTK_ORIENTATION_VERTICAL,
				      "spacing", 4, NULL);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *scroll, *l, *spin, *empty, *stack;
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	GtkDropTarget *drop;
	GtkEventController *keys;
	GType types[] = { PD_TYPE_MEDIA_ITEM, GDK_TYPE_FILE_LIST };

	v->app = app;

	/* transport line: the switch, next, and what is going on */
	v->toggle = gtk_toggle_button_new_with_label("AUTOMIX");
	gtk_widget_add_css_class(v->toggle, "automix-toggle");
	gtk_widget_set_focusable(v->toggle, FALSE);
	gtk_widget_set_tooltip_text(v->toggle, "Play the queue hands free "
				    "on decks A and B");
	g_signal_connect(v->toggle, "toggled", G_CALLBACK(on_toggle), v);
	gtk_box_append(GTK_BOX(bar), v->toggle);
	gtk_box_append(GTK_BOX(bar), tool("Next ⏭", "Mix into the next "
					  "track now", G_CALLBACK(on_next),
					  v));
	v->now = gtk_label_new("");
	gtk_widget_add_css_class(v->now, "automix-now");
	gtk_widget_set_hexpand(v->now, TRUE);
	gtk_label_set_xalign(GTK_LABEL(v->now), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(v->now), PANGO_ELLIPSIZE_END);
	gtk_widget_set_margin_start(v->now, 10);
	gtk_box_append(GTK_BOX(bar), v->now);
	v->status = gtk_label_new("");
	gtk_widget_add_css_class(v->status, "automix-status");
	gtk_label_set_xalign(GTK_LABEL(v->status), 1.0f);
	gtk_box_append(GTK_BOX(bar), v->status);
	gtk_widget_add_css_class(bar, "toolbar");
	gtk_box_append(GTK_BOX(v), bar);

	/* settings line */
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	l = gtk_label_new("Transition");
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(bar), l);
	spin = gtk_spin_button_new_with_range(2, 90, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), app->cfg.automix_fade);
	gtk_widget_set_tooltip_text(spin, "Seconds the crossfader takes; "
				    "with Auto length the minimum");
	g_signal_connect(spin, "value-changed", G_CALLBACK(on_fade), v);
	gtk_box_append(GTK_BOX(bar), spin);
	l = gtk_label_new("s");
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(bar), l);
	l = option("Auto length", "Size each transition from the two "
		   "tracks (3 to 45 s): a 32 beat blend when the tempos "
		   "match, longer to cover a quiet outro or intro, a short "
		   "cut when they cannot be matched", app->cfg.automix_auto,
		   G_CALLBACK(on_auto), v);
	gtk_widget_set_margin_start(l, 12);
	gtk_box_append(GTK_BOX(bar), l);
	l = option("Smart order", "Play the queued track that best follows "
		   "the current one: closest tempo, then the closest key on "
		   "the Camelot wheel; off plays the queue in order",
		   app->cfg.automix_smart, G_CALLBACK(on_smart), v);
	gtk_widget_set_margin_start(l, 12);
	gtk_box_append(GTK_BOX(bar), l);
	l = option("Sync tempo", "Meet halfway in tempo before the blend, "
		   "keep the beats locked while it runs, glide back to the "
		   "track's own tempo after", app->cfg.automix_sync,
		   G_CALLBACK(on_sync), v);
	gtk_widget_set_margin_start(l, 12);
	gtk_box_append(GTK_BOX(bar), l);
	gtk_widget_add_css_class(bar, "toolbar");
	gtk_box_append(GTK_BOX(v), bar);

	/* the queue */
	v->selection = gtk_single_selection_new(
			G_LIST_MODEL(g_object_ref(app->queue)));
	gtk_single_selection_set_autoselect(v->selection, FALSE);
	g_signal_connect(f, "setup", G_CALLBACK(setup_row), v);
	g_signal_connect(f, "bind", G_CALLBACK(bind_row), v);
	g_signal_connect(f, "unbind", G_CALLBACK(unbind_row), v);
	v->list = gtk_list_view_new(GTK_SELECTION_MODEL(v->selection), f);
	gtk_widget_add_css_class(v->list, "queue-list");
	keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), v);
	gtk_widget_add_controller(v->list, keys);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v->list);
	gtk_widget_set_vexpand(scroll, TRUE);

	empty = gtk_label_new("The queue is empty.\n\nDrag tracks here from "
			      "the Library or SoundCloud tab, drop audio "
			      "files from your file manager, or select "
			      "tracks and press + Queue.");
	gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
	gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
	gtk_widget_add_css_class(empty, "dim-label");
	gtk_widget_add_css_class(empty, "placeholder");
	gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);

	stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(stack), scroll, "list");
	gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
	gtk_widget_set_vexpand(stack, TRUE);
	gtk_box_append(GTK_BOX(v), stack);
	v->stack = stack;
	g_signal_connect(app->queue, "items-changed",
			 G_CALLBACK(on_items_changed), v);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(bar), tool("▲", "Move up", G_CALLBACK(on_up),
					  v));
	gtk_box_append(GTK_BOX(bar), tool("▼", "Move down",
					  G_CALLBACK(on_down), v));
	gtk_box_append(GTK_BOX(bar), tool("Remove", "Remove the selected "
					  "track (Delete)",
					  G_CALLBACK(on_remove), v));
	gtk_box_append(GTK_BOX(bar), tool("Clear", "Empty the queue",
					  G_CALLBACK(on_clear), v));
	v->count = gtk_label_new("");
	gtk_widget_add_css_class(v->count, "dim-label");
	gtk_widget_set_hexpand(v->count, TRUE);
	gtk_label_set_xalign(GTK_LABEL(v->count), 1.0f);
	gtk_box_append(GTK_BOX(bar), v->count);
	gtk_widget_add_css_class(bar, "toolbar");
	gtk_box_append(GTK_BOX(v), bar);

	drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
	gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), v);
	gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(drop));

	v->timer = g_timeout_add(250, refresh, v);
	refresh(v);
	on_items_changed(G_LIST_MODEL(app->queue), 0, 0, 0, v);
	return GTK_WIDGET(v);
}
