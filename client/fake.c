/*
 * client/fake.c — offline mode, `CARO_FAKE=1 ./build/caro_client`: a scripted server, so the UI
 * can be built before the real server feature exists.
 * Owner: Nghĩa.
 *
 * Replies follow docs/protocol.md and reach the UI exactly like real ones: net_request() has
 * already queued the callback, the reply goes through dispatch(), which pops the FIFO. Nothing
 * here touches a widget. Everything is delivered from a time-ordered outbox by a main-loop timer
 * (g_timeout_add, the same kind of source as g_idle_add): replies after FAKE_LATENCY_MS, which
 * imitates network delay and makes "buttons locked while waiting" visible; scripted events later.
 * An outbox entry can also be an action run at its due time (a fake player changes status, an
 * invitation expires), so the fake server's state changes when its event is sent, not earlier.
 *
 * Scenarios
 *   C1  PING -> 200 · REGISTER|taken -> 409 · LOGIN with password "wrong" -> 401 ·
 *       LOGIN|online -> 409 · other LOGIN -> 200 ELO 1000 · LOGOUT
 *   C2  LIST_PLAYERS: han 1016 IDLE, minh 980 IN_GAME, tuan 1100 IDLE (lan offline). Then
 *       +3 s lan comes online, +6 s minh is IDLE with ELO 996, +15 s minh goes OFFLINE.
 *       CHALLENGE han (or anyone else) -> accepted: GAME_START 2 s later, we play X
 *       CHALLENGE tuan -> CHALLENGE_DECLINED 2 s later
 *       CHALLENGE lan  -> no answer: CHALLENGE_EXPIRED after CHALLENGE_EXPIRE_SEC
 *       CHALLENGE minh while IN_GAME -> 409 player_busy
 *       fake_simulate_invite() (Ctrl+I / Ctrl+U in main.c): CHALLENGE_FROM from an idle fake
 *       player; accepting gives GAME_START (we play O, the challenger plays X as in §10);
 *       unanswered -> CHALLENGE_EXPIRED after 30 s; with withdraw -> CHALLENGE_CANCELLED after 5 s.
 *   C3  MOVE checked in the §6.3 order (400, 403, 433, 430, 431, 432) -> 200|MOVE|move_no, then
 *       0.5 s later OPP_MOVE. The fake opponent is greedy: it plays the empty cell that makes its
 *       own longest line (random among equal ones) and never blocks — leave it alone to lose,
 *       build your own five to win. When it plays X it opens 0.5 s after GAME_START.
 *       Five in a row or a full board -> GAME_END (±16 ELO, 0 for a draw; the real server uses
 *       the §9 formula), then PLAYER_STATUS with the opponent's new ELO.
 *   anything else -> 400 unknown_command (not faked yet)
 *
 * TODO(C4): RESIGN, DRAW_OFFER/DRAW_REPLY, REMATCH/REMATCH_REPLY, LEAVE_GAME.
 * TODO(C5): HISTORY, REPLAY (one 20-move game), LEADERBOARD, FIND_MATCH / CANCEL_MATCH.
 */
#include "fake.h"

#include <stdarg.h>
#include <string.h>

#include "dispatch.h"
#include "frame.h"
#include "proto.h"

#define FAKE_LATENCY_MS     300
#define FAKE_ANSWER_MS      2000     /* a fake player answers our challenge after 2 s */
#define FAKE_WITHDRAW_MS    5000     /* a withdrawn invitation is cancelled after 5 s */
#define FAKE_MAX_INVITES    8
#define FAKE_OPP_MOVE_MS    500      /* the fake opponent answers a move after 0.5 s */
#define FAKE_ELO_DELTA      16

/* ---- outbox: messages and actions, delivered in due-time order ---------------------------------- */

typedef void (*FakeAction)(gpointer arg);

typedef struct {
    guint      id;
    gint64     due_us;      /* g_get_monotonic_time() at which to deliver */
    char      *msg;         /* dispatched, or NULL */
    FakeAction action;      /* run first, or NULL */
    gpointer   arg;
} FakeMsg;

static GQueue outbox = G_QUEUE_INIT;   /* sorted by due_us, equal times in insertion order */
static guint  timer_id;
static guint  next_id = 1;

static gboolean on_timer(gpointer data);

static void schedule_head(void)
{
    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }
    FakeMsg *m = g_queue_peek_head(&outbox);
    if (m == NULL)
        return;
    gint64 wait_us = m->due_us - g_get_monotonic_time();
    timer_id = g_timeout_add(wait_us > 0 ? (guint)((wait_us + 999) / 1000) : 0, on_timer, NULL);
}

static void fake_msg_free(FakeMsg *m)
{
    g_free(m->msg);
    g_free(m);
}

static gboolean on_timer(gpointer data)
{
    (void)data;
    timer_id = 0;
    FakeMsg *m;
    while ((m = g_queue_peek_head(&outbox)) != NULL && m->due_us <= g_get_monotonic_time()) {
        g_queue_pop_head(&outbox);
        if (m->action != NULL)
            m->action(m->arg);
        if (m->msg != NULL)
            dispatch(m->msg);      /* the same entry point as a frame from the real socket */
        fake_msg_free(m);
    }
    schedule_head();
    return G_SOURCE_REMOVE;
}

static guint schedule(guint delay_ms, char *msg, FakeAction action, gpointer arg)
{
    FakeMsg *m = g_new0(FakeMsg, 1);
    m->id = next_id++;
    m->due_us = g_get_monotonic_time() + (gint64)delay_ms * 1000;
    m->msg = msg;
    m->action = action;
    m->arg = arg;

    /* Insert after the last entry due at or before this one (stable for equal times). */
    GList *l = outbox.tail;
    while (l != NULL && ((FakeMsg *)l->data)->due_us > m->due_us)
        l = l->prev;
    if (l != NULL)
        g_queue_insert_after(&outbox, l, m);
    else
        g_queue_push_head(&outbox, m);
    schedule_head();
    return m->id;
}

guint fake_push(guint delay_ms, const char *msg)
{
    return schedule(delay_ms, g_strdup(msg), NULL, NULL);
}

static guint later(guint delay_ms, FakeAction action, gpointer arg)
{
    return schedule(delay_ms, NULL, action, arg);
}

static void cancel(guint id)
{
    for (GList *l = outbox.head; l != NULL; l = l->next) {
        FakeMsg *m = l->data;
        if (m->id == id) {
            g_queue_delete_link(&outbox, l);
            fake_msg_free(m);
            schedule_head();
            return;
        }
    }
}

/* printf-style message after delay_ms */
static void push_fmt(guint delay_ms, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void push_fmt(guint delay_ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    schedule(delay_ms, msg, NULL, NULL);
}

#define reply(...) push_fmt(FAKE_LATENCY_MS, __VA_ARGS__)

/* ---- fake world ----------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    int         elo;
    const char *status;      /* PS_IDLE, PS_IN_GAME, PS_OFFLINE, ... */
} FakePlayer;

static const FakePlayer initial_players[] = {   /* sorted by name, like LIST_PLAYERS */
    { "han",  1016, PS_IDLE },
    { "lan",  1050, PS_OFFLINE },
    { "minh",  980, PS_IN_GAME },
    { "tuan", 1100, PS_IDLE },
};
#define N_PLAYERS G_N_ELEMENTS(initial_players)

typedef struct {
    int   cid;
    int   from;              /* index in players[] */
    guint timer;             /* its expiry / withdrawal */
} FakeInvite;

static FakePlayer   players[N_PLAYERS];
static SessionState fstate;                    /* the fake server's view of our session */
static char         me[USERNAME_MAX + 1];
static gboolean     script_started;
static int          next_cid = 17, next_gid = 5;   /* the numbers of the §10 sample session */
static int          out_cid, out_target;           /* our outgoing challenge (0 = none) */
static guint        out_timer;
static FakeInvite   invites[FAKE_MAX_INVITES];     /* invitations we have not answered */
static int          n_invites;
static int          invite_rr;                     /* round robin over possible inviters */

static int find_player(const char *name)
{
    for (int i = 0; i < (int)N_PLAYERS; i++)
        if (strcmp(players[i].name, name) == 0 && strcmp(players[i].name, me) != 0)
            return i;
    return -1;
}

static void set_status(int i, const char *status, int elo, guint delay_ms)
{
    players[i].status = status;
    players[i].elo = elo;
    push_fmt(delay_ms, "%s|%s|%s|%d", EV_PLAYER_STATUS, players[i].name, status, elo);
}

/* strcmp, not ==: C does not promise that equal string literals share one address. */
static gboolean has_status(int i, const char *status)
{
    return strcmp(players[i].status, status) == 0;
}

static int find_invite(int cid)
{
    for (int i = 0; i < n_invites; i++)
        if (invites[i].cid == cid)
            return i;
    return -1;
}

static void drop_invite(int k)
{
    cancel(invites[k].timer);
    invites[k] = invites[--n_invites];
}

/* Entering a game or logging out removes every pending challenge of ours (§6.2). The other
 * sides would get CHALLENGE_CANCELLED; they are fake, so nothing is sent to us. */
static void clear_challenges(void)
{
    if (out_cid != 0) {
        cancel(out_timer);
        out_cid = 0;
    }
    while (n_invites > 0)
        drop_invite(n_invites - 1);
}

/* -- the game -- */

static struct {
    gboolean exists;         /* GAME_START sent (game running or ended) */
    gboolean running;
    int      gid, opp;
    char     me_sym, turn;
    int      move_no;
    char     cells[BOARD_SIZE][BOARD_SIZE];
} game;
static int me_elo = ELO_START;

static const int DIRS[4][2] = { { 0, 1 }, { 1, 0 }, { 1, 1 }, { 1, -1 } };

static char other(char sym)
{
    return sym == 'X' ? 'O' : 'X';
}

static gboolean in_board(int r, int c)
{
    return r >= 0 && c >= 0 && r < BOARD_SIZE && c < BOARD_SIZE;
}

/* Cells of `mark` next to (r,c) going (dr,dc), (r,c) itself not counted. */
static int count_dir(int r, int c, int dr, int dc, char mark)
{
    int k = 0;
    for (int rr = r + dr, cc = c + dc; in_board(rr, cc) && game.cells[rr][cc] == mark; rr += dr, cc += dc)
        k++;
    return k;
}

/* After a mark at (r,c): five or more in a row? line = 5 consecutive cells including (r,c). */
static gboolean find_five(int r, int c, int line[WIN_LENGTH][2])
{
    char m = game.cells[r][c];
    for (int d = 0; d < 4; d++) {
        int dr = DIRS[d][0], dc = DIRS[d][1];
        int back = count_dir(r, c, -dr, -dc, m), fwd = count_dir(r, c, dr, dc, m);
        if (back + fwd + 1 < WIN_LENGTH)
            continue;
        int w = MAX(-back, -(WIN_LENGTH - 1));        /* window start, relative to (r,c) */
        if (w + WIN_LENGTH - 1 > fwd)
            w = fwd - (WIN_LENGTH - 1);
        for (int i = 0; i < WIN_LENGTH; i++) {
            line[i][0] = r + (w + i) * dr;
            line[i][1] = c + (w + i) * dc;
        }
        return TRUE;
    }
    return FALSE;
}

/* Longest line `mark` would have through the empty cell (r,c). */
static int line_if_played(int r, int c, char mark)
{
    int best = 1;
    for (int d = 0; d < 4; d++)
        best = MAX(best, 1 + count_dir(r, c, DIRS[d][0], DIRS[d][1], mark)
                            + count_dir(r, c, -DIRS[d][0], -DIRS[d][1], mark));
    return best;
}

/* winner: 'X', 'O', or 0 for a draw. GAME_END to us, then the opponent's new ELO to "others". */
static void end_game(char winner, const char *reason, int line[WIN_LENGTH][2], guint delay_ms)
{
    game.running = FALSE;
    int delta = winner == 0 ? 0 : winner == game.me_sym ? FAKE_ELO_DELTA : -FAKE_ELO_DELTA;
    me_elo += delta;
    players[game.opp].elo -= delta;

    GString *s = g_string_new(NULL);
    g_string_printf(s, "%s|%d|%s|%s|%d|", EV_GAME_END, game.gid,
                    winner == 'X' ? RES_X_WIN : winner == 'O' ? RES_O_WIN : RES_DRAW, reason, me_elo);
    if (delta == 0)
        g_string_append(s, "0");                 /* §2: signed deltas, but 0 has no sign */
    else
        g_string_append_printf(s, "%+d", delta);
    for (int i = 0; line != NULL && i < WIN_LENGTH; i++)
        g_string_append_printf(s, "|%d,%d", line[i][0], line[i][1]);
    fake_push(delay_ms, s->str);
    g_string_free(s, TRUE);
    set_status(game.opp, PS_IN_GAME, players[game.opp].elo, delay_ms);
}

/* After a mark at (r,c) by `sym`: GAME_END if it won or filled the board. TRUE if the game ended. */
static gboolean check_end(int r, int c, char sym, guint delay_ms)
{
    int line[WIN_LENGTH][2];
    if (find_five(r, c, line))
        end_game(sym, END_FIVE, line, delay_ms);
    else if (game.move_no == BOARD_SIZE * BOARD_SIZE)
        end_game(0, END_BOARD_FULL, NULL, delay_ms);
    else
        return FALSE;
    return TRUE;
}

static void act_opp_move(gpointer arg)
{
    char sym = other(game.me_sym);
    if (!game.running || game.gid != GPOINTER_TO_INT(arg) || game.turn != sym)
        return;
    int best = 0, n_best = 0, pick_r = -1, pick_c = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (game.cells[r][c] != '.')
                continue;
            int len = line_if_played(r, c, sym);
            if (len > best) {
                best = len;
                n_best = 0;
            }
            if (len == best && g_random_int_range(0, ++n_best) == 0) {   /* uniform among ties */
                pick_r = r;
                pick_c = c;
            }
        }
    }
    if (pick_r < 0)
        return;
    game.cells[pick_r][pick_c] = sym;
    game.move_no++;
    game.turn = game.me_sym;
    push_fmt(0, "%s|%d|%d|%d|%d", EV_OPP_MOVE, game.gid, pick_r, pick_c, game.move_no);
    check_end(pick_r, pick_c, sym, 0);
}

static void start_game(int opp, char my_symbol, guint delay_ms)
{
    clear_challenges();
    fstate = SESSION_IN_GAME;
    memset(&game, 0, sizeof game);
    game.exists = game.running = TRUE;
    game.gid = next_gid++;
    game.opp = opp;
    game.me_sym = my_symbol;
    game.turn = 'X';
    memset(game.cells, '.', sizeof game.cells);

    push_fmt(delay_ms, "%s|%d|%d|%c|%s|%d|%d", EV_GAME_START, game.gid, BOARD_SIZE, my_symbol,
             players[opp].name, players[opp].elo, SEC_PER_MOVE_NORMAL);
    set_status(opp, PS_IN_GAME, players[opp].elo, delay_ms);   /* others learn it after GAME_START */
    if (my_symbol == 'O')                                      /* the fake opponent opens */
        later(delay_ms + FAKE_OPP_MOVE_MS, act_opp_move, GINT_TO_POINTER(game.gid));
}

/* -- scripted actions (run at their due time) -- */

static void act_lan_online(gpointer arg)
{
    (void)arg;
    int i = find_player("lan");
    if (i >= 0 && has_status(i, PS_OFFLINE))
        set_status(i, PS_IDLE, players[i].elo, 0);
}

static void act_minh_idle(gpointer arg)
{
    (void)arg;
    int i = find_player("minh");
    if (i >= 0 && has_status(i, PS_IN_GAME))
        set_status(i, PS_IDLE, 996, 0);
}

static void act_minh_offline(gpointer arg)
{
    (void)arg;
    int i = find_player("minh");
    if (i >= 0 && has_status(i, PS_IDLE))       /* not while playing with us */
        set_status(i, PS_OFFLINE, players[i].elo, 0);
}

static void act_out_accept(gpointer arg)
{
    if (out_cid == GPOINTER_TO_INT(arg)) {
        out_cid = 0;
        start_game(out_target, 'X', 0);   /* the challenger plays X, as in §10 */
    }
}

static void act_out_decline(gpointer arg)
{
    int cid = GPOINTER_TO_INT(arg);
    if (out_cid == cid) {
        out_cid = 0;
        push_fmt(0, "%s|%d|%s", EV_CHALLENGE_DECLINED, cid, players[out_target].name);
    }
}

static void act_out_expire(gpointer arg)
{
    int cid = GPOINTER_TO_INT(arg);
    if (out_cid == cid) {
        out_cid = 0;
        push_fmt(0, "%s|%d", EV_CHALLENGE_EXPIRED, cid);
    }
}

static void act_invite_end(gpointer arg, const char *event)
{
    int cid = GPOINTER_TO_INT(arg);
    int k = find_invite(cid);
    if (k < 0)
        return;
    invites[k] = invites[--n_invites];  /* its timer is this very entry: nothing to cancel */
    push_fmt(0, "%s|%d", event, cid);
}

static void act_invite_expire(gpointer arg)
{
    act_invite_end(arg, EV_CHALLENGE_EXPIRED);
}

static void act_invite_withdraw(gpointer arg)
{
    act_invite_end(arg, EV_CHALLENGE_CANCELLED);
}

/* ---- session ---------------------------------------------------------------------------------------- */

void fake_start(void)
{
    fake_stop();
}

void fake_stop(void)
{
    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }
    FakeMsg *m;
    while ((m = g_queue_pop_head(&outbox)) != NULL)
        fake_msg_free(m);

    memcpy(players, initial_players, sizeof players);
    memset(&game, 0, sizeof game);
    me_elo = ELO_START;
    fstate = SESSION_CONNECTED;
    me[0] = '\0';
    script_started = FALSE;
    out_cid = 0;
    n_invites = 0;
}

/* ---- requests ---------------------------------------------------------------------------------------- */

#define IN(s) (1u << (s))

/* The protocol state rule (§4): not allowed here -> 401 if not logged in, else 403. */
static gboolean allowed(const char *cmd, unsigned states)
{
    if (states & IN(fstate))
        return TRUE;
    if (fstate == SESSION_CONNECTED)
        reply("%d|%s|%s", ST_NOT_AUTHENTICATED, cmd, ERR_NOT_LOGGED_IN);
    else
        reply("%d|%s|%s", ST_FORBIDDEN, cmd, ERR_FORBIDDEN_STATE);
    return FALSE;
}

/* Username/password format is already checked by ui_login before sending; like the real
 * server, the fake only checks the field count here. */
static void fake_register(char **f, int n)
{
    if (!allowed(CMD_REGISTER, IN(SESSION_CONNECTED)))
        return;
    if (n != 3)
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_REGISTER, ERR_BAD_ARGS);
    else if (strcmp(f[1], "taken") == 0)
        reply("%d|%s|%s", ST_CONFLICT, CMD_REGISTER, ERR_USERNAME_TAKEN);
    else
        reply("%d|%s", ST_CREATED, CMD_REGISTER);
}

static void fake_login(char **f, int n)
{
    if (!allowed(CMD_LOGIN, IN(SESSION_CONNECTED)))
        return;
    if (n != 3) {
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_LOGIN, ERR_BAD_ARGS);
    } else if (strcmp(f[2], "wrong") == 0) {
        reply("%d|%s|%s", ST_NOT_AUTHENTICATED, CMD_LOGIN, ERR_INVALID_CREDENTIALS);
    } else if (strcmp(f[1], "online") == 0) {
        reply("%d|%s|%s", ST_CONFLICT, CMD_LOGIN, ERR_ALREADY_LOGGED_IN);
    } else {
        fstate = SESSION_LOBBY;
        g_strlcpy(me, f[1], sizeof me);
        reply("%d|%s|%s|%d|%d|%d|%d", ST_OK, CMD_LOGIN, me, me_elo, 0, 0, 0);
    }
}

static void fake_logout(void)
{
    if (!allowed(CMD_LOGOUT, IN(SESSION_LOBBY) | IN(SESSION_IN_QUEUE)))
        return;
    clear_challenges();
    fstate = SESSION_CONNECTED;
    reply("%d|%s", ST_OK, CMD_LOGOUT);
}

static void fake_list_players(void)
{
    if (!allowed(CMD_LIST_PLAYERS, IN(SESSION_LOBBY) | IN(SESSION_IN_QUEUE) | IN(SESSION_IN_GAME)))
        return;
    GString *s = g_string_new(NULL);
    int count = 0;
    for (int i = 0; i < (int)N_PLAYERS; i++) {
        if (has_status(i, PS_OFFLINE) || strcmp(players[i].name, me) == 0)
            continue;
        g_string_append_printf(s, "|%s,%d,%s", players[i].name, players[i].elo, players[i].status);
        count++;
    }
    reply("%d|%s|%d%s", ST_OK, CMD_LIST_PLAYERS, count, s->str);
    g_string_free(s, TRUE);

    if (!script_started) {             /* the lobby comes alive once per session */
        script_started = TRUE;
        later(3000, act_lan_online, NULL);
        later(6000, act_minh_idle, NULL);
        later(15000, act_minh_offline, NULL);
    }
}

static void fake_challenge(char **f, int n)
{
    if (!allowed(CMD_CHALLENGE, IN(SESSION_LOBBY)))
        return;
    int size, sec;
    int t = n == 4 ? find_player(f[1]) : -1;
    if (n != 4 || !field_int(f[2], &size) || !field_int(f[3], &sec) || size != BOARD_SIZE
        || (sec != SEC_PER_MOVE_FAST && sec != SEC_PER_MOVE_NORMAL && sec != SEC_PER_MOVE_SLOW)
        || strcmp(f[1], me) == 0) {
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_CHALLENGE, ERR_BAD_ARGS);
    } else if (t < 0 || has_status(t, PS_OFFLINE)) {
        reply("%d|%s|%s", ST_NOT_FOUND, CMD_CHALLENGE, ERR_USER_NOT_FOUND);
    } else if (!has_status(t, PS_IDLE)) {
        reply("%d|%s|%s", ST_CONFLICT, CMD_CHALLENGE, ERR_PLAYER_BUSY);
    } else if (out_cid != 0) {
        reply("%d|%s|%s", ST_CONFLICT, CMD_CHALLENGE, ERR_CHALLENGE_PENDING);
    } else {
        out_cid = next_cid++;
        out_target = t;
        reply("%d|%s|%d", ST_OK, CMD_CHALLENGE, out_cid);
        gpointer arg = GINT_TO_POINTER(out_cid);
        if (strcmp(players[t].name, "tuan") == 0)
            out_timer = later(FAKE_LATENCY_MS + FAKE_ANSWER_MS, act_out_decline, arg);
        else if (strcmp(players[t].name, "lan") == 0)
            out_timer = later(FAKE_LATENCY_MS + CHALLENGE_EXPIRE_SEC * 1000, act_out_expire, arg);
        else
            out_timer = later(FAKE_LATENCY_MS + FAKE_ANSWER_MS, act_out_accept, arg);
    }
}

static void fake_challenge_cancel(char **f, int n)
{
    if (!allowed(CMD_CHALLENGE_CANCEL, IN(SESSION_LOBBY)))
        return;
    int cid;
    if (n != 2 || !field_int(f[1], &cid)) {
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_CHALLENGE_CANCEL, ERR_BAD_ARGS);
    } else if (out_cid == 0 || cid != out_cid) {
        reply("%d|%s|%s", ST_NOT_FOUND, CMD_CHALLENGE_CANCEL, ERR_CHALLENGE_NOT_FOUND);
    } else {
        cancel(out_timer);
        out_cid = 0;
        reply("%d|%s", ST_OK, CMD_CHALLENGE_CANCEL);
    }
}

static void fake_challenge_reply(char **f, int n)
{
    if (!allowed(CMD_CHALLENGE_REPLY, IN(SESSION_LOBBY)))
        return;
    int cid, k;
    if (n != 3 || !field_int(f[1], &cid)
        || (strcmp(f[2], ARG_ACCEPT) != 0 && strcmp(f[2], ARG_DECLINE) != 0)) {
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_CHALLENGE_REPLY, ERR_BAD_ARGS);
    } else if ((k = find_invite(cid)) < 0) {
        reply("%d|%s|%s", ST_NOT_FOUND, CMD_CHALLENGE_REPLY, ERR_CHALLENGE_NOT_FOUND);
    } else if (strcmp(f[2], ARG_DECLINE) == 0) {
        drop_invite(k);
        reply("%d|%s", ST_OK, CMD_CHALLENGE_REPLY);
    } else if (!has_status(invites[k].from, PS_IDLE)) {
        drop_invite(k);
        reply("%d|%s|%s", ST_CONFLICT, CMD_CHALLENGE_REPLY, ERR_PLAYER_BUSY);
    } else {
        int from = invites[k].from;
        reply("%d|%s", ST_OK, CMD_CHALLENGE_REPLY);
        start_game(from, 'O', FAKE_LATENCY_MS);    /* response first, then GAME_START (§3) */
    }
}

/* MOVE|gid|row|col — checks in the exact order of docs/protocol.md §6.3, first failure wins. */
static void fake_move(char **f, int n)
{
    if (!allowed(CMD_MOVE, IN(SESSION_IN_GAME)))
        return;
    int gid, r, c;
    if (n != 4 || !field_int(f[1], &gid) || !field_int(f[2], &r) || !field_int(f[3], &c))
        reply("%d|%s|%s", ST_BAD_REQUEST, CMD_MOVE, ERR_BAD_ARGS);
    else if (!game.exists || gid != game.gid)
        reply("%d|%s|%s", ST_FORBIDDEN, CMD_MOVE, ERR_NOT_IN_GAME);
    else if (!game.running)
        reply("%d|%s|%s", ST_GAME_NOT_ACTIVE, CMD_MOVE, ERR_GAME_NOT_ACTIVE);
    else if (game.turn != game.me_sym)
        reply("%d|%s|%s", ST_NOT_YOUR_TURN, CMD_MOVE, ERR_NOT_YOUR_TURN);
    else if (!in_board(r, c))
        reply("%d|%s|%s", ST_OUT_OF_BOARD, CMD_MOVE, ERR_OUT_OF_BOARD);
    else if (game.cells[r][c] != '.')
        reply("%d|%s|%s", ST_CELL_OCCUPIED, CMD_MOVE, ERR_CELL_OCCUPIED);
    else {
        game.cells[r][c] = game.me_sym;
        game.move_no++;
        game.turn = other(game.me_sym);
        reply("%d|%s|%d", ST_OK, CMD_MOVE, game.move_no);        /* response first (§3) ... */
        if (!check_end(r, c, game.me_sym, FAKE_LATENCY_MS))      /* ... then GAME_END */
            later(FAKE_LATENCY_MS + FAKE_OPP_MOVE_MS, act_opp_move, GINT_TO_POINTER(game.gid));
    }
}

gboolean fake_simulate_invite(gboolean withdraw)
{
    if (fstate != SESSION_LOBBY || n_invites == FAKE_MAX_INVITES)
        return FALSE;
    int from = -1;
    for (int tries = 0; tries < (int)N_PLAYERS && from < 0; tries++) {
        int i = invite_rr++ % (int)N_PLAYERS;
        if (has_status(i, PS_IDLE) && strcmp(players[i].name, me) != 0)
            from = i;
    }
    if (from < 0)
        return FALSE;

    FakeInvite *inv = &invites[n_invites++];
    inv->cid = next_cid++;
    inv->from = from;
    push_fmt(0, "%s|%d|%s|%d|%d|%d", EV_CHALLENGE_FROM, inv->cid, players[from].name,
             players[from].elo, BOARD_SIZE, SEC_PER_MOVE_NORMAL);
    gpointer arg = GINT_TO_POINTER(inv->cid);
    inv->timer = withdraw ? later(FAKE_WITHDRAW_MS, act_invite_withdraw, arg)
                          : later(CHALLENGE_EXPIRE_SEC * 1000, act_invite_expire, arg);
    return TRUE;
}

void fake_handle(const char *payload)
{
    char *copy = g_strdup(payload);
    char *f[MAX_FIELDS];
    int n = msg_split(copy, f, MAX_FIELDS);
    const char *cmd = f[0];

    if (strcmp(cmd, CMD_PING) == 0)
        reply("%d|%s", ST_OK, CMD_PING);
    else if (strcmp(cmd, CMD_REGISTER) == 0)
        fake_register(f, n);
    else if (strcmp(cmd, CMD_LOGIN) == 0)
        fake_login(f, n);
    else if (strcmp(cmd, CMD_LOGOUT) == 0)
        fake_logout();
    else if (strcmp(cmd, CMD_LIST_PLAYERS) == 0)
        fake_list_players();
    else if (strcmp(cmd, CMD_CHALLENGE) == 0)
        fake_challenge(f, n);
    else if (strcmp(cmd, CMD_CHALLENGE_CANCEL) == 0)
        fake_challenge_cancel(f, n);
    else if (strcmp(cmd, CMD_CHALLENGE_REPLY) == 0)
        fake_challenge_reply(f, n);
    else if (strcmp(cmd, CMD_MOVE) == 0)
        fake_move(f, n);
    else
        reply("%d|%s|%s", ST_BAD_REQUEST, cmd, ERR_UNKNOWN_COMMAND);

    g_free(copy);
}
