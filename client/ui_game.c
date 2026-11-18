/*
 * client/ui_game.c — page "game": board, turn, controls, result.
 * Owner: Nghĩa.
 *
 * C2 placeholder: shows the game that GAME_START announced (stored in app.game by ui_lobby.c).
 *
 * Rule: never draw a move before the server answers 200|MOVE.
 *
 * TODO(C3): side panel: both names + ELO, my symbol, "Lượt của bạn" / "Đợi đối thủ".
 * TODO(C3): on_cell: ignore if not my turn, cell taken or a MOVE is pending; else send
 *           MOVE|gid|row|col and mark pending; place the mark only on 200|MOVE;
 *           430–433 -> short message, clear pending.
 * TODO(C3): OPP_MOVE (check gid) -> place the opponent's mark, my turn.
 * TODO(C3): GAME_END -> lock the board, highlight the winning line, result dialog
 *           (Thắng/Thua/Hòa + reason in Vietnamese + new ELO ±delta), update app's ELO.
 * TODO(C4): buttons Xin thua (confirm -> RESIGN), Cầu hòa (DRAW_OFFER), Đấu lại (after GAME_END
 *           -> REMATCH), Về sảnh (LEAVE_GAME, confirm if the game is still running).
 * TODO(C4): DRAW_OFFERED / REMATCH_REQUESTED -> Accept/Decline dialogs; DRAW_DECLINED /
 *           REMATCH_DECLINED -> notice; GAME_START here (rematch) -> clear board, new symbol;
 *           OPP_LEFT -> "Đối thủ đã rời", disable Đấu lại.
 * TODO(C4): reasons FIVE, BOARD_FULL, RESIGN, DRAW_AGREED, TIMEOUT, DISCONNECT in Vietnamese.
 * TODO(C5): countdown of the current move (g_timeout_add_seconds 1), reset after every move;
 *           display only, the server decides TIMEOUT.
 */
#include <gtk/gtk.h>

#include "app.h"

static GtkWidget *l_info;

void ui_game_enter(void)
{
    const GameInfo *g = &app.game;
    char *markup = g_markup_printf_escaped(
        "<span size='x-large'><b>Ván #%d</b></span>\n\n"
        "<b>%s</b> (ELO %d, quân <b>%c</b>)   đấu với   <b>%s</b> (ELO %d, quân <b>%c</b>)\n"
        "Bàn %d×%d · %d giây mỗi nước · %s",
        g->gid, app.username, app.elo, g->my_symbol, g->opponent, g->opp_elo,
        g->my_symbol == 'X' ? 'O' : 'X', g->size, g->size, g->sec_per_move,
        g->my_symbol == 'X' ? "bạn đi trước" : "đối thủ đi trước");
    gtk_label_set_markup(GTK_LABEL(l_info), markup);
    g_free(markup);
}

GtkWidget *ui_game_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    l_info = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(l_info), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), l_info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("TODO (C3): bàn cờ và nước đi"), FALSE, FALSE, 0);
    return box;
}
