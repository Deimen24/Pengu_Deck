// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * prefs.c - preferences window
 */
#include <string.h>

#include "prefs.h"

struct prefs {
	struct app *app;
	GtkWindow *win;
	void (*applied)(gpointer data);
	gpointer applied_data;

	GtkWidget *backend;
	GtkWidget *device;
	GtkWidget *rate;
	GtkWidget *period;
	GtkWidget *hp_mode;
	GtkWidget *xf_curve;
	GtkWidget *pitch_range;
	GtkWidget *keylock;
	GtkWidget *folders;
	GtkWidget *record_dir;
	GtkWidget *client_id;
	GtkWidget *token;
	GtkWidget *detect;
	GtkWidget *status;
};

static const char *const backend_names[] = {
	"Automatic", "PulseAudio / PipeWire", "ALSA", "JACK / PipeWire-JACK",
	NULL,
};
static const char *const hp_names[] = {
	"Off", "Split stereo: left = master, right = cue",
	"4 channel interface: 1/2 master, 3/4 cue", NULL,
};
static const char *const xf_names[] = {
	"Dipless (mixing)", "Constant power", "Sharp cut (scratching)", NULL,
};
static const char *const rate_names[] = {
	"Device default", "44100 Hz", "48000 Hz", "96000 Hz", NULL,
};
static const unsigned int rate_values[] = { 0, 44100, 48000, 96000 };
static const char *const period_names[] = {
	"Backend default", "64 (lowest latency)", "128", "256", "512", "1024",
	NULL,
};
static const unsigned int period_values[] = { 0, 64, 128, 256, 512, 1024 };
static const char *const range_names[] = { "±8 %", "±16 %", "±50 %", NULL };
static const int range_values[] = { 8, 16, 50 };

/* ---- helpers ----------------------------------------------------- */

static GtkWidget *row(GtkWidget *grid, int y, const char *label,
		      GtkWidget *w)
{
	GtkWidget *l = gtk_label_new(label);

	gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_widget_set_hexpand(w, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
	return w;
}

static GtkWidget *dropdown(const char *const *names, guint selected)
{
	GtkWidget *d = gtk_drop_down_new_from_strings(names);

	gtk_drop_down_set_selected(GTK_DROP_DOWN(d), selected);
	return d;
}

static guint index_of_uint(const unsigned int *vals, size_t n,
			   unsigned int v)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (vals[i] == v)
			return (guint)i;
	return 0;
}

static GtkWidget *page(GtkWidget *notebook, const char *title)
{
	GtkWidget *grid = gtk_grid_new();

	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_widget_set_margin_top(grid, 16);
	gtk_widget_set_margin_bottom(grid, 16);
	gtk_widget_set_margin_start(grid, 16);
	gtk_widget_set_margin_end(grid, 16);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid,
				 gtk_label_new(title));
	return grid;
}

static void set_status(struct prefs *p, const char *msg, gboolean error)
{
	gtk_label_set_text(GTK_LABEL(p->status), msg ? msg : "");
	if (error)
		gtk_widget_add_css_class(p->status, "error");
	else
		gtk_widget_remove_css_class(p->status, "error");
}

/* ---- devices ----------------------------------------------------- */

static void fill_devices(struct prefs *p)
{
	guint b = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->backend));
	char **names = engine_list_devices((enum audio_backend)b);
	GtkStringList *list = gtk_string_list_new(NULL);
	guint i, sel = 0;

	gtk_string_list_append(list, "Default device");
	for (i = 0; names[i]; i++) {
		gtk_string_list_append(list, names[i]);
		if (p->app->cfg.device && strcmp(names[i],
						 p->app->cfg.device) == 0)
			sel = i + 1;
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->device), G_LIST_MODEL(list));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->device), sel);
	g_object_unref(list);
	g_strfreev(names);
}

static void on_backend(GObject *d, GParamSpec *ps, struct prefs *p)
{
	fill_devices(p);
}

/* ---- folders ----------------------------------------------------- */

static void refresh_folders(struct prefs *p)
{
	GtkStringList *list = gtk_string_list_new(
			(const char *const *)p->app->cfg.folders);
	GtkSelectionModel *sel = GTK_SELECTION_MODEL(
			gtk_single_selection_new(G_LIST_MODEL(list)));

	gtk_list_view_set_model(GTK_LIST_VIEW(p->folders), sel);
	g_object_unref(sel);
}

static void folder_setup(GtkListItemFactory *f, GtkListItem *li,
			 gpointer data)
{
	GtkWidget *l = gtk_label_new("");

	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
	gtk_list_item_set_child(li, l);
}

static void folder_bind(GtkListItemFactory *f, GtkListItem *li,
			gpointer data)
{
	GtkStringObject *s = gtk_list_item_get_item(li);

	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
			   gtk_string_object_get_string(s));
}

static void folder_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	struct prefs *p = data;
	GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
							res, NULL);
	char *path, **folders;
	guint n;

	if (!f)
		return;
	path = g_file_get_path(f);
	g_object_unref(f);
	if (!path)
		return;
	n = g_strv_length(p->app->cfg.folders);
	folders = g_realloc_n(p->app->cfg.folders, n + 2, sizeof(char *));
	folders[n] = path;
	folders[n + 1] = NULL;
	p->app->cfg.folders = folders;
	refresh_folders(p);
}

static void on_add_folder(GtkButton *b, struct prefs *p)
{
	GtkFileDialog *d = gtk_file_dialog_new();

	gtk_file_dialog_select_folder(d, p->win, NULL, folder_chosen, p);
	g_object_unref(d);
}

static void on_remove_folder(GtkButton *b, struct prefs *p)
{
	GtkSelectionModel *sel = gtk_list_view_get_model(
					GTK_LIST_VIEW(p->folders));
	guint i = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(sel));
	char **folders = p->app->cfg.folders;
	guint n = g_strv_length(folders);

	if (i == GTK_INVALID_LIST_POSITION || i >= n)
		return;
	g_free(folders[i]);
	memmove(folders + i, folders + i + 1, (n - i) * sizeof(char *));
	refresh_folders(p);
}

static void record_dir_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	struct prefs *p = data;
	GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
							res, NULL);
	char *path;

	if (!f)
		return;
	path = g_file_get_path(f);
	g_object_unref(f);
	if (path)
		gtk_editable_set_text(GTK_EDITABLE(p->record_dir), path);
	g_free(path);
}

static void on_record_dir(GtkButton *b, struct prefs *p)
{
	GtkFileDialog *d = gtk_file_dialog_new();

	gtk_file_dialog_select_folder(d, p->win, NULL, record_dir_chosen, p);
	g_object_unref(d);
}

/* ---- soundcloud -------------------------------------------------- */

struct detect {
	struct prefs *p;
	char *id;
	GError *err;
};

static gboolean detect_done(gpointer data)
{
	struct detect *d = data;
	struct prefs *p = d->p;

	if (p->win) {
		gtk_widget_set_sensitive(p->detect, TRUE);
		if (d->id) {
			gtk_editable_set_text(GTK_EDITABLE(p->client_id),
					      d->id);
			set_status(p, "Client ID found", FALSE);
		} else {
			set_status(p, d->err->message, TRUE);
		}
	}
	g_free(d->id);
	g_clear_error(&d->err);
	g_free(d);
	return G_SOURCE_REMOVE;
}

static gpointer detect_thread(gpointer data)
{
	struct detect *d = data;

	d->id = sc_detect_client_id(&d->err);
	g_idle_add(detect_done, d);
	return NULL;
}

static void on_detect(GtkButton *b, struct prefs *p)
{
	struct detect *d = g_new0(struct detect, 1);

	d->p = p;
	gtk_widget_set_sensitive(p->detect, FALSE);
	set_status(p, "Looking for a client ID on soundcloud.com…", FALSE);
	g_thread_unref(g_thread_new("pd-sc-detect", detect_thread, d));
}

/* ---- apply ------------------------------------------------------- */

static char *dropdown_text(GtkWidget *d)
{
	GtkStringObject *s = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(d));

	return s ? g_strdup(gtk_string_object_get_string(s)) : NULL;
}

static void on_apply(GtkButton *b, struct prefs *p)
{
	struct app *a = p->app;
	struct config *c = &a->cfg;
	gboolean audio_changed;
	guint dev = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->device));
	char *device = dev > 0 ? dropdown_text(p->device) : NULL;
	char *warn = NULL;
	struct config old = *c;

	c->backend = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->backend));
	c->device = device;
	c->rate = rate_values[gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->rate))];
	c->period = period_values[gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->period))];
	c->hp_mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->hp_mode));
	c->xf_curve = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->xf_curve));
	c->pitch_range = range_values[gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->pitch_range))];
	c->keylock = gtk_check_button_get_active(
					GTK_CHECK_BUTTON(p->keylock));
	c->record_dir = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->record_dir)));
	c->sc_client_id = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->client_id)));
	c->sc_token = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->token)));
	g_free(old.device);
	g_free(old.record_dir);
	g_free(old.sc_client_id);
	g_free(old.sc_token);

	audio_changed = c->backend != old.backend ||
			g_strcmp0(c->device, old.device) != 0 ||
			c->rate != old.rate || c->period != old.period ||
			c->hp_mode != old.hp_mode;
	config_save(c);

	if (audio_changed) {
		app_open_audio(a, &warn);
		if (warn) {
			set_status(p, warn, TRUE);
			g_free(warn);
		} else {
			char *msg = g_strdup_printf("Audio: %s, %u Hz",
						    a->engine.device_name,
						    a->engine.rate);

			set_status(p, msg, FALSE);
			g_free(msg);
		}
	} else {
		set_status(p, "Settings saved", FALSE);
	}
	if (p->applied)
		p->applied(p->applied_data);
}

static void on_close(GtkButton *b, struct prefs *p)
{
	gtk_window_close(p->win);
}

static gboolean free_prefs(gpointer data)
{
	g_free(data);
	return G_SOURCE_REMOVE;
}

static void on_destroy(GtkWidget *w, struct prefs *p)
{
	p->win = NULL;
	/* Keep the struct alive for a detect thread still in flight. */
	g_timeout_add_seconds(120, free_prefs, p);
}

/* ---- construction ------------------------------------------------ */

static void build_audio(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "Audio");
	GtkWidget *l;

	p->backend = row(g, 0, "Backend", dropdown(backend_names,
						   c->backend));
	p->device = row(g, 1, "Output device",
			gtk_drop_down_new(NULL, NULL));
	p->rate = row(g, 2, "Sample rate", dropdown(rate_names,
			index_of_uint(rate_values, G_N_ELEMENTS(rate_values),
				      c->rate)));
	p->period = row(g, 3, "Buffer size", dropdown(period_names,
			index_of_uint(period_values,
				      G_N_ELEMENTS(period_values), c->period)));
	p->hp_mode = row(g, 4, "Headphone cue", dropdown(hp_names,
							 c->hp_mode));
	p->xf_curve = row(g, 5, "Crossfader curve", dropdown(xf_names,
							     c->xf_curve));
	l = gtk_label_new("On CachyOS / Arch with PipeWire the automatic "
			  "backend uses pipewire-pulse. For the lowest "
			  "latency pick JACK and run with pipewire-jack, or "
			  "ALSA with a direct hw: device.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 6, 2, 1);
	fill_devices(p);
	g_signal_connect(p->backend, "notify::selected",
			 G_CALLBACK(on_backend), p);
}

static void build_library(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "Library");
	GtkWidget *scroll, *box, *b;
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();

	g_signal_connect(f, "setup", G_CALLBACK(folder_setup), NULL);
	g_signal_connect(f, "bind", G_CALLBACK(folder_bind), NULL);
	p->folders = gtk_list_view_new(NULL, f);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
				      p->folders);
	gtk_scrolled_window_set_min_content_height(
				GTK_SCROLLED_WINDOW(scroll), 140);
	gtk_widget_add_css_class(scroll, "frame");
	row(g, 0, "Music folders", scroll);
	refresh_folders(p);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	b = gtk_button_new_with_label("Add folder…");
	g_signal_connect(b, "clicked", G_CALLBACK(on_add_folder), p);
	gtk_box_append(GTK_BOX(box), b);
	b = gtk_button_new_with_label("Remove selected");
	g_signal_connect(b, "clicked", G_CALLBACK(on_remove_folder), p);
	gtk_box_append(GTK_BOX(box), b);
	gtk_grid_attach(GTK_GRID(g), box, 1, 1, 1, 1);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->record_dir = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(p->record_dir), c->record_dir);
	gtk_widget_set_hexpand(p->record_dir, TRUE);
	gtk_box_append(GTK_BOX(box), p->record_dir);
	b = gtk_button_new_from_icon_name("folder-open-symbolic");
	g_signal_connect(b, "clicked", G_CALLBACK(on_record_dir), p);
	gtk_box_append(GTK_BOX(box), b);
	row(g, 2, "Recordings", box);
}

static void build_soundcloud(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "SoundCloud");
	GtkWidget *box, *l;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	p->client_id = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(p->client_id),
			      c->sc_client_id ? c->sc_client_id : "");
	gtk_widget_set_hexpand(p->client_id, TRUE);
	gtk_box_append(GTK_BOX(box), p->client_id);
	p->detect = gtk_button_new_with_label("Detect");
	gtk_widget_set_tooltip_text(p->detect, "Read the public client ID "
				    "from the SoundCloud web player");
	g_signal_connect(p->detect, "clicked", G_CALLBACK(on_detect), p);
	gtk_box_append(GTK_BOX(box), p->detect);
	row(g, 0, "Client ID", box);

	p->token = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(p->token),
					      TRUE);
	gtk_editable_set_text(GTK_EDITABLE(p->token),
			      c->sc_token ? c->sc_token : "");
	row(g, 1, "OAuth token", p->token);

	l = gtk_label_new(
		"Search and public links only need the client ID. Press "
		"Detect to fetch the one the soundcloud.com player uses; it "
		"changes every few weeks, so press it again if searches "
		"start failing.\n\n"
		"Your Likes need an OAuth token. Log in at soundcloud.com, "
		"open the browser developer tools, and copy the value of "
		"the \"oauth_token\" cookie (or the Authorization header of "
		"any api-v2 request) here. The token is stored in "
		"~/.config/pengu-deck/settings.ini with mode 0600.\n\n"
		"Streams come from SoundCloud's transcoding endpoints and "
		"are decoded on the fly; only what you play is downloaded, "
		"and nothing is saved to disk.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 2, 2, 1);
}

static void build_decks(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "Decks");
	guint sel = c->pitch_range == 16 ? 1 : c->pitch_range == 50 ? 2 : 0;

	p->pitch_range = row(g, 0, "Pitch range", dropdown(range_names,
							   sel));
	p->keylock = gtk_check_button_new_with_label("Enable keylock on "
						     "new tracks");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->keylock),
				    c->keylock);
	gtk_widget_set_sensitive(p->keylock, deck_has_keylock());
	row(g, 1, "", p->keylock);
}

void prefs_show(struct app *app, GCallback applied, gpointer data)
{
	struct prefs *p = g_new0(struct prefs, 1);
	GtkWidget *win = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *nb = gtk_notebook_new();
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b;

	p->app = app;
	p->win = GTK_WINDOW(win);
	p->applied = (void (*)(gpointer))applied;
	p->applied_data = data;

	gtk_window_set_title(p->win, "Preferences");
	gtk_window_set_transient_for(p->win, app->win);
	gtk_window_set_modal(p->win, TRUE);
	gtk_window_set_default_size(p->win, 620, 480);

	build_audio(p, nb);
	build_library(p, nb);
	build_soundcloud(p, nb);
	build_decks(p, nb);
	gtk_widget_set_vexpand(nb, TRUE);
	gtk_box_append(GTK_BOX(box), nb);

	p->status = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
	gtk_label_set_xalign(GTK_LABEL(p->status), 0.0f);
	gtk_widget_set_hexpand(p->status, TRUE);
	gtk_widget_set_margin_start(p->status, 12);
	gtk_box_append(GTK_BOX(buttons), p->status);
	b = gtk_button_new_with_label("Close");
	g_signal_connect(b, "clicked", G_CALLBACK(on_close), p);
	gtk_box_append(GTK_BOX(buttons), b);
	b = gtk_button_new_with_label("Apply");
	gtk_widget_add_css_class(b, "suggested-action");
	g_signal_connect(b, "clicked", G_CALLBACK(on_apply), p);
	gtk_box_append(GTK_BOX(buttons), b);
	gtk_widget_set_margin_bottom(buttons, 12);
	gtk_widget_set_margin_end(buttons, 12);
	gtk_box_append(GTK_BOX(box), buttons);

	gtk_window_set_child(p->win, box);
	g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), p);
	gtk_window_present(p->win);
}
