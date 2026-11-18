/*
 * client/board_view.c — cairo board drawing, shared by the game page and the replay viewer.
 * Owner: Nghĩa.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C3): GtkDrawingArea; data: size, board[BOARD_SIZE_MAX][BOARD_SIZE_MAX] ('.', 'X', 'O'),
 *           last move, winning line (5 cells), interactive flag.
 * TODO(C3): draw the grid, X (two dark-red diagonals), O (blue circle), outline the last move,
 *           fill the 5 winning cells; cell size follows the widget size.
 * TODO(C3): button press (GDK_BUTTON_PRESS_MASK) -> row/col -> on_cell callback.
 * TODO(C5): read-only mode (interactive = FALSE) for the replay viewer.
 */
