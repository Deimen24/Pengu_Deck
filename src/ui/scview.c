// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * scview.c - SoundCloud search panel
 *
 * The search entry doubles as a link box: a soundcloud.com URL resolves
 * to the track, playlist or artist behind it.
 */
#include <string.h>

#include "scview.h"

struct _PdScView {
	GtkBox parent;
	struct app *app;
	PdMediaView *media;
	GtkWidget *entry;
	GtkWidget *spinner;
	GtkWidget *likes;
	GtkWidget *hint;
	guint pending;
};

G_DEFINE_FINAL_TYPE(PdScView, pd_sc_view, GTK_TYPE_BOX)

enum query_kind {
	QUERY_SEARCH,
	QUERY_RESOLVE,
	QUERY_LIKES,
};

struct query {
	PdScView *view;
	enum query_kind kind;
	char *text;
	struct sc_auth *auth;
	GPtrArray *result;
	GError *err;
};

static void query_free(struct query *q)
{
	g_free(q->text);
	sc_auth_free(q->auth);
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
	if (q->result) {
		g_list_store_splice(a->sc_results, 0,
				    g_list_model_get_n_items(
						G_LIST_MODEL(a->sc_results)),
				    q->result->pdata, q->result->len);
		if (q->result->len == 0)
			app_toast(a, "SoundCloud: nothing found");
	} else if (q->err) {
		app_toast(a, "SoundCloud: %s", q->err->message);
	}
	query_free(q);
	return G_SOURCE_REMOVE;
}

static gpointer query_thread(gpointer data)
{
	struct query *q = data;

	switch (q->kind) {
	case QUERY_SEARCH:
		q->result = sc_search(q->auth, q->text, &q->err);
		break;
	case QUERY_RESOLVE:
		q->result = sc_resolve(q->auth, q->text, &q->err);
		break;
	case QUERY_LIKES:
		q->result = sc_likes(q->auth, &q->err);
		break;
	}
	g_idle_add(query_done, q);
	return NULL;
}

static void run_query(PdScView *v, enum query_kind kind, const char *text)
{
	struct query *q = g_new0(struct query, 1);

	q->view = g_object_ref(v);
	q->kind = kind;
	q->text = g_strdup(text);
	q->auth = app_sc_auth(v->app);
	v->pending++;
	gtk_widget_set_visible(v->spinner, TRUE);
	g_thread_unref(g_thread_new("pd-sc-query", query_thread, q));
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
				  "Enter.\n\nA client ID is required, see "
				  "Preferences → SoundCloud.");
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

	v->likes = gtk_button_new_with_label("♥ Likes");
	gtk_widget_set_tooltip_text(v->likes, "Load your liked tracks "
				    "(needs an OAuth token in Preferences)");
	gtk_widget_set_focusable(v->likes, FALSE);
	g_signal_connect(v->likes, "clicked", G_CALLBACK(on_likes), v);
	gtk_box_append(GTK_BOX(bar), v->likes);

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
	gtk_box_append(GTK_BOX(v), media);
	return GTK_WIDGET(v);
}

PdMediaView *pd_sc_view_media(PdScView *v)
{
	return v->media;
}
