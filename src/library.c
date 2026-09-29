// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * library.c - scanning music folders
 *
 * The tag cache is a tab separated file in the user cache directory with
 * one line per file: path, mtime, title, artist, album, genre, duration
 * and tag bpm.  Tabs and newlines inside tags are replaced by spaces.
 */
#include <stdbool.h>
#include <string.h>

#include <glib/gstdio.h>

#include "config.h"
#include "decoder.h"
#include "library.h"

#define CACHE_FIELDS	8
#define CACHE_VERSION	"# pengu-deck library cache v1"

static const char *const audio_ext[] = {
	".mp3", ".flac", ".wav", ".ogg", ".oga", ".opus", ".m4a", ".aac",
	".aif", ".aiff", ".wma", ".alac", ".mp4", ".webm", ".mka", ".ape",
	".wv", NULL,
};

bool library_is_audio(const char *path)
{
	char *lower = g_ascii_strdown(path, -1);
	bool ok = false;
	int i;

	for (i = 0; audio_ext[i] && !ok; i++)
		ok = g_str_has_suffix(lower, audio_ext[i]);
	g_free(lower);
	return ok;
}

static char *cache_path(void)
{
	char *dir = config_cache_dir();
	char *path = g_build_filename(dir, "library.tsv", NULL);

	g_free(dir);
	return path;
}

static char *clean(const char *s)
{
	char *c = g_strdup(s ? s : ""), *p;

	for (p = c; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
	return c;
}

static PdMediaItem *item_from_line(char *line)
{
	char **f = g_strsplit(line, "\t", CACHE_FIELDS);
	PdMediaItem *m = NULL;

	if (g_strv_length(f) == CACHE_FIELDS) {
		m = pd_media_item_new(MEDIA_LOCAL, f[0]);
		m->location = g_strdup(f[0]);
		m->mtime = g_ascii_strtoll(f[1], NULL, 10);
		m->title = g_strdup(f[2]);
		m->artist = g_strdup(f[3]);
		m->album = g_strdup(f[4]);
		m->genre = g_strdup(f[5]);
		m->duration = g_ascii_strtod(f[6], NULL);
		m->bpm = g_ascii_strtod(f[7], NULL);
	}
	g_strfreev(f);
	return m;
}

static GHashTable *cache_load(void)
{
	GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					      g_object_unref);
	char *path = cache_path(), *text = NULL;
	char **lines, **l;

	if (!g_file_get_contents(path, &text, NULL, NULL) ||
	    !g_str_has_prefix(text, CACHE_VERSION)) {
		g_free(text);
		g_free(path);
		return h;
	}
	lines = g_strsplit(text, "\n", -1);
	for (l = lines + 1; *l; l++) {
		PdMediaItem *m = **l ? item_from_line(*l) : NULL;

		if (m)
			g_hash_table_replace(h, m->key, m);
	}
	g_strfreev(lines);
	g_free(text);
	g_free(path);
	return h;
}

static void cache_save(GPtrArray *items)
{
	GString *s = g_string_new(CACHE_VERSION "\n");
	char *path = cache_path();
	char num[2][G_ASCII_DTOSTR_BUF_SIZE];
	guint i;

	for (i = 0; i < items->len; i++) {
		PdMediaItem *m = items->pdata[i];
		char *t = clean(m->title), *a = clean(m->artist);
		char *al = clean(m->album), *g = clean(m->genre);

		g_ascii_dtostr(num[0], sizeof(num[0]), m->duration);
		g_ascii_dtostr(num[1], sizeof(num[1]), m->bpm);
		g_string_append_printf(s, "%s\t%" G_GINT64_FORMAT
				       "\t%s\t%s\t%s\t%s\t%s\t%s\n",
				       m->key, m->mtime, t, a, al, g,
				       num[0], num[1]);
		g_free(t);
		g_free(a);
		g_free(al);
		g_free(g);
	}
	g_file_set_contents(path, s->str, (gssize)s->len, NULL);
	g_string_free(s, TRUE);
	g_free(path);
}

PdMediaItem *library_probe_file(const char *path)
{
	struct media_tags tags;
	PdMediaItem *m = pd_media_item_new(MEDIA_LOCAL, path);
	GStatBuf st;

	m->location = g_strdup(path);
	if (g_stat(path, &st) == 0)
		m->mtime = st.st_mtime;
	if (decoder_probe(path, &tags) == 0) {
		m->title = g_strdup(tags.title);
		m->artist = g_strdup(tags.artist);
		m->album = g_strdup(tags.album);
		m->genre = g_strdup(tags.genre);
		m->duration = tags.duration;
		m->bpm = tags.bpm;
		media_tags_clear(&tags);
	}
	if (!m->title || !*m->title) {
		char *base = g_path_get_basename(path);
		char *dot = strrchr(base, '.');

		if (dot)
			*dot = '\0';
		g_free(m->title);
		m->title = base;
	}
	if (!m->artist)
		m->artist = g_strdup("");
	if (!m->album)
		m->album = g_strdup("");
	if (!m->genre)
		m->genre = g_strdup("");
	return m;
}

struct scan {
	GHashTable *cache;
	GHashTable *seen;
	GPtrArray *items;
	GCancellable *cancel;
};

static void scan_file(struct scan *s, const char *path)
{
	PdMediaItem *m;
	GStatBuf st;

	if (g_hash_table_contains(s->seen, path))
		return;
	if (g_stat(path, &st) != 0)
		return;
	m = g_hash_table_lookup(s->cache, path);
	if (m && m->mtime == (gint64)st.st_mtime)
		g_object_ref(m);
	else
		m = library_probe_file(path);
	g_hash_table_add(s->seen, m->key);
	g_ptr_array_add(s->items, m);
}

static void scan_dir(struct scan *s, const char *dir, int depth)
{
	GDir *d;
	const char *name;

	if (depth > 32 || g_cancellable_is_cancelled(s->cancel))
		return;
	d = g_dir_open(dir, 0, NULL);
	if (!d)
		return;
	while ((name = g_dir_read_name(d))) {
		char *path;

		if (name[0] == '.')
			continue;
		path = g_build_filename(dir, name, NULL);
		if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
			if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK))
				scan_dir(s, path, depth + 1);
		} else if (library_is_audio(path)) {
			scan_file(s, path);
		}
		g_free(path);
	}
	g_dir_close(d);
}

static void scan_thread(GTask *task, gpointer src, gpointer data,
			GCancellable *cancel)
{
	char **folders = data;
	struct scan s = {
		.cache = cache_load(),
		.seen = g_hash_table_new(g_str_hash, g_str_equal),
		.items = g_ptr_array_new_with_free_func(g_object_unref),
		.cancel = cancel,
	};
	int i;

	for (i = 0; folders && folders[i]; i++)
		scan_dir(&s, folders[i], 0);

	g_hash_table_unref(s.seen);
	g_hash_table_unref(s.cache);
	if (g_cancellable_is_cancelled(cancel)) {
		g_ptr_array_unref(s.items);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
					"Scan cancelled");
		return;
	}
	cache_save(s.items);
	g_task_return_pointer(task, s.items,
			      (GDestroyNotify)g_ptr_array_unref);
}

void library_scan_async(char **folders, GCancellable *cancel,
			GAsyncReadyCallback done, gpointer data)
{
	GTask *task = g_task_new(NULL, cancel, done, data);

	g_task_set_task_data(task, g_strdupv(folders),
			     (GDestroyNotify)g_strfreev);
	g_task_run_in_thread(task, scan_thread);
	g_object_unref(task);
}

GPtrArray *library_scan_finish(GAsyncResult *res, GError **err)
{
	return g_task_propagate_pointer(G_TASK(res), err);
}
