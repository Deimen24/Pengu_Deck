// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * libview.c - local music library panel
 */
#include "library.h"
#include "libview.h"

struct _PdLibView {
	GtkBox parent;
	struct app *app;
	PdMediaView *media;
	GtkWidget *spinner;
	GtkWidget *rescan;
	GCancellable *cancel;
};

G_DEFINE_FINAL_TYPE(PdLibView, pd_lib_view, GTK_TYPE_BOX)

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

static void pd_lib_view_dispose(GObject *obj)
{
	PdLibView *v = PD_LIB_VIEW(obj);

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
				    "orientation", GTK_ORIENTATION_VERTICAL,
				    "spacing", 4, NULL);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b, *media;

	v->app = app;
	media = pd_media_view_new(app, app->library,
				  "No music found.\nAdd a folder with the "
				  "button above, or drop audio files onto a "
				  "deck.");
	v->media = PD_MEDIA_VIEW(media);

	gtk_widget_set_hexpand(pd_media_view_search_entry(v->media), TRUE);
	gtk_box_append(GTK_BOX(bar), pd_media_view_search_entry(v->media));
	gtk_box_append(GTK_BOX(bar), pd_media_view_queue_button(v->media));
	b = gtk_button_new_from_icon_name("folder-new-symbolic");
	gtk_widget_set_tooltip_text(b, "Add a music folder");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_add_folder), v);
	gtk_box_append(GTK_BOX(bar), b);
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
	v->spinner = gtk_spinner_new();
	gtk_spinner_set_spinning(GTK_SPINNER(v->spinner), TRUE);
	gtk_widget_set_visible(v->spinner, FALSE);
	gtk_box_append(GTK_BOX(bar), v->spinner);

	gtk_box_append(GTK_BOX(v), bar);
	gtk_box_append(GTK_BOX(v), media);
	return GTK_WIDGET(v);
}

PdMediaView *pd_lib_view_media(PdLibView *v)
{
	return v->media;
}
