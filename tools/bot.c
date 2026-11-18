/*
 * tools/bot.c — N bots in one process, for load testing the server.
 * Usage (from C6): ./build/bot <host> <port> <N>
 * Owner: Nghĩa.
 *
 * SKELETON: only prints a message.
 * TODO(C6): open N connections in ONE process, one select() loop (same model as the server).
 * TODO(C6): each bot: REGISTER bot_<i> (ignore 409) -> LOGIN -> FIND_MATCH; on GAME_START keep
 *           its own board from 200|MOVE and OPP_MOVE and play a random empty cell on its turn;
 *           GAME_END -> LEAVE_GAME -> FIND_MATCH again.
 * TODO(C6): every 10 s print bots online, games finished, error responses received.
 * TODO(C6): Ctrl+C exits cleanly.
 */
#include <stdio.h>

int main(void)
{
    printf("bot: not implemented yet (TODO C6)\n");
    return 0;
}
