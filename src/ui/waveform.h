/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * waveform.h - scrolling coloured waveform with beat grid and markers
 *
 * Two modes: the zoomed view scrolls under a fixed playhead and supports
 * scratching by dragging; the overview shows the whole track and seeks on
 * click.
 */
#ifndef PD_UI_WAVEFORM_H
#define PD_UI_WAVEFORM_H

#include <gtk/gtk.h>

#include "deck.h"

G_BEGIN_DECLS

#define PD_TYPE_WAVEFORM (pd_waveform_get_type())
G_DECLARE_FINAL_TYPE(PdWaveform, pd_waveform, PD, WAVEFORM, GtkWidget)

GtkWidget *pd_waveform_new(struct deck *d, gboolean overview);
/* Zoom in pixels per second of audio, zoomed view only. */
void pd_waveform_set_zoom(PdWaveform *w, double px_per_sec);
void pd_waveform_zoom_by(PdWaveform *w, double factor);
void pd_waveform_set_color(PdWaveform *w, const char *hex);

G_END_DECLS

#endif /* PD_UI_WAVEFORM_H */
