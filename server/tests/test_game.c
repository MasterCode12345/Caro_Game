
/*
 * server/tests/test_game.c — tests for pure Caro board logic.
 * Owner: Hân.
 */
#include "../game.h"

#include <stdio.h>
#include <stdlib.h>

static int total_tests = 0;
static int failed_tests = 0;

#define TEST(name) \
    static void name(void)

#define RUN(name) do { \
    total_tests++; \
    name(); \
    if (failed_tests == 0) \
        printf("[PASS] %s\n", #name); \
} while (0)

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("[FAIL] %s:%d: %s\n", \
               __FILE__, __LINE__, #condition); \
        failed_tests++; \
        return; \
    } \
} while (0)

/* Check that the returned winning line contains the anchor move. */
static int line_contains(int line[5][2], int row, int col)
{
    for (int i = 0; i < 5; i++) {
        if (line[i][0] == row && line[i][1] == col)
            return 1;
    }

    return 0;
}

/* Check that the returned line consists of five valid X/O cells. */
static int line_has_five_valid_cells(const Board *b, int line[5][2])
{
    char sym = b->cell[line[0][0]][line[0][1]];

    if (sym != CELL_X && sym != CELL_O)
        return 0;

    for (int i = 0; i < 5; i++) {
        if (!board_in_range(b, line[i][0], line[i][1]))
            return 0;

        if (b->cell[line[i][0]][line[i][1]] != sym)
            return 0;
    }

    return 1;
}

/* Horizontal win */
TEST(test_horizontal_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int c = 3; c < 8; c++)
        board_place(&b, 5, c, CELL_X);

    CHECK(board_check_win(&b, 5, 5, line) == 1);
    CHECK(line_contains(line, 5, 5));
    CHECK(line_has_five_valid_cells(&b, line));
}

/* Vertical win */
TEST(test_vertical_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int r = 2; r < 7; r++)
        board_place(&b, r, 4, CELL_O);

    CHECK(board_check_win(&b, 4, 4, line) == 1);
    CHECK(line_contains(line, 4, 4));
    CHECK(line_has_five_valid_cells(&b, line));
}

/* Diagonal down-right win */
TEST(test_diagonal_down_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int i = 0; i < 5; i++)
        board_place(&b, 2 + i, 3 + i, CELL_X);

    CHECK(board_check_win(&b, 4, 5, line) == 1);
    CHECK(line_contains(line, 4, 5));
    CHECK(line_has_five_valid_cells(&b, line));
}

/* Diagonal up-right win */
TEST(test_diagonal_up_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int i = 0; i < 5; i++)
        board_place(&b, 6 - i, 3 + i, CELL_O);

    CHECK(board_check_win(&b, 4, 5, line) == 1);
    CHECK(line_contains(line, 4, 5));
    CHECK(line_has_five_valid_cells(&b, line));
}

/* Win near the board edge */
TEST(test_win_near_edge)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int c = 0; c < 5; c++)
        board_place(&b, 0, c, CELL_X);

    CHECK(board_check_win(&b, 0, 4, line) == 1);
    CHECK(line_contains(line, 0, 4));
    CHECK(line_has_five_valid_cells(&b, line));
}

/* Four consecutive symbols must not win */
TEST(test_four_in_a_row_not_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int c = 3; c < 7; c++)
        board_place(&b, 5, c, CELL_X);

    CHECK(board_check_win(&b, 5, 6, line) == 0);
}

/* Six consecutive symbols must win.
 * The returned five coordinates must include the anchor move.
 */
TEST(test_six_in_a_row_win)
{
    Board b;
    int line[5][2];

    board_init(&b, 15);

    for (int c = 3; c < 9; c++)
        board_place(&b, 5, c, CELL_X);

    CHECK(board_check_win(&b, 5, 6, line) == 1);
    CHECK(line_contains(line, 5, 6));
    CHECK(line_has_five_valid_cells(&b, line));

    /* All five returned coordinates must be on the same row. */
    for (int i = 0; i < 5; i++)
        CHECK(line[i][0] == 5);

    /* Returned coordinates must be consecutive. */
    for (int i = 1; i < 5; i++)
        CHECK(line[i][1] == line[i - 1][1] + 1);
}

/* A completely filled board must be detected. */
TEST(test_board_full)
{
    Board b;

    board_init(&b, 3);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            char sym = ((r + c) % 2 == 0) ? CELL_X : CELL_O;
            board_place(&b, r, c, sym);
        }
    }

    CHECK(board_is_full(&b) == 1);
    CHECK(b.move_count == 9);
}

/* Coordinates outside the active board must be rejected. */
TEST(test_out_of_range)
{
    Board b;

    board_init(&b, 15);

    CHECK(board_in_range(&b, -1, 0) == 0);
    CHECK(board_in_range(&b, 15, 0) == 0);
    CHECK(board_in_range(&b, 0, 15) == 0);

    CHECK(board_is_empty(&b, -1, 0) == 0);
    CHECK(board_is_empty(&b, 15, 0) == 0);

    board_place(&b, -1, 0, CELL_X);
    board_place(&b, 15, 0, CELL_X);

    CHECK(b.move_count == 0);
}

/* Placing a symbol on an occupied cell must not overwrite it. */
TEST(test_occupied_cell)
{
    Board b;

    board_init(&b, 15);

    board_place(&b, 5, 5, CELL_X);

    CHECK(b.cell[5][5] == CELL_X);
    CHECK(b.move_count == 1);

    board_place(&b, 5, 5, CELL_O);

    CHECK(b.cell[5][5] == CELL_X);
    CHECK(b.move_count == 1);
}

/* Invalid symbol must not be placed. */
TEST(test_invalid_symbol)
{
    Board b;

    board_init(&b, 15);

    board_place(&b, 5, 5, 'A');

    CHECK(b.cell[5][5] == CELL_EMPTY);
    CHECK(b.move_count == 0);
}

/* Invalid board size must produce an unusable board. */
TEST(test_invalid_board_size)
{
    Board b;

    board_init(&b, 20);

    CHECK(b.size == 0);
    CHECK(board_in_range(&b, 0, 0) == 0);
    CHECK(board_is_full(&b) == 0);
}

int main(void)
{
    RUN(test_horizontal_win);
    RUN(test_vertical_win);
    RUN(test_diagonal_down_win);
    RUN(test_diagonal_up_win);
    RUN(test_win_near_edge);
    RUN(test_four_in_a_row_not_win);
    RUN(test_six_in_a_row_win);
    RUN(test_board_full);
    RUN(test_out_of_range);
    RUN(test_occupied_cell);
    RUN(test_invalid_symbol);
    RUN(test_invalid_board_size);

    printf("\n%d/%d tests passed.\n",
           total_tests - failed_tests, total_tests);

    return failed_tests == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
