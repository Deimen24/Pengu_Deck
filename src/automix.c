// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * automix.c - hands free playback of the queue
 */
#include <math.h>
#include <stdarg.h>

#include "automix.h"

#define TICK_MS		100
#define PRELOAD_SECS	20.0	/* load the next track this early */
#define DECKS_USED	2	/* A and B */

enum state {
	AM_IDLE,		/* nothing playing, waiting for the queue */
	AM_PLAYING,		/* one deck plays, next not loaded yet */
	AM_PRELOADED,		/* next track sits in the other deck */
	AM_FADING,		/* crossfader on its way */
};

static struct {
	struct app *app;
	guint timer;
	bool on;
	enum state state;
	int active;		/* deck currently carrying the mix */
	int next;		/* deck holding the upcoming track */
	gint64 fade_start;
	double fade_from;
	double fade_to;
	bool force;		/* automix_next(): fade as soon as loaded */
	char status[160];
} am;

static struct deck *deck(int i)
{
	return &am.app->engine.deck[i];
}

static double remaining(int i)
{
	struct track *t = deck_track(deck(i));
	double len;

	if (!t)
		return 0.0;
	len = (double)track_length(t);
	return (len - deck_position(deck(i))) / t->rate;
}

static bool loaded(int i)
{
	struct track *t = deck_track(deck(i));

	return t && track_frames(t) > 0 &&
	       atomic_load(&t->state) != TRACK_FAILED;
}

static bool failed(int i)
{
	struct track *t = deck_track(deck(i));

	return t && atomic_load(&t->state) == TRACK_FAILED;
}

static PdMediaItem *pop_queue(void)
{
	GListStore *q = am.app->queue;
	PdMediaItem *m = g_list_model_get_item(G_LIST_MODEL(q), 0);

	if (m)
		g_list_store_remove(q, 0);
	return m;
}

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	g_vsnprintf(am.status, sizeof(am.status), fmt, ap);
	va_end(ap);
}

static double side_value(int i)
{
	switch (atomic_load(&deck(i)->xf_side)) {
	case XF_LEFT:
		return -1.0;
	case XF_RIGHT:
		return 1.0;
	default:
		return atomic_load(&am.app->engine.xfader);
	}
}

/* Load the head of the queue into deck @i, false when the queue is empty. */
static bool load_next(int i)
{
	PdMediaItem *m = pop_queue();

	if (!m)
		return false;
	app_load_item(am.app, i, m);
	g_object_unref(m);
	return true;
}

static void start_deck(int i)
{
	struct deck *d = deck(i);

	deck_seek(d, d->cue);
	deck_play(d, true);
}

static void begin_fade(void)
{
	struct deck *from = deck(am.active), *to = deck(am.next);
	double range = am.app->cfg.pitch_range / 100.0;

	if (am.app->cfg.automix_sync) {
		double p = deck_sync_pitch(to, from);

		if (!isnan(p) && fabs(p) <= range)
			atomic_store(&to->pitch, (float)p);
	}
	start_deck(am.next);
	if (am.app->cfg.automix_sync)
		deck_sync_phase(to, from);

	am.fade_start = g_get_monotonic_time();
	am.fade_from = atomic_load(&am.app->engine.xfader);
	am.fade_to = side_value(am.next);
	am.state = AM_FADING;
}

static void end_fade(void)
{
	deck_play(deck(am.active), false);
	am.active = am.next;
	am.next = -1;
	am.state = AM_PLAYING;
}

/* Pick the deck to start on: a playing A/B deck, else an empty one. */
static int pick_start_deck(void)
{
	int i;

	for (i = 0; i < DECKS_USED; i++)
		if (atomic_load(&deck(i)->playing))
			return i;
	for (i = 0; i < DECKS_USED; i++)
		if (!deck_track(deck(i)))
			return i;
	return 0;
}

static void tick_idle(void)
{
	int i = pick_start_deck();

	if (atomic_load(&deck(i)->playing)) {
		am.active = i;
		am.state = AM_PLAYING;
		return;
	}
	if (!load_next(i)) {
		set_status("Automix: queue is empty");
		return;
	}
	am.active = i;
	am.next = -1;
	am.state = AM_PLAYING;
	set_status("Automix: starting on deck %c", app_deck_letter(i));
}

static void tick_playing(void)
{
	struct deck *d = deck(am.active);
	int other = am.active ^ 1;

	/* A freshly loaded first track: start it once audio is there. */
	if (!atomic_load(&d->playing)) {
		if (failed(am.active)) {
			set_status("Automix: track failed, skipping");
			am.state = AM_IDLE;
			app_unload(am.app, am.active);
			return;
		}
		if (loaded(am.active) && remaining(am.active) > 1.0)
			start_deck(am.active);
		else if (loaded(am.active)) {
			/* Ended without a next track: go straight on. */
			am.state = AM_IDLE;
		}
		return;
	}

	set_status("Automix: deck %c, %d s left",
		   app_deck_letter(am.active), (int)remaining(am.active));

	if (remaining(am.active) > PRELOAD_SECS + am.app->cfg.automix_fade)
		return;
	if (atomic_load(&deck(other)->playing) || am.app->loading[other])
		return;
	if (!load_next(other))
		return;
	am.next = other;
	am.state = AM_PRELOADED;
}

static void tick_preloaded(void)
{
	if (failed(am.next)) {
		set_status("Automix: next track failed, skipping");
		app_unload(am.app, am.next);
		am.next = -1;
		am.state = AM_PLAYING;
		return;
	}
	set_status("Automix: deck %c, %d s left, %c is next",
		   app_deck_letter(am.active), (int)remaining(am.active),
		   app_deck_letter(am.next));
	if (!loaded(am.next))
		return;
	if (am.force || !atomic_load(&deck(am.active)->playing) ||
	    remaining(am.active) <= am.app->cfg.automix_fade) {
		am.force = false;
		begin_fade();
	}
}

static void tick_fading(void)
{
	double secs = am.app->cfg.automix_fade;
	double t = (g_get_monotonic_time() - am.fade_start) / 1e6 / secs;
	double x;

	if (t >= 1.0)
		t = 1.0;
	x = am.fade_from + (am.fade_to - am.fade_from) * t;
	atomic_store(&am.app->engine.xfader, (float)x);
	set_status("Automix: mixing %c → %c", app_deck_letter(am.active),
		   app_deck_letter(am.next));
	if (t >= 1.0)
		end_fade();
}

static gboolean tick(gpointer data)
{
	switch (am.state) {
	case AM_IDLE:
		tick_idle();
		break;
	case AM_PLAYING:
		tick_playing();
		break;
	case AM_PRELOADED:
		tick_preloaded();
		break;
	case AM_FADING:
		tick_fading();
		break;
	}
	return G_SOURCE_CONTINUE;
}

void automix_init(struct app *a)
{
	am.app = a;
	am.next = -1;
	set_status("Automix is off");
}

void automix_shutdown(void)
{
	automix_set_enabled(false);
}

void automix_set_enabled(bool on)
{
	if (on == am.on)
		return;
	am.on = on;
	if (on) {
		am.state = AM_IDLE;
		am.next = -1;
		am.timer = g_timeout_add(TICK_MS, tick, NULL);
		set_status("Automix: waiting");
	} else {
		if (am.timer)
			g_source_remove(am.timer);
		am.timer = 0;
		set_status("Automix is off");
	}
}

bool automix_enabled(void)
{
	return am.on;
}

bool automix_fading(void)
{
	return am.on && am.state == AM_FADING;
}

void automix_next(void)
{
	int other;

	if (!am.on)
		return;
	if (am.state == AM_PRELOADED) {
		if (loaded(am.next))
			begin_fade();
		return;
	}
	if (am.state != AM_PLAYING)
		return;
	other = am.active ^ 1;
	if (atomic_load(&deck(other)->playing) || !load_next(other))
		return;
	am.next = other;
	am.state = AM_PRELOADED;
	am.force = true;
}

const char *automix_status(void)
{
	return am.status;
}
