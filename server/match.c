/*
 * server/match.c — MOVE / RESIGN / DRAW / REMATCH / LEAVE handlers, game_end()
 * (docs/protocol.md §6.3, §7, §8).
 * Owner: Hân.
 *
 * S2 (temporary): game_start() and LEAVE_GAME below are the minimum needed to test challenges.
 * S3 replaces game_start() with one that creates a real struct Game.
 * Remaining stages (CLAUDE.md Roadmap):
 * TODO(S3): game_start() must also create the struct Game (board, turn, deadline).
 * TODO(S3): MOVE|gid|row|col (G), checks in the exact order of §6.3: strtol + end pointer
 *           (400 bad_args), sender plays game gid (403 not_in_game), then game.c (433, 430,
 *           431, 432). Success: 200|MOVE|move_no to the mover, then OPP_MOVE|gid|row|col|move_no
 *           to the opponent, then GAME_END to both if the game is over. A move clears a
 *           pending draw offer.
 * TODO(S3): game_end(): mark the game ended, send each player their own
 *           GAME_END|gid|result|reason|new_elo|elo_delta (+ 5 "r,c" fields only for FIVE);
 *           both players stay IN_GAME until LEAVE_GAME or rematch.
 * TODO(S4): RESIGN|gid -> GAME_END reason RESIGN (433 on an ended game).
 * TODO(S4): DRAW_OFFER|gid (409 offer_pending) -> opponent gets DRAW_OFFERED|gid;
 *           DRAW_REPLY|gid|ACCEPT -> GAME_END DRAW / DRAW_AGREED;
 *           DRAW_REPLY|gid|DECLINE -> offerer gets DRAW_DECLINED|gid; 404 no_pending_offer.
 * TODO(S4): REMATCH|gid (ended game; 404 opponent_left, 409 offer_pending) ->
 *           REMATCH_REQUESTED|gid; REMATCH_REPLY|gid|ACCEPT -> new gid, X and O swapped,
 *           GAME_START to both; DECLINE -> requester gets REMATCH_DECLINED|gid.
 * TODO(S4): LEAVE_GAME|gid -> 200, state LOBBY; if still running, GAME_END (RESIGN) to both
 *           first; then OPP_LEFT|gid to the opponent.
 * TODO(S4): disconnect (from session_end()): running game -> opponent wins, reason DISCONNECT;
 *           ended game -> OPP_LEFT|gid.
 * TODO(S5): game_end() rates every finished game (incl. RESIGN, TIMEOUT, DISCONNECT) with
 *           elo.c, stores the match and its moves (db.c), PLAYER_STATUS with the new ELO.
 * TODO(S7): move timer from tick(): the player to move loses, reason TIMEOUT, after
 *           sec_per_move seconds.
 */

#include "match.h"
#include "lobby.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int next_gid = 1;       /* game ids grow from 1 */

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

    int gid = next_gid++;

    /* Copy what we need first: send_event() can call session_end(), which wipes the struct. */
    char xname[USERNAME_MAX + 1], oname[USERNAME_MAX + 1];
    snprintf(xname, sizeof(xname), "%s", x->username);
    snprintf(oname, sizeof(oname), "%s", o->username);
    int xelo = x->elo, oelo = o->elo;

    x->state = SESSION_IN_GAME;
    o->state = SESSION_IN_GAME;

    send_event(x, EV_GAME_START "|%d|%d|" SYM_X "|%s|%d|%d", gid, size, oname, oelo, sec);
    send_event(o, EV_GAME_START "|%d|%d|" SYM_O "|%s|%d|%d", gid, size, xname, xelo, sec);

    if (x->fd >= 0)
        broadcast_status(x, xname, PS_IN_GAME, xelo);
    if (o->fd >= 0)
        broadcast_status(o, oname, PS_IN_GAME, oelo);
}

/* LEAVE_GAME|gid — S2 stub: back to the lobby. S3/S4 check the gid, end a running game, etc. */
void handle_leave_game(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    char *end;
    errno = 0;
    long gid = strtol(fields[1], &end, 10);

    if (errno != 0 || end == fields[1] || *end != '\0' || gid < 1) {
        reply(c, ST_BAD_REQUEST, CMD_LEAVE_GAME, ERR_BAD_ARGS);
        return;
    }

    if (reply(c, ST_OK, CMD_LEAVE_GAME, NULL) < 0)
        return;

    c->game = NULL;
    c->state = SESSION_LOBBY;
    broadcast_status(c, c->username, PS_IDLE, c->elo);
}