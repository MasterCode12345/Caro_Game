/*
 * server/lobby.h — interface of lobby.c.
 * Owner: Hân.
 *
 * PLAYER_STATUS broadcast, LIST_PLAYERS, challenges, matchmaking, STATS.
 */
#ifndef CARO_SERVER_LOBBY_H
#define CARO_SERVER_LOBBY_H

#include "server.h"

/* Broadcast player status to every other logged-in client. */
void broadcast_status(const Client *exclude,
                     const char *username,
                     const char *status,
                     int elo);

/* Request handlers (signature of the command table in protocol.c). */
void handle_list_players(Client *c, char *fields[], int nfields);
void handle_challenge(Client *c, char *fields[], int nfields);
void handle_challenge_reply(Client *c, char *fields[], int nfields);
void handle_challenge_cancel(Client *c, char *fields[], int nfields);
/* FIND_MATCH (LOBBY -> IN_QUEUE) and CANCEL_MATCH (IN_QUEUE -> LOBBY). */
void handle_find_match(Client *c, char *fields[], int nfields);
void handle_cancel_match(Client *c, char *fields[], int nfields);
/* STATS or STATS|user (docs/protocol.md section 6.4). */
void handle_stats(Client *c, char *fields[], int nfields);
/* Remove every pending challenge that involves c (outgoing or incoming); the other side gets
 * CHALLENGE_CANCELLED|cid. Call when c enters a game or the queue, logs out or disconnects. */
void challenges_drop_for(Client *c);

/* Called once per select() loop from tick(): expire challenges older than
 * CHALLENGE_EXPIRE_SEC (both sides get CHALLENGE_EXPIRED|cid), and pair queued players at most
 * once per second. */
void lobby_tick(time_t now);

#endif /* CARO_SERVER_LOBBY_H */
