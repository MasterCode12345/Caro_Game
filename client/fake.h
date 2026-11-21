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

/* Test hook (Ctrl+D/R/L/K/T on the game page, main.c): the fake opponent or the fake server
 * does something on its own. FALSE if it makes no sense now (e.g. a draw offer after the end). */
typedef enum {
    FAKE_OPP_DRAW_OFFER,     /* DRAW_OFFERED (game running, no offer pending) */
    FAKE_OPP_REMATCH,        /* REMATCH_REQUESTED (game ended, opponent still there) */
    FAKE_OPP_LEAVE,          /* LEAVE_GAME: running -> GAME_END RESIGN + OPP_LEFT; ended -> OPP_LEFT */
    FAKE_OPP_DISCONNECT,     /* lost connection: running -> GAME_END DISCONNECT; ended -> OPP_LEFT */
    FAKE_TIMEOUT             /* the player to move runs out of time: GAME_END TIMEOUT */
} FakeOpponentAction;
gboolean fake_simulate_opponent(FakeOpponentAction action);

#endif /* CARO_CLIENT_FAKE_H */
