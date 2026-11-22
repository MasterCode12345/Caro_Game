/*
 * client/dispatch.c — routes every incoming message, real (net.c) or fake (fake.c), to the code
 * that waits for it. Both paths call dispatch(), so fake mode exercises exactly this code.
 * Owner: Nghĩa.
 *
 * RESPONSES — FIFO matching (docs/protocol.md §3)
 *   The server answers every request with exactly one response, and answers one client's
 *   requests in the order it received them. TCP keeps the order of our requests, so the n-th
 *   response always belongs to the n-th request still waiting. net_request() appends
 *   {COMMAND, callback} to a queue; each response pops the head. No request id is needed in
 *   the protocol. The COMMAND field of the response is compared with the head as a sanity check.
 *
 * EVENTS — messages whose first field is not a number (PLAYER_STATUS, GAME_START, ...) are
 *   unrequested, so they never touch the queue: they are looked up by name in a table filled
 *   with dispatch_on() at start-up: ui_lobby.c (PLAYER_STATUS, CHALLENGE_*, GAME_START) and
 *   ui_game.c (OPP_MOVE, DRAW_*, REMATCH_*, GAME_END, OPP_LEFT). The history page has no
 *   events: HISTORY, REPLAY and LEADERBOARD are plain request/response.
 */
#include "dispatch.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "frame.h"

typedef struct {
    char      *command;    /* first field of the request, e.g. "LOGIN" */
    ResponseCb cb;
    gpointer   user_data;
} Pending;

static GQueue      pending = G_QUEUE_INIT;
static GHashTable *events;             /* event name -> EventHandler */

/* ---- helpers ---------------------------------------------------------------------------------- */

gboolean field_int(const char *s, int *out)
{
    if (s == NULL || *s == '\0')
        return FALSE;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < INT_MIN || v > INT_MAX)
        return FALSE;
    *out = (int)v;
    return TRUE;
}

gboolean field_rc(const char *s, int *row, int *col)
{
    const char *comma = s != NULL ? strchr(s, ITEM_SEP) : NULL;
    if (comma == NULL || comma - s > 15)
        return FALSE;
    char first[16];
    memcpy(first, s, (size_t)(comma - s));
    first[comma - s] = '\0';
    return field_int(first, row) && field_int(comma + 1, col);
}

int item_split(char *item, char *parts[], int max)
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

/* ---- error responses -> Vietnamese (protocol.md §4) -------------------------------------------- */

/* Every reason token of protocol.md §4. A page that knows more (e.g. WHO is busy) says it
 * itself and uses error_text() for everything else. */
static const struct { const char *token, *text; } reason_texts[] = {
    { ERR_UNKNOWN_COMMAND,     "Server chưa hỗ trợ chức năng này" },
    { ERR_BAD_ARGS,            "Yêu cầu sai định dạng" },
    { ERR_INVALID_USERNAME,    "Tên đăng nhập không hợp lệ (3–20 ký tự: chữ không dấu, số, _)" },
    { ERR_INVALID_PASSWORD,    "Mật khẩu không hợp lệ (6–32 ký tự ASCII, không dấu cách, không |)" },
    { ERR_NOT_LOGGED_IN,       "Bạn chưa đăng nhập" },
    { ERR_INVALID_CREDENTIALS, "Sai tên đăng nhập hoặc mật khẩu" },
    { ERR_FORBIDDEN_STATE,     "Không thể làm việc này lúc này" },
    { ERR_NOT_IN_GAME,         "Bạn không ở trong ván này" },
    { ERR_USER_NOT_FOUND,      "Người chơi này không tồn tại hoặc không còn trực tuyến" },
    { ERR_CHALLENGE_NOT_FOUND, "Lời mời không còn hiệu lực" },
    { ERR_MATCH_NOT_FOUND,     "Không tìm thấy ván đấu này" },
    { ERR_NO_PENDING_OFFER,    "Lời đề nghị không còn hiệu lực" },
    { ERR_OPPONENT_LEFT,       "Đối thủ đã rời ván" },
    { ERR_USERNAME_TAKEN,      "Tên đã có người dùng, hãy chọn tên khác" },
    { ERR_ALREADY_LOGGED_IN,   "Tài khoản đang đăng nhập ở nơi khác" },
    { ERR_PLAYER_BUSY,         "Người chơi đang bận" },
    { ERR_CHALLENGE_PENDING,   "Bạn đang có một lời mời chưa được trả lời" },
    { ERR_OFFER_PENDING,       "Đang có một lời đề nghị chưa được trả lời" },
    { ERR_NOT_YOUR_TURN,       "Chưa đến lượt bạn" },
    { ERR_OUT_OF_BOARD,        "Ô này nằm ngoài bàn cờ" },
    { ERR_CELL_OCCUPIED,       "Ô này đã có quân" },
    { ERR_GAME_NOT_ACTIVE,     "Ván đã kết thúc" },
    { ERR_SERVER_ERROR,        "Lỗi phía server, hãy thử lại sau" },
};

/* Every error status code of protocol.md §4, for a token we do not know (or none). */
static const struct { int code; const char *text; } code_texts[] = {
    { ST_BAD_REQUEST,       "Yêu cầu không hợp lệ" },
    { ST_NOT_AUTHENTICATED, "Chưa đăng nhập hoặc sai thông tin đăng nhập" },
    { ST_FORBIDDEN,         "Không được phép làm việc này ở trạng thái hiện tại" },
    { ST_NOT_FOUND,         "Không tìm thấy (người chơi, lời mời hoặc ván đấu không còn)" },
    { ST_CONFLICT,          "Yêu cầu bị từ chối vì trùng hoặc đang bận" },
    { ST_NOT_YOUR_TURN,     "Chưa đến lượt bạn" },
    { ST_OUT_OF_BOARD,      "Ô này nằm ngoài bàn cờ" },
    { ST_CELL_OCCUPIED,     "Ô này đã có quân" },
    { ST_GAME_NOT_ACTIVE,   "Ván đã kết thúc" },
    { ST_SERVER_ERROR,      "Lỗi phía server, hãy thử lại sau" },
};

char *error_text(int code, char **f, int n)
{
    const char *reason = n >= 3 ? f[2] : "";
    for (size_t i = 0; i < G_N_ELEMENTS(reason_texts); i++)
        if (strcmp(reason, reason_texts[i].token) == 0)
            return g_strdup(reason_texts[i].text);
    for (size_t i = 0; i < G_N_ELEMENTS(code_texts); i++)
        if (code == code_texts[i].code)
            return g_strdup(code_texts[i].text);
    return g_strdup_printf("Lỗi %d: %s", code, *reason != '\0' ? reason : "không rõ lý do");
}

static gboolean is_status_code(const char *s)
{
    if (*s == '\0')
        return FALSE;
    for (; *s != '\0'; s++)
        if (!g_ascii_isdigit(*s))
            return FALSE;
    return TRUE;
}

static void pending_free(Pending *p)
{
    g_free(p->command);
    g_free(p);
}

void dispatch_trace(const char *dir, const char *msg)
{
    if (!app.debug)
        return;

    /* REGISTER|user|pass and LOGIN|user|pass: never print the password, even in a demo. */
    char *shown = NULL;
    if (g_str_has_prefix(msg, CMD_REGISTER "|") || g_str_has_prefix(msg, CMD_LOGIN "|")) {
        const char *second = strchr(strchr(msg, '|') + 1, '|');
        if (second != NULL)
            shown = g_strdup_printf("%.*s|******", (int)(second - msg), msg);
    }

    GDateTime *now = g_date_time_new_now_local();
    gchar *hms = g_date_time_format(now, "%H:%M:%S");
    g_printerr("[%s.%03d] %s %s\n", hms, g_date_time_get_microsecond(now) / 1000, dir,
               shown != NULL ? shown : msg);
    g_free(hms);
    g_date_time_unref(now);
    g_free(shown);
}

/* ---- registration ----------------------------------------------------------------------------- */

void dispatch_on(const char *event, EventHandler handler)
{
    if (events == NULL)
        events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_replace(events, g_strdup(event), (gpointer)handler);
}

void dispatch_expect(const char *payload, ResponseCb cb, gpointer user_data)
{
    Pending *p = g_new0(Pending, 1);
    const char *bar = strchr(payload, '|');
    p->command = bar != NULL ? g_strndup(payload, (gsize)(bar - payload)) : g_strdup(payload);
    p->cb = cb;
    p->user_data = user_data;
    g_queue_push_tail(&pending, p);
}

/* ---- incoming --------------------------------------------------------------------------------- */

static void handle_response(char **f, int n)
{
    Pending *p = g_queue_pop_head(&pending);
    if (p == NULL) {
        g_printerr("caro_client: warning: response '%s|%s' but no request is waiting; ignored\n",
                   f[0], n > 1 ? f[1] : "");
        return;
    }
    if (n < 2 || strcmp(f[1], p->command) != 0)
        g_printerr("caro_client: warning: response for '%s' while '%s' was expected\n",
                   n > 1 ? f[1] : "?", p->command);

    int code;
    if (!field_int(f[0], &code))
        code = ST_SERVER_ERROR;        /* digits only but absurdly long: treat as an error */
    if (p->cb != NULL)
        p->cb(code, f, n, p->user_data);
    pending_free(p);
}

static void handle_event(char **f, int n)
{
    EventHandler h = events != NULL ? (EventHandler)g_hash_table_lookup(events, f[0]) : NULL;
    if (h == NULL) {
        g_printerr("caro_client: ignoring unknown event '%s'\n", f[0]);
        return;
    }
    h(f, n);
}

void dispatch(char *msg)
{
    dispatch_trace("<<", msg);         /* before msg_split() cuts msg into pieces */

    char *f[MAX_FIELDS];
    int n = msg_split(msg, f, MAX_FIELDS);
    if (n < 1)
        return;
    if (is_status_code(f[0]))          /* a number first: response to one of our requests */
        handle_response(f, n);
    else                               /* a name first: unrequested event */
        handle_event(f, n);
}

void dispatch_fail_pending(void)
{
    char code[] = "0", reason[] = "disconnected";
    Pending *p;
    while ((p = g_queue_pop_head(&pending)) != NULL) {
        char *f[3] = { code, p->command, reason };
        if (p->cb != NULL)
            p->cb(RESP_DISCONNECTED, f, 3, p->user_data);
        pending_free(p);
    }
}

void dispatch_cleanup(void)
{
    Pending *p;
    while ((p = g_queue_pop_head(&pending)) != NULL)
        pending_free(p);
    if (events != NULL) {
        g_hash_table_destroy(events);
        events = NULL;
    }
}
