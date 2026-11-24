/*
 * server/server.h — shared server data: Client / Challenge / Game structs and the global tables.
 * Owner: Hân.
 *
 * struct Game is in match.h, the Challenge table in lobby.c, the Game table in match.c.
 */
#ifndef CARO_SERVER_H
#define CARO_SERVER_H

#include <time.h>

#include "../common/frame.h"
#include "../common/proto.h"

#define MAX_CLIENTS 100

struct Game;                        /* match.h */

typedef struct Client {
    int fd;                         /* -1 = free slot */
    char addr[64];                  /* IP:port */
    SessionState state;             /* common/proto.h */

    /* account data, valid when state is not SESSION_CONNECTED */
    int user_id;
    char username[USERNAME_MAX + 1];
    int elo;
    int wins;
    int losses;
    int draws;

    /* receive buffer for TCP framing */
    FrameBuf in;

    /* current game (running or ended), NULL outside SESSION_IN_GAME */
    struct Game *game;

    /* when FIND_MATCH was received, only in SESSION_IN_QUEUE */
    time_t queued_at;
} Client;

/* A pending invitation (docs/protocol.md 6.2). Static table in lobby.c, ids from 1.
 * `used` = 0 means the slot is free. Pointers stay valid because clients[] is a static array. */
typedef struct {
    int id;
    Client *from;                   /* challenger: plays X */
    Client *to;                     /* target: the only one who may reply */
    int size;
    int sec;
    time_t expires;                 /* created + CHALLENGE_EXPIRE_SEC */
    int used;
} Challenge;

extern Client clients[MAX_CLIENTS];

#endif /* CARO_SERVER_H */
