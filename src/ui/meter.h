/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * meter.h - stereo peak level meter with falling peak hold
 */
#ifndef PD_UI_METER_H
#define PD_UI_METER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PD_TYPE_METER (pd_meter_get_type())
G_DECLARE_FINAL_TYPE(PdMeter, pd_meter, PD, METER, GtkWidget)

GtkWidget *pd_meter_new(void);
/* Feed linear peak values (1.0 = full scale), call from a frame clock. */
void pd_meter_update(PdMeter *m, float left, float right);

G_END_DECLS

#endif /* PD_UI_METER_H */
