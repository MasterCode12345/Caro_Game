/*
 * tools/cli.c — interactive line client for manual protocol testing (both members use it).
 * Usage (from C0): ./build/cli <host> <port>
 * Owner: Nghĩa.
 *
 * SKELETON: only prints a message.
 * TODO(C0): connect with getaddrinfo() + connect() (Lec03); signal(SIGPIPE, SIG_IGN).
 * TODO(C0): select() on stdin and the socket together: each typed line -> one frame;
 *           each received frame printed at once as "<< 200|PING".
 * TODO(C0): special commands for testing the server:
 *             /bytes <text>              one frame sent one byte at a time, 50 ms apart
 *             /burst <a> ;; <b> ;; <c>   several frames in a single send()
 *             /zero                      a frame with LENGTH 0 (malformed)
 *             /huge                      a header announcing LENGTH 20000 (malformed)
 *             /quit
 * TODO(C0): server closed the connection -> print a message and exit.
 */
#include <stdio.h>

int main(void)
{
    printf("cli: not implemented yet (TODO C0)\n");
    return 0;
}
