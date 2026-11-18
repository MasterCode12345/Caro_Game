/*
 * client/ui_lobby.c — lobby page: players, challenges, quick match, leaderboard.
 * Owner: Nghĩa.
 *
 * C1 placeholder: shows who is logged in and has a working Logout button (LOGOUT -> close the
 * connection -> login page), so the C1 login/logout flow can be tested end to end.
 *
 * Dialogs: GtkMessageDialog + "response" signal, never gtk_dialog_run().
 *
 * TODO(C2): top bar "<username> · ELO <elo> · W/L/D" + Logout (LOGOUT -> login, net_close()).
 * TODO(C2): GtkTreeView + GtkListStore (name, ELO, status), filled by LIST_PLAYERS on entry;
 *           PLAYER_STATUS events add/update rows, OFFLINE removes the row.
 * TODO(C2): Challenge button only for an IDLE row -> CHALLENGE|user|15|30; 200 -> "waiting"
 *           dialog with Cancel (CHALLENGE_CANCEL|cid); 404/409 -> Vietnamese message.
 * TODO(C2): CHALLENGE_FROM -> one Accept/Decline dialog per cid (CHALLENGE_REPLY);
 *           CHALLENGE_DECLINED / _EXPIRED / _CANCELLED -> close that cid's dialog + short notice.
 * TODO(C2): GAME_START -> store the game in app, close every challenge dialog, show game page.
 * TODO(C4): after LEAVE_GAME: back here, LIST_PLAYERS again, refresh the ELO bar.
 * TODO(C5): "Lịch sử" button; "Bảng xếp hạng" (LEADERBOARD|10 dialog, own row in bold);
 *           "Tìm trận nhanh" (FIND_MATCH -> "searching" dialog with Cancel = CANCEL_MATCH;
 *           GAME_START closes it).
 */
#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
#include "net.h"

static GtkWidget *l_user, *l_status, *b_logout;

void ui_lobby_enter(void)
{
    char *markup = g_markup_printf_escaped(
        "<span size='x-large'><b>%s</b></span>   ·   ELO %d   ·   Thắng/Thua/Hòa %d/%d/%d",
        app.username, app.elo, app.wins, app.losses, app.draws);
    gtk_label_set_markup(GTK_LABEL(l_user), markup);
    g_free(markup);
    gtk_label_set_text(GTK_LABEL(l_status), "");
    gtk_widget_set_sensitive(b_logout, TRUE);
}

static void on_logout_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED)
        return;                        /* net_on_lost() already went back to login */
    if (code == ST_OK) {
        net_close();                   /* LOGOUT ends the session; the next login reconnects */
        app_go_login(NULL);
        return;
    }
    gtk_widget_set_sensitive(b_logout, TRUE);
    char *s = g_strdup_printf("Đăng xuất thất bại: %d %s", code, n >= 3 ? f[2] : "");
    gtk_label_set_text(GTK_LABEL(l_status), s);
    g_free(s);
}

static void on_logout_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_widget_set_sensitive(b_logout, FALSE);
    if (!net_request(CMD_LOGOUT, on_logout_reply, NULL)) {
        net_close();
        app_go_login(NULL);
    }
}

GtkWidget *ui_lobby_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    l_user = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(box), l_user, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       gtk_label_new("TODO (C2): danh sách người chơi, thách đấu, tìm trận"),
                       FALSE, FALSE, 0);

    b_logout = gtk_button_new_with_label("Đăng xuất");
    gtk_widget_set_halign(b_logout, GTK_ALIGN_CENTER);
    g_signal_connect(b_logout, "clicked", G_CALLBACK(on_logout_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), b_logout, FALSE, FALSE, 0);

    l_status = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(box), l_status, FALSE, FALSE, 0);
    return box;
}
