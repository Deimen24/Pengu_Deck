// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * automix.c - hands free playback of the queue
 */
#include <math.h>
#include <stdarg.h>

#include "analyze.h"
#include "automix.h"

#define TICK_MS		100
#define PRELOAD_SECS	20.0	/* load the next track this early */
#define DECKS_USED	2	/* A and B */
#define FADE_MIN	3.0
#define FADE_MAX	45.0
#define OUTRO_MAX	30.0
#define BLEND_BEATS	16.0	/* beat matched blends */
#define CUT_MAX		8.0	/* when tempos cannot be matched */

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
	double plan_len;	/* transition length for the next fade */
	double plan_start;	/* where the next track starts, frames */
	bool planned;
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

static void start_deck(int i, double at)
{
	struct deck *d = deck(i);

	deck_seek(d, at >= 0.0 ? at : d->cue);
	deck_play(d, true);
}

/* Pitch the incoming deck would need to match the outgoing one. */
static bool tempo_matchable(int from, int to)
{
	double p = deck_sync_pitch(deck(to), deck(from));
	double range = am.app->cfg.pitch_range / 100.0;

	return !isnan(p) && fabs(p) <= range;
}

/*
 * Size the transition from the two tracks: a beat matched blend of
 * BLEND_BEATS when the tempos fit, stretched to cover a quiet outro of
 * the outgoing track, or a short cut when they do not.  A long quiet
 * intro of the incoming track is skipped so its first drop lands as the
 * outgoing track ends.
 */
static void plan_transition(int from, int to)
{
	struct config *c = &am.app->cfg;
	struct track *tf = deck_track(deck(from)), *tt = deck_track(deck(to));
	double len = c->automix_fade;
	double outro = 0.0, intro = 0.0, bpm = 0.0;
	bool match;

	am.plan_len = len;
	am.plan_start = -1.0;
	am.planned = true;
	if (!c->automix_auto || !tf || !tt)
		return;

	match = c->automix_sync && tempo_matchable(from, to);
	bpm = deck_bpm(deck(from));
	outro = analyze_quiet_tail(tf);
	intro = analyze_quiet_head(tt, (size_t)deck(to)->cue);

	if (match && bpm > 0.0)
		len = fmax(len, BLEND_BEATS * 60.0 / bpm);
	if (outro >= 4.0)
		len = fmax(len, fmin(outro, OUTRO_MAX));
	if (!match)
		len = fmin(len, CUT_MAX);
	len = CLAMP(len, FADE_MIN, FADE_MAX);

	/* Never fade longer than what is left of the outgoing track. */
	len = fmin(len, fmax(FADE_MIN, remaining(from) - 0.5));

	if (deck(to)->cue <= 0.0 && intro > len + 4.0)
		am.plan_start = (intro - len) * tt->rate;
	am.plan_len = len;
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
	if (!am.planned)
		plan_transition(am.active, am.next);
	start_deck(am.next, am.plan_start);
	if (am.app->cfg.automix_sync)
		deck_sync_phase(to, from);

	am.fade_start = g_get_monotonic_time();
	am.fade_from = atomic_load(&am.app->engine.xfader);
	am.fade_to = side_value(am.next);
	am.state = AM_FADING;
	am.planned = false;
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
			start_deck(am.active, -1.0);
		else if (loaded(am.active)) {
			/* Ended without a next track: go straight on. */
			am.state = AM_IDLE;
		}
		return;
	}

	set_status("Automix: deck %c, %d s left",
		   app_deck_letter(am.active), (int)remaining(am.active));

	if (remaining(am.active) > PRELOAD_SECS + (am.app->cfg.automix_auto ?
						   FADE_MAX :
						   am.app->cfg.automix_fade))
		return;
	if (atomic_load(&deck(other)->playing) || am.app->loading[other])
		return;
	if (!load_next(other))
		return;
	am.next = other;
	am.state = AM_PRELOADED;
	am.planned = false;
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
	if (!loaded(am.next)) {
		set_status("Automix: deck %c, %d s left, loading %c",
			   app_deck_letter(am.active),
			   (int)remaining(am.active),
			   app_deck_letter(am.next));
		return;
	}
	/* Plan once both tracks are fully known, re-plan when it changes. */
	if (!am.planned || (am.app->cfg.automix_auto &&
			    track_done(deck_track(deck(am.next))) &&
			    am.plan_len == am.app->cfg.automix_fade))
		plan_transition(am.active, am.next);
	set_status("Automix: deck %c, %d s left, %c next, %d s blend",
		   app_deck_letter(am.active), (int)remaining(am.active),
		   app_deck_letter(am.next), (int)am.plan_len);
	if (am.force || !atomic_load(&deck(am.active)->playing) ||
	    remaining(am.active) <= am.plan_len) {
		am.force = false;
		begin_fade();
	}
}

static void tick_fading(void)
{
	double secs = am.plan_len > 0.0 ? am.plan_len : am.app->cfg.automix_fade;
	double t = (g_get_monotonic_time() - am.fade_start) / 1e6 / secs;
	double x;

	if (t >= 1.0)
		t = 1.0;
	x = am.fade_from + (am.fade_to - am.fade_from) * t;
	atomic_store(&am.app->engine.xfader, (float)x);
	set_status("Automix: mixing %c → %c over %d s",
		   app_deck_letter(am.active), app_deck_letter(am.next),
		   (int)secs);
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
