/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * config.h - persistent user settings
 *
 * Stored as a key file in $XDG_CONFIG_HOME/pengu-deck/settings.ini.
 */
#ifndef PD_CONFIG_H
#define PD_CONFIG_H

#include <glib.h>

#include "engine.h"

struct config {
	/* audio */
	enum audio_backend backend;
	char *device;
	unsigned int rate;
	unsigned int period;
	enum hp_mode hp_mode;
	int xf_curve;

	/* library */
	char **folders;
	char *record_dir;

	/* soundcloud */
	char *sc_client_id;
	char *sc_token;

	/* decks */
	int ndecks;		/* 2 to 4 shown */
	int pitch_range;	/* percent: 8, 16 or 50 */
	gboolean keylock;
	gboolean quantize;
	gboolean autogain;

	/* automix */
	int automix_fade;	/* seconds */
	gboolean automix_sync;

	/* sampler */
	char *samples[8];

	/* recording and broadcasting */
	int rec_format;		/* enum enc_format */
	int rec_bitrate;
	char *ice_host;
	int ice_port;
	char *ice_mount;
	char *ice_user;
	char *ice_password;
	char *ice_name;
	int ice_format;		/* ENC_MP3 or ENC_OPUS */
	int ice_bitrate;

	/* microphone */
	gboolean mic;
	char *mic_device;
	int talkover_db;
};

void config_load(struct config *c);
void config_save(const struct config *c);
void config_clear(struct config *c);

/* $XDG_DATA_HOME/pengu-deck, created on demand. */
char *config_data_dir(void);
char *config_cache_dir(void);

#endif /* PD_CONFIG_H */
