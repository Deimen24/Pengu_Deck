/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * encoder.h - encode the master mix to a file or an Icecast server
 *
 * Built on libavformat, so the same code writes WAV, FLAC, MP3 and Opus
 * files and streams to icecast://source:password@host:port/mount.
 */
#ifndef PD_ENCODER_H
#define PD_ENCODER_H

#include <stdbool.h>

enum enc_format {
	ENC_WAV,
	ENC_FLAC,
	ENC_MP3,
	ENC_OPUS,
	ENC_COUNT,
};

const char *enc_format_name(enum enc_format f);
const char *enc_format_ext(enum enc_format f);
/* True when this FFmpeg build has the needed encoder. */
bool enc_format_available(enum enc_format f);

struct encoder;

/*
 * @url is a path or an icecast:// URL.  @bitrate_kbps applies to MP3
 * and Opus.  @meta may carry "title"/"description"/"genre" for streams.
 */
struct encoder *encoder_open(const char *url, enum enc_format f,
			     unsigned int rate, int bitrate_kbps,
			     const char *stream_name, char **err);
/* Interleaved stereo float frames; returns false once the sink broke. */
bool encoder_write(struct encoder *e, const float *frames, unsigned int n);
void encoder_close(struct encoder *e);

#endif /* PD_ENCODER_H */
