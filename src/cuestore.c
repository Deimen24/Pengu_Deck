// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * cuestore.c - per track cue points and analysis results
 *
 * A key file with one group per track, named by the SHA-1 of the track
 * key (file paths may contain characters not allowed in group names).
 * Writes are batched and flushed a moment after the last change.
 */
#include <glib.h>

#include "config.h"
#include "cuestore.h"

#define FLUSH_DELAY	2	/* seconds */

static GKeyFile *store;
static char *store_path;
static guint flush_id;

static char *group_for(const char *key)
{
	return g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
}

void cuestore_open(void)
{
	char *dir;

	if (store)
		return;
	dir = config_data_dir();
	store_path = g_build_filename(dir, "tracks.ini", NULL);
	g_free(dir);
	store = g_key_file_new();
	g_key_file_load_from_file(store, store_path, G_KEY_FILE_NONE, NULL);
}

static void flush(void)
{
	GError *err = NULL;

	if (!store)
		return;
	if (!g_key_file_save_to_file(store, store_path, &err)) {
		g_warning("Saving cue points failed: %s", err->message);
		g_error_free(err);
	}
}

static gboolean flush_cb(gpointer data)
{
	flush_id = 0;
	flush();
	return G_SOURCE_REMOVE;
}

void cuestore_close(void)
{
	if (!store)
		return;
	if (flush_id) {
		g_source_remove(flush_id);
		flush_id = 0;
	}
	flush();
	g_key_file_free(store);
	store = NULL;
	g_free(store_path);
	store_path = NULL;
}

static double get_double(const char *grp, const char *key, double def)
{
	GError *err = NULL;
	double v = g_key_file_get_double(store, grp, key, &err);

	if (err) {
		g_error_free(err);
		return def;
	}
	return v;
}

bool cuestore_get(const char *key, struct track_info *info)
{
	char *grp;
	char name[16];
	int i;

	if (!store || !key)
		return false;
	grp = group_for(key);
	if (!g_key_file_has_group(store, grp)) {
		g_free(grp);
		return false;
	}
	info->bpm = get_double(grp, "bpm", 0.0);
	info->beat_offset = get_double(grp, "beat_offset", 0.0);
	info->cue = get_double(grp, "cue", 0.0);
	info->mkey = (int)get_double(grp, "mkey", -1.0);
	info->has_gain = g_key_file_has_key(store, grp, "gain", NULL);
	info->gain_db = get_double(grp, "gain", 0.0);
	for (i = 0; i < DECK_HOTCUES; i++) {
		g_snprintf(name, sizeof(name), "hotcue%d", i + 1);
		info->hotcue[i] = get_double(grp, name, -1.0);
	}
	g_free(grp);
	return true;
}

void cuestore_put(const char *key, const struct track_info *info)
{
	char *grp;
	char name[16];
	int i;

	if (!store || !key)
		return;
	grp = group_for(key);
	g_key_file_set_string(store, grp, "key", key);
	g_key_file_set_double(store, grp, "bpm", info->bpm);
	g_key_file_set_double(store, grp, "beat_offset", info->beat_offset);
	g_key_file_set_double(store, grp, "cue", info->cue);
	if (info->mkey >= 0)
		g_key_file_set_double(store, grp, "mkey", info->mkey);
	if (info->has_gain)
		g_key_file_set_double(store, grp, "gain", info->gain_db);
	for (i = 0; i < DECK_HOTCUES; i++) {
		g_snprintf(name, sizeof(name), "hotcue%d", i + 1);
		if (info->hotcue[i] >= 0.0)
			g_key_file_set_double(store, grp, name,
					      info->hotcue[i]);
		else
			g_key_file_remove_key(store, grp, name, NULL);
	}
	g_free(grp);

	if (!flush_id)
		flush_id = g_timeout_add_seconds(FLUSH_DELAY, flush_cb, NULL);
}

double cuestore_bpm(const char *key)
{
	struct track_info info;

	return cuestore_get(key, &info) ? info.bpm : 0.0;
}

int cuestore_key(const char *key)
{
	struct track_info info;

	return cuestore_get(key, &info) ? info.mkey : -1;
}
