// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * mediaview.c - track list
 */
#include "mediaview.h"
#include "sccache.h"

struct _PdMediaView {
	GtkBox parent;
	struct app *app;
	GListStore *store;
	GtkFilterListModel *filtered;
	GtkSortListModel *sorted;
	GtkSingleSelection *selection;
	GtkWidget *view;
	GtkWidget *search;
	GtkWidget *count;
	GtkWidget *placeholder;
	GtkWidget *stack;
	char **terms;
};

G_DEFINE_FINAL_TYPE(PdMediaView, pd_media_view, GTK_TYPE_BOX)

/* ---- filtering and sorting --------------------------------------- */

static gboolean match(gpointer item, gpointer data)
{
	PdMediaView *v = data;
	const char *hay;
	int i;

	if (!v->terms)
		return TRUE;
	hay = pd_media_item_haystack(item);
	for (i = 0; v->terms[i]; i++)
		if (*v->terms[i] && !strstr(hay, v->terms[i]))
			return FALSE;
	return TRUE;
}

static int cmp_str(gconstpointer a, gconstpointer b, gpointer data)
{
	gsize off = GPOINTER_TO_SIZE(data);
	const char *sa = *(const char *const *)((const char *)a + off);
	const char *sb = *(const char *const *)((const char *)b + off);

	return g_utf8_collate(sa ? sa : "", sb ? sb : "");
}

static int cmp_double(gconstpointer a, gconstpointer b, gpointer data)
{
	gsize off = GPOINTER_TO_SIZE(data);
	double da = *(const double *)((const char *)a + off);
	double db = *(const double *)((const char *)b + off);

	return da < db ? -1 : da > db;
}

/* ---- drag source ------------------------------------------------- */

static GdkContentProvider *drag_prepare(GtkDragSource *src, double x,
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

static void drag_begin(GtkDragSource *src, GdkDrag *drag, GtkListItem *li)
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

/* ---- cells ------------------------------------------------------- */

enum column {
	COL_STATE,
	COL_TITLE,
	COL_ARTIST,
	COL_ALBUM,
	COL_GENRE,
	COL_BPM,
	COL_DURATION,
};

static void setup_cell(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	GtkWidget *l = gtk_label_new("");
	enum column col = GPOINTER_TO_INT(data);
	GtkDragSource *src = gtk_drag_source_new();

	gtk_label_set_xalign(GTK_LABEL(l), col >= COL_BPM ? 1.0f : 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(l, TRUE);
	if (col >= COL_BPM)
		gtk_widget_add_css_class(l, "mono");
	/* Every cell is a drag handle for its row. */
	gtk_drag_source_set_actions(src, GDK_ACTION_COPY);
	g_signal_connect(src, "prepare", G_CALLBACK(drag_prepare), li);
	g_signal_connect(src, "drag-begin", G_CALLBACK(drag_begin), li);
	gtk_widget_add_controller(l, GTK_EVENT_CONTROLLER(src));
	gtk_list_item_set_child(li, l);
}

static void bind_cell(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	GtkWidget *l = gtk_list_item_get_child(li);
	PdMediaView *v = g_object_get_data(G_OBJECT(f), "view");
	enum column col = GPOINTER_TO_INT(data);
	char *tmp = NULL;
	const char *text = "";

	if (app_item_played(v->app, m))
		gtk_widget_add_css_class(l, "played");
	else
		gtk_widget_remove_css_class(l, "played");

	switch (col) {
	case COL_STATE: {
		int pc = sccache_progress(m);

		if (app_item_played(v->app, m))
			text = "✓";
		if (pc == 100)
			text = tmp = g_strdup_printf("%s ⬇", text);
		else if (pc >= 0)
			text = tmp = g_strdup_printf("%s %d%%", text, pc);
		else if (pc == -2)
			text = tmp = g_strdup_printf("%s ✗", text);
		break;
	}
	case COL_TITLE:
		if (m->source == MEDIA_SOUNDCLOUD && m->preview)
			text = tmp = g_strdup_printf("%s (preview)",
						     m->title);
		else
			text = m->title;
		break;
	case COL_ARTIST:
		text = m->artist;
		break;
	case COL_ALBUM:
		text = m->album;
		break;
	case COL_GENRE:
		text = m->genre;
		break;
	case COL_BPM:
		if (m->bpm > 0.0)
			text = tmp = g_strdup_printf("%.0f", m->bpm);
		break;
	case COL_DURATION:
		text = tmp = format_duration(m->duration);
		break;
	}
	gtk_label_set_text(GTK_LABEL(l), text ? text : "");
	g_free(tmp);
}

static void add_column(PdMediaView *v, const char *title, enum column col,
		       GtkSorter *sorter, gboolean expand, int width)
{
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	GtkColumnViewColumn *c;

	g_object_set_data(G_OBJECT(f), "view", v);
	g_signal_connect(f, "setup", G_CALLBACK(setup_cell),
			 GINT_TO_POINTER(col));
	g_signal_connect(f, "bind", G_CALLBACK(bind_cell),
			 GINT_TO_POINTER(col));
	c = gtk_column_view_column_new(title, f);
	gtk_column_view_column_set_sorter(c, sorter);
	gtk_column_view_column_set_expand(c, expand);
	gtk_column_view_column_set_resizable(c, TRUE);
	if (width > 0)
		gtk_column_view_column_set_fixed_width(c, width);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(v->view), c);
	g_object_unref(sorter);
	g_object_unref(c);
}

/* ---- loading ----------------------------------------------------- */

static PdMediaItem *selected(PdMediaView *v)
{
	return gtk_single_selection_get_selected_item(v->selection);
}

/* Prefer an empty visible deck, then a paused one, then deck A. */
static int free_deck(struct app *a)
{
	struct deck *d = a->engine.deck;
	int i;

	for (i = 0; i < a->cfg.ndecks; i++)
		if (!deck_track(&d[i]))
			return i;
	for (i = 0; i < a->cfg.ndecks; i++)
		if (!atomic_load(&d[i].playing))
			return i;
	return 0;
}

static void on_activate(GtkColumnView *cv, guint pos, PdMediaView *v)
{
	PdMediaItem *m = g_list_model_get_item(G_LIST_MODEL(v->selection), pos);

	if (!m)
		return;
	app_load_item(v->app, free_deck(v->app), m);
	g_object_unref(m);
}

void pd_media_view_load_selected(PdMediaView *v, int idx)
{
	PdMediaItem *m = selected(v);

	if (m)
		app_load_item(v->app, idx, m);
}

static void on_load_deck(GSimpleAction *a, GVariant *p, gpointer data)
{
	pd_media_view_load_selected(data, g_variant_get_int32(p));
}

static void on_open_link(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdMediaItem *m = selected(data);
	PdMediaView *v = data;
	GtkUriLauncher *l;

	if (!m || !m->permalink)
		return;
	l = gtk_uri_launcher_new(m->permalink);
	gtk_uri_launcher_launch(l, v->app->win, NULL, NULL, NULL);
	g_object_unref(l);
}

static GMenuModel *build_menu(PdMediaView *v);

static void on_right_click(GtkGestureClick *g, int n, double x, double y,
			   PdMediaView *v)
{
	GtkWidget *pop = g_object_get_data(G_OBJECT(v), "popover");
	GdkRectangle r = { (int)x, (int)y, 1, 1 };

	GMenuModel *menu = build_menu(v);

	/* Rebuild the menu so it lists exactly the visible decks. */
	gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pop), menu);
	g_object_unref(menu);
	gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
	gtk_popover_popup(GTK_POPOVER(pop));
}

/* ---- construction ------------------------------------------------ */

static void on_search(GtkEditable *e, PdMediaView *v)
{
	pd_media_view_set_filter(v, gtk_editable_get_text(e));
}

static void on_items_changed(GListModel *m, guint pos, guint rm, guint add,
			     PdMediaView *v)
{
	guint n = g_list_model_get_n_items(m);
	guint total = g_list_model_get_n_items(G_LIST_MODEL(v->store));
	char *s;

	if (n == total)
		s = g_strdup_printf("%u track%s", n, n == 1 ? "" : "s");
	else
		s = g_strdup_printf("%u of %u tracks", n, total);
	gtk_label_set_text(GTK_LABEL(v->count), s);
	g_free(s);
	gtk_stack_set_visible_child_name(GTK_STACK(v->stack),
					 total ? "list" : "empty");
}

static void pd_media_view_finalize(GObject *obj)
{
	PdMediaView *v = PD_MEDIA_VIEW(obj);

	g_strfreev(v->terms);
	G_OBJECT_CLASS(pd_media_view_parent_class)->finalize(obj);
}

static void pd_media_view_class_init(PdMediaViewClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = pd_media_view_finalize;
}

static void pd_media_view_init(PdMediaView *v)
{
}

static GMenuModel *build_menu(PdMediaView *v)
{
	GMenu *menu = g_menu_new();
	int i;

	for (i = 0; i < v->app->cfg.ndecks; i++) {
		char *label = g_strdup_printf("Load to deck %c",
					      app_deck_letter(i));
		GMenuItem *item = g_menu_item_new(label, NULL);

		g_menu_item_set_action_and_target(item, "media.load", "i", i);
		g_menu_append_item(menu, item);
		g_object_unref(item);
		g_free(label);
	}
	g_menu_append(menu, "Add to automix queue", "media.queue");
	if (v->store == v->app->sc_results) {
		g_menu_append(menu, "Download to cache", "media.cache");
		g_menu_append(menu, "Open on SoundCloud", "media.open-link");
	}
	return G_MENU_MODEL(menu);
}

void pd_media_view_queue_selected(PdMediaView *v)
{
	PdMediaItem *m = selected(v);

	if (!m)
		return;
	g_list_store_append(v->app->queue, m);
	app_toast(v->app, "Queued \"%s\"", m->title);
}

static void on_queue(GSimpleAction *a, GVariant *p, gpointer data)
{
	pd_media_view_queue_selected(data);
}

GtkWidget *pd_media_view_queue_button(PdMediaView *v)
{
	GtkWidget *b = gtk_button_new_with_label("+ Queue");

	gtk_widget_add_css_class(b, "queue-button");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_tooltip_text(b, "Add the selected track to the "
				    "automix queue");
	g_signal_connect_swapped(b, "clicked",
				 G_CALLBACK(pd_media_view_queue_selected), v);
	return b;
}

static void on_cache(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdMediaItem *m = selected(data);

	if (m)
		sccache_fetch(m);
}

static GtkWidget *build_popover(PdMediaView *v)
{
	GSimpleActionGroup *grp = g_simple_action_group_new();
	static const GActionEntry entries[] = {
		{ "load", on_load_deck, "i" },
		{ "queue", on_queue },
		{ "cache", on_cache },
		{ "open-link", on_open_link },
	};
	GMenuModel *menu = build_menu(v);
	GtkWidget *pop;

	g_action_map_add_action_entries(G_ACTION_MAP(grp), entries,
					G_N_ELEMENTS(entries), v);
	gtk_widget_insert_action_group(GTK_WIDGET(v), "media",
				       G_ACTION_GROUP(grp));
	pop = gtk_popover_menu_new_from_model(menu);
	gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
	gtk_widget_set_parent(pop, v->view);
	g_object_unref(menu);
	g_object_unref(grp);
	return pop;
}

GtkWidget *pd_media_view_new(struct app *app, GListStore *store,
			     const char *placeholder)
{
	PdMediaView *v = g_object_new(PD_TYPE_MEDIA_VIEW,
				      "orientation", GTK_ORIENTATION_VERTICAL,
				      "spacing", 4, NULL);
	GtkCustomFilter *filter = gtk_custom_filter_new(match, v, NULL);
	GtkWidget *scroll, *pop, *empty;
	GtkGesture *click;
	gboolean sc = store == app->sc_results;

	v->app = app;
	v->store = store;
	v->filtered = gtk_filter_list_model_new(
			G_LIST_MODEL(g_object_ref(store)), GTK_FILTER(filter));
	gtk_filter_list_model_set_incremental(v->filtered, TRUE);

	v->view = gtk_column_view_new(NULL);
	v->sorted = gtk_sort_list_model_new(G_LIST_MODEL(v->filtered),
			g_object_ref(gtk_column_view_get_sorter(
					GTK_COLUMN_VIEW(v->view))));
	v->selection = gtk_single_selection_new(G_LIST_MODEL(v->sorted));
	gtk_single_selection_set_autoselect(v->selection, FALSE);
	gtk_column_view_set_model(GTK_COLUMN_VIEW(v->view),
				  GTK_SELECTION_MODEL(v->selection));
	gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(v->view),
						  FALSE);
	gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(v->view),
						FALSE);
	gtk_widget_add_css_class(v->view, "data-table");

#define STR_SORTER(field) \
	GTK_SORTER(gtk_custom_sorter_new(cmp_str, \
		GSIZE_TO_POINTER(G_STRUCT_OFFSET(PdMediaItem, field)), NULL))
#define NUM_SORTER(field) \
	GTK_SORTER(gtk_custom_sorter_new(cmp_double, \
		GSIZE_TO_POINTER(G_STRUCT_OFFSET(PdMediaItem, field)), NULL))
	add_column(v, "", COL_STATE, STR_SORTER(key), FALSE, 64);
	add_column(v, "Title", COL_TITLE, STR_SORTER(title), TRUE, 0);
	add_column(v, "Artist", COL_ARTIST, STR_SORTER(artist), TRUE, 0);
	if (!sc)
		add_column(v, "Album", COL_ALBUM, STR_SORTER(album), TRUE, 0);
	add_column(v, "Genre", COL_GENRE, STR_SORTER(genre), FALSE, 110);
	add_column(v, "BPM", COL_BPM, NUM_SORTER(bpm), FALSE, 60);
	add_column(v, "Length", COL_DURATION, NUM_SORTER(duration), FALSE,
		   70);
#undef STR_SORTER
#undef NUM_SORTER

	g_signal_connect(v->view, "activate", G_CALLBACK(on_activate), v);
	click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
				      GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(on_right_click), v);
	gtk_widget_add_controller(v->view, GTK_EVENT_CONTROLLER(click));
	pop = build_popover(v);
	g_object_set_data(G_OBJECT(v), "popover", pop);

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v->view);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);

	empty = gtk_label_new(placeholder);
	gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
	gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(empty, "dim-label");
	gtk_widget_add_css_class(empty, "placeholder");
	gtk_widget_set_vexpand(empty, TRUE);
	gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);

	v->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(v->stack), scroll, "list");
	gtk_stack_add_named(GTK_STACK(v->stack), empty, "empty");
	gtk_widget_set_vexpand(v->stack, TRUE);
	gtk_box_append(GTK_BOX(v), v->stack);

	v->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
					      sc ? "Search SoundCloud or paste "
						   "a link" : "Filter library");
	g_signal_connect(v->search, "changed", G_CALLBACK(on_search), v);
	v->count = gtk_label_new("");
	gtk_widget_add_css_class(v->count, "dim-label");
	gtk_box_append(GTK_BOX(v), v->count);
	g_signal_connect(v->selection, "items-changed",
			 G_CALLBACK(on_items_changed), v);
	on_items_changed(G_LIST_MODEL(v->selection), 0, 0, 0, v);
	return GTK_WIDGET(v);
}

GtkWidget *pd_media_view_search_entry(PdMediaView *v)
{
	return v->search;
}

void pd_media_view_set_filter(PdMediaView *v, const char *text)
{
	char *fold = g_utf8_casefold(text ? text : "", -1);

	g_strfreev(v->terms);
	v->terms = *fold ? g_strsplit(fold, " ", -1) : NULL;
	g_free(fold);
	gtk_filter_changed(gtk_filter_list_model_get_filter(v->filtered),
			   GTK_FILTER_CHANGE_DIFFERENT);
}

guint pd_media_view_count(PdMediaView *v)
{
	return g_list_model_get_n_items(G_LIST_MODEL(v->selection));
}
