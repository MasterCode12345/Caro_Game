/*
 * client/fake.c — offline mode, `CARO_FAKE=1 ./build/caro_client`: a scripted server, so the UI
 * can be built before the real server feature exists.
 * Owner: Nghĩa.
 *
 * Replies follow docs/protocol.md and reach the UI exactly like real ones: net_request() has
 * already queued the callback, the reply goes through dispatch(), which pops the FIFO. Nothing
 * here touches a widget. Replies arrive FAKE_LATENCY_MS later through a main-loop timer
 * (g_timeout_add, the same kind of source as g_idle_add), which imitates network delay and makes
 * the "buttons locked while waiting" state visible.
 *
 * C1 scenarios:
 *   PING                        -> 200|PING
 *   REGISTER|taken|...          -> 409 username_taken      (any other name -> 201)
 *   LOGIN|<user>|wrong          -> 401 invalid_credentials
 *   LOGIN|online|...            -> 409 already_logged_in
 *   LOGIN|<user>|<other>        -> 200|LOGIN|<user>|1000|0|0|0
 *   LOGOUT                      -> 200 (states L, Q), 401 not_logged_in (state C)
 *   anything else               -> 400 unknown_command (not faked yet)
 *
 * TODO(C2): LIST_PLAYERS (3 fake players); PLAYER_STATUS of a new player after 3 s;
 *           CHALLENGE -> 200 then GAME_START after 2 s; a way to simulate CHALLENGE_FROM.
 * TODO(C3): MOVE -> 200 then OPP_MOVE on a random empty cell after 0.5 s; simple five-in-a-row
 *           check -> GAME_END.
 * TODO(C4): RESIGN, DRAW_OFFER/DRAW_REPLY, REMATCH/REMATCH_REPLY, LEAVE_GAME.
 * TODO(C5): HISTORY, REPLAY (one 20-move game), LEADERBOARD, FIND_MATCH / CANCEL_MATCH.
 */
#include "fake.h"

#include <stdarg.h>
#include <string.h>

#include "dispatch.h"
#include "frame.h"
#include "proto.h"

#define FAKE_LATENCY_MS  300

typedef struct {
    gint64 due_us;      /* g_get_monotonic_time() at which to deliver */
    char  *msg;
} FakeMsg;

static GQueue       outbox = G_QUEUE_INIT;   /* sorted by due_us */
static guint        timer_id;
static SessionState fstate;                  /* the fake server's view of our session */

/* ---- delivery ------------------------------------------------------------------------------------ */

static gboolean on_timer(gpointer data);

static void schedule_head(void)
{
    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }
    FakeMsg *m = g_queue_peek_head(&outbox);
    if (m == NULL)
        return;
    gint64 wait_us = m->due_us - g_get_monotonic_time();
    timer_id = g_timeout_add(wait_us > 0 ? (guint)((wait_us + 999) / 1000) : 0, on_timer, NULL);
}

static gboolean on_timer(gpointer data)
{
    (void)data;
    timer_id = 0;
    FakeMsg *m;
    while ((m = g_queue_peek_head(&outbox)) != NULL && m->due_us <= g_get_monotonic_time()) {
        g_queue_pop_head(&outbox);
        dispatch(m->msg);          /* the same entry point as a frame from the real socket */
        g_free(m->msg);
        g_free(m);
    }
    schedule_head();
    return G_SOURCE_REMOVE;
}

void fake_push(guint delay_ms, const char *msg)
{
    FakeMsg *m = g_new0(FakeMsg, 1);
    m->due_us = g_get_monotonic_time() + (gint64)delay_ms * 1000;
    m->msg = g_strdup(msg);

    /* Insert after the last message due at or before this one (stable for equal times). */
    GList *l = outbox.tail;
    while (l != NULL && ((FakeMsg *)l->data)->due_us > m->due_us)
        l = l->prev;
    if (l != NULL)
        g_queue_insert_after(&outbox, l, m);
    else
        g_queue_push_head(&outbox, m);
    schedule_head();
}

static void reply(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fake_push(FAKE_LATENCY_MS, msg);
    g_free(msg);
}

/* ---- session ------------------------------------------------------------------------------------- */

void fake_start(void)
{
    fake_stop();
    fstate = SESSION_CONNECTED;
}

void fake_stop(void)
{
    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }
    FakeMsg *m;
    while ((m = g_queue_pop_head(&outbox)) != NULL) {
        g_free(m->msg);
        g_free(m);
    }
    fstate = SESSION_CONNECTED;
}

/* ---- requests ------------------------------------------------------------------------------------ */

/* Username/password format is already checked by ui_login before sending; like the real
 * server, the fake only checks the field count here. */
static void fake_register(char **f, int n)
{
    if (fstate != SESSION_CONNECTED)
        reply("%d|%s|%s", ST_FORBIDDEN, CMD_REGISTER, ERR_FORBIDDEN_STATE);
    else if (n != 3)
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_REGISTER, ERR_BAD_ARGS);
    else if (strcmp(f[1], "taken") == 0)
        reply("%d|%s|%s", ST_CONFLICT, CMD_REGISTER, ERR_USERNAME_TAKEN);
    else
        reply("%d|%s", ST_CREATED, CMD_REGISTER);
}

static void fake_login(char **f, int n)
{
    if (fstate != SESSION_CONNECTED) {
        reply("%d|%s|%s", ST_FORBIDDEN, CMD_LOGIN, ERR_FORBIDDEN_STATE);
    } else if (n != 3) {
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_LOGIN, ERR_BAD_ARGS);
    } else if (strcmp(f[2], "wrong") == 0) {
        reply("%d|%s|%s", ST_NOT_AUTHENTICATED, CMD_LOGIN, ERR_INVALID_CREDENTIALS);
    } else if (strcmp(f[1], "online") == 0) {
        reply("%d|%s|%s", ST_CONFLICT, CMD_LOGIN, ERR_ALREADY_LOGGED_IN);
    } else {
        fstate = SESSION_LOBBY;
        reply("%d|%s|%s|%d|%d|%d|%d", ST_OK, CMD_LOGIN, f[1], ELO_START, 0, 0, 0);
    }
}

static void fake_logout(void)
{
    if (fstate == SESSION_CONNECTED) {
        reply("%d|%s|%s", ST_NOT_AUTHENTICATED, CMD_LOGOUT, ERR_NOT_LOGGED_IN);
    } else if (fstate == SESSION_IN_GAME) {
        reply("%d|%s|%s", ST_FORBIDDEN, CMD_LOGOUT, ERR_FORBIDDEN_STATE);
    } else {
        fstate = SESSION_CONNECTED;
        reply("%d|%s", ST_OK, CMD_LOGOUT);
    }
}

void fake_handle(const char *payload)
{
    char *copy = g_strdup(payload);
    char *f[MAX_FIELDS];
    int n = msg_split(copy, f, MAX_FIELDS);

    if (strcmp(f[0], CMD_PING) == 0)
        reply("%d|%s", ST_OK, CMD_PING);
    else if (strcmp(f[0], CMD_REGISTER) == 0)
        fake_register(f, n);
    else if (strcmp(f[0], CMD_LOGIN) == 0)
        fake_login(f, n);
    else if (strcmp(f[0], CMD_LOGOUT) == 0)
        fake_logout();
    else
        reply("%d|%s|%s", ST_BAD_REQUEST, f[0], ERR_UNKNOWN_COMMAND);

    g_free(copy);
}
