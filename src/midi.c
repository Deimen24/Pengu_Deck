// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * midi.c - MIDI controllers through the ALSA sequencer
 */
#include <math.h>
#include <string.h>

#include <glib-unix.h>

#include "automix.h"
#include "midi.h"
#include "pd-build.h"

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#define JOG_BEND	0.004f	/* bend per encoder tick while playing */
#define JOG_SEEK	0.03	/* seconds per encoder tick while paused */
#define BEND_HOLD_MS	120

/* ---- control table ----------------------------------------------- */

#define DECK_CONTROLS(n, L) \
	{ "deck" #n ".play", "Deck " L ": play / pause", MIDI_BUTTON, n }, \
	{ "deck" #n ".cue", "Deck " L ": cue", MIDI_BUTTON, n }, \
	{ "deck" #n ".sync", "Deck " L ": sync", MIDI_BUTTON, n }, \
	{ "deck" #n ".keylock", "Deck " L ": keylock", MIDI_BUTTON, n }, \
	{ "deck" #n ".hotcue1", "Deck " L ": hot cue 1", MIDI_BUTTON, n }, \
	{ "deck" #n ".hotcue2", "Deck " L ": hot cue 2", MIDI_BUTTON, n }, \
	{ "deck" #n ".hotcue3", "Deck " L ": hot cue 3", MIDI_BUTTON, n }, \
	{ "deck" #n ".hotcue4", "Deck " L ": hot cue 4", MIDI_BUTTON, n }, \
	{ "deck" #n ".loop", "Deck " L ": loop on/off", MIDI_BUTTON, n }, \
	{ "deck" #n ".loop4", "Deck " L ": 4 beat loop", MIDI_BUTTON, n }, \
	{ "deck" #n ".loop_half", "Deck " L ": loop ½", MIDI_BUTTON, n }, \
	{ "deck" #n ".loop_double", "Deck " L ": loop 2×", MIDI_BUTTON, n }, \
	{ "deck" #n ".load", "Deck " L ": load selected", MIDI_BUTTON, n }, \
	{ "deck" #n ".reverse", "Deck " L ": reverse", MIDI_BUTTON, n }, \
	{ "deck" #n ".slip", "Deck " L ": slip mode", MIDI_BUTTON, n }, \
	{ "deck" #n ".quantize", "Deck " L ": quantize", MIDI_BUTTON, n }, \
	{ "deck" #n ".censor", "Deck " L ": censor (hold)", MIDI_BUTTON, n }, \
	{ "deck" #n ".roll", "Deck " L ": ½ beat roll (hold)", MIDI_BUTTON, n }, \
	{ "deck" #n ".jump_back", "Deck " L ": beat jump back", MIDI_BUTTON, n }, \
	{ "deck" #n ".jump_fwd", "Deck " L ": beat jump forward", MIDI_BUTTON, n }, \
	{ "deck" #n ".key_down", "Deck " L ": key -1", MIDI_BUTTON, n }, \
	{ "deck" #n ".key_up", "Deck " L ": key +1", MIDI_BUTTON, n }, \
	{ "deck" #n ".pfl", "Deck " L ": headphone cue", MIDI_BUTTON, n }, \
	{ "deck" #n ".pitch", "Deck " L ": pitch fader", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".volume", "Deck " L ": channel fader", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".trim", "Deck " L ": trim", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".eq_high", "Deck " L ": EQ high", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".eq_mid", "Deck " L ": EQ mid", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".eq_low", "Deck " L ": EQ low", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".filter", "Deck " L ": filter", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".fx_on", "Deck " L ": FX on/off", MIDI_BUTTON, n }, \
	{ "deck" #n ".fx_wet", "Deck " L ": FX wet", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".fx_param", "Deck " L ": FX parameter", MIDI_ABSOLUTE, n }, \
	{ "deck" #n ".jog", "Deck " L ": jog wheel", MIDI_RELATIVE, n }

static const struct midi_control controls[] = {
	{ "xfader", "Crossfader", MIDI_ABSOLUTE, -1 },
	{ "master", "Master level", MIDI_ABSOLUTE, -1 },
	{ "cue_mix", "Headphone mix", MIDI_ABSOLUTE, -1 },
	{ "cue_vol", "Headphone volume", MIDI_ABSOLUTE, -1 },
	{ "automix", "Automix on/off", MIDI_BUTTON, -1 },
	{ "automix_next", "Automix next", MIDI_BUTTON, -1 },
	{ "sampler1", "Sampler pad 1", MIDI_BUTTON, -1 },
	{ "sampler2", "Sampler pad 2", MIDI_BUTTON, -1 },
	{ "sampler3", "Sampler pad 3", MIDI_BUTTON, -1 },
	{ "sampler4", "Sampler pad 4", MIDI_BUTTON, -1 },
	{ "sampler5", "Sampler pad 5", MIDI_BUTTON, -1 },
	{ "sampler6", "Sampler pad 6", MIDI_BUTTON, -1 },
	{ "sampler7", "Sampler pad 7", MIDI_BUTTON, -1 },
	{ "sampler8", "Sampler pad 8", MIDI_BUTTON, -1 },
	{ "sampler_vol", "Sampler volume", MIDI_ABSOLUTE, -1 },
	DECK_CONTROLS(0, "A"),
	DECK_CONTROLS(1, "B"),
	DECK_CONTROLS(2, "C"),
	DECK_CONTROLS(3, "D"),
};

const struct midi_control *midi_controls(unsigned int *n)
{
	*n = G_N_ELEMENTS(controls);
	return controls;
}

/* ---- state ------------------------------------------------------- */

enum msg_type {
	MSG_NOTE,
	MSG_CC,
};

struct msg {
	enum msg_type type;
	int channel;
	int number;
};

static struct app *app;
static GHashTable *bindings;	/* "note:ch:num" → control name */
static GKeyFile *keyfile;
static char *keyfile_path;
static char *learn_target;
static char last_msg[64];
static guint bend_timer[ENGINE_DECKS];

#ifdef HAVE_ALSA
static snd_seq_t *seq;
static int port;
static guint *fd_sources;
static unsigned int n_fd_sources;
#endif

static char *msg_key(const struct msg *m)
{
	return g_strdup_printf("%s:%d:%d", m->type == MSG_NOTE ? "note" : "cc",
			       m->channel, m->number);
}

static char *msg_text(const struct msg *m)
{
	return g_strdup_printf("%s %d ch %d", m->type == MSG_NOTE ? "Note" :
			       "CC", m->number, m->channel + 1);
}

static void save_bindings(void)
{
	GError *err = NULL;

	if (!keyfile)
		return;
	if (!g_key_file_save_to_file(keyfile, keyfile_path, &err)) {
		g_warning("Saving MIDI mapping failed: %s", err->message);
		g_error_free(err);
	}
}

static void load_bindings(void)
{
	char **keys;
	gsize n, i;

	keyfile = g_key_file_new();
	g_key_file_load_from_file(keyfile, keyfile_path, G_KEY_FILE_NONE,
				  NULL);
	keys = g_key_file_get_keys(keyfile, "mappings", &n, NULL);
	for (i = 0; keys && i < n; i++) {
		char *v = g_key_file_get_string(keyfile, "mappings", keys[i],
						NULL);

		if (v && *v)
			g_hash_table_insert(bindings, g_strdup(keys[i]), v);
		else
			g_free(v);
	}
	g_strfreev(keys);
}

/* ---- actions ----------------------------------------------------- */

static gboolean bend_release(gpointer data)
{
	int i = GPOINTER_TO_INT(data);

	bend_timer[i] = 0;
	atomic_store(&app->engine.deck[i].bend, 0.0f);
	return G_SOURCE_REMOVE;
}

static void jog(int i, int delta)
{
	struct deck *d = &app->engine.deck[i];
	struct track *t = deck_track(d);

	if (!t)
		return;
	if (atomic_load(&d->playing)) {
		atomic_store(&d->bend, CLAMP(delta * JOG_BEND, -0.2f, 0.2f));
		if (bend_timer[i])
			g_source_remove(bend_timer[i]);
		bend_timer[i] = g_timeout_add(BEND_HOLD_MS, bend_release,
					      GINT_TO_POINTER(i));
	} else {
		deck_seek(d, deck_position(d) + delta * JOG_SEEK * t->rate);
	}
}

static void deck_button(int i, const char *what, bool press)
{
	struct deck *d = &app->engine.deck[i];

	if (g_str_equal(what, "play")) {
		if (press)
			deck_play(d, !atomic_load(&d->playing));
	} else if (g_str_equal(what, "cue")) {
		if (press)
			deck_cue_press(d);
		else
			deck_cue_release(d);
	} else if (g_str_equal(what, "sync")) {
		if (press && app->sync_deck)
			app->sync_deck(app->ui_data, i);
	} else if (g_str_equal(what, "keylock")) {
		if (press)
			atomic_store(&d->keylock, !atomic_load(&d->keylock));
	} else if (g_str_has_prefix(what, "hotcue")) {
		if (press)
			deck_hotcue(d, what[6] - '1');
	} else if (g_str_equal(what, "loop")) {
		if (press)
			deck_loop_toggle(d);
	} else if (g_str_equal(what, "loop4")) {
		if (press)
			deck_loop_beats(d, 4.0);
	} else if (g_str_equal(what, "loop_half")) {
		if (press)
			deck_loop_scale(d, 0.5);
	} else if (g_str_equal(what, "loop_double")) {
		if (press)
			deck_loop_scale(d, 2.0);
	} else if (g_str_equal(what, "load")) {
		if (press && app->load_selected)
			app->load_selected(app->ui_data, i);
	} else if (g_str_equal(what, "pfl")) {
		if (press)
			atomic_store(&d->pfl, !atomic_load(&d->pfl));
	} else if (g_str_equal(what, "reverse")) {
		if (press)
			atomic_store(&d->reverse, !atomic_load(&d->reverse));
	} else if (g_str_equal(what, "slip")) {
		if (press)
			atomic_store(&d->slip, !atomic_load(&d->slip));
	} else if (g_str_equal(what, "quantize")) {
		if (press)
			atomic_store(&d->quantize, !atomic_load(&d->quantize));
	} else if (g_str_equal(what, "censor")) {
		deck_censor(d, press);
	} else if (g_str_equal(what, "roll")) {
		if (press)
			deck_roll_start(d, 0.5);
		else
			deck_roll_end(d);
	} else if (g_str_equal(what, "jump_back")) {
		if (press)
			deck_beat_jump(d, -d->loop_beats);
	} else if (g_str_equal(what, "jump_fwd")) {
		if (press)
			deck_beat_jump(d, d->loop_beats);
	} else if (g_str_equal(what, "key_down")) {
		if (press)
			atomic_store(&d->key_shift,
				     CLAMP(atomic_load(&d->key_shift) - 1,
					   -12, 12));
	} else if (g_str_equal(what, "fx_on")) {
		if (press)
			atomic_store(&d->fx.on, !atomic_load(&d->fx.on));
	} else if (g_str_equal(what, "key_up")) {
		if (press)
			atomic_store(&d->key_shift,
				     CLAMP(atomic_load(&d->key_shift) + 1,
					   -12, 12));
	}
}

static void deck_absolute(int i, const char *what, double f)
{
	struct deck *d = &app->engine.deck[i];
	double range = app->cfg.pitch_range / 100.0;

	if (g_str_equal(what, "pitch"))
		atomic_store(&d->pitch, (float)((f * 2.0 - 1.0) * range));
	else if (g_str_equal(what, "volume"))
		atomic_store(&d->volume, (float)f);
	else if (g_str_equal(what, "trim"))
		atomic_store(&d->trim_db, (float)(f * 24.0 - 12.0));
	else if (g_str_equal(what, "eq_high"))
		atomic_store(&d->eq_db[EQ_HIGH], (float)(f * 35.0 - 26.0));
	else if (g_str_equal(what, "eq_mid"))
		atomic_store(&d->eq_db[EQ_MID], (float)(f * 35.0 - 26.0));
	else if (g_str_equal(what, "eq_low"))
		atomic_store(&d->eq_db[EQ_LOW], (float)(f * 35.0 - 26.0));
	else if (g_str_equal(what, "filter"))
		atomic_store(&d->filter, (float)(f * 2.0 - 1.0));
	else if (g_str_equal(what, "fx_wet"))
		atomic_store(&d->fx.wet, (float)f);
	else if (g_str_equal(what, "fx_param"))
		atomic_store(&d->fx.param, (float)f);
}

static void global_control(const char *name, int value, bool press)
{
	struct engine *e = &app->engine;
	double f = value / 127.0;

	if (g_str_equal(name, "xfader"))
		atomic_store(&e->xfader, (float)(f * 2.0 - 1.0));
	else if (g_str_equal(name, "master"))
		atomic_store(&e->master, (float)(f * 1.5));
	else if (g_str_equal(name, "cue_mix"))
		atomic_store(&e->cue_mix, (float)f);
	else if (g_str_equal(name, "cue_vol"))
		atomic_store(&e->cue_vol, (float)(f * 1.5));
	else if (g_str_equal(name, "automix") && press)
		automix_set_enabled(!automix_enabled());
	else if (g_str_equal(name, "automix_next") && press)
		automix_next();
	else if (g_str_has_prefix(name, "sampler_vol"))
		atomic_store(&e->sampler.volume, (float)f);
	else if (g_str_has_prefix(name, "sampler"))
		sampler_trigger(&e->sampler, name[7] - '1', press);
}

static const struct midi_control *find_control(const char *name)
{
	size_t i;

	for (i = 0; i < G_N_ELEMENTS(controls); i++)
		if (g_str_equal(controls[i].name, name))
			return &controls[i];
	return NULL;
}

static void dispatch(const char *name, const struct msg *m, int value)
{
	const struct midi_control *c = find_control(name);
	const char *what;
	bool press;

	if (!c)
		return;
	/* Note off and cc == 0 count as release for buttons. */
	press = value > 63;
	if (c->deck < 0) {
		global_control(name, value, press);
		return;
	}
	what = strchr(name, '.') + 1;
	switch (c->kind) {
	case MIDI_BUTTON:
		deck_button(c->deck, what, press);
		break;
	case MIDI_ABSOLUTE:
		deck_absolute(c->deck, what, value / 127.0);
		break;
	case MIDI_RELATIVE:
		/* Two's complement (Pioneer) or sign bit relative modes. */
		jog(c->deck, value < 64 ? value : value - 128);
		break;
	}
}

static void handle(const struct msg *m, int value)
{
	char *key = msg_key(m);
	char *text = msg_text(m);
	const char *bound;

	g_snprintf(last_msg, sizeof(last_msg), "%s = %d", text, value);
	if (learn_target) {
		const struct midi_control *c = find_control(learn_target);

		/* A button release must not steal the learn slot. */
		if (!c || (c->kind == MIDI_BUTTON && value == 0)) {
			g_free(key);
			g_free(text);
			return;
		}
		midi_unbind(learn_target);
		g_hash_table_insert(bindings, g_strdup(key),
				    g_strdup(learn_target));
		g_key_file_set_string(keyfile, "mappings", key, learn_target);
		save_bindings();
		g_clear_pointer(&learn_target, g_free);
		app_toast(app, "MIDI: %s → %s", text, c->label);
	}
	bound = g_hash_table_lookup(bindings, key);
	if (bound)
		dispatch(bound, m, value);
	g_free(key);
	g_free(text);
}

/* ---- alsa -------------------------------------------------------- */

#ifdef HAVE_ALSA
static void connect_client(int client)
{
	snd_seq_client_info_t *ci;
	snd_seq_port_info_t *pi;
	int mine = snd_seq_client_id(seq);

	if (client == mine || client == SND_SEQ_CLIENT_SYSTEM)
		return;
	snd_seq_client_info_alloca(&ci);
	snd_seq_port_info_alloca(&pi);
	if (snd_seq_get_any_client_info(seq, client, ci) < 0)
		return;
	snd_seq_port_info_set_client(pi, client);
	snd_seq_port_info_set_port(pi, -1);
	while (snd_seq_query_next_port(seq, pi) >= 0) {
		unsigned int caps = snd_seq_port_info_get_capability(pi);
		unsigned int want = SND_SEQ_PORT_CAP_READ |
				    SND_SEQ_PORT_CAP_SUBS_READ;

		if ((caps & want) != want ||
		    (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
			continue;
		if (!(snd_seq_port_info_get_type(pi) &
		      SND_SEQ_PORT_TYPE_MIDI_GENERIC))
			continue;
		snd_seq_connect_from(seq, port, client,
				     snd_seq_port_info_get_port(pi));
	}
}

static void connect_all(void)
{
	snd_seq_client_info_t *ci;

	snd_seq_client_info_alloca(&ci);
	snd_seq_client_info_set_client(ci, -1);
	while (snd_seq_query_next_client(seq, ci) >= 0)
		connect_client(snd_seq_client_info_get_client(ci));
}

static void handle_event(const snd_seq_event_t *ev)
{
	struct msg m;

	switch (ev->type) {
	case SND_SEQ_EVENT_NOTEON:
	case SND_SEQ_EVENT_NOTEOFF:
		m.type = MSG_NOTE;
		m.channel = ev->data.note.channel;
		m.number = ev->data.note.note;
		handle(&m, ev->type == SND_SEQ_EVENT_NOTEOFF ? 0 :
		       ev->data.note.velocity);
		break;
	case SND_SEQ_EVENT_CONTROLLER:
		m.type = MSG_CC;
		m.channel = ev->data.control.channel;
		m.number = ev->data.control.param;
		handle(&m, ev->data.control.value);
		break;
	case SND_SEQ_EVENT_PORT_START:
		connect_client(ev->data.addr.client);
		break;
	default:
		break;
	}
}

static gboolean seq_readable(gint fd, GIOCondition cond, gpointer data)
{
	snd_seq_event_t *ev;

	while (snd_seq_event_input_pending(seq, 1) > 0) {
		if (snd_seq_event_input(seq, &ev) < 0)
			break;
		handle_event(ev);
	}
	return G_SOURCE_CONTINUE;
}

static void open_seq(void)
{
	struct pollfd *pfd;
	int n, i;

	if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT,
			 SND_SEQ_NONBLOCK) < 0) {
		seq = NULL;
		return;
	}
	snd_seq_set_client_name(seq, "Pengu Deck");
	port = snd_seq_create_simple_port(seq, "control in",
			SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC |
			SND_SEQ_PORT_TYPE_APPLICATION);
	if (port < 0) {
		snd_seq_close(seq);
		seq = NULL;
		return;
	}
	/* Hot plug notifications come from the system announce port. */
	snd_seq_connect_from(seq, port, SND_SEQ_CLIENT_SYSTEM,
			     SND_SEQ_PORT_SYSTEM_ANNOUNCE);
	connect_all();

	n = snd_seq_poll_descriptors_count(seq, POLLIN);
	pfd = g_new0(struct pollfd, n);
	n = snd_seq_poll_descriptors(seq, pfd, n, POLLIN);
	fd_sources = g_new0(guint, n);
	n_fd_sources = n;
	for (i = 0; i < n; i++)
		fd_sources[i] = g_unix_fd_add(pfd[i].fd, G_IO_IN,
					      seq_readable, NULL);
	g_free(pfd);
}

static void close_seq(void)
{
	unsigned int i;

	for (i = 0; i < n_fd_sources; i++)
		g_source_remove(fd_sources[i]);
	g_free(fd_sources);
	fd_sources = NULL;
	n_fd_sources = 0;
	if (seq)
		snd_seq_close(seq);
	seq = NULL;
}
#endif

/* ---- api --------------------------------------------------------- */

void midi_init(struct app *a)
{
	char *dir = g_build_filename(g_get_user_config_dir(), "pengu-deck",
				     NULL);

	app = a;
	g_mkdir_with_parents(dir, 0700);
	keyfile_path = g_build_filename(dir, "midi.ini", NULL);
	g_free(dir);
	bindings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					 g_free);
	load_bindings();
	g_strlcpy(last_msg, "nothing received yet", sizeof(last_msg));
#ifdef HAVE_ALSA
	open_seq();
#endif
}

void midi_shutdown(void)
{
	int i;

#ifdef HAVE_ALSA
	close_seq();
#endif
	for (i = 0; i < ENGINE_DECKS; i++)
		if (bend_timer[i])
			g_source_remove(bend_timer[i]);
	g_clear_pointer(&bindings, g_hash_table_unref);
	g_clear_pointer(&keyfile, g_key_file_free);
	g_clear_pointer(&keyfile_path, g_free);
	g_clear_pointer(&learn_target, g_free);
}

bool midi_available(void)
{
#ifdef HAVE_ALSA
	return seq != NULL;
#else
	return false;
#endif
}

char **midi_devices(void)
{
	GPtrArray *names = g_ptr_array_new();
#ifdef HAVE_ALSA
	snd_seq_client_info_t *ci;

	if (seq) {
		snd_seq_client_info_alloca(&ci);
		snd_seq_client_info_set_client(ci, -1);
		while (snd_seq_query_next_client(seq, ci) >= 0) {
			int id = snd_seq_client_info_get_client(ci);

			if (id == SND_SEQ_CLIENT_SYSTEM ||
			    id == snd_seq_client_id(seq))
				continue;
			if (snd_seq_client_info_get_type(ci) !=
			    SND_SEQ_KERNEL_CLIENT)
				continue;
			g_ptr_array_add(names, g_strdup(
					snd_seq_client_info_get_name(ci)));
		}
	}
#endif
	g_ptr_array_add(names, NULL);
	return (char **)g_ptr_array_free(names, FALSE);
}

static const char *key_for_control(const char *control)
{
	GHashTableIter it;
	gpointer k, v;

	g_hash_table_iter_init(&it, bindings);
	while (g_hash_table_iter_next(&it, &k, &v))
		if (g_str_equal(v, control))
			return k;
	return NULL;
}

char *midi_binding(const char *control)
{
	const char *key = key_for_control(control);
	char **parts;
	char *text;

	if (!key)
		return NULL;
	parts = g_strsplit(key, ":", 3);
	text = g_strdup_printf("%s %s ch %d",
			       g_str_equal(parts[0], "note") ? "Note" : "CC",
			       parts[2], atoi(parts[1]) + 1);
	g_strfreev(parts);
	return text;
}

void midi_unbind(const char *control)
{
	const char *key;

	while ((key = key_for_control(control))) {
		g_key_file_remove_key(keyfile, "mappings", key, NULL);
		g_hash_table_remove(bindings, key);
	}
	save_bindings();
}

void midi_learn(const char *control)
{
	g_free(learn_target);
	learn_target = g_strdup(control);
}

bool midi_learning(void)
{
	return learn_target != NULL;
}

const char *midi_last_message(void)
{
	return last_msg;
}
