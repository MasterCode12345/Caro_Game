/*
 * client/ui_lobby.c — page "lobby": who is online, challenges (docs/protocol.md §6.2, §7).
 * Owner: Nghĩa.
 *
 * WHY THE LOBBY UPDATES ITSELF (no polling)
 *   Entering the page sends ONE LIST_PLAYERS: a snapshot. After that the server PUSHES a
 *   PLAYER_STATUS event to every logged-in client whenever someone logs in or out, enters or
 *   leaves the queue or a game, or changes ELO, and the event handler edits only that row.
 *   The socket watch in net.c delivers the event the moment it arrives, so the list is always
 *   current without asking again. Events that arrive before the LIST_PLAYERS response are
 *   already contained in it (the server built the snapshot later, TCP keeps the order), so the
 *   response simply replaces the whole list.
 *
 * CHALLENGES
 *   Outgoing: at most one (the protocol allows one pending per user). States: REQUESTING
 *   (CHALLENGE sent) -> WAITING (got cid, "waiting" dialog) -> CANCELLING (CHALLENGE_CANCEL sent).
 *   It ends with GAME_START, CHALLENGE_DECLINED / _EXPIRED / _CANCELLED, or 200|CHALLENGE_CANCEL.
 *   Incoming: any number at once, one Accept/Decline dialog each, kept by cid.
 *   Dialogs: GtkMessageDialog + "response" signal, never gtk_dialog_run() — it would run a
 *   nested main loop, and code that thinks it waits for the user would then see network events
 *   (even GAME_START) happen "inside" it.
 *
 * BACK FROM A GAME (ui_game.c, after 200|LEAVE_GAME)
 *   app_show_page("lobby") sends LIST_PLAYERS again (the list was not kept up to date while we
 *   played), then ui_lobby_request_stats() sends STATS. The top bar is redrawn from the STATS
 *   response: the client never adds or subtracts ELO or wins itself, the server's database is
 *   the reference. If we left a running game, its GAME_END arrives after the 200 (§3), while
 *   we are already here: ui_game.c then only updates app.elo and calls ui_lobby_update_bar().
 * TODO(C5): "Lịch sử" button; "Bảng xếp hạng" (LEADERBOARD|10 dialog, own row in bold);
 *           "Tìm trận nhanh" (FIND_MATCH -> "searching" dialog with Cancel = CANCEL_MATCH;
 *           GAME_START closes it).
 */
#include <stdarg.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
#include "net.h"

enum { COL_NAME, COL_ELO, COL_STATUS, COL_STATUS_TEXT, N_COLS };

typedef enum { OUT_NONE, OUT_REQUESTING, OUT_WAITING, OUT_CANCELLING } OutState;

typedef struct {
    int        cid;
    char       from[USERNAME_MAX + 1];
    GtkWidget *dialog;
    gboolean   accepting;          /* ACCEPT sent, waiting for 200 + GAME_START */
} Invite;

static GtkWidget    *l_user, *l_count, *l_notice, *b_logout, *b_challenge, *tree;
static GtkListStore *store;
static GHashTable   *invites;      /* cid -> Invite* (incoming) */
static struct {
    OutState   state;
    int        cid;
    char       target[USERNAME_MAX + 1];
    GtkWidget *dialog;
} out;                             /* our outgoing challenge */

/* ---- small helpers ------------------------------------------------------------------------------- */

typedef enum { NOTE_INFO, NOTE_ERROR } NoteKind;

static void notice(NoteKind kind, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void notice(NoteKind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    char *markup = g_markup_printf_escaped("<span foreground='%s'>%s</span>",
                                           kind == NOTE_ERROR ? "#c01c28" : "#1c71d8", text);
    gtk_label_set_markup(GTK_LABEL(l_notice), markup);
    g_free(markup);
    g_free(text);
}

static void notice_clear(void)
{
    gtk_label_set_text(GTK_LABEL(l_notice), "");
}

static gboolean is_player_status(const char *s)
{
    return strcmp(s, PS_IDLE) == 0 || strcmp(s, PS_IN_QUEUE) == 0 || strcmp(s, PS_IN_GAME) == 0;
}

static const char *status_text(const char *s)
{
    if (strcmp(s, PS_IDLE) == 0)
        return "Rảnh";
    if (strcmp(s, PS_IN_QUEUE) == 0)
        return "Đang tìm trận";
    if (strcmp(s, PS_IN_GAME) == 0)
        return "Đang chơi";
    return s;
}

/* "han,1016,IDLE" -> 3 parts, in place. Returns the number of parts, max + 1 if there are more. */
static int split_item(char *item, char *parts[], int max)
{
    int k = 0;
    parts[k++] = item;
    for (char *p = item; *p != '\0'; p++) {
        if (*p != ITEM_SEP)
            continue;
        if (k == max)
            return max + 1;
        *p = '\0';
        parts[k++] = p + 1;
    }
    return k;
}

/* ---- player list ---------------------------------------------------------------------------------- */

static gboolean find_row(const char *name, GtkTreeIter *it)
{
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    for (gboolean ok = gtk_tree_model_get_iter_first(m, it); ok; ok = gtk_tree_model_iter_next(m, it)) {
        gchar *n;
        gtk_tree_model_get(m, it, COL_NAME, &n, -1);
        gboolean same = strcmp(n, name) == 0;
        g_free(n);
        if (same)
            return TRUE;
    }
    return FALSE;
}

static void set_row(const char *name, int elo, const char *status)
{
    GtkTreeIter it;
    if (find_row(name, &it))
        gtk_list_store_set(store, &it, COL_ELO, elo, COL_STATUS, status,
                           COL_STATUS_TEXT, status_text(status), -1);
    else
        gtk_list_store_insert_with_values(store, NULL, -1, COL_NAME, name, COL_ELO, elo,
                                          COL_STATUS, status, COL_STATUS_TEXT, status_text(status), -1);
}

static void update_count(void)
{
    char *s = g_strdup_printf("Người chơi trực tuyến: %d",
                              gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), NULL));
    gtk_label_set_text(GTK_LABEL(l_count), s);
    g_free(s);
}

static gboolean get_selected(char **name, char **status)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &m, &it))
        return FALSE;
    gtk_tree_model_get(m, &it, COL_NAME, name, COL_STATUS, status, -1);
    return TRUE;
}

/* Challenge is possible only on an IDLE player, from the lobby, with no challenge of ours pending. */
static void update_challenge_button(void)
{
    char *name = NULL, *status = NULL;
    gboolean ok = app.state == SESSION_LOBBY && out.state == OUT_NONE
                  && get_selected(&name, &status) && strcmp(status, PS_IDLE) == 0;
    gtk_widget_set_sensitive(b_challenge, ok);
    g_free(name);
    g_free(status);
}

/* 200|LIST_PLAYERS|n|user,elo,status|... */
static void on_list_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED)
        return;
    int count;
    if (code != ST_OK || n < 3 || !field_int(f[2], &count)) {
        notice(NOTE_ERROR, "Không tải được danh sách người chơi (%d %s)", code, n >= 3 ? f[2] : "");
        return;
    }
    if (count != n - 3)
        g_printerr("caro_client: warning: LIST_PLAYERS says %d players but has %d\n", count, n - 3);

    gtk_list_store_clear(store);
    for (int i = 3; i < n; i++) {
        char *p[3];
        int elo;
        if (split_item(f[i], p, 3) != 3 || !field_int(p[1], &elo) || !is_player_status(p[2])
            || strcmp(p[0], app.username) == 0) {
            g_printerr("caro_client: warning: bad LIST_PLAYERS item %d ignored\n", i - 2);
            continue;
        }
        set_row(p[0], elo, p[2]);
    }
    update_count();
    update_challenge_button();
}

/* PLAYER_STATUS|user|status|elo: add or update the row; OFFLINE removes it. */
static void on_player_status(char **f, int n)
{
    int elo;
    if (n != 4 || !field_int(f[3], &elo)) {
        g_printerr("caro_client: warning: bad PLAYER_STATUS ignored\n");
        return;
    }
    if (strcmp(f[1], app.username) == 0)
        return;                                    /* the server never sends our own; be safe */
    if (strcmp(f[2], PS_OFFLINE) == 0) {
        GtkTreeIter it;
        if (find_row(f[1], &it))
            gtk_list_store_remove(store, &it);
    } else if (is_player_status(f[2])) {
        set_row(f[1], elo, f[2]);
    }
    update_count();
    update_challenge_button();
}

/* ---- outgoing challenge ---------------------------------------------------------------------------- */

static void out_reset(void)
{
    if (out.dialog != NULL) {
        gtk_widget_destroy(out.dialog);
        out.dialog = NULL;
    }
    out.state = OUT_NONE;
    out.cid = 0;
    out.target[0] = '\0';
}

static gboolean out_is(int cid)
{
    return (out.state == OUT_WAITING || out.state == OUT_CANCELLING) && out.cid == cid;
}

static void on_cancel_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || out.state != OUT_CANCELLING || out.cid != GPOINTER_TO_INT(data))
        return;          /* already settled by an event (accepted, declined, expired) */
    if (code == ST_OK)
        notice(NOTE_INFO, "Đã hủy lời mời tới %s", out.target);
    else
        notice(NOTE_ERROR, "Không hủy được lời mời (%d %s)", code, n >= 3 ? f[2] : "");
    out_reset();
    update_challenge_button();
}

static void on_waiting_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)response;                  /* "Hủy lời mời" or the window's close button: both cancel */
    (void)data;
    if (out.dialog == GTK_WIDGET(dialog))
        out.dialog = NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (out.state != OUT_WAITING)
        return;
    char *payload = g_strdup_printf("%s|%d", CMD_CHALLENGE_CANCEL, out.cid);
    if (net_request(payload, on_cancel_reply, GINT_TO_POINTER(out.cid))) {
        out.state = OUT_CANCELLING;
        notice(NOTE_INFO, "Đang hủy lời mời tới %s…", out.target);
    }
    g_free(payload);
}

static void show_waiting_dialog(void)
{
    GtkWidget *d = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_INFO, GTK_BUTTONS_NONE,
                                          "Đang chờ %s trả lời…", out.target);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "Bàn %d×%d, %d giây mỗi nước. Lời mời tự hết hạn sau %d giây.",
        BOARD_SIZE, BOARD_SIZE, SEC_PER_MOVE_NORMAL, CHALLENGE_EXPIRE_SEC);
    gtk_dialog_add_button(GTK_DIALOG(d), "Hủy lời mời", GTK_RESPONSE_CANCEL);
    g_signal_connect(d, "response", G_CALLBACK(on_waiting_response), NULL);
    out.dialog = d;
    gtk_widget_show_all(d);
}

static void on_challenge_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED || out.state != OUT_REQUESTING)
        return;
    int cid;
    if (code == ST_OK && n >= 3 && field_int(f[2], &cid)) {      /* 200|CHALLENGE|cid */
        out.state = OUT_WAITING;
        out.cid = cid;
        notice_clear();
        show_waiting_dialog();
        update_challenge_button();
        return;
    }

    const char *reason = n >= 3 ? f[2] : "";
    if (strcmp(reason, ERR_USER_NOT_FOUND) == 0)
        notice(NOTE_ERROR, "%s không còn trực tuyến", out.target);
    else if (strcmp(reason, ERR_PLAYER_BUSY) == 0)
        notice(NOTE_ERROR, "%s đang bận, hãy thử lại sau", out.target);
    else if (strcmp(reason, ERR_CHALLENGE_PENDING) == 0)
        notice(NOTE_ERROR, "Bạn đang có một lời mời chưa được trả lời");
    else if (strcmp(reason, ERR_BAD_ARGS) == 0)
        notice(NOTE_ERROR, "Không thể thách đấu %s", out.target);
    else if (code == ST_FORBIDDEN)
        notice(NOTE_ERROR, "Không thể thách đấu lúc này");
    else
        notice(NOTE_ERROR, "Thách đấu thất bại (%d %s)", code, reason);
    out_reset();
    update_challenge_button();
}

static void on_challenge_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char *name = NULL, *status = NULL;
    if (out.state == OUT_NONE && get_selected(&name, &status) && strcmp(status, PS_IDLE) == 0) {
        char *payload = g_strdup_printf("%s|%s|%d|%d", CMD_CHALLENGE, name, BOARD_SIZE,
                                        SEC_PER_MOVE_NORMAL);
        if (net_request(payload, on_challenge_reply, NULL)) {
            out.state = OUT_REQUESTING;
            g_strlcpy(out.target, name, sizeof out.target);
            notice(NOTE_INFO, "Đang gửi lời mời tới %s…", name);
        }
        g_free(payload);
    }
    g_free(name);
    g_free(status);
    update_challenge_button();
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    (void)data;
    update_challenge_button();
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tv;
    (void)path;
    (void)col;
    if (gtk_widget_get_sensitive(b_challenge))      /* double-click = Thách đấu */
        on_challenge_clicked(NULL, data);
}

/* ---- incoming invitations --------------------------------------------------------------------------- */

static void invite_free(gpointer p)
{
    Invite *inv = p;
    if (inv->dialog != NULL)
        gtk_widget_destroy(inv->dialog);
    g_free(inv);
}

static Invite *invite_get(int cid)
{
    return g_hash_table_lookup(invites, GINT_TO_POINTER(cid));
}

static void invite_remove(int cid)
{
    g_hash_table_remove(invites, GINT_TO_POINTER(cid));   /* destroys its dialog */
}

static void on_accept_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || code == ST_OK)
        return;                      /* 200: GAME_START follows and closes every dialog */
    Invite *inv = invite_get(GPOINTER_TO_INT(data));
    if (inv == NULL)
        return;
    const char *reason = n >= 3 ? f[2] : "";
    if (strcmp(reason, ERR_CHALLENGE_NOT_FOUND) == 0)
        notice(NOTE_ERROR, "Lời mời của %s không còn hiệu lực", inv->from);
    else if (strcmp(reason, ERR_PLAYER_BUSY) == 0)
        notice(NOTE_ERROR, "%s đã bận, không thể vào ván", inv->from);
    else
        notice(NOTE_ERROR, "Không nhận được lời mời của %s (%d %s)", inv->from, code, reason);
    invite_remove(inv->cid);
}

static void on_invite_response(GtkDialog *dialog, gint response, gpointer data)
{
    int cid = GPOINTER_TO_INT(data);
    Invite *inv = invite_get(cid);
    if (inv == NULL) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    if (inv->accepting)
        return;                      /* already answered ACCEPT: wait for GAME_START */

    gboolean accept = response == GTK_RESPONSE_ACCEPT;
    char *payload = g_strdup_printf("%s|%d|%s", CMD_CHALLENGE_REPLY, cid,
                                    accept ? ARG_ACCEPT : ARG_DECLINE);
    if (accept) {
        inv->accepting = TRUE;
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_REJECT, FALSE);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Đang vào ván…");
        net_request(payload, on_accept_reply, data);
    } else {                         /* "Từ chối" or the window's close button */
        net_request(payload, NULL, NULL);
        invite_remove(cid);
    }
    g_free(payload);
}

/* CHALLENGE_FROM|cid|from|from_elo|size|sec */
static void on_challenge_from(char **f, int n)
{
    int cid, elo, size, sec;
    if (n != 6 || !field_int(f[1], &cid) || !field_int(f[3], &elo) || !field_int(f[4], &size)
        || !field_int(f[5], &sec)) {
        g_printerr("caro_client: warning: bad CHALLENGE_FROM ignored\n");
        return;
    }
    if (invite_get(cid) != NULL)
        return;

    Invite *inv = g_new0(Invite, 1);
    inv->cid = cid;
    g_strlcpy(inv->from, f[2], sizeof inv->from);
    GtkWidget *d = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "%s (ELO %d) mời bạn chơi", inv->from, elo);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "Bàn %d×%d, %d giây mỗi nước.", size, size, sec);
    gtk_dialog_add_button(GTK_DIALOG(d), "Từ chối", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(d), "Đồng ý", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    g_signal_connect(d, "response", G_CALLBACK(on_invite_response), GINT_TO_POINTER(cid));
    inv->dialog = d;
    g_hash_table_insert(invites, GINT_TO_POINTER(cid), inv);
    gtk_widget_show_all(d);
}

/* ---- challenge outcome events ------------------------------------------------------------------------ */

/* CHALLENGE_DECLINED|cid|by_user — to the challenger */
static void on_challenge_declined(char **f, int n)
{
    int cid;
    if (n < 2 || !field_int(f[1], &cid) || !out_is(cid))
        return;
    notice(NOTE_INFO, "%s đã từ chối lời mời", n >= 3 ? f[2] : out.target);
    out_reset();
    update_challenge_button();
}

/* CHALLENGE_EXPIRED|cid — to both sides */
static void on_challenge_expired(char **f, int n)
{
    int cid;
    if (n < 2 || !field_int(f[1], &cid))
        return;
    Invite *inv = invite_get(cid);
    if (out_is(cid)) {
        notice(NOTE_INFO, "Lời mời tới %s đã hết hạn (%d giây không trả lời)", out.target,
               CHALLENGE_EXPIRE_SEC);
        out_reset();
        update_challenge_button();
    } else if (inv != NULL) {
        notice(NOTE_INFO, "Lời mời của %s đã hết hạn", inv->from);
        invite_remove(cid);
    }
}

/* CHALLENGE_CANCELLED|cid — to the other side: withdrawn, or no longer possible */
static void on_challenge_cancelled(char **f, int n)
{
    int cid;
    if (n < 2 || !field_int(f[1], &cid))
        return;
    Invite *inv = invite_get(cid);
    if (inv != NULL) {
        notice(NOTE_INFO, "%s đã hủy lời mời", inv->from);
        invite_remove(cid);
    } else if (out_is(cid)) {
        notice(NOTE_INFO, "Lời mời tới %s đã bị hủy (người đó không còn rảnh)", out.target);
        out_reset();
        update_challenge_button();
    }
}

static void close_all_dialogs(void)
{
    out_reset();
    g_hash_table_remove_all(invites);
}

/* GAME_START|gid|size|your_symbol|opponent|opp_elo|sec_per_move — to both players.
 * Also arrives on the game page after an accepted rematch: the same code starts the new game
 * (new gid, symbols swapped), and ui_game_enter() clears the board. */
static void on_game_start(char **f, int n)
{
    int gid, size, opp_elo, sec;
    if (n != 7 || !field_int(f[1], &gid) || !field_int(f[2], &size) || size < 1
        || size > BOARD_SIZE_MAX || (strcmp(f[3], SYM_X) != 0 && strcmp(f[3], SYM_O) != 0)
        || !field_int(f[5], &opp_elo) || !field_int(f[6], &sec)) {
        g_printerr("caro_client: warning: bad GAME_START ignored\n");
        return;
    }
    close_all_dialogs();             /* the server already removed every pending challenge */

    memset(&app.game, 0, sizeof app.game);
    app.game.active = TRUE;
    app.game.gid = gid;
    app.game.size = size;
    app.game.my_symbol = f[3][0];
    g_strlcpy(app.game.opponent, f[4], sizeof app.game.opponent);
    app.game.opp_elo = opp_elo;
    app.game.sec_per_move = sec;
    app.state = SESSION_IN_GAME;
    update_challenge_button();
    app_show_page("game");
}

/* ---- logout --------------------------------------------------------------------------------------- */

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
    notice(NOTE_ERROR, "Đăng xuất thất bại (%d %s)", code, n >= 3 ? f[2] : "");
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

/* ---- page ------------------------------------------------------------------------------------------ */

void ui_lobby_update_bar(void)
{
    char *markup = g_markup_printf_escaped(
        "<span size='large'><b>%s</b></span>   ·   ELO %d   ·   Thắng/Thua/Hòa %d/%d/%d",
        app.username, app.elo, app.wins, app.losses, app.draws);
    gtk_label_set_markup(GTK_LABEL(l_user), markup);
    g_free(markup);
}

/* 200|STATS|user|elo|wins|losses|draws — our own numbers, straight from the server's DB */
static void on_stats_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED)
        return;
    int elo, wins, losses, draws;
    if (code != ST_OK) {
        notice(NOTE_ERROR, "Không lấy được ELO và thành tích từ server (%d %s)", code,
               n >= 3 ? f[2] : "");
        return;
    }
    if (n < 7 || strcmp(f[2], app.username) != 0 || !field_int(f[3], &elo)
        || !field_int(f[4], &wins) || !field_int(f[5], &losses) || !field_int(f[6], &draws)) {
        notice(NOTE_ERROR, "Server trả về phản hồi STATS không hợp lệ");
        return;
    }
    app.elo = elo;
    app.wins = wins;
    app.losses = losses;
    app.draws = draws;
    ui_lobby_update_bar();
}

void ui_lobby_request_stats(void)
{
    net_request(CMD_STATS, on_stats_reply, NULL);     /* no argument = the caller's own stats */
}

void ui_lobby_enter(void)
{
    ui_lobby_update_bar();
    gtk_widget_set_sensitive(b_logout, TRUE);
    notice_clear();

    gtk_list_store_clear(store);
    update_count();
    update_challenge_button();
    net_request(CMD_LIST_PLAYERS, on_list_reply, NULL);   /* the snapshot; events do the rest */
}

void ui_lobby_reset(void)
{
    close_all_dialogs();
    gtk_list_store_clear(store);
    notice_clear();
}

static GtkTreeViewColumn *add_column(const char *title, int col, float xalign)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_sort_column_id(c, col);       /* click the header to sort */
    gtk_tree_view_column_set_expand(c, col == COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    return c;
}

GtkWidget *ui_lobby_new(void)
{
    invites = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, invite_free);
    dispatch_on(EV_PLAYER_STATUS, on_player_status);
    dispatch_on(EV_CHALLENGE_FROM, on_challenge_from);
    dispatch_on(EV_CHALLENGE_DECLINED, on_challenge_declined);
    dispatch_on(EV_CHALLENGE_EXPIRED, on_challenge_expired);
    dispatch_on(EV_CHALLENGE_CANCELLED, on_challenge_cancelled);
    dispatch_on(EV_GAME_START, on_game_start);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(page, "margin", 20, NULL);

    /* top bar: who I am + logout */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    l_user = gtk_label_new(NULL);
    gtk_widget_set_halign(l_user, GTK_ALIGN_START);
    b_logout = gtk_button_new_with_label("Đăng xuất");
    g_signal_connect(b_logout, "clicked", G_CALLBACK(on_logout_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), l_user, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), b_logout, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    l_count = gtk_label_new(NULL);
    gtk_widget_set_halign(l_count, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), l_count, FALSE, FALSE, 0);

    /* player list, kept sorted by name like LIST_PLAYERS */
    store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), COL_NAME, GTK_SORT_ASCENDING);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);                                  /* the view holds the reference */
    add_column("Tên", COL_NAME, 0.0f);
    add_column("ELO", COL_ELO, 1.0f);
    add_column("Trạng thái", COL_STATUS_TEXT, 0.0f);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
                     G_CALLBACK(on_selection_changed), NULL);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* bottom: notice + challenge */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    l_notice = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(l_notice), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l_notice), 0.0f);
    b_challenge = gtk_button_new_with_label("Thách đấu");
    gtk_style_context_add_class(gtk_widget_get_style_context(b_challenge), "suggested-action");
    g_signal_connect(b_challenge, "clicked", G_CALLBACK(on_challenge_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), l_notice, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), b_challenge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bottom, FALSE, FALSE, 0);

    if (app.fake_mode) {
        GtkWidget *hint = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hint),
            "<small>Chế độ offline: <b>han</b> nhận lời · <b>tuan</b> từ chối · <b>lan</b> không trả "
            "lời (hết hạn sau 30 giây) · <b>minh</b> đang chơi (409), rảnh sau 6 giây, offline sau "
            "15 giây. <b>Ctrl+I</b>: nhận một lời mời giả · <b>Ctrl+U</b>: lời mời giả bị hủy sau "
            "5 giây.</small>");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);
    }
    update_count();
    return page;
}
