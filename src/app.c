// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * app.c - state shared by all views
 */
#include <stdarg.h>

#define G_LOG_DOMAIN "pengu-deck"

#include "app.h"
#include "cuestore.h"
#include "decoder.h"
#include "library.h"
#include "net.h"
#include "playlists.h"
#include "sccache.h"

static void queue_changed(GListModel *m, guint pos, guint removed,
			  guint added, gpointer data);

static void sc_tokens_changed(gpointer data);

void app_init(struct app *a, GtkApplication *gtk)
{
	int i;

	a->gtk = gtk;
	config_load(&a->cfg);
	engine_init(&a->engine);
	atomic_store(&a->engine.xf_curve, a->cfg.xf_curve);
	for (i = 0; i < ENGINE_DECKS; i++)
		atomic_store(&a->engine.deck[i].keylock, a->cfg.keylock);
	cuestore_open();
	playlists_open();
	net_init();
	app_sc_configure(a);
	{
		struct sc_tokens t = {
			.access = a->cfg.sc_access,
			.refresh = a->cfg.sc_refresh,
			.expires_at = a->cfg.sc_expires,
			.user = a->cfg.sc_user,
		};

		sc_session_set_tokens(&t);
	}
	sc_session_set_changed(sc_tokens_changed, a);
	sc_set_full_only(a->cfg.sc_full_only);
	a->library = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	a->sc_results = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	a->queue = g_list_store_new(PD_TYPE_MEDIA_ITEM);
	a->played = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					  NULL);
	g_signal_connect(a->queue, "items-changed",
			 G_CALLBACK(queue_changed), a);
	sccache_init(a);
}

void app_shutdown(struct app *a)
{
	int i;

	sccache_shutdown();
	for (i = 0; i < ENGINE_DECKS; i++)
		app_save_cues(a, i);
	engine_fini(&a->engine);
	playlists_close();
	cuestore_close();
	config_save(&a->cfg);
	config_clear(&a->cfg);
	g_clear_object(&a->library);
	g_clear_object(&a->sc_results);
	g_clear_object(&a->queue);
	g_clear_pointer(&a->played, g_hash_table_unref);
	g_clear_pointer(&a->preview_key, g_free);
}

/*
 * Queued SoundCloud tracks are downloaded right away.  Starting the
 * download marks the row as changed, and a list must not be changed
 * from inside its own change signal, so the work waits for the next
 * main loop turn.
 */
static gboolean fetch_queued(gpointer data)
{
	PdMediaItem *item = data;

	sccache_fetch(item);
	g_object_unref(item);
	return G_SOURCE_REMOVE;
}

static void queue_changed(GListModel *m, guint pos, guint removed,
			  guint added, gpointer data)
{
	guint i;

	for (i = pos; i < pos + added; i++)
		g_idle_add(fetch_queued, g_list_model_get_item(m, i));
}

void app_item_changed(struct app *a, PdMediaItem *m)
{
	pd_media_item_changed(m->key);
}

bool app_item_played(struct app *a, PdMediaItem *m)
{
	return g_hash_table_contains(a->played, m->key);
}

void app_reset_played(struct app *a)
{
	GList *keys = g_hash_table_get_keys(a->played), *l;

	for (l = keys; l; l = l->next) {
		PdMediaItem probe = { .key = l->data };

		g_hash_table_steal(a->played, l->data);
		app_item_changed(a, &probe);
		g_free(l->data);
	}
	g_list_free(keys);
}

static void mark_played(struct app *a, PdMediaItem *m)
{
	if (g_hash_table_contains(a->played, m->key))
		return;
	g_hash_table_add(a->played, g_strdup(m->key));
	app_item_changed(a, m);
}

gboolean app_open_audio(struct app *a, char **warn)
{
	struct engine_opts o = {
		.backend = a->cfg.backend,
		.device = a->cfg.device,
		.rate = a->cfg.rate,
		.period = a->cfg.period,
		.hp_mode = a->cfg.hp_mode,
		.mic = a->cfg.mic,
		.mic_device = a->cfg.mic_device,
	};
	char *err = NULL;

	atomic_store(&a->engine.talkover_db, (float)a->cfg.talkover_db);
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
	info.mkey = atomic_load(&t->mkey);
	info.has_gain = atomic_load(&t->gain_known);
	info.gain_db = atomic_load(&t->gain_db);
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
	if (info.mkey >= 0)
		atomic_store(&t->mkey, info.mkey);
	if (info.has_gain) {
		atomic_store(&t->gain_db, (float)info.gain_db);
		atomic_store(&t->gain_known, true);
	}
	d->cue = info.cue * t->rate;
	for (i = 0; i < DECK_HOTCUES; i++)
		d->hotcue[i] = info.hotcue[i] >= 0.0 ?
			       info.hotcue[i] * t->rate : -1.0;
	if (d->cue > 0.0)
		deck_seek(d, d->cue);
}

/* ---- loading ----------------------------------------------------- */

#define DECK_PREVIEW	(-1)

static struct deck *deck_for(struct app *a, int idx)
{
	return idx == DECK_PREVIEW ? &a->engine.preview : &a->engine.deck[idx];
}

static void load_uri(struct app *a, int idx, const char *uri,
		     const char *key, const char *title, const char *artist,
		     double tag_bpm)
{
	struct deck *d = deck_for(a, idx);
	unsigned int rate = a->engine.rate ? a->engine.rate : 48000;
	struct track *t;

	if (idx >= 0)
		app_save_cues(a, idx);
	t = track_new(uri, key, rate);
	track_set_meta(t, title, artist);
	deck_load(d, t);
	atomic_store(&d->keylock, a->cfg.keylock && deck_has_keylock());
	atomic_store(&d->quantize, a->cfg.quantize);
	atomic_store(&d->autogain, a->cfg.autogain);
	restore_cues(d, t);
	if (idx == DECK_PREVIEW)
		deck_play(d, true);
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

	if (l->idx == DECK_PREVIEW)
		a->preview_loading = FALSE;
	else
		a->loading[l->idx] = FALSE;
	/* the preview was stopped or moved on while the url resolved */
	if (l->idx == DECK_PREVIEW &&
	    g_strcmp0(a->preview_key, l->item->key) != 0) {
		g_free(l->url);
		l->url = NULL;
		g_clear_error(&l->err);
		l->err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
					     "");
	}
	if (l->url) {
		load_uri(a, l->idx, l->url, l->item->key, l->item->title,
			 l->item->artist, l->item->bpm);
		if (l->item->preview)
			app_toast(a, "Only a 30 second preview of \"%s\" is "
				  "available", l->item->title);
	} else if (!g_error_matches(l->err, G_IO_ERROR,
				    G_IO_ERROR_CANCELLED)) {
		if (l->idx >= 0)
			a->load_failed[l->idx] = TRUE;
		g_debug("load of %s failed: %s", l->item->key,
			l->err ? l->err->message : "no url");
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
	l->url = sc_stream_url(l->item, &l->err);
	g_debug("stream for %s: %s", l->item->key, l->url ? l->url :
		l->err ? l->err->message : "?");
	g_idle_add(sc_load_done, l);
	return NULL;
}

static void load_item(struct app *a, int idx, PdMediaItem *m)
{
	struct sc_load *l;
	char *cached;
	gboolean *busy = idx == DECK_PREVIEW ? &a->preview_loading :
			 &a->loading[idx];

	if (idx >= 0)
		a->load_failed[idx] = FALSE;
	if (m->source == MEDIA_LOCAL) {
		load_uri(a, idx, m->location, m->key, m->title, m->artist,
			 m->bpm);
		return;
	}
	cached = sccache_lookup(m);
	if (cached) {
		load_uri(a, idx, cached, m->key, m->title, m->artist, m->bpm);
		g_free(cached);
		return;
	}
	/* Stream now, and keep a copy for next time. */
	sccache_fetch(m);
	if (*busy)
		return;
	*busy = TRUE;
	if (idx != DECK_PREVIEW)
		app_toast(a, "Loading \"%s\" from SoundCloud…", m->title);
	l = g_new0(struct sc_load, 1);
	l->app = a;
	l->idx = idx;
	l->item = g_object_ref(m);
	g_thread_unref(g_thread_new("pd-sc-load", sc_load_thread, l));
}

void app_load_item(struct app *a, int idx, PdMediaItem *m)
{
	if (idx >= 0)
		a->by_hand[idx] = !a->automix_loading;
	mark_played(a, m);
	history_add(m);
	load_item(a, idx, m);
}

void app_preview(struct app *a, PdMediaItem *m)
{
	PdMediaItem probe = { .key = a->preview_key };

	if (app_previewing(a, m)) {
		app_preview_stop(a);
		return;
	}
	if (a->preview_loading)
		return;		/* one stream url at a time */
	if (a->preview_key)
		app_item_changed(a, &probe);
	g_free(a->preview_key);
	a->preview_key = g_strdup(m->key);
	load_item(a, DECK_PREVIEW, m);
	app_item_changed(a, m);
}

void app_preview_stop(struct app *a)
{
	PdMediaItem probe = { .key = a->preview_key };

	deck_play(&a->engine.preview, false);
	if (!a->preview_key)
		return;
	/* notify while the key is still alive, the probe aliases it */
	app_item_changed(a, &probe);
	g_clear_pointer(&a->preview_key, g_free);
}

bool app_previewing(struct app *a, PdMediaItem *m)
{
	return a->preview_key && g_str_equal(a->preview_key, m->key) &&
	       atomic_load(&a->engine.preview.playing);
}

void app_unload(struct app *a, int idx)
{
	struct deck *d = &a->engine.deck[idx];
	struct track *old;

	app_save_cues(a, idx);
	atomic_store(&d->playing, false);
	old = atomic_exchange(&d->track, NULL);
	while (atomic_load(&d->in_use))
		g_thread_yield();
	if (old) {
		atomic_store(&old->cancel, true);
		track_unref(old);
	}
}

/* ---- misc -------------------------------------------------------- */

int app_sync_master(struct app *a, int idx)
{
	int i;

	for (i = 0; i < a->cfg.ndecks; i++)
		if (i != idx && atomic_load(&a->engine.deck[i].playing) &&
		    deck_bpm(&a->engine.deck[i]) > 0.0)
			return i;
	for (i = 0; i < a->cfg.ndecks; i++)
		if (i != idx && deck_bpm(&a->engine.deck[i]) > 0.0)
			return i;
	return idx ^ 1;
}

void app_set_deck_count(struct app *a, int n)
{
	a->cfg.ndecks = CLAMP(n, 2, ENGINE_DECKS);
	config_save(&a->cfg);
}

static const char *const deck_colors[ENGINE_DECKS] = {
	"#4fc3f7", "#ff8a65", "#7bd88f", "#c792ea",
};
static const char *const deck_classes[ENGINE_DECKS] = {
	"deck-a", "deck-b", "deck-c", "deck-d",
};

const char *app_deck_color(int idx)
{
	return deck_colors[CLAMP(idx, 0, ENGINE_DECKS - 1)];
}

const char *app_deck_class(int idx)
{
	return deck_classes[CLAMP(idx, 0, ENGINE_DECKS - 1)];
}

char app_deck_letter(int idx)
{
	return (char)('A' + CLAMP(idx, 0, ENGINE_DECKS - 1));
}

/* The client refreshed or dropped its tokens: keep the config in step. */
static void sc_tokens_changed(gpointer data)
{
	struct app *a = data;
	struct sc_tokens t;

	sc_session_get_tokens(&t);
	g_free(a->cfg.sc_access);
	g_free(a->cfg.sc_refresh);
	a->cfg.sc_access = t.access;
	a->cfg.sc_refresh = t.refresh;
	a->cfg.sc_expires = t.expires_at;
	a->cfg.sc_user = t.user;
	config_save(&a->cfg);
}

void app_sc_configure(struct app *a)
{
	sc_session_set_app(a->cfg.sc_client_id, a->cfg.sc_client_secret);
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
