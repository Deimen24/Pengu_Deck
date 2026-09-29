// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * encoder.c - encode the master mix to a file or an Icecast server
 */
#include <string.h>

#include <glib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "encoder.h"
#include "pd-build.h"

struct fmt_info {
	const char *name;
	const char *ext;
	const char *muxer;
	const char *codec;	/* encoder name */
	enum AVCodecID id;
};

static const struct fmt_info fmts[ENC_COUNT] = {
	[ENC_WAV] = { "WAV", "wav", "wav", "pcm_s16le", AV_CODEC_ID_PCM_S16LE },
	[ENC_FLAC] = { "FLAC", "flac", "flac", "flac", AV_CODEC_ID_FLAC },
	[ENC_MP3] = { "MP3", "mp3", "mp3", "libmp3lame", AV_CODEC_ID_MP3 },
	[ENC_OPUS] = { "Opus", "opus", "ogg", "libopus", AV_CODEC_ID_OPUS },
};

struct encoder {
	AVFormatContext *oc;
	AVCodecContext *cc;
	AVStream *st;
	SwrContext *swr;
	AVFrame *frame;
	AVPacket *pkt;
	int frame_size;
	int filled;		/* samples in frame */
	int64_t pts;
	bool broken;
	unsigned int rate;
};

const char *enc_format_name(enum enc_format f)
{
	return f >= 0 && f < ENC_COUNT ? fmts[f].name : "";
}

const char *enc_format_ext(enum enc_format f)
{
	return f >= 0 && f < ENC_COUNT ? fmts[f].ext : "wav";
}

static const AVCodec *find_encoder(enum enc_format f)
{
	const AVCodec *c = avcodec_find_encoder_by_name(fmts[f].codec);

	return c ? c : avcodec_find_encoder(fmts[f].id);
}

bool enc_format_available(enum enc_format f)
{
	return f >= 0 && f < ENC_COUNT && find_encoder(f) != NULL;
}

static enum AVSampleFormat pick_sample_fmt(const AVCodec *c)
{
	const enum AVSampleFormat *p = NULL;
	int n = 0;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
	avcodec_get_supported_config(NULL, c, AV_CODEC_CONFIG_SAMPLE_FORMAT,
				     0, (const void **)&p, &n);
#else
	p = c->sample_fmts;
	for (n = 0; p && p[n] != AV_SAMPLE_FMT_NONE; n++)
		;
#endif
	if (!p || n == 0)
		return AV_SAMPLE_FMT_S16;
	/* prefer float planar, then s16 */
	{
		int i;

		for (i = 0; i < n; i++)
			if (p[i] == AV_SAMPLE_FMT_FLTP || p[i] == AV_SAMPLE_FMT_FLT)
				return p[i];
	}
	return p[0];
}

static int flush_packets(struct encoder *e)
{
	int ret;

	for (;;) {
		ret = avcodec_receive_packet(e->cc, e->pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;
		av_packet_rescale_ts(e->pkt, e->cc->time_base, e->st->time_base);
		e->pkt->stream_index = e->st->index;
		ret = av_interleaved_write_frame(e->oc, e->pkt);
		av_packet_unref(e->pkt);
		if (ret < 0)
			return ret;
	}
}

struct encoder *encoder_open(const char *url, enum enc_format f,
			     unsigned int rate, int bitrate_kbps,
			     const char *stream_name, char **err)
{
	struct encoder *e = g_new0(struct encoder, 1);
	const AVCodec *codec = find_encoder(f);
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	AVDictionary *opts = NULL;
	char errbuf[AV_ERROR_MAX_STRING_SIZE];
	int ret;

	e->rate = rate;
	if (!codec) {
		*err = g_strdup_printf("This FFmpeg has no %s encoder",
				       fmts[f].name);
		goto fail;
	}
	ret = avformat_alloc_output_context2(&e->oc, NULL, fmts[f].muxer, url);
	if (ret < 0)
		goto averr;
	e->st = avformat_new_stream(e->oc, NULL);
	e->cc = avcodec_alloc_context3(codec);
	if (!e->st || !e->cc) {
		ret = AVERROR(ENOMEM);
		goto averr;
	}
	e->cc->sample_rate = (f == ENC_OPUS) ? 48000 : (int)rate;
	av_channel_layout_copy(&e->cc->ch_layout, &stereo);
	e->cc->sample_fmt = pick_sample_fmt(codec);
	e->cc->time_base = (AVRational){ 1, e->cc->sample_rate };
	if (f == ENC_MP3 || f == ENC_OPUS)
		e->cc->bit_rate = (int64_t)(bitrate_kbps > 0 ? bitrate_kbps :
					    192) * 1000;
	if (e->oc->oformat->flags & AVFMT_GLOBALHEADER)
		e->cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	ret = avcodec_open2(e->cc, codec, NULL);
	if (ret < 0)
		goto averr;
	ret = avcodec_parameters_from_context(e->st->codecpar, e->cc);
	if (ret < 0)
		goto averr;
	e->st->time_base = e->cc->time_base;

	ret = swr_alloc_set_opts2(&e->swr, &e->cc->ch_layout, e->cc->sample_fmt,
				  e->cc->sample_rate, &stereo,
				  AV_SAMPLE_FMT_FLT, (int)rate, 0, NULL);
	if (ret < 0 || (ret = swr_init(e->swr)) < 0)
		goto averr;

	e->frame_size = e->cc->frame_size > 0 ? e->cc->frame_size : 1024;
	e->frame = av_frame_alloc();
	e->pkt = av_packet_alloc();
	if (!e->frame || !e->pkt) {
		ret = AVERROR(ENOMEM);
		goto averr;
	}
	e->frame->nb_samples = e->frame_size;
	e->frame->format = e->cc->sample_fmt;
	e->frame->sample_rate = e->cc->sample_rate;
	av_channel_layout_copy(&e->frame->ch_layout, &e->cc->ch_layout);
	ret = av_frame_get_buffer(e->frame, 0);
	if (ret < 0)
		goto averr;

	if (stream_name && *stream_name) {
		av_dict_set(&e->oc->metadata, "title", stream_name, 0);
		av_dict_set(&opts, "ice_name", stream_name, 0);
	}
	av_dict_set(&opts, "content_type",
		    f == ENC_MP3 ? "audio/mpeg" : f == ENC_OPUS ?
		    "audio/ogg" : "application/octet-stream", 0);
	av_dict_set(&opts, "user_agent", "PenguDeck/" PD_VERSION, 0);
	if (!(e->oc->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open2(&e->oc->pb, url, AVIO_FLAG_WRITE, NULL,
				 &opts);
		if (ret < 0)
			goto averr;
	}
	ret = avformat_write_header(e->oc, NULL);
	if (ret < 0)
		goto averr;
	av_dict_free(&opts);
	return e;

averr:
	av_strerror(ret, errbuf, sizeof(errbuf));
	*err = g_strdup_printf("%s: %s", url, errbuf);
fail:
	av_dict_free(&opts);
	encoder_close(e);
	return NULL;
}

static bool send_frame(struct encoder *e, AVFrame *frame)
{
	int ret = avcodec_send_frame(e->cc, frame);

	if (ret < 0)
		return false;
	return flush_packets(e) >= 0;
}

static bool emit_frame(struct encoder *e, int samples)
{
	e->frame->nb_samples = samples;
	e->frame->pts = e->pts;
	e->pts += samples;
	if (!send_frame(e, e->frame)) {
		e->broken = true;
		return false;
	}
	e->filled = 0;
	return true;
}

/*
 * Convert @n input frames (NULL flushes the resampler) into the
 * encoder frame, sending it whenever it fills up, and keep pulling
 * until swresample's FIFO is drained.
 */
static bool convert(struct encoder *e, const float *frames, unsigned int n,
		    bool flush)
{
	const uint8_t *in = (const uint8_t *)frames;
	static const float none[2];
	int in_n = (int)n;
	int planes = av_sample_fmt_is_planar(e->cc->sample_fmt) ? 2 : 1;
	int bps = av_get_bytes_per_sample(e->cc->sample_fmt);

	for (;;) {
		uint8_t *out[AV_NUM_DATA_POINTERS];
		int want = e->frame_size - e->filled, got, c;

		if (av_frame_make_writable(e->frame) < 0)
			return false;
		for (c = 0; c < planes; c++)
			out[c] = e->frame->data[c] +
				 e->filled * bps * (planes == 1 ? 2 : 1);
		if (flush)
			got = swr_convert(e->swr, out, want, NULL, 0);
		else
			got = swr_convert(e->swr, out, want, in ? &in :
					  (const uint8_t *[]){ (const uint8_t *)none },
					  in_n);
		in = NULL;
		in_n = 0;
		if (got < 0)
			return false;
		e->filled += got;
		if (e->filled == e->frame_size && !emit_frame(e, e->frame_size))
			return false;
		if (got < want)
			return true;	/* FIFO drained */
	}
}

bool encoder_write(struct encoder *e, const float *frames, unsigned int n)
{
	if (!e || e->broken)
		return false;
	if (!convert(e, frames, n, false)) {
		e->broken = true;
		return false;
	}
	return true;
}

void encoder_close(struct encoder *e)
{
	if (!e)
		return;
	if (e->oc && e->cc && !e->broken && e->frame) {
		convert(e, NULL, 0, true);
		if (e->filled > 0)
			emit_frame(e, e->filled);
		send_frame(e, NULL);
		av_write_trailer(e->oc);
	}
	if (e->oc && e->oc->pb && !(e->oc->oformat->flags & AVFMT_NOFILE))
		avio_closep(&e->oc->pb);
	av_frame_free(&e->frame);
	av_packet_free(&e->pkt);
	swr_free(&e->swr);
	avcodec_free_context(&e->cc);
	avformat_free_context(e->oc);
	g_free(e);
}
