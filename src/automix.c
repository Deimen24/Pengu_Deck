// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * automix.c - hands free playback of the queue
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "analyze.h"
#include "automix.h"
#include "cuestore.h"

#define TICK_MS		100
#define DECKS_USED	2	/* A and B */
#define FADE_MIN	3.0
#define FADE_MAX	45.0
#define OUTRO_MAX	30.0
#define BLEND_BEATS	32.0	/* beat matched blends */
#define INTRO_MAX	32.0	/* an intro up to this long is blended over */
#define CUT_MAX		8.0	/* when tempos cannot be matched */
#define UNMIXABLE	10.0	/* score: tempo out of the pitch range */

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
	bool plan_final;	/* both tracks were fully known when planned */
	gint64 plan_time;
	bool started;		/* the active deck was started by automix */
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

/*
 * While a SoundCloud stream url is being resolved the deck still holds
 * its previous track, so a pending load counts as "nothing there yet".
 */
static bool loaded(int i)
{
	struct track *t = deck_track(deck(i));

	return !am.app->loading[i] && t && track_frames(t) > 0 &&
	       atomic_load(&t->state) != TRACK_FAILED;
}

static bool failed(int i)
{
	struct track *t = deck_track(deck(i));

	return !am.app->loading[i] && t &&
	       atomic_load(&t->state) == TRACK_FAILED;
}

static double item_bpm(PdMediaItem *m)
{
	return m->bpm > 0.0 ? m->bpm : cuestore_bpm(m->key);
}

/*
 * How well @m follows the track on deck @from: 0 is a perfect match.
 * Tempo counts most: the pitch needed to match, as a share of the pitch
 * range (half and double time count as well), out of range is a cut.
 * Key adds a little: same or neighbouring Camelot keys mix cleanly.
 * Unknown values sit in the middle so analysed tracks are preferred.
 */
static double match_score(int from, PdMediaItem *m)
{
	double bpm = deck_bpm(deck(from)), b = item_bpm(m);
	double range = am.app->cfg.pitch_range / 100.0;
	double score = 0.0;
	struct track *t = deck_track(deck(from));
	int key = t ? atomic_load(&t->mkey) : -1;
	int mkey = cuestore_key(m->key);

	if (bpm > 0.0 && b > 0.0) {
		double r = b / bpm;

		if (r > 1.5)
			r /= 2.0;
		else if (r < 0.75)
			r *= 2.0;
		score += fabs(r - 1.0) > range ? UNMIXABLE :
			 fabs(r - 1.0) / range;
	} else {
		score += 1.0;
	}
	if (key >= 0 && mkey >= 0) {
		/* Camelot wheel: same 0, neighbour or relative 1, and on */
		const char *ca = key_camelot(key), *cb = key_camelot(mkey);
		int d = abs(atoi(ca) - atoi(cb));

		if (d > 6)
			d = 12 - d;
		if (ca[strlen(ca) - 1] != cb[strlen(cb) - 1])
			d++;
		score += fmin(d, 3) * 0.25;
	} else {
		score += 0.5;
	}
	return score;
}

/*
 * Take the next track out of the queue: the first one, or with smart
 * order the one that best follows what plays on deck @from.
 */
static PdMediaItem *pop_queue(int from)
{
	GListStore *q = am.app->queue;
	guint n = g_list_model_get_n_items(G_LIST_MODEL(q)), i, best = 0;
	double best_score = 0.0;
	PdMediaItem *m;

	if (n == 0)
		return NULL;
	if (am.app->cfg.automix_smart && from >= 0 && deck_track(deck(from))) {
		for (i = 0; i < n; i++) {
			double s;

			m = g_list_model_get_item(G_LIST_MODEL(q), i);
			s = match_score(from, m);
			g_object_unref(m);
			if (i == 0 || s < best_score - 1e-9) {
				best_score = s;
				best = i;
			}
		}
	}
	m = g_list_model_get_item(G_LIST_MODEL(q), best);
	g_list_store_remove(q, best);
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
	PdMediaItem *m = pop_queue(i ^ 1);

	if (!m)
		return false;
	am.app->automix_loading = TRUE;
	app_load_item(am.app, i, m);
	am.app->automix_loading = FALSE;
	g_object_unref(m);
	return true;
}

/*
 * A deck the user loaded and did not play yet is the next track: it
 * takes precedence over the queue and is never overwritten.
 */
static bool hand_ready(int i)
{
	return am.app->by_hand[i] && loaded(i) &&
	       !atomic_load(&deck(i)->playing) && remaining(i) > 1.0;
}

/* Start deck @i; one the user already started plays on untouched. */
static void start_deck(int i, double at)
{
	struct deck *d = deck(i);

	if (!atomic_load(&d->playing)) {
		deck_seek(d, at >= 0.0 ? at : d->cue);
		deck_play(d, true);
	}
	if (i == am.active)
		am.started = true;
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
	am.plan_time = g_get_monotonic_time();
	am.plan_final = true;
	if (!c->automix_auto || !tf || !tt)
		return;
	am.plan_final = track_done(tf) && track_done(tt) &&
			atomic_load(&tt->analysed);

	match = c->automix_sync && tempo_matchable(from, to);
	bpm = deck_bpm(deck(from));
	outro = analyze_quiet_tail(tf);
	intro = analyze_quiet_head(tt, (size_t)deck(to)->cue);

	/*
	 * Matched tempos: a long blend, the base length or BLEND_BEATS,
	 * whichever is longer.  A quiet outro is covered so the incoming
	 * track carries the end; a quiet intro up to INTRO_MAX is blended
	 * over so its first drop lands as the fade ends, a longer one is
	 * skipped.  Unmatched tempos get a short cut instead.
	 */
	if (match && bpm > 0.0)
		len = fmax(len, BLEND_BEATS * 60.0 / bpm);
	if (outro >= 4.0)
		len = fmax(len, fmin(outro + 2.0, OUTRO_MAX));
	if (intro >= 4.0 && intro <= INTRO_MAX)
		len = fmax(len, intro);
	if (!match)
		len = fmin(len, CUT_MAX);
	len = CLAMP(len, FADE_MIN, FADE_MAX);

	/* Never fade longer than what is left of the outgoing track. */
	len = fmin(len, fmax(FADE_MIN, remaining(from) - 0.5));

	if (deck(to)->cue <= 0.0 && intro > INTRO_MAX)
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
	am.started = true;
	am.app->by_hand[am.next] = FALSE;
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
	int k;

	if (atomic_load(&deck(i)->playing)) {
		am.active = i;
		am.started = true;
		am.state = AM_PLAYING;
		return;
	}
	for (k = 0; k < DECKS_USED; k++) {
		if (!hand_ready(k))
			continue;
		am.active = k;
		am.next = -1;
		am.started = false;
		am.state = AM_PLAYING;
		set_status("Automix: starting deck %c", app_deck_letter(k));
		return;
	}
	if (!load_next(i)) {
		set_status("Automix: queue is empty");
		return;
	}
	am.active = i;
	am.next = -1;
	am.started = false;
	am.state = AM_PLAYING;
	set_status("Automix: starting on deck %c", app_deck_letter(i));
}

static void tick_playing(void)
{
	struct deck *d = deck(am.active);
	int other = am.active ^ 1;

	if (!atomic_load(&d->playing)) {
		if (failed(am.active)) {
			set_status("Automix: track failed, skipping");
			am.state = AM_IDLE;
			app_unload(am.app, am.active);
			return;
		}
		if (!loaded(am.active))
			return;
		if (!am.started && remaining(am.active) > 1.0) {
			/* A freshly loaded first track: start it. */
			start_deck(am.active, -1.0);
		} else if (remaining(am.active) <= 1.0) {
			/* Ended without a next track: go straight on. */
			am.state = AM_IDLE;
		} else {
			/* Paused by hand: wait, do not restart it. */
			set_status("Automix: deck %c paused",
				   app_deck_letter(am.active));
		}
		return;
	}

	set_status("Automix: deck %c, %d s left",
		   app_deck_letter(am.active), (int)remaining(am.active));

	/*
	 * Load the next track as soon as the other deck is free, so it
	 * is decoded and analysed long before the transition is due.
	 */
	if (atomic_load(&deck(other)->playing) || am.app->loading[other])
		return;
	if (!hand_ready(other) && !load_next(other))
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
	/*
	 * Plan as soon as audio is there, then re-plan once a second while
	 * decoding and analysis still change what is known about the tracks.
	 */
	if (!am.planned || (!am.plan_final &&
			    g_get_monotonic_time() - am.plan_time > G_USEC_PER_SEC))
		plan_transition(am.active, am.next);
	set_status("Automix: deck %c, %d s left, %c next, %d s blend",
		   app_deck_letter(am.active), (int)remaining(am.active),
		   app_deck_letter(am.next), (int)am.plan_len);
	if (!atomic_load(&deck(am.active)->playing) && !am.force &&
	    remaining(am.active) > 1.0) {
		/* Paused by hand: hold the transition until play resumes. */
		set_status("Automix: deck %c paused, %c ready",
			   app_deck_letter(am.active),
			   app_deck_letter(am.next));
		return;
	}
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
	if (atomic_load(&deck(other)->playing) || am.app->loading[other])
		return;
	if (!hand_ready(other) && !load_next(other))
		return;
	am.next = other;
	am.state = AM_PRELOADED;
	am.planned = false;
	am.force = true;
}

const char *automix_status(void)
{
	return am.status;
}
