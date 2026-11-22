/*
 * server/match.c — MOVE / LEAVE handlers, game_start(), game_end()
 * (docs/protocol.md §6.3, §7, §8).
 * Owner: Hân.
 *
 * S3: struct Game table, game_start(), MOVE with the checks in the exact order of §6.3,
 *     game_end(), LEAVE_GAME (checks the gid, a running game is ended by RESIGN first).
 * Remaining stages (CLAUDE.md Roadmap):
 * TODO(S4): RESIGN|gid -> GAME_END reason RESIGN (433 on an ended game).
 * TODO(S4): DRAW_OFFER|gid (409 offer_pending) -> opponent gets DRAW_OFFERED|gid;
 *           DRAW_REPLY|gid|ACCEPT -> GAME_END DRAW / DRAW_AGREED;
 *           DRAW_REPLY|gid|DECLINE -> offerer gets DRAW_DECLINED|gid; 404 no_pending_offer.
 * TODO(S4): REMATCH|gid (ended game; 404 opponent_left, 409 offer_pending) ->
 *           REMATCH_REQUESTED|gid; REMATCH_REPLY|gid|ACCEPT -> new gid, X and O swapped,
 *           GAME_START to both; DECLINE -> requester gets REMATCH_DECLINED|gid.
 * TODO(S4): disconnect (game_detach() from session_end()): running game -> opponent wins,
 *           reason DISCONNECT; ended game -> OPP_LEFT|gid.
 * TODO(S5): game_end() rates every finished game (incl. RESIGN, TIMEOUT, DISCONNECT) with
 *           elo.c, stores the match and its moves (db.c), PLAYER_STATUS with the new ELO.
 * TODO(S7): move timer from tick(): the player to move loses, reason TIMEOUT, after
 *           sec_per_move seconds.
 *
 * Same rule as lobby.c: every send can fail, and a failed send ends THAT client's session,
 * which calls game_detach(). So the game is fully updated BEFORE anything is sent, and after a
 * send the code re-reads the players from the Game instead of trusting older pointers.
 */

#include "match.h"
#include "lobby.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

static Game games[MAX_CLIENTS];    /* a client is in at most one game: 100 slots are enough */
static int next_gid = 1;           /* game ids grow from 1 */

/* ---------------------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------------------- */

/* Decimal integer, whole string consumed, no overflow. 1 = ok. */
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

static Game *alloc_game(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (games[i].id == 0)
            return &games[i];
    }
    return NULL;
}

/* The other player of g, or NULL if he left. */
static Client *opponent_of(const Game *g, const Client *c)
{
    return (g->x == c) ? g->o : g->x;
}

/* Take c out of g; free the slot when both players are gone. */
static void game_remove_player(Game *g, Client *c)
{
    if (g->x == c)
        g->x = NULL;
    if (g->o == c)
        g->o = NULL;
    if (c->game == g)
        c->game = NULL;

    if (g->x == NULL && g->o == NULL)
        memset(g, 0, sizeof(*g));       /* id = 0: free */
}

/* ELO change with an explicit sign: "+16", "-16", "0". */
static void fmt_delta(int delta, char *out, size_t cap)
{
    if (delta > 0)
        snprintf(out, cap, "+%d", delta);
    else
        snprintf(out, cap, "%d", delta);
}

/* ---------------------------------------------------------------------------------------
 * game_start / game_end / game_detach
 * ------------------------------------------------------------------------------------- */

/* Start a game between x (symbol X) and o (symbol O). */
void game_start(Client *x, Client *o, int size, int sec)
{
    if (x == NULL || o == NULL || x == o || x->fd < 0 || o->fd < 0)
        return;

    /* Entering a game cancels every other pending challenge of both players. A failed send
     * inside may end somebody's session, so re-check the sockets afterwards. */
    challenges_drop_for(x);
    challenges_drop_for(o);

    if (x->fd < 0 || o->fd < 0)
        return;

    Game *g = alloc_game();

    if (g == NULL)                      /* cannot happen: 100 slots for 100 clients */
        return;

    memset(g, 0, sizeof(*g));
    g->id = next_gid++;
    board_init(&g->board, size);
    g->x = x;
    g->o = o;
    g->turn = CELL_X;                   /* X moves first */
    g->ended = 0;
    g->sec = sec;
    g->started_at = time(NULL);
    g->deadline = g->started_at + sec;

    x->game = g;
    o->game = g;
    x->state = SESSION_IN_GAME;
    o->state = SESSION_IN_GAME;

    /* Copy what we need first: send_event() can call session_end(), which wipes the struct. */
    int gid = g->id;
    char xname[USERNAME_MAX + 1], oname[USERNAME_MAX + 1];
    snprintf(xname, sizeof(xname), "%s", x->username);
    snprintf(oname, sizeof(oname), "%s", o->username);
    int xelo = x->elo, oelo = o->elo;

    send_event(x, EV_GAME_START "|%d|%d|" SYM_X "|%s|%d|%d", gid, size, oname, oelo, sec);
    send_event(o, EV_GAME_START "|%d|%d|" SYM_O "|%s|%d|%d", gid, size, xname, xelo, sec);

    if (x->fd >= 0)
        broadcast_status(x, xname, PS_IN_GAME, xelo);
    if (o->fd >= 0)
        broadcast_status(o, oname, PS_IN_GAME, oelo);
}

void game_end(Game *g, char result, const char *reason, const int line[5][2])
{
    if (g == NULL || g->id == 0 || g->ended)
        return;

    g->ended = 1;                       /* update the game first, send afterwards */
    g->draw_offer_by = 0;

    const char *res = (result == CELL_X) ? RES_X_WIN :
                      (result == CELL_O) ? RES_O_WIN : RES_DRAW;
    int gid = g->id;
    Client *player[2] = { g->x, g->o };

    /* The five winning cells are sent only for FIVE. Copy them: line may point into the
     * caller's stack, which is fine, but a copy keeps this loop independent of it. */
    char cells[128] = "";
    if (line != NULL && strcmp(reason, END_FIVE) == 0) {
        size_t used = 0;
        for (int i = 0; i < 5; i++) {
            int n = snprintf(cells + used, sizeof(cells) - used, "|%d,%d",
                             line[i][0], line[i][1]);
            if (n < 0 || (size_t)n >= sizeof(cells) - used)
                break;
            used += (size_t)n;
        }
    }

    for (int i = 0; i < 2; i++) {
        Client *p = player[i];

        if (p == NULL || p->fd < 0)     /* left, or his session ended during this loop */
            continue;

        char delta[16];
        fmt_delta(0, delta, sizeof(delta));     /* TODO(S5): real ELO change */

        send_event(p, EV_GAME_END "|%d|%s|%s|%d|%s%s",
                   gid, res, reason, p->elo, delta, cells);
    }
}

void game_detach(Client *c)
{
    if (c == NULL || c->game == NULL)
        return;

    Game *g = c->game;
    int id = g->id;
    int was_running = !g->ended;

    /* session_end() has already set c->fd = -1, so game_end() skips c and only the opponent
     * is written to. c stays in the Game until the end, so a failed send to the opponent
     * (which detaches HIM) can never free g under our feet. */
    if (was_running) {
        Client *opp = opponent_of(g, c);

        if (opp != NULL)
            game_end(g, (g->x == c) ? CELL_O : CELL_X, END_DISCONNECT, NULL);
        else
            g->ended = 1;
    }

    if (g->id != id)                    /* cannot happen, but never touch a freed slot */
        return;

    g->draw_offer_by = 0;
    g->rematch_by = 0;                  /* a pending rematch dies with the player */

    Client *opp = opponent_of(g, c);

    game_remove_player(g, c);           /* c->game = NULL; frees g if nobody is left */

    /* An ended game: the opponent is told that c is gone (protocol section 8). A running game
     * already told him with GAME_END (DISCONNECT). */
    if (!was_running && opp != NULL && opp->fd >= 0)
        send_event(opp, EV_OPP_LEFT "|%d", id);
}

/* ---------------------------------------------------------------------------------------
 * MOVE|gid|row|col (IN_GAME)
 * ------------------------------------------------------------------------------------- */

void handle_move(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    long gid, row, col;

    /* 1. gid, row, col are valid integers. */
    if (!parse_long(fields[1], &gid) || !parse_long(fields[2], &row) ||
        !parse_long(fields[3], &col)) {
        reply(c, ST_BAD_REQUEST, CMD_MOVE, ERR_BAD_ARGS);
        return;
    }

    /* 2. the sender plays game gid. The game is found through c->game, never by trusting the
     *    number: a client can only ever touch its own game. */
    Game *g = c->game;

    if (g == NULL || g->id != gid || (g->x != c && g->o != c)) {
        reply(c, ST_FORBIDDEN, CMD_MOVE, ERR_NOT_IN_GAME);
        return;
    }

    /* 3. the game is running. */
    if (g->ended) {
        reply(c, ST_GAME_NOT_ACTIVE, CMD_MOVE, ERR_GAME_NOT_ACTIVE);
        return;
    }

    /* 4. it is the sender's turn. */
    char sym = (g->x == c) ? CELL_X : CELL_O;

    if (g->turn != sym) {
        reply(c, ST_NOT_YOUR_TURN, CMD_MOVE, ERR_NOT_YOUR_TURN);
        return;
    }

    /* 5. inside the board (compared as long: no cast of a huge number to int). */
    if (row < 0 || col < 0 || row >= g->board.size || col >= g->board.size) {
        reply(c, ST_OUT_OF_BOARD, CMD_MOVE, ERR_OUT_OF_BOARD);
        return;
    }

    /* 6. the cell is empty. */
    if (!board_is_empty(&g->board, (int)row, (int)col)) {
        reply(c, ST_CELL_OCCUPIED, CMD_MOVE, ERR_CELL_OCCUPIED);
        return;
    }

    /* 7. every check passed: only now does the board change. */
    board_place(&g->board, (int)row, (int)col, sym);

    int move_no = g->board.move_count;          /* first move = 1 */
    g->moves[move_no - 1][0] = (int)row;
    g->moves[move_no - 1][1] = (int)col;
    g->turn = (sym == CELL_X) ? CELL_O : CELL_X;
    g->deadline = time(NULL) + g->sec;
    g->draw_offer_by = 0;

    /* Decide the outcome now; the sends below may end sessions and detach players. */
    int line[5][2];
    int won = board_check_win(&g->board, (int)row, (int)col, line);
    int full = !won && board_is_full(&g->board);

    int id = g->id;
    Client *opp = opponent_of(g, c);

    /* Response first, then OPP_MOVE, then GAME_END (protocol section 3). */
    if (reply(c, ST_OK, CMD_MOVE, "%d", move_no) < 0) {
        /* c is gone and already detached; the opponent still has to hear about the move. */
        if (opp != NULL && opp->fd >= 0)
            send_event(opp, EV_OPP_MOVE "|%d|%d|%d|%d", id, (int)row, (int)col, move_no);
        return;
    }

    if (opp != NULL && opp->fd >= 0)
        send_event(opp, EV_OPP_MOVE "|%d|%d|%d|%d", id, (int)row, (int)col, move_no);

    /* The game may have been freed if both sessions ended during the sends. */
    if (g->id != id)
        return;

    if (won)
        game_end(g, sym, END_FIVE, (const int (*)[2])line);
    else if (full)
        game_end(g, 'D', END_BOARD_FULL, NULL);
}

/* ---------------------------------------------------------------------------------------
 * LEAVE_GAME|gid (IN_GAME)
 * ------------------------------------------------------------------------------------- */

void handle_leave_game(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    long gid;

    if (!parse_long(fields[1], &gid)) {
        reply(c, ST_BAD_REQUEST, CMD_LEAVE_GAME, ERR_BAD_ARGS);
        return;
    }

    Game *g = c->game;

    if (g == NULL || g->id != gid) {
        reply(c, ST_FORBIDDEN, CMD_LEAVE_GAME, ERR_NOT_IN_GAME);
        return;
    }

    int id = g->id;
    int was_running = !g->ended;

    if (reply(c, ST_OK, CMD_LEAVE_GAME, NULL) < 0)
        return;                         /* c is gone and already detached */

    /* Leaving a running game = resigning it: the opponent wins, both get GAME_END (§6.3). */
    if (was_running && g->id == id)
        game_end(g, (g->x == c) ? CELL_O : CELL_X, END_RESIGN, NULL);

    if (c->fd < 0 || g->id != id)
        return;                         /* c's session ended during game_end() */

    Client *opp = opponent_of(g, c);

    g->rematch_by = 0;                  /* a rematch request of c dies with him */
    game_remove_player(g, c);           /* c->game = NULL; frees g if nobody is left */

    c->state = SESSION_LOBBY;
    broadcast_status(c, c->username, PS_IDLE, c->elo);

    if (opp != NULL && opp->fd >= 0)
        send_event(opp, EV_OPP_LEFT "|%d", id);
}


/* ---------------------------------------------------------------------------------------
 * S4: RESIGN, DRAW_OFFER, DRAW_REPLY, REMATCH, REMATCH_REPLY
 *
 * Every handler follows the same order: validate -> change the Game -> response to the sender
 * -> events. After the response (a send that can fail) the handler checks again that the
 * sender and the Game are still alive before it sends anything else.
 * ------------------------------------------------------------------------------------- */

/* Checks 1 and 2 shared by all five: gid is a number (400 bad_args) and c plays game gid
 * (403 not_in_game). Sends the error itself and returns NULL, or returns the Game. */
static Game *game_of(Client *c, const char *gid_text, const char *cmd)
{
    long gid;

    if (!parse_long(gid_text, &gid)) {
        reply(c, ST_BAD_REQUEST, cmd, ERR_BAD_ARGS);
        return NULL;
    }

    Game *g = c->game;

    if (g == NULL || g->id != gid || (g->x != c && g->o != c)) {
        reply(c, ST_FORBIDDEN, cmd, ERR_NOT_IN_GAME);
        return NULL;
    }

    return g;
}

/* ACCEPT -> 1, DECLINE -> 0, anything else -> -1. */
static int parse_answer(const char *s)
{
    if (strcmp(s, ARG_ACCEPT) == 0)
        return 1;
    if (strcmp(s, ARG_DECLINE) == 0)
        return 0;
    return -1;
}

static char symbol_of(const Game *g, const Client *c)
{
    return (g->x == c) ? CELL_X : CELL_O;
}

/* The client that plays symbol sym, or NULL if he left. */
static Client *player_with(const Game *g, char sym)
{
    return (sym == CELL_X) ? g->x : g->o;
}

/* RESIGN|gid: the opponent wins. */
void handle_resign(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    Game *g = game_of(c, fields[1], CMD_RESIGN);

    if (g == NULL)
        return;

    if (g->ended) {
        reply(c, ST_GAME_NOT_ACTIVE, CMD_RESIGN, ERR_GAME_NOT_ACTIVE);
        return;
    }

    int id = g->id;
    char winner = (symbol_of(g, c) == CELL_X) ? CELL_O : CELL_X;

    /* If this send fails, c is detached and game_detach() already ended the game. */
    if (reply(c, ST_OK, CMD_RESIGN, NULL) < 0)
        return;

    if (g->id == id)
        game_end(g, winner, END_RESIGN, NULL);
}

/* DRAW_OFFER|gid: ask the opponent for a draw (one offer at a time). */
void handle_draw_offer(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    Game *g = game_of(c, fields[1], CMD_DRAW_OFFER);

    if (g == NULL)
        return;

    if (g->ended) {
        reply(c, ST_GAME_NOT_ACTIVE, CMD_DRAW_OFFER, ERR_GAME_NOT_ACTIVE);
        return;
    }

    if (g->draw_offer_by != 0) {
        reply(c, ST_CONFLICT, CMD_DRAW_OFFER, ERR_OFFER_PENDING);
        return;
    }

    int id = g->id;

    g->draw_offer_by = symbol_of(g, c);

    if (reply(c, ST_OK, CMD_DRAW_OFFER, NULL) < 0)
        return;                         /* c is gone: the game was ended by game_detach() */

    if (g->id != id)
        return;

    Client *opp = opponent_of(g, c);

    if (opp != NULL && opp->fd >= 0)
        send_event(opp, EV_DRAW_OFFERED "|%d", id);
}

/* DRAW_REPLY|gid|ACCEPT|DECLINE: answer the OPPONENT's offer. */
void handle_draw_reply(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    int accept = parse_answer(fields[2]);

    if (accept < 0) {
        reply(c, ST_BAD_REQUEST, CMD_DRAW_REPLY, ERR_BAD_ARGS);
        return;
    }

    Game *g = game_of(c, fields[1], CMD_DRAW_REPLY);

    if (g == NULL)
        return;

    if (g->ended) {
        reply(c, ST_GAME_NOT_ACTIVE, CMD_DRAW_REPLY, ERR_GAME_NOT_ACTIVE);
        return;
    }

    /* There must be an offer, and it must come from the other player (not from c himself). */
    if (g->draw_offer_by == 0 || g->draw_offer_by == symbol_of(g, c)) {
        reply(c, ST_NOT_FOUND, CMD_DRAW_REPLY, ERR_NO_PENDING_OFFER);
        return;
    }

    int id = g->id;
    Client *offerer = player_with(g, g->draw_offer_by);

    if (!accept)
        g->draw_offer_by = 0;           /* update first, send afterwards */

    if (reply(c, ST_OK, CMD_DRAW_REPLY, NULL) < 0)
        return;

    if (g->id != id)
        return;

    if (accept) {
        game_end(g, 'D', END_DRAW_AGREED, NULL);
        return;
    }

    offerer = player_with(g, (symbol_of(g, c) == CELL_X) ? CELL_O : CELL_X);   /* re-read */

    if (offerer != NULL && offerer->fd >= 0)
        send_event(offerer, EV_DRAW_DECLINED "|%d", id);
}

/* REMATCH|gid: ask for another game. Only after the game ended. */
void handle_rematch(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    Game *g = game_of(c, fields[1], CMD_REMATCH);

    if (g == NULL)
        return;

    if (!g->ended) {                    /* a running game cannot have a rematch */
        reply(c, ST_FORBIDDEN, CMD_REMATCH, ERR_FORBIDDEN_STATE);
        return;
    }

    if (opponent_of(g, c) == NULL) {
        reply(c, ST_NOT_FOUND, CMD_REMATCH, ERR_OPPONENT_LEFT);
        return;
    }

    if (g->rematch_by != 0) {
        reply(c, ST_CONFLICT, CMD_REMATCH, ERR_OFFER_PENDING);
        return;
    }

    int id = g->id;

    g->rematch_by = symbol_of(g, c);

    if (reply(c, ST_OK, CMD_REMATCH, NULL) < 0)
        return;                         /* c is gone and detached: the request died with him */

    if (g->id != id)
        return;

    Client *opp = opponent_of(g, c);

    if (opp != NULL && opp->fd >= 0)
        send_event(opp, EV_REMATCH_REQUESTED "|%d", id);
}

/* REMATCH_REPLY|gid|ACCEPT|DECLINE: answer the opponent's rematch request. */
void handle_rematch_reply(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    int accept = parse_answer(fields[2]);

    if (accept < 0) {
        reply(c, ST_BAD_REQUEST, CMD_REMATCH_REPLY, ERR_BAD_ARGS);
        return;
    }

    Game *g = game_of(c, fields[1], CMD_REMATCH_REPLY);

    if (g == NULL)
        return;

    if (!g->ended) {
        reply(c, ST_FORBIDDEN, CMD_REMATCH_REPLY, ERR_FORBIDDEN_STATE);
        return;
    }

    if (opponent_of(g, c) == NULL) {
        reply(c, ST_NOT_FOUND, CMD_REMATCH_REPLY, ERR_OPPONENT_LEFT);
        return;
    }

    if (g->rematch_by == 0 || g->rematch_by == symbol_of(g, c)) {
        reply(c, ST_NOT_FOUND, CMD_REMATCH_REPLY, ERR_NO_PENDING_OFFER);
        return;
    }

    int id = g->id;
    int size = g->board.size;
    int sec = g->sec;

    g->rematch_by = 0;                  /* update first, send afterwards */

    if (reply(c, ST_OK, CMD_REMATCH_REPLY, NULL) < 0)
        return;                         /* c is gone: the old game gets OPP_LEFT for the other */

    if (g->id != id)
        return;

    if (!accept) {
        Client *requester = opponent_of(g, c);

        if (requester != NULL && requester->fd >= 0)
            send_event(requester, EV_REMATCH_DECLINED "|%d", id);
        return;
    }

    /* ACCEPT: the old game is freed and a new one starts with X and O swapped. */
    Client *old_x = g->x;
    Client *old_o = g->o;

    if (old_x == NULL || old_o == NULL || old_x->fd < 0 || old_o->fd < 0)
        return;

    game_remove_player(g, old_x);
    game_remove_player(g, old_o);       /* second call frees the slot */

    game_start(old_o, old_x, size, sec);
}