// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * decoder.c - FFmpeg based decoding of local files and network streams
 *
 * Everything is converted to interleaved stereo s16 at the track rate and
 * appended to the track as it arrives, so playback of a SoundCloud stream
 * can start long before the download finishes.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "analyze.h"
#include "pd-build.h"
#include "decoder.h"

#define OUT_FRAMES	8192
#define USER_AGENT	"Mozilla/5.0 (X11; Linux x86_64) PenguDeck/" PD_VERSION

struct decode_ctx {
	struct track *t;
	AVFormatContext *fmt;
	AVCodecContext *codec;
	SwrContext *swr;
	AVPacket *pkt;
	AVFrame *frame;
	int stream;
	int16_t *out;
	int out_cap;
};

static int interrupt_cb(void *opaque)
{
	atomic_bool *cancel = opaque;

	return atomic_load(cancel);
}

static const char *dict_value(AVDictionary *d, const char *key)
{
	AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0);

	return e && e->value && *e->value ? e->value : NULL;
}

/* Look a tag up on the container first and fall back to the stream. */
static const char *tag(AVFormatContext *fmt, int stream, const char *key)
{
	const char *v = dict_value(fmt->metadata, key);

	if (!v && stream >= 0)
		v = dict_value(fmt->streams[stream]->metadata, key);
	return v;
}

int decoder_open_input(AVFormatContext **fmt, const char *uri,
		       atomic_bool *cancel)
{
	AVDictionary *opts = NULL;
	int ret;

	*fmt = avformat_alloc_context();
	if (!*fmt)
		return AVERROR(ENOMEM);
	if (cancel) {
		(*fmt)->interrupt_callback.callback = interrupt_cb;
		(*fmt)->interrupt_callback.opaque = cancel;
	}
	if (strstr(uri, "://")) {
		av_dict_set(&opts, "user_agent", USER_AGENT, 0);
		av_dict_set(&opts, "reconnect", "1", 0);
		av_dict_set(&opts, "reconnect_streamed", "1", 0);
		av_dict_set(&opts, "rw_timeout", "20000000", 0);
	}
	ret = avformat_open_input(fmt, uri, NULL, &opts);
	av_dict_free(&opts);
	return ret;
}

static int setup(struct decode_ctx *c)
{
	struct track *t = c->t;
	const AVCodec *dec = NULL;
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	AVStream *st;
	char *title;
	int ret;

	ret = decoder_open_input(&c->fmt, t->uri, &t->cancel);
	if (ret < 0)
		return ret;
	ret = avformat_find_stream_info(c->fmt, NULL);
	if (ret < 0)
		return ret;
	ret = av_find_best_stream(c->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
	if (ret < 0)
		return ret;
	c->stream = ret;
	st = c->fmt->streams[c->stream];

	c->codec = avcodec_alloc_context3(dec);
	if (!c->codec)
		return AVERROR(ENOMEM);
	ret = avcodec_parameters_to_context(c->codec, st->codecpar);
	if (ret < 0)
		return ret;
	ret = avcodec_open2(c->codec, dec, NULL);
	if (ret < 0)
		return ret;

	if (c->codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&c->codec->ch_layout,
					  c->codec->ch_layout.nb_channels);
	ret = swr_alloc_set_opts2(&c->swr, &stereo, AV_SAMPLE_FMT_S16,
				  t->rate, &c->codec->ch_layout,
				  c->codec->sample_fmt, c->codec->sample_rate,
				  0, NULL);
	if (ret < 0)
		return ret;
	ret = swr_init(c->swr);
	if (ret < 0)
		return ret;

	if (c->fmt->duration != AV_NOPTS_VALUE && c->fmt->duration > 0)
		atomic_store(&t->length_hint,
			     (size_t)((double)c->fmt->duration * t->rate /
				      AV_TIME_BASE));

	title = g_path_get_basename(t->uri);
	track_set_meta(t, title, NULL);
	g_free(title);
	track_set_meta(t, tag(c->fmt, c->stream, "title"),
		       tag(c->fmt, c->stream, "artist"));

	c->pkt = av_packet_alloc();
	c->frame = av_frame_alloc();
	c->out = av_malloc(OUT_FRAMES * 4);
	c->out_cap = OUT_FRAMES;
	if (!c->pkt || !c->frame || !c->out)
		return AVERROR(ENOMEM);
	return 0;
}

static int convert(struct decode_ctx *c, AVFrame *f)
{
	const uint8_t **in = f ? (const uint8_t **)f->extended_data : NULL;
	int in_n = f ? f->nb_samples : 0;
	int need = swr_get_out_samples(c->swr, in_n);
	uint8_t *out;
	int n;

	if (need > c->out_cap) {
		av_free(c->out);
		c->out = av_malloc((size_t)need * 4);
		c->out_cap = c->out ? need : 0;
		if (!c->out)
			return AVERROR(ENOMEM);
	}
	/* A NULL input flushes, so only loop when draining at the end. */
	do {
		out = (uint8_t *)c->out;
		n = swr_convert(c->swr, &out, c->out_cap, in, in_n);
		if (n < 0)
			return n;
		if (n > 0 && track_append(c->t, c->out, (size_t)n) < 0)
			return AVERROR(ENOSPC);
	} while (!f && n > 0);
	return 0;
}

static int drain_frames(struct decode_ctx *c)
{
	int ret;

	for (;;) {
		ret = avcodec_receive_frame(c->codec, c->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;
		ret = convert(c, c->frame);
		av_frame_unref(c->frame);
		if (ret < 0)
			return ret;
	}
}

static int decode_all(struct decode_ctx *c)
{
	int ret, errors = 0;

	while (!atomic_load(&c->t->cancel)) {
		ret = av_read_frame(c->fmt, c->pkt);
		if (ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return ret;
		if (c->pkt->stream_index != c->stream) {
			av_packet_unref(c->pkt);
			continue;
		}
		ret = avcodec_send_packet(c->codec, c->pkt);
		av_packet_unref(c->pkt);
		/* Tolerate a few corrupt packets, common in old MP3s. */
		if (ret < 0 && ret != AVERROR(EAGAIN) && ++errors > 50)
			return ret;
		ret = drain_frames(c);
		if (ret < 0 && ++errors > 50)
			return ret;
	}
	if (atomic_load(&c->t->cancel))
		return AVERROR_EXIT;

	avcodec_send_packet(c->codec, NULL);
	ret = drain_frames(c);
	if (ret < 0)
		return ret;
	return convert(c, NULL);
}

static void cleanup(struct decode_ctx *c)
{
	av_free(c->out);
	av_frame_free(&c->frame);
	av_packet_free(&c->pkt);
	swr_free(&c->swr);
	avcodec_free_context(&c->codec);
	avformat_close_input(&c->fmt);
}

static void analyse(struct track *t)
{
	double bpm, offset;

	if (atomic_load(&t->analysed))
		return;
	if (analyze_tempo(t, &bpm, &offset) == 0) {
		atomic_store(&t->beat_offset, offset);
		atomic_store(&t->bpm, bpm);
	}
	atomic_store(&t->analysed, true);
}

int decoder_run(struct track *t)
{
	struct decode_ctx c = { .t = t, .stream = -1 };
	char err[AV_ERROR_MAX_STRING_SIZE];
	int ret;

	ret = setup(&c);
	if (ret >= 0)
		ret = decode_all(&c);
	cleanup(&c);

	if (ret < 0) {
		if (ret == AVERROR_EXIT)
			track_fail(t, "Cancelled");
		else {
			av_strerror(ret, err, sizeof(err));
			track_fail(t, err);
		}
		return -1;
	}
	if (track_frames(t) == 0) {
		track_fail(t, "No audio found");
		return -1;
	}
	atomic_store(&t->state, TRACK_DECODED);
	analyse(t);
	atomic_store(&t->state, TRACK_READY);
	return 0;
}

static gpointer decoder_thread(gpointer data)
{
	struct track *t = data;

	decoder_run(t);
	track_unref(t);
	return NULL;
}

void decoder_start(struct track *t)
{
	GThread *th = g_thread_new("pd-decoder", decoder_thread, track_ref(t));

	g_thread_unref(th);
}

int decoder_probe(const char *path, struct media_tags *tags)
{
	AVFormatContext *fmt = NULL;
	const char *bpm;
	int stream;

	memset(tags, 0, sizeof(*tags));
	if (decoder_open_input(&fmt, path, NULL) < 0) {
		avformat_free_context(fmt);
		return -1;
	}
	if (fmt->duration == AV_NOPTS_VALUE || fmt->duration <= 0)
		avformat_find_stream_info(fmt, NULL);
	stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream < 0) {
		avformat_close_input(&fmt);
		return -1;
	}

	tags->title = g_strdup(tag(fmt, stream, "title"));
	tags->artist = g_strdup(tag(fmt, stream, "artist"));
	tags->album = g_strdup(tag(fmt, stream, "album"));
	tags->genre = g_strdup(tag(fmt, stream, "genre"));
	if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
		tags->duration = (double)fmt->duration / AV_TIME_BASE;
	bpm = tag(fmt, stream, "TBPM");
	if (!bpm)
		bpm = tag(fmt, stream, "bpm");
	if (bpm)
		tags->bpm = g_ascii_strtod(bpm, NULL);

	avformat_close_input(&fmt);
	return 0;
}

void media_tags_clear(struct media_tags *tags)
{
	g_free(tags->title);
	g_free(tags->artist);
	g_free(tags->album);
	g_free(tags->genre);
	memset(tags, 0, sizeof(*tags));
}
