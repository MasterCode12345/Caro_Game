/*
 * server/lobby.c — LIST_PLAYERS, PLAYER_STATUS, challenges, matchmaking (docs/protocol.md §6.2).
 * Owner: Hân.
 *
 * S2 (done): LIST_PLAYERS, CHALLENGE, CHALLENGE_REPLY, CHALLENGE_CANCEL, expiry from tick(),
 *            cleanup with challenges_drop_for().
 * TODO(S6): HISTORY, REPLAY, LEADERBOARD, STATS handlers (§6.4) — here or a new file, Hân decides.
 * TODO(S7): FIND_MATCH (L) -> 200, state Q; CANCEL_MATCH (Q) -> 200, state L.
 * TODO(S7): matchmaking pass every second from tick(): pair two queued players whose ELO gap
 *           <= MM_BASE_GAP + MM_STEP_GAP * floor(waited / MM_STEP_SEC), capped at MM_MAX_GAP;
 *           BOARD_SIZE, MATCH_SEC_PER_MOVE; the player who waited longer plays X.
 *
 * Why no mutex: the server is one thread in one select() loop, so exactly one handler runs at
 * a time. A handler can write to ANY client's socket (e.g. CHALLENGE_FROM to the target)
 * without locking because nothing else touches the tables while it runs.
 *
 * One rule used everywhere below: a send can fail, and a failed send calls session_end() for
 * THAT client, which calls challenges_drop_for() and edits the challenge table. So a challenge
 * is always removed from the table BEFORE any event about it is sent, and loops re-check
 * `used` and `fd` on every iteration instead of trusting values read earlier.
 */

#include "lobby.h"
#include "match.h"
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Broadcast status to all logged-in clients except the sender. */
void broadcast_status(const Client *exclude,
                     const char *username,
                     const char *status,
                     int elo)
{
    if (username == NULL || status == NULL)
        return;

    char event[128];

    int len = snprintf(event, sizeof(event),
                       "PLAYER_STATUS|%s|%s|%d",
                       username, status, elo);

    if (len < 0 || (size_t)len >= sizeof(event))
        return;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];

        if (c->fd < 0 || c == exclude)
            continue;

        /* Only clients who have logged in receive this event. */
        if (c->state != SESSION_LOBBY &&
            c->state != SESSION_IN_QUEUE &&
            c->state != SESSION_IN_GAME)
            continue;

        send_event(c, "%s", event);
    }
}

/* ---------------------------------------------------------------------------------------
 * S2: helpers
 * ------------------------------------------------------------------------------------- */

#define MAX_CHALLENGES MAX_CLIENTS      /* at most one outgoing challenge per client */

static Challenge challenges[MAX_CHALLENGES];
static int next_cid = 1;                /* challenge ids grow from 1 */

static int is_logged_in(const Client *c)
{
    return c->fd >= 0 &&
           (c->state == SESSION_LOBBY || c->state == SESSION_IN_QUEUE ||
            c->state == SESSION_IN_GAME);
}

const char *client_status(const Client *c)
{
    switch (c->state) {
    case SESSION_IN_QUEUE: return PS_IN_QUEUE;
    case SESSION_IN_GAME:  return PS_IN_GAME;
    default:               return PS_IDLE;
    }
}

/* Decimal integer, whole string consumed (strtol + end pointer), no overflow. */
static int parse_long(const char *s, long *out)
{
    if (s == NULL || *s == '\0')
        return 0;

    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);

    if (errno != 0 || *end != '\0')
        return 0;

    *out = v;
    return 1;
}

static Client *find_by_name(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (is_logged_in(&clients[i]) && strcmp(clients[i].username, name) == 0)
            return &clients[i];
    }
    return NULL;
}

static Challenge *find_challenge(int cid)
{
    for (int i = 0; i < MAX_CHALLENGES; i++) {
        if (challenges[i].used && challenges[i].id == cid)
            return &challenges[i];
    }
    return NULL;
}

static Challenge *find_outgoing(const Client *c)
{
    for (int i = 0; i < MAX_CHALLENGES; i++) {
        if (challenges[i].used && challenges[i].from == c)
            return &challenges[i];
    }
    return NULL;
}

static Challenge *alloc_challenge(void)
{
    for (int i = 0; i < MAX_CHALLENGES; i++) {
        if (!challenges[i].used)
            return &challenges[i];
    }
    return NULL;
}

static void challenge_remove(Challenge *ch)
{
    memset(ch, 0, sizeof(*ch));         /* used = 0 */
}

/* ---------------------------------------------------------------------------------------
 * LIST_PLAYERS (L, Q, G) -> 200|LIST_PLAYERS|n|user,elo,status|...
 * ------------------------------------------------------------------------------------- */

static int cmp_by_username(const void *a, const void *b)
{
    const Client *x = *(const Client * const *)a;
    const Client *y = *(const Client * const *)b;
    return strcmp(x->username, y->username);
}

void handle_list_players(Client *c, char *fields[], int nfields)
{
    (void)fields;
    (void)nfields;

    const Client *list[MAX_CLIENTS];
    int n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (&clients[i] != c && is_logged_in(&clients[i]))
            list[n++] = &clients[i];
    }

    qsort(list, (size_t)n, sizeof(list[0]), cmp_by_username);

    /* 100 players x ~35 bytes is far below the 16 KB frame limit. */
    char data[FRAME_MAX_PAYLOAD / 2];
    size_t used = 0;
    int len = snprintf(data, sizeof(data), "%d", n);
    used = (size_t)len;

    for (int i = 0; i < n; i++) {
        len = snprintf(data + used, sizeof(data) - used, "|%s,%d,%s",
                       list[i]->username, list[i]->elo, client_status(list[i]));
        if (len < 0 || (size_t)len >= sizeof(data) - used) {
            reply(c, ST_SERVER_ERROR, CMD_LIST_PLAYERS, ERR_SERVER_ERROR);
            return;
        }
        used += (size_t)len;
    }

    reply(c, ST_OK, CMD_LIST_PLAYERS, "%s", data);
}

/* ---------------------------------------------------------------------------------------
 * CHALLENGE|user|size|sec (L)
 * ------------------------------------------------------------------------------------- */

static int valid_sec(long sec)
{
    return sec == SEC_PER_MOVE_FAST || sec == SEC_PER_MOVE_NORMAL || sec == SEC_PER_MOVE_SLOW;
}

void handle_challenge(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    const char *target_name = fields[1];
    long size, sec;

    /* 400: not numbers, size != 15, sec not 15/30/60, or challenging yourself. */
    if (!parse_long(fields[2], &size) || !parse_long(fields[3], &sec) ||
        size != BOARD_SIZE || !valid_sec(sec) ||
        strcmp(target_name, c->username) == 0) {
        reply(c, ST_BAD_REQUEST, CMD_CHALLENGE, ERR_BAD_ARGS);
        return;
    }

    Client *target = find_by_name(target_name);

    if (target == NULL) {
        reply(c, ST_NOT_FOUND, CMD_CHALLENGE, ERR_USER_NOT_FOUND);
        return;
    }

    if (target->state != SESSION_LOBBY) {
        reply(c, ST_CONFLICT, CMD_CHALLENGE, ERR_PLAYER_BUSY);
        return;
    }

    if (find_outgoing(c) != NULL) {
        reply(c, ST_CONFLICT, CMD_CHALLENGE, ERR_CHALLENGE_PENDING);
        return;
    }

    Challenge *ch = alloc_challenge();

    if (ch == NULL) {
        reply(c, ST_SERVER_ERROR, CMD_CHALLENGE, ERR_SERVER_ERROR);
        return;
    }

    ch->id = next_cid++;
    ch->from = c;
    ch->to = target;
    ch->size = (int)size;
    ch->sec = (int)sec;
    ch->expires = time(NULL) + CHALLENGE_EXPIRE_SEC;
    ch->used = 1;

    int cid = ch->id;

    /* Response first, then the event (protocol.md section 3). If this send fails, c's session
     * ends and challenges_drop_for() has already removed the challenge: nothing more to do. */
    if (reply(c, ST_OK, CMD_CHALLENGE, "%d", cid) < 0)
        return;

    send_event(target, EV_CHALLENGE_FROM "|%d|%s|%d|%d|%d",
               cid, c->username, c->elo, (int)size, (int)sec);
}

/* ---------------------------------------------------------------------------------------
 * CHALLENGE_REPLY|cid|ACCEPT|DECLINE (L, target only)
 * ------------------------------------------------------------------------------------- */

void handle_challenge_reply(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    long cid;

    if (!parse_long(fields[1], &cid)) {
        reply(c, ST_BAD_REQUEST, CMD_CHALLENGE_REPLY, ERR_BAD_ARGS);
        return;
    }

    Challenge *ch = find_challenge((int)cid);

    /* Unknown id, or somebody other than the target: the same answer, so a client cannot
     * probe other players' challenge ids. */
    if (ch == NULL || ch->to != c) {
        reply(c, ST_NOT_FOUND, CMD_CHALLENGE_REPLY, ERR_CHALLENGE_NOT_FOUND);
        return;
    }

    int accept = strcmp(fields[2], ARG_ACCEPT) == 0;
    int decline = strcmp(fields[2], ARG_DECLINE) == 0;

    if (!accept && !decline) {
        reply(c, ST_BAD_REQUEST, CMD_CHALLENGE_REPLY, ERR_BAD_ARGS);
        return;
    }

    Client *from = ch->from;
    int size = ch->size, sec = ch->sec;
    int id = ch->id;

    if (decline) {
        challenge_remove(ch);
        if (reply(c, ST_OK, CMD_CHALLENGE_REPLY, NULL) < 0)
            return;
        send_event(from, EV_CHALLENGE_DECLINED "|%d|%s", id, c->username);
        return;
    }

    /* ACCEPT: the challenger may have left the lobby in the meantime. */
    challenge_remove(ch);

    if (from->fd < 0 || from->state != SESSION_LOBBY) {
        reply(c, ST_CONFLICT, CMD_CHALLENGE_REPLY, ERR_PLAYER_BUSY);
        return;
    }

    if (reply(c, ST_OK, CMD_CHALLENGE_REPLY, NULL) < 0)
        return;

    game_start(from, c, size, sec);       /* the challenger plays X */
}

/* ---------------------------------------------------------------------------------------
 * CHALLENGE_CANCEL|cid (L, challenger only)
 * ------------------------------------------------------------------------------------- */

void handle_challenge_cancel(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    long cid;

    if (!parse_long(fields[1], &cid)) {
        reply(c, ST_BAD_REQUEST, CMD_CHALLENGE_CANCEL, ERR_BAD_ARGS);
        return;
    }

    Challenge *ch = find_challenge((int)cid);

    if (ch == NULL || ch->from != c) {
        reply(c, ST_NOT_FOUND, CMD_CHALLENGE_CANCEL, ERR_CHALLENGE_NOT_FOUND);
        return;
    }

    Client *to = ch->to;
    int id = ch->id;

    challenge_remove(ch);

    if (reply(c, ST_OK, CMD_CHALLENGE_CANCEL, NULL) < 0)
        return;

    send_event(to, EV_CHALLENGE_CANCELLED "|%d", id);
}

/* ---------------------------------------------------------------------------------------
 * Cleanup and expiry
 * ------------------------------------------------------------------------------------- */

void challenges_drop_for(Client *c)
{
    for (int i = 0; i < MAX_CHALLENGES; i++) {
        Challenge *ch = &challenges[i];

        if (!ch->used || (ch->from != c && ch->to != c))
            continue;

        Client *other = (ch->from == c) ? ch->to : ch->from;
        int id = ch->id;

        challenge_remove(ch);           /* before sending: see the note at the top */

        if (other != NULL && other->fd >= 0)
            send_event(other, EV_CHALLENGE_CANCELLED "|%d", id);
    }
}

/* Called from tick() once per select() round (at most 1 s apart, thanks to the select timeout:
 * no thread or alarm needed). */
void lobby_tick(time_t now)
{
    for (int i = 0; i < MAX_CHALLENGES; i++) {
        Challenge *ch = &challenges[i];

        if (!ch->used || now < ch->expires)
            continue;

        Client *from = ch->from, *to = ch->to;
        int id = ch->id;

        challenge_remove(ch);

        if (from->fd >= 0)
            send_event(from, EV_CHALLENGE_EXPIRED "|%d", id);
        if (to->fd >= 0)
            send_event(to, EV_CHALLENGE_EXPIRED "|%d", id);
    }
}