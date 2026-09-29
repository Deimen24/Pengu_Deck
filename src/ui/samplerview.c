// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * samplerview.c - eight sample pads
 *
 * Click or hold a pad to play it, right click for the mode and to load
 * or clear a sample; audio files can also be dropped onto a pad.  The
 * pad assignments are remembered in the settings.
 */
#include "decoder.h"
#include "knob.h"
#include "samplerview.h"

struct _PdSamplerView {
	GtkBox parent;
	struct app *app;
	GtkWidget *pad[SAMPLER_PADS];
	GtkWidget *name[SAMPLER_PADS];
	GtkWidget *bar[SAMPLER_PADS];
	guint tick;
};

G_DEFINE_FINAL_TYPE(PdSamplerView, pd_sampler_view, GTK_TYPE_BOX)

static struct sampler *sampler_of(PdSamplerView *v)
{
	return &v->app->engine.sampler;
}

static void set_pad_name(PdSamplerView *v, int i)
{
	struct track *t = atomic_load(&sampler_of(v)->pad[i].track);
	char *title = t ? track_title(t) : NULL;

	gtk_label_set_text(GTK_LABEL(v->name[i]), title ? title : "empty");
	gtk_widget_set_sensitive(v->pad[i], t != NULL);
	g_free(title);
}

static void load_sample(PdSamplerView *v, int i, const char *path)
{
	unsigned int rate = v->app->engine.rate ? v->app->engine.rate : 48000;
	struct track *t = track_new(path, path, rate);
	char *base = g_path_get_basename(path);

	track_set_meta(t, base, NULL);
	atomic_store(&t->analysed, true);	/* no tempo needed */
	sampler_load(sampler_of(v), i, t);
	decoder_start(t);
	track_unref(t);
	g_free(base);
	g_free(v->app->cfg.samples[i]);
	v->app->cfg.samples[i] = g_strdup(path);
	config_save(&v->app->cfg);
	set_pad_name(v, i);
}

static void pad_press(GtkGestureClick *g, int n, double x, double y,
		      gpointer data)
{
	PdSamplerView *v = g_object_get_data(G_OBJECT(g), "view");

	sampler_trigger(sampler_of(v), GPOINTER_TO_INT(data), true);
}

static void pad_release(GtkGestureClick *g, int n, double x, double y,
			gpointer data)
{
	PdSamplerView *v = g_object_get_data(G_OBJECT(g), "view");

	sampler_trigger(sampler_of(v), GPOINTER_TO_INT(data), false);
}

static void file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	PdSamplerView *v = g_object_get_data(G_OBJECT(src), "view");
	GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
	char *path;

	if (!f)
		return;
	path = g_file_get_path(f);
	if (path)
		load_sample(v, GPOINTER_TO_INT(data), path);
	g_free(path);
	g_object_unref(f);
}

static void on_load(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdSamplerView *v = data;
	GtkFileDialog *d = gtk_file_dialog_new();

	gtk_file_dialog_set_title(d, "Load sample");
	g_object_set_data(G_OBJECT(d), "view", v);
	gtk_file_dialog_open(d, v->app->win, NULL, file_chosen,
			     GINT_TO_POINTER(g_variant_get_int32(p)));
	g_object_unref(d);
}

static void on_clear(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdSamplerView *v = data;
	int i = g_variant_get_int32(p);

	sampler_clear(sampler_of(v), i);
	g_clear_pointer(&v->app->cfg.samples[i], g_free);
	config_save(&v->app->cfg);
	set_pad_name(v, i);
}

static void on_mode(GSimpleAction *a, GVariant *p, gpointer data)
{
	PdSamplerView *v = data;
	gint32 pad, mode;

	g_variant_get(p, "(ii)", &pad, &mode);
	atomic_store(&sampler_of(v)->pad[pad].mode, mode);
}

static void on_right_click(GtkGestureClick *g, int n, double x, double y,
			   gpointer data)
{
	PdSamplerView *v = g_object_get_data(G_OBJECT(g), "view");
	int i = GPOINTER_TO_INT(data);
	GtkWidget *pop = g_object_get_data(G_OBJECT(v->pad[i]), "popover");
	GMenu *menu = g_menu_new(), *modes = g_menu_new();
	GMenuItem *item;
	static const char *const mode_names[] = {
		"One shot", "Loop", "Hold",
	};
	int k;

	item = g_menu_item_new("Load sample…", NULL);
	g_menu_item_set_action_and_target(item, "pad.load", "i", i);
	g_menu_append_item(menu, item);
	g_object_unref(item);
	for (k = 0; k < 3; k++) {
		item = g_menu_item_new(mode_names[k], NULL);
		g_menu_item_set_action_and_target(item, "pad.mode", "(ii)", i,
						  k);
		g_menu_append_item(modes, item);
		g_object_unref(item);
	}
	g_menu_append_section(menu, "Mode", G_MENU_MODEL(modes));
	item = g_menu_item_new("Clear", NULL);
	g_menu_item_set_action_and_target(item, "pad.clear", "i", i);
	g_menu_append_item(menu, item);
	g_object_unref(item);
	gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pop),
					G_MENU_MODEL(menu));
	gtk_popover_popup(GTK_POPOVER(pop));
	g_object_unref(modes);
	g_object_unref(menu);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *val, double x,
			double y, gpointer data)
{
	PdSamplerView *v = g_object_get_data(G_OBJECT(t), "view");
	GSList *files = g_value_get_boxed(val);
	char *path;

	if (!files)
		return FALSE;
	path = g_file_get_path(files->data);
	if (path)
		load_sample(v, GPOINTER_TO_INT(data), path);
	g_free(path);
	return path != NULL;
}

static void on_volume(PdKnob *k, PdSamplerView *v)
{
	atomic_store(&sampler_of(v)->volume, (float)pd_knob_get_value(k));
}

static void on_stop_all(GtkButton *b, PdSamplerView *v)
{
	sampler_stop_all(sampler_of(v));
}

static gboolean tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	PdSamplerView *v = PD_SAMPLER_VIEW(w);
	struct sampler *s = sampler_of(v);
	int i;

	for (i = 0; i < SAMPLER_PADS; i++) {
		struct track *t = atomic_load(&s->pad[i].track);
		gboolean playing = atomic_load(&s->pad[i].playing);
		double frac = 0.0;

		if (t && track_length(t) > 0)
			frac = atomic_load(&s->pad[i].pos) /
			       (double)track_length(t);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->bar[i]),
					      playing ? frac : 0.0);
		if (playing)
			gtk_widget_add_css_class(v->pad[i], "playing");
		else
			gtk_widget_remove_css_class(v->pad[i], "playing");
		if (t && !g_object_get_data(G_OBJECT(v->pad[i]), "named")) {
			set_pad_name(v, i);
			g_object_set_data(G_OBJECT(v->pad[i]), "named",
					  GINT_TO_POINTER(1));
		}
	}
	return G_SOURCE_CONTINUE;
}

static void pd_sampler_view_dispose(GObject *obj)
{
	PdSamplerView *v = PD_SAMPLER_VIEW(obj);

	if (v->tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(v), v->tick);
		v->tick = 0;
	}
	G_OBJECT_CLASS(pd_sampler_view_parent_class)->dispose(obj);
}

static void pd_sampler_view_class_init(PdSamplerViewClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = pd_sampler_view_dispose;
}

static void pd_sampler_view_init(PdSamplerView *v)
{
}

static GtkWidget *build_pad(PdSamplerView *v, int i)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *num, *pop;
	GtkGesture *g;
	GtkDropTarget *drop;
	char label[8];

	g_snprintf(label, sizeof(label), "%d", i + 1);
	num = gtk_label_new(label);
	gtk_widget_add_css_class(num, "sample-number");
	v->name[i] = gtk_label_new("empty");
	gtk_label_set_ellipsize(GTK_LABEL(v->name[i]), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(v->name[i]), 14);
	gtk_widget_add_css_class(v->name[i], "sample-name");
	gtk_box_append(GTK_BOX(box), num);
	gtk_box_append(GTK_BOX(box), v->name[i]);
	v->bar[i] = gtk_progress_bar_new();
	gtk_widget_add_css_class(v->bar[i], "sample-bar");
	gtk_box_append(GTK_BOX(box), v->bar[i]);

	v->pad[i] = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(v->pad[i]), box);
	gtk_widget_add_css_class(v->pad[i], "sample-pad");
	gtk_widget_set_focusable(v->pad[i], FALSE);
	gtk_widget_set_hexpand(v->pad[i], TRUE);
	gtk_widget_set_vexpand(v->pad[i], TRUE);
	gtk_widget_set_tooltip_text(v->pad[i], "Click to play, right click "
				    "to load or change the mode, drop an "
				    "audio file to load it");

	g = gtk_gesture_click_new();
	g_object_set_data(G_OBJECT(g), "view", v);
	g_signal_connect(g, "pressed", G_CALLBACK(pad_press),
			 GINT_TO_POINTER(i));
	g_signal_connect(g, "released", G_CALLBACK(pad_release),
			 GINT_TO_POINTER(i));
	g_signal_connect(g, "cancel", G_CALLBACK(pad_release),
			 GINT_TO_POINTER(i));
	gtk_widget_add_controller(v->pad[i], GTK_EVENT_CONTROLLER(g));

	g = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g),
				      GDK_BUTTON_SECONDARY);
	g_object_set_data(G_OBJECT(g), "view", v);
	g_signal_connect(g, "pressed", G_CALLBACK(on_right_click),
			 GINT_TO_POINTER(i));
	gtk_widget_add_controller(v->pad[i], GTK_EVENT_CONTROLLER(g));

	pop = gtk_popover_menu_new_from_model(NULL);
	gtk_widget_set_parent(pop, v->pad[i]);
	g_object_set_data(G_OBJECT(v->pad[i]), "popover", pop);

	drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_object_set_data(G_OBJECT(drop), "view", v);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop),
			 GINT_TO_POINTER(i));
	gtk_widget_add_controller(v->pad[i], GTK_EVENT_CONTROLLER(drop));
	return v->pad[i];
}

GtkWidget *pd_sampler_view_new(struct app *app)
{
	PdSamplerView *v = g_object_new(PD_TYPE_SAMPLER_VIEW,
					"orientation",
					GTK_ORIENTATION_HORIZONTAL,
					"spacing", 10, NULL);
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *k, *b, *l;
	static const GActionEntry entries[] = {
		{ "load", on_load, "i" },
		{ "clear", on_clear, "i" },
		{ "mode", on_mode, "(ii)" },
	};
	GSimpleActionGroup *grp = g_simple_action_group_new();
	int i;

	v->app = app;
	g_action_map_add_action_entries(G_ACTION_MAP(grp), entries,
					G_N_ELEMENTS(entries), v);
	gtk_widget_insert_action_group(GTK_WIDGET(v), "pad",
				       G_ACTION_GROUP(grp));
	g_object_unref(grp);

	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
	for (i = 0; i < SAMPLER_PADS; i++)
		gtk_grid_attach(GTK_GRID(grid), build_pad(v, i), i % 4, i / 4,
				1, 1);
	gtk_widget_set_hexpand(grid, TRUE);
	gtk_widget_set_vexpand(grid, TRUE);
	gtk_box_append(GTK_BOX(v), grid);

	l = gtk_label_new("SAMPLER");
	gtk_widget_add_css_class(l, "section-label");
	gtk_box_append(GTK_BOX(side), l);
	k = pd_knob_new("VOL", 0.0, 1.0, 0.8);
	pd_knob_set_detent(PD_KNOB(k), FALSE);
	pd_knob_set_accent(PD_KNOB(k), "#f472b6");
	g_signal_connect(k, "changed", G_CALLBACK(on_volume), v);
	gtk_box_append(GTK_BOX(side), k);
	b = gtk_button_new_with_label("Stop all");
	gtk_widget_set_focusable(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_stop_all), v);
	gtk_box_append(GTK_BOX(side), b);
	gtk_widget_set_valign(side, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(v), side);

	for (i = 0; i < SAMPLER_PADS; i++)
		if (app->cfg.samples[i] && *app->cfg.samples[i])
			load_sample(v, i, app->cfg.samples[i]);

	v->tick = gtk_widget_add_tick_callback(GTK_WIDGET(v), tick, NULL,
					       NULL);
	return GTK_WIDGET(v);
}
