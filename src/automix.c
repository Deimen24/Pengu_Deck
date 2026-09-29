// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * automix.c - hands free playback of the queue
 */
#define G_LOG_DOMAIN "pengu-deck"

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
#define PREP_SECS	8.0	/* outgoing deck glides to the meeting tempo */
#define RETURN_SECS	30.0	/* incoming deck glides back to 0 % after */
#define LOCK_MAX	0.02	/* largest nudge the beat lock applies */
#define LOCK_SECS	1.5	/* time the beat lock takes to close a gap */
#define LOCK_INT	0.4	/* per second: integral gain against steady drift */
#define LOCK_DEAD	0.001	/* beats: closer than this counts as locked */
#define LOW_CUT		-20.0	/* dB the lows are taken down to in a blend */
#define MID_DIP		-8.0	/* dB the outgoing mids give way by the end */
#define MID_IN		-6.0	/* dB the incoming mids start below */
#define LPF_SWEEP	-0.6	/* outgoing filter knob at the end of the blend */
#define BAR_BEATS	4.0
#define PHRASE_BEATS	16.0	/* blends start on a phrase of the outgoing track */
#define BAR_TOL		0.08	/* bars: close enough to a downbeat to go */

enum state {
	AM_IDLE,		/* nothing playing, waiting for the queue */
	AM_PLAYING,		/* one deck plays, next not loaded yet */
	AM_PRELOADED,		/* next track sits in the other deck */
	AM_FADING,		/* crossfader on its way */
};

/* A pitch fader moved by automix over time; the user moving it wins. */
struct glide {
	bool on;
	int deck;
	_Atomic float *target;	/* pitch or trim of that deck */
	gint64 start;
	double secs;
	double from, to;
	double last;		/* value last written, to spot the user */
};

static struct {
	struct app *app;
	guint timer;
	bool on;
	bool match;		/* the tempos of the planned pair will meet */
	double meet_from;	/* pitch for the outgoing deck at the blend */
	double meet_to;		/* pitch for the incoming deck at the blend */
	struct glide prep;	/* outgoing deck towards meet_from */
	struct glide ret;	/* new active deck back to 0 % */
	struct glide trim;	/* incoming trim back to what it was */
	double shape;		/* fade curve power: >1 brings the new one in slowly */
	double lock_int;	/* beat lock: accumulated drift, in beats */
	double lock_err;	/* last phase gap seen, for the log */
	int snaps;		/* exact corrections done at the blend start */
	gint64 lock_time;
	double level_db;	/* incoming level relative to outgoing at the blend */
	float trim_to;		/* incoming trim before the blend */
	bool prepped;
	gint64 due_at;		/* the blend is due, waiting for a downbeat */
	double tempo_ratio;	/* outgoing over incoming tempo at the blend */
	float eq_from[EQ_BANDS];	/* EQ and filter before the blend, */
	float eq_to[EQ_BANDS];		/* restored after */
	float filt_from;
	int xf_curve;			/* the user's crossfader curve */
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

/*
 * Where the music of deck @i ends, in frames: trailing silence is not
 * part of the track as far as automix is concerned, so a blend ends
 * there and never leaves a gap.
 */
static double audible_end(int i)
{
	struct track *t = deck_track(deck(i));
	double len;

	if (!t)
		return 0.0;
	len = (double)track_length(t);
	if (track_done(t))
		len -= analyze_silence_tail(t) * t->rate;
	return fmax(len, 0.0);
}

static double remaining(int i)
{
	struct track *t = deck_track(deck(i));

	if (!t)
		return 0.0;
	return (audible_end(i) - deck_position(deck(i))) / t->rate;
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

/*
 * The load came to nothing: the decoder gave up, the stream url could
 * not be had (the deck then keeps whatever it held, or nothing), or a
 * track was expected but the deck is empty.
 */
static bool failed(int i)
{
	struct track *t = deck_track(deck(i));

	if (am.app->loading[i])
		return false;
	if (am.app->load_failed[i])
		return true;
	return t && atomic_load(&t->state) == TRACK_FAILED;
}

static bool next_failed(void)
{
	return failed(am.next) ||
	       (!am.app->loading[am.next] && !deck_track(deck(am.next)));
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
		/* both decks move, so twice the range can be bridged */
		score += fabs(r - 1.0) > 2.0 * range ? UNMIXABLE :
			 fabs(r - 1.0) / range;
	} else {
		score += 1.0;
	}
	if (t && t->key && m->key && g_str_equal(t->key, m->key))
		score += UNMIXABLE / 2.0;	/* the same track again */
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
	char old[sizeof(am.status)];
	va_list ap;

	g_strlcpy(old, am.status, sizeof(old));
	va_start(ap, fmt);
	g_vsnprintf(am.status, sizeof(am.status), fmt, ap);
	va_end(ap);
	if (strcmp(old, am.status) != 0)
		g_debug("%s", am.status);
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

static void glide_to(struct glide *g, int i, _Atomic float *target,
		     double to, double secs, const char *what)
{
	g->on = true;
	g->deck = i;
	g->target = target;
	g->start = g_get_monotonic_time();
	g->secs = fmax(secs, 0.1);
	g->from = atomic_load(target);
	g->to = to;
	g->last = g->from;
	g_debug("automix: deck %c %s %.2f -> %.2f over %.0f s",
		app_deck_letter(i), what, g->from, to, g->secs);
}

static void glide_start(struct glide *g, int i, double to, double secs)
{
	glide_to(g, i, &deck(i)->pitch, to, secs, "pitch");
}

static void glide_tick(struct glide *g)
{
	double now, t, p;

	if (!g->on)
		return;
	now = atomic_load(g->target);
	if (fabs(now - g->last) > 1e-4) {
		/* the user took the fader: leave it alone */
		g->on = false;
		return;
	}
	t = (g_get_monotonic_time() - g->start) / 1e6 / g->secs;
	if (t >= 1.0) {
		t = 1.0;
		g->on = false;
	}
	p = g->from + (g->to - g->from) * t;
	atomic_store(g->target, (float)p);
	g->last = (float)p;
}

/*
 * Where deck @to should start so that its beats and bars fall on deck
 * @from's right now: the point on @to's grid nearest to @at with the
 * same position inside the bar as @from has.  Seeking once before the
 * start beats seeking a deck that already plays.
 */
static double aligned_start(int to, int from, double at)
{
	struct track *t = deck_track(deck(to)), *mt = deck_track(deck(from));
	double b, mb, off, moff, mbeats, beats, want;

	if (!t || !mt)
		return at;
	b = track_beat_len(t);
	mb = track_beat_len(mt);
	if (b <= 0.0 || mb <= 0.0)
		return at;
	off = atomic_load(&t->beat_offset);
	moff = atomic_load(&mt->beat_offset);
	mbeats = (deck_position(deck(from)) - moff) / mb;
	beats = (at - off) / b;
	/* same fraction of the bar, on the bar of @to nearest to @at */
	want = floor(beats / BAR_BEATS) * BAR_BEATS + fmod(mbeats, BAR_BEATS);
	if (want < 0.0)
		want += BAR_BEATS;
	if (want - beats > BAR_BEATS / 2.0)
		want -= BAR_BEATS;
	else if (beats - want > BAR_BEATS / 2.0)
		want += BAR_BEATS;
	return fmax(off + want * b, 0.0);
}

/*
 * Beat lock: while the blend runs, nudge the incoming deck so its beats
 * stay on the outgoing deck's.  Proportional on the phase gap, closing
 * it within LOCK_SECS, plus an integral part that cancels a steady
 * drift from a beat grid that is a hair off.
 */
static void beat_lock(int from, int to)
{
	struct deck *m = deck(from), *d = deck(to);
	struct track *mt = deck_track(m), *t = deck_track(d);
	double b, mb, phase, mphase, e, bend, bpm, dt;
	gint64 now = g_get_monotonic_time();

	if (!mt || !t || !atomic_load(&m->playing) || !atomic_load(&d->playing))
		goto off;
	b = track_beat_len(t);
	mb = track_beat_len(mt);
	bpm = deck_bpm(d);
	if (b <= 0.0 || mb <= 0.0 || bpm <= 0.0)
		goto off;
	mphase = (deck_position(m) - atomic_load(&mt->beat_offset)) / mb;
	phase = (deck_position(d) - atomic_load(&t->beat_offset)) / b;
	e = (mphase - floor(mphase)) - (phase - floor(phase));
	if (e > 0.5)
		e -= 1.0;
	else if (e < -0.5)
		e += 1.0;
	dt = am.lock_time ? (now - am.lock_time) / 1e6 : 0.0;
	am.lock_time = now;
	am.lock_err = e;
	/*
	 * Right after the start the incoming deck is still inaudible: put
	 * it exactly on the beat with a seek, which the nudge could only
	 * approach.  Later on only the nudge is used, a seek would be
	 * heard.
	 */
	if (am.snaps < 3 && fabs(e) > 0.002 &&
	    (now - am.fade_start) / 1e6 < 1.0) {
		deck_seek(d, deck_position(d) + e * b);
		am.snaps++;
		am.lock_int = 0.0;
		g_debug("automix: snapped %c by %.4f beats", app_deck_letter(to),
			e);
		goto off;
	}
	am.lock_int = CLAMP(am.lock_int + e * dt * LOCK_INT, -0.05, 0.05);
	if (fabs(e) < LOCK_DEAD && fabs(am.lock_int) < LOCK_DEAD)
		goto off;
	bend = (e / LOCK_SECS + am.lock_int) * 60.0 / bpm;
	atomic_store(&d->bend, (float)CLAMP(bend, -LOCK_MAX, LOCK_MAX));
	return;
off:
	atomic_store(&d->bend, 0.0f);
}

/* Position of deck @i inside its bar, 0..1, or -1 without a beat grid. */
static double bar_phase(int i)
{
	struct track *t = deck_track(deck(i));
	double b, beats;

	if (!t)
		return -1.0;
	b = track_beat_len(t);
	if (b <= 0.0)
		return -1.0;
	beats = (deck_position(deck(i)) - atomic_load(&t->beat_offset)) / b;
	beats /= BAR_BEATS;
	return beats - floor(beats);
}

/*
 * Shift deck @d by whole beats so its downbeats fall on @m's: the beat
 * phase is matched by deck_sync_phase(), this matches the bar.
 */
static void bar_align(int d, int m)
{
	struct track *t = deck_track(deck(d)), *mt = deck_track(deck(m));
	double b, mb, beat, mbeat, delta;

	if (!t || !mt)
		return;
	b = track_beat_len(t);
	mb = track_beat_len(mt);
	if (b <= 0.0 || mb <= 0.0)
		return;
	beat = floor((deck_position(deck(d)) - atomic_load(&t->beat_offset)) /
		     b);
	mbeat = floor((deck_position(deck(m)) -
		       atomic_load(&mt->beat_offset)) / mb);
	delta = fmod(mbeat - beat, BAR_BEATS);
	if (delta < 0.0)
		delta += BAR_BEATS;
	if (delta > BAR_BEATS / 2.0)
		delta -= BAR_BEATS;
	if (delta != 0.0)
		deck_seek(deck(d), deck_position(deck(d)) + delta * b);
}

/* Smooth step: the crossfader eases out of one track and into the other. */
static double ease(double t)
{
	t = CLAMP(t, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

static double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

/*
 * The frequency hand over.  Lows are swapped: the outgoing lows leave
 * over the first 60 %, the incoming lows arrive over the last 60 %, so
 * two bass lines never fight.  Mids cross more gently: the incoming
 * starts a little under and rises early, the outgoing dips late.  The
 * highs are left to the crossfader, they carry the new track's
 * character from the first bar.  A low pass sweep takes the outgoing
 * track's edge off in the last stretch.  Everything returns after.
 */
static void blend_eq(double t)
{
	struct deck *from = deck(am.active), *to = deck(am.next);

	atomic_store(&from->eq_db[EQ_LOW],
		     (float)lerp(am.eq_from[EQ_LOW], LOW_CUT, ease(t / 0.6)));
	atomic_store(&from->eq_db[EQ_MID],
		     (float)lerp(am.eq_from[EQ_MID], am.eq_from[EQ_MID] + MID_DIP,
				 ease((t - 0.4) / 0.6)));
	if (fabs(am.filt_from) < 0.05)
		atomic_store(&from->filter,
			     (float)lerp(0.0, LPF_SWEEP, ease((t - 0.6) / 0.4)));
	atomic_store(&to->eq_db[EQ_LOW],
		     (float)lerp(LOW_CUT, am.eq_to[EQ_LOW], ease((t - 0.4) / 0.6)));
	atomic_store(&to->eq_db[EQ_MID],
		     (float)lerp(am.eq_to[EQ_MID] + MID_IN, am.eq_to[EQ_MID],
				 ease(t / 0.6)));
}

static void blend_save(void)
{
	struct deck *from = deck(am.active), *to = deck(am.next);
	int b;

	for (b = 0; b < EQ_BANDS; b++) {
		am.eq_from[b] = atomic_load(&from->eq_db[b]);
		am.eq_to[b] = atomic_load(&to->eq_db[b]);
	}
	am.filt_from = atomic_load(&from->filter);
	am.xf_curve = atomic_load(&am.app->engine.xf_curve);
	/* equal power while automix fades: no bump in the middle */
	atomic_store(&am.app->engine.xf_curve, XF_POWER);
}

static void blend_restore(void)
{
	struct deck *from = deck(am.active), *to = deck(am.next);
	int b;

	for (b = 0; b < EQ_BANDS; b++) {
		atomic_store(&from->eq_db[b], am.eq_from[b]);
		atomic_store(&to->eq_db[b], am.eq_to[b]);
	}
	atomic_store(&from->filter, am.filt_from);
	atomic_store(&am.app->engine.xf_curve, am.xf_curve);
	atomic_store(&to->bend, 0.0f);
}

/* Snap @frame of @t to its nearest bar, or phrase when one is that close. */
static double snap_grid(struct track *t, double frame)
{
	double b = track_beat_len(t), off, beats, bar, phrase;

	if (b <= 0.0)
		return frame;
	off = atomic_load(&t->beat_offset);
	beats = (frame - off) / b;
	bar = floor(beats / BAR_BEATS + 0.5) * BAR_BEATS;
	phrase = floor(beats / PHRASE_BEATS + 0.5) * PHRASE_BEATS;
	if (fabs(phrase - beats) <= BAR_BEATS)
		bar = phrase;
	return fmax(0.0, off + bar * b);
}

/* Load the head of the queue into deck @i, false when the queue is empty. */
static bool load_next(int i)
{
	PdMediaItem *m = pop_queue(i ^ 1);

	if (!m)
		return false;
	g_debug("automix: loading \"%s\" into deck %c", m->title,
		app_deck_letter(i));
	am.app->automix_loading = TRUE;
	app_load_item(am.app, i, m);
	am.app->automix_loading = FALSE;
	g_object_unref(m);
	pd_media_item_changed("*");
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
	struct track *t = deck_track(d);

	if (!atomic_load(&d->playing)) {
		double lead;

		if (at < 0.0)
			at = d->cue;
		/* never play leading silence */
		lead = t ? analyze_silence_head(t, (size_t)at) : 0.0;
		if (lead > 0.0) {
			g_debug("automix: deck %c skips %.1f s of silence",
				app_deck_letter(i), lead);
			at += lead * t->rate;
		}
		deck_seek(d, at);
		deck_play(d, true);
	}
	if (i == am.active)
		am.started = true;
}

/*
 * Where the two tempos meet: the outgoing deck slows (or speeds) to the
 * geometric middle and the incoming one covers the rest, so neither
 * jumps.  When that leaves the pitch range on one side, the incoming
 * deck takes the whole difference.  False when no split fits.
 */
static bool tempo_meet(int from, int to, double *mf, double *mt)
{
	double range = am.app->cfg.pitch_range / 100.0;
	double full = deck_sync_pitch(deck(to), deck(from));
	double cur = atomic_load(&deck(from)->pitch);
	double half;

	if (isnan(full))
		return false;
	half = sqrt(1.0 + full);
	*mt = half - 1.0;
	*mf = (1.0 + cur) / half - 1.0;
	if (fabs(*mt) <= range && fabs(*mf) <= range)
		return true;
	*mf = cur;
	*mt = full;
	return fabs(full) <= range;
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

	match = c->automix_sync &&
		tempo_meet(from, to, &am.meet_from, &am.meet_to);
	am.match = match;
	if (match)
		g_debug("automix: tempo plan %c %.1f%% -> %.1f%%, %c at "
			"%.1f%%", app_deck_letter(from),
			atomic_load(&deck(from)->pitch) * 100.0,
			am.meet_from * 100.0, app_deck_letter(to),
			am.meet_to * 100.0);
	bpm = deck_bpm(deck(from));
	/* silence is skipped outright; quiet is what the blend covers */
	outro = fmax(0.0, analyze_quiet_tail(tf) - analyze_silence_tail(tf));
	{
		double lead = analyze_silence_head(tt, (size_t)deck(to)->cue);

		if (lead > 0.0)
			am.plan_start = deck(to)->cue + lead * tt->rate;
		intro = analyze_quiet_head(tt, (size_t)(deck(to)->cue +
						       lead * tt->rate));
	}
	g_debug("automix: %c silence at end %.1f s, %c silence at start "
		"%.1f s", app_deck_letter(from), analyze_silence_tail(tf),
		app_deck_letter(to),
		analyze_silence_head(tt, (size_t)deck(to)->cue));

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

	/*
	 * Start on a phrase of the outgoing track and run to its end: the
	 * last 16 beat boundary that leaves room for the blend, or the
	 * next one when that has passed already.
	 */
	if (match && track_beat_len(tf) > 0.0 && track_done(tf)) {
		double b = track_beat_len(tf);
		double off = atomic_load(&tf->beat_offset);
		double end = audible_end(from);
		double pos = deck_position(deck(from));
		double beats = (end - len * tf->rate - off) / b;
		double start = off + floor(beats / PHRASE_BEATS) *
			       PHRASE_BEATS * b;
		double phrase_len = PHRASE_BEATS * b;

		while (start < pos + 0.3 * tf->rate)
			start += phrase_len;
		if ((end - start) / tf->rate >= FADE_MIN &&
		    (end - start) / tf->rate <= FADE_MAX)
			len = (end - start) / tf->rate;
	}

	if (deck(to)->cue <= 0.0 && intro > INTRO_MAX)
		am.plan_start = fmax(am.plan_start, 0.0) +
				(intro - len) * tt->rate;
	/* the incoming track comes in on a bar, or a phrase when near one */
	if (match && atomic_load(&tt->analysed))
		am.plan_start = snap_grid(tt, am.plan_start >= 0.0 ?
					  am.plan_start : deck(to)->cue);
	am.plan_len = len;

	/*
	 * How the two sound where they meet.  A louder incoming stretch
	 * is trimmed down to the outgoing level for the blend and comes
	 * in on a slower curve; a quieter one is lifted and comes in
	 * faster, so the level never jumps at the hand over.
	 */
	am.shape = 1.0;
	am.level_db = 0.0;
	if (track_done(tf) && track_done(tt)) {
		double end = audible_end(from);
		double start = am.plan_start >= 0.0 ? am.plan_start :
			       deck(to)->cue;
		double out = analyze_level(tf, (size_t)fmax(end - len *
							     tf->rate, 0.0),
					   (size_t)end);
		double in = analyze_level(tt, (size_t)start,
					  (size_t)(start + len * tt->rate));

		if (out > 0.0 && in > 0.0) {
			am.level_db = 20.0 * log10(in / out);
			/* never hurry: the playing track always leaves slowly */
			am.shape = CLAMP(1.0 + am.level_db / 12.0, 1.0, 1.8);
		}
	}
}

static void begin_fade(void)
{
	struct deck *from = deck(am.active), *to = deck(am.next);
	double range = am.app->cfg.pitch_range / 100.0;

	if (!am.planned)
		plan_transition(am.active, am.next);
	am.prep.on = false;
	am.ret.on = false;
	am.ret.deck = -1;
	if (am.app->cfg.automix_sync) {
		double p;

		if (am.match && am.prepped)
			atomic_store(&from->pitch, (float)am.meet_from);
		/* whatever the outgoing deck runs at now, match it */
		p = deck_sync_pitch(to, from);
		if (!isnan(p) && fabs(p) <= range)
			atomic_store(&to->pitch, (float)p);
	}
	am.started = true;
	am.app->by_hand[am.next] = FALSE;
	blend_save();
	am.trim.on = false;
	am.trim_to = atomic_load(&to->trim_db);
	if (fabs(am.level_db) > 1.0) {
		/* level match for the blend: a trim, undone afterwards */
		float trim = (float)CLAMP(am.trim_to - am.level_db, -12.0,
					  6.0);

		atomic_store(&to->trim_db, trim);
		g_debug("automix: %c is %.1f dB against %c, trim %.1f dB",
			app_deck_letter(am.next), am.level_db,
			app_deck_letter(am.active), trim);
	}
	/* the incoming track enters without its lows, they come in later */
	atomic_store(&to->eq_db[EQ_LOW], (float)LOW_CUT);
	atomic_store(&to->eq_db[EQ_MID], (float)(am.eq_to[EQ_MID] + MID_IN));
	am.lock_int = 0.0;
	am.lock_time = 0;
	am.snaps = 0;
	if (am.app->cfg.automix_sync && am.match &&
	    !atomic_load(&to->playing)) {
		double at = am.plan_start >= 0.0 ? am.plan_start : to->cue;

		at += analyze_silence_head(deck_track(to), (size_t)at) *
		      deck_track(to)->rate;
		start_deck(am.next, aligned_start(am.next, am.active, at));
	} else {
		start_deck(am.next, am.plan_start);
		if (am.app->cfg.automix_sync) {
			deck_sync_phase(to, from);
			bar_align(am.next, am.active);
		}
	}
	pd_media_item_changed("*");

	am.tempo_ratio = deck_bpm(to) > 0.0 ? deck_bpm(from) / deck_bpm(to) :
			 0.0;
	am.fade_start = g_get_monotonic_time();
	am.fade_from = atomic_load(&am.app->engine.xfader);
	am.fade_to = side_value(am.next);
	am.state = AM_FADING;
	am.planned = false;
}

static void end_fade(void)
{
	deck_play(deck(am.active), false);
	blend_restore();
	if (fabs(atomic_load(&deck(am.next)->trim_db) - am.trim_to) > 0.1)
		glide_to(&am.trim, am.next, &deck(am.next)->trim_db, am.trim_to,
			 RETURN_SECS, "trim");
	pd_media_item_changed("*");
	am.app->by_hand[am.active] = FALSE;
	am.active = am.next;
	am.next = -1;
	am.state = AM_PLAYING;
	am.prepped = false;
	/* back to the track's own tempo, unless the blend started that */
	if (am.app->cfg.automix_sync && !(am.ret.on && am.ret.deck == am.active) &&
	    fabs(atomic_load(&deck(am.active)->pitch)) > 1e-4)
		glide_start(&am.ret, am.active, 0.0, RETURN_SECS);
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
		am.app->by_hand[i] = FALSE;
		return;
	}
	for (k = 0; k < DECKS_USED; k++) {
		if (!hand_ready(k))
			continue;
		am.app->by_hand[k] = FALSE;
		am.active = k;
		am.next = -1;
		am.started = false;
		am.state = AM_PLAYING;
		pd_media_item_changed("*");
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
	pd_media_item_changed("*");
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
			am.app->load_failed[am.active] = FALSE;
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
	if (remaining(am.active) <= 0.0) {
		/* only silence is left: end it here, do not play the gap */
		deck_play(d, false);
		am.state = AM_IDLE;
		return;
	}

	/*
	 * Load the next track as soon as the other deck is free, so it
	 * is decoded and analysed long before the transition is due.
	 */
	if (atomic_load(&deck(other)->playing) || am.app->loading[other])
		return;
	if (!hand_ready(other) && !load_next(other))
		return;
	am.app->by_hand[other] = FALSE;
	am.next = other;
	am.state = AM_PRELOADED;
	am.planned = false;
}

static void tick_preloaded(void)
{
	if (next_failed()) {
		set_status("Automix: next track failed, skipping");
		app_unload(am.app, am.next);
		am.app->load_failed[am.next] = FALSE;
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
	if (!am.due_at)
		set_status("Automix: deck %c, %d s left, %c next, %d s blend",
			   app_deck_letter(am.active),
			   (int)remaining(am.active),
			   app_deck_letter(am.next), (int)am.plan_len);
	if (!atomic_load(&deck(am.active)->playing) && !am.force &&
	    remaining(am.active) > 1.0) {
		/* Paused by hand: hold the transition until play resumes. */
		set_status("Automix: deck %c paused, %c ready",
			   app_deck_letter(am.active),
			   app_deck_letter(am.next));
		return;
	}
	if (am.app->cfg.automix_sync && am.match && !am.prepped &&
	    remaining(am.active) <= am.plan_len + PREP_SECS) {
		/* ease the outgoing deck to the meeting tempo before the blend */
		am.prepped = true;
		am.ret.on = false;
		glide_start(&am.prep, am.active, am.meet_from,
			    fmin(PREP_SECS, remaining(am.active) - am.plan_len));
	}
	if (am.force || !atomic_load(&deck(am.active)->playing) ||
	    remaining(am.active) <= am.plan_len) {
		/*
		 * Due.  Wait for the outgoing deck's next downbeat so the
		 * incoming track comes in on a bar, one bar at most.
		 */
		double phase = bar_phase(am.active);
		double bar = deck_bpm(deck(am.active)) > 0.0 ?
			     BAR_BEATS * 60.0 / deck_bpm(deck(am.active)) : 0.0;
		gint64 now = g_get_monotonic_time();

		if (!am.due_at)
			am.due_at = now;
		if (atomic_load(&deck(am.active)->playing) && phase >= 0.0 &&
		    phase > BAR_TOL && phase < 1.0 - BAR_TOL &&
		    (now - am.due_at) / 1e6 < bar + 0.5 &&
		    remaining(am.active) > FADE_MIN + bar) {
			set_status("Automix: deck %c, waiting for the bar",
				   app_deck_letter(am.active));
			return;
		}
		am.force = false;
		am.due_at = 0;
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
	/* a slow start brings a loud newcomer in gently, and vice versa */
	x = am.fade_from + (am.fade_to - am.fade_from) *
	    ease(pow(t, am.shape > 0.0 ? am.shape : 1.0));
	atomic_store(&am.app->engine.xfader, (float)x);
	blend_eq(t);
	if (am.app->cfg.automix_sync && am.match) {
		/*
		 * From the middle of the blend on, both decks drift
		 * together towards the incoming track's own tempo: the
		 * return glide starts here and the outgoing deck follows
		 * it, so the pair stays locked while the tempo settles.
		 */
		if (t >= 0.5 && !am.ret.on && am.ret.deck != am.next &&
		    fabs(atomic_load(&deck(am.next)->pitch)) > 1e-4)
			glide_start(&am.ret, am.next, 0.0, RETURN_SECS);
		if (am.ret.on && am.ret.deck == am.next && am.tempo_ratio > 0.0) {
			struct track *tf = deck_track(deck(am.active));
			struct track *tt = deck_track(deck(am.next));
			double bf = tf ? atomic_load(&tf->bpm) : 0.0;
			double bt = tt ? atomic_load(&tt->bpm) : 0.0;

			if (bf > 0.0 && bt > 0.0) {
				double pt = atomic_load(&deck(am.next)->pitch);
				double pf = am.tempo_ratio * bt * (1.0 + pt) /
					    bf - 1.0;

				atomic_store(&deck(am.active)->pitch,
					     (float)pf);
			}
		}
		beat_lock(am.active, am.next);
	}
	set_status("Automix: mixing %c → %c over %d s",
		   app_deck_letter(am.active), app_deck_letter(am.next),
		   (int)secs);
	if ((int)(t * 10.0) != (int)((t - 0.02) * 10.0))
		g_debug("automix: blend %.0f%%: lock err %.4f beats, int %.4f, "
			"bend %.4f", t * 100.0, am.lock_err, am.lock_int,
			atomic_load(&deck(am.next)->bend));
	if (0)
		g_debug("automix: blend %.0f%%: xf %.2f, %c low %.0f mid %.0f "
			"filt %.2f pitch %.1f%%, %c low %.0f mid %.0f pitch "
			"%.1f%%", t * 100.0, x, app_deck_letter(am.active),
			atomic_load(&deck(am.active)->eq_db[EQ_LOW]),
			atomic_load(&deck(am.active)->eq_db[EQ_MID]),
			atomic_load(&deck(am.active)->filter),
			atomic_load(&deck(am.active)->pitch) * 100.0,
			app_deck_letter(am.next),
			atomic_load(&deck(am.next)->eq_db[EQ_LOW]),
			atomic_load(&deck(am.next)->eq_db[EQ_MID]),
			atomic_load(&deck(am.next)->pitch) * 100.0);
	if (t >= 1.0)
		end_fade();
}

static gboolean tick(gpointer data)
{
	glide_tick(&am.prep);
	glide_tick(&am.ret);
	glide_tick(&am.trim);
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
		if (am.state == AM_FADING)
			blend_restore();	/* stopped mid blend */
		am.prep.on = false;
		am.ret.on = false;
		am.trim.on = false;
		am.state = AM_IDLE;
		am.due_at = 0;
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

int automix_active_deck(void)
{
	return am.on && am.state != AM_IDLE ? am.active : -1;
}

int automix_next_deck(void)
{
	return am.on && (am.state == AM_PRELOADED || am.state == AM_FADING) ?
	       am.next : -1;
}

double automix_pitch_for(PdMediaItem *m)
{
	int from = am.on && am.state != AM_IDLE ? am.active : -1;
	double bpm, b, r;
	int i;

	if (from < 0) {
		/* nothing running: relate to whatever plays */
		for (i = 0; i < DECKS_USED; i++)
			if (atomic_load(&deck(i)->playing))
				from = i;
	}
	if (from < 0)
		return NAN;
	bpm = deck_bpm(deck(from));
	b = item_bpm(m);
	if (bpm <= 0.0 || b <= 0.0)
		return NAN;
	r = bpm / b;
	if (r > 1.5)
		r /= 2.0;
	else if (r < 0.75)
		r *= 2.0;
	return r - 1.0;
}
