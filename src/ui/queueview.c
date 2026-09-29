// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * queueview.c - automix panel
 */
#include "automix.h"
#include "queueview.h"

struct _PdQueueView {
	GtkBox parent;
	struct app *app;
	GtkWidget *list;
	GtkSingleSelection *selection;
	GtkWidget *toggle;
	GtkWidget *stack;
	GtkWidget *status;
	GtkWidget *count;
	guint timer;
};

G_DEFINE_FINAL_TYPE(PdQueueView, pd_queue_view, GTK_TYPE_BOX)

static void setup_row(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *num = gtk_label_new("");
	GtkWidget *title = gtk_label_new("");
	GtkWidget *len = gtk_label_new("");

	gtk_widget_add_css_class(num, "queue-number");
	gtk_label_set_width_chars(GTK_LABEL(num), 3);
	gtk_label_set_xalign(GTK_LABEL(num), 1.0f);
	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(title, TRUE);
	gtk_widget_add_css_class(len, "mono");
	gtk_widget_add_css_class(len, "dim-label");
	gtk_box_append(GTK_BOX(box), num);
	gtk_box_append(GTK_BOX(box), title);
	gtk_box_append(GTK_BOX(box), len);
	gtk_list_item_set_child(li, box);
}

static void bind_row(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	GtkWidget *box = gtk_list_item_get_child(li);
	GtkWidget *num = gtk_widget_get_first_child(box);
	GtkWidget *title = gtk_widget_get_next_sibling(num);
	GtkWidget *len = gtk_widget_get_next_sibling(title);
	char *s;

	s = g_strdup_printf("%u", gtk_list_item_get_position(li) + 1);
	gtk_label_set_text(GTK_LABEL(num), s);
	g_free(s);
	if (m->artist && *m->artist)
		s = g_strdup_printf("%s  –  %s", m->artist, m->title);
	else
		s = g_strdup(m->title);
	gtk_label_set_text(GTK_LABEL(title), s);
	g_free(s);
	s = format_duration(m->duration);
	gtk_label_set_text(GTK_LABEL(len), s);
	g_free(s);
}

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

static void on_sync(GtkCheckButton *c, PdQueueView *v)
{
	v->app->cfg.automix_sync = gtk_check_button_get_active(c);
	config_save(&v->app->cfg);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *val, double x,
			double y, PdQueueView *v)
{
	if (!G_VALUE_HOLDS(val, PD_TYPE_MEDIA_ITEM))
		return FALSE;
	g_list_store_append(v->app->queue, g_value_get_object(val));
	return TRUE;
}

static void on_items_changed(GListModel *m, guint pos, guint rm, guint add,
			     PdQueueView *v)
{
	gtk_stack_set_visible_child_name(GTK_STACK(v->stack),
					 g_list_model_get_n_items(m) ?
					 "list" : "empty");
}

static gboolean refresh(gpointer data)
{
	PdQueueView *v = data;
	guint n = g_list_model_get_n_items(G_LIST_MODEL(v->app->queue));
	char *s = g_strdup_printf("%u queued", n);

	gtk_label_set_text(GTK_LABEL(v->status), automix_status());
	gtk_label_set_text(GTK_LABEL(v->count), s);
	g_free(s);
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

GtkWidget *pd_queue_view_new(struct app *app)
{
	PdQueueView *v = g_object_new(PD_TYPE_QUEUE_VIEW,
				      "orientation", GTK_ORIENTATION_VERTICAL,
				      "spacing", 4, NULL);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *scroll, *l, *spin, *check, *empty, *stack;
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	GtkDropTarget *drop;

	v->app = app;

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

	l = gtk_label_new("Transition");
	gtk_widget_add_css_class(l, "dim-label");
	gtk_widget_set_margin_start(l, 12);
	gtk_box_append(GTK_BOX(bar), l);
	spin = gtk_spin_button_new_with_range(2, 90, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), app->cfg.automix_fade);
	gtk_widget_set_tooltip_text(spin, "Seconds the crossfader takes");
	g_signal_connect(spin, "value-changed", G_CALLBACK(on_fade), v);
	gtk_box_append(GTK_BOX(bar), spin);
	l = gtk_label_new("s");
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(bar), l);

	check = gtk_check_button_new_with_label("Auto length");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
				    app->cfg.automix_auto);
	gtk_widget_set_tooltip_text(check, "Size each transition from the "
				    "tracks: long beat matched blends when "
				    "the tempos fit, the whole outro when a "
				    "track winds down, short cuts otherwise; "
				    "the seconds above are the minimum");
	gtk_widget_set_margin_start(check, 12);
	g_signal_connect(check, "toggled", G_CALLBACK(on_auto), v);
	gtk_box_append(GTK_BOX(bar), check);

	check = gtk_check_button_new_with_label("Sync tempo");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
				    app->cfg.automix_sync);
	gtk_widget_set_margin_start(check, 12);
	g_signal_connect(check, "toggled", G_CALLBACK(on_sync), v);
	gtk_box_append(GTK_BOX(bar), check);

	v->status = gtk_label_new("");
	gtk_widget_add_css_class(v->status, "automix-status");
	gtk_widget_set_hexpand(v->status, TRUE);
	gtk_label_set_xalign(GTK_LABEL(v->status), 1.0f);
	gtk_box_append(GTK_BOX(bar), v->status);
	gtk_box_append(GTK_BOX(v), bar);

	v->selection = gtk_single_selection_new(
			G_LIST_MODEL(g_object_ref(app->queue)));
	gtk_single_selection_set_autoselect(v->selection, FALSE);
	g_signal_connect(f, "setup", G_CALLBACK(setup_row), NULL);
	g_signal_connect(f, "bind", G_CALLBACK(bind_row), NULL);
	v->list = gtk_list_view_new(GTK_SELECTION_MODEL(v->selection), f);
	gtk_widget_add_css_class(v->list, "queue-list");
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v->list);
	gtk_widget_set_vexpand(scroll, TRUE);

	empty = gtk_label_new("The queue is empty.\nRight click tracks in "
			      "the Library or SoundCloud tab and choose "
			      "\"Add to automix queue\", or drag them here.");
	gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
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
					  "track", G_CALLBACK(on_remove), v));
	gtk_box_append(GTK_BOX(bar), tool("Clear", "Empty the queue",
					  G_CALLBACK(on_clear), v));
	v->count = gtk_label_new("");
	gtk_widget_add_css_class(v->count, "dim-label");
	gtk_widget_set_hexpand(v->count, TRUE);
	gtk_label_set_xalign(GTK_LABEL(v->count), 1.0f);
	gtk_box_append(GTK_BOX(bar), v->count);
	gtk_box_append(GTK_BOX(v), bar);

	drop = gtk_drop_target_new(PD_TYPE_MEDIA_ITEM, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), v);
	gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(drop));

	v->timer = g_timeout_add(250, refresh, v);
	refresh(v);
	on_items_changed(G_LIST_MODEL(app->queue), 0, 0, 0, v);
	return GTK_WIDGET(v);
}
