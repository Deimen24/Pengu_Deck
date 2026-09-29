/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * knob.h - rotary control, dragged vertically, double click resets
 */
#ifndef PD_UI_KNOB_H
#define PD_UI_KNOB_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PD_TYPE_KNOB (pd_knob_get_type())
G_DECLARE_FINAL_TYPE(PdKnob, pd_knob, PD, KNOB, GtkWidget)

GtkWidget *pd_knob_new(const char *label, double min, double max,
		       double def);
double pd_knob_get_value(PdKnob *k);
void pd_knob_set_value(PdKnob *k, double v);
/* Centre detent: the knob snaps to @def when close to it. */
void pd_knob_set_detent(PdKnob *k, gboolean on);
void pd_knob_set_accent(PdKnob *k, const char *hex);
/* Emitted as "changed" when the value changes through the widget. */

G_END_DECLS

#endif /* PD_UI_KNOB_H */
