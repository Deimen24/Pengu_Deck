// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * app.c - state shared by all views
 */
#include <stdarg.h>

#include "app.h"
#include "cuestore.h"
#include "decoder.h"
#include "library.h"
#include "net.h"

void app_init(struct app *a, GtkApplication *gtk)
{
	a->gtk = gtk;
	config_load(&a->cfg);
	engine_init(&a->engine);
	atomic_store(&a->engine.xf_curve, a->cfg.xf_curve);
	atomic_store(&a->engine.deck[0].keylock, a->cfg.keylock);
	atomic_store(&a->engine.deck[1].keylock, a->cfg.keylock);
	cuestore_open();
	net_init();
	a->library = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	a->sc_results = g_list_store_new(PD_TYPE_MEDIA_ITEM);
}

void app_shutdown(struct app *a)
{
	app_save_cues(a, 0);
	app_save_cues(a, 1);
	engine_fini(&a->engine);
	cuestore_close();
	config_save(&a->cfg);
	config_clear(&a->cfg);
	g_clear_object(&a->library);
	g_clear_object(&a->sc_results);
}

gboolean app_open_audio(struct app *a, char **warn)
{
	struct engine_opts o = {
		.backend = a->cfg.backend,
		.device = a->cfg.device,
		.rate = a->cfg.rate,
		.period = a->cfg.period,
		.hp_mode = a->cfg.hp_mode,
	};
	char *err = NULL;

	*warn = NULL;
	if (engine_open(&a->engine, &o, &err) == 0) {
		*warn = err;
		return TRUE;
	}
	/* Fall back to the null device so the UI keeps working. */
	*warn = err;
	o.backend = BACKEND_NULL;
	o.device = NULL;
	o.rate = 48000;
	engine_open(&a->engine, &o, &err);
	g_free(err);
	return FALSE;
}

/* ---- cue persistence --------------------------------------------- */

void app_save_cues(struct app *a, int idx)
{
	struct deck *d = &a->engine.deck[idx];
	struct track *t = deck_track(d);
	struct track_info info;
	int i;

	if (!t || atomic_load(&t->state) == TRACK_FAILED)
		return;
	info.bpm = atomic_load(&t->bpm);
	info.beat_offset = atomic_load(&t->beat_offset) / t->rate;
	info.cue = d->cue / t->rate;
	for (i = 0; i < DECK_HOTCUES; i++)
		info.hotcue[i] = d->hotcue[i] >= 0.0 ?
				 d->hotcue[i] / t->rate : -1.0;
	cuestore_put(t->key, &info);
}

static void restore_cues(struct deck *d, struct track *t)
{
	struct track_info info;
	int i;

	if (!cuestore_get(t->key, &info))
		return;
	if (info.bpm > 0.0) {
		atomic_store(&t->bpm, info.bpm);
		atomic_store(&t->beat_offset, info.beat_offset * t->rate);
		atomic_store(&t->analysed, true);
	}
	d->cue = info.cue * t->rate;
	for (i = 0; i < DECK_HOTCUES; i++)
		d->hotcue[i] = info.hotcue[i] >= 0.0 ?
			       info.hotcue[i] * t->rate : -1.0;
	if (d->cue > 0.0)
		deck_seek(d, d->cue);
}

/* ---- loading ----------------------------------------------------- */

static void load_uri(struct app *a, int idx, const char *uri,
		     const char *key, const char *title, const char *artist,
		     double tag_bpm)
{
	struct deck *d = &a->engine.deck[idx];
	unsigned int rate = a->engine.rate ? a->engine.rate : 48000;
	struct track *t;

	app_save_cues(a, idx);
	t = track_new(uri, key, rate);
	track_set_meta(t, title, artist);
	deck_load(d, t);
	atomic_store(&d->keylock, a->cfg.keylock && deck_has_keylock());
	restore_cues(d, t);
	if (!atomic_load(&t->analysed) && tag_bpm > 0.0) {
		/* Keep the tag tempo, still analyse to find the grid. */
		atomic_store(&t->bpm, tag_bpm);
	}
	decoder_start(t);
	track_unref(t);
}

void app_load_path(struct app *a, int idx, const char *path)
{
	PdMediaItem *m = library_probe_file(path);

	app_load_item(a, idx, m);
	g_object_unref(m);
}

struct sc_load {
	struct app *app;
	int idx;
	PdMediaItem *item;
	char *url;
	GError *err;
};

static gboolean sc_load_done(gpointer data)
{
	struct sc_load *l = data;
	struct app *a = l->app;

	a->loading[l->idx] = FALSE;
	if (l->url) {
		load_uri(a, l->idx, l->url, l->item->key, l->item->title,
			 l->item->artist, l->item->bpm);
		if (l->item->preview)
			app_toast(a, "Only a 30 second preview of \"%s\" is "
				  "available", l->item->title);
	} else {
		app_toast(a, "%s", l->err ? l->err->message :
			  "Could not load the SoundCloud track");
	}
	g_clear_error(&l->err);
	g_free(l->url);
	g_object_unref(l->item);
	g_free(l);
	return G_SOURCE_REMOVE;
}

static gpointer sc_load_thread(gpointer data)
{
	struct sc_load *l = data;
	struct sc_auth *auth = app_sc_auth(l->app);

	l->url = sc_stream_url(auth, l->item, &l->err);
	sc_auth_free(auth);
	g_idle_add(sc_load_done, l);
	return NULL;
}

void app_load_item(struct app *a, int idx, PdMediaItem *m)
{
	struct sc_load *l;

	if (m->source == MEDIA_LOCAL) {
		load_uri(a, idx, m->location, m->key, m->title, m->artist,
			 m->bpm);
		return;
	}
	if (a->loading[idx])
		return;
	a->loading[idx] = TRUE;
	app_toast(a, "Loading \"%s\" from SoundCloud…", m->title);
	l = g_new0(struct sc_load, 1);
	l->app = a;
	l->idx = idx;
	l->item = g_object_ref(m);
	g_thread_unref(g_thread_new("pd-sc-load", sc_load_thread, l));
}

/* ---- misc -------------------------------------------------------- */

struct sc_auth *app_sc_auth(struct app *a)
{
	struct sc_auth *auth = g_new0(struct sc_auth, 1);

	auth->client_id = g_strdup(a->cfg.sc_client_id);
	auth->token = g_strdup(a->cfg.sc_token);
	return auth;
}

void sc_auth_free(struct sc_auth *auth)
{
	if (!auth)
		return;
	g_free(auth->client_id);
	g_free(auth->token);
	g_free(auth);
}

void app_toast(struct app *a, const char *fmt, ...)
{
	va_list ap;
	char *msg;

	va_start(ap, fmt);
	msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	if (a->toast)
		a->toast(a->toast_data, msg);
	else
		g_message("%s", msg);
	g_free(msg);
}
