/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mixerview.h - two channel strips, crossfader, master and cue section
 */
#ifndef PD_UI_MIXERVIEW_H
#define PD_UI_MIXERVIEW_H

#include <gtk/gtk.h>

#include "app.h"

G_BEGIN_DECLS

#define PD_TYPE_MIXER_VIEW (pd_mixer_view_get_type())
G_DECLARE_FINAL_TYPE(PdMixerView, pd_mixer_view, PD, MIXER_VIEW, GtkBox)

GtkWidget *pd_mixer_view_new(struct app *app);
void pd_mixer_view_nudge_xfader(PdMixerView *v, double delta);
void pd_mixer_view_set_xfader(PdMixerView *v, double value);
void pd_mixer_view_set_decks(PdMixerView *v, int n);
void pd_mixer_view_apply_config(PdMixerView *v);
/* Same levels as pd_deck_view_set_compact(). */
void pd_mixer_view_set_compact(PdMixerView *v, int level);

G_END_DECLS

#endif /* PD_UI_MIXERVIEW_H */
