/*
 * server/match.h — interface of match.c: in-game handlers and game_end().
 * Owner: Hân.
 *
 * S2 (temporary): game_start() and LEAVE_GAME without a real Game struct.
 * TODO(S3): real struct Game, MOVE handler, game_end().
 * TODO(S4): RESIGN / DRAW_* / REMATCH* / full LEAVE_GAME handlers and the disconnect hook
 *           used by session_end().
 * TODO(S7): the move-timer check called from tick().
 */
#ifndef CARO_SERVER_MATCH_H
#define CARO_SERVER_MATCH_H

#include "server.h"

/* Start a game: x plays X (the challenger moves first), o plays O. Both become IN_GAME,
 * both get GAME_START, everybody else gets PLAYER_STATUS IN_GAME. */
void game_start(Client *x, Client *o, int size, int sec);

/* LEAVE_GAME|gid (S2 version: just goes back to the lobby; S4 makes it complete). */
void handle_leave_game(Client *c, char *fields[], int nfields);

#endif /* CARO_SERVER_MATCH_H */