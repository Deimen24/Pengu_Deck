/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PD_NET_H
#define PD_NET_H

#include <glib.h>

#define NET_ERROR (net_error_quark())
GQuark net_error_quark(void);

enum net_error_code {
	NET_ERROR_TRANSPORT,
	NET_ERROR_HTTP,
};

void net_init(void);

/*
 * Blocking HTTP GET.  @auth, when not NULL, is sent as the Authorization
 * header.  Returns the body (NUL terminated) or NULL with @err set.  On
 * HTTP errors the status is stored in @status when not NULL.
 */
char *net_get(const char *url, const char *auth, long *status,
	      GError **err);

#endif /* PD_NET_H */
