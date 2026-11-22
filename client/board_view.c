/*
 * client/board_view.c — cairo board drawing, shared by the game page and the replay viewer.
 * Owner: Nghĩa.
 *
 * A GtkDrawingArea: GTK asks us to repaint ("draw" signal) whenever needed, and every setter
 * calls gtk_widget_queue_draw() to ask for one. The cell size is recomputed from the widget's
 * size at every paint, so the board follows the window when it is resized. Marks sit inside
 * the cells (Caro style); row/col count from 0 at the top-left, as in docs/protocol.md §2.
 *
 * The game page and the replay viewer (ui_history.c) each own one BoardView; the replay one is
 * never made interactive, so it ignores clicks and shows no hover.
 */
#include "board_view.h"

#include <string.h>

#define MARGIN    12      /* px around the board */
#define MIN_CELL  8

struct BoardView {
    GtkWidget  *area;
    int         size;
    char        cells[BOARD_SIZE_MAX][BOARD_SIZE_MAX];
    int         last_row, last_col;
    int         win[WIN_LENGTH][2];
    int         n_win;
    gboolean    interactive;
    int         hover_row, hover_col;
    BoardCellCb on_cell;
    gpointer    user_data;
};

typedef struct { int x0, y0, cell; } Geometry;

static Geometry bv_geometry(const BoardView *bv, int width, int height)
{
    Geometry g;
    int side = MIN(width, height) - 2 * MARGIN;
    g.cell = MAX(side / bv->size, MIN_CELL);
    g.x0 = (width - g.cell * bv->size) / 2;
    g.y0 = (height - g.cell * bv->size) / 2;
    return g;
}

static gboolean bv_cell_at(const BoardView *bv, int width, int height, double x, double y,
                           int *row, int *col)
{
    Geometry g = bv_geometry(bv, width, height);
    if (x < g.x0 || y < g.y0)
        return FALSE;
    int c = (int)(x - g.x0) / g.cell, r = (int)(y - g.y0) / g.cell;
    if (r >= bv->size || c >= bv->size)
        return FALSE;
    *row = r;
    *col = c;
    return TRUE;
}

/* ---- painting ------------------------------------------------------------------------------------ */

static void bv_fill_cell(cairo_t *cr, Geometry g, int row, int col)
{
    cairo_rectangle(cr, g.x0 + col * g.cell, g.y0 + row * g.cell, g.cell, g.cell);
    cairo_fill(cr);
}

static void bv_paint(const BoardView *bv, cairo_t *cr, int width, int height)
{
    Geometry g = bv_geometry(bv, width, height);
    int n = bv->size, c = g.cell, side = c * n;

    cairo_set_source_rgb(cr, 0.96, 0.89, 0.73);                  /* board */
    cairo_rectangle(cr, g.x0, g.y0, side, side);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.62, 0.89, 0.55);                  /* winning cells */
    for (int i = 0; i < bv->n_win; i++)
        bv_fill_cell(cr, g, bv->win[i][0], bv->win[i][1]);

    if (bv->interactive && bv->hover_row >= 0 && bv->cells[bv->hover_row][bv->hover_col] == '.') {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.08);                /* cell under the mouse */
        bv_fill_cell(cr, g, bv->hover_row, bv->hover_col);
    }

    cairo_set_source_rgb(cr, 0.55, 0.42, 0.25);                  /* grid: n+1 lines each way */
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= n; i++) {
        cairo_move_to(cr, g.x0 + i * c + 0.5, g.y0);             /* +0.5: crisp 1-px lines */
        cairo_line_to(cr, g.x0 + i * c + 0.5, g.y0 + side);
        cairo_move_to(cr, g.x0, g.y0 + i * c + 0.5);
        cairo_line_to(cr, g.x0 + side, g.y0 + i * c + 0.5);
    }
    cairo_stroke(cr);

    double lw = MAX(2.0, c * 0.12), inset = c * 0.22;
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int r = 0; r < n; r++) {
        for (int k = 0; k < n; k++) {
            double x = g.x0 + k * c, y = g.y0 + r * c;
            if (bv->cells[r][k] == 'X') {                        /* two dark-red diagonals */
                cairo_set_source_rgb(cr, 0.70, 0.08, 0.08);
                cairo_move_to(cr, x + inset, y + inset);
                cairo_line_to(cr, x + c - inset, y + c - inset);
                cairo_move_to(cr, x + c - inset, y + inset);
                cairo_line_to(cr, x + inset, y + c - inset);
                cairo_stroke(cr);
            } else if (bv->cells[r][k] == 'O') {                 /* blue circle */
                cairo_set_source_rgb(cr, 0.10, 0.35, 0.80);
                cairo_new_sub_path(cr);
                cairo_arc(cr, x + c / 2.0, y + c / 2.0, c * 0.30, 0, 2 * G_PI);
                cairo_stroke(cr);
            }
        }
    }

    if (bv->last_row >= 0) {                                     /* last move: orange outline */
        cairo_set_source_rgb(cr, 1.0, 0.55, 0.0);
        cairo_set_line_width(cr, 2.5);
        cairo_rectangle(cr, g.x0 + bv->last_col * c + 2.5, g.y0 + bv->last_row * c + 2.5,
                        c - 5, c - 5);
        cairo_stroke(cr);
    }
}

/* ---- GTK signals ---------------------------------------------------------------------------------- */

static gboolean bv_on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    bv_paint(data, cr, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    return FALSE;
}

static gboolean bv_on_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    BoardView *bv = data;
    int row, col;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1 || !bv->interactive || bv->on_cell == NULL)
        return FALSE;
    if (bv_cell_at(bv, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w),
                   ev->x, ev->y, &row, &col))
        bv->on_cell(row, col, bv->user_data);
    return TRUE;
}

static void bv_set_hover(BoardView *bv, int row, int col)
{
    if (row != bv->hover_row || col != bv->hover_col) {
        bv->hover_row = row;
        bv->hover_col = col;
        gtk_widget_queue_draw(bv->area);
    }
}

static gboolean bv_on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    BoardView *bv = data;
    int row = -1, col = -1;
    if (!bv_cell_at(bv, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w),
                    ev->x, ev->y, &row, &col))
        row = col = -1;
    bv_set_hover(bv, row, col);
    return FALSE;
}

static gboolean bv_on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    (void)w;
    (void)ev;
    bv_set_hover(data, -1, -1);
    return FALSE;
}

static void bv_on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    g_free(data);
}

/* ---- public ----------------------------------------------------------------------------------------- */

BoardView *board_view_new(BoardCellCb on_cell, gpointer user_data)
{
    BoardView *bv = g_new0(BoardView, 1);
    bv->on_cell = on_cell;
    bv->user_data = user_data;
    bv->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(bv->area, 300, 300);
    gtk_widget_set_hexpand(bv->area, TRUE);
    gtk_widget_set_vexpand(bv->area, TRUE);
    gtk_widget_add_events(bv->area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK
                                    | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(bv->area, "draw", G_CALLBACK(bv_on_draw), bv);
    g_signal_connect(bv->area, "button-press-event", G_CALLBACK(bv_on_press), bv);
    g_signal_connect(bv->area, "motion-notify-event", G_CALLBACK(bv_on_motion), bv);
    g_signal_connect(bv->area, "leave-notify-event", G_CALLBACK(bv_on_leave), bv);
    g_signal_connect(bv->area, "destroy", G_CALLBACK(bv_on_destroy), bv);
    board_view_reset(bv, BOARD_SIZE);
    return bv;
}

GtkWidget *board_view_widget(BoardView *bv)
{
    return bv->area;
}

void board_view_reset(BoardView *bv, int size)
{
    bv->size = CLAMP(size, 1, BOARD_SIZE_MAX);
    memset(bv->cells, '.', sizeof bv->cells);
    bv->last_row = bv->last_col = -1;
    bv->hover_row = bv->hover_col = -1;
    bv->n_win = 0;
    gtk_widget_queue_draw(bv->area);
}

int board_view_size(const BoardView *bv)
{
    return bv->size;
}

char board_view_get(const BoardView *bv, int row, int col)
{
    if (row < 0 || col < 0 || row >= bv->size || col >= bv->size)
        return 0;
    return bv->cells[row][col];
}

void board_view_set(BoardView *bv, int row, int col, char mark)
{
    if (board_view_get(bv, row, col) == 0)
        return;
    bv->cells[row][col] = mark;
    gtk_widget_queue_draw(bv->area);
}

void board_view_set_last(BoardView *bv, int row, int col)
{
    gboolean on = board_view_get(bv, row, col) != 0;
    bv->last_row = on ? row : -1;
    bv->last_col = on ? col : -1;
    gtk_widget_queue_draw(bv->area);
}

void board_view_set_win_line(BoardView *bv, const int cells[][2], int n)
{
    bv->n_win = 0;
    for (int i = 0; i < n && i < WIN_LENGTH; i++) {
        if (board_view_get(bv, cells[i][0], cells[i][1]) == 0)
            continue;                          /* ignore a cell off the board */
        bv->win[bv->n_win][0] = cells[i][0];
        bv->win[bv->n_win][1] = cells[i][1];
        bv->n_win++;
    }
    gtk_widget_queue_draw(bv->area);
}

void board_view_set_interactive(BoardView *bv, gboolean on)
{
    bv->interactive = on;
    gtk_widget_queue_draw(bv->area);
}
