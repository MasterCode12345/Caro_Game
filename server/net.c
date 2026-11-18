/*
 * server/net.c — connection layer: accept, per-client receive, reply(), session_end().
 * Owner: Hân.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap, docs/protocol.md §1, §8):
 * TODO(S0): accept a client (retry on EINTR), take a free slot in the client table,
 *           SO_SNDTIMEO = 2 s (a client that stops reading cannot freeze the server),
 *           frame_init(), state = SESSION_CONNECTED, remember "IP:port" for the log.
 * TODO(S0): receive: frame_recv() once; 0 or -1 -> session_end(). Then drain frame_next()
 *           until it returns 0, passing each message to protocol.c. A malformed frame
 *           (LENGTH 0 or > FRAME_MAX_PAYLOAD) -> log it + session_end().
 * TODO(S0): reply(): build "code|COMMAND|data" with snprintf, frame_send(); a failed send()
 *           ends the session; write the request's line to server.log (log.c).
 * TODO(S0): session_end(c): close the fd and set c->fd = -1.
 * TODO(S1): event senders; session_end: logged-in user -> PLAYER_STATUS|user|OFFLINE|elo
 *           to the others.
 * TODO(S2): session_end: remove every pending challenge involving c, CHALLENGE_CANCELLED|cid
 *           to the other side.
 * TODO(S4): session_end: running game -> opponent wins, reason DISCONNECT;
 *           ended game -> OPP_LEFT|gid to the opponent (match.c).
 * TODO(S7): session_end: remove c from the matchmaking queue.
 * TODO(S8): malformed input and many clients (build/bot) never crash or block the server.
 */
