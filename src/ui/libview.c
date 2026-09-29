// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * libview.c - local music library panel
 *
 * A sidebar switches the list between the whole library, the play
 * history of this session and the playlists (crates).
 */
#include "library.h"
#include "libview.h"
#include "playlists.h"

struct _PdLibView {
	GtkBox parent;
	struct app *app;
	PdMediaView *media;
	GtkWidget *sidebar;
	GtkWidget *spinner;
	GtkWidget *rescan;
	GtkWidget *add_folder;
	GtkWidget *list_tools;		/* playlist / history actions */
	GtkWidget *export_btn;
	GtkWidget *delete_btn;
	GCancellable *cancel;
	struct playlist *current;	/* NULL for library or history */
	gboolean showing_history;
};

G_DEFINE_FINAL_TYPE(PdLibView, pd_lib_view, GTK_TYPE_BOX)

/* ---- scanning ---------------------------------------------------- */

static void scan_done(GObject *src, GAsyncResult *res, gpointer data)
{
	PdLibView *v = data;
	GError *err = NULL;
	GPtrArray *items = library_scan_finish(res, &err);

	/* A cancelled scan may outlive the widgets, touch nothing. */
	if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error(&err);
		g_object_unref(v);
		return;
	}
	gtk_widget_set_visible(v->spinner, FALSE);
	gtk_widget_set_sensitive(v->rescan, TRUE);
	g_clear_object(&v->cancel);
	if (!items) {
		app_toast(v->app, "Library scan failed: %s", err->message);
		g_clear_error(&err);
		g_object_unref(v);
		return;
	}
	g_list_store_splice(v->app->library, 0,
			    g_list_model_get_n_items(
					G_LIST_MODEL(v->app->library)),
			    items->pdata, items->len);
	g_ptr_array_unref(items);
	g_object_unref(v);
}

void pd_lib_view_rescan(PdLibView *v)
{
	if (v->cancel)
		g_cancellable_cancel(v->cancel);
	v->cancel = g_cancellable_new();
	gtk_widget_set_visible(v->spinner, TRUE);
	gtk_widget_set_sensitive(v->rescan, FALSE);
	library_scan_async(v->app->cfg.folders, v->cancel, scan_done,
			   g_object_ref(v));
}

static void on_rescan(GtkButton *b, PdLibView *v)
{
	pd_lib_view_rescan(v);
}

static void folder_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	PdLibView *v = data;
	GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
							res, NULL);
	char *path;
	char **folders;
	guint n;

	if (!f)
		return;
	path = g_file_get_path(f);
	g_object_unref(f);
	if (!path)
		return;
	if (g_strv_contains((const char *const *)v->app->cfg.folders,
			    path)) {
		g_free(path);
		return;
	}
	n = g_strv_length(v->app->cfg.folders);
	folders = g_realloc_n(v->app->cfg.folders, n + 2, sizeof(char *));
	folders[n] = path;
	folders[n + 1] = NULL;
	v->app->cfg.folders = folders;
	config_save(&v->app->cfg);
	pd_lib_view_rescan(v);
}

static void on_add_folder(GtkButton *b, PdLibView *v)
{
	GtkFileDialog *d = gtk_file_dialog_new();

	gtk_file_dialog_set_title(d, "Add music folder");
	gtk_file_dialog_select_folder(d, v->app->win, NULL, folder_chosen, v);
	g_object_unref(d);
}

/* ---- sidebar ----------------------------------------------------- */

enum side_kind {
	SIDE_LIBRARY,
	SIDE_HISTORY,
	SIDE_PLAYLIST,
};

static GtkWidget *side_row(const char *icon, const char *text,
			   enum side_kind kind, struct playlist *p)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *img = gtk_image_new_from_icon_name(icon);
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), img);
	gtk_box_append(GTK_BOX(box), l);
	gtk_widget_add_css_class(row, "side-row");
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(kind));
	g_object_set_data(G_OBJECT(row), "playlist", p);
	return row;
}

static void show_selection(PdLibView *v, enum side_kind kind,
			   struct playlist *p)
{
	v->current = NULL;
	v->showing_history = FALSE;
	switch (kind) {
	case SIDE_HISTORY:
		v->showing_history = TRUE;
		pd_media_view_set_store(v->media, history_items(), NULL);
		break;
	case SIDE_PLAYLIST:
		v->current = p;
		pd_media_view_set_store(v->media, p->items, p);
		break;
	case SIDE_LIBRARY:
	default:
		pd_media_view_set_store(v->media, v->app->library, NULL);
		break;
	}
	gtk_widget_set_visible(v->add_folder, kind == SIDE_LIBRARY);
	gtk_widget_set_visible(v->rescan, kind == SIDE_LIBRARY);
	gtk_widget_set_visible(v->list_tools, kind != SIDE_LIBRARY);
	gtk_widget_set_visible(v->delete_btn, kind == SIDE_PLAYLIST);
	gtk_button_set_label(GTK_BUTTON(v->export_btn),
			     kind == SIDE_HISTORY ? "Export set list…" :
			     "Export M3U…");
}

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, PdLibView *v)
{
	if (!row)
		return;
	show_selection(v, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row),
							    "kind")),
		       g_object_get_data(G_OBJECT(row), "playlist"));
}

static void rebuild_sidebar(PdLibView *v)
{
	GtkWidget *child;
	GPtrArray *lists = playlists_all();
	guint i;
	GtkListBoxRow *select = NULL;

	while ((child = gtk_widget_get_first_child(v->sidebar)))
		gtk_list_box_remove(GTK_LIST_BOX(v->sidebar), child);
	gtk_list_box_append(GTK_LIST_BOX(v->sidebar),
			    side_row("folder-music-symbolic", "Library",
				     SIDE_LIBRARY, NULL));
	gtk_list_box_append(GTK_LIST_BOX(v->sidebar),
			    side_row("document-open-recent-symbolic",
				     "History", SIDE_HISTORY, NULL));
	for (i = 0; lists && i < lists->len; i++) {
		struct playlist *p = lists->pdata[i];
		GtkWidget *row = side_row("view-list-symbolic", p->name,
					  SIDE_PLAYLIST, p);

		gtk_list_box_append(GTK_LIST_BOX(v->sidebar), row);
		if (p == v->current)
			select = GTK_LIST_BOX_ROW(row);
	}
	if (!select)
		select = gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->sidebar),
						       v->showing_history ?
						       1 : 0);
	gtk_list_box_select_row(GTK_LIST_BOX(v->sidebar), select);
}

static void playlists_changed(gpointer data)
{
	PdLibView *v = data;
	GPtrArray *lists = playlists_all();
	guint i;
	gboolean alive = FALSE;

	for (i = 0; lists && i < lists->len; i++)
		if (lists->pdata[i] == v->current)
			alive = TRUE;
	if (!alive)
		v->current = NULL;
	rebuild_sidebar(v);
}

static void on_create_clicked(GtkButton *ok, PdLibView *v)
{
	GtkWidget *entry = g_object_get_data(G_OBJECT(ok), "entry");
	GtkWidget *win = g_object_get_data(G_OBJECT(ok), "window");
	const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
	struct playlist *p;

	if (name && *name) {
		p = playlists_create(name);
		v->current = p;
		rebuild_sidebar(v);
	}
	gtk_window_destroy(GTK_WINDOW(win));
}

static void on_entry_activate(GtkEntry *e, gpointer data)
{
	gtk_widget_activate(g_object_get_data(G_OBJECT(e), "ok"));
}

static void on_new_playlist(GtkButton *b, PdLibView *v)
{
	/* GtkAlertDialog has no entry, so use a small window instead. */
	GtkWidget *win = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *entry = gtk_entry_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *ok, *cancel;

	gtk_window_set_title(GTK_WINDOW(win), "New playlist");
	gtk_window_set_transient_for(GTK_WINDOW(win), v->app->win);
	gtk_window_set_modal(GTK_WINDOW(win), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 320, -1);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Playlist name");
	gtk_widget_set_margin_top(box, 12);
	gtk_widget_set_margin_bottom(box, 12);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_box_append(GTK_BOX(box), entry);
	cancel = gtk_button_new_with_label("Cancel");
	ok = gtk_button_new_with_label("Create");
	gtk_widget_add_css_class(ok, "suggested-action");
	gtk_widget_set_hexpand(cancel, TRUE);
	gtk_widget_set_halign(cancel, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(row), cancel);
	gtk_box_append(GTK_BOX(row), ok);
	gtk_box_append(GTK_BOX(box), row);
	gtk_window_set_child(GTK_WINDOW(win), box);
	g_object_set_data(G_OBJECT(ok), "entry", entry);
	g_object_set_data(G_OBJECT(ok), "window", win);
	g_object_set_data(G_OBJECT(entry), "ok", ok);
	g_signal_connect_swapped(cancel, "clicked",
				 G_CALLBACK(gtk_window_destroy), win);
	g_signal_connect(ok, "clicked", G_CALLBACK(on_create_clicked), v);
	g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate),
			 NULL);
	gtk_window_present(GTK_WINDOW(win));
}

static void on_delete_playlist(GtkButton *b, PdLibView *v)
{
	if (!v->current)
		return;
	playlists_delete(v->current->name);
}

static void export_done(GObject *src, GAsyncResult *res, gpointer data)
{
	PdLibView *v = data;
	GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
	GError *err = NULL;
	char *path;
	gboolean ok;

	if (!f)
		return;
	path = g_file_get_path(f);
	g_object_unref(f);
	if (!path)
		return;
	if (v->showing_history)
		ok = history_export(path, &err);
	else if (v->current)
		ok = playlists_export_m3u(v->current, path, &err);
	else
		ok = TRUE;
	if (!ok) {
		app_toast(v->app, "Export failed: %s", err->message);
		g_clear_error(&err);
	} else {
		app_toast(v->app, "Exported to %s", path);
	}
	g_free(path);
}

static void on_export(GtkButton *b, PdLibView *v)
{
	GtkFileDialog *d = gtk_file_dialog_new();
	char *name = v->showing_history ? g_strdup("set list.txt") :
		     v->current ? g_strdup_printf("%s.m3u8", v->current->name) :
		     g_strdup("playlist.m3u8");

	gtk_file_dialog_set_initial_name(d, name);
	gtk_file_dialog_save(d, v->app->win, NULL, export_done, v);
	g_object_unref(d);
	g_free(name);
}

/* ---- widget ------------------------------------------------------ */

static void pd_lib_view_dispose(GObject *obj)
{
	PdLibView *v = PD_LIB_VIEW(obj);

	playlists_set_changed_cb(NULL, NULL);
	if (v->cancel) {
		g_cancellable_cancel(v->cancel);
		g_clear_object(&v->cancel);
	}
	G_OBJECT_CLASS(pd_lib_view_parent_class)->dispose(obj);
}

static void pd_lib_view_class_init(PdLibViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_lib_view_dispose;
}

static void pd_lib_view_init(PdLibView *v)
{
}

GtkWidget *pd_lib_view_new(struct app *app)
{
	PdLibView *v = g_object_new(PD_TYPE_LIB_VIEW,
				    "orientation", GTK_ORIENTATION_HORIZONTAL,
				    "spacing", 0, NULL);
	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b, *media, *scroll;

	v->app = app;
	media = pd_media_view_new(app, app->library,
				  "No music found.\nAdd a folder with the "
				  "button above, or drop audio files onto a "
				  "deck.");
	v->media = PD_MEDIA_VIEW(media);

	/* sidebar */
	v->sidebar = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(v->sidebar),
					GTK_SELECTION_SINGLE);
	gtk_widget_add_css_class(v->sidebar, "sidebar");
	g_signal_connect(v->sidebar, "row-selected",
			 G_CALLBACK(on_row_selected), v);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v->sidebar);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_widget_set_size_request(scroll, 170, -1);
	gtk_box_append(GTK_BOX(side), scroll);
	b = gtk_button_new_with_label("+ New playlist");
	gtk_widget_add_css_class(b, "side-new");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_new_playlist), v);
	gtk_box_append(GTK_BOX(side), b);
	gtk_widget_add_css_class(side, "side-panel");
	gtk_box_append(GTK_BOX(v), side);

	/* toolbar */
	gtk_widget_set_hexpand(pd_media_view_search_entry(v->media), TRUE);
	gtk_box_append(GTK_BOX(bar), pd_media_view_search_entry(v->media));
	gtk_box_append(GTK_BOX(bar), pd_media_view_queue_button(v->media));
	v->add_folder = gtk_button_new_from_icon_name("folder-new-symbolic");
	gtk_widget_set_tooltip_text(v->add_folder, "Add a music folder");
	gtk_widget_set_focusable(v->add_folder, FALSE);
	g_signal_connect(v->add_folder, "clicked", G_CALLBACK(on_add_folder),
			 v);
	gtk_box_append(GTK_BOX(bar), v->add_folder);
	v->rescan = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(v->rescan, "Rescan music folders");
	gtk_widget_set_focusable(v->rescan, FALSE);
	g_signal_connect(v->rescan, "clicked", G_CALLBACK(on_rescan), v);
	gtk_box_append(GTK_BOX(bar), v->rescan);
	b = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
	gtk_widget_set_tooltip_text(b, "Reset the played marks (✓)");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(app_reset_played),
				 app);
	gtk_box_append(GTK_BOX(bar), b);
	v->list_tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	v->export_btn = gtk_button_new_with_label("Export M3U…");
	gtk_widget_set_focusable(v->export_btn, FALSE);
	g_signal_connect(v->export_btn, "clicked", G_CALLBACK(on_export), v);
	gtk_box_append(GTK_BOX(v->list_tools), v->export_btn);
	v->delete_btn = gtk_button_new_with_label("Delete playlist");
	gtk_widget_add_css_class(v->delete_btn, "destructive");
	gtk_widget_set_focusable(v->delete_btn, FALSE);
	g_signal_connect(v->delete_btn, "clicked",
			 G_CALLBACK(on_delete_playlist), v);
	gtk_box_append(GTK_BOX(v->list_tools), v->delete_btn);
	gtk_box_append(GTK_BOX(bar), v->list_tools);
	v->spinner = gtk_spinner_new();
	gtk_spinner_set_spinning(GTK_SPINNER(v->spinner), TRUE);
	gtk_widget_set_visible(v->spinner, FALSE);
	gtk_box_append(GTK_BOX(bar), v->spinner);

	gtk_box_append(GTK_BOX(right), bar);
	gtk_box_append(GTK_BOX(right), media);
	gtk_widget_set_hexpand(right, TRUE);
	gtk_widget_set_margin_start(right, 6);
	gtk_box_append(GTK_BOX(v), right);

	playlists_set_changed_cb(playlists_changed, v);
	rebuild_sidebar(v);
	show_selection(v, SIDE_LIBRARY, NULL);
	return GTK_WIDGET(v);
}

PdMediaView *pd_lib_view_media(PdLibView *v)
{
	return v->media;
}
