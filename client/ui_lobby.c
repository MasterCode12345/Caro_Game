/*
 * client/ui_lobby.c — page "lobby": who is online, challenges, quick match, leaderboard
 * (docs/protocol.md §6.2, §6.4, §7).
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
 *
 * QUICK MATCH (FIND_MATCH / CANCEL_MATCH, §6.2) — states:
 *   REQUESTING (FIND_MATCH sent) -> SEARCHING (200: we are IN_QUEUE, "searching" dialog)
 *   -> CANCELLING (its Hủy button sent CANCEL_MATCH) -> 200: back to LOBBY.
 *   GAME_START ends any of them: the server paired us, the dialog closes, the game page opens.
 *   - Not possible while our own challenge waits for an answer: entering the queue removes our
 *     challenge at the server, and only the TARGET is told (CHALLENGE_CANCELLED), so our
 *     "waiting" dialog would never close. Thách đấu is locked the whole time we search.
 *   - Entering the queue also removes the invitations we received (§6.2): their dialogs are
 *     closed at 200|FIND_MATCH, and a CHALLENGE_CANCELLED for them is handled as usual.
 *   - RACE: the server pairs players in its 1-second tick. If it pairs us just before our
 *     CANCEL_MATCH arrives, it has already sent GAME_START, and CANCEL_MATCH (state G now) gets
 *     403 forbidden_state. TCP keeps the order, so we see GAME_START first: it resets the quick
 *     match state, and the late 403 no longer matches a CANCELLING state — it is dropped and we
 *     stay in the game. The serial number makes this check exact.
 *
 * LEADERBOARD|10 opens a dialog (rank, name, ELO, W/L/D, our own row in bold). Lịch sử opens
 * the history page (ui_history.c). Both are allowed in LOBBY and IN_QUEUE (§6.4).
 */
#include <stdarg.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
#include "net.h"

enum { COL_NAME, COL_ELO, COL_STATUS, COL_STATUS_TEXT, N_COLS };

typedef enum { OUT_NONE, OUT_REQUESTING, OUT_WAITING, OUT_CANCELLING } OutState;
typedef enum { MM_NONE, MM_REQUESTING, MM_SEARCHING, MM_CANCELLING } MatchState;
enum { LB_RANK, LB_NAME, LB_ELO, LB_WLD, LB_WEIGHT, LB_N_COLS };

typedef struct {
    int        cid;
    char       from[USERNAME_MAX + 1];
    GtkWidget *dialog;
    gboolean   accepting;          /* ACCEPT sent, waiting for 200 + GAME_START */
} Invite;

static GtkWidget    *l_user, *l_count, *l_notice, *b_logout, *b_challenge, *tree;
static GtkWidget    *b_history, *b_leaderboard, *b_quick;
static GtkListStore *store;
static GHashTable   *invites;      /* cid -> Invite* (incoming) */
static gboolean      logout_sent;  /* LOGOUT sent, response not received yet */
static struct {
    OutState   state;
    int        cid;
    char       target[USERNAME_MAX + 1];
    GtkWidget *dialog;
} out;                             /* our outgoing challenge */
static struct {
    MatchState state;
    guint      serial;             /* +1 per FIND_MATCH / CANCEL_MATCH: a late response is spotted */
    GtkWidget *dialog;             /* "Đang tìm đối thủ…" with Hủy */
    guint      tick_id;            /* 1-second timer for "Đã chờ N giây" */
    gint64     started_us;         /* when the server put us in the queue */
} mm;                              /* quick match */
static struct {
    gboolean      pending;         /* LEADERBOARD sent, response not received yet */
    GtkWidget    *dialog;
    GtkListStore *store;           /* owned by the dialog's tree view */
    GtkWidget    *note;
} lb;

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

/* "<what>: <the Vietnamese sentence for this error response>" (dispatch.c error_text()). */
static void error_notice(const char *what, int code, char **f, int n)
{
    char *why = error_text(code, f, n);
    notice(NOTE_ERROR, "%s: %s", what, why);
    g_free(why);
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

static gboolean invite_accepting(gpointer key, gpointer value, gpointer data);

/* Thách đấu: an IDLE player selected, we are in the lobby (not searching), no challenge of ours
 * pending. Tìm trận nhanh: in the lobby, not searching yet, no challenge of ours pending and
 * no invitation being accepted (its GAME_START is on the way). Đăng xuất: not twice, and not
 * while an accepted invitation's GAME_START is on the way (LOGOUT would then reach the server
 * in a game and get 403). Nothing at all once LOGOUT is sent. */
static void update_buttons(void)
{
    gboolean accepting = g_hash_table_find(invites, invite_accepting, NULL) != NULL;
    gboolean idle = app.state == SESSION_LOBBY && mm.state == MM_NONE && out.state == OUT_NONE
                    && !logout_sent;
    char *name = NULL, *status = NULL;
    gboolean ok = idle && get_selected(&name, &status) && strcmp(status, PS_IDLE) == 0;
    gtk_widget_set_sensitive(b_challenge, ok);
    g_free(name);
    g_free(status);
    gtk_widget_set_sensitive(b_quick, idle && !accepting);
    gtk_widget_set_sensitive(b_leaderboard, !lb.pending && !logout_sent);
    gtk_widget_set_sensitive(b_history, !logout_sent);
    gtk_widget_set_sensitive(b_logout, !logout_sent && !accepting);
}

/* 200|LIST_PLAYERS|n|user,elo,status|... */
static void on_list_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED)
        return;
    int count;
    if (code != ST_OK) {
        error_notice("Không tải được danh sách người chơi", code, f, n);
        return;
    }
    if (n < 3 || !field_int(f[2], &count)) {
        notice(NOTE_ERROR, "Server trả về phản hồi LIST_PLAYERS không hợp lệ");
        return;
    }
    if (count != n - 3)
        g_printerr("caro_client: warning: LIST_PLAYERS says %d players but has %d\n", count, n - 3);

    gtk_list_store_clear(store);
    for (int i = 3; i < n; i++) {
        char *p[3];
        int elo;
        if (item_split(f[i], p, 3) != 3 || !field_int(p[1], &elo) || !is_player_status(p[2])
            || strcmp(p[0], app.username) == 0) {
            g_printerr("caro_client: warning: bad LIST_PLAYERS item %d ignored\n", i - 2);
            continue;
        }
        set_row(p[0], elo, p[2]);
    }
    update_count();
    update_buttons();
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
    update_buttons();
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
        error_notice("Không hủy được lời mời", code, f, n);
    out_reset();
    update_buttons();
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
        update_buttons();
        return;
    }

    const char *reason = n >= 3 ? f[2] : "";
    if (strcmp(reason, ERR_USER_NOT_FOUND) == 0)
        notice(NOTE_ERROR, "%s không còn trực tuyến", out.target);
    else if (strcmp(reason, ERR_PLAYER_BUSY) == 0)
        notice(NOTE_ERROR, "%s đang bận, hãy thử lại sau", out.target);
    else if (strcmp(reason, ERR_CHALLENGE_PENDING) == 0)
        notice(NOTE_ERROR, "Bạn đang có một lời mời chưa được trả lời");
    else
        error_notice("Không thách đấu được", code, f, n);
    out_reset();
    update_buttons();
}

static void on_challenge_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char *name = NULL, *status = NULL;
    if (app.state == SESSION_LOBBY && mm.state == MM_NONE && out.state == OUT_NONE
        && get_selected(&name, &status) && strcmp(status, PS_IDLE) == 0) {
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
    update_buttons();
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    (void)data;
    update_buttons();
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

static gboolean invite_accepting(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    return ((Invite *)value)->accepting;
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
    else {
        char *what = g_strdup_printf("Không nhận được lời mời của %s", inv->from);
        error_notice(what, code, f, n);
        g_free(what);
    }
    invite_remove(inv->cid);
    update_buttons();
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
        update_buttons();            /* no quick match while GAME_START is on the way */
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
    update_buttons();
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
        update_buttons();
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
        if (app.state == SESSION_IN_QUEUE)
            notice(NOTE_INFO, "Lời mời của %s đã bị hủy vì bạn đang tìm trận", inv->from);
        else
            notice(NOTE_INFO, "%s đã hủy lời mời", inv->from);
        invite_remove(cid);
        update_buttons();
    } else if (out_is(cid)) {
        notice(NOTE_INFO, "Lời mời tới %s đã bị hủy (người đó không còn rảnh)", out.target);
        out_reset();
        update_buttons();
    }
}

/* ---- quick match (FIND_MATCH / CANCEL_MATCH) --------------------------------------------------------- */

static void mm_close_dialog(void)
{
    if (mm.tick_id != 0) {
        g_source_remove(mm.tick_id);
        mm.tick_id = 0;
    }
    GtkWidget *d = mm.dialog;
    if (d != NULL) {
        mm.dialog = NULL;                /* first: nothing may use it while it is destroyed */
        gtk_widget_destroy(d);
    }
}

static void mm_reset(void)
{
    mm_close_dialog();
    mm.state = MM_NONE;
}

static void search_dialog_update(void)
{
    if (mm.dialog == NULL)
        return;
    int waited = (int)((g_get_monotonic_time() - mm.started_us) / G_USEC_PER_SEC);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(mm.dialog),
        "Đã chờ %d giây. Server ghép bạn với người có ELO gần bạn; chờ càng lâu, khoảng ELO "
        "cho phép càng rộng.\nVán ghép: bàn %d×%d, %d giây mỗi nước.",
        waited, BOARD_SIZE, BOARD_SIZE, MATCH_SEC_PER_MOVE);
}

static gboolean on_search_tick(gpointer data)
{
    (void)data;
    search_dialog_update();
    return G_SOURCE_CONTINUE;
}

static void on_cancel_match_reply(int code, char **f, int n, gpointer data);
static void show_search_dialog(void);

static void on_search_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)dialog;
    (void)response;                  /* "Hủy" or the window's close button: both cancel */
    (void)data;
    mm_close_dialog();
    if (mm.state != MM_SEARCHING)
        return;
    if (net_request(CMD_CANCEL_MATCH, on_cancel_match_reply, GUINT_TO_POINTER(++mm.serial))) {
        mm.state = MM_CANCELLING;
        notice(NOTE_INFO, "Đang hủy tìm trận…");
    }
}

static void show_search_dialog(void)
{
    GtkWidget *d = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_INFO, GTK_BUTTONS_NONE,
                                          "Đang tìm đối thủ…");
    gtk_dialog_add_button(GTK_DIALOG(d), "Hủy", GTK_RESPONSE_CANCEL);
    g_signal_connect(d, "response", G_CALLBACK(on_search_response), NULL);
    mm.dialog = d;
    search_dialog_update();
    mm.tick_id = g_timeout_add_seconds(1, on_search_tick, NULL);
    gtk_widget_show_all(d);
}

/* 200|CANCEL_MATCH: we are back in the lobby. 403 after a GAME_START: see RACE above. */
static void on_cancel_match_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || mm.state != MM_CANCELLING || mm.serial != GPOINTER_TO_UINT(data))
        return;                      /* GAME_START came first and reset mm: keep the game page */
    if (code == ST_OK || code == ST_FORBIDDEN) {
        /* 403 without a GAME_START before it: the server has us in no queue and no game */
        mm_reset();
        app.state = SESSION_LOBBY;
        if (code == ST_OK)
            notice(NOTE_INFO, "Đã hủy tìm trận");
        else
            notice(NOTE_ERROR, "Server cho biết bạn không còn trong hàng chờ");
    } else {                         /* not cancelled: we are still in the queue */
        mm.state = MM_SEARCHING;
        error_notice("Không hủy được tìm trận", code, f, n);
        show_search_dialog();
    }
    update_buttons();
}

/* 200|FIND_MATCH: we are IN_QUEUE; GAME_START comes when the server pairs us. */
static void on_find_match_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || mm.state != MM_REQUESTING || mm.serial != GPOINTER_TO_UINT(data))
        return;
    if (code != ST_OK) {
        mm_reset();
        error_notice("Không tìm trận được", code, f, n);
        update_buttons();
        return;
    }
    mm.state = MM_SEARCHING;
    mm.started_us = g_get_monotonic_time();
    app.state = SESSION_IN_QUEUE;
    /* §6.2: entering the queue removed every invitation we had received. */
    guint dropped = g_hash_table_size(invites);
    g_hash_table_remove_all(invites);
    if (dropped > 0)
        notice(NOTE_INFO, "Đang tìm trận: %u lời mời đang chờ đã bị hủy", dropped);
    else
        notice_clear();
    show_search_dialog();
    update_buttons();
}

static void on_quick_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (app.state != SESSION_LOBBY || mm.state != MM_NONE || out.state != OUT_NONE)
        return;
    if (net_request(CMD_FIND_MATCH, on_find_match_reply, GUINT_TO_POINTER(++mm.serial))) {
        mm.state = MM_REQUESTING;
        notice(NOTE_INFO, "Đang vào hàng chờ…");
    }
    update_buttons();
}

/* ---- leaderboard (LEADERBOARD|10) ------------------------------------------------------------------- */

static void lb_close(void)
{
    GtkWidget *d = lb.dialog;
    if (d != NULL) {
        lb.dialog = NULL;
        lb.store = NULL;                 /* destroyed with the dialog's tree view */
        lb.note = NULL;
        gtk_widget_destroy(d);
    }
}

static void on_lb_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)dialog;
    (void)response;
    (void)data;
    lb_close();
}

static void lb_add_column(GtkWidget *view, const char *title, int col, float xalign)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                                    "weight", LB_WEIGHT, NULL);
    gtk_tree_view_column_set_alignment(c, xalign);
    gtk_tree_view_column_set_expand(c, col == LB_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}

static void lb_create(void)
{
    GtkWidget *d = gtk_dialog_new_with_buttons("Bảng xếp hạng", app.window,
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "Đóng", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, 420);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_box_set_spacing(GTK_BOX(box), 8);
    g_object_set(box, "margin", 12, NULL);

    GtkWidget *title = gtk_label_new(NULL);
    char *markup = g_strdup_printf("<span size='large'><b>Top %d theo ELO</b></span>",
                                   LEADERBOARD_DEFAULT);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    lb.store = gtk_list_store_new(LB_N_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                  G_TYPE_INT);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(lb.store));
    g_object_unref(lb.store);                               /* the view holds the reference */
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
                                GTK_SELECTION_NONE);
    lb_add_column(view, "Hạng", LB_RANK, 1.0f);
    lb_add_column(view, "Tên", LB_NAME, 0.0f);
    lb_add_column(view, "ELO", LB_ELO, 1.0f);
    lb_add_column(view, "Thắng/Thua/Hòa", LB_WLD, 1.0f);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    lb.note = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lb.note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(lb.note), TRUE);
    gtk_box_pack_start(GTK_BOX(box), lb.note, FALSE, FALSE, 0);

    g_signal_connect(d, "response", G_CALLBACK(on_lb_response), NULL);
    lb.dialog = d;
}

/* 200|LEADERBOARD|k|rank,user,elo,wins,losses,draws|... */
static void on_lb_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    lb.pending = FALSE;
    if (code == RESP_DISCONNECTED)
        return;
    update_buttons();
    if (app.state != SESSION_LOBBY && app.state != SESSION_IN_QUEUE)
        return;                      /* GAME_START came first: no table over the game page */
    int k;
    if (code != ST_OK) {
        error_notice("Không tải được bảng xếp hạng", code, f, n);
        return;
    }
    if (n < 3 || !field_int(f[2], &k)) {
        notice(NOTE_ERROR, "Server trả về phản hồi LEADERBOARD không hợp lệ");
        return;
    }
    if (k != n - 3)
        g_printerr("caro_client: warning: LEADERBOARD says %d rows but has %d\n", k, n - 3);

    if (lb.dialog == NULL)
        lb_create();
    gtk_list_store_clear(lb.store);
    gboolean me_listed = FALSE;
    for (int i = 3; i < n; i++) {
        char *p[6];
        int rank, elo, w, l, d;
        if (item_split(f[i], p, 6) != 6 || !field_int(p[0], &rank) || !field_int(p[2], &elo)
            || !field_int(p[3], &w) || !field_int(p[4], &l) || !field_int(p[5], &d)) {
            g_printerr("caro_client: warning: bad LEADERBOARD item %d ignored\n", i - 2);
            continue;
        }
        gboolean me = strcmp(p[1], app.username) == 0;
        me_listed = me_listed || me;
        char wld[48];
        snprintf(wld, sizeof wld, "%d/%d/%d", w, l, d);
        gtk_list_store_insert_with_values(lb.store, NULL, -1, LB_RANK, rank, LB_NAME, p[1],
                                          LB_ELO, elo, LB_WLD, wld,
                                          LB_WEIGHT, me ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, -1);
    }
    char *note = me_listed
        ? g_strdup("Dòng in đậm là bạn.")
        : g_strdup_printf("Bạn (ELO %d) chưa có mặt trong top %d.", app.elo, LEADERBOARD_DEFAULT);
    gtk_label_set_text(GTK_LABEL(lb.note), note);
    g_free(note);
    gtk_widget_show_all(lb.dialog);
    gtk_window_present(GTK_WINDOW(lb.dialog));
}

static void on_leaderboard_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (lb.pending)
        return;
    char *payload = g_strdup_printf("%s|%d", CMD_LEADERBOARD, LEADERBOARD_DEFAULT);
    if (net_request(payload, on_lb_reply, NULL))
        lb.pending = TRUE;
    g_free(payload);
    update_buttons();
}

static void on_history_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    app_show_page("history");        /* HISTORY is allowed in LOBBY and IN_QUEUE (§6.4) */
}

static void close_all_dialogs(void)
{
    out_reset();
    g_hash_table_remove_all(invites);
    mm_reset();
    lb_close();
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
    update_buttons();
    app_show_page("game");
}

/* ---- logout --------------------------------------------------------------------------------------- */

static void on_logout_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    if (code == RESP_DISCONNECTED)
        return;                        /* net_on_lost() already went back to login */
    if (code == ST_OK) {
        logout_sent = FALSE;
        net_close();                   /* LOGOUT ends the session; the next login reconnects */
        app_go_login(NULL);
        return;
    }
    logout_sent = FALSE;
    update_buttons();
    error_notice("Không đăng xuất được", code, f, n);
}

static void on_logout_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (logout_sent)
        return;
    if (!net_request(CMD_LOGOUT, on_logout_reply, NULL)) {
        net_close();
        app_go_login(NULL);
        return;
    }
    logout_sent = TRUE;
    update_buttons();
}

/* ---- page ------------------------------------------------------------------------------------------ */

void ui_lobby_update_bar(void)
{
    char *markup = g_markup_printf_escaped(
        "<span size='large' insert_hyphens='false'><b>%s</b></span>   ·   ELO %d   ·   "
        "Thắng/Thua/Hòa %d/%d/%d",
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
        error_notice("Không lấy được ELO và thành tích từ server", code, f, n);
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
    logout_sent = FALSE;
    notice_clear();

    gtk_list_store_clear(store);
    update_count();
    update_buttons();
    net_request(CMD_LIST_PLAYERS, on_list_reply, NULL);   /* the snapshot; events do the rest */
}

void ui_lobby_reset(void)
{
    close_all_dialogs();
    lb.pending = FALSE;
    logout_sent = FALSE;
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

    /* top bar: who I am + history, leaderboard, logout */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    l_user = gtk_label_new(NULL);
    gtk_widget_set_halign(l_user, GTK_ALIGN_START);
    /* May wrap, so a 20-character name cannot push the three buttons out of a narrow window. */
    gtk_label_set_line_wrap(GTK_LABEL(l_user), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l_user), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(l_user), 0.0f);
    b_history = gtk_button_new_with_label("Lịch sử");
    g_signal_connect(b_history, "clicked", G_CALLBACK(on_history_clicked), NULL);
    b_leaderboard = gtk_button_new_with_label("Bảng xếp hạng");
    g_signal_connect(b_leaderboard, "clicked", G_CALLBACK(on_leaderboard_clicked), NULL);
    b_logout = gtk_button_new_with_label("Đăng xuất");
    g_signal_connect(b_logout, "clicked", G_CALLBACK(on_logout_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), l_user, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), b_logout, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), b_leaderboard, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), b_history, FALSE, FALSE, 0);
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

    /* bottom: notice + quick match + challenge */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    l_notice = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(l_notice), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l_notice), 0.0f);
    b_quick = gtk_button_new_with_label("Tìm trận nhanh");
    g_signal_connect(b_quick, "clicked", G_CALLBACK(on_quick_clicked), NULL);
    b_challenge = gtk_button_new_with_label("Thách đấu");
    gtk_style_context_add_class(gtk_widget_get_style_context(b_challenge), "suggested-action");
    g_signal_connect(b_challenge, "clicked", G_CALLBACK(on_challenge_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), l_notice, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), b_challenge, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), b_quick, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bottom, FALSE, FALSE, 0);

    if (app.fake_mode) {
        GtkWidget *hint = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hint),
            "<small>Chế độ offline: <b>han</b> nhận lời · <b>tuan</b> từ chối · <b>lan</b> không trả "
            "lời (hết hạn sau 30 giây) · <b>minh</b> đang chơi (409), rảnh sau 6 giây, offline sau "
            "15 giây. <b>Ctrl+I</b>: nhận một lời mời giả · <b>Ctrl+U</b>: lời mời giả bị hủy sau "
            "5 giây. <b>Tìm trận nhanh</b>: ghép sau khoảng 3 giây · <b>Ctrl+M</b>: lần Hủy tìm "
            "trận tiếp theo tới server ngay sau khi đã ghép xong (thử tranh chấp).</small>");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);
    }
    update_count();
    return page;
}
