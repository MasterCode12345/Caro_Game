/*
 * server/history.c — HISTORY, REPLAY, LEADERBOARD (docs/protocol.md section 6.4).
 * Owner: Hân.
 *
 * All three are read-only queries answered from the database (db.c). The response of each is
 * built into a local buffer first and sent with ONE reply(), so the response always goes out
 * whole and before anything else.
 */
#include "history.h"
#include "db.h"
#include "match.h"
#include "net.h"
#include "protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A page number far beyond any real database is a bad argument, not an empty page; it also
 * keeps (page - 1) * 10 far away from any overflow. */
#define HISTORY_MAX_PAGE 1000000

/* snprintf at the end of buf: appends to buf[*used], returns 0, or -1 if it does not fit. */
static int append(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);

    if (len < 0 || (size_t)len >= cap - *used)
        return -1;

    *used += (size_t)len;
    return 0;
}

/* HISTORY or HISTORY|page: the caller's games, newest first, HISTORY_PAGE_SIZE per page.
 * 200|HISTORY|page|n|mid,opponent,my_symbol,outcome,reason,ended_at|... */
void handle_history(Client *c, char *fields[], int nfields)
{
    long page = 1;

    if (nfields >= 2 &&
        (!parse_long(fields[1], &page) || page < 1 || page > HISTORY_MAX_PAGE)) {
        reply(c, ST_BAD_REQUEST, CMD_HISTORY, ERR_BAD_ARGS);
        return;
    }

    HistoryRow rows[HISTORY_PAGE_SIZE];
    int n = db_history(c->user_id, (int)page, rows, HISTORY_PAGE_SIZE);

    if (n < 0) {
        reply(c, ST_SERVER_ERROR, CMD_HISTORY, ERR_SERVER_ERROR);
        return;
    }

    char body[FRAME_MAX_PAYLOAD / 2];
    size_t used = 0;
    int bad = append(body, sizeof(body), &used, "%ld|%d", page, n);

    for (int i = 0; i < n && !bad; i++)
        bad = append(body, sizeof(body), &used, "|%d,%s,%s,%s,%s,%s",
                     rows[i].match_id, rows[i].opponent, rows[i].my_symbol,
                     rows[i].outcome, rows[i].reason, rows[i].ended_at);

    if (bad) {
        reply(c, ST_SERVER_ERROR, CMD_HISTORY, ERR_SERVER_ERROR);
        return;
    }

    reply(c, ST_OK, CMD_HISTORY, "%s", body);
}

/* REPLAY|mid: any logged-in user may replay any match.
 * 200|REPLAY|mid|size|x_user|o_user|result|reason|n|r,c|r,c|...   (moves in play order) */
void handle_replay(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    long mid;

    if (!parse_long(fields[1], &mid)) {
        reply(c, ST_BAD_REQUEST, CMD_REPLAY, ERR_BAD_ARGS);
        return;
    }

    ReplayHeader head;
    int found = (mid >= 1 && mid <= 2147483647L) ? db_replay_header((int)mid, &head) : 0;

    if (found < 0) {
        reply(c, ST_SERVER_ERROR, CMD_REPLAY, ERR_SERVER_ERROR);
        return;
    }

    if (found == 0) {
        reply(c, ST_NOT_FOUND, CMD_REPLAY, ERR_MATCH_NOT_FOUND);
        return;
    }

    int moves[MAX_MOVES][2];
    int n = db_replay_moves(head.match_id, moves, MAX_MOVES);

    if (n < 0) {
        reply(c, ST_SERVER_ERROR, CMD_REPLAY, ERR_SERVER_ERROR);
        return;
    }

    /* 361 moves x 8 bytes + the fixed part is far below the 16 KB frame limit; the 8 KB
     * buffer is still checked on every append. */
    char body[FRAME_MAX_PAYLOAD / 2];
    size_t used = 0;
    int bad = append(body, sizeof(body), &used, "%d|%d|%s|%s|%s|%s|%d",
                     head.match_id, head.board_size, head.x_user, head.o_user,
                     head.result, head.reason, n);

    for (int i = 0; i < n && !bad; i++)
        bad = append(body, sizeof(body), &used, "|%d,%d", moves[i][0], moves[i][1]);

    if (bad) {
        reply(c, ST_SERVER_ERROR, CMD_REPLAY, ERR_SERVER_ERROR);
        return;
    }

    reply(c, ST_OK, CMD_REPLAY, "%s", body);
}

/* LEADERBOARD or LEADERBOARD|n: n defaults to LEADERBOARD_DEFAULT, at most LEADERBOARD_MAX
 * (a bigger n is cut to the maximum), n < 1 or not a number -> 400.
 * 200|LEADERBOARD|k|rank,user,elo,wins,losses,draws|... */
void handle_leaderboard(Client *c, char *fields[], int nfields)
{
    long n = LEADERBOARD_DEFAULT;

    if (nfields >= 2 && (!parse_long(fields[1], &n) || n < 1)) {
        reply(c, ST_BAD_REQUEST, CMD_LEADERBOARD, ERR_BAD_ARGS);
        return;
    }

    if (n > LEADERBOARD_MAX)
        n = LEADERBOARD_MAX;

    RankRow rows[LEADERBOARD_MAX];
    int k = db_leaderboard((int)n, rows);

    if (k < 0) {
        reply(c, ST_SERVER_ERROR, CMD_LEADERBOARD, ERR_SERVER_ERROR);
        return;
    }

    char body[FRAME_MAX_PAYLOAD / 2];
    size_t used = 0;
    int bad = append(body, sizeof(body), &used, "%d", k);

    for (int i = 0; i < k && !bad; i++)
        bad = append(body, sizeof(body), &used, "|%d,%s,%d,%d,%d,%d",
                     i + 1, rows[i].username, rows[i].elo, rows[i].wins,
                     rows[i].losses, rows[i].draws);

    if (bad) {
        reply(c, ST_SERVER_ERROR, CMD_LEADERBOARD, ERR_SERVER_ERROR);
        return;
    }

    reply(c, ST_OK, CMD_LEADERBOARD, "%s", body);
}
