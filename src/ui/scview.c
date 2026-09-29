// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * scview.c - SoundCloud search panel
 *
 * The search entry doubles as a link box: a soundcloud.com URL resolves
 * to the track, playlist or artist behind it.
 */
#include <string.h>

#include "sclogin.h"
#include "scview.h"
#include "soundcloud.h"

struct _PdScView {
	GtkBox parent;
	struct app *app;
	PdMediaView *media;
	GtkWidget *entry;
	GtkWidget *spinner;
	GtkWidget *likes;
	GtkWidget *playlists;
	GtkWidget *login;
	GtkWidget *full;
	GtkWidget *hint;
	GtkWidget *stack;		/* "tracks" or "playlists" */
	GtkWidget *filter;
	GListStore *pl_store;		/* PdScPlaylist */
	guint pending;
};

static void update_login(PdScView *v);

G_DEFINE_FINAL_TYPE(PdScView, pd_sc_view, GTK_TYPE_BOX)

enum query_kind {
	QUERY_SEARCH,
	QUERY_RESOLVE,
	QUERY_LIKES,
	QUERY_PLAYLISTS,	/* the user's playlists, to the list */
	QUERY_PL_OPEN,		/* one playlist's tracks, to the results */
	QUERY_PL_QUEUE,		/* one playlist's tracks, to the automix */
};

struct query {
	PdScView *view;
	enum query_kind kind;
	char *text;
	PdScPlaylist *pl;
	GPtrArray *result;
	GError *err;
};

static void show_page(PdScView *v, const char *name)
{
	gtk_stack_set_visible_child_name(GTK_STACK(v->stack), name);
	gtk_widget_set_visible(v->filter, g_str_equal(name, "tracks"));
}

static void query_free(struct query *q)
{
	g_free(q->text);
	g_clear_object(&q->pl);
	if (q->result)
		g_ptr_array_unref(q->result);
	g_clear_error(&q->err);
	g_object_unref(q->view);
	g_free(q);
}

static gboolean query_done(gpointer data)
{
	struct query *q = data;
	PdScView *v = q->view;
	struct app *a = v->app;

	v->pending--;
	gtk_widget_set_visible(v->spinner, v->pending > 0);
	if (!q->result) {
		if (q->err)
			app_toast(a, "SoundCloud: %s", q->err->message);
		query_free(q);
		return G_SOURCE_REMOVE;
	}
	switch (q->kind) {
	case QUERY_PLAYLISTS:
		g_list_store_splice(v->pl_store, 0,
				    g_list_model_get_n_items(
						G_LIST_MODEL(v->pl_store)),
				    q->result->pdata, q->result->len);
		if (q->result->len == 0)
			app_toast(a, "SoundCloud: you have no playlists");
		show_page(v, "playlists");
		break;
	case QUERY_PL_QUEUE: {
		guint i, skipped = q->pl->all - q->result->len;

		for (i = 0; i < q->result->len; i++)
			g_list_store_append(a->queue, q->result->pdata[i]);
		if (skipped)
			app_toast(a, "Queued %u tracks from \"%s\", %u not "
				  "playable left out", q->result->len,
				  q->pl->title, skipped);
		else
			app_toast(a, "Queued %u tracks from \"%s\"",
				  q->result->len, q->pl->title);
		break;
	}
	default:
		g_list_store_splice(a->sc_results, 0,
				    g_list_model_get_n_items(
						G_LIST_MODEL(a->sc_results)),
				    q->result->pdata, q->result->len);
		if (q->result->len == 0)
			app_toast(a, "SoundCloud: nothing found");
		show_page(v, "tracks");
		break;
	}
	query_free(q);
	return G_SOURCE_REMOVE;
}

static gpointer query_thread(gpointer data)
{
	struct query *q = data;

	switch (q->kind) {
	case QUERY_SEARCH:
		q->result = sc_search(q->text, &q->err);
		break;
	case QUERY_RESOLVE:
		q->result = sc_resolve(q->text, &q->err);
		break;
	case QUERY_LIKES:
		q->result = sc_likes(&q->err);
		break;
	case QUERY_PLAYLISTS:
		q->result = sc_playlists(&q->err);
		break;
	case QUERY_PL_OPEN:
	case QUERY_PL_QUEUE:
		q->result = sc_playlist_tracks(q->pl, &q->err);
		break;
	}
	g_idle_add(query_done, q);
	return NULL;
}

static void run_query_pl(PdScView *v, enum query_kind kind,
			 const char *text, PdScPlaylist *pl)
{
	struct query *q = g_new0(struct query, 1);

	q->view = g_object_ref(v);
	q->kind = kind;
	q->text = g_strdup(text);
	q->pl = pl ? g_object_ref(pl) : NULL;
	v->pending++;
	gtk_widget_set_visible(v->spinner, TRUE);
	g_thread_unref(g_thread_new("pd-sc-query", query_thread, q));
}

static void run_query(PdScView *v, enum query_kind kind, const char *text)
{
	run_query_pl(v, kind, text, NULL);
}

static gboolean is_sc_link(const char *s)
{
	return strstr(s, "soundcloud.com/") != NULL ||
	       strstr(s, "on.soundcloud.com/") != NULL;
}

static void on_activate(GtkEntry *e, PdScView *v)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(e));
	char *trim = g_strstrip(g_strdup(text));

	if (*trim)
		run_query(v, is_sc_link(trim) ? QUERY_RESOLVE : QUERY_SEARCH,
			  trim);
	g_free(trim);
}

static void on_likes(GtkButton *b, PdScView *v)
{
	run_query(v, QUERY_LIKES, NULL);
}

/* The Playlists button shows the list, fetching it the first time. */
static void on_playlists(GtkButton *b, PdScView *v)
{
	if (g_list_model_get_n_items(G_LIST_MODEL(v->pl_store)) > 0)
		show_page(v, "playlists");
	else
		run_query(v, QUERY_PLAYLISTS, NULL);
}

static void on_pl_reload(GtkButton *b, PdScView *v)
{
	run_query(v, QUERY_PLAYLISTS, NULL);
}

static void on_pl_back(GtkButton *b, PdScView *v)
{
	show_page(v, "tracks");
}

/* ---- playlist rows ----------------------------------------------- */

static PdScPlaylist *row_playlist(GtkListItem *li)
{
	return gtk_list_item_get_item(li);
}

static void on_pl_open(GtkButton *b, GtkListItem *li)
{
	PdScView *v = g_object_get_data(G_OBJECT(b), "view");
	PdScPlaylist *p = row_playlist(li);

	if (p)
		run_query_pl(v, QUERY_PL_OPEN, NULL, p);
}

static void on_pl_queue(GtkButton *b, GtkListItem *li)
{
	PdScView *v = g_object_get_data(G_OBJECT(b), "view");
	PdScPlaylist *p = row_playlist(li);

	if (p)
		run_query_pl(v, QUERY_PL_QUEUE, NULL, p);
}

static void on_pl_activate(GtkListView *lv, guint pos, PdScView *v)
{
	PdScPlaylist *p = g_list_model_get_item(
			G_LIST_MODEL(gtk_list_view_get_model(lv)), pos);

	if (p) {
		run_query_pl(v, QUERY_PL_OPEN, NULL, p);
		g_object_unref(p);
	}
}

static void setup_pl_row(GtkListItemFactory *f, GtkListItem *li,
			 gpointer data)
{
	PdScView *v = data;
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	GtkWidget *title = gtk_label_new("");
	GtkWidget *sub = gtk_label_new("");
	GtkWidget *b;

	gtk_widget_add_css_class(title, "row-title");
	gtk_widget_add_css_class(sub, "row-sub");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(text), title);
	gtk_box_append(GTK_BOX(text), sub);
	gtk_box_append(GTK_BOX(row), text);

	b = gtk_button_new_with_label("Open");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(b, "Show the playlist's tracks");
	g_object_set_data(G_OBJECT(b), "view", v);
	g_signal_connect(b, "clicked", G_CALLBACK(on_pl_open), li);
	gtk_box_append(GTK_BOX(row), b);
	b = gtk_button_new_with_label("+ Automix");
	gtk_widget_add_css_class(b, "queue-button");
	gtk_widget_set_focusable(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(b, "Add the whole playlist to the "
				    "automix queue; tracks that cannot be "
				    "played are left out");
	g_object_set_data(G_OBJECT(b), "view", v);
	g_signal_connect(b, "clicked", G_CALLBACK(on_pl_queue), li);
	gtk_box_append(GTK_BOX(row), b);
	gtk_list_item_set_child(li, row);
}

static void bind_pl_row(GtkListItemFactory *f, GtkListItem *li,
			gpointer data)
{
	PdScPlaylist *p = row_playlist(li);
	GtkWidget *text = gtk_widget_get_first_child(gtk_list_item_get_child(li));
	GtkWidget *title = gtk_widget_get_first_child(text);
	GtkWidget *sub = gtk_widget_get_next_sibling(title);
	char *s;

	gtk_label_set_text(GTK_LABEL(title), p->title);
	s = g_strdup_printf("%u track%s%s%s%s", p->count,
			    p->count == 1 ? "" : "s",
			    p->liked ? "  ·  liked" : "",
			    p->user && *p->user ? "  ·  by " : "",
			    p->user && *p->user ? p->user : "");
	gtk_label_set_text(GTK_LABEL(sub), s);
	g_free(s);
}

static GtkWidget *build_playlists(PdScView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *l, *b, *list, *scroll;
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	GtkSingleSelection *sel;

	l = gtk_label_new("Your playlists");
	gtk_widget_add_css_class(l, "section-label");
	gtk_widget_set_hexpand(l, TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_widget_set_margin_start(l, 8);
	gtk_box_append(GTK_BOX(bar), l);
	b = gtk_button_new_with_label("Reload");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_pl_reload), v);
	gtk_box_append(GTK_BOX(bar), b);
	b = gtk_button_new_with_label("Back to tracks");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_pl_back), v);
	gtk_box_append(GTK_BOX(bar), b);
	gtk_box_append(GTK_BOX(box), bar);

	v->pl_store = g_list_store_new(PD_TYPE_SC_PLAYLIST);
	sel = gtk_single_selection_new(
			G_LIST_MODEL(g_object_ref(v->pl_store)));
	gtk_single_selection_set_autoselect(sel, FALSE);
	g_signal_connect(f, "setup", G_CALLBACK(setup_pl_row), v);
	g_signal_connect(f, "bind", G_CALLBACK(bind_pl_row), v);
	list = gtk_list_view_new(GTK_SELECTION_MODEL(sel), f);
	gtk_widget_add_css_class(list, "queue-list");
	g_signal_connect(list, "activate", G_CALLBACK(on_pl_activate), v);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);
	return box;
}

/* Full tracks only: rerun the current search under the new filter. */
static void on_full(GtkToggleButton *b, PdScView *v)
{
	struct app *a = v->app;

	a->cfg.sc_full_only = gtk_toggle_button_get_active(b);
	sc_set_full_only(a->cfg.sc_full_only);
	config_save(&a->cfg);
	on_activate(GTK_ENTRY(v->entry), v);
}

static void login_done(gpointer data)
{
	PdScView *v = data;

	update_login(v);
	run_query(v, QUERY_LIKES, NULL);
}

static void on_login(GtkButton *b, PdScView *v)
{
	if (sc_session_logged_in()) {
		sclogin_logout(v->app);
		update_login(v);
		return;
	}
	sclogin_show(v->app, login_done, v);
}

static void update_login(PdScView *v)
{
	gboolean in = sc_session_logged_in();

	gtk_button_set_label(GTK_BUTTON(v->login), in ? "Log out" :
			     "Log in to SoundCloud");
	gtk_widget_set_visible(v->likes, in);
	gtk_widget_set_visible(v->playlists, in);
	if (in)
		gtk_widget_add_css_class(v->login, "logged-in");
	else
		gtk_widget_remove_css_class(v->login, "logged-in");
}

static void pd_sc_view_class_init(PdScViewClass *klass)
{
}

static void pd_sc_view_init(PdScView *v)
{
}

GtkWidget *pd_sc_view_new(struct app *app)
{
	PdScView *v = g_object_new(PD_TYPE_SC_VIEW,
				   "orientation", GTK_ORIENTATION_VERTICAL,
				   "spacing", 4, NULL);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *media, *filter;

	v->app = app;
	media = pd_media_view_new(app, app->sc_results,
				  "Search SoundCloud above, or paste a "
				  "track, playlist or artist link and press "
				  "Enter.\n\nLog in to load your likes and "
				  "playlists.");
	v->media = PD_MEDIA_VIEW(media);

	v->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(v->entry),
				       "Search SoundCloud, or paste a link "
				       "and press Enter");
	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(v->entry),
					  GTK_ENTRY_ICON_PRIMARY,
					  "system-search-symbolic");
	gtk_widget_set_hexpand(v->entry, TRUE);
	g_signal_connect(v->entry, "activate", G_CALLBACK(on_activate), v);
	gtk_box_append(GTK_BOX(bar), v->entry);

	v->full = gtk_toggle_button_new_with_label("Full tracks");
	gtk_widget_add_css_class(v->full, "sc-full");
	gtk_widget_set_focusable(v->full, FALSE);
	gtk_widget_set_tooltip_text(v->full, "Only list tracks SoundCloud "
				    "lets this app play in full. Off: "
				    "tracks with a preview snippet are "
				    "listed too.");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->full),
				     app->cfg.sc_full_only);
	g_signal_connect(v->full, "toggled", G_CALLBACK(on_full), v);
	gtk_box_append(GTK_BOX(bar), v->full);
	gtk_box_append(GTK_BOX(bar), pd_media_view_queue_button(v->media));
	v->login = gtk_button_new_with_label("Log in to SoundCloud");
	gtk_widget_add_css_class(v->login, "sc-login");
	gtk_widget_set_focusable(v->login, FALSE);
	gtk_widget_set_tooltip_text(v->login, "Sign in with your SoundCloud "
				    "account to reach your likes and "
				    "playlists");
	g_signal_connect(v->login, "clicked", G_CALLBACK(on_login), v);
	gtk_box_append(GTK_BOX(bar), v->login);
	v->likes = gtk_button_new_with_label("♥ Likes");
	gtk_widget_set_tooltip_text(v->likes, "Load your liked tracks");
	gtk_widget_set_focusable(v->likes, FALSE);
	g_signal_connect(v->likes, "clicked", G_CALLBACK(on_likes), v);
	gtk_box_append(GTK_BOX(bar), v->likes);
	v->playlists = gtk_button_new_with_label("Playlists");
	gtk_widget_set_tooltip_text(v->playlists, "Your own and liked "
				    "playlists: open one or add it whole to "
				    "the automix queue");
	gtk_widget_set_focusable(v->playlists, FALSE);
	g_signal_connect(v->playlists, "clicked", G_CALLBACK(on_playlists), v);
	gtk_box_append(GTK_BOX(bar), v->playlists);

	v->spinner = gtk_spinner_new();
	gtk_spinner_set_spinning(GTK_SPINNER(v->spinner), TRUE);
	gtk_widget_set_visible(v->spinner, FALSE);
	gtk_box_append(GTK_BOX(bar), v->spinner);
	gtk_box_append(GTK_BOX(v), bar);

	/* The media view's own entry filters the results locally. */
	filter = pd_media_view_search_entry(v->media);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(filter),
					      "Filter results");
	gtk_box_append(GTK_BOX(v), filter);
	v->filter = filter;
	v->stack = gtk_stack_new();
	gtk_widget_set_vexpand(v->stack, TRUE);
	gtk_stack_add_named(GTK_STACK(v->stack), media, "tracks");
	gtk_stack_add_named(GTK_STACK(v->stack), build_playlists(v),
			    "playlists");
	gtk_box_append(GTK_BOX(v), v->stack);
	update_login(v);
	return GTK_WIDGET(v);
}

PdMediaView *pd_sc_view_media(PdScView *v)
{
	return v->media;
}
