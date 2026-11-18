/*
 * server/log.c — server.log, one line per request.
 * Owner: Hân.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S0): open server.log in append mode at start-up, close it at exit.
 * TODO(S0): one line per request: time, IP:port, username or "-", command, status code;
 *           flush after each line.
 * TODO(S0): log malformed frames (docs/protocol.md §1) before the session ends.
 * Never log passwords (REGISTER / LOGIN: log the command name only).
 */
