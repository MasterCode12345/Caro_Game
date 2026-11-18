/*
 * server/game.c — board, move validation, win/draw (docs/protocol.md §6.3).
 * Owner: Hân.
 *
 * PURE: no sockets, no DB. Only this file changes a board. That is why it can be unit-tested
 * in server/tests/test_game.c (build/test_game).
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S3): new board: BOARD_SIZE, empty cells, X to move, move_no = 0; keep the moves in play
 *           order (needed later for the moves table and REPLAY).
 * TODO(S3): move validation in the exact order of §6.3 (first failure wins, the board never
 *           changes on error): game running (433), sender's turn (430),
 *           0 <= row, col < size (431), empty cell (432).
 * TODO(S3): apply: place the mark, move_no++ (first move = 1), switch the turn.
 * TODO(S3): result after a move: FIVE (WIN_LENGTH or more in a row through the last move, in
 *           one of 4 directions; report 5 consecutive cells that include the last move),
 *           BOARD_FULL (draw), or still running.
 */
