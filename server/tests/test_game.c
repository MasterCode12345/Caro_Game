/*
 * server/tests/test_game.c — unit tests for server/game.c (pure: no sockets, no DB).
 * Built as build/test_game, run by `make test`.
 * Owner: Hân.
 *
 * SKELETON: no tests yet.
 * TODO(S3): five in a row in all 4 directions (horizontal, vertical, both diagonals) -> FIVE;
 *           the 5 reported cells include the last move, also for a line longer than five.
 * TODO(S3): four in a row is not a win; a full board without five -> BOARD_FULL.
 * TODO(S3): rejected moves leave the board unchanged: not your turn (430), out of board (431,
 *           including negative row/col), cell occupied (432), game ended (433).
 * TODO(S3): print PASS/FAIL per test; return non-zero if any test fails.
 */
int main(void)
{
    return 0;
}
