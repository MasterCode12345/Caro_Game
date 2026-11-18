/*
 * tools/test_frame.c — unit tests for common/frame.c over socketpair(AF_UNIX, SOCK_STREAM).
 * Built as build/test_frame, run by `make test`.
 * Owner: Nghĩa.
 *
 * SKELETON: no tests yet.
 * TODO(C0): a) one frame written one byte at a time -> frame_next() returns 1 only when the
 *              frame is complete, with the right content;
 *           b) 3 frames in one write() -> all 3 come out, in order;
 *           c) LENGTH 0 and LENGTH 16385 -> frame_next() returns -1;
 *           d) msg_split("200|LIST_PLAYERS|2|han,1016,IDLE|minh,980,IN_GAME") -> 5 fields.
 * TODO(C0): print PASS/FAIL per test; exit code non-zero if any test fails.
 */
#include <stdio.h>

int main(void)
{
    printf("test_frame: no tests yet (TODO C0)\n");
    return 0;
}
