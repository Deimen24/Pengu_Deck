// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * window.c - main window: decks on top, mixer between them, library below
 */
#include "deckview.h"
#include "libview.h"
#include "mixerview.h"
#include "pd-build.h"
#include "prefs.h"
#include "queueview.h"
#include "scview.h"
#include "window.h"

#define TOAST_SECONDS	4

struct _PdWindow {
	GtkApplicationWindow parent;
	struct app *app;
	GtkWidget *deck[ENGINE_DECKS];
	GtkWidget *mixer;
	GtkWidget *lib;
	GtkWidget *sc;
	GtkWidget *queue;
	GtkWidget *deck_btn[ENGINE_DECKS - 1];
	gboolean updating;
	GtkWidget *notebook;
	GtkWidget *toast;
	GtkWidget *toast_label;
	GtkWidget *audio_label;
	guint toast_id;
	int next_deck;
};

G_DEFINE_FINAL_TYPE(PdWindow, pd_window, GTK_TYPE_APPLICATION_WINDOW)

/* ---- toast ------------------------------------------------------- */

static gboolean toast_hide(gpointer data)
{
	PdWindow *w = data;

	w->toast_id = 0;
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->toast), FALSE);
	return G_SOURCE_REMOVE;
}

static void toast_cb(gpointer data, const char *msg)
{
	PdWindow *w = data;

	gtk_label_set_text(GTK_LABEL(w->toast_label), msg);
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->toast), TRUE);
	if (w->toast_id)
		g_source_remove(w->toast_id);
	w->toast_id = g_timeout_add_seconds(TOAST_SECONDS, toast_hide, w);
}

/* ---- keyboard ---------------------------------------------------- */

struct key_binding {
	guint key;
	int deck;
	const char *action;
};

static const struct key_binding bindings[] = {
	{ GDK_KEY_q, 0, "play" },	{ GDK_KEY_p, 1, "play" },
	{ GDK_KEY_w, 0, "cue" },	{ GDK_KEY_o, 1, "cue" },
	{ GDK_KEY_e, 0, "sync" },	{ GDK_KEY_i, 1, "sync" },
	{ GDK_KEY_a, 0, "bend-" },	{ GDK_KEY_k, 1, "bend-" },
	{ GDK_KEY_s, 0, "bend+" },	{ GDK_KEY_l, 1, "bend+" },
	{ GDK_KEY_z, 0, "loop4" },	{ GDK_KEY_m, 1, "loop4" },
	{ GDK_KEY_x, 0, "loop" },	{ GDK_KEY_comma, 1, "loop" },
	{ GDK_KEY_1, 0, "hotcue1" },	{ GDK_KEY_7, 1, "hotcue1" },
	{ GDK_KEY_2, 0, "hotcue2" },	{ GDK_KEY_8, 1, "hotcue2" },
	{ GDK_KEY_3, 0, "hotcue3" },	{ GDK_KEY_9, 1, "hotcue3" },
	{ GDK_KEY_4, 0, "hotcue4" },	{ GDK_KEY_0, 1, "hotcue4" },
};

static PdMediaView *current_media(PdWindow *w);

static gboolean text_focused(PdWindow *w)
{
	GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(w));

	return f && (GTK_IS_EDITABLE(f) || GTK_IS_TEXT(f));
}

static gboolean handle_key(PdWindow *w, guint key, GdkModifierType mod,
			   gboolean press)
{
	PdMediaView *mv;
	size_t i;

	if (mod & (GDK_CONTROL_MASK | GDK_ALT_MASK)) {
		if (!press)
			return FALSE;
		if (key == GDK_KEY_l || key == GDK_KEY_f) {
			gtk_notebook_set_current_page(
					GTK_NOTEBOOK(w->notebook), 0);
			gtk_widget_grab_focus(pd_media_view_search_entry(
				pd_lib_view_media(PD_LIB_VIEW(w->lib))));
			return TRUE;
		}
		if (key == GDK_KEY_k) {
			gtk_notebook_set_current_page(
					GTK_NOTEBOOK(w->notebook), 1);
			return TRUE;
		}
		if (key == GDK_KEY_m) {
			gtk_notebook_set_current_page(
					GTK_NOTEBOOK(w->notebook), 2);
			return TRUE;
		}
		return FALSE;
	}

	/* Enter in a list loads to the next deck, Shift+Enter to B. */
	if (press && (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) &&
	    (mod & GDK_SHIFT_MASK)) {
		mv = current_media(w);
		pd_media_view_load_selected(mv, 1);
		return TRUE;
	}

	if (text_focused(w))
		return FALSE;

	if (key == GDK_KEY_Left || key == GDK_KEY_Right) {
		if (press)
			pd_mixer_view_nudge_xfader(PD_MIXER_VIEW(w->mixer),
					key == GDK_KEY_Left ? -0.1 : 0.1);
		return TRUE;
	}
	if (key == GDK_KEY_space) {
		if (press)
			pd_deck_view_action(PD_DECK_VIEW(w->deck[0]), "play",
					    TRUE);
		return TRUE;
	}
	for (i = 0; i < G_N_ELEMENTS(bindings); i++) {
		if (gdk_keyval_to_lower(key) != bindings[i].key)
			continue;
		pd_deck_view_action(PD_DECK_VIEW(w->deck[bindings[i].deck]),
				    bindings[i].action, press);
		return TRUE;
	}
	return FALSE;
}

static gboolean on_key_press(GtkEventControllerKey *c, guint key,
			     guint code, GdkModifierType mod, PdWindow *w)
{
	return handle_key(w, key, mod, TRUE);
}

static void on_key_release(GtkEventControllerKey *c, guint key,
			   guint code, GdkModifierType mod, PdWindow *w)
{
	handle_key(w, key, mod, FALSE);
}

/* ---- actions ----------------------------------------------------- */

static void update_audio_label(PdWindow *w)
{
	struct engine *e = &w->app->engine;
	char *s;

	if (e->running && e->device_name)
		s = g_strdup_printf("%s · %u Hz", e->device_name, e->rate);
	else
		s = g_strdup("No audio device");
	gtk_label_set_text(GTK_LABEL(w->audio_label), s);
	g_free(s);
}

static void apply_deck_count(PdWindow *w)
{
	int n = w->app->cfg.ndecks, i;

	w->updating = TRUE;
	for (i = 0; i < ENGINE_DECKS; i++)
		gtk_widget_set_visible(w->deck[i], i < n);
	for (i = 0; i < ENGINE_DECKS - 1; i++)
		gtk_toggle_button_set_active(
				GTK_TOGGLE_BUTTON(w->deck_btn[i]), i + 2 == n);
	pd_mixer_view_set_decks(PD_MIXER_VIEW(w->mixer), n);
	w->updating = FALSE;
}

static void on_deck_count(GtkToggleButton *b, PdWindow *w)
{
	if (w->updating || !gtk_toggle_button_get_active(b))
		return;
	app_set_deck_count(w->app, GPOINTER_TO_INT(
			g_object_get_data(G_OBJECT(b), "count")));
	apply_deck_count(w);
}

static void prefs_applied(gpointer data)
{
	PdWindow *w = data;
	int i;

	for (i = 0; i < ENGINE_DECKS; i++)
		pd_deck_view_apply_config(PD_DECK_VIEW(w->deck[i]));
	pd_mixer_view_apply_config(PD_MIXER_VIEW(w->mixer));
	pd_lib_view_rescan(PD_LIB_VIEW(w->lib));
	update_audio_label(w);
}

static void ui_sync_deck(gpointer data, int idx)
{
	PdWindow *w = data;

	pd_deck_view_action(PD_DECK_VIEW(w->deck[idx]), "sync", TRUE);
}

static PdMediaView *current_media(PdWindow *w)
{
	switch (gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook))) {
	case 1:
		return pd_sc_view_media(PD_SC_VIEW(w->sc));
	default:
		return pd_lib_view_media(PD_LIB_VIEW(w->lib));
	}
}

static void ui_load_selected(gpointer data, int idx)
{
	PdWindow *w = data;

	pd_media_view_load_selected(current_media(w), idx);
}

static void on_prefs(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdWindow *w = data;

	prefs_show(w->app, G_CALLBACK(prefs_applied), w);
}

static void on_about(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdWindow *w = data;
	const char *authors[] = { "Pengu Deck contributors", NULL };

	gtk_show_about_dialog(GTK_WINDOW(w),
			      "program-name", "Pengu Deck",
			      "version", PD_VERSION,
			      "comments", "Two deck DJ software for Linux "
					  "with local files and SoundCloud",
			      "license-type", GTK_LICENSE_GPL_3_0,
			      "website", "https://github.com/deimen24/"
					 "Pengu_Deck",
			      "authors", authors,
			      "logo-icon-name", PD_APP_ID,
			      NULL);
}

static void on_shortcuts(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdWindow *w = data;

	toast_cb(w, "Deck A: Q play · W cue · E sync · A/S nudge · Z loop 4 "
		    "· X loop · 1-4 hot cues   |   Deck B: P · O · I · K/L "
		    "· M · , · 7-0   |   ←/→ crossfader · Ctrl+L library "
		    "· Ctrl+K SoundCloud · Shift+Enter load to B");
}

static void on_open_file(GObject *src, GAsyncResult *res, gpointer data)
{
	PdWindow *w = data;
	GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res,
					       NULL);
	char *path;

	if (!f)
		return;
	path = g_file_get_path(f);
	if (path)
		pd_window_load_file(w, path);
	g_free(path);
	g_object_unref(f);
}

static void on_open(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdWindow *w = data;
	GtkFileDialog *d = gtk_file_dialog_new();

	gtk_file_dialog_set_title(d, "Open audio file");
	gtk_file_dialog_open(d, GTK_WINDOW(w), NULL, on_open_file, w);
	g_object_unref(d);
}

/* ---- construction ------------------------------------------------ */

static GtkWidget *build_header(PdWindow *w)
{
	GtkWidget *hb = gtk_header_bar_new();
	GtkWidget *menu_btn = gtk_menu_button_new();
	GMenu *menu = g_menu_new();
	GtkWidget *title = gtk_label_new("Pengu Deck");

	gtk_widget_add_css_class(title, "title");
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title);
	g_menu_append(menu, "Open file…", "win.open");
	g_menu_append(menu, "Preferences", "win.prefs");
	g_menu_append(menu, "Keyboard shortcuts", "win.shortcuts");
	g_menu_append(menu, "About", "win.about");
	g_menu_append(menu, "Quit", "app.quit");
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn),
				      "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn),
				       G_MENU_MODEL(menu));
	gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), menu_btn);

	w->audio_label = gtk_label_new("");
	gtk_widget_add_css_class(w->audio_label, "dim-label");
	gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), w->audio_label);

	/* Deck count switcher: purely a view change, audio keeps running. */
	{
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		GtkWidget *l = gtk_label_new("DECKS");
		GtkWidget *group = NULL;
		int i;

		gtk_widget_add_css_class(l, "section-label");
		gtk_box_append(GTK_BOX(box), l);
		for (i = 0; i < ENGINE_DECKS - 1; i++) {
			char label[2] = { (char)('2' + i), '\0' };
			GtkWidget *b = gtk_toggle_button_new_with_label(label);

			gtk_widget_set_focusable(b, FALSE);
			gtk_widget_add_css_class(b, "deck-count");
			if (group)
				gtk_toggle_button_set_group(
						GTK_TOGGLE_BUTTON(b),
						GTK_TOGGLE_BUTTON(group));
			else
				group = b;
			g_object_set_data(G_OBJECT(b), "count",
					  GINT_TO_POINTER(i + 2));
			g_signal_connect(b, "toggled",
					 G_CALLBACK(on_deck_count), w);
			gtk_box_append(GTK_BOX(box), b);
			w->deck_btn[i] = b;
		}
		gtk_widget_add_css_class(box, "linked");
		gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), box);
	}
	g_object_unref(menu);
	return hb;
}

static void pd_window_dispose(GObject *obj)
{
	PdWindow *w = PD_WINDOW(obj);

	if (w->toast_id) {
		g_source_remove(w->toast_id);
		w->toast_id = 0;
	}
	if (w->app && w->app->toast_data == w) {
		w->app->toast = NULL;
		w->app->toast_data = NULL;
		w->app->sync_deck = NULL;
		w->app->load_selected = NULL;
		w->app->ui_data = NULL;
	}
	G_OBJECT_CLASS(pd_window_parent_class)->dispose(obj);
}

static void pd_window_class_init(PdWindowClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_window_dispose;
}

static void pd_window_init(PdWindow *w)
{
}

GtkWidget *pd_window_new(struct app *app)
{
	PdWindow *w = g_object_new(PD_TYPE_WINDOW, "application", app->gtk,
				   NULL);
	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *top = gtk_grid_new();
	int i;
	GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	GtkWidget *overlay = gtk_overlay_new();
	GtkEventController *keys = gtk_event_controller_key_new();
	static const GActionEntry entries[] = {
		{ "open", on_open },
		{ "prefs", on_prefs },
		{ "about", on_about },
		{ "shortcuts", on_shortcuts },
	};

	w->app = app;
	app->win = GTK_WINDOW(w);
	app->toast = toast_cb;
	app->toast_data = w;
	app->sync_deck = ui_sync_deck;
	app->load_selected = ui_load_selected;
	app->ui_data = w;
	gtk_window_set_title(GTK_WINDOW(w), "Pengu Deck");
	gtk_window_set_default_size(GTK_WINDOW(w), 1440, 900);
	gtk_window_set_icon_name(GTK_WINDOW(w), PD_APP_ID);
	gtk_window_set_titlebar(GTK_WINDOW(w), build_header(w));
	g_action_map_add_action_entries(G_ACTION_MAP(w), entries,
					G_N_ELEMENTS(entries), w);

	for (i = 0; i < ENGINE_DECKS; i++)
		w->deck[i] = pd_deck_view_new(app, i);
	w->mixer = pd_mixer_view_new(app);
	/* A and B beside the mixer, C and D in a second row. */
	gtk_grid_set_column_spacing(GTK_GRID(top), 8);
	gtk_grid_set_row_spacing(GTK_GRID(top), 8);
	gtk_grid_set_column_homogeneous(GTK_GRID(top), FALSE);
	gtk_grid_attach(GTK_GRID(top), w->deck[0], 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(top), w->mixer, 1, 0, 1, 2);
	gtk_grid_attach(GTK_GRID(top), w->deck[1], 2, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(top), w->deck[2], 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(top), w->deck[3], 2, 1, 1, 1);
	gtk_widget_set_margin_top(top, 8);
	gtk_widget_set_margin_start(top, 8);
	gtk_widget_set_margin_end(top, 8);
	gtk_widget_set_margin_bottom(top, 4);

	w->lib = pd_lib_view_new(app);
	w->sc = pd_sc_view_new(app);
	w->queue = pd_queue_view_new(app);
	w->notebook = gtk_notebook_new();
	gtk_notebook_append_page(GTK_NOTEBOOK(w->notebook), w->lib,
				 gtk_label_new("Library"));
	gtk_notebook_append_page(GTK_NOTEBOOK(w->notebook), w->sc,
				 gtk_label_new("SoundCloud"));
	gtk_notebook_append_page(GTK_NOTEBOOK(w->notebook), w->queue,
				 gtk_label_new("Automix"));
	gtk_widget_set_margin_start(w->notebook, 8);
	gtk_widget_set_margin_end(w->notebook, 8);
	gtk_widget_set_margin_bottom(w->notebook, 8);

	gtk_paned_set_start_child(GTK_PANED(paned), top);
	gtk_paned_set_end_child(GTK_PANED(paned), w->notebook);
	gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
	gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
	gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
	gtk_widget_set_vexpand(paned, TRUE);
	gtk_box_append(GTK_BOX(root), paned);

	w->toast_label = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(w->toast_label), TRUE);
	gtk_widget_add_css_class(w->toast_label, "toast");
	w->toast = gtk_revealer_new();
	gtk_revealer_set_child(GTK_REVEALER(w->toast), w->toast_label);
	gtk_revealer_set_transition_type(GTK_REVEALER(w->toast),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_widget_set_halign(w->toast, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w->toast, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom(w->toast, 16);
	gtk_widget_set_can_target(w->toast, FALSE);

	gtk_overlay_set_child(GTK_OVERLAY(overlay), root);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->toast);
	gtk_window_set_child(GTK_WINDOW(w), overlay);

	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_press), w);
	g_signal_connect(keys, "key-released", G_CALLBACK(on_key_release), w);
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	gtk_widget_add_controller(GTK_WIDGET(w), keys);

	update_audio_label(w);
	apply_deck_count(w);
	pd_lib_view_rescan(PD_LIB_VIEW(w->lib));
	return GTK_WIDGET(w);
}

void pd_window_load_file(PdWindow *w, const char *path)
{
	app_load_path(w->app, w->next_deck, path);
	w->next_deck = (w->next_deck + 1) % w->app->cfg.ndecks;
}
