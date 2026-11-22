/*
 * client/ui_history.c — page "history": our finished games (HISTORY) and the replay viewer
 * (REPLAY), docs/protocol.md §6.4.
 * Owner: Nghĩa.
 *
 * The page holds two views in an inner GtkStack: "list" and "replay".
 *
 * LIST — HISTORY|page, 10 games per page, page 1 = the newest games.
 *   The server sends at most HISTORY_PAGE_SIZE rows. Fewer than 10 means this is the last page,
 *   so "Trang sau" is locked; with exactly 10 games on the last page the next page just comes
 *   back empty. "Trang trước" is locked on page 1. While a HISTORY or REPLAY request waits for
 *   its response, the page buttons are locked and clicks on the list are ignored: one click
 *   sends one request.
 *
 * REPLAY — one click on a row sends REPLAY|mid. The response lists every move in play order,
 *   X first, so move k (counting from 1) is X when k is odd: the moves need no symbol. The
 *   board at step k is simply the first k moves, so ⏮ ◀ ▶ ⏭ only change k and the board is
 *   redrawn from scratch (at most 225 cells, nothing to undo). It is a second BoardView with
 *   interactive = FALSE: clicks and hover do nothing, it can only be watched. When the last move
 *   of a FIVE game is shown, the winning line is found again on the board and drawn in green
 *   (REPLAY does not carry it). Any logged-in user may replay any match (§6.4).
 *   Tự chạy = g_timeout_add(700 ms): one move per tick, it stops by itself at the last move.
 *   Its source is removed with g_source_remove() by every way out: a step button, "Về danh
 *   sách", leaving the page (app_show_page() calls ui_history_leave(), also when GAME_START
 *   takes us into a game while we watch from the queue) and the end of the session.
 *
 * LATE RESPONSES — `visit` goes up each time the page is entered or left, and every request
 *   carries the visit it was sent in. A response from an earlier visit (the user went back to
 *   the lobby before it came) is dropped. dispatch.c has already popped it from the FIFO, so
 *   the responses that follow still meet the right callbacks.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "board_view.h"
#include "dispatch.h"
#include "net.h"

#define AUTO_PLAY_MS     700
#define SIDE_WIDTH       250
#define SIDE_WRAP_CHARS  30
#define MAX_MOVES        (BOARD_SIZE_MAX * BOARD_SIZE_MAX)

enum { H_MID, H_OPP, H_SYM, H_SYM_COLOR, H_RESULT, H_RESULT_COLOR, H_REASON, H_TIME, H_N_COLS };

static GtkWidget *views;           /* inner GtkStack: "list" | "replay" */
static guint      visit;           /* +1 on enter and on leave: late responses are dropped */
static gboolean   loading;         /* a HISTORY or REPLAY request waits for its response */

/* list */
static GtkWidget    *tree, *l_page, *l_list_note, *b_prev_page, *b_next_page;
static GtkListStore *store;
static int           cur_page = 1;
static int           req_page;     /* the page asked for by the HISTORY request in flight */
static int           rows_on_page; /* rows the server sent for cur_page */
static char          req_time[64]; /* "Thời gian" of the row whose REPLAY is in flight */

/* replay */
static BoardView *rbv;
static GtkWidget *l_title, *l_x, *l_o, *l_result, *l_pos, *l_last, *l_replay_note;
static GtkWidget *b_first, *b_back, *b_fwd, *b_end, *b_auto;
static struct {
    int   mid, size;
    char  x_user[USERNAME_MAX + 1], o_user[USERNAME_MAX + 1];
    char  result[16], reason[24];
    int   n;                       /* moves received */
    int   moves[MAX_MOVES][2];
    int   pos;                     /* moves shown on the board: 0..n */
    guint auto_id;                 /* Tự chạy: g_timeout_add() source, 0 = stopped */
} rp;

/* ---- small helpers ------------------------------------------------------------------------------- */

typedef enum { NOTE_INFO, NOTE_ERROR } NoteKind;

static void note(GtkWidget *label, NoteKind kind, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static void note(GtkWidget *label, NoteKind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    char *markup = g_markup_printf_escaped("<span foreground='%s'>%s</span>",
                                           kind == NOTE_ERROR ? "#c01c28" : "#1c71d8", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(text);
}

/* "<what>: <the Vietnamese sentence for this error response>" under the list. */
static void error_note(const char *what, int code, char **f, int n)
{
    char *why = error_text(code, f, n);
    note(l_list_note, NOTE_ERROR, "%s: %s", what, why);
    g_free(why);
}

static const char *symbol_markup(char s)
{
    return s == 'X' ? "<span foreground='#b31414'><b>X</b></span>"
                    : "<span foreground='#1a59cc'><b>O</b></span>";
}

/* "2026-11-21 21:40" (protocol.md §6.4) -> "21/11/2026 21:40"; anything else is shown as is. */
static void format_time(const char *s, char *out, size_t len)
{
    int y, mo, d, h, mi;
    char extra;
    if (sscanf(s, "%4d-%2d-%2d %2d:%2d%c", &y, &mo, &d, &h, &mi, &extra) == 5)
        snprintf(out, len, "%02d/%02d/%04d %02d:%02d", d, mo, y, h, mi);
    else
        snprintf(out, len, "%s", s);
}

/* The reason of one of OUR games, from our side (HISTORY outcome is ours). */
static const char *my_reason_text(const char *reason, const char *outcome)
{
    gboolean won = strcmp(outcome, OUT_WIN) == 0;
    if (strcmp(reason, END_FIVE) == 0)
        return "5 quân liên tiếp";
    if (strcmp(reason, END_BOARD_FULL) == 0)
        return "Bàn cờ đã kín";
    if (strcmp(reason, END_RESIGN) == 0)
        return won ? "Đối thủ xin thua" : "Bạn xin thua";
    if (strcmp(reason, END_DRAW_AGREED) == 0)
        return "Hai bên đồng ý hòa";
    if (strcmp(reason, END_TIMEOUT) == 0)
        return won ? "Đối thủ hết giờ" : "Bạn hết giờ";
    if (strcmp(reason, END_DISCONNECT) == 0)
        return won ? "Đối thủ mất kết nối" : "Bạn mất kết nối";
    return reason;
}

/* ---- list: HISTORY|page -------------------------------------------------------------------------- */

static void update_list_controls(void)
{
    char page[32];
    snprintf(page, sizeof page, "Trang %d", cur_page);
    gtk_label_set_text(GTK_LABEL(l_page), page);
    gtk_widget_set_sensitive(b_prev_page, !loading && cur_page > 1);
    gtk_widget_set_sensitive(b_next_page, !loading && rows_on_page >= HISTORY_PAGE_SIZE);
}

/* One item "mid,opponent,my_symbol,outcome,reason,ended_at" -> one row. FALSE if malformed. */
static gboolean add_history_row(char *item)
{
    char *p[6];
    int mid;
    if (item_split(item, p, 6) != 6 || !field_int(p[0], &mid) || p[1][0] == '\0'
        || (strcmp(p[2], SYM_X) != 0 && strcmp(p[2], SYM_O) != 0) || p[4][0] == '\0')
        return FALSE;
    const char *result, *color;
    if (strcmp(p[3], OUT_WIN) == 0) {
        result = "Thắng";
        color = "#26a269";
    } else if (strcmp(p[3], OUT_LOSS) == 0) {
        result = "Thua";
        color = "#c01c28";
    } else if (strcmp(p[3], OUT_DRAW) == 0) {
        result = "Hòa";
        color = "#3d3846";
    } else {
        return FALSE;
    }
    char when[64];
    format_time(p[5], when, sizeof when);
    gtk_list_store_insert_with_values(store, NULL, -1, H_MID, mid, H_OPP, p[1], H_SYM, p[2],
                                      H_SYM_COLOR, p[2][0] == 'X' ? "#b31414" : "#1a59cc",
                                      H_RESULT, result, H_RESULT_COLOR, color,
                                      H_REASON, my_reason_text(p[4], p[3]), H_TIME, when, -1);
    return TRUE;
}

/* 200|HISTORY|page|n|mid,opponent,my_symbol,outcome,reason,ended_at|... */
static void on_history_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || GPOINTER_TO_UINT(data) != visit)
        return;                      /* connection gone, or sent during an earlier visit */
    loading = FALSE;
    int page, count;
    if (code != ST_OK || n < 4 || !field_int(f[2], &page) || !field_int(f[3], &count)) {
        if (code != ST_OK)
            error_note("Không tải được lịch sử", code, f, n);
        else
            note(l_list_note, NOTE_ERROR, "Server trả về phản hồi HISTORY không hợp lệ");
        update_list_controls();
        return;
    }
    if (page != req_page)
        g_printerr("caro_client: warning: HISTORY page %d asked, page %d received\n", req_page, page);
    if (count != n - 4)
        g_printerr("caro_client: warning: HISTORY says %d games but has %d\n", count, n - 4);

    cur_page = req_page;
    rows_on_page = n - 4;            /* what the server sent decides "is there a next page" */
    gtk_list_store_clear(store);
    for (int i = 4; i < n; i++)
        if (!add_history_row(f[i]))
            g_printerr("caro_client: warning: bad HISTORY item %d ignored\n", i - 3);

    if (rows_on_page == 0 && cur_page == 1)
        note(l_list_note, NOTE_INFO, "Bạn chưa chơi ván nào.");
    else if (rows_on_page == 0)
        note(l_list_note, NOTE_INFO, "Trang %d không còn ván nào.", cur_page);
    else
        note(l_list_note, NOTE_INFO, "Bấm vào một ván để xem lại.");
    update_list_controls();
}

static void request_page(int page)
{
    if (loading)
        return;
    char *payload = g_strdup_printf("%s|%d", CMD_HISTORY, page);
    if (net_request(payload, on_history_reply, GUINT_TO_POINTER(visit))) {
        loading = TRUE;
        req_page = page;
        note(l_list_note, NOTE_INFO, "Đang tải trang %d…", page);
    }
    g_free(payload);
    update_list_controls();
}

static void on_prev_page(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (cur_page > 1)
        request_page(cur_page - 1);
}

static void on_next_page(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (rows_on_page >= HISTORY_PAGE_SIZE)
        request_page(cur_page + 1);
}

static void on_lobby_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    app_show_page("lobby");          /* calls ui_history_leave() */
}

/* ---- replay: drawing and controls ---------------------------------------------------------------- */

/* The five cells of the line of 5+ through (r,c), or 0 if there is none. Same window rule as the
 * server (§7): five consecutive cells that include (r,c). */
static int five_through(int r, int c, int line[WIN_LENGTH][2])
{
    static const int dirs[4][2] = { { 0, 1 }, { 1, 0 }, { 1, 1 }, { 1, -1 } };
    char m = board_view_get(rbv, r, c);
    if (m != 'X' && m != 'O')
        return 0;
    for (int d = 0; d < 4; d++) {
        int dr = dirs[d][0], dc = dirs[d][1], back = 0, fwd = 0;
        while (board_view_get(rbv, r - (back + 1) * dr, c - (back + 1) * dc) == m)
            back++;
        while (board_view_get(rbv, r + (fwd + 1) * dr, c + (fwd + 1) * dc) == m)
            fwd++;
        if (back + fwd + 1 < WIN_LENGTH)
            continue;
        int start = -MIN(back, WIN_LENGTH - 1);      /* the window ends at or after (r,c) */
        for (int i = 0; i < WIN_LENGTH; i++) {
            line[i][0] = r + (start + i) * dr;
            line[i][1] = c + (start + i) * dc;
        }
        return WIN_LENGTH;
    }
    return 0;
}

static void update_replay_controls(void)
{
    gtk_widget_set_sensitive(b_first, rp.pos > 0);
    gtk_widget_set_sensitive(b_back, rp.pos > 0);
    gtk_widget_set_sensitive(b_fwd, rp.pos < rp.n);
    gtk_widget_set_sensitive(b_end, rp.pos < rp.n);
    gtk_widget_set_sensitive(b_auto, rp.n > 0);
    gtk_button_set_label(GTK_BUTTON(b_auto), rp.auto_id != 0 ? "⏸ Dừng" : "Tự chạy");
}

/* The board after the first rp.pos moves, X on odd move numbers. */
static void replay_draw(void)
{
    board_view_reset(rbv, rp.size);
    for (int i = 0; i < rp.pos; i++)
        board_view_set(rbv, rp.moves[i][0], rp.moves[i][1], i % 2 == 0 ? 'X' : 'O');

    char *pos = g_strdup_printf("<span size='x-large'><b>Nước %d / %d</b></span>", rp.pos, rp.n);
    gtk_label_set_markup(GTK_LABEL(l_pos), pos);
    g_free(pos);

    if (rp.pos == 0) {
        gtk_label_set_text(GTK_LABEL(l_last), "Bàn cờ trống. Bấm ▶ hoặc Tự chạy.");
    } else {
        int r = rp.moves[rp.pos - 1][0], c = rp.moves[rp.pos - 1][1];
        board_view_set_last(rbv, r, c);
        int line[WIN_LENGTH][2];
        if (rp.pos == rp.n && strcmp(rp.reason, END_FIVE) == 0)
            board_view_set_win_line(rbv, (const int (*)[2])line, five_through(r, c, line));
        char *last = g_strdup_printf("%s đi hàng %d, cột %d", symbol_markup(rp.pos % 2 == 1 ? 'X' : 'O'),
                                     r, c);
        gtk_label_set_markup(GTK_LABEL(l_last), last);
        g_free(last);
    }
    update_replay_controls();
}

static void auto_stop(void)
{
    if (rp.auto_id != 0) {
        g_source_remove(rp.auto_id);
        rp.auto_id = 0;
    }
    update_replay_controls();
}

static gboolean on_auto_tick(gpointer data)
{
    (void)data;
    if (rp.pos < rp.n)
        rp.pos++;
    if (rp.pos < rp.n) {
        replay_draw();
        return G_SOURCE_CONTINUE;
    }
    rp.auto_id = 0;                  /* the last move: stop by ourselves */
    replay_draw();
    return G_SOURCE_REMOVE;
}

static void on_auto_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (rp.auto_id != 0) {
        auto_stop();
        return;
    }
    if (rp.n == 0)
        return;
    if (rp.pos >= rp.n)              /* at the end: play again from the empty board */
        rp.pos = 0;
    rp.auto_id = g_timeout_add(AUTO_PLAY_MS, on_auto_tick, NULL);
    replay_draw();
}

/* ⏮ ◀ ▶ ⏭: data = the step (-1, +1) or a big jump (G_MININT / G_MAXINT). Stops Tự chạy. */
static void on_step_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    auto_stop();
    int step = GPOINTER_TO_INT(data);
    if (step == G_MININT)
        rp.pos = 0;
    else if (step == G_MAXINT)
        rp.pos = rp.n;
    else
        rp.pos = CLAMP(rp.pos + step, 0, rp.n);
    replay_draw();
}

static void on_back_to_list(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    auto_stop();
    gtk_stack_set_visible_child_name(GTK_STACK(views), "list");
}

/* ---- replay: REPLAY|mid -------------------------------------------------------------------------- */

static void set_replay_player(GtkWidget *label, char sym, const char *name)
{
    char *esc = g_markup_escape_text(name, -1);
    char *markup = g_strdup_printf("<span size='large' insert_hyphens='false'>%s  <b>%s</b>"
                                   "</span>%s", symbol_markup(sym),
                                   esc, strcmp(name, app.username) == 0 ? "  (bạn)" : "");
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(esc);
}

/* Result and reason from nobody's side: the viewer may have played neither colour. */
static void set_replay_result(void)
{
    gboolean x_won = strcmp(rp.result, RES_X_WIN) == 0, o_won = strcmp(rp.result, RES_O_WIN) == 0;
    const char *winner = x_won ? rp.x_user : o_won ? rp.o_user : NULL;
    const char *loser = x_won ? rp.o_user : o_won ? rp.x_user : NULL;
    char *why;
    if (strcmp(rp.reason, END_FIVE) == 0)
        why = g_strdup("5 quân liên tiếp");
    else if (strcmp(rp.reason, END_BOARD_FULL) == 0)
        why = g_strdup("Bàn cờ đã kín mà không ai có 5 quân liên tiếp");
    else if (strcmp(rp.reason, END_DRAW_AGREED) == 0)
        why = g_strdup("Hai bên đồng ý hòa");
    else if (strcmp(rp.reason, END_RESIGN) == 0)
        why = g_strdup_printf("%s xin thua", loser != NULL ? loser : "Một bên");
    else if (strcmp(rp.reason, END_TIMEOUT) == 0)
        why = g_strdup_printf("%s hết thời gian suy nghĩ", loser != NULL ? loser : "Một bên");
    else if (strcmp(rp.reason, END_DISCONNECT) == 0)
        why = g_strdup_printf("%s mất kết nối", loser != NULL ? loser : "Một bên");
    else
        why = g_strdup(rp.reason);

    char *head = winner != NULL
        ? g_markup_printf_escaped("<span size='x-large' foreground='#26a269'><b>%s thắng</b></span>",
                                  winner)
        : g_strdup("<span size='x-large'><b>Hòa</b></span>");
    char *why_esc = g_markup_escape_text(why, -1);
    char *markup = g_strdup_printf("%s %s\n%s", head,
                                   winner != NULL ? symbol_markup(x_won ? 'X' : 'O') : "", why_esc);
    gtk_label_set_markup(GTK_LABEL(l_result), markup);
    g_free(markup);
    g_free(why_esc);
    g_free(head);
    g_free(why);
}

/* The moves f[first..n-1] ("r,c", X first) -> rp.moves. Stops at the first one that is not a
 * free cell of the board (the server is the reference, but a bad replay must not crash us).
 * Returns FALSE if one was bad. */
static gboolean read_moves(char **f, int first, int n)
{
    static char seen[BOARD_SIZE_MAX][BOARD_SIZE_MAX];
    memset(seen, 0, sizeof seen);
    rp.n = 0;
    for (int i = first; i < n; i++) {
        int r, c;
        if (rp.n == MAX_MOVES || !field_rc(f[i], &r, &c) || r < 0 || c < 0 || r >= rp.size
            || c >= rp.size || seen[r][c])
            return FALSE;
        seen[r][c] = 1;
        rp.moves[rp.n][0] = r;
        rp.moves[rp.n][1] = c;
        rp.n++;
    }
    return TRUE;
}

static gboolean is_result(const char *s)
{
    return strcmp(s, RES_X_WIN) == 0 || strcmp(s, RES_O_WIN) == 0 || strcmp(s, RES_DRAW) == 0;
}

/* 200|REPLAY|mid|size|x_user|o_user|result|reason|n|r,c|r,c|... */
static void on_replay_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED || GPOINTER_TO_UINT(data) != visit)
        return;
    loading = FALSE;
    update_list_controls();
    if (code != ST_OK) {
        error_note("Không tải được ván", code, f, n);    /* 404 match_not_found, ... */
        return;
    }
    int mid, size, count;
    if (n < 9 || !field_int(f[2], &mid) || !field_int(f[3], &size) || size < 1
        || size > BOARD_SIZE_MAX || !is_result(f[6]) || !field_int(f[8], &count)) {
        note(l_list_note, NOTE_ERROR, "Server trả về phản hồi REPLAY không hợp lệ");
        return;
    }
    if (count != n - 9)
        g_printerr("caro_client: warning: REPLAY says %d moves but has %d\n", count, n - 9);

    auto_stop();
    rp.mid = mid;
    rp.size = size;
    g_strlcpy(rp.x_user, f[4], sizeof rp.x_user);
    g_strlcpy(rp.o_user, f[5], sizeof rp.o_user);
    g_strlcpy(rp.result, f[6], sizeof rp.result);
    g_strlcpy(rp.reason, f[7], sizeof rp.reason);
    gboolean all_ok = read_moves(f, 9, n);
    rp.pos = 0;                      /* from the empty board: the viewer steps forward */

    char *title = g_strdup_printf("<span size='x-large'><b>Xem lại ván #%d</b></span>\n"
                                  "Bàn %d×%d%s%s", mid, size, size, req_time[0] ? " · " : "",
                                  req_time);
    gtk_label_set_markup(GTK_LABEL(l_title), title);
    g_free(title);
    set_replay_player(l_x, 'X', rp.x_user);
    set_replay_player(l_o, 'O', rp.o_user);
    set_replay_result();
    if (all_ok) {
        gtk_label_set_text(GTK_LABEL(l_replay_note), "");
    } else {
        g_printerr("caro_client: warning: REPLAY %d: bad move after %d moves\n", mid, rp.n);
        note(l_replay_note, NOTE_ERROR, "Dữ liệu ván có nước đi không hợp lệ: chỉ hiện %d nước đầu",
             rp.n);
    }
    replay_draw();
    note(l_list_note, NOTE_INFO, "Bấm vào một ván để xem lại.");
    gtk_stack_set_visible_child_name(GTK_STACK(views), "replay");
}

/* A click on a row (single click: activate-on-single-click) -> REPLAY|mid. */
static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tv;
    (void)col;
    (void)data;
    GtkTreeIter it;
    if (loading || !gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, path))
        return;
    int mid;
    gchar *when;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &it, H_MID, &mid, H_TIME, &when, -1);
    char *payload = g_strdup_printf("%s|%d", CMD_REPLAY, mid);
    if (net_request(payload, on_replay_reply, GUINT_TO_POINTER(visit))) {
        loading = TRUE;
        g_strlcpy(req_time, when, sizeof req_time);
        note(l_list_note, NOTE_INFO, "Đang tải ván #%d…", mid);
    }
    g_free(payload);
    g_free(when);
    update_list_controls();
}

/* ---- page ------------------------------------------------------------------------------------------ */

void ui_history_enter(void)
{
    visit++;
    loading = FALSE;
    auto_stop();
    gtk_stack_set_visible_child_name(GTK_STACK(views), "list");
    gtk_list_store_clear(store);
    cur_page = 1;
    rows_on_page = 0;
    request_page(1);
}

void ui_history_leave(void)
{
    visit++;                         /* a response still on its way is now from an old visit */
    loading = FALSE;
    auto_stop();
}

void ui_history_reset(void)
{
    ui_history_leave();
    gtk_list_store_clear(store);
    gtk_label_set_text(GTK_LABEL(l_list_note), "");
    gtk_stack_set_visible_child_name(GTK_STACK(views), "list");
}

static void add_column(const char *title, int col, int color_col, float xalign, gboolean expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    if (color_col >= 0)
        gtk_tree_view_column_add_attribute(c, r, "foreground", color_col);
    gtk_tree_view_column_set_alignment(c, xalign);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static GtkWidget *build_list_view(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(box, "margin", 20, NULL);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='x-large'><b>Lịch sử đấu</b></span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *b_lobby = gtk_button_new_with_label("Về sảnh");
    g_signal_connect(b_lobby, "clicked", G_CALLBACK(on_lobby_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), b_lobby, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* newest first, in the server's order: no sortable headers */
    store = gtk_list_store_new(H_N_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);                                  /* the view holds the reference */
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree), TRUE);
    add_column("Đối thủ", H_OPP, -1, 0.0f, TRUE);
    add_column("Quân", H_SYM, H_SYM_COLOR, 0.5f, FALSE);
    add_column("Kết quả", H_RESULT, H_RESULT_COLOR, 0.0f, FALSE);
    add_column("Lý do", H_REASON, -1, 0.0f, FALSE);
    add_column("Thời gian", H_TIME, -1, 0.0f, FALSE);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    l_list_note = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(l_list_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l_list_note), 0.0f);
    b_prev_page = gtk_button_new_with_label("◀ Trang trước");
    g_signal_connect(b_prev_page, "clicked", G_CALLBACK(on_prev_page), NULL);
    l_page = gtk_label_new(NULL);
    gtk_label_set_width_chars(GTK_LABEL(l_page), 8);
    b_next_page = gtk_button_new_with_label("Trang sau ▶");
    g_signal_connect(b_next_page, "clicked", G_CALLBACK(on_next_page), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), l_list_note, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), b_next_page, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), l_page, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), b_prev_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    update_list_controls();
    return box;
}

static GtkWidget *side_label(GtkWidget *box, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
    if (wrap) {
        /* WORD_CHAR: a 20-character name has no space, it may break inside instead of
         * widening the panel (and shrinking the board). */
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(l), SIDE_WRAP_CHARS);
    }
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static GtkWidget *grid_button(GtkWidget *grid, const char *label, const char *tip, int col, int row,
                              int width, GCallback on_click, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (tip != NULL)
        gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", on_click, data);
    gtk_grid_attach(GTK_GRID(grid), b, col, row, width, 1);
    return b;
}

static GtkWidget *build_replay_view(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_object_set(page, "margin", 16, NULL);

    rbv = board_view_new(NULL, NULL);          /* no click callback, never interactive */
    gtk_box_pack_start(GTK_BOX(page), board_view_widget(rbv), TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(side, SIDE_WIDTH, -1);
    l_title = side_label(side, FALSE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_x = side_label(side, TRUE);            /* "X  <name up to 20 chars>  (bạn)" */
    l_o = side_label(side, TRUE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_result = side_label(side, TRUE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_pos = side_label(side, FALSE);
    l_last = side_label(side, TRUE);
    l_replay_note = side_label(side, TRUE);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    b_first = grid_button(grid, "⏮", "Về bàn cờ trống", 0, 0, 1, G_CALLBACK(on_step_clicked),
                          GINT_TO_POINTER(G_MININT));
    b_back = grid_button(grid, "◀", "Lùi một nước", 1, 0, 1, G_CALLBACK(on_step_clicked),
                         GINT_TO_POINTER(-1));
    b_fwd = grid_button(grid, "▶", "Tiến một nước", 2, 0, 1, G_CALLBACK(on_step_clicked),
                        GINT_TO_POINTER(1));
    b_end = grid_button(grid, "⏭", "Đến nước cuối", 3, 0, 1, G_CALLBACK(on_step_clicked),
                        GINT_TO_POINTER(G_MAXINT));
    b_auto = grid_button(grid, "Tự chạy", "Tự đi một nước mỗi 0,7 giây", 0, 1, 4,
                         G_CALLBACK(on_auto_clicked), NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(b_auto), "suggested-action");
    grid_button(grid, "Về danh sách", NULL, 0, 2, 4, G_CALLBACK(on_back_to_list), NULL);
    gtk_box_pack_end(GTK_BOX(side), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), side, FALSE, FALSE, 0);
    update_replay_controls();
    return page;
}

GtkWidget *ui_history_new(void)
{
    views = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(views), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_add_named(GTK_STACK(views), build_list_view(), "list");
    gtk_stack_add_named(GTK_STACK(views), build_replay_view(), "replay");
    return views;
}
