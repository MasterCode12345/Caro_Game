/*
 * client/net.h — the client's only connection to the server.
 * Owner: Nghĩa.
 */
#ifndef CARO_CLIENT_NET_H
#define CARO_CLIENT_NET_H

#include <glib.h>

#include "dispatch.h"   /* ResponseCb, RESP_DISCONNECTED */

/* Connects (or, in fake mode, pretends to). On failure returns FALSE and sets *err_out to a
 * message the caller must g_free(). Closes any previous connection first. */
gboolean net_connect(const char *host, int port, char **err_out);

gboolean net_is_connected(void);
gboolean net_is_connected_to(const char *host, int port);

/* Sends one request; cb gets its response later, from the main loop — never from inside
 * net_request() itself. Returns FALSE (and cb is never called) if not connected or if the
 * payload is empty or longer than FRAME_MAX_PAYLOAD. */
gboolean net_request(const char *payload, ResponseCb cb, gpointer user_data);

/* Closes the connection on purpose (e.g. after LOGOUT). Waiting callbacks get RESP_DISCONNECTED. */
void net_close(void);

/* The connection died: net_close() + back to the login page with "Mất kết nối tới server". */
void net_on_lost(void);

#endif /* CARO_CLIENT_NET_H */
