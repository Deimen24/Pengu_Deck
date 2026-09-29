// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * playlists.c - crates, play history and Rekordbox import
 */
#include <string.h>

#include <glib/gstdio.h>

#include "config.h"
#include "cuestore.h"
#include "playlists.h"

#define FIELDS		9
#define FLUSH_DELAY	2

static GPtrArray *lists;	/* struct playlist */
static GListStore *history;
static char *lists_path;
static char *history_path;
static guint flush_id;
static void (*changed_cb)(gpointer data);
static gpointer changed_data;

/* ---- serialisation ----------------------------------------------- */

static char *clean(const char *s)
{
	char *c = g_strdup(s ? s : ""), *p;

	for (p = c; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
	return c;
}

/* playlist \t key \t location \t title \t artist \t album \t genre \t
 * duration \t bpm */
static void append_line(GString *s, const char *list, PdMediaItem *m)
{
	char *f[6] = {
		clean(list), clean(m->key), clean(m->location),
		clean(m->title), clean(m->artist), clean(m->album),
	};
	char *genre = clean(m->genre);
	char num[2][G_ASCII_DTOSTR_BUF_SIZE];
	int i;

	g_ascii_dtostr(num[0], sizeof(num[0]), m->duration);
	g_ascii_dtostr(num[1], sizeof(num[1]), m->bpm);
	g_string_append_printf(s, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			       f[0], f[1], f[2], f[3], f[4], f[5], genre,
			       num[0], num[1]);
	for (i = 0; i < 6; i++)
		g_free(f[i]);
	g_free(genre);
}

static PdMediaItem *item_from_fields(char **f)
{
	enum media_source src = g_str_has_prefix(f[1], "soundcloud:") ?
				MEDIA_SOUNDCLOUD : MEDIA_LOCAL;
	PdMediaItem *m = pd_media_item_new(src, f[1]);

	m->location = g_strdup(f[2]);
	m->title = g_strdup(f[3]);
	m->artist = g_strdup(f[4]);
	m->album = g_strdup(f[5]);
	m->genre = g_strdup(f[6]);
	m->duration = g_ascii_strtod(f[7], NULL);
	m->bpm = g_ascii_strtod(f[8], NULL);
	return m;
}

static void save_lists(void)
{
	GString *s = g_string_new("# pengu-deck playlists v1\n");
	guint i, j;

	for (i = 0; i < lists->len; i++) {
		struct playlist *p = lists->pdata[i];
		guint n = g_list_model_get_n_items(G_LIST_MODEL(p->items));

		/* An empty playlist still needs a line to exist. */
		if (!n) {
			char *name = clean(p->name);

			g_string_append_printf(s, "%s\t\t\t\t\t\t\t0\t0\n",
					       name);
			g_free(name);
		}
		for (j = 0; j < n; j++) {
			PdMediaItem *m = g_list_model_get_item(
					G_LIST_MODEL(p->items), j);

			append_line(s, p->name, m);
			g_object_unref(m);
		}
	}
	g_file_set_contents(lists_path, s->str, (gssize)s->len, NULL);
	g_string_free(s, TRUE);
}

static gboolean flush_cb(gpointer data)
{
	flush_id = 0;
	save_lists();
	return G_SOURCE_REMOVE;
}

static void schedule_save(void)
{
	if (!flush_id)
		flush_id = g_timeout_add_seconds(FLUSH_DELAY, flush_cb, NULL);
	if (changed_cb)
		changed_cb(changed_data);
}

static void playlist_free(gpointer data)
{
	struct playlist *p = data;

	g_free(p->name);
	g_object_unref(p->items);
	g_free(p);
}

static void load_lists(void)
{
	char *text = NULL, **lines, **l;

	if (!g_file_get_contents(lists_path, &text, NULL, NULL))
		return;
	lines = g_strsplit(text, "\n", -1);
	for (l = lines; *l; l++) {
		char **f;
		struct playlist *p;

		if (!**l || **l == '#')
			continue;
		f = g_strsplit(*l, "\t", FIELDS);
		if (g_strv_length(f) == FIELDS) {
			p = playlists_find(f[0]);
			if (!p)
				p = playlists_create(f[0]);
			if (*f[1])
				g_list_store_append(p->items,
						    item_from_fields(f + 1) -
						    0);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(text);
	/* creating lists scheduled saves, none needed */
	if (flush_id) {
		g_source_remove(flush_id);
		flush_id = 0;
	}
}

void playlists_open(void)
{
	char *dir;

	if (lists)
		return;
	dir = config_data_dir();
	lists_path = g_build_filename(dir, "playlists.tsv", NULL);
	history_path = g_build_filename(dir, "history.tsv", NULL);
	g_free(dir);
	lists = g_ptr_array_new_with_free_func(playlist_free);
	history = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	load_lists();
}

void playlists_close(void)
{
	if (!lists)
		return;
	if (flush_id) {
		g_source_remove(flush_id);
		flush_id = 0;
	}
	save_lists();
	g_ptr_array_unref(lists);
	lists = NULL;
	g_clear_object(&history);
	g_clear_pointer(&lists_path, g_free);
	g_clear_pointer(&history_path, g_free);
}

/* ---- playlists --------------------------------------------------- */

GPtrArray *playlists_all(void)
{
	return lists;
}

struct playlist *playlists_find(const char *name)
{
	guint i;

	for (i = 0; lists && i < lists->len; i++) {
		struct playlist *p = lists->pdata[i];

		if (g_str_equal(p->name, name))
			return p;
	}
	return NULL;
}

struct playlist *playlists_create(const char *name)
{
	struct playlist *p;

	if (!lists || !name || !*name)
		return NULL;
	p = playlists_find(name);
	if (p)
		return p;
	p = g_new0(struct playlist, 1);
	p->name = g_strdup(name);
	p->items = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	g_ptr_array_add(lists, p);
	schedule_save();
	return p;
}

void playlists_delete(const char *name)
{
	struct playlist *p = playlists_find(name);

	if (!p)
		return;
	g_ptr_array_remove(lists, p);
	schedule_save();
}

void playlists_rename(struct playlist *p, const char *name)
{
	if (!p || !name || !*name || playlists_find(name))
		return;
	g_free(p->name);
	p->name = g_strdup(name);
	schedule_save();
}

void playlists_add(struct playlist *p, PdMediaItem *m)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(p->items)), i;

	for (i = 0; i < n; i++) {
		PdMediaItem *x = g_list_model_get_item(G_LIST_MODEL(p->items),
						       i);
		gboolean dup = g_str_equal(x->key, m->key);

		g_object_unref(x);
		if (dup)
			return;
	}
	g_list_store_append(p->items, m);
	schedule_save();
}

void playlists_remove(struct playlist *p, guint pos)
{
	if (pos < g_list_model_get_n_items(G_LIST_MODEL(p->items))) {
		g_list_store_remove(p->items, pos);
		schedule_save();
	}
}

void playlists_set_changed_cb(void (*cb)(gpointer data), gpointer data)
{
	changed_cb = cb;
	changed_data = data;
}

gboolean playlists_export_m3u(struct playlist *p, const char *path,
			      GError **err)
{
	GString *s = g_string_new("#EXTM3U\n");
	guint n = g_list_model_get_n_items(G_LIST_MODEL(p->items)), i;
	gboolean ok;

	for (i = 0; i < n; i++) {
		PdMediaItem *m = g_list_model_get_item(G_LIST_MODEL(p->items),
						       i);

		g_string_append_printf(s, "#EXTINF:%d,%s - %s\n%s\n",
				       (int)m->duration,
				       m->artist ? m->artist : "",
				       m->title ? m->title : "",
				       m->source == MEDIA_SOUNDCLOUD &&
				       m->permalink ? m->permalink :
				       m->location ? m->location : "");
		g_object_unref(m);
	}
	ok = g_file_set_contents(path, s->str, (gssize)s->len, err);
	g_string_free(s, TRUE);
	return ok;
}

/* ---- history ----------------------------------------------------- */

GListStore *history_items(void)
{
	return history;
}

void history_add(PdMediaItem *m)
{
	GDateTime *now = g_date_time_new_now_local();
	char *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
	char *title = clean(m->title), *artist = clean(m->artist);
	char *line = g_strdup_printf("%s\t%s\t%s\t%s\n", stamp, m->key,
				     artist, title);
	FILE *f;

	if (history) {
		/* newest first */
		g_object_set_data_full(G_OBJECT(m), "played-at",
				       g_strdup(stamp), g_free);
		g_list_store_insert(history, 0, m);
	}
	f = history_path ? g_fopen(history_path, "a") : NULL;
	if (f) {
		fputs(line, f);
		fclose(f);
	}
	g_free(line);
	g_free(title);
	g_free(artist);
	g_free(stamp);
	g_date_time_unref(now);
}

gboolean history_export(const char *path, GError **err)
{
	GString *s = g_string_new("");
	guint n = history ? g_list_model_get_n_items(G_LIST_MODEL(history)) : 0;
	guint i;
	gboolean ok;

	for (i = n; i > 0; i--) {
		PdMediaItem *m = g_list_model_get_item(G_LIST_MODEL(history),
						       i - 1);
		const char *at = g_object_get_data(G_OBJECT(m), "played-at");

		g_string_append_printf(s, "%s  %s - %s\n", at ? at : "",
				       m->artist && *m->artist ? m->artist :
				       "Unknown artist", m->title);
		g_object_unref(m);
	}
	ok = g_file_set_contents(path, s->str, (gssize)s->len, err);
	g_string_free(s, TRUE);
	return ok;
}

/* ---- rekordbox xml ----------------------------------------------- */

struct rb_track {
	PdMediaItem *item;
	struct track_info info;
	gboolean has_info;
};

struct rb_ctx {
	struct rb_import *out;
	GHashTable *by_id;		/* TrackID → struct rb_track */
	struct rb_track *cur;
	struct playlist *cur_list;
	int depth_in_playlists;
	gboolean in_playlists;
};

static const char *attr(const char **names, const char **values,
			const char *name)
{
	int i;

	for (i = 0; names[i]; i++)
		if (g_str_equal(names[i], name))
			return values[i];
	return NULL;
}

static double attr_d(const char **n, const char **v, const char *name)
{
	const char *s = attr(n, v, name);

	return s ? g_ascii_strtod(s, NULL) : 0.0;
}

static char *location_to_path(const char *loc)
{
	const char *p = loc;
	char *path;

	if (!p)
		return NULL;
	if (g_str_has_prefix(p, "file://localhost"))
		p += strlen("file://localhost");
	else if (g_str_has_prefix(p, "file://"))
		p += strlen("file://");
	path = g_uri_unescape_string(p, NULL);
	return path;
}

static void rb_track_free(gpointer data)
{
	struct rb_track *t = data;

	g_clear_object(&t->item);
	g_free(t);
}

static void start_track(struct rb_ctx *c, const char **n, const char **v)
{
	const char *id = attr(n, v, "TrackID");
	char *path = location_to_path(attr(n, v, "Location"));
	struct rb_track *t;
	PdMediaItem *m;
	int i;

	if (!id || !path)
		return;
	m = pd_media_item_new(MEDIA_LOCAL, path);
	m->location = path;
	m->title = g_strdup(attr(n, v, "Name"));
	m->artist = g_strdup(attr(n, v, "Artist"));
	m->album = g_strdup(attr(n, v, "Album"));
	m->genre = g_strdup(attr(n, v, "Genre"));
	m->bpm = attr_d(n, v, "AverageBpm");
	m->duration = attr_d(n, v, "TotalTime");
	if (!m->title)
		m->title = g_path_get_basename(path);
	if (!m->artist)
		m->artist = g_strdup("");
	if (!m->album)
		m->album = g_strdup("");
	if (!m->genre)
		m->genre = g_strdup("");

	t = g_new0(struct rb_track, 1);
	t->item = m;
	t->info.bpm = m->bpm;
	t->info.mkey = -1;
	for (i = 0; i < DECK_HOTCUES; i++)
		t->info.hotcue[i] = -1.0;
	g_hash_table_insert(c->by_id, g_strdup(id), t);
	g_ptr_array_add(c->out->tracks, g_object_ref(m));
	c->cur = t;
}

static void start_element(GMarkupParseContext *ctx, const char *el,
			  const char **n, const char **v, gpointer data,
			  GError **err)
{
	struct rb_ctx *c = data;

	if (g_str_equal(el, "PLAYLISTS")) {
		c->in_playlists = TRUE;
		return;
	}
	if (c->in_playlists) {
		if (g_str_equal(el, "NODE")) {
			const char *type = attr(n, v, "Type");
			const char *name = attr(n, v, "Name");

			c->depth_in_playlists++;
			if (type && g_str_equal(type, "1") && name) {
				c->cur_list = playlists_create(name);
				if (c->cur_list)
					c->out->playlists++;
			}
		} else if (g_str_equal(el, "TRACK") && c->cur_list) {
			const char *key = attr(n, v, "Key");
			struct rb_track *t = key ?
				g_hash_table_lookup(c->by_id, key) : NULL;

			if (t)
				playlists_add(c->cur_list, t->item);
		}
		return;
	}
	if (g_str_equal(el, "TRACK")) {
		start_track(c, n, v);
	} else if (g_str_equal(el, "TEMPO") && c->cur) {
		double inizio = attr_d(n, v, "Inizio");
		double bpm = attr_d(n, v, "Bpm");

		/* First tempo marker defines the grid we support. */
		if (!c->cur->has_info) {
			c->cur->info.beat_offset = inizio;
			if (bpm > 0.0)
				c->cur->info.bpm = bpm;
			c->cur->has_info = TRUE;
		}
	} else if (g_str_equal(el, "POSITION_MARK") && c->cur) {
		int num = (int)attr_d(n, v, "Num");
		double start = attr_d(n, v, "Start");

		if (num >= 0 && num < DECK_HOTCUES) {
			c->cur->info.hotcue[num] = start;
			c->out->cues++;
		} else if (num < 0 && c->cur->info.cue <= 0.0) {
			c->cur->info.cue = start;
			c->out->cues++;
		}
		c->cur->has_info = TRUE;
	}
}

static void end_element(GMarkupParseContext *ctx, const char *el,
			gpointer data, GError **err)
{
	struct rb_ctx *c = data;

	if (g_str_equal(el, "PLAYLISTS")) {
		c->in_playlists = FALSE;
	} else if (c->in_playlists && g_str_equal(el, "NODE")) {
		c->depth_in_playlists--;
		c->cur_list = NULL;
	} else if (g_str_equal(el, "TRACK") && !c->in_playlists && c->cur) {
		if (c->cur->has_info)
			cuestore_put(c->cur->item->key, &c->cur->info);
		c->cur = NULL;
	}
}

gboolean rekordbox_import(const char *xml_path, struct rb_import *out,
			  GError **err)
{
	GMarkupParser parser = { start_element, end_element, NULL, NULL, NULL };
	struct rb_ctx c = {
		.out = out,
		.by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					       rb_track_free),
	};
	GMarkupParseContext *ctx;
	char *text = NULL;
	gsize len;
	gboolean ok;

	memset(out, 0, sizeof(*out));
	out->tracks = g_ptr_array_new_with_free_func(g_object_unref);
	if (!g_file_get_contents(xml_path, &text, &len, err)) {
		g_hash_table_unref(c.by_id);
		return FALSE;
	}
	ctx = g_markup_parse_context_new(&parser, 0, &c, NULL);
	ok = g_markup_parse_context_parse(ctx, text, (gssize)len, err) &&
	     g_markup_parse_context_end_parse(ctx, err);
	g_markup_parse_context_free(ctx);
	g_hash_table_unref(c.by_id);
	g_free(text);
	return ok;
}

void rb_import_clear(struct rb_import *out)
{
	g_clear_pointer(&out->tracks, g_ptr_array_unref);
}
