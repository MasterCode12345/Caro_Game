/*
 * client/fake.h — offline mode (CARO_FAKE=1): a scripted server inside the client.
 * Owner: Nghĩa.
 */
#ifndef CARO_CLIENT_FAKE_H
#define CARO_CLIENT_FAKE_H

#include <glib.h>

void fake_start(void);                   /* "connection" opened: fresh fake session */
void fake_stop(void);                    /* "connection" closed: drop every undelivered message */
void fake_handle(const char *payload);   /* a request from net_request() */

/* Delivers msg to dispatch() after delay_ms, from the main loop. Messages come out in order of
 * their due time, ties in the order they were pushed — like bytes on one TCP connection. */
guint fake_push(guint delay_ms, const char *msg);

/* Test hook (Ctrl+I / Ctrl+U in main.c): an idle fake player invites us (CHALLENGE_FROM).
 * withdraw: the inviter cancels it after 5 s. FALSE if not in the lobby or nobody is idle. */
gboolean fake_simulate_invite(gboolean withdraw);

#endif /* CARO_CLIENT_FAKE_H */
