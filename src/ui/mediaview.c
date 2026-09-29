// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * mediaview.c - track list
 *
 * Rows show the title with artist and album underneath, the musical key
 * as a coloured Camelot pill, the tempo, the length, state icons and a
 * headphone button for pre-listening.  The filter understands plain
 * words plus "bpm:120-128", "key:8A" and "genre:techno" tokens.
 */
#include <stdlib.h>
#include <string.h>

#include "analyze.h"
#include "cuestore.h"
#include "mediaview.h"
#include "sccache.h"

struct _PdMediaView {
	GtkBox parent;
	struct app *app;
	GListStore *store;
	GtkFilterListModel *filtered;
	GtkSortListModel *sorted;
	GtkMultiSelection *selection;
	GtkWidget *view;
	GtkWidget *search;
	GtkWidget *count;
	GtkWidget *stack;
	char **terms;
	double bpm_lo, bpm_hi;		/* from a bpm: token, 0 when unused */
	char *key_filter;		/* Camelot code, lower case */
	char *sel_key;			/* key of the selected item */
	struct playlist *playlist;	/* when showing a playlist */
	gboolean played_only;		/* list only tracks played this session */
};

G_DEFINE_FINAL_TYPE(PdMediaView, pd_media_view, GTK_TYPE_BOX)

/* ---- helpers ----------------------------------------------------- */

static double item_bpm(PdMediaItem *m)
{
	double b = m->bpm;

	if (b <= 0.0)
		b = cuestore_bpm(m->key);
	return b;
}

/* ---- filtering and sorting --------------------------------------- */

static gboolean match(gpointer item, gpointer data)
{
	PdMediaView *v = data;
	PdMediaItem *m = item;
	const char *hay;
	int i;

	if (v->played_only && !app_item_played(v->app, m))
		return FALSE;
	if (v->bpm_hi > 0.0) {
		double b = item_bpm(m);

		if (b < v->bpm_lo || b > v->bpm_hi)
			return FALSE;
	}
	if (v->key_filter) {
		char *c = g_ascii_strdown(key_camelot(cuestore_key(m->key)), -1);
		gboolean ok = g_str_equal(c, v->key_filter);

		g_free(c);
		if (!ok)
			return FALSE;
	}
	if (!v->terms)
		return TRUE;
	hay = pd_media_item_haystack(m);
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

static int cmp_key(gconstpointer a, gconstpointer b, gpointer data)
{
	int ka = cuestore_key(((PdMediaItem *)a)->key);
	int kb = cuestore_key(((PdMediaItem *)b)->key);
	int na = atoi(key_camelot(ka)), nb = atoi(key_camelot(kb));

	if (na != nb)
		return na - nb;
	return ka - kb;
}

static int cmp_bpm(gconstpointer a, gconstpointer b, gpointer data)
{
	double da = item_bpm((PdMediaItem *)a), db = item_bpm((PdMediaItem *)b);

	return da < db ? -1 : da > db;
}

/* ---- cells ------------------------------------------------------- */

enum column {
	COL_STATE,
	COL_TRACK,
	COL_ARTIST,
	COL_ALBUM,
	COL_GENRE,
	COL_KEY,
	COL_BPM,
	COL_DURATION,
	COL_PREVIEW,
};

static void on_preview_clicked(GtkButton *b, GtkListItem *li)
{
	PdMediaView *v = g_object_get_data(G_OBJECT(b), "view");
	PdMediaItem *m = gtk_list_item_get_item(li);

	if (m)
		app_preview(v->app, m);
}

static GtkWidget *make_cell(PdMediaView *v, enum column col, GtkListItem *li)
{
	GtkWidget *w;

	switch (col) {
	case COL_TRACK:
	case COL_ARTIST:
	case COL_ALBUM:
	case COL_GENRE:
		w = gtk_label_new("");
		gtk_widget_add_css_class(w, col == COL_TRACK ? "row-title" :
					 "row-sub");
		gtk_label_set_xalign(GTK_LABEL(w), 0.0f);
		gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
		gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
		return w;
	case COL_KEY:
	case COL_BPM:
		w = gtk_label_new("");
		gtk_widget_add_css_class(w, "pill");
		gtk_widget_add_css_class(w, col == COL_KEY ? "pill-key" :
					 "pill-bpm");
		gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
		return w;
	case COL_PREVIEW:
		w = gtk_button_new_from_icon_name(
				"audio-headphones-symbolic");
		gtk_widget_add_css_class(w, "row-preview");
		gtk_widget_set_focusable(w, FALSE);
		gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(w, "Pre-listen in the headphones");
		g_object_set_data(G_OBJECT(w), "view", v);
		g_signal_connect(w, "clicked", G_CALLBACK(on_preview_clicked),
				 li);
		return w;
	case COL_STATE:
	case COL_DURATION:
	default:
		w = gtk_label_new("");
		gtk_widget_add_css_class(w, "mono");
		gtk_widget_add_css_class(w, col == COL_STATE ? "row-state" :
					 "row-len");
		gtk_label_set_xalign(GTK_LABEL(w), col == COL_STATE ? 0.5f :
				     1.0f);
		gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
		return w;
	}
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

/* Right click acts on the row under the pointer, so select it first. */
static void on_cell_right_click(GtkGestureClick *g, int n, double x,
				double y, GtkListItem *li)
{
	PdMediaView *v = g_object_get_data(G_OBJECT(g), "view");
	guint pos = gtk_list_item_get_position(li);

	if (pos != GTK_INVALID_LIST_POSITION &&
	    !gtk_selection_model_is_selected(GTK_SELECTION_MODEL(v->selection),
					     pos))
		gtk_selection_model_select_item(
				GTK_SELECTION_MODEL(v->selection), pos, TRUE);
}

static void on_item_changed(GObject *watch, const char *key, GtkListItem *li);

static void setup_cell(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	PdMediaView *v = g_object_get_data(G_OBJECT(f), "view");
	enum column col = GPOINTER_TO_INT(data);
	GtkWidget *w = make_cell(v, col, li);
	GtkDragSource *src = gtk_drag_source_new();
	GtkGesture *rc = gtk_gesture_click_new();

	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc),
				      GDK_BUTTON_SECONDARY);
	g_object_set_data(G_OBJECT(rc), "view", v);
	g_signal_connect(rc, "pressed", G_CALLBACK(on_cell_right_click), li);
	gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(rc));

	gtk_widget_set_hexpand(w, col == COL_TRACK || col == COL_ARTIST ||
			       col == COL_ALBUM);
	g_object_set_data(G_OBJECT(li), "view", v);
	g_object_set_data(G_OBJECT(li), "col", GINT_TO_POINTER(col));
	g_signal_connect_object(pd_media_watch(), "changed",
				G_CALLBACK(on_item_changed), li, 0);
	/* Every cell is a drag handle for its row. */
	gtk_drag_source_set_actions(src, GDK_ACTION_COPY);
	g_signal_connect(src, "prepare", G_CALLBACK(drag_prepare), li);
	g_signal_connect(src, "drag-begin", G_CALLBACK(drag_begin), li);
	gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(src));
	gtk_list_item_set_child(li, w);
}

static void bind_title(GtkWidget *l, PdMediaItem *m)
{
	char *t = m->source == MEDIA_SOUNDCLOUD && m->preview ?
		  g_strdup_printf("%s (preview)", m->title) : g_strdup(m->title);

	gtk_label_set_text(GTK_LABEL(l), t);
	g_free(t);
}

static void bind_text(GtkWidget *l, const char *text, const char *none)
{
	gtk_label_set_text(GTK_LABEL(l), text && *text ? text : none);
}

/* The pill takes one of twelve "camelot-N" classes for its colour. */
static void bind_key(GtkWidget *l, PdMediaItem *m)
{
	int k = cuestore_key(m->key);
	int n = atoi(key_camelot(k)), i;
	char cls[16], *text;

	for (i = 1; i <= 12; i++) {
		g_snprintf(cls, sizeof(cls), "camelot-%d", i);
		gtk_widget_remove_css_class(l, cls);
	}
	if (k < 0) {
		gtk_widget_set_visible(l, FALSE);
		return;
	}
	gtk_widget_set_visible(l, TRUE);
	text = g_strdup_printf("%s %s", key_camelot(k), key_name(k));
	gtk_label_set_text(GTK_LABEL(l), text);
	g_free(text);
	g_snprintf(cls, sizeof(cls), "camelot-%d", n);
	gtk_widget_add_css_class(l, cls);
}

/*
 * Played tracks are styled as whole rows: flag the cell and the row
 * widget that holds it, so the stylesheet can tint the entire line.
 */
static void mark_played(GtkWidget *cell, gboolean played)
{
	GtkWidget *w = cell;

	for (;;) {
		if (played)
			gtk_widget_add_css_class(w, "played");
		else
			gtk_widget_remove_css_class(w, "played");
		if (g_strcmp0(gtk_widget_get_css_name(w), "row") == 0)
			return;
		w = gtk_widget_get_parent(w);
		if (!w || GTK_IS_COLUMN_VIEW(w))
			return;
	}
}

static void bind_item(PdMediaView *v, GtkListItem *li, enum column col)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	GtkWidget *w = gtk_list_item_get_child(li);
	gboolean played;
	char *tmp = NULL;

	if (!m)
		return;
	played = app_item_played(v->app, m);

	/* every history entry was played, marking them all says nothing */
	if (v->store == history_items())
		played = FALSE;
	mark_played(w, played);

	switch (col) {
	case COL_STATE: {
		int pc = sccache_progress(m);
		GString *s = g_string_new("");

		if (pc == 100)
			g_string_append(s, " ⬇");
		else if (pc >= 0)
			g_string_append_printf(s, " %d%%", pc);
		else if (pc == -2)
			g_string_append(s, " ✗");
		gtk_label_set_text(GTK_LABEL(w), g_strstrip(s->str));
		g_string_free(s, TRUE);
		break;
	}
	case COL_TRACK:
		bind_title(w, m);
		break;
	case COL_ARTIST:
		bind_text(w, m->artist, "Unknown artist");
		break;
	case COL_ALBUM:
		bind_text(w, m->album, m->source == MEDIA_SOUNDCLOUD ?
			  "SoundCloud" : "");
		break;
	case COL_GENRE:
		bind_text(w, m->genre, "");
		break;
	case COL_KEY:
		bind_key(w, m);
		break;
	case COL_BPM: {
		double b = item_bpm(m);

		gtk_widget_set_visible(w, b > 0.0);
		tmp = g_strdup_printf("%.0f", b);
		gtk_label_set_text(GTK_LABEL(w), tmp);
		break;
	}
	case COL_DURATION:
		tmp = format_duration(m->duration);
		gtk_label_set_text(GTK_LABEL(w), tmp);
		break;
	case COL_PREVIEW:
		if (app_previewing(v->app, m)) {
			gtk_widget_add_css_class(w, "previewing");
			gtk_button_set_icon_name(GTK_BUTTON(w),
						 "media-playback-stop-symbolic");
		} else {
			gtk_widget_remove_css_class(w, "previewing");
			gtk_button_set_icon_name(GTK_BUTTON(w),
						 "audio-headphones-symbolic");
		}
		break;
	}
	g_free(tmp);
}

static void bind_cell(GtkListItemFactory *f, GtkListItem *li, gpointer data)
{
	bind_item(g_object_get_data(G_OBJECT(f), "view"), li,
		  GPOINTER_TO_INT(data));
}

/* An item's state changed somewhere: refresh the cell showing it. */
static void on_item_changed(GObject *watch, const char *key, GtkListItem *li)
{
	PdMediaItem *m = gtk_list_item_get_item(li);
	PdMediaView *v = g_object_get_data(G_OBJECT(li), "view");

	if (!m || !v)
		return;
	if (g_str_equal(key, "*") || g_strcmp0(m->key, key) == 0)
		bind_item(v, li, GPOINTER_TO_INT(
				g_object_get_data(G_OBJECT(li), "col")));
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
	gtk_column_view_column_set_resizable(c, expand);
	if (width > 0)
		gtk_column_view_column_set_fixed_width(c, width);
	gtk_column_view_append_column(GTK_COLUMN_VIEW(v->view), c);
	if (sorter)
		g_object_unref(sorter);
	g_object_unref(c);
}

/* ---- loading ----------------------------------------------------- */

/* Every selected item, in list order; the array holds references. */
static GPtrArray *selected_all(PdMediaView *v)
{
	GtkSelectionModel *sel = GTK_SELECTION_MODEL(v->selection);
	GtkBitset *set = gtk_selection_model_get_selection(sel);
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GtkBitsetIter it;
	guint pos;

	if (gtk_bitset_iter_init_first(&it, set, &pos)) {
		do {
			PdMediaItem *m = g_list_model_get_item(
						G_LIST_MODEL(sel), pos);

			if (m)
				g_ptr_array_add(out, m);
		} while (gtk_bitset_iter_next(&it, &pos));
	}
	gtk_bitset_unref(set);
	return out;
}

/* The first selected item, borrowed, NULL when nothing is selected. */
static PdMediaItem *selected(PdMediaView *v)
{
	GPtrArray *all = selected_all(v);
	PdMediaItem *m = all->len ? all->pdata[0] : NULL;

	g_ptr_array_unref(all);
	return m;
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

void pd_media_view_queue_selected(PdMediaView *v)
{
	GPtrArray *all = selected_all(v);
	guint i;

	for (i = 0; i < all->len; i++)
		g_list_store_append(v->app->queue, all->pdata[i]);
	if (all->len == 1)
		app_toast(v->app, "Queued \"%s\"",
			  ((PdMediaItem *)all->pdata[0])->title);
	else if (all->len > 1)
		app_toast(v->app, "Queued %u tracks", all->len);
	g_ptr_array_unref(all);
}

static void on_load_deck(GSimpleAction *a, GVariant *p, gpointer data)
{
	pd_media_view_load_selected(data, g_variant_get_int32(p));
}

static void on_queue(GSimpleAction *a, GVariant *p, gpointer data)
{
	pd_media_view_queue_selected(data);
}

static void on_preview(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdMediaView *v = data;
	PdMediaItem *m = selected(v);

	if (m)
		app_preview(v->app, m);
}

static void on_cache(GSimpleAction *a, GVariant *p, gpointer data)
{
	GPtrArray *all = selected_all(data);
	guint i;

	for (i = 0; i < all->len; i++)
		sccache_fetch(all->pdata[i]);
	g_ptr_array_unref(all);
}

static void on_add_to_playlist(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdMediaView *v = data;
	GPtrArray *all = selected_all(v);
	const char *name = g_variant_get_string(p, NULL);
	struct playlist *pl = playlists_find(name);
	guint i;

	for (i = 0; pl && i < all->len; i++)
		playlists_add(pl, all->pdata[i]);
	if (pl && all->len)
		app_toast(v->app, "Added %u track%s to \"%s\"", all->len,
			  all->len == 1 ? "" : "s", name);
	g_ptr_array_unref(all);
}

static void on_remove_from_playlist(GSimpleAction *a, GVariant *p,
				    gpointer data)
{
	PdMediaView *v = data;
	PdMediaItem *m = selected(v);
	guint n, i;

	if (!m || !v->playlist)
		return;
	n = g_list_model_get_n_items(G_LIST_MODEL(v->playlist->items));
	for (i = 0; i < n; i++) {
		PdMediaItem *x = g_list_model_get_item(
				G_LIST_MODEL(v->playlist->items), i);

		g_object_unref(x);
		if (x == m) {
			playlists_remove(v->playlist, i);
			return;
		}
	}
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

static void on_state_changed(GObject *watch, const char *key, PdMediaView *v)
{
	if (v->played_only)
		gtk_filter_changed(gtk_filter_list_model_get_filter(v->filtered),
				   GTK_FILTER_CHANGE_DIFFERENT);
}

static void on_played_only(GtkToggleButton *b, PdMediaView *v)
{
	v->played_only = gtk_toggle_button_get_active(b);
	gtk_filter_changed(gtk_filter_list_model_get_filter(v->filtered),
			   v->played_only ? GTK_FILTER_CHANGE_MORE_STRICT :
			   GTK_FILTER_CHANGE_LESS_STRICT);
}

/* The track's SoundCloud address, for a browser or another app. */
static void on_copy_link(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdMediaView *v = data;
	PdMediaItem *m = selected(v);
	GdkClipboard *clip;

	if (!m || !m->permalink)
		return;
	clip = gtk_widget_get_clipboard(GTK_WIDGET(v));
	gdk_clipboard_set_text(clip, m->permalink);
	app_toast(v->app, "Link copied: %s", m->permalink);
}

GtkWidget *pd_media_view_played_button(PdMediaView *v)
{
	GtkWidget *b = gtk_toggle_button_new_with_label("Played");

	gtk_widget_add_css_class(b, "played-button");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_tooltip_text(b, "Only list the tracks played in "
				    "this session");
	g_signal_connect(b, "toggled", G_CALLBACK(on_played_only), v);
	return b;
}

GtkWidget *pd_media_view_queue_button(PdMediaView *v)
{
	GtkWidget *b = gtk_button_new_with_label("+ Queue");

	gtk_widget_add_css_class(b, "queue-button");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_tooltip_text(b, "Add the selected tracks to the "
				    "automix queue (Ctrl or Shift click "
				    "selects several)");
	g_signal_connect_swapped(b, "clicked",
				 G_CALLBACK(pd_media_view_queue_selected), v);
	return b;
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
	g_menu_append(menu, "Pre-listen in headphones", "media.preview");
	g_menu_append(menu, "Add to automix queue", "media.queue");
	{
		GPtrArray *lists = playlists_all();
		GMenu *sub = g_menu_new();
		guint k;

		for (k = 0; lists && k < lists->len; k++) {
			struct playlist *pl = lists->pdata[k];
			GMenuItem *item = g_menu_item_new(pl->name, NULL);

			g_menu_item_set_action_and_target(item,
					"media.playlist", "s", pl->name);
			g_menu_append_item(sub, item);
			g_object_unref(item);
		}
		if (lists && lists->len)
			g_menu_append_submenu(menu, "Add to playlist",
					      G_MENU_MODEL(sub));
		g_object_unref(sub);
		if (v->playlist)
			g_menu_append(menu, "Remove from this playlist",
				      "media.unplaylist");
	}
	if (v->store == v->app->sc_results) {
		g_menu_append(menu, "Download to cache", "media.cache");
		g_menu_append(menu, "Open on SoundCloud", "media.open-link");
		g_menu_append(menu, "Copy SoundCloud link", "media.copy-link");
	}
	return G_MENU_MODEL(menu);
}

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

static GtkWidget *build_popover(PdMediaView *v)
{
	GSimpleActionGroup *grp = g_simple_action_group_new();
	static const GActionEntry entries[] = {
		{ "load", on_load_deck, "i" },
		{ "queue", on_queue },
		{ "preview", on_preview },
		{ "playlist", on_add_to_playlist, "s" },
		{ "unplaylist", on_remove_from_playlist },
		{ "cache", on_cache },
		{ "open-link", on_open_link },
		{ "copy-link", on_copy_link },
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

/* ---- construction ------------------------------------------------ */

static void on_search(GtkEditable *e, PdMediaView *v)
{
	pd_media_view_set_filter(v, gtk_editable_get_text(e));
}

static void on_selected(GtkSelectionModel *sel, guint pos, guint n,
			PdMediaView *v)
{
	PdMediaItem *m = selected(v);

	if (!m)
		return;
	g_free(v->sel_key);
	v->sel_key = g_strdup(m->key);
}

/*
 * Refreshing a row (played mark, cache state) re-emits its item, which
 * makes the selection drop it; put it back when it is still in the
 * changed range and nothing else is selected.
 */
static void reselect(PdMediaView *v, GListModel *m, guint pos, guint add)
{
	GtkBitset *set = gtk_selection_model_get_selection(
				GTK_SELECTION_MODEL(v->selection));
	gboolean empty = gtk_bitset_is_empty(set);
	guint i;

	gtk_bitset_unref(set);
	if (!v->sel_key || !empty)
		return;
	for (i = pos; i < pos + add; i++) {
		PdMediaItem *x = g_list_model_get_item(m, i);
		gboolean hit = x && g_strcmp0(x->key, v->sel_key) == 0;

		g_clear_object(&x);
		if (hit) {
			gtk_selection_model_select_item(
				GTK_SELECTION_MODEL(v->selection), i, TRUE);
			return;
		}
	}
}

static void on_items_changed(GListModel *m, guint pos, guint rm, guint add,
			     PdMediaView *v)
{
	guint n = g_list_model_get_n_items(m);
	guint total = g_list_model_get_n_items(G_LIST_MODEL(v->store));
	char *s;

	reselect(v, m, pos, add);
	if (n == total)
		s = g_strdup_printf("%u track%s", n, n == 1 ? "" : "s");
	else
		s = g_strdup_printf("%u of %u tracks", n, total);
	gtk_label_set_text(GTK_LABEL(v->count), s);
	g_free(s);
	gtk_stack_set_visible_child_name(GTK_STACK(v->stack),
					 total ? "list" : "empty");
}

static void pd_media_view_dispose(GObject *obj)
{
	PdMediaView *v = PD_MEDIA_VIEW(obj);
	GtkWidget *pop = g_object_steal_data(obj, "popover");

	/* the popover is a foreign child of the column view */
	if (pop)
		gtk_widget_unparent(pop);
	if (v->selection)
		g_signal_handlers_disconnect_by_data(v->selection, v);
	g_clear_object(&v->selection);
	G_OBJECT_CLASS(pd_media_view_parent_class)->dispose(obj);
}

static void pd_media_view_finalize(GObject *obj)
{
	PdMediaView *v = PD_MEDIA_VIEW(obj);

	g_strfreev(v->terms);
	g_free(v->key_filter);
	g_free(v->sel_key);
	G_OBJECT_CLASS(pd_media_view_parent_class)->finalize(obj);
}

static void pd_media_view_class_init(PdMediaViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_media_view_dispose;
	G_OBJECT_CLASS(klass)->finalize = pd_media_view_finalize;
}

static void pd_media_view_init(PdMediaView *v)
{
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
	g_signal_connect_object(pd_media_watch(), "changed",
				G_CALLBACK(on_state_changed), v, 0);
	v->filtered = gtk_filter_list_model_new(
			G_LIST_MODEL(g_object_ref(store)), GTK_FILTER(filter));
	gtk_filter_list_model_set_incremental(v->filtered, TRUE);

	v->view = gtk_column_view_new(NULL);
	v->sorted = gtk_sort_list_model_new(G_LIST_MODEL(v->filtered),
			g_object_ref(gtk_column_view_get_sorter(
					GTK_COLUMN_VIEW(v->view))));
	v->selection = gtk_multi_selection_new(G_LIST_MODEL(v->sorted));
	gtk_column_view_set_model(GTK_COLUMN_VIEW(v->view),
				  GTK_SELECTION_MODEL(v->selection));
	gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(v->view),
						  FALSE);
	gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(v->view),
						FALSE);
	gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(v->view), FALSE);
	gtk_widget_add_css_class(v->view, "data-table");

#define STR_SORTER(field) \
	GTK_SORTER(gtk_custom_sorter_new(cmp_str, \
		GSIZE_TO_POINTER(G_STRUCT_OFFSET(PdMediaItem, field)), NULL))
#define NUM_SORTER(field) \
	GTK_SORTER(gtk_custom_sorter_new(cmp_double, \
		GSIZE_TO_POINTER(G_STRUCT_OFFSET(PdMediaItem, field)), NULL))
	add_column(v, "", COL_STATE, NULL, FALSE, 56);
	add_column(v, "Title", COL_TRACK, STR_SORTER(title), TRUE, 0);
	add_column(v, "Artist", COL_ARTIST, STR_SORTER(artist), TRUE, 0);
	add_column(v, "Album", COL_ALBUM, STR_SORTER(album), TRUE, 0);
	add_column(v, "Genre", COL_GENRE, STR_SORTER(genre), FALSE, 110);
	add_column(v, "Key", COL_KEY,
		   GTK_SORTER(gtk_custom_sorter_new(cmp_key, NULL, NULL)),
		   FALSE, 92);
	add_column(v, "BPM", COL_BPM,
		   GTK_SORTER(gtk_custom_sorter_new(cmp_bpm, NULL, NULL)),
		   FALSE, 64);
	add_column(v, "Length", COL_DURATION, NUM_SORTER(duration), FALSE,
		   72);
	add_column(v, "", COL_PREVIEW, NULL, FALSE, 44);
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
	gtk_widget_add_css_class(v->stack, "media-stack");
	gtk_stack_add_named(GTK_STACK(v->stack), scroll, "list");
	gtk_stack_add_named(GTK_STACK(v->stack), empty, "empty");
	gtk_widget_set_vexpand(v->stack, TRUE);
	gtk_box_append(GTK_BOX(v), v->stack);

	v->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v->search),
					      sc ? "Filter results" :
					      "Filter: words, bpm:120-128, "
					      "key:8A, genre:techno");
	g_signal_connect(v->search, "changed", G_CALLBACK(on_search), v);
	v->count = gtk_label_new("");
	gtk_widget_add_css_class(v->count, "dim-label");
	gtk_widget_add_css_class(v->count, "row-count");
	gtk_box_append(GTK_BOX(v), v->count);
	g_signal_connect(v->selection, "items-changed",
			 G_CALLBACK(on_items_changed), v);
	g_signal_connect(v->selection, "selection-changed",
			 G_CALLBACK(on_selected), v);
	on_items_changed(G_LIST_MODEL(v->selection), 0, 0, 0, v);
	return GTK_WIDGET(v);
}

GtkWidget *pd_media_view_search_entry(PdMediaView *v)
{
	return v->search;
}

GListStore *pd_media_view_store(PdMediaView *v)
{
	return v->store;
}

void pd_media_view_set_store(PdMediaView *v, GListStore *store,
			     struct playlist *playlist)
{
	GtkFilter *filter = gtk_filter_list_model_get_filter(v->filtered);

	if (store == v->store && playlist == v->playlist)
		return;
	g_signal_handlers_disconnect_by_data(v->selection, v);
	v->store = store;
	v->playlist = playlist;
	/*
	 * The sort and selection models take ownership of the model they
	 * wrap, so only the selection is ours to drop; it releases the
	 * chain.  The filter is kept alive across the swap.
	 */
	g_object_ref(filter);
	g_clear_object(&v->selection);
	v->filtered = gtk_filter_list_model_new(
			G_LIST_MODEL(g_object_ref(store)), filter);
	gtk_filter_list_model_set_incremental(v->filtered, TRUE);
	v->sorted = gtk_sort_list_model_new(G_LIST_MODEL(v->filtered),
			g_object_ref(gtk_column_view_get_sorter(
					GTK_COLUMN_VIEW(v->view))));
	v->selection = gtk_multi_selection_new(G_LIST_MODEL(v->sorted));
	gtk_column_view_set_model(GTK_COLUMN_VIEW(v->view),
				  GTK_SELECTION_MODEL(v->selection));
	g_signal_connect(v->selection, "items-changed",
			 G_CALLBACK(on_items_changed), v);
	g_signal_connect(v->selection, "selection-changed",
			 G_CALLBACK(on_selected), v);
	on_items_changed(G_LIST_MODEL(v->selection), 0, 0, 0, v);
}

void pd_media_view_set_filter(PdMediaView *v, const char *text)
{
	char *fold = g_utf8_casefold(text ? text : "", -1);
	char **words = g_strsplit(fold, " ", -1);
	GPtrArray *plain = g_ptr_array_new();
	int i;

	g_strfreev(v->terms);
	v->terms = NULL;
	v->bpm_lo = v->bpm_hi = 0.0;
	g_clear_pointer(&v->key_filter, g_free);

	for (i = 0; words[i]; i++) {
		char *w = words[i];

		if (!*w)
			continue;
		if (g_str_has_prefix(w, "bpm:")) {
			double lo = g_ascii_strtod(w + 4, NULL);
			const char *dash = strchr(w + 4, '-');

			v->bpm_lo = lo - (dash ? 0.0 : 2.0);
			v->bpm_hi = dash ? g_ascii_strtod(dash + 1, NULL) :
				    lo + 2.0;
			if (v->bpm_hi < v->bpm_lo)
				v->bpm_hi = v->bpm_lo;
		} else if (g_str_has_prefix(w, "key:")) {
			v->key_filter = g_strdup(w + 4);
		} else if (g_str_has_prefix(w, "genre:")) {
			g_ptr_array_add(plain, g_strdup(w + 6));
		} else {
			g_ptr_array_add(plain, g_strdup(w));
		}
	}
	if (plain->len) {
		g_ptr_array_add(plain, NULL);
		v->terms = (char **)g_ptr_array_free(plain, FALSE);
	} else {
		g_ptr_array_free(plain, TRUE);
	}
	g_strfreev(words);
	g_free(fold);
	gtk_filter_changed(gtk_filter_list_model_get_filter(v->filtered),
			   GTK_FILTER_CHANGE_DIFFERENT);
}

guint pd_media_view_count(PdMediaView *v)
{
	return g_list_model_get_n_items(G_LIST_MODEL(v->selection));
}
