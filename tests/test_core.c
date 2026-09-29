// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * test_core.c - unit tests for the audio core
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "analyze.h"
#include "cuestore.h"
#include "decoder.h"
#include "deck.h"
#include "dsp.h"
#include "engine.h"
#include "fx.h"
#include "playlists.h"
#include "sampler.h"
#include "soundcloud.h"
#include "track.h"

#define RATE	44100

/* Synthesise a kick drum pattern at @bpm with a swung offbeat hat. */
static struct track *make_beat(double bpm, double seconds, double offset)
{
	struct track *t = track_new("mem", "mem", RATE);
	size_t n = (size_t)(seconds * RATE), i;
	int16_t *pcm = g_new0(int16_t, 2 * n);
	double beat = 60.0 * RATE / bpm;
	GRand *rnd = g_rand_new_with_seed(7);

	for (i = 0; i < n; i++) {
		double since = fmod((double)i - offset + beat, beat);
		double hat = fmod((double)i - offset + beat * 1.5, beat);
		double v = 0.0;

		/* 60 Hz sine kick with an exponential decay */
		v += 0.8 * exp(-since / (0.09 * RATE)) *
		     sin(2.0 * M_PI * 60.0 * since / RATE);
		/* noise burst hi-hat between the kicks */
		v += 0.15 * exp(-hat / (0.02 * RATE)) *
		     (g_rand_double(rnd) * 2.0 - 1.0);
		/* sustained pad to make the envelope non-trivial */
		v += 0.1 * sin(2.0 * M_PI * 440.0 * i / RATE);
		pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(v * 20000.0);
	}
	g_assert_cmpint(track_append(t, pcm, n), ==, 0);
	atomic_store(&t->state, TRACK_DECODED);
	g_free(pcm);
	g_rand_free(rnd);
	return t;
}

static void test_track_append(void)
{
	struct track *t = track_new("x", "k", RATE);
	int16_t buf[2 * 1000];
	size_t i;

	for (i = 0; i < 1000; i++) {
		buf[2 * i] = (int16_t)i;
		buf[2 * i + 1] = (int16_t)-i;
	}
	/* cross a chunk boundary */
	for (i = 0; i < TRACK_CHUNK_FRAMES / 1000 + 2; i++)
		g_assert_cmpint(track_append(t, buf, 1000), ==, 0);

	g_assert_cmpuint(track_frames(t), ==, 1000 * (TRACK_CHUNK_FRAMES /
						       1000 + 2));
	g_assert_cmpint(track_frame(t, TRACK_CHUNK_FRAMES + 5)[0], ==,
			(TRACK_CHUNK_FRAMES + 5) % 1000);
	g_assert_cmpint(track_frame(t, 999)[1], ==, -999);
	g_assert_cmpuint(track_bins(t), >, 0);
	track_unref(t);
}

static void check_tempo(double bpm)
{
	struct track *t = make_beat(bpm, 40.0, 0.31 * RATE);
	double got = 0.0, off = 0.0, beat, phase;

	g_assert_cmpint(analyze_tempo(t, &got, &off), ==, 0);
	g_assert_cmpfloat(fabs(got - bpm), <, 0.05);

	beat = 60.0 * RATE / bpm;
	phase = fmod(off - 0.31 * RATE + beat * 100, beat);
	if (phase > beat / 2)
		phase = beat - phase;
	/* first beat within 25 ms of the true onset */
	g_assert_cmpfloat(phase, <, 0.025 * RATE);
	track_unref(t);
}

static void test_tempo_128(void)
{
	check_tempo(128.0);
}

static void test_tempo_174(void)
{
	check_tempo(174.0);
}

static void test_tempo_95(void)
{
	check_tempo(95.0);
}

/* Chords of C major: expect C major (or its relative A minor). */
static void test_key(void)
{
	struct track *t = track_new("mem", "mem", RATE);
	size_t n = 30 * RATE, i;
	int16_t *pcm = g_new0(int16_t, 2 * n);
	static const double chords[4][3] = {
		{ 261.63, 329.63, 392.00 },	/* C E G */
		{ 349.23, 440.00, 523.25 },	/* F A C */
		{ 392.00, 493.88, 587.33 },	/* G B D */
		{ 261.63, 329.63, 392.00 },
	};
	int key;

	for (i = 0; i < n; i++) {
		const double *c = chords[(i / (4 * RATE)) % 4];
		double v = 0.0;
		int k;

		for (k = 0; k < 3; k++)
			v += sin(2 * M_PI * c[k] * i / RATE) +
			     0.3 * sin(2 * M_PI * c[k] * 2 * i / RATE);
		pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(v * 4000.0);
	}
	track_append(t, pcm, n);
	g_free(pcm);
	key = analyze_key(t);
	g_assert_true(key == 0 || key == 21);
	g_assert_cmpstr(key_camelot(0), ==, "8B");
	g_assert_cmpstr(key_camelot(21), ==, "8A");
	g_assert_cmpstr(key_name(18), ==, "F#m");
	g_assert_cmpint(key_distance(0, 2), ==, 2);
	g_assert_cmpint(key_distance(0, 11), ==, -1);
	g_assert_cmpint(key_distance(0, 21), ==, 0);
	track_unref(t);
}

static void test_gain(void)
{
	struct track *t = track_new("mem", "mem", RATE);
	size_t n = 5 * RATE, i;
	int16_t *pcm = g_new0(int16_t, 2 * n);
	float g;

	/* full scale sine: -3 dBFS RMS, so the gain must be about -15 dB */
	for (i = 0; i < n; i++)
		pcm[2 * i] = pcm[2 * i + 1] =
			(int16_t)(sin(2 * M_PI * 440 * i / RATE) * 32000);
	track_append(t, pcm, n);
	g_free(pcm);
	g = analyze_gain(t);
	g_assert_cmpfloat(fabsf(g + 12.0f), <, 0.5f);	/* clamped */
	track_unref(t);
}

static void test_fx(void)
{
	struct fx f;
	float buf[512 * 2];
	int t, i;

	fx_init(&f);
	fx_set_rate(&f, RATE);
	atomic_store(&f.on, true);
	for (t = FX_NONE; t < FX_COUNT; t++) {
		float energy = 0.0f;

		atomic_store(&f.type, t);
		for (i = 0; i < 20; i++) {
			int j;

			for (j = 0; j < 512; j++)
				buf[2 * j] = buf[2 * j + 1] =
					0.5f * sinf(2.0f * (float)M_PI * 220.0f *
						    (i * 512 + j) / RATE);
			fx_process(&f, buf, 512, 128.0);
			for (j = 0; j < 1024; j++) {
				g_assert_false(isnan(buf[j]));
				energy += buf[j] * buf[j];
			}
		}
		g_assert_cmpfloat(energy, >, 0.0f);
	}
	fx_fini(&f);
}

static void test_sampler(void)
{
	struct sampler s;
	struct track *t = make_beat(120.0, 1.0, 0);
	float out[256 * 2];
	int i;

	sampler_init(&s);
	sampler_set_rate(&s, RATE);
	sampler_load(&s, 0, t);
	track_unref(t);
	memset(out, 0, sizeof(out));
	sampler_render(&s, out, 256);
	g_assert_cmpfloat(out[0], ==, 0.0f);	/* not triggered */
	sampler_trigger(&s, 0, true);
	memset(out, 0, sizeof(out));
	for (i = 0; i < 4; i++)
		sampler_render(&s, out, 256);
	g_assert_true(atomic_load(&s.pad[0].playing));
	g_assert_cmpfloat(atomic_load(&s.pad[0].pos), >, 0.0);
	/* one shot ends by itself */
	for (i = 0; i < RATE / 256 + 2; i++)
		sampler_render(&s, out, 256);
	g_assert_false(atomic_load(&s.pad[0].playing));
	sampler_fini(&s);
}

static void test_rekordbox(void)
{
	static const char xml[] =
		"<?xml version=\"1.0\"?><DJ_PLAYLISTS Version=\"1.0.0\">"
		"<COLLECTION Entries=\"1\">"
		"<TRACK TrackID=\"7\" Name=\"Song\" Artist=\"DJ X\" "
		"Album=\"LP\" Genre=\"Techno\" AverageBpm=\"128.00\" "
		"TotalTime=\"300\" Location=\"file://localhost/tmp/My%20Song.mp3\">"
		"<TEMPO Inizio=\"0.250\" Bpm=\"128.00\" Metro=\"4/4\" Battito=\"1\"/>"
		"<POSITION_MARK Name=\"\" Type=\"0\" Start=\"1.500\" Num=\"-1\"/>"
		"<POSITION_MARK Name=\"\" Type=\"0\" Start=\"30.000\" Num=\"0\"/>"
		"<POSITION_MARK Name=\"\" Type=\"0\" Start=\"60.000\" Num=\"2\"/>"
		"</TRACK></COLLECTION>"
		"<PLAYLISTS><NODE Type=\"0\" Name=\"ROOT\" Count=\"1\">"
		"<NODE Name=\"Peak time\" Type=\"1\" KeyType=\"0\" Entries=\"1\">"
		"<TRACK Key=\"7\"/></NODE></NODE></PLAYLISTS></DJ_PLAYLISTS>";
	char *path = g_build_filename(g_get_tmp_dir(), "pd-rb.xml", NULL);
	struct rb_import out;
	struct track_info info;
	struct playlist *p;
	GError *err = NULL;
	PdMediaItem *m;

	g_setenv("XDG_DATA_HOME", g_get_tmp_dir(), TRUE);
	cuestore_open();
	playlists_open();
	g_assert_true(g_file_set_contents(path, xml, -1, NULL));
	g_assert_true(rekordbox_import(path, &out, &err));
	g_assert_no_error(err);
	g_assert_cmpuint(out.tracks->len, ==, 1);
	g_assert_cmpuint(out.cues, ==, 3);
	g_assert_cmpuint(out.playlists, ==, 1);
	m = out.tracks->pdata[0];
	g_assert_cmpstr(m->key, ==, "/tmp/My Song.mp3");
	g_assert_cmpstr(m->artist, ==, "DJ X");
	g_assert_true(cuestore_get("/tmp/My Song.mp3", &info));
	g_assert_cmpfloat(info.bpm, ==, 128.0);
	g_assert_cmpfloat(info.beat_offset, ==, 0.25);
	g_assert_cmpfloat(info.cue, ==, 1.5);
	g_assert_cmpfloat(info.hotcue[0], ==, 30.0);
	g_assert_cmpfloat(info.hotcue[2], ==, 60.0);
	g_assert_cmpfloat(info.hotcue[1], <, 0.0);
	p = playlists_find("Peak time");
	g_assert_nonnull(p);
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(p->items)),
			 ==, 1);
	playlists_add(p, m);	/* duplicate is ignored */
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(p->items)),
			 ==, 1);
	rb_import_clear(&out);
	playlists_delete("Peak time");
	g_assert_null(playlists_find("Peak time"));
	playlists_close();
	cuestore_close();
	g_unlink(path);
	g_free(path);
}

static void test_bpm_fold(void)
{
	g_assert_cmpfloat(bpm_fold(64.0), ==, 128.0);
	g_assert_cmpfloat(bpm_fold(256.0), ==, 128.0);
	g_assert_cmpfloat(bpm_fold(0.0), ==, 0.0);
	g_assert_cmpfloat(bpm_fold(100.0), ==, 100.0);
}

static float biquad_gain(struct biquad *bq, double freq)
{
	float peak = 0.0f;
	int i;

	biquad_reset(bq);
	for (i = 0; i < RATE; i++) {
		float y = biquad_run(bq, sinf(2.0f * (float)M_PI * (float)freq *
					      i / RATE));

		if (i > RATE / 2 && fabsf(y) > peak)
			peak = fabsf(y);
	}
	return peak;
}

static void test_biquad(void)
{
	struct biquad bq;

	biquad_design(&bq, BQ_LOWSHELF, RATE, 200.0, 0.7, -12.0);
	g_assert_cmpfloat(biquad_gain(&bq, 40.0), <, 0.3f);
	g_assert_cmpfloat(biquad_gain(&bq, 8000.0), >, 0.9f);

	biquad_design(&bq, BQ_HIGHPASS, RATE, 2000.0, 1.0, 0.0);
	g_assert_cmpfloat(biquad_gain(&bq, 100.0), <, 0.05f);
	g_assert_cmpfloat(biquad_gain(&bq, 10000.0), >, 0.9f);

	biquad_bypass(&bq);
	g_assert_cmpfloat(fabsf(biquad_gain(&bq, 1000.0) - 1.0f), <, 0.01f);
}

static void test_deck_play(void)
{
	struct engine e;
	struct track *t = make_beat(120.0, 4.0, 0);
	float out[512 * 2];
	double pos;
	int i;

	engine_init(&e);
	e.rate = RATE;
	e.channels = 2;
	deck_set_rate(&e.deck[0], RATE);
	deck_set_rate(&e.deck[1], RATE);
	deck_load(&e.deck[0], t);
	track_unref(t);

	/* silence while stopped */
	engine_process(&e, out, 512);
	for (i = 0; i < 1024; i++)
		g_assert_cmpfloat(out[i], ==, 0.0f);

	deck_play(&e.deck[0], true);
	atomic_store(&e.deck[0].pitch, 0.08f);
	for (i = 0; i < 100; i++)
		engine_process(&e, out, 512);
	pos = deck_position(&e.deck[0]);
	g_assert_cmpfloat(fabs(pos - 100 * 512 * 1.08), <, 2.0);

	/* something audible came out */
	g_assert_cmpfloat(atomic_load(&e.peak_l), >, 0.05f);

	/* loop of one beat keeps position inside the loop */
	deck_loop_beats(&e.deck[0], 1.0);
	for (i = 0; i < 200; i++) {
		engine_process(&e, out, 512);
		pos = deck_position(&e.deck[0]);
		g_assert_cmpfloat(pos, <=, atomic_load(&e.deck[0].loop_out) + 2);
	}

	/* keylock path renders without crashing and keeps advancing */
	if (deck_has_keylock()) {
		atomic_store(&e.deck[0].loop_on, false);
		atomic_store(&e.deck[0].keylock, true);
		pos = deck_position(&e.deck[0]);
		for (i = 0; i < 50; i++)
			engine_process(&e, out, 512);
		g_assert_cmpfloat(deck_position(&e.deck[0]), >, pos);
	}
	engine_fini(&e);
}

static void test_seek_while_loading(void)
{
	struct engine e;
	struct track *t = track_new("mem", "mem", RATE);

	engine_init(&e);
	deck_set_rate(&e.deck[0], RATE);
	deck_set_rate(&e.deck[1], RATE);
	atomic_store(&t->length_hint, (size_t)RATE * 60);
	deck_load(&e.deck[0], t);
	track_unref(t);

	/* Nothing decoded yet: a restored cue point must survive. */
	deck_seek(&e.deck[0], 30.0 * RATE);
	g_assert_cmpfloat(deck_position(&e.deck[0]), ==, 30.0 * RATE);

	/* Once decoding is done, seeks clamp to the real length. */
	atomic_store(&t->state, TRACK_DECODED);
	deck_seek(&e.deck[0], 30.0 * RATE);
	g_assert_cmpfloat(deck_position(&e.deck[0]), ==, 0.0);
	engine_fini(&e);
}

static void test_sync(void)
{
	struct engine e;
	struct track *a = make_beat(128.0, 2.0, 0);
	struct track *b = make_beat(100.0, 2.0, 0);

	engine_init(&e);
	deck_set_rate(&e.deck[0], RATE);
	deck_set_rate(&e.deck[1], RATE);
	atomic_store(&a->bpm, 128.0);
	atomic_store(&b->bpm, 100.0);
	deck_load(&e.deck[0], a);
	deck_load(&e.deck[1], b);
	track_unref(a);
	track_unref(b);

	g_assert_cmpfloat(fabs(deck_sync_pitch(&e.deck[1], &e.deck[0]) -
			       0.28), <, 1e-6);
	atomic_store(&e.deck[0].pitch, -0.5f);	/* 64 bpm -> match at 2x */
	g_assert_cmpfloat(fabs(deck_sync_pitch(&e.deck[1], &e.deck[0]) -
			       0.28), <, 1e-6);
	engine_fini(&e);
}

static void write_wav(const char *path, double seconds)
{
	size_t n = (size_t)(seconds * RATE), i;
	guint32 data = (guint32)(n * 4), riff = data + 36;
	guint16 fmt = 1, ch = 2, bits = 16, align = 4;
	guint32 rate = RATE, brate = RATE * 4, len = 16;
	FILE *f = fopen(path, "wb");

	g_assert_nonnull(f);
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&len, 4, 1, f);
	fwrite(&fmt, 2, 1, f);
	fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&brate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	for (i = 0; i < n; i++) {
		int16_t s = (int16_t)(sin(2 * M_PI * 220 * i / RATE) * 8000);

		fwrite(&s, 2, 1, f);
		fwrite(&s, 2, 1, f);
	}
	fclose(f);
}

static void test_decoder(void)
{
	char *path = g_build_filename(g_get_tmp_dir(), "pd-test.wav", NULL);
	struct track *t;
	struct media_tags tags;

	write_wav(path, 3.0);
	t = track_new(path, path, 48000);
	atomic_store(&t->analysed, true);	/* skip tempo, too short */
	g_assert_cmpint(decoder_run(t), ==, 0);
	g_assert_cmpint(atomic_load(&t->state), ==, TRACK_READY);
	/* resampled from 44.1k to 48k */
	g_assert_cmpfloat(fabs((double)track_frames(t) - 3.0 * 48000), <,
			  100.0);
	track_unref(t);

	g_assert_cmpint(decoder_probe(path, &tags), ==, 0);
	g_assert_cmpfloat(fabs(tags.duration - 3.0), <, 0.1);
	media_tags_clear(&tags);

	t = track_new("/nonexistent/file.mp3", NULL, 48000);
	g_assert_cmpint(decoder_run(t), ==, -1);
	g_assert_cmpint(atomic_load(&t->state), ==, TRACK_FAILED);
	track_unref(t);

	g_unlink(path);
	g_free(path);
}

static void test_sc_parse(void)
{
	static const char json[] =
		"{\"collection\":[{\"id\":123,\"title\":\"Song\","
		"\"duration\":30000,\"full_duration\":200000,"
		"\"genre\":\"Techno\",\"policy\":\"ALLOW\","
		"\"permalink_url\":\"https://soundcloud.com/x/y\","
		"\"track_authorization\":\"abc\","
		"\"user\":{\"username\":\"DJ X\"},"
		"\"media\":{\"transcodings\":["
		"{\"url\":\"https://a/hls\",\"snipped\":false,"
		"\"format\":{\"protocol\":\"hls\",\"mime_type\":\"audio/mpeg\"}},"
		"{\"url\":\"https://a/prog\",\"snipped\":false,"
		"\"format\":{\"protocol\":\"progressive\","
		"\"mime_type\":\"audio/mpeg\"}},"
		"{\"url\":\"https://a/drm\",\"snipped\":false,"
		"\"format\":{\"protocol\":\"encrypted-hls\","
		"\"mime_type\":\"audio/mpeg\"}}]}},"
		"{\"id\":5,\"kind\":\"track\"},"
		"{\"track\":{\"id\":9,\"title\":\"Liked\",\"policy\":\"SNIP\","
		"\"media\":{\"transcodings\":[]}}}]}";
	GError *err = NULL;
	GPtrArray *res = sc_parse_tracks(json, &err);
	PdMediaItem *m;

	g_assert_no_error(err);
	g_assert_nonnull(res);
	g_assert_cmpuint(res->len, ==, 2);
	m = res->pdata[0];
	g_assert_cmpstr(m->key, ==, "soundcloud:123");
	g_assert_cmpstr(m->title, ==, "Song");
	g_assert_cmpstr(m->artist, ==, "DJ X");
	g_assert_cmpstr(m->location, ==, "https://a/prog");
	g_assert_cmpfloat(m->duration, ==, 200.0);
	g_assert_false(m->preview);
	m = res->pdata[1];
	g_assert_cmpstr(m->title, ==, "Liked");
	g_assert_true(m->preview);
	g_assert_null(m->location);
	g_ptr_array_unref(res);

	res = sc_parse_tracks("not json", &err);
	g_assert_null(res);
	g_assert_nonnull(err);
	g_clear_error(&err);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/track/append", test_track_append);
	g_test_add_func("/analyze/128", test_tempo_128);
	g_test_add_func("/analyze/174", test_tempo_174);
	g_test_add_func("/analyze/95", test_tempo_95);
	g_test_add_func("/analyze/fold", test_bpm_fold);
	g_test_add_func("/analyze/key", test_key);
	g_test_add_func("/fx/all", test_fx);
	g_test_add_func("/library/rekordbox", test_rekordbox);
	g_test_add_func("/sampler/oneshot", test_sampler);
	g_test_add_func("/analyze/gain", test_gain);
	g_test_add_func("/dsp/biquad", test_biquad);
	g_test_add_func("/deck/play", test_deck_play);
	g_test_add_func("/deck/seek-loading", test_seek_while_loading);
	g_test_add_func("/deck/sync", test_sync);
	g_test_add_func("/decoder/wav", test_decoder);
	g_test_add_func("/soundcloud/parse", test_sc_parse);
	return g_test_run();
}
