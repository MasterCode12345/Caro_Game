/*
 * server/main.c — entry point of caro_server.
 * Owner: Hân.
 *
 * Uses one process and one select() loop instead of fork() or threads.
 * select() lets one process monitor the listening socket and multiple client
 * sockets concurrently. This avoids creating a separate process/thread for
 * every client and keeps shared server state in one place.
 *
 * Start: signals, server.log, database, listening socket. Then the loop: select() -> accept ->
 * receive (net_receive in net.c) -> tick(). Ctrl+C or SIGTERM ends the loop and main() closes
 * everything before it returns 0.
 */
#include "server.h"
#include "net.h"
#include "log.h"
#include "db.h"
#include "lobby.h"
#include "match.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <netinet/in.h>
#include <arpa/inet.h>

Client clients[MAX_CLIENTS];

/* Set by the signal handler, read by the select() loop. sig_atomic_t: reading or writing it is
 * one indivisible step even if the signal arrives in the middle. volatile: the compiler must
 * really read it on every iteration, because it changes outside the normal flow of the code. */
static volatile sig_atomic_t stop_requested = 0;

/* SIGINT (Ctrl+C) and SIGTERM: ask the loop to stop, nothing else.
 *
 * A signal can arrive between any two instructions: inside printf(), inside malloc(), in the
 * middle of a SQLite transaction. Only async-signal-safe functions may be called here, and
 * printf(), fclose(), free() or sqlite3_close() are not: if the interrupted code is inside
 * malloc() and holds its lock, a handler that calls malloc() waits for that lock forever; if it
 * is in the middle of a database write, a handler that closes the database pulls it away under
 * SQLite's feet. So the handler only sets a flag, and main() does the cleanup as ordinary code,
 * at a point where no request and no transaction is in progress. */
static void on_stop_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

/* Install the stop handlers and ignore SIGPIPE. 0 ok, -1 error. */
static int install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;

    if (sigemptyset(&sa.sa_mask) < 0)
        return -1;

    /* No SA_RESTART: a select() that is waiting must return -1 / EINTR at once, so the loop
     * sees the flag immediately. (A signal that arrives just BEFORE select() starts waiting
     * interrupts nothing; the 1 second timeout then bounds the delay.) */
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    /* send() to a connection the peer closed must return EPIPE, not kill the server. */
    sa.sa_handler = SIG_IGN;

    return sigaction(SIGPIPE, &sa, NULL);
}

/* Timer tasks, run after every select() round. select() has a 1 second timeout, so this runs
 * at least once a second even when no client sends anything: that is the only clock the server
 * has (no thread, no alarm()). lobby_tick(): challenge expiry and the matchmaking pass.
 * match_tick(): the move timer. Everything is late by at most about one second. */
static void tick(time_t now)
{
    lobby_tick(now);
    match_tick(now);
}

static int parse_port(const char *arg, int *port)
{
    char *endptr;
    long value;

    errno = 0;
    value = strtol(arg, &endptr, 10);

    if (errno != 0 || endptr == arg || *endptr != '\0' ||
        value < 1 || value > 65535) {
        return -1;
    }

    *port = (int)value;
    return 0;
}

/* Server shutdown: close every client socket. Not session_end(): that would end each running
 * game as a DISCONNECT loss of whichever player has the lower slot, although nobody left. Games
 * still running when the server stops are dropped and not rated; finished ones are already in
 * the database (db_match_save() is called before GAME_END is sent). */
static void close_clients(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0)
            continue;

        if (close(clients[i].fd) < 0)
            log_event("close %s failed: %s", clients[i].addr, strerror(errno));

        clients[i].fd = -1;
    }
}

/* socket() + SO_REUSEADDR + bind() + listen(). Returns the listening socket, or -1. */
static int open_listener(int port)
{
    int opt = 1;
    struct sockaddr_in server_addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Restart at once on the same port, without waiting for old TIME_WAIT connections. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        goto fail;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);    /* every interface: LAN clients too */
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        goto fail;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        goto fail;
    }

    return fd;

fail:
    if (close(fd) < 0)
        perror("close");
    return -1;
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    const char *db_path = "caro.db";
    int listenfd;
    int exit_code = EXIT_SUCCESS;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [port] [db_path]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2 && parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (argc >= 3) {
        db_path = argv[2];
    }

    if (install_signals() < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    /* Initialize every client slot as free. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    /* Without a log file the server still works: every line also goes to stdout. */
    if (log_open("server.log") < 0)
        fprintf(stderr, "server.log cannot be opened, logging to stdout only\n");

    if (db_open(db_path) != 0) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        log_close();
        return EXIT_FAILURE;
    }

    listenfd = open_listener(port);

    if (listenfd < 0) {
        db_close();
        log_close();
        return EXIT_FAILURE;
    }

    printf("Caro server listening on port %d\n", port);
    printf("Database: %s\n", db_path);

    /* Main select() loop, until SIGINT or SIGTERM. */
    while (!stop_requested) {
        fd_set readfds;
        int maxfd = listenfd;
        struct timeval timeout;
        int ready;

        FD_ZERO(&readfds);
        FD_SET(listenfd, &readfds);

        /* Add every active client socket to the set. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &readfds);

                if (clients[i].fd > maxfd) {
                    maxfd = clients[i].fd;
                }
            }
        }

        /* select() modifies timeval, so reset it every iteration. */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;       /* a signal: the loop condition looks at stop_requested */
            }

            log_event("select failed: %s", strerror(errno));
            exit_code = EXIT_FAILURE;
            break;
        }

        /* A new connection is waiting. */
        if (FD_ISSET(listenfd, &readfds)) {
            accept_client(listenfd);
        }

        /* Receive from every readable client. A handler may end ANY session (a failed send to
         * the opponent, for example), so fd is checked again for each slot. A slot cannot be
         * reused inside this loop: connections are only accepted above. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];

            if (c->fd >= 0 && FD_ISSET(c->fd, &readfds)) {
                net_receive(c);
            }
        }

        tick(time(NULL));
    }

    /* Clean stop (or a fatal select() error). */
    close_clients();

    if (close(listenfd) < 0)
        log_event("close listening socket failed: %s", strerror(errno));

    db_close();
    log_close();                /* last line of server.log: "server stopped" */
    return exit_code;
}
