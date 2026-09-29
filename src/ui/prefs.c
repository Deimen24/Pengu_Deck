// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * prefs.c - preferences window
 */
#include <string.h>

#include "midi.h"
#include "prefs.h"
#include "sccache.h"

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
	GtkWidget *autogain;
	GtkWidget *quantize;
	GtkWidget *folders;
	GtkWidget *record_dir;
	GtkWidget *client_id;
	GtkWidget *token;
	GtkWidget *detect;
	GtkWidget *cache_label;
	GtkWidget *mic;
	GtkWidget *mic_device;
	GtkWidget *talkover;
	GtkWidget *rec_format;
	GtkWidget *rec_bitrate;
	GtkWidget *ice_host;
	GtkWidget *ice_port;
	GtkWidget *ice_mount;
	GtkWidget *ice_user;
	GtkWidget *ice_password;
	GtkWidget *ice_name;
	GtkWidget *ice_format;
	GtkWidget *ice_bitrate;
	GtkWidget *status;
	GtkWidget *midi_status;
	GtkWidget *midi_grid;
	GtkWidget *midi_labels[128];
	guint midi_timer;
};

static void on_clear_cache(GtkButton *b, struct prefs *p)
{
	sccache_clear();
	gtk_label_set_text(GTK_LABEL(p->cache_label),
			   "0 bytes of cached streams");
}

static const char *const backend_names[] = {
	"Automatic", "PulseAudio / PipeWire", "ALSA", "JACK / PipeWire-JACK",
	"Silent (no output, for testing)", NULL,
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
static const char *const range_names[] = {
	"±8 %", "±16 %", "±50 %", "±100 %", NULL,
};
static const int range_values[] = { 8, 16, 50, 100 };

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

static void fill_mic_devices(struct prefs *p)
{
	guint b = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->backend));
	char **names = engine_list_capture_devices((enum audio_backend)b);
	GtkStringList *list = gtk_string_list_new(NULL);
	guint i, sel = 0;

	gtk_string_list_append(list, "Default microphone");
	for (i = 0; names[i]; i++) {
		gtk_string_list_append(list, names[i]);
		if (p->app->cfg.mic_device &&
		    strcmp(names[i], p->app->cfg.mic_device) == 0)
			sel = i + 1;
	}
	gtk_drop_down_set_model(GTK_DROP_DOWN(p->mic_device),
				G_LIST_MODEL(list));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->mic_device), sel);
	g_object_unref(list);
	g_strfreev(names);
}

static void on_backend(GObject *d, GParamSpec *ps, struct prefs *p)
{
	fill_devices(p);
	fill_mic_devices(p);
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
	c->mic = gtk_check_button_get_active(GTK_CHECK_BUTTON(p->mic));
	{
		guint md = gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->mic_device));

		g_free(c->mic_device);
		c->mic_device = md > 0 ? dropdown_text(p->mic_device) : NULL;
	}
	c->talkover_db = gtk_spin_button_get_value_as_int(
					GTK_SPIN_BUTTON(p->talkover));
	c->rec_format = gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->rec_format));
	c->rec_bitrate = gtk_spin_button_get_value_as_int(
					GTK_SPIN_BUTTON(p->rec_bitrate));
	g_free(c->ice_host);
	c->ice_host = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->ice_host)));
	c->ice_port = gtk_spin_button_get_value_as_int(
					GTK_SPIN_BUTTON(p->ice_port));
	g_free(c->ice_mount);
	c->ice_mount = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->ice_mount)));
	g_free(c->ice_user);
	c->ice_user = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->ice_user)));
	g_free(c->ice_password);
	c->ice_password = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->ice_password)));
	g_free(c->ice_name);
	c->ice_name = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->ice_name)));
	c->ice_format = gtk_drop_down_get_selected(
				GTK_DROP_DOWN(p->ice_format)) == 0 ? ENC_MP3 :
				ENC_OPUS;
	c->ice_bitrate = gtk_spin_button_get_value_as_int(
					GTK_SPIN_BUTTON(p->ice_bitrate));
	c->xf_curve = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->xf_curve));
	c->pitch_range = range_values[gtk_drop_down_get_selected(
					GTK_DROP_DOWN(p->pitch_range))];
	c->keylock = gtk_check_button_get_active(
					GTK_CHECK_BUTTON(p->keylock));
	c->autogain = gtk_check_button_get_active(
					GTK_CHECK_BUTTON(p->autogain));
	c->quantize = gtk_check_button_get_active(
					GTK_CHECK_BUTTON(p->quantize));
	{
		int i;

		for (i = 0; i < ENGINE_DECKS; i++) {
			atomic_store(&a->engine.deck[i].autogain, c->autogain);
			atomic_store(&a->engine.deck[i].quantize, c->quantize);
		}
	}
	c->record_dir = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->record_dir)));
	c->sc_client_id = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->client_id)));
	c->sc_token = g_strdup(gtk_editable_get_text(
					GTK_EDITABLE(p->token)));
	audio_changed = c->backend != old.backend ||
			g_strcmp0(c->device, old.device) != 0 ||
			c->rate != old.rate || c->period != old.period ||
			c->hp_mode != old.hp_mode || c->mic != old.mic ||
			g_strcmp0(c->mic_device, old.mic_device) != 0;
	atomic_store(&a->engine.talkover_db, (float)c->talkover_db);
	g_free(old.device);
	g_free(old.record_dir);
	g_free(old.sc_client_id);
	g_free(old.sc_token);
	g_free(old.mic_device);
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
	midi_learn(NULL);
	if (p->midi_timer) {
		g_source_remove(p->midi_timer);
		p->midi_timer = 0;
	}
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
	p->mic = gtk_check_button_new_with_label("Microphone input with "
						 "talkover");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->mic), c->mic);
	row(g, 6, "", p->mic);
	p->mic_device = row(g, 7, "Microphone",
			    gtk_drop_down_new(NULL, NULL));
	p->talkover = gtk_spin_button_new_with_range(-40, 0, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->talkover),
				  c->talkover_db);
	gtk_widget_set_tooltip_text(p->talkover, "How much the music is "
				    "turned down while you talk, in dB");
	row(g, 8, "Talkover", p->talkover);
	fill_mic_devices(p);

	l = gtk_label_new("With PipeWire the automatic "
			  "backend uses pipewire-pulse. For the lowest "
			  "latency pick JACK and run with pipewire-jack, or "
			  "ALSA with a direct hw: device.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 9, 2, 1);
	fill_devices(p);
	g_signal_connect(p->backend, "notify::selected",
			 G_CALLBACK(on_backend), p);
}

static GtkWidget *entry_with(const char *text)
{
	GtkWidget *e = gtk_entry_new();

	gtk_editable_set_text(GTK_EDITABLE(e), text ? text : "");
	return e;
}

static void build_stream(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "Record & Stream");
	GtkStringList *names = gtk_string_list_new(NULL);
	static const char *const ice_fmts[] = { "MP3", "Opus (Ogg)", NULL };
	GtkWidget *l;
	int i;

	for (i = 0; i < ENC_COUNT; i++) {
		char *n = g_strdup_printf("%s%s", enc_format_name(i),
					  enc_format_available(i) ? "" :
					  " (not in this FFmpeg)");

		gtk_string_list_append(names, n);
		g_free(n);
	}
	p->rec_format = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(p->rec_format),
				   CLAMP(c->rec_format, 0, ENC_COUNT - 1));
	row(g, 0, "Recording format", p->rec_format);
	p->rec_bitrate = gtk_spin_button_new_with_range(64, 320, 32);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->rec_bitrate),
				  c->rec_bitrate);
	row(g, 1, "Bitrate (kbps, MP3/Opus)", p->rec_bitrate);

	l = gtk_label_new("ICECAST BROADCAST");
	gtk_widget_add_css_class(l, "section-label");
	gtk_widget_set_margin_top(l, 12);
	gtk_grid_attach(GTK_GRID(g), l, 0, 2, 2, 1);
	p->ice_host = row(g, 3, "Server", entry_with(c->ice_host));
	p->ice_port = gtk_spin_button_new_with_range(1, 65535, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->ice_port), c->ice_port);
	row(g, 4, "Port", p->ice_port);
	p->ice_mount = row(g, 5, "Mount point", entry_with(c->ice_mount));
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->ice_mount), "live.mp3");
	p->ice_user = row(g, 6, "User", entry_with(c->ice_user));
	gtk_entry_set_placeholder_text(GTK_ENTRY(p->ice_user), "source");
	p->ice_password = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(
			GTK_PASSWORD_ENTRY(p->ice_password), TRUE);
	gtk_editable_set_text(GTK_EDITABLE(p->ice_password),
			      c->ice_password ? c->ice_password : "");
	row(g, 7, "Password", p->ice_password);
	p->ice_name = row(g, 8, "Stream name", entry_with(c->ice_name));
	p->ice_format = dropdown(ice_fmts, c->ice_format == ENC_MP3 ? 0 : 1);
	row(g, 9, "Stream format", p->ice_format);
	p->ice_bitrate = gtk_spin_button_new_with_range(32, 320, 16);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->ice_bitrate),
				  c->ice_bitrate);
	row(g, 10, "Stream bitrate", p->ice_bitrate);
	l = gtk_label_new("Works with Icecast 2 and Shoutcast compatible "
			  "servers. Press LIVE in the mixer to go on air; "
			  "REC and LIVE can run at the same time.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 11, 2, 1);
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

	{
		GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		GtkWidget *b = gtk_button_new_with_label("Clear cache");
		char *size = g_format_size(sccache_size());
		char *txt = g_strdup_printf("%s of cached streams in "
					    "~/.cache/pengu-deck/soundcloud",
					    size);

		p->cache_label = gtk_label_new(txt);
		gtk_widget_add_css_class(p->cache_label, "dim-label");
		gtk_widget_set_hexpand(p->cache_label, TRUE);
		gtk_label_set_xalign(GTK_LABEL(p->cache_label), 0.0f);
		g_signal_connect(b, "clicked", G_CALLBACK(on_clear_cache), p);
		gtk_box_append(GTK_BOX(cbox), p->cache_label);
		gtk_box_append(GTK_BOX(cbox), b);
		row(g, 2, "Cache", cbox);
		g_free(txt);
		g_free(size);
	}

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
		"Tracks you load or queue are downloaded into the cache in "
		"the background and play from disk from then on.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 3, 2, 1);
}

/* ---- midi -------------------------------------------------------- */

static void refresh_midi_row(struct prefs *p, unsigned int i)
{
	unsigned int n;
	const struct midi_control *c = midi_controls(&n);
	char *b = midi_binding(c[i].name);

	gtk_label_set_text(GTK_LABEL(p->midi_labels[i]), b ? b : "–");
	if (b)
		gtk_widget_remove_css_class(p->midi_labels[i], "dim-label");
	else
		gtk_widget_add_css_class(p->midi_labels[i], "dim-label");
	g_free(b);
}

static void refresh_midi(struct prefs *p)
{
	unsigned int n, i;
	char **devs = midi_devices();
	char *joined = g_strjoinv(", ", devs);
	char *txt;

	midi_controls(&n);
	for (i = 0; i < n && i < G_N_ELEMENTS(p->midi_labels); i++)
		refresh_midi_row(p, i);
	if (!midi_available())
		txt = g_strdup("The ALSA sequencer is not available");
	else if (!*joined)
		txt = g_strdup("No MIDI device connected. Plug a controller "
			       "in; it is picked up automatically.");
	else
		txt = g_strdup_printf("Connected: %s\nLast message: %s",
				      joined, midi_last_message());
	gtk_label_set_text(GTK_LABEL(p->midi_status), txt);
	g_free(txt);
	g_free(joined);
	g_strfreev(devs);
}

static gboolean midi_tick(gpointer data)
{
	struct prefs *p = data;

	if (!p->win) {
		p->midi_timer = 0;
		return G_SOURCE_REMOVE;
	}
	refresh_midi(p);
	return G_SOURCE_CONTINUE;
}

static void on_learn(GtkButton *b, struct prefs *p)
{
	const char *name = g_object_get_data(G_OBJECT(b), "control");

	if (midi_learning()) {
		midi_learn(NULL);
		set_status(p, "Learn cancelled", FALSE);
		return;
	}
	midi_learn(name);
	set_status(p, "Move or press the control on your device…", FALSE);
}

static void on_unlearn(GtkButton *b, struct prefs *p)
{
	midi_unbind(g_object_get_data(G_OBJECT(b), "control"));
	refresh_midi(p);
}

static void build_midi(struct prefs *p, GtkWidget *nb)
{
	GtkWidget *g = page(nb, "MIDI");
	GtkWidget *scroll, *grid, *l, *b;
	unsigned int n, i;
	const struct midi_control *c = midi_controls(&n);

	p->midi_status = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(p->midi_status), TRUE);
	gtk_label_set_xalign(GTK_LABEL(p->midi_status), 0.0f);
	gtk_grid_attach(GTK_GRID(g), p->midi_status, 0, 0, 2, 1);

	l = gtk_label_new("Press Learn next to a function, then move the "
			  "knob or press the button on your controller. "
			  "Pioneer DDJ controllers and CDJs in MIDI mode work "
			  "without any driver. The mapping is stored in "
			  "~/.config/pengu-deck/midi.ini.");
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 1, 2, 1);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	for (i = 0; i < n && i < G_N_ELEMENTS(p->midi_labels); i++) {
		l = gtk_label_new(c[i].label);
		gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
		gtk_widget_set_hexpand(l, TRUE);
		gtk_grid_attach(GTK_GRID(grid), l, 0, (int)i, 1, 1);
		p->midi_labels[i] = gtk_label_new("");
		gtk_label_set_xalign(GTK_LABEL(p->midi_labels[i]), 0.0f);
		gtk_label_set_width_chars(GTK_LABEL(p->midi_labels[i]), 14);
		gtk_widget_add_css_class(p->midi_labels[i], "mono");
		gtk_grid_attach(GTK_GRID(grid), p->midi_labels[i], 1, (int)i,
				1, 1);
		b = gtk_button_new_with_label("Learn");
		g_object_set_data(G_OBJECT(b), "control", (gpointer)c[i].name);
		g_signal_connect(b, "clicked", G_CALLBACK(on_learn), p);
		gtk_grid_attach(GTK_GRID(grid), b, 2, (int)i, 1, 1);
		b = gtk_button_new_from_icon_name("edit-clear-symbolic");
		gtk_widget_set_tooltip_text(b, "Remove this mapping");
		g_object_set_data(G_OBJECT(b), "control", (gpointer)c[i].name);
		g_signal_connect(b, "clicked", G_CALLBACK(on_unlearn), p);
		gtk_grid_attach(GTK_GRID(grid), b, 3, (int)i, 1, 1);
	}
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_widget_set_hexpand(scroll, TRUE);
	gtk_grid_attach(GTK_GRID(g), scroll, 0, 2, 2, 1);
	p->midi_grid = grid;

	refresh_midi(p);
	p->midi_timer = g_timeout_add(500, midi_tick, p);
}

static void build_decks(struct prefs *p, GtkWidget *nb)
{
	struct config *c = &p->app->cfg;
	GtkWidget *g = page(nb, "Decks");
	guint sel = c->pitch_range == 16 ? 1 : c->pitch_range == 50 ? 2 :
		    c->pitch_range == 100 ? 3 : 0;

	p->pitch_range = row(g, 0, "Pitch range", dropdown(range_names,
							   sel));
	p->keylock = gtk_check_button_new_with_label("Enable keylock on "
						     "new tracks");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->keylock),
				    c->keylock);
	gtk_widget_set_sensitive(p->keylock, deck_has_keylock());
	row(g, 1, "", p->keylock);
	p->autogain = gtk_check_button_new_with_label("Auto gain: level "
						      "tracks to -18 dBFS RMS");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->autogain),
				    c->autogain);
	row(g, 2, "", p->autogain);
	p->quantize = gtk_check_button_new_with_label("Quantize cues and "
						      "loops to the beat "
						      "grid by default");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(p->quantize),
				    c->quantize);
	row(g, 3, "", p->quantize);
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
	build_stream(p, nb);
	build_midi(p, nb);
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
