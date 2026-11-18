/*
 * client/board_view.h — the board widget, shared by the game page and the replay viewer.
 * Owner: Nghĩa.
 *
 * A BoardView holds what it draws (cells, last move, winning line) and reports clicks; it knows
 * nothing about turns or the network. It is freed together with its widget.
 */
#ifndef CARO_CLIENT_BOARD_VIEW_H
#define CARO_CLIENT_BOARD_VIEW_H

#include <gtk/gtk.h>

#include "proto.h"

typedef struct BoardView BoardView;

/* Left click on a cell of an interactive board. */
typedef void (*BoardCellCb)(int row, int col, gpointer user_data);

BoardView *board_view_new(BoardCellCb on_cell, gpointer user_data);
GtkWidget *board_view_widget(BoardView *bv);

void board_view_reset(BoardView *bv, int size);             /* empty size x size board */
int  board_view_size(const BoardView *bv);
char board_view_get(const BoardView *bv, int row, int col);  /* '.', 'X', 'O'; 0 off the board */
void board_view_set(BoardView *bv, int row, int col, char mark);
void board_view_set_last(BoardView *bv, int row, int col);   /* outlined cell; -1, -1 = none */
void board_view_set_win_line(BoardView *bv, const int cells[][2], int n);   /* n = 0 clears */
void board_view_set_interactive(BoardView *bv, gboolean on); /* clicks + hover only when TRUE */

#endif /* CARO_CLIENT_BOARD_VIEW_H */
