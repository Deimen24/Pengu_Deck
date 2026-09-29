// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * config.c - persistent user settings
 */
#include <string.h>

#include <glib/gstdio.h>

#include "config.h"

#define APP_DIR	"pengu-deck"

static char *config_path(void)
{
	return g_build_filename(g_get_user_config_dir(), APP_DIR,
				"settings.ini", NULL);
}

static char *ensure_dir(const char *base)
{
	char *dir = g_build_filename(base, APP_DIR, NULL);

	g_mkdir_with_parents(dir, 0755);
	return dir;
}

char *config_data_dir(void)
{
	return ensure_dir(g_get_user_data_dir());
}

char *config_cache_dir(void)
{
	return ensure_dir(g_get_user_cache_dir());
}

static int get_int(GKeyFile *kf, const char *grp, const char *key, int def)
{
	GError *err = NULL;
	int v = g_key_file_get_integer(kf, grp, key, &err);

	if (err) {
		g_error_free(err);
		return def;
	}
	return v;
}

static char *get_str(GKeyFile *kf, const char *grp, const char *key)
{
	char *v = g_key_file_get_string(kf, grp, key, NULL);

	if (v && !*v) {
		g_free(v);
		v = NULL;
	}
	return v;
}

static char **default_folders(void)
{
	const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
	char **v = g_new0(char *, 2);

	v[0] = music ? g_strdup(music) :
		       g_build_filename(g_get_home_dir(), "Music", NULL);
	return v;
}

void config_load(struct config *c)
{
	GKeyFile *kf = g_key_file_new();
	char *path = config_path();

	memset(c, 0, sizeof(*c));
	g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);

	c->backend = get_int(kf, "audio", "backend", BACKEND_AUTO);
	c->device = get_str(kf, "audio", "device");
	c->rate = get_int(kf, "audio", "rate", 0);
	c->period = get_int(kf, "audio", "period", 256);
	c->hp_mode = get_int(kf, "audio", "headphones", HP_OFF);
	c->xf_curve = get_int(kf, "audio", "crossfader", XF_DIPLESS);

	c->folders = g_key_file_get_string_list(kf, "library", "folders",
						NULL, NULL);
	if (!c->folders)
		c->folders = default_folders();
	c->record_dir = get_str(kf, "library", "recordings");
	if (!c->record_dir)
		c->record_dir = g_build_filename(c->folders[0] ?
						 c->folders[0] :
						 g_get_home_dir(),
						 "Pengu Deck Mixes", NULL);

	c->sc_client_id = get_str(kf, "soundcloud", "client_id");
	c->sc_token = get_str(kf, "soundcloud", "oauth_token");

	c->ndecks = get_int(kf, "decks", "count", 2);
	c->pitch_range = get_int(kf, "decks", "pitch_range", 8);
	c->keylock = get_int(kf, "decks", "keylock", 0);
	c->quantize = get_int(kf, "decks", "quantize", 1);

	c->automix_fade = CLAMP(get_int(kf, "automix", "fade", 12), 2, 90);
	c->automix_sync = get_int(kf, "automix", "sync", 1);

	if (c->backend < BACKEND_AUTO || c->backend > BACKEND_NULL)
		c->backend = BACKEND_AUTO;
	if (c->hp_mode < HP_OFF || c->hp_mode > HP_CH34)
		c->hp_mode = HP_OFF;
	if (c->pitch_range != 16 && c->pitch_range != 50)
		c->pitch_range = 8;
	c->ndecks = CLAMP(c->ndecks, 2, ENGINE_DECKS);

	g_free(path);
	g_key_file_free(kf);
}

void config_save(const struct config *c)
{
	GKeyFile *kf = g_key_file_new();
	char *path = config_path();
	char *dir = g_path_get_dirname(path);
	GError *err = NULL;

	g_key_file_set_integer(kf, "audio", "backend", c->backend);
	g_key_file_set_string(kf, "audio", "device",
			      c->device ? c->device : "");
	g_key_file_set_integer(kf, "audio", "rate", c->rate);
	g_key_file_set_integer(kf, "audio", "period", c->period);
	g_key_file_set_integer(kf, "audio", "headphones", c->hp_mode);
	g_key_file_set_integer(kf, "audio", "crossfader", c->xf_curve);

	g_key_file_set_string_list(kf, "library", "folders",
				   (const char *const *)c->folders,
				   g_strv_length(c->folders));
	g_key_file_set_string(kf, "library", "recordings", c->record_dir);

	g_key_file_set_string(kf, "soundcloud", "client_id",
			      c->sc_client_id ? c->sc_client_id : "");
	g_key_file_set_string(kf, "soundcloud", "oauth_token",
			      c->sc_token ? c->sc_token : "");

	g_key_file_set_integer(kf, "decks", "count", c->ndecks);
	g_key_file_set_integer(kf, "decks", "pitch_range", c->pitch_range);
	g_key_file_set_integer(kf, "decks", "keylock", c->keylock);
	g_key_file_set_integer(kf, "decks", "quantize", c->quantize);

	g_key_file_set_integer(kf, "automix", "fade", c->automix_fade);
	g_key_file_set_integer(kf, "automix", "sync", c->automix_sync);

	g_mkdir_with_parents(dir, 0700);
	if (!g_key_file_save_to_file(kf, path, &err)) {
		g_warning("Saving settings failed: %s", err->message);
		g_error_free(err);
	} else {
		/* The file may hold an OAuth token. */
		g_chmod(path, 0600);
	}

	g_free(dir);
	g_free(path);
	g_key_file_free(kf);
}

void config_clear(struct config *c)
{
	g_free(c->device);
	g_strfreev(c->folders);
	g_free(c->record_dir);
	g_free(c->sc_client_id);
	g_free(c->sc_token);
	memset(c, 0, sizeof(*c));
}
