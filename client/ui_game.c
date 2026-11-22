/*
 * client/ui_game.c — page "game": board, turn, controls, result (docs/protocol.md §6.3, §7, §8).
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
 * CONTROLS — same rule: a button only sends a request, the result comes from the server.
 *   Xin thua  confirm -> RESIGN|gid -> 200, then GAME_END (RESIGN) to both
 *   Cầu hòa   DRAW_OFFER|gid -> 200; the opponent answers: GAME_END (DRAW_AGREED) or
 *             DRAW_DECLINED. The next move, by either player, cancels the offer (§6.3 step 7).
 *   Đấu lại   only after GAME_END: REMATCH|gid -> 200; then GAME_START (new gid, X and O
 *             swapped) or REMATCH_DECLINED. After OPP_LEFT it can never be accepted.
 *   Về sảnh   LEAVE_GAME|gid; if the game is still running, confirm first: it counts as a loss.
 *   The opponent's DRAW_OFFERED / REMATCH_REQUESTED open an Accept/Decline dialog (DRAW_REPLY,
 *   REMATCH_REPLY). Every button is locked while its request waits for the response.
 *
 * WHY OUR DRAW OFFER HAS TWO STATES (SENDING, PENDING)
 *   A move cancels the offer at the server. Responses to our requests and events caused by the
 *   opponent reach us in the order the server handled them (one TCP stream, §3), so:
 *     opponent moved BEFORE the server got our offer: OPP_MOVE arrives while SENDING -> the
 *       offer is still alive (its 200 comes next);
 *     opponent moved AFTER: 200 came first (PENDING), then OPP_MOVE -> the offer is gone.
 *   One flag could not tell these apart.
 *
 * LEAVING A RUNNING GAME
 *   The server answers 200|LEAVE_GAME first and only then sends GAME_END of that game (§3:
 *   the response before the events). We switch to the lobby on the 200, so the GAME_END finds
 *   us there: app.game.left marks that game and its GAME_END only updates the ELO, it never
 *   opens the game page again. The lobby then sends LIST_PLAYERS and STATS; the ELO and
 *   W/L/D shown there come from the server, the client never adds or subtracts itself.
 *   Every event whose gid is not the game on screen (OPP_MOVE, DRAW_OFFERED, ...) is ignored.
 *
 * MOVE COUNTDOWN — display only (protocol.md §8: the server decides TIMEOUT)
 *   Starts from GAME_START's sec_per_move (a rematch's GAME_START starts it again), restarts
 *   after 200|MOVE and after OPP_MOVE — the moments the server resets its own move timer — and
 *   stops at GAME_END or when we leave. A g_timeout_add_seconds(1) source redraws it, but the
 *   number shown is computed from a deadline on the monotonic clock, not by counting ticks:
 *   GLib may fire a seconds-timeout a little early or late (it groups them to save wake-ups),
 *   and counting ticks would drift. Rounding the remaining time UP means 0 is never shown before
 *   the deadline. At 0 the clock stays red at 0 and the source is removed; the GAME_END
 *   (TIMEOUT) comes from the server. The other player's screen counts the same way.
 */
#include <stdarg.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "board_view.h"
#include "dispatch.h"
#include "net.h"

/* Side panel width. A wrapping GtkLabel asks for its whole text on one line as natural width,
 * which would widen the panel and shrink the board: wrapping labels get a max width in chars. */
#define SIDE_WIDTH       250
#define SIDE_WRAP_CHARS  30
#define CLOCK_WARN_SEC   5      /* the countdown turns red at 5 seconds or less */

static BoardView *bv;
static GtkWidget *l_title, *l_me, *l_opp, *l_turn, *l_clock, *l_moves, *l_notice;
static GtkWidget *b_resign, *b_draw, *b_rematch, *b_leave;

/* At most one of each. Every dialog uses the "response" signal, never gtk_dialog_run(). */
static GtkWidget *result_dialog;     /* GAME_END: who won, why, new ELO */
static GtkWidget *confirm_dialog;    /* "xin thua?" or "rời ván đang chơi?" */
static GtkWidget *draw_dialog;       /* DRAW_OFFERED: Accept / Decline */
static GtkWidget *rematch_dialog;    /* REMATCH_REQUESTED: Accept / Decline */

static struct {
    gboolean known;            /* GAME_END received for the current game */
    int      elo_before, new_elo, delta;
    char     outcome;          /* 'W' win, 'L' loss, 'D' draw */
    char     reason[24];       /* END_FIVE, END_RESIGN, ... as received */
} end;

static struct {
    gboolean running;          /* counting for the current move */
    gint64   deadline_us;      /* g_get_monotonic_time() at which the mover runs out of time */
    guint    source_id;        /* g_timeout_add_seconds() source, 0 = none */
} move_clock;

typedef enum { OFFER_NONE, OFFER_SENDING, OFFER_PENDING } OfferState;

static struct {
    gboolean   resign_sent;    /* RESIGN sent: wait for GAME_END */
    OfferState draw;           /* our draw offer */
    OfferState rematch;        /* our rematch request */
    gboolean   accepting;      /* DRAW_REPLY / REMATCH_REPLY ACCEPT sent: wait for GAME_END /
                                  GAME_START */
    gboolean   leaving;        /* LEAVE_GAME sent: wait for its response */
    gboolean   opp_left;       /* OPP_LEFT, or the opponent disconnected: no rematch possible */
} ctl;

enum { CONFIRM_RESIGN = 1, CONFIRM_LEAVE };

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

/* A response to a request sent for game `data` (GINT_TO_POINTER(gid)) that still concerns the
 * game on screen. FALSE when the connection is gone (net.c already went back to login) or when
 * another game started meanwhile. */
static gboolean reply_is_current(int code, gpointer data)
{
    return code != RESP_DISCONNECTED && app.game.active && app.game.gid == GPOINTER_TO_INT(data);
}

/* "EVENT|gid": TRUE if it is about the game on screen; events of any other gid are ignored. */
static gboolean event_is_current(char **f, int n)
{
    int gid;
    if (n < 2 || !field_int(f[1], &gid)) {
        g_printerr("caro_client: warning: bad %s ignored\n", f[0]);
        return FALSE;
    }
    return app.game.active && gid == app.game.gid;
}

/* Sends "CMD|gid" for the game on screen; the callback gets the gid back as user_data. */
static gboolean send_game_cmd(const char *cmd, ResponseCb cb)
{
    char *payload = g_strdup_printf("%s|%d", cmd, app.game.gid);
    gboolean ok = net_request(payload, cb, GINT_TO_POINTER(app.game.gid));
    g_free(payload);
    return ok;
}

/* Error response of RESIGN / DRAW_* / REMATCH_* / LEAVE_GAME -> "Không <what> được: <why>". */
static void error_notice(const char *what, int code, char **f, int n)
{
    char *why = error_text(code, f, n);
    notice(NOTE_ERROR, "Không %s được: %s", what, why);
    g_free(why);
}

/* ---- dialogs -------------------------------------------------------------------------------------- */

static void close_dialog(GtkWidget **slot)
{
    GtkWidget *d = *slot;
    if (d != NULL) {
        *slot = NULL;                    /* first: nothing may use it while it is destroyed */
        gtk_widget_destroy(d);
    }
}

/* In a "response" handler: forget the dialog, then destroy it. */
static void dialog_done(GtkWidget **slot, GtkDialog *dialog)
{
    if (*slot == GTK_WIDGET(dialog))
        *slot = NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void close_all_dialogs(void)
{
    close_dialog(&result_dialog);
    close_dialog(&confirm_dialog);
    close_dialog(&draw_dialog);
    close_dialog(&rematch_dialog);
}

/* Question dialog with a "no" (REJECT) and a "yes" (ACCEPT) button; the gid travels with it so
 * that a late answer can be recognised as belonging to an older game. */
static GtkWidget *question_dialog(const char *title, const char *detail, const char *no_label,
                                  const char *yes_label, gboolean yes_is_default,
                                  GCallback on_response, gpointer data)
{
    GtkWidget *d = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);
    gtk_dialog_add_button(GTK_DIALOG(d), no_label, GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(d), yes_label, GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(d),
                                    yes_is_default ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_REJECT);
    g_object_set_data(G_OBJECT(d), "gid", GINT_TO_POINTER(app.game.gid));
    g_signal_connect(d, "response", on_response, data);
    gtk_widget_show_all(d);
    return d;
}

static int dialog_gid(GtkDialog *dialog)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "gid"));
}

/* ---- move countdown ---------------------------------------------------------------------------------- */

/* Whole seconds left for the current move, rounded up; 0 once the deadline has passed. */
static int clock_left(void)
{
    gint64 left_us = move_clock.deadline_us - g_get_monotonic_time();
    return left_us <= 0 ? 0 : (int)((left_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);
}

static void clock_show(void)
{
    if (!move_clock.running) {
        gtk_label_set_text(GTK_LABEL(l_clock), "");
        return;
    }
    int left = clock_left();
    gboolean mine = app.game.turn == app.game.my_symbol;
    char *markup = g_strdup_printf("<span foreground='%s'>%s còn <span size='x-large'><b>%d</b>"
                                   "</span> giây</span>",
                                   left <= CLOCK_WARN_SEC ? "#c01c28" : "#3d3846",
                                   mine ? "Bạn" : "Đối thủ", left);
    gtk_label_set_markup(GTK_LABEL(l_clock), markup);
    g_free(markup);
}

static gboolean on_clock_tick(gpointer data)
{
    (void)data;
    clock_show();
    if (clock_left() > 0)
        return G_SOURCE_CONTINUE;
    move_clock.source_id = 0;                     /* stays at 0 until the server's GAME_END */
    return G_SOURCE_REMOVE;
}

static void clock_remove_source(void)
{
    if (move_clock.source_id != 0) {
        g_source_remove(move_clock.source_id);
        move_clock.source_id = 0;
    }
}

static void clock_stop(void)
{
    clock_remove_source();
    move_clock.running = FALSE;
    clock_show();
}

/* A new move begins: full sec_per_move again for the player to move. */
static void clock_restart(void)
{
    clock_remove_source();
    int sec = app.game.sec_per_move;
    move_clock.running = app.game.active && !app.game.ended && sec > 0;
    if (move_clock.running) {
        move_clock.deadline_us = g_get_monotonic_time() + (gint64)sec * G_USEC_PER_SEC;
        move_clock.source_id = g_timeout_add_seconds(1, on_clock_tick, NULL);
    }
    clock_show();
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
    /* insert_hyphens: a name broken across two lines must not show a '-' it does not have */
    char *markup = g_strdup_printf("<span size='large' insert_hyphens='false'>%s %s  <b>%s</b>"
                                   "</span>\n<small>%s</small>",
                                   to_move ? "▶" : "   ", symbol_markup(sym), esc, elo);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(esc);
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

static void update_buttons(void)
{
    const GameInfo *g = &app.game;
    gboolean on = g->active && !ctl.leaving;
    gboolean running = on && !g->ended && !ctl.accepting;
    gtk_widget_set_sensitive(b_resign, running && !ctl.resign_sent);
    gtk_widget_set_sensitive(b_draw, running && !ctl.resign_sent && ctl.draw == OFFER_NONE
                                     && draw_dialog == NULL);
    gtk_widget_set_sensitive(b_rematch, on && g->ended && !ctl.accepting && !ctl.opp_left
                                        && ctl.rematch == OFFER_NONE && rematch_dialog == NULL);
    /* Not while an ACCEPT waits: its GAME_END / GAME_START is a few milliseconds away, and a
     * LEAVE_GAME for the old gid could reach the server after the new game started. */
    gtk_widget_set_sensitive(b_leave, on && !ctl.accepting);
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

    char *t;
    if (g->ended) {                          /* result + why, kept after the dialog is closed */
        const char *res = end.outcome == 'W' ? "<span foreground='#26a269'><b>Bạn thắng</b></span>"
                        : end.outcome == 'L' ? "<span foreground='#c01c28'><b>Bạn thua</b></span>"
                                             : "<b>Hòa</b>";
        char *why = g_markup_escape_text(end_reason_text(end.reason, end.outcome), -1);
        t = g_strdup_printf("<span size='x-large'>%s</span>\n%s", res, why);
        g_free(why);
    } else {
        const char *turn;
        if (ctl.leaving)
            turn = "<span foreground='#5e5c64'>Đang rời ván…</span>";
        else if (g->move_pending)
            turn = "<span foreground='#5e5c64'>Đang gửi nước đi…</span>";
        else if (my_turn)
            turn = "<span foreground='#26a269'><b>Lượt của bạn</b></span>";
        else
            turn = "<span foreground='#5e5c64'>Đợi đối thủ…</span>";
        t = g_strdup_printf("<span size='x-large'>%s</span>", turn);
    }
    gtk_label_set_markup(GTK_LABEL(l_turn), t);
    g_free(t);

    clock_show();                            /* "Bạn còn" / "Đối thủ còn" follows the turn */

    char moves[48];
    snprintf(moves, sizeof moves, "Số nước đã đi: %d", g->move_no);
    gtk_label_set_text(GTK_LABEL(l_moves), moves);

    board_view_set_interactive(bv, g->active && my_turn && !g->move_pending && !ctl.leaving
                                   && !ctl.resign_sent && !ctl.accepting);
    update_buttons();
}

/* §6.3 step 7: a move, ours or the opponent's, cancels any pending draw offer. */
static void move_cancels_draw_offers(void)
{
    if (ctl.draw == OFFER_PENDING) {
        ctl.draw = OFFER_NONE;
        notice(NOTE_INFO, "Lời cầu hòa của bạn hết hiệu lực vì đã có nước đi mới");
    }
    if (draw_dialog != NULL) {
        close_dialog(&draw_dialog);
        notice(NOTE_INFO, "Lời cầu hòa của đối thủ hết hiệu lực vì đã có nước đi mới");
    }
}

/* ---- my move ------------------------------------------------------------------------------------------ */

/* 200|MOVE|move_no — only now is our mark drawn. user_data = the gid the move was sent for. */
static void on_move_reply(int code, char **f, int n, gpointer data)
{
    GameInfo *g = &app.game;
    if (!reply_is_current(code, data))
        return;                              /* connection gone, or an older game */
    g->move_pending = FALSE;

    if (code == ST_OK) {
        int move_no;
        board_view_set(bv, g->pending_row, g->pending_col, g->my_symbol);
        board_view_set_last(bv, g->pending_row, g->pending_col);
        g->move_no = n >= 3 && field_int(f[2], &move_no) ? move_no : g->move_no + 1;
        g->turn = opp_symbol();
        move_cancels_draw_offers();
        clock_restart();                     /* the server reset its move timer: now theirs */
    } else if (!g->ended) {                  /* after GAME_END a refused move needs no message */
        char *why = error_text(code, f, n);  /* 430 not_your_turn ... 433 game_not_active */
        notice(NOTE_ERROR, "Nước đi bị từ chối: %s", why);
        g_free(why);
    }
    update_panel();
}

static void on_cell(int row, int col, gpointer data)
{
    (void)data;
    GameInfo *g = &app.game;
    if (!g->active || g->ended || g->move_pending || g->turn != g->my_symbol || ctl.leaving
        || ctl.resign_sent || ctl.accepting || board_view_get(bv, row, col) != '.')
        return;

    char *payload = g_strdup_printf("%s|%d|%d|%d", CMD_MOVE, g->gid, row, col);
    if (net_request(payload, on_move_reply, GINT_TO_POINTER(g->gid))) {
        g->move_pending = TRUE;              /* locked until the server answers */
        g->pending_row = row;
        g->pending_col = col;
        notice_clear();
        update_panel();
    }
    g_free(payload);
}

/* ---- Xin thua ------------------------------------------------------------------------------------------ */

static void on_resign_reply(int code, char **f, int n, gpointer data)
{
    if (!reply_is_current(code, data) || code == ST_OK)
        return;                              /* 200: GAME_END (RESIGN) follows */
    ctl.resign_sent = FALSE;
    if (!app.game.ended)
        error_notice("xin thua", code, f, n);
    update_panel();
}

static void send_resign(void)
{
    if (send_game_cmd(CMD_RESIGN, on_resign_reply)) {
        ctl.resign_sent = TRUE;
        notice(NOTE_INFO, "Đang xin thua…");
    }
    update_panel();
}

/* ---- Về sảnh -------------------------------------------------------------------------------------------- */

/* 200|LEAVE_GAME: the server has put us back in the lobby (state L). */
static void back_to_lobby(void)
{
    GameInfo *g = &app.game;
    close_all_dialogs();
    clock_stop();
    g->left = !g->ended;                     /* its GAME_END is still on the way (§3) */
    g->active = FALSE;
    memset(&ctl, 0, sizeof ctl);
    app.state = SESSION_LOBBY;
    app_show_page("lobby");                  /* sends LIST_PLAYERS again */
    ui_lobby_request_stats();                /* then STATS: ELO and W/L/D from the server */
}

static void on_leave_reply(int code, char **f, int n, gpointer data)
{
    if (code == RESP_DISCONNECTED)
        return;
    gboolean same = app.game.active && app.game.gid == GPOINTER_TO_INT(data);
    /* 200: we are in the lobby, whatever happened meanwhile. 403 for this very game: the server
     * no longer sees us in it, so the lobby is where we are too. */
    if (code == ST_OK || (code == ST_FORBIDDEN && same)) {
        back_to_lobby();
        return;
    }
    if (!same)
        return;
    ctl.leaving = FALSE;
    error_notice("rời ván", code, f, n);
    update_panel();
}

static void send_leave(void)
{
    if (send_game_cmd(CMD_LEAVE_GAME, on_leave_reply)) {
        ctl.leaving = TRUE;
        close_all_dialogs();                 /* nothing on this page matters any more */
        notice_clear();
    }
    update_panel();
}

/* ---- confirmations ---------------------------------------------------------------------------------------- */

static void on_confirm_response(GtkDialog *dialog, gint response, gpointer data)
{
    int action = GPOINTER_TO_INT(data);
    int gid = dialog_gid(dialog);
    dialog_done(&confirm_dialog, dialog);
    const GameInfo *g = &app.game;
    /* The game may have ended while the question was open: the dialog was then closed, so
     * reaching here with "yes" means the game is still the running one we asked about. */
    if (response != GTK_RESPONSE_ACCEPT || !g->active || g->gid != gid || g->ended || ctl.leaving)
        return;
    if (action == CONFIRM_RESIGN && !ctl.resign_sent)
        send_resign();
    else if (action == CONFIRM_LEAVE)
        send_leave();
}

static void on_resign_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (!app.game.active || app.game.ended || ctl.resign_sent || ctl.leaving)
        return;
    close_dialog(&confirm_dialog);
    confirm_dialog = question_dialog("Bạn chắc chắn muốn xin thua?",
                                     "Ván kết thúc ngay, bạn bị tính thua và bị trừ ELO.",
                                     "Chơi tiếp", "Xin thua", FALSE,
                                     G_CALLBACK(on_confirm_response),
                                     GINT_TO_POINTER(CONFIRM_RESIGN));
}

static void on_leave_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (!app.game.active || ctl.leaving || ctl.accepting)
        return;
    if (app.game.ended) {                    /* nothing to lose: leave at once */
        send_leave();
        return;
    }
    close_dialog(&confirm_dialog);
    confirm_dialog = question_dialog("Ván đang diễn ra. Về sảnh?",
                                     "Rời ván bây giờ được tính là bạn xin thua: bạn bị tính "
                                     "thua và bị trừ ELO.",
                                     "Ở lại", "Rời ván", FALSE,
                                     G_CALLBACK(on_confirm_response),
                                     GINT_TO_POINTER(CONFIRM_LEAVE));
}

/* ---- Cầu hòa ----------------------------------------------------------------------------------------------- */

static void on_draw_offer_reply(int code, char **f, int n, gpointer data)
{
    if (!reply_is_current(code, data) || ctl.draw != OFFER_SENDING)
        return;                              /* GAME_END already settled it */
    if (code == ST_OK) {
        ctl.draw = OFFER_PENDING;
        notice(NOTE_INFO, "Đã gửi lời cầu hòa, đang chờ đối thủ trả lời…");
    } else {
        ctl.draw = OFFER_NONE;
        if (!app.game.ended)
            error_notice("cầu hòa", code, f, n);
    }
    update_panel();
}

static void on_draw_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (!app.game.active || app.game.ended || ctl.draw != OFFER_NONE || ctl.leaving)
        return;
    if (send_game_cmd(CMD_DRAW_OFFER, on_draw_offer_reply)) {
        ctl.draw = OFFER_SENDING;
        notice(NOTE_INFO, "Đang gửi lời cầu hòa…");
    }
    update_panel();
}

/* DRAW_REPLY / REMATCH_REPLY: 200 ACCEPT -> GAME_END / GAME_START follows; 200 DECLINE -> done. */
static void on_answer_reply(int code, char **f, int n, gpointer data)
{
    if (!reply_is_current(code, data) || code == ST_OK)
        return;
    gboolean was_accepting = ctl.accepting;
    ctl.accepting = FALSE;
    if (n >= 3 && strcmp(f[2], ERR_OPPONENT_LEFT) == 0)
        ctl.opp_left = TRUE;
    if (was_accepting)                       /* a refused DECLINE changes nothing for the user */
        error_notice("trả lời", code, f, n);
    update_panel();
}

static void send_answer(const char *cmd, int gid, gboolean accept)
{
    char *payload = g_strdup_printf("%s|%d|%s", cmd, gid, accept ? ARG_ACCEPT : ARG_DECLINE);
    if (net_request(payload, on_answer_reply, GINT_TO_POINTER(gid)) && accept)
        ctl.accepting = TRUE;
    g_free(payload);
}

static void on_draw_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)data;
    int gid = dialog_gid(dialog);
    dialog_done(&draw_dialog, dialog);
    if (!app.game.active || app.game.gid != gid || app.game.ended || ctl.leaving)
        return;
    gboolean accept = response == GTK_RESPONSE_ACCEPT;   /* the close button = decline */
    send_answer(CMD_DRAW_REPLY, gid, accept);
    notice(NOTE_INFO, accept ? "Bạn đã đồng ý hòa…" : "Bạn đã từ chối lời cầu hòa");
    update_panel();
}

/* DRAW_OFFERED|gid — the opponent offers a draw */
static void on_draw_offered(char **f, int n)
{
    if (!event_is_current(f, n) || app.game.ended || ctl.leaving || draw_dialog != NULL)
        return;
    char *detail = g_strdup_printf("%s đề nghị hòa ván này. Nếu bạn đi tiếp một nước mà chưa "
                                   "trả lời, lời đề nghị sẽ hết hiệu lực.", app.game.opponent);
    draw_dialog = question_dialog("Đối thủ cầu hòa", detail, "Từ chối", "Đồng ý hòa", TRUE,
                                  G_CALLBACK(on_draw_dialog_response), NULL);
    g_free(detail);
    update_panel();
}

/* DRAW_DECLINED|gid — to the offerer */
static void on_draw_declined(char **f, int n)
{
    if (!event_is_current(f, n) || ctl.draw == OFFER_NONE)
        return;
    ctl.draw = OFFER_NONE;
    notice(NOTE_INFO, "Đối thủ đã từ chối lời cầu hòa");
    update_panel();
}

/* ---- Đấu lại ------------------------------------------------------------------------------------------------ */

static void on_rematch_reply(int code, char **f, int n, gpointer data)
{
    if (!reply_is_current(code, data) || ctl.rematch != OFFER_SENDING)
        return;
    if (code == ST_OK) {
        ctl.rematch = OFFER_PENDING;
        notice(NOTE_INFO, "Đã mời đấu lại, đang chờ đối thủ trả lời…");
    } else {
        ctl.rematch = OFFER_NONE;
        if (n >= 3 && strcmp(f[2], ERR_OPPONENT_LEFT) == 0)
            ctl.opp_left = TRUE;
        error_notice("đấu lại", code, f, n);
    }
    update_panel();
}

static void on_rematch_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (!app.game.active || !app.game.ended || ctl.opp_left || ctl.rematch != OFFER_NONE
        || ctl.leaving)
        return;
    if (send_game_cmd(CMD_REMATCH, on_rematch_reply)) {
        ctl.rematch = OFFER_SENDING;
        notice(NOTE_INFO, "Đang mời đấu lại…");
    }
    update_panel();
}

static void on_rematch_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)data;
    int gid = dialog_gid(dialog);
    dialog_done(&rematch_dialog, dialog);
    if (!app.game.active || app.game.gid != gid || ctl.opp_left || ctl.leaving)
        return;
    gboolean accept = response == GTK_RESPONSE_ACCEPT;
    send_answer(CMD_REMATCH_REPLY, gid, accept);
    notice(NOTE_INFO, accept ? "Đang vào ván mới…" : "Bạn đã từ chối đấu lại");
    update_panel();
}

/* REMATCH_REQUESTED|gid — the opponent wants another game */
static void on_rematch_requested(char **f, int n)
{
    if (!event_is_current(f, n) || !app.game.ended || ctl.leaving || ctl.opp_left
        || rematch_dialog != NULL)
        return;
    close_dialog(&result_dialog);            /* the panel still shows the result and why */
    char *detail = g_strdup_printf("%s mời bạn đấu thêm một ván (hai bên đổi X và O).",
                                   app.game.opponent);
    rematch_dialog = question_dialog("Đối thủ muốn đấu lại", detail, "Từ chối", "Đấu lại", TRUE,
                                     G_CALLBACK(on_rematch_dialog_response), NULL);
    g_free(detail);
    update_panel();
}

/* REMATCH_DECLINED|gid — to the requester */
static void on_rematch_declined(char **f, int n)
{
    if (!event_is_current(f, n) || ctl.rematch == OFFER_NONE)
        return;
    ctl.rematch = OFFER_NONE;
    notice(NOTE_INFO, "Đối thủ đã từ chối đấu lại");
    update_panel();
}

/* OPP_LEFT|gid — the opponent left the (finished) game or disconnected after it ended */
static void on_opp_left(char **f, int n)
{
    if (!event_is_current(f, n))
        return;
    ctl.opp_left = TRUE;
    ctl.rematch = OFFER_NONE;
    close_dialog(&rematch_dialog);
    notice(NOTE_INFO, "Đối thủ đã rời ván");
    update_panel();
}

/* ---- the opponent's move, the end ---------------------------------------------------------------------------- */

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
    move_cancels_draw_offers();
    clock_restart();                         /* our move now, full time again */
    update_panel();
}

static void on_result_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)response;
    (void)data;
    dialog_done(&result_dialog, dialog);
}

static void show_result_dialog(void)
{
    close_dialog(&result_dialog);
    char d[16];
    fmt_delta(d, sizeof d, end.delta);
    GtkWidget *dlg = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
        end.outcome == 'W' ? "Bạn thắng!" : end.outcome == 'L' ? "Bạn thua" : "Hòa");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s.\nELO mới: %d (%s)",
                                             end_reason_text(end.reason, end.outcome),
                                             end.new_elo, d);
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
    if (!g->active) {
        /* We left this running game: 200|LEAVE_GAME came first and we are in the lobby. Only
         * the new ELO matters; the game page is not shown again. */
        if (g->left && gid == g->gid && !g->ended) {
            g->ended = TRUE;
            g->left = FALSE;
            app.elo = new_elo;
            ui_lobby_update_bar();
        }
        return;
    }
    if (gid != g->gid || g->ended)
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
    clock_stop();
    if (strcmp(reason, END_FIVE) == 0) {     /* the five winning cells follow */
        int cells[WIN_LENGTH][2], k = 0;
        for (int i = 6; i < n && k < WIN_LENGTH; i++)
            if (field_rc(f[i], &cells[k][0], &cells[k][1]))
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
    g_strlcpy(end.reason, reason, sizeof end.reason);
    /* The server's number. W/L/D are not counted here: the lobby asks STATS after LEAVE_GAME. */
    app.elo = new_elo;

    /* Everything about the running game is over. */
    close_dialog(&confirm_dialog);
    close_dialog(&draw_dialog);
    ctl.resign_sent = FALSE;
    ctl.accepting = FALSE;
    ctl.draw = OFFER_NONE;
    if (strcmp(reason, END_DISCONNECT) == 0)  /* the opponent's connection is gone */
        ctl.opp_left = TRUE;

    notice_clear();
    update_panel();
    if (!ctl.leaving)                        /* leaving anyway: no dialog that closes at once */
        show_result_dialog();
}

/* ---- page ------------------------------------------------------------------------------------------------ */

/* A new game (GAME_START): from the lobby, or here for a rematch (new gid, symbols swapped). */
void ui_game_enter(void)
{
    GameInfo *g = &app.game;
    const char *shown = gtk_stack_get_visible_child_name(app.stack);
    gboolean rematch = shown != NULL && strcmp(shown, "game") == 0;

    close_all_dialogs();
    memset(&ctl, 0, sizeof ctl);
    memset(&end, 0, sizeof end);
    board_view_reset(bv, g->size);
    g->turn = 'X';                           /* X always moves first */
    g->move_no = 0;
    g->move_pending = FALSE;
    g->ended = FALSE;
    clock_restart();                         /* from sec_per_move, also after a rematch */
    if (rematch)
        notice(NOTE_INFO, "Đấu lại: ván mới #%d, bạn cầm %c%s", g->gid, g->my_symbol,
               g->my_symbol == 'X' ? " và đi trước" : "");
    else
        notice_clear();
    update_panel();
}

void ui_game_reset(void)
{
    close_all_dialogs();
    clock_stop();
    memset(&ctl, 0, sizeof ctl);
}

static GtkWidget *side_label(GtkWidget *box, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
    if (wrap) {
        /* WORD_CHAR: a 20-character name has no space to break at, so it may break inside the
         * name instead of widening the panel (and shrinking the board). */
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(l), SIDE_WRAP_CHARS);
    }
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static GtkWidget *control_button(GtkWidget *grid, const char *label, int col, int row,
                                 GCallback on_click, const char *style_class)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (style_class != NULL)
        gtk_style_context_add_class(gtk_widget_get_style_context(b), style_class);
    g_signal_connect(b, "clicked", on_click, NULL);
    gtk_grid_attach(GTK_GRID(grid), b, col, row, 1, 1);
    return b;
}

GtkWidget *ui_game_new(void)
{
    dispatch_on(EV_OPP_MOVE, on_opp_move);
    dispatch_on(EV_GAME_END, on_game_end);
    dispatch_on(EV_DRAW_OFFERED, on_draw_offered);
    dispatch_on(EV_DRAW_DECLINED, on_draw_declined);
    dispatch_on(EV_REMATCH_REQUESTED, on_rematch_requested);
    dispatch_on(EV_REMATCH_DECLINED, on_rematch_declined);
    dispatch_on(EV_OPP_LEFT, on_opp_left);
    /* GAME_START is handled in ui_lobby.c for both cases; it calls ui_game_enter(). */

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_object_set(page, "margin", 16, NULL);

    bv = board_view_new(on_cell, NULL);
    gtk_box_pack_start(GTK_BOX(page), board_view_widget(bv), TRUE, TRUE, 0);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(side, SIDE_WIDTH, -1);
    l_title = side_label(side, FALSE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_me = side_label(side, TRUE);           /* "▶ X  <name up to 20 chars>" + ELO */
    l_opp = side_label(side, TRUE);
    gtk_box_pack_start(GTK_BOX(side), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    l_turn = side_label(side, TRUE);
    l_clock = side_label(side, FALSE);
    l_moves = side_label(side, FALSE);
    l_notice = side_label(side, TRUE);

    /* bottom of the side panel: the four controls, then (offline mode) the test keys */
    if (app.fake_mode) {
        GtkWidget *hint = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hint),
            "<small>Chế độ offline: đối thủ giả từ chối lần cầu hòa / mời đấu lại đầu tiên của "
            "mỗi ván, đồng ý từ lần thứ hai. <b>Ctrl+D</b> đối thủ cầu hòa · <b>Ctrl+R</b> đối "
            "thủ mời đấu lại · <b>Ctrl+L</b> đối thủ rời ván · <b>Ctrl+K</b> đối thủ mất kết "
            "nối · <b>Ctrl+T</b> người đang đi hết giờ.</small>");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), SIDE_WRAP_CHARS);
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_end(GTK_BOX(side), hint, FALSE, FALSE, 0);
    }
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    b_resign = control_button(grid, "Xin thua", 0, 0, G_CALLBACK(on_resign_clicked),
                              "destructive-action");
    b_draw = control_button(grid, "Cầu hòa", 1, 0, G_CALLBACK(on_draw_clicked), NULL);
    b_rematch = control_button(grid, "Đấu lại", 0, 1, G_CALLBACK(on_rematch_clicked),
                               "suggested-action");
    b_leave = control_button(grid, "Về sảnh", 1, 1, G_CALLBACK(on_leave_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(side), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), side, FALSE, FALSE, 0);
    return page;
}
