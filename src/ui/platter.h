/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * platter.h - jog wheel: spins with the track, drag to scratch
 */
#ifndef PD_UI_PLATTER_H
#define PD_UI_PLATTER_H

#include <gtk/gtk.h>

#include "deck.h"

G_BEGIN_DECLS

#define PD_TYPE_PLATTER (pd_platter_get_type())
G_DECLARE_FINAL_TYPE(PdPlatter, pd_platter, PD, PLATTER, GtkWidget)

GtkWidget *pd_platter_new(struct deck *d, const char *color, int size);

G_END_DECLS

#endif /* PD_UI_PLATTER_H */
