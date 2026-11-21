
/*
 * server/game.c — pure Caro board logic.
 * Owner: Hân.
 *
 * Only the four lines passing through the last move need to be checked.
 * A new winning line can only be created by the newly placed symbol.
 */
#include "game.h"

#include <string.h>

/* Initialize the board. Invalid sizes produce an empty board of size 0. */
void board_init(Board *b, int size)
{
    if (b == NULL)
        return;

    memset(b, 0, sizeof(*b));

    if (size < 1 || size > MAX_BOARD) {
        b->size = 0;
        return;
    }

    b->size = size;

    for (int r = 0; r < MAX_BOARD; r++) {
        for (int c = 0; c < MAX_BOARD; c++) {
            b->cell[r][c] = CELL_EMPTY;
        }
    }

    b->move_count = 0;
}

int board_in_range(const Board *b, int row, int col)
{
    if (b == NULL || b->size < 1 || b->size > MAX_BOARD)
        return 0;

    return row >= 0 && row < b->size &&
           col >= 0 && col < b->size;
}

int board_is_empty(const Board *b, int row, int col)
{
    return board_in_range(b, row, col) &&
           b->cell[row][col] == CELL_EMPTY;
}

void board_place(Board *b, int row, int col, char sym)
{
    if (!board_is_empty(b, row, col))
        return;

    if (sym != CELL_X && sym != CELL_O)
        return;

    b->cell[row][col] = sym;
    b->move_count++;
}

/*
 * Check one direction through the last move.
 * dr, dc define the direction.
 *
 * The entire contiguous segment is collected first. Then a window of
 * exactly 5 cells containing the anchor move is selected.
 */
static int check_direction(const Board *b, int row, int col,
                           int dr, int dc, int line[5][2])
{
    char sym = b->cell[row][col];

    int coords[2 * MAX_BOARD][2];
    int count = 0;

    /* Walk backward to the beginning of the contiguous segment. */
    int r = row;
    int c = col;

    while (board_in_range(b, r - dr, c - dc) &&
           b->cell[r - dr][c - dc] == sym) {
        r -= dr;
        c -= dc;
    }

    /* Walk forward and collect the whole segment. */
    while (board_in_range(b, r, c) && b->cell[r][c] == sym) {
        coords[count][0] = r;
        coords[count][1] = c;
        count++;

        r += dr;
        c += dc;
    }

    if (count < 5)
        return 0;

    /* Find the anchor move within the collected segment. */
    int anchor = 0;

    for (int i = 0; i < count; i++) {
        if (coords[i][0] == row && coords[i][1] == col) {
            anchor = i;
            break;
        }
    }

    /* Select 5 cells containing the anchor. */
    int start = anchor - 4;

    if (start < 0)
        start = 0;

    if (start > count - 5)
        start = count - 5;

    for (int i = 0; i < 5; i++) {
        line[i][0] = coords[start + i][0];
        line[i][1] = coords[start + i][1];
    }

    return 1;
}

int board_check_win(const Board *b, int row, int col, int line[5][2])
{
    if (!board_in_range(b, row, col) || line == NULL)
        return 0;

    char sym = b->cell[row][col];

    if (sym != CELL_X && sym != CELL_O)
        return 0;

    /* Horizontal */
    if (check_direction(b, row, col, 0, 1, line))
        return 1;

    /* Vertical */
    if (check_direction(b, row, col, 1, 0, line))
        return 1;

    /* Diagonal down */
    if (check_direction(b, row, col, 1, 1, line))
        return 1;

    /* Diagonal up */
    if (check_direction(b, row, col, 1, -1, line))
        return 1;

    return 0;
}

int board_is_full(const Board *b)
{
    if (b == NULL || b->size < 1 || b->size > MAX_BOARD)
        return 0;

    return b->move_count == b->size * b->size;
}
