
/*
 * server/game.h — pure Caro board logic.
 * Owner: Hân.
 *
 * No sockets, database or networking dependencies.
 */
#ifndef CARO_GAME_H
#define CARO_GAME_H
#include <stddef.h>
#define MAX_BOARD 19

typedef char Cell;

/* Board cell values */
#define CELL_EMPTY '.'
#define CELL_X     'X'
#define CELL_O     'O'

typedef struct {
    int size;
    Cell cell[MAX_BOARD][MAX_BOARD];
    int move_count;
} Board;

/* Initialize a board of the given size. */
void board_init(Board *b, int size);

/* Return 1 if the coordinate is inside the active board, otherwise 0. */
int board_in_range(const Board *b, int row, int col);

/* Return 1 if the cell is empty and inside the active board. */
int board_is_empty(const Board *b, int row, int col);

/* Place a symbol on an empty valid cell. Invalid moves are ignored. */
void board_place(Board *b, int row, int col, char sym);

/*
 * Check whether the move at (row, col) creates five or more consecutive
 * symbols. If so, fill line with five consecutive coordinates containing
 * the last move and return 1. Otherwise return 0.
 */
int board_check_win(const Board *b, int row, int col, int line[5][2]);

/* Return 1 if the active board is full, otherwise 0. */
int board_is_full(const Board *b);

#endif /* CARO_GAME_H */
