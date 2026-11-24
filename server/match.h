/*
 * server/match.h — interface of match.c: the Game struct, in-game handlers and game_end().
 * Owner: Hân.
 *
 * Reason tokens (END_FIVE, ...) are in common/proto.h.
 */
#ifndef CARO_SERVER_MATCH_H
#define CARO_SERVER_MATCH_H

#include "server.h"
#include "game.h"

#define MAX_MOVES (MAX_BOARD * MAX_BOARD)   /* 361 */

/* One game. A slot of the static table in match.c is free when id == 0.
 * x or o becomes NULL when that player left or disconnected; the Game is freed when both did. */
typedef struct Game {
    int id;
    Board board;
    Client *x, *o;
    char turn;                      /* 'X' or 'O': who must move now */
    int ended;                      /* 1 after game_end() */
    int sec;                        /* seconds per move */
    time_t deadline;                /* turn deadline: now + sec after every move, read by match_tick() */
    time_t started_at;
    int moves[MAX_MOVES][2];        /* (row, col) in play order, moves[0] = first move */
    int draw_offer_by;              /* 0, 'X' or 'O': who offered a draw */
    int rematch_by;                 /* 0, 'X' or 'O': who asked for a rematch */
} Game;

/* Start a game: x plays X (the challenger moves first), o plays O. Both become IN_GAME,
 * both get GAME_START, everybody else gets PLAYER_STATUS IN_GAME. */
void game_start(Client *x, Client *o, int size, int sec);

/* End a game. result: 'X' (X wins), 'O' (O wins) or 'D' (draw). line: the 5 winning cells,
 * only for reason END_FIVE, otherwise NULL. Rates the game and saves it (elo.c, db.c), then sends
 * GAME_END to each player still connected.
 * Both players stay IN_GAME until LEAVE_GAME. Does nothing if the game already ended. */
void game_end(Game *g, char result, const char *reason, const int line[5][2]);

/* Called by session_end(): c's connection is gone. A running game is won by the opponent (reason
 * DISCONNECT), after an ended one he gets OPP_LEFT. Removes c from its game and frees the Game
 * when nobody is left, so no pointer to a reset Client survives. */
void game_detach(Client *c);

/* MOVE|gid|row|col and LEAVE_GAME|gid (signature of the command table in protocol.c). */
void handle_move(Client *c, char *fields[], int nfields);
void handle_leave_game(Client *c, char *fields[], int nfields);

/* RESIGN|gid, DRAW_OFFER|gid, DRAW_REPLY|gid|ACCEPT|DECLINE, REMATCH|gid,
 * REMATCH_REPLY|gid|ACCEPT|DECLINE. */
void handle_resign(Client *c, char *fields[], int nfields);
void handle_draw_offer(Client *c, char *fields[], int nfields);
void handle_draw_reply(Client *c, char *fields[], int nfields);
void handle_rematch(Client *c, char *fields[], int nfields);
void handle_rematch_reply(Client *c, char *fields[], int nfields);

/* The move timer. Called from tick() once per select() round: every running game whose
 * deadline has passed ends with reason TIMEOUT, and the player who was NOT to move wins. */
void match_tick(time_t now);

#endif /* CARO_SERVER_MATCH_H */
