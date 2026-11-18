/*
 * client/ui_game.c — page "game": board, turn, result (docs/protocol.md §6.3, §7).
 * Owner: Nghĩa.
 *
 * THE SERVER DECIDES — why a click does not draw a mark
 *   A click only sends MOVE|gid|row|col and marks the move as pending (the board is locked).
 *   The mark appears when 200|MOVE comes back. The server is the only judge of whose turn it is,
 *   whether the cell is free and whether the game is still running: it may have received the
 *   opponent's RESIGN, or the move timer may have run out, a moment before our MOVE. If the
 *   client drew first, a refused move (430–433) would have to be erased again, and for a short
 *   time the two players would see different boards. Drawing only what the server confirmed
 *   keeps both boards identical: every mark on screen comes from 200|MOVE or OPP_MOVE.
 *
 * TODO(C4): buttons Xin thua (confirm -> RESIGN), Cầu hòa (DRAW_OFFER), Đấu lại (after GAME_END
 *           -> REMATCH), Về sảnh (LEAVE_GAME, confirm if the game is still running).
 * TODO(C4): DRAW_OFFERED / REMATCH_REQUESTED -> Accept/Decline dialogs; DRAW_DECLINED /
 *           REMATCH_DECLINED -> notice; GAME_START here (rematch) -> clear board, new symbol;
 *           OPP_LEFT -> "Đối thủ đã rời", disable Đấu lại.
 * TODO(C5): countdown of the current move (g_timeout_add_seconds 1), reset after every move;
 *           display only, the server decides TIMEOUT.
 */
#include <stdarg.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "board_view.h"
#include "dispatch.h"
#include "net.h"

static BoardView *bv;
static GtkWidget *l_title, *l_me, *l_opp, *l_turn, *l_moves, *l_notice;
static GtkWidget *result_dialog;
static struct {
    gboolean known;            /* GAME_END received for the current game */
    int      elo_before, new_elo, delta;
    char     outcome;          /* 'W' win, 'L' loss, 'D' draw */
} end;

/* ---- helpers ------------------------------------------------------------------------------------- */

static char opp_symbol(void)
{
    return app.game.my_symbol == 'X' ? 'O' : 'X';
}

static const char *symbol_markup(char s)
{
    return s == 'X' ? "<span foreground='#b31414'><b>X</b></span>"
                    : "<span foreground='#1a59cc'><b>O</b></span>";
}

static void fmt_delta(char *buf, size_t len, int d)
{
    if (d == 0)
        snprintf(buf, len, "0");          /* the protocol writes 0 without a sign */
    else
        snprintf(buf, len, "%+d", d);
}

static void notice(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    char *markup = g_markup_printf_escaped("<span foreground='#c01c28'>%s</span>", text);
    gtk_label_set_markup(GTK_LABEL(l_notice), markup);
    g_free(markup);
    g_free(text);
}

/* "7,11" -> 7, 11 (in place) */
static gboolean parse_rc(char *s, int *r, int *c)
{
    char *comma = strchr(s, ITEM_SEP);
    if (comma == NULL)
        return FALSE;
    *comma = '\0';
    gboolean ok = field_int(s, r) && field_int(comma + 1, c);
    *comma = ITEM_SEP;
    return ok;
}

/* ---- side panel ------------------------------------------------------------------------------------ */

static void set_player(GtkWidget *label, gboolean to_move, char sym, const char *name,
                       int elo_before, gboolean show_after, int elo_after)
{
    char elo[64], d[16];
    if (show_after) {
        fmt_delta(d, sizeof d, elo_after - elo_before);
        snprintf(elo, sizeof elo, "ELO %d → %d (%s)", elo_before, elo_after, d);
    } else {
        snprintf(elo, sizeof elo, "ELO %d", elo_before);
    }
    char *esc = g_markup_escape_text(name, -1);
    char *markup = g_strdup_printf("<span size='large'>%s %s  <b>%s</b></span>\n<small>%s</small>",
                                   to_move ? "▶" : "   ", symbol_markup(sym), esc, elo);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(esc);
}

static void update_panel(void)
{
    const GameInfo *g = &app.game;
    gboolean my_turn = !g->ended && g->turn == g->my_symbol;
    gboolean opp_turn = !g->ended && g->turn == opp_symbol();

    char *title = g_strdup_printf("<span size='x-large'><b>Ván #%d</b></span>\n"
                                  "Bàn %d×%d · %d giây mỗi nước", g->gid, g->size, g->size,
                                  g->sec_per_move);
    gtk_label_set_markup(GTK_LABEL(l_title), title);
    g_free(title);

    /* Only our own delta is sent; the opponent's is its negation (protocol.md §9). */
    set_player(l_me, my_turn, g->my_symbol, app.username,
               end.known ? end.elo_before : app.elo, end.known, end.new_elo);
    set_player(l_opp, opp_turn, opp_symbol(), g->opponent, g->opp_elo, end.known,
               g->opp_elo - end.delta);

    const char *turn;
    if (g->ended)
        turn = end.outcome == 'W' ? "<span foreground='#26a269'><b>Bạn thắng</b></span>"
             : end.outcome == 'L' ? "<span foreground='#c01c28'><b>Bạn thua</b></span>"
                                  : "<b>Hòa</b>";
    else if (g->move_pending)
        turn = "<span foreground='#5e5c64'>Đang gửi nước đi…</span>";
    else if (my_turn)
        turn = "<span foreground='#26a269'><b>Lượt của bạn</b></span>";
    else
        turn = "<span foreground='#5e5c64'>Đợi đối thủ…</span>";
    char *t = g_strdup_printf("<span size='x-large'>%s</span>", turn);
    gtk_label_set_markup(GTK_LABEL(l_turn), t);
    g_free(t);

    char moves[48];
    snprintf(moves, sizeof moves, "Số nước đã đi: %d", g->move_no);
    gtk_label_set_text(GTK_LABEL(l_moves), moves);

    board_view_set_interactive(bv, my_turn && !g->move_pending);
}

/* ---- my move ------------------------------------------------------------------------------------------ */

static const char *move_error_text(int code)
{
    switch (code) {
    case ST_NOT_YOUR_TURN:   return "Chưa đến lượt bạn";
    case ST_OUT_OF_BOARD:    return "Ô này nằm ngoài bàn cờ";
    case ST_CELL_OCCUPIED:   return "Ô này đã có quân";
    case ST_GAME_NOT_ACTIVE: return "Ván đã kết thúc";
    case ST_FORBIDDEN:       return "Bạn không ở trong ván này";
    case ST_BAD_REQUEST:     return "Nước đi không hợp lệ";
    default:                 return NULL;
    }
}

/* 200|MOVE|move_no — only now is our mark drawn. user_data = the gid the move was sent for. */
static void on_move_reply(int code, char **f, int n, gpointer data)
{
    GameInfo *g = &app.game;
    if (code == RESP_DISCONNECTED || !g->active || g->gid != GPOINTER_TO_INT(data))
        return;                              /* connection gone, or an older game */
    g->move_pending = FALSE;

    if (code == ST_OK) {
        int move_no;
        board_view_set(bv, g->pending_row, g->pending_col, g->my_symbol);
        board_view_set_last(bv, g->pending_row, g->pending_col);
        g->move_no = n >= 3 && field_int(f[2], &move_no) ? move_no : g->move_no + 1;
        g->turn = opp_symbol();
    } else if (!g->ended) {                  /* after GAME_END a refused move needs no message */
        const char *text = move_error_text(code);
        if (text != NULL)
            notice("%s", text);
        else
            notice("Nước đi bị từ chối (%d %s)", code, n >= 3 ? f[2] : "");
    }
    update_panel();
}

static void on_cell(int row, int col, gpointer data)
{
    (void)data;
    GameInfo *g = &app.game;
    if (!g->active || g->ended || g->move_pending || g->turn != g->my_symbol
        || board_view_get(bv, row, col) != '.')
        return;

    char *payload = g_strdup_printf("%s|%d|%d|%d", CMD_MOVE, g->gid, row, col);
    if (net_request(payload, on_move_reply, GINT_TO_POINTER(g->gid))) {
        g->move_pending = TRUE;              /* locked until the server answers */
        g->pending_row = row;
        g->pending_col = col;
        gtk_label_set_text(GTK_LABEL(l_notice), "");
        update_panel();
    }
    g_free(payload);
}

/* ---- events ---------------------------------------------------------------------------------------------- */

/* OPP_MOVE|gid|row|col|move_no */
static void on_opp_move(char **f, int n)
{
    GameInfo *g = &app.game;
    int gid, r, c, move_no;
    if (n != 5 || !field_int(f[1], &gid) || !field_int(f[2], &r) || !field_int(f[3], &c)
        || !field_int(f[4], &move_no)) {
        g_printerr("caro_client: warning: bad OPP_MOVE ignored\n");
        return;
    }
    if (!g->active || gid != g->gid)
        return;                              /* not the game on screen */
    char cell = board_view_get(bv, r, c);
    if (cell == 0) {
        g_printerr("caro_client: warning: OPP_MOVE off the board ignored\n");
        return;
    }
    if (cell != '.')                         /* the server is the reference: follow it */
        g_printerr("caro_client: warning: OPP_MOVE on a non-empty cell %d,%d (boards differ?)\n", r, c);
    board_view_set(bv, r, c, opp_symbol());
    board_view_set_last(bv, r, c);
    g->move_no = move_no;
    g->turn = g->my_symbol;
    update_panel();
}

static const char *end_reason_text(const char *reason, char outcome)
{
    gboolean won = outcome == 'W';
    if (strcmp(reason, END_FIVE) == 0)
        return won ? "Bạn có 5 quân liên tiếp" : "Đối thủ có 5 quân liên tiếp";
    if (strcmp(reason, END_BOARD_FULL) == 0)
        return "Bàn cờ đã kín mà không ai có 5 quân liên tiếp";
    if (strcmp(reason, END_RESIGN) == 0)
        return won ? "Đối thủ xin thua" : "Bạn xin thua";
    if (strcmp(reason, END_DRAW_AGREED) == 0)
        return "Hai bên đồng ý hòa";
    if (strcmp(reason, END_TIMEOUT) == 0)
        return won ? "Đối thủ hết thời gian suy nghĩ" : "Bạn hết thời gian suy nghĩ";
    if (strcmp(reason, END_DISCONNECT) == 0)
        return won ? "Đối thủ mất kết nối" : "Bạn mất kết nối";
    return reason;
}

static void close_result_dialog(void)
{
    if (result_dialog != NULL) {
        gtk_widget_destroy(result_dialog);
        result_dialog = NULL;
    }
}

static void on_result_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)dialog;
    (void)response;
    (void)data;
    close_result_dialog();
}

static void show_result_dialog(const char *reason)
{
    close_result_dialog();
    char d[16];
    fmt_delta(d, sizeof d, end.delta);
    GtkWidget *dlg = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
        end.outcome == 'W' ? "Bạn thắng!" : end.outcome == 'L' ? "Bạn thua" : "Hòa");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s.\nELO mới: %d (%s)",
                                             end_reason_text(reason, end.outcome), end.new_elo, d);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Đóng", GTK_RESPONSE_CLOSE);
    g_signal_connect(dlg, "response", G_CALLBACK(on_result_response), NULL);
    result_dialog = dlg;
    gtk_widget_show_all(dlg);
}

/* GAME_END|gid|result|reason|new_elo|elo_delta[|r,c x5 when reason is FIVE] — to both players */
static void on_game_end(char **f, int n)
{
    GameInfo *g = &app.game;
    int gid, new_elo, delta;
    if (n < 6 || !field_int(f[1], &gid) || !field_int(f[4], &new_elo) || !field_int(f[5], &delta)) {
        g_printerr("caro_client: warning: bad GAME_END ignored\n");
        return;
    }
    if (!g->active || gid != g->gid || g->ended)
        return;

    const char *result = f[2], *reason = f[3];
    char mine[8];
    snprintf(mine, sizeof mine, "%c_WIN", g->my_symbol);
    char outcome;
    if (strcmp(result, RES_DRAW) == 0)
        outcome = 'D';
    else if (strcmp(result, mine) == 0)
        outcome = 'W';
    else if (strcmp(result, RES_X_WIN) == 0 || strcmp(result, RES_O_WIN) == 0)
        outcome = 'L';
    else {
        g_printerr("caro_client: warning: GAME_END with unknown result '%s' ignored\n", result);
        return;
    }

    g->ended = TRUE;
    g->move_pending = FALSE;
    if (strcmp(reason, END_FIVE) == 0) {     /* the five winning cells follow */
        int cells[WIN_LENGTH][2], k = 0;
        for (int i = 6; i < n && k < WIN_LENGTH; i++)
            if (parse_rc(f[i], &cells[k][0], &cells[k][1]))
                k++;
        if (k != WIN_LENGTH)
            g_printerr("caro_client: warning: GAME_END FIVE with %d valid cells\n", k);
        board_view_set_win_line(bv, (const int (*)[2])cells, k);
    }

    end.known = TRUE;
    end.elo_before = app.elo;
    end.new_elo = new_elo;
    end.delta = delta;
    end.outcome = outcome;
    app.elo = new_elo;
    if (outcome == 'W')
        app.wins++;
    else if (outcome == 'L')
        app.losses++;
    else
        app.draws++;

    update_panel();
    show_result_dialog(reason);
}

/* ---- page ------------------------------------------------------------------------------------------------ */

void ui_game_enter(void)
{
    GameInfo *g = &app.game;
    close_result_dialog();
    board_view_reset(bv, g->size);
    g->turn = 'X';                           /* X always moves first */
    g->move_no = 0;
    g->move_pending = FALSE;
    g->ended = FALSE;
    memset(&end, 0, sizeof end);
    gtk_label_set_text(GTK_LABEL(l_notice), "");
    update_panel();
}

void ui_game_reset(void)
{
    close_result_dialog();
}

static GtkWidget *side_label(GtkWidget *box, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

GtkWidget *ui_game_new(void)
{
    dispatch_on(EV_OPP_MOVE, on_opp_move);
    dispatch_on(EV_GAME_END, on_game_end);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_object_set(page, "margin", 16, NULL);

    bv = board_view_new(on_cell, NULL);
    gtk_box_pack_start(GTK_BOX(page), board_view_widget(bv), TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(side, 250, -1);
    l_title = side_label(side, FALSE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_me = side_label(side, FALSE);
    l_opp = side_label(side, FALSE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_turn = side_label(side, FALSE);
    l_moves = side_label(side, FALSE);
    l_notice = side_label(side, TRUE);
    gtk_box_pack_start(GTK_BOX(page), side, FALSE, FALSE, 0);
    return page;
}
