// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * miniaudio_impl.c - the single translation unit compiling miniaudio
 *
 * Only the Linux backends and the WAV encoder are built.
 */
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_JACK
#define MA_ENABLE_NULL
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_MP3
#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
