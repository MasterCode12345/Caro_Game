/*
 * server/main.c — entry point of caro_server.
 * Owner: Hân.
 *
 * Uses one process and one select() loop instead of fork() or threads.
 * select() lets one process monitor the listening socket and multiple client
 * sockets concurrently. This avoids creating a separate process/thread for
 * every client and keeps shared server state in one place.
 *
 * S0: listener, select() loop, framing, message handling and tick().
 * S1: open SQLite database at startup.
 */
#include "server.h"
#include "net.h"
#include "protocol.h"
#include "log.h"
#include "db.h"
#include "lobby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <netinet/in.h>
#include <arpa/inet.h>

Client clients[MAX_CLIENTS];

/* Timer tasks, run after every select() round (select has a 1 s timeout, so at most ~1 s
 * late). S2: challenge expiry. S7 will add matchmaking and the move timer. */
static void tick(time_t now)
{
    lobby_tick(now);
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

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    const char *db_path = "caro.db";
    int listenfd;
    int opt = 1;
    struct sockaddr_in server_addr;

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

    signal(SIGPIPE, SIG_IGN);

    /* Initialize every client slot as free. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    log_open("server.log");

    /* S1: initialize SQLite database. */
    if (db_open(db_path) != 0) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        return EXIT_FAILURE;
    }

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        db_close();
        return EXIT_FAILURE;
    }

    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listenfd);
        db_close();
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(listenfd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(listenfd);
        db_close();
        return EXIT_FAILURE;
    }

    if (listen(listenfd, 16) < 0) {
        perror("listen");
        close(listenfd);
        db_close();
        return EXIT_FAILURE;
    }

    printf("Caro server listening on port %d\n", port);
    printf("Database: %s\n", db_path);

    /* Main select() loop. */
    while (1) {
        fd_set readfds;
        int maxfd = listenfd;
        struct timeval timeout;
        int ready;

        FD_ZERO(&readfds);
        FD_SET(listenfd, &readfds);

        /* Add every active client socket to the set. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                if (clients[i].fd >= FD_SETSIZE) {
                    session_end(&clients[i]);
                    continue;
                }

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
                continue;
            }

            perror("select");
            break;
        }

        /* A new connection is waiting. */
        if (FD_ISSET(listenfd, &readfds)) {
            accept_client(listenfd);
        }

        /* Process readable clients. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];

            if (c->fd < 0) {
                continue;
            }

            if (!FD_ISSET(c->fd, &readfds)) {
                continue;
            }

            int recv_result = frame_recv(&c->in, c->fd);

            if (recv_result <= 0) {
                session_end(c);
                continue;
            }

            char msg[FRAME_MAX_PAYLOAD + 1];

            /* Drain every complete frame in the receive buffer. */
            while (c->fd >= 0) {
                int frame_result = frame_next(
                    &c->in, msg, sizeof(msg)
                );

                if (frame_result == 0) {
                    break; /* Need more bytes. */
                }

                if (frame_result < 0) {
                    session_end(c);
                    break;
                }

                handle_message(c, msg);
            }
        }

        /* Timers: challenge expiry (S2); matchmaking and move clock follow in S7. */
        tick(time(NULL));
    }

    close(listenfd);
    db_close();
    return EXIT_FAILURE;
}