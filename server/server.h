/*
 * server/server.h — shared server data: Client / Challenge / Game structs and the global tables.
 * Owner: Hân.
 *
 * SKELETON: no declarations yet. Stages (CLAUDE.md Roadmap):
 * TODO(S0): Client: fd (-1 = free slot), FrameBuf (common/frame.h), peer "IP:port" for the log,
 *           SessionState (common/proto.h). A fixed client table (select() handles at most
 *           FD_SETSIZE descriptors).
 * TODO(S1): Client: logged-in username, elo, wins, losses, draws.
 * TODO(S2): Challenge: cid, challenger, target, size, sec, creation time; challenge table.
 * TODO(S3): Game: gid, the X and O players, the board (game.h), sec_per_move, ended flag;
 *           game table; link Client -> current game.
 * TODO(S4): Game: pending draw offer, pending rematch request, opponent-left flag.
 * TODO(S7): Client: time it joined the matchmaking queue; Game: deadline of the current move.
 */
#ifndef CARO_SERVER_H
#define CARO_SERVER_H

#endif /* CARO_SERVER_H */
