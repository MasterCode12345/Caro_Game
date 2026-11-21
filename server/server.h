/*
 * server/server.h — shared server data: Client / Challenge / Game structs and the global tables.
 * Owner: Hân.
 *
 * S0: Client table and shared client data for the select() loop.
 * Later stages extend Client with account, challenge, game and matchmaking data.
 */
#ifndef CARO_SERVER_H
#define CARO_SERVER_H

#include <time.h>

#include "../common/frame.h"
#include "../common/proto.h"

#define MAX_CLIENTS 100

/* Reuse SessionState from common/proto.h. */

/* Game will be implemented in later stages. */
struct Game;

typedef struct Client {
    int fd;                         /* -1 = free slot */
    char addr[64];                  /* IP:port */
    SessionState state;

    /* S1: account data */
    int user_id;
    char username[21];
    int elo;
    int wins;
    int losses;
    int draws;

    /* S0: receive buffer for TCP framing */
    FrameBuf in;

    /* S3: current game */
    struct Game *game;

    /* S7: matchmaking queue timestamp */
    time_t queued_at;
} Client;

extern Client clients[MAX_CLIENTS];

#endif /* CARO_SERVER_H */