// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sccache.c - on disk cache of SoundCloud streams
 */
#include <stdatomic.h>
#include <string.h>

#include <glib/gstdio.h>
#include <libavformat/avformat.h>

#include "config.h"
#include "decoder.h"
#include "sccache.h"
#include "soundcloud.h"

#define WORKERS		2
#define EXT		".mka"

struct entry {
	char *key;
	char *path;
	atomic_int percent;	/* see sccache_progress() */
	PdMediaItem *item;	/* owned while in flight */
};

static struct app *app;
static char *cache_dir;
static GHashTable *entries;	/* key → struct entry, main thread */
static GThreadPool *pool;
static atomic_bool shutting_down;

static char *path_for(const char *key)
{
	const char *id = strrchr(key, ':');

	return g_strdup_printf("%s/%s" EXT, cache_dir, id ? id + 1 : key);
}

void sccache_init(struct app *a)
{
	char *base = config_cache_dir();

	app = a;
	cache_dir = g_build_filename(base, "soundcloud", NULL);
	g_mkdir_with_parents(cache_dir, 0755);
	g_free(base);
	entries = g_hash_table_new(g_str_hash, g_str_equal);
}

void sccache_shutdown(void)
{
	atomic_store(&shutting_down, true);
	if (pool) {
		g_thread_pool_free(pool, TRUE, TRUE);
		pool = NULL;
	}
}

char *sccache_lookup(PdMediaItem *m)
{
	char *path;

	if (m->source != MEDIA_SOUNDCLOUD || !cache_dir)
		return NULL;
	path = path_for(m->key);
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return path;
	g_free(path);
	return NULL;
}

/* ---- worker ------------------------------------------------------ */

static int remux(const char *url, const char *out, struct entry *e)
{
	AVFormatContext *in = NULL, *oc = NULL;
	AVStream *ist, *ost;
	AVPacket *pkt = av_packet_alloc();
	double duration;
	int stream, ret;

	ret = decoder_open_input(&in, url, &shutting_down);
	if (ret < 0)
		goto out;
	ret = avformat_find_stream_info(in, NULL);
	if (ret < 0)
		goto out;
	stream = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream < 0) {
		ret = stream;
		goto out;
	}
	ist = in->streams[stream];
	duration = in->duration > 0 ? (double)in->duration / AV_TIME_BASE :
				      0.0;

	ret = avformat_alloc_output_context2(&oc, NULL, "matroska", out);
	if (ret < 0)
		goto out;
	ost = avformat_new_stream(oc, NULL);
	if (!ost) {
		ret = AVERROR(ENOMEM);
		goto out;
	}
	ret = avcodec_parameters_copy(ost->codecpar, ist->codecpar);
	if (ret < 0)
		goto out;
	ost->codecpar->codec_tag = 0;
	ret = avio_open(&oc->pb, out, AVIO_FLAG_WRITE);
	if (ret < 0)
		goto out;
	ret = avformat_write_header(oc, NULL);
	if (ret < 0)
		goto out;

	while ((ret = av_read_frame(in, pkt)) >= 0) {
		if (pkt->stream_index != stream) {
			av_packet_unref(pkt);
			continue;
		}
		if (duration > 0.0 && pkt->pts != AV_NOPTS_VALUE) {
			double t = pkt->pts * av_q2d(ist->time_base);
			int pc = (int)(t / duration * 100.0);

			atomic_store(&e->percent, CLAMP(pc, 0, 99));
		}
		pkt->stream_index = 0;
		av_packet_rescale_ts(pkt, ist->time_base, ost->time_base);
		pkt->pos = -1;
		ret = av_interleaved_write_frame(oc, pkt);
		av_packet_unref(pkt);
		if (ret < 0)
			goto out;
	}
	ret = ret == AVERROR_EOF ? 0 : ret;
	if (ret == 0)
		ret = av_write_trailer(oc);
out:
	if (oc && oc->pb)
		avio_closep(&oc->pb);
	avformat_free_context(oc);
	avformat_close_input(&in);
	av_packet_free(&pkt);
	return ret;
}

struct done_msg {
	struct entry *e;
	int percent;
};

static gboolean fetch_done(gpointer data)
{
	struct done_msg *d = data;
	struct entry *e = d->e;
	PdMediaItem *m = e->item;

	atomic_store(&e->percent, d->percent);
	e->item = NULL;
	if (m) {
		app_item_changed(app, m);
		if (d->percent == 100)
			app_toast(app, "Cached \"%s\"", m->title);
		g_object_unref(m);
	}
	g_free(d);
	return G_SOURCE_REMOVE;
}

static void fetch_thread(gpointer data, gpointer user)
{
	struct entry *e = data;
	struct sc_auth *auth = app_sc_auth(app);
	struct done_msg *d = g_new0(struct done_msg, 1);
	char *part = g_strdup_printf("%s.part", e->path);
	char *url;
	int ret = -1;

	d->e = e;
	url = sc_stream_url(auth, e->item, NULL);
	if (url)
		ret = remux(url, part, e);
	if (ret == 0 && g_rename(part, e->path) == 0) {
		d->percent = 100;
	} else {
		g_unlink(part);
		d->percent = -2;
	}
	g_free(url);
	g_free(part);
	sc_auth_free(auth);
	if (!atomic_load(&shutting_down))
		g_idle_add(fetch_done, d);
	else
		g_free(d);
}

/* ---- main thread api --------------------------------------------- */

static struct entry *entry_for(PdMediaItem *m)
{
	struct entry *e = g_hash_table_lookup(entries, m->key);

	if (e)
		return e;
	e = g_new0(struct entry, 1);
	e->key = g_strdup(m->key);
	e->path = path_for(m->key);
	atomic_init(&e->percent,
		    g_file_test(e->path, G_FILE_TEST_IS_REGULAR) ? 100 : -1);
	g_hash_table_insert(entries, e->key, e);
	return e;
}

void sccache_fetch(PdMediaItem *m)
{
	struct entry *e;
	int pc;

	if (m->source != MEDIA_SOUNDCLOUD || !entries || !m->location)
		return;
	e = entry_for(m);
	pc = atomic_load(&e->percent);
	if (pc >= 0 || e->item)
		return;
	if (!pool)
		pool = g_thread_pool_new(fetch_thread, NULL, WORKERS, FALSE,
					 NULL);
	e->item = g_object_ref(m);
	atomic_store(&e->percent, 0);
	g_thread_pool_push(pool, e, NULL);
	app_item_changed(app, m);
}

int sccache_progress(PdMediaItem *m)
{
	if (m->source != MEDIA_SOUNDCLOUD || !entries)
		return -1;
	return atomic_load(&entry_for(m)->percent);
}

guint64 sccache_size(void)
{
	GDir *d = cache_dir ? g_dir_open(cache_dir, 0, NULL) : NULL;
	const char *name;
	guint64 total = 0;

	if (!d)
		return 0;
	while ((name = g_dir_read_name(d))) {
		char *p = g_build_filename(cache_dir, name, NULL);
		GStatBuf st;

		if (g_stat(p, &st) == 0)
			total += (guint64)st.st_size;
		g_free(p);
	}
	g_dir_close(d);
	return total;
}

void sccache_clear(void)
{
	GDir *d = cache_dir ? g_dir_open(cache_dir, 0, NULL) : NULL;
	GHashTableIter it;
	gpointer key, val;
	const char *name;

	if (!d)
		return;
	while ((name = g_dir_read_name(d))) {
		char *p = g_build_filename(cache_dir, name, NULL);

		if (g_str_has_suffix(name, EXT))
			g_unlink(p);
		g_free(p);
	}
	g_dir_close(d);
	g_hash_table_iter_init(&it, entries);
	while (g_hash_table_iter_next(&it, &key, &val)) {
		struct entry *e = val;

		if (!e->item)
			atomic_store(&e->percent, -1);
	}
}
