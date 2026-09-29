/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * samplerview.h - eight sample pads
 */
#ifndef PD_UI_SAMPLERVIEW_H
#define PD_UI_SAMPLERVIEW_H

#include <gtk/gtk.h>

#include "app.h"

G_BEGIN_DECLS

#define PD_TYPE_SAMPLER_VIEW (pd_sampler_view_get_type())
G_DECLARE_FINAL_TYPE(PdSamplerView, pd_sampler_view, PD, SAMPLER_VIEW,
		     GtkBox)

GtkWidget *pd_sampler_view_new(struct app *app);

G_END_DECLS

#endif /* PD_UI_SAMPLERVIEW_H */
