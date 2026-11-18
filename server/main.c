/*
 * server/main.c — entry point of caro_server.
 * Owner: Hân.
 *
 * Responsibility: parse argv, open the listening socket, run the single select() loop over
 * the listening socket and every client, call tick(now) after every loop iteration.
 * One process, one thread, I/O multiplexing with select() (Lec05) — see CLAUDE.md "Server"
 * for why not fork() and not threads.
 *
 * SKELETON: only prints a message. Stages (CLAUDE.md Roadmap):
 * TODO(S0): `./build/caro_server [port]`, default DEFAULT_PORT (common/proto.h); validate the
 *           port with strtol + end pointer.
 * TODO(S0): signal(SIGPIPE, SIG_IGN) at the start of main() (MSG_NOSIGNAL is 0 on macOS).
 * TODO(S0): socket() + bind() + listen(), checking every return value.
 * TODO(S0): select() loop with a 1 s timeout: re-initialise the fd_sets and the struct timeval
 *           on every iteration; handle EINTR; listening fd readable -> accept (net.c);
 *           client fd readable -> receive (net.c); then tick(now).
 * TODO(S0): open server.log at start-up (log.c).
 * TODO(S1): open the SQLite database at start-up (db.c), create the tables if missing.
 * TODO(S2): tick(now): challenge expiry (lobby.c).
 * TODO(S7): tick(now): matchmaking pass (lobby.c) and move timers -> TIMEOUT (match.c).
 * TODO(S8): free everything and close the DB/log on exit so valgrind is clean.
 */
#include <stdio.h>

int main(void)
{
    printf("caro_server: not implemented yet\n");
    return 0;
}
