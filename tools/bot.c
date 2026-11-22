/*
 * tools/bot.c — N bots in one process, ONE POSIX THREAD PER BOT (Lec06), for load testing.
 * Usage: ./build/bot <host> <port> <N> [--secs S] [--idle K] [--match]
 * Owner: Nghĩa.
 *
 *   N         number of bots, even: bot_0 .. bot_<N-1>, all with password "botpass1"
 *   --secs S  stop by itself after S seconds (default: run until Ctrl+C or SIGTERM)
 *   --idle K  also open K "slow clients" (see THE --idle EXPERIMENT below)
 *   --match   pair through matchmaking (FIND_MATCH, server stage S7) instead of challenges (S2)
 *
 * WHAT ONE BOT DOES
 *   REGISTER bot_<i>|botpass1 (409 = the account exists from an earlier run: fine) -> LOGIN ->
 *   pair -> play -> GAME_END -> LEAVE_GAME -> pair again, until told to stop.
 *   Pairing (default): bot 2i sends CHALLENGE|bot_<2i+1>|15|30; bot 2i+1 accepts a CHALLENGE_FROM
 *   of its partner only and declines anyone else. 404 user_not_found (partner not logged in yet)
 *   or 409 player_busy (partner still in the last game) -> wait 200 ms and try again.
 *   Playing: the bot keeps its own copy of the board, updated from 200|MOVE (its own move, only
 *   once the server accepted it) and OPP_MOVE, and plays a random empty cell on its turn.
 *   Every other event (PLAYER_STATUS, OPP_LEFT, DRAW_OFFERED, ...) is ignored.
 *
 * WHY THREADS HERE, WHEN THE SERVER USES select() (Lec05, Lec06)
 *   The bots are independent clients that share nothing but a few counters. With one thread per
 *   bot, each bot is plain blocking code ("send the request, wait for its response"), and a
 *   recv() that blocks only blocks that one bot. The server is the opposite case: its clients
 *   share the lobby, the challenges and the games, so threads would need locks everywhere
 *   (see "Why not threads" in CLAUDE.md).
 *
 * PRIVATE AND SHARED DATA
 *   - Bot bots[N] is allocated by main() and lives until every thread has been joined; thread i
 *     gets &bots[i], never the address of a local variable of the creating loop (that variable
 *     changes at the next iteration while the new thread may still be reading it). The socket,
 *     FrameBuf, board, counters and latency samples in bots[i] are used by thread i only, so
 *     they need no lock; main() reads them after pthread_join(), which guarantees that it sees
 *     everything the thread wrote.
 *   - Stats g_stats (bots online, games, moves, error responses by code) is shared, so every
 *     access holds the ONE mutex g_stats_lock. The critical sections are only additions (and
 *     main's struct copy): no send(), recv() or printf() while holding the lock, so no thread
 *     ever waits for another thread's I/O.
 *   - g_stop and g_running are atomic_int (C11 <stdatomic.h>). A plain int written by one thread
 *     while another reads it is a data race, undefined behaviour in C11 (the compiler may keep
 *     it in a register and never see the change); an atomic flag needs no mutex.
 *
 * LATENCY
 *   clock_gettime(CLOCK_MONOTONIC) just before MOVE is sent and when 200|MOVE arrives. The
 *   monotonic clock never jumps when the wall clock is changed (NTP, the user), so the
 *   difference is always a real duration. Each thread appends to its own array; after the joins
 *   main() merges them and prints mean, P95 and max in ms.
 *
 * STOPPING CLEANLY
 *   - SIGPIPE is ignored: send() to a closed connection returns EPIPE instead of killing us.
 *   - main() blocks SIGINT, SIGTERM and SIGALRM with pthread_sigmask() BEFORE creating any thread;
 *     a new thread inherits its creator's signal mask, so these signals never interrupt a bot.
 *     main() then takes them one at a time with sigwait(): SIGALRM comes every second from
 *     setitimer() (progress every 10 s, the --secs deadline), SIGINT/SIGTERM mean "stop". There
 *     is no signal handler, so no async-signal-safety rules: the stop code is ordinary code.
 *   - Stop: main() sets g_stop. Every bot socket has SO_RCVTIMEO = 1 s, so a thread blocked in
 *     recv() gets -1/EAGAIN at least once a second, sees g_stop and returns. main() joins every
 *     thread, then prints the final table. The bots just close their sockets; the server ends
 *     their sessions (a running game is won by the opponent, reason DISCONNECT).
 *
 * THE --idle EXPERIMENT ("slow client", Lec06 slides 2 and 16)
 *   K extra connections each send the first 2 bytes of a 4-byte frame header and then nothing
 *   for the whole run (main() keeps the fds, no thread needed). A server that blocks in recv()
 *   until a frame is complete would be stuck on the first of them. Our select() server only
 *   reads a socket that select() reports readable and keeps the 2 bytes in that client's
 *   FrameBuf, so the bots' latency with --idle 50 should stay the same as with --idle 0.
 *
 * DATA RACE CHECK with ThreadSanitizer (not part of the Makefile):
 *   cc -std=c11 -Wall -Wextra -g -O1 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Icommon \
 *      -fsanitize=thread -pthread tools/bot.c common/frame.c -o build/bot_tsan
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>      /* setitimer, struct timeval */
#include <sys/types.h>

#include "frame.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0     /* macOS: SIGPIPE is ignored in main() instead */
#endif

#define BOT_MAX          1000   /* the server's select() handles at most FD_SETSIZE sockets */
#define IDLE_MAX         1000
#define BOT_PASSWORD     "botpass1"
#define BOT_FIELDS       16     /* a bot only looks at the first fields of a message */
#define PENDING_MAX      8      /* requests sent and not answered yet, per bot */
#define RETRY_MS         200    /* partner offline or busy, or our challenge was refused */
#define ERROR_RETRY_MS   1000   /* any other error: slow down instead of spinning */
#define RECV_TIMEOUT_S   1      /* SO_RCVTIMEO: a blocked thread looks at g_stop this often */
#define REPORT_EVERY_S   10
#define TABLE_MAX_ROWS   40     /* the final table lists every bot only up to this N */
#define CODE_MIN         300    /* error responses are counted by code, 300..599 */
#define CODE_SLOTS       300

/* Return values of every function that reads from a bot's socket. */
enum {
    B_FAIL  = -1,   /* connection lost or unexpected answer: b->end says why, the bot ends */
    B_STOP  = 0,    /* g_stop was set */
    B_OK    = 1,    /* step done, or an event / a fire-and-forget response was handled */
    B_REPLY = 2     /* the response the bot was waiting for is in *out */
};

typedef struct {
    int  code;
    char arg[48];                 /* first data field, "" if none: cid, move_no or reason token */
} Reply;

typedef struct {
    char cmd[24];                 /* COMMAND of the request, checked against the response */
    int  wait;                    /* 1: the bot's flow waits for it; 0: fire and forget */
} Pending;

typedef struct {
    int  active;                  /* GAME_START received, LEAVE_GAME not done yet */
    int  over;                    /* GAME_END received */
    int  gid, size;
    char mine, theirs;            /* 'X' or 'O' */
    int  my_turn;
    char cell[BOARD_SIZE_MAX][BOARD_SIZE_MAX];   /* 0 empty, 'X', 'O', '?' = server said occupied */
} BotGame;

typedef struct {
    /* Written by main() before pthread_create(), only read by the thread afterwards. */
    int      id;
    char     name[USERNAME_MAX + 1];
    char     partner[USERNAME_MAX + 1];
    unsigned seed;                /* rand_r() state, one per thread */

    /* Used by this bot's thread only; main() reads them after pthread_join(). */
    int      fd;
    FrameBuf in;
    Pending  pending[PENDING_MAX];  /* FIFO: the server answers in request order (protocol §3) */
    int      p_head, p_count;
    BotGame  g;
    int      out_open, out_cid;     /* bot 2i: our challenge, waiting for an answer */
    int      in_open, in_cid;       /* bot 2i+1: the partner's challenge, to accept */
    int      logged_in;
    long     games, moves;
    double  *lat;                   /* MOVE -> 200|MOVE, in ms */
    size_t   lat_n, lat_cap;
    long     lat_dropped;           /* realloc() failed: sample not kept */
    char     end[96];               /* why the thread ended */
} Bot;

typedef struct {
    int  online;                  /* bots logged in right now */
    long games;                   /* games finished, counted once (by the X player) */
    long moves;                   /* 200|MOVE received */
    long errors[CODE_SLOTS];      /* error responses: errors[code - CODE_MIN] */
} Stats;

typedef struct {
    const char *host, *port;
    int n, secs, idle, match;
} Options;

/* ---- the only data shared by the bot threads ------------------------------------------------ */

static Stats           g_stats;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static atomic_int g_stop;         /* set by main(): every thread returns within ~1 s */
static atomic_int g_running;      /* bot threads that have not returned yet */
static atomic_int g_warned;       /* "the server lacks this command" printed once, not N times */

/* Set by main() before the first pthread_create(), read-only afterwards: pthread_create()
 * makes these writes visible to the new thread, so no lock is needed. */
static const char *g_host, *g_port;
static int         g_match;

/* pthread_mutex_lock() on an initialised default mutex fails only when misused, so its
 * result is not checked. The critical sections are the additions and nothing else. */
static void stats_add(int online, long games, long moves)
{
    pthread_mutex_lock(&g_stats_lock);
    g_stats.online += online;
    g_stats.games  += games;
    g_stats.moves  += moves;
    pthread_mutex_unlock(&g_stats_lock);
}

static void stats_error(int code)  /* code is 300..599, checked by on_reply() */
{
    pthread_mutex_lock(&g_stats_lock);
    g_stats.errors[code - CODE_MIN]++;
    pthread_mutex_unlock(&g_stats_lock);
}

/* main() copies the struct under the lock and prints the copy after unlocking: a slow terminal
 * must never keep the bots waiting for the lock. */
static Stats stats_snapshot(void)
{
    pthread_mutex_lock(&g_stats_lock);
    Stats copy = g_stats;
    pthread_mutex_unlock(&g_stats_lock);
    return copy;
}

/* ---- small helpers -------------------------------------------------------------------------- */

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Sleeps in 100 ms steps so that a stop request is seen quickly. */
static void sleep_ms(int ms)
{
    while (ms > 0 && !atomic_load(&g_stop)) {
        int step = ms < 100 ? ms : 100;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

/* strtol + end pointer: the whole string must be a number (errno is per thread). */
static int parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return 0;
    *out = (int)v;
    return 1;
}

/* strerror() may format into one static buffer shared by every thread; strerror_r() writes into
 * the caller's buffer. (_DEFAULT_SOURCE and macOS give the POSIX version, which returns int.) */
static const char *errstr(int e, char *buf, size_t cap)
{
    if (strerror_r(e, buf, cap) != 0)
        snprintf(buf, cap, "error %d", e);
    return buf;
}

/* getaddrinfo() (thread-safe, unlike gethostbyname()) + connect() to the first address that
 * answers. Returns the fd, or -1 with a message in err. */
static int connect_to(const char *host, const char *port, char *err, size_t errlen)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%s: %s", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1, saved_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        char buf[64];
        snprintf(err, errlen, "connect %s:%s: %s", host, port,
                 errstr(saved_errno, buf, sizeof buf));
    }
    return fd;
}

/* SO_RCVTIMEO: recv() gives up after 1 s with EAGAIN, so a thread that waits for a message still
 * looks at g_stop once a second. SO_SNDTIMEO: a server that stops reading cannot block a thread
 * in send() forever. Together they make sure pthread_join() always returns. */
static int set_timeouts(int fd)
{
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
        return -1;
    return 0;
}

/* ---- one bot's connection: requests, responses, events -------------------------------------- */

static void fail(Bot *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Bot *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->end, sizeof b->end, fmt, ap);
    va_end(ap);
}

/* Next complete message into msg: B_OK, B_STOP or B_FAIL.
 * Blocking recv() is fine here: it blocks this bot's thread only. */
static int read_message(Bot *b, char *msg, size_t cap)
{
    for (;;) {
        int r = frame_next(&b->in, msg, cap);    /* drain the buffer before the next recv() */
        if (r == 1)
            return B_OK;
        if (r < 0) {
            fail(b, "malformed frame from the server");
            return B_FAIL;
        }
        if (atomic_load(&g_stop))
            return B_STOP;

        errno = 0;
        r = frame_recv(&b->in, b->fd);
        if (r > 0)
            continue;
        if (r == 0) {
            fail(b, "server closed the connection");
            return B_FAIL;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;          /* SO_RCVTIMEO: 1 s without data, not an error: look at g_stop */
        char buf[64];
        fail(b, "recv: %s", errstr(errno, buf, sizeof buf));
        return B_FAIL;
    }
}

/* Queues the request's COMMAND in the FIFO, then sends it. */
static int send_request(Bot *b, const char *payload, int wait)
{
    if (b->p_count == PENDING_MAX) {
        fail(b, "too many requests without a response");
        return -1;
    }
    Pending *p = &b->pending[(b->p_head + b->p_count) % PENDING_MAX];
    int len = (int)strcspn(payload, "|");
    snprintf(p->cmd, sizeof p->cmd, "%.*s", len, payload);
    p->wait = wait;
    b->p_count++;

    if (frame_send(b->fd, payload) < 0) {
        char buf[64];
        fail(b, "send: %s", errstr(errno, buf, sizeof buf));
        return -1;
    }
    return 0;
}

/* A response belongs to the oldest request without one (protocol §3). Error codes go to the
 * shared stats, except 409 to REGISTER, which only means "registered in an earlier run". */
static int on_reply(Bot *b, char **f, int n, Reply *out)
{
    if (b->p_count == 0) {
        fail(b, "response \"%s\" with no request pending", f[0]);
        return B_FAIL;
    }
    Pending p = b->pending[b->p_head];
    b->p_head = (b->p_head + 1) % PENDING_MAX;
    b->p_count--;

    int code;
    if (!parse_int(f[0], &code) || code < 100 || code > 599 || n < 2 || strcmp(f[1], p.cmd) != 0) {
        fail(b, "unexpected response to %s", p.cmd);
        return B_FAIL;
    }
    int expected = strcmp(p.cmd, CMD_REGISTER) == 0 && code == ST_CONFLICT;
    if (code >= CODE_MIN && !expected)
        stats_error(code);

    if (!p.wait)
        return B_OK;           /* fire and forget (a DECLINE): nobody waits for it */
    out->code = code;
    snprintf(out->arg, sizeof out->arg, "%s", n > 2 ? f[2] : "");
    return B_REPLY;
}

/* GAME_START|gid|size|your_symbol|opponent|opp_elo|sec_per_move */
static int on_game_start(Bot *b, char **f, int n)
{
    int gid, size;
    if (n < 4 || !parse_int(f[1], &gid) || !parse_int(f[2], &size)
        || size < WIN_LENGTH || size > BOARD_SIZE_MAX
        || (strcmp(f[3], SYM_X) != 0 && strcmp(f[3], SYM_O) != 0)) {
        fail(b, "malformed GAME_START");
        return B_FAIL;
    }
    memset(&b->g, 0, sizeof b->g);
    b->g.active = 1;
    b->g.gid = gid;
    b->g.size = size;
    b->g.mine = f[3][0];
    b->g.theirs = b->g.mine == 'X' ? 'O' : 'X';
    b->g.my_turn = b->g.mine == 'X';            /* X moves first */
    b->out_open = b->in_open = 0;               /* the server drops our other challenges */
    return B_OK;
}

/* 1 if the mark at (r, c) is part of five or more in a row (WIN_LENGTH). */
static int makes_five(const BotGame *g, int r, int c)
{
    static const int dir[4][2] = { { 0, 1 }, { 1, 0 }, { 1, 1 }, { 1, -1 } };
    char s = g->cell[r][c];
    for (int d = 0; d < 4; d++) {
        int count = 1;
        for (int sign = -1; sign <= 1; sign += 2) {
            int rr = r + sign * dir[d][0], cc = c + sign * dir[d][1];
            while (rr >= 0 && rr < g->size && cc >= 0 && cc < g->size && g->cell[rr][cc] == s) {
                count++;
                rr += sign * dir[d][0];
                cc += sign * dir[d][1];
            }
        }
        if (count >= WIN_LENGTH)
            return 1;
    }
    return 0;
}

/* OPP_MOVE|gid|row|col|move_no: the opponent's move, already validated by the server.
 * If it made five, the game is over and GAME_END is on its way: the server sends it with a
 * separate send() after OPP_MOVE, so this thread may wake up before it arrives. Moving then
 * would only get 433 game_not_active, so the bot waits for GAME_END instead. */
static void on_opp_move(Bot *b, char **f, int n)
{
    int gid, r, c;
    if (n < 4 || !b->g.active || b->g.over || !parse_int(f[1], &gid) || gid != b->g.gid
        || !parse_int(f[2], &r) || !parse_int(f[3], &c)
        || r < 0 || r >= b->g.size || c < 0 || c >= b->g.size)
        return;
    b->g.cell[r][c] = b->g.theirs;
    b->g.my_turn = !makes_five(&b->g, r, c);
}

/* GAME_END|gid|result|reason|new_elo|elo_delta[|r,c x 5] */
static void on_game_end(Bot *b, char **f, int n)
{
    int gid;
    if (n < 2 || !b->g.active || b->g.over || !parse_int(f[1], &gid) || gid != b->g.gid)
        return;
    b->g.over = 1;
    b->g.my_turn = 0;
    b->games++;
    if (b->g.mine == 'X')
        stats_add(0, 1, 0);    /* both players get GAME_END: only X counts the game */
}

/* CHALLENGE_FROM|cid|from|from_elo|size|sec: bot 2i+1 keeps its partner's challenge (accepted by
 * pair_acceptor()); anything else is declined at once, without waiting for the answer. */
static int on_challenge_from(Bot *b, char **f, int n)
{
    int cid;
    if (n < 3 || !parse_int(f[1], &cid))
        return B_OK;
    if (!g_match && b->id % 2 == 1 && strcmp(f[2], b->partner) == 0 && !b->g.active) {
        b->in_open = 1;
        b->in_cid = cid;
        return B_OK;
    }
    char payload[64];
    snprintf(payload, sizeof payload, "%s|%d|%s", CMD_CHALLENGE_REPLY, cid, ARG_DECLINE);
    return send_request(b, payload, 0) < 0 ? B_FAIL : B_OK;
}

/* CHALLENGE_DECLINED / _EXPIRED / _CANCELLED|cid: that challenge is gone. */
static void on_challenge_closed(Bot *b, char **f, int n)
{
    int cid;
    if (n < 2 || !parse_int(f[1], &cid))
        return;
    if (b->out_open && cid == b->out_cid)
        b->out_open = 0;
    if (b->in_open && cid == b->in_cid)
        b->in_open = 0;
}

static int on_event(Bot *b, char **f, int n)
{
    const char *ev = f[0];
    if (strcmp(ev, EV_GAME_START) == 0)
        return on_game_start(b, f, n);
    if (strcmp(ev, EV_OPP_MOVE) == 0)
        on_opp_move(b, f, n);
    else if (strcmp(ev, EV_GAME_END) == 0)
        on_game_end(b, f, n);
    else if (strcmp(ev, EV_CHALLENGE_FROM) == 0)
        return on_challenge_from(b, f, n);
    else if (strcmp(ev, EV_CHALLENGE_DECLINED) == 0 || strcmp(ev, EV_CHALLENGE_EXPIRED) == 0
             || strcmp(ev, EV_CHALLENGE_CANCELLED) == 0)
        on_challenge_closed(b, f, n);
    /* PLAYER_STATUS, OPP_LEFT, DRAW_OFFERED, REMATCH_REQUESTED, ...: a bot does not need them */
    return B_OK;
}

/* Reads and handles ONE message: B_REPLY (the awaited response is in *out), B_OK (an event or
 * a fire-and-forget response), B_STOP or B_FAIL. */
static int pump(Bot *b, Reply *out)
{
    char msg[FRAME_MAX_PAYLOAD + 1];
    int r = read_message(b, msg, sizeof msg);
    if (r != B_OK)
        return r;
    char *f[BOT_FIELDS];
    int n = msg_split(msg, f, BOT_FIELDS);
    if (f[0][0] >= '0' && f[0][0] <= '9')       /* a number first: a response (protocol §3) */
        return on_reply(b, f, n, out);
    return on_event(b, f, n);
}

/* Handles events until the awaited response arrives: B_REPLY, B_STOP or B_FAIL. */
static int wait_reply(Bot *b, Reply *out)
{
    for (;;) {
        int r = pump(b, out);
        if (r != B_OK)
            return r;
    }
}

static int request(Bot *b, Reply *out, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int request(Bot *b, Reply *out, const char *fmt, ...)
{
    char payload[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof payload, fmt, ap);
    va_end(ap);
    if (send_request(b, payload, 1) < 0)
        return B_FAIL;
    return wait_reply(b, out);
}

/* Handles events (and answers nothing) until stopped. */
static int idle_until_stop(Bot *b)
{
    Reply rep;
    for (;;) {
        int r = pump(b, &rep);
        if (r == B_STOP || r == B_FAIL)
            return r;
    }
}

/* The server is at an earlier stage and answers 400 unknown_command: stay logged in. */
static int unsupported(Bot *b, const char *cmd, const char *stage)
{
    fail(b, "server has no %s yet (%s)", cmd, stage);
    if (atomic_exchange(&g_warned, 1) == 0)
        printf("bot: the server answers 400 unknown_command to %s (needs %s): "
               "the bots stay logged in until stopped\n", cmd, stage);
    return idle_until_stop(b);
}

/* ---- one bot's flow ------------------------------------------------------------------------- */

static int login(Bot *b)
{
    Reply rep;
    int r = request(b, &rep, "%s|%s|%s", CMD_REGISTER, b->name, BOT_PASSWORD);
    if (r != B_REPLY)
        return r;
    if (rep.code != ST_CREATED && rep.code != ST_CONFLICT) {   /* 409: registered earlier */
        fail(b, "REGISTER: %d %s", rep.code, rep.arg);
        return B_FAIL;
    }
    r = request(b, &rep, "%s|%s|%s", CMD_LOGIN, b->name, BOT_PASSWORD);
    if (r != B_REPLY)
        return r;
    if (rep.code != ST_OK) {
        fail(b, "LOGIN: %d %s", rep.code, rep.arg);
        return B_FAIL;
    }
    b->logged_in = 1;
    stats_add(+1, 0, 0);
    return B_OK;
}

static int partner_not_ready(const Reply *rep)
{
    return (rep->code == ST_NOT_FOUND && strcmp(rep->arg, ERR_USER_NOT_FOUND) == 0)
        || (rep->code == ST_CONFLICT && strcmp(rep->arg, ERR_PLAYER_BUSY) == 0);
}

static int is_unknown_command(const Reply *rep)
{
    return rep->code == ST_BAD_REQUEST && strcmp(rep->arg, ERR_UNKNOWN_COMMAND) == 0;
}

/* Bot 2i: challenge bot 2i+1 until a game starts. */
static int pair_challenger(Bot *b)
{
    Reply rep;
    while (!b->g.active) {
        if (atomic_load(&g_stop))
            return B_STOP;
        int r = request(b, &rep, "%s|%s|%d|%d", CMD_CHALLENGE, b->partner,
                        BOARD_SIZE, SEC_PER_MOVE_NORMAL);
        if (r != B_REPLY)
            return r;

        if (rep.code == ST_OK) {
            if (!parse_int(rep.arg, &b->out_cid)) {
                fail(b, "200|CHALLENGE without a cid");
                return B_FAIL;
            }
            b->out_open = 1;
            /* The answer is an event: GAME_START (accepted) or DECLINED / EXPIRED / CANCELLED. */
            while (b->out_open && !b->g.active) {
                r = pump(b, &rep);
                if (r == B_STOP || r == B_FAIL)
                    return r;
            }
            if (!b->g.active)
                sleep_ms(RETRY_MS);
        } else if (partner_not_ready(&rep)) {
            sleep_ms(RETRY_MS);            /* not logged in yet, or still in the last game */
        } else if (is_unknown_command(&rep)) {
            return unsupported(b, CMD_CHALLENGE, "S2");
        } else {
            sleep_ms(ERROR_RETRY_MS);      /* already counted in the stats; do not spin */
        }
    }
    return B_OK;
}

/* Bot 2i+1: wait for the partner's CHALLENGE_FROM and accept it. */
static int pair_acceptor(Bot *b)
{
    Reply rep;
    while (!b->g.active) {
        if (atomic_load(&g_stop))
            return B_STOP;
        int r;
        if (b->in_open) {
            b->in_open = 0;
            r = request(b, &rep, "%s|%d|%s", CMD_CHALLENGE_REPLY, b->in_cid, ARG_ACCEPT);
            /* 200: GAME_START follows (response first, then events, protocol §3).
             * 404 challenge_not_found / 409 player_busy: wait for the partner's next challenge. */
            if (r != B_REPLY)
                return r;
        } else {
            r = pump(b, &rep);
            if (r == B_STOP || r == B_FAIL)
                return r;
        }
    }
    return B_OK;
}

/* --match: FIND_MATCH, then wait in the queue for GAME_START. */
static int pair_by_queue(Bot *b)
{
    Reply rep;
    while (!b->g.active) {
        if (atomic_load(&g_stop))
            return B_STOP;
        int r = request(b, &rep, "%s", CMD_FIND_MATCH);
        if (r != B_REPLY)
            return r;
        if (rep.code == ST_OK) {
            while (!b->g.active) {
                r = pump(b, &rep);
                if (r == B_STOP || r == B_FAIL)
                    return r;
            }
        } else if (is_unknown_command(&rep)) {
            return unsupported(b, CMD_FIND_MATCH, "S7");
        } else {
            sleep_ms(ERROR_RETRY_MS);
        }
    }
    return B_OK;
}

/* A random empty cell of our copy of the board. rand_r() keeps its state in b->seed; rand()
 * keeps one hidden state for the whole process, shared by every thread (not thread-safe). */
static int pick_cell(Bot *b, int *row, int *col)
{
    int empty = 0;
    for (int r = 0; r < b->g.size; r++)
        for (int c = 0; c < b->g.size; c++)
            empty += b->g.cell[r][c] == 0;
    if (empty == 0)
        return 0;
    int k = rand_r(&b->seed) % empty;
    for (int r = 0; r < b->g.size; r++)
        for (int c = 0; c < b->g.size; c++)
            if (b->g.cell[r][c] == 0 && k-- == 0) {
                *row = r;
                *col = c;
                return 1;
            }
    return 0;
}

/* Latency samples are private to the thread: no lock. */
static void lat_push(Bot *b, double ms)
{
    if (b->lat_n == b->lat_cap) {
        size_t cap = b->lat_cap ? 2 * b->lat_cap : 1024;
        double *p = realloc(b->lat, cap * sizeof *p);
        if (p == NULL) {
            b->lat_dropped++;
            return;
        }
        b->lat = p;
        b->lat_cap = cap;
    }
    b->lat[b->lat_n++] = ms;
}

static int play_game(Bot *b)
{
    Reply rep;
    while (!b->g.over) {
        if (atomic_load(&g_stop))
            return B_STOP;
        int r, row, col;
        if (!b->g.my_turn || !pick_cell(b, &row, &col)) {
            r = pump(b, &rep);                     /* OPP_MOVE, GAME_END, ... */
            if (r == B_STOP || r == B_FAIL)
                return r;
            continue;
        }

        char payload[64];
        snprintf(payload, sizeof payload, "%s|%d|%d|%d", CMD_MOVE, b->g.gid, row, col);
        double t0 = now_ms();
        if (send_request(b, payload, 1) < 0)
            return B_FAIL;
        r = wait_reply(b, &rep);
        if (r != B_REPLY)
            return r;
        double t1 = now_ms();

        b->g.my_turn = 0;
        if (rep.code == ST_OK) {
            /* the mark goes on our board only now that the server has accepted it */
            b->g.cell[row][col] = b->g.mine;
            lat_push(b, t1 - t0);
            b->moves++;
            stats_add(0, 0, 1);
        } else if (rep.code == ST_CELL_OCCUPIED) {
            b->g.cell[row][col] = '?';             /* our copy was wrong: never pick it again */
            b->g.my_turn = 1;
        } else if (rep.code == ST_FORBIDDEN) {
            b->g.over = 1;                         /* 403 not_in_game: this game is gone */
        }
        /* 430 / 433: not our turn or the game ended; OPP_MOVE or GAME_END will tell. */
    }
    return B_OK;
}

static int leave_game(Bot *b)
{
    Reply rep;
    int r = request(b, &rep, "%s|%d", CMD_LEAVE_GAME, b->g.gid);
    if (r != B_REPLY)
        return r;
    b->g.active = 0;           /* 200, or 403 not_in_game: we are in the lobby either way */
    return B_OK;
}

static int bot_run(Bot *b)
{
    char err[96];
    b->fd = connect_to(g_host, g_port, err, sizeof err);
    if (b->fd < 0) {
        fail(b, "%s", err);
        return B_FAIL;
    }
    if (set_timeouts(b->fd) < 0) {
        char buf[64];
        fail(b, "setsockopt: %s", errstr(errno, buf, sizeof buf));
        return B_FAIL;
    }

    int r = login(b);
    while (r == B_OK) {
        if (g_match)
            r = pair_by_queue(b);
        else if (b->id % 2 == 0)
            r = pair_challenger(b);
        else
            r = pair_acceptor(b);
        if (r == B_OK)
            r = play_game(b);
        if (r == B_OK)
            r = leave_game(b);
    }
    return r;
}

/* The thread's start function. arg = &bots[i], which lives in main's array until the join. */
static void *bot_thread(void *arg)
{
    Bot *b = arg;
    if (bot_run(b) == B_FAIL && !atomic_load(&g_stop))
        fprintf(stderr, "bot: %s: %s\n", b->name, b->end);
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
    if (b->logged_in)
        stats_add(-1, 0, 0);
    atomic_fetch_sub(&g_running, 1);
    return NULL;
}

static void bot_init(Bot *b, int id, unsigned seed_base)
{
    memset(b, 0, sizeof *b);
    b->id = id;
    snprintf(b->name, sizeof b->name, "bot_%d", id);
    snprintf(b->partner, sizeof b->partner, "bot_%d", id ^ 1);    /* 0-1, 2-3, 4-5, ... */
    b->seed = seed_base + (unsigned)id * 2654435761u;             /* a different seed each */
    b->fd = -1;
    frame_init(&b->in);
    snprintf(b->end, sizeof b->end, "stopped");
}

/* ---- main thread: idle connections, signals, reports ---------------------------------------- */

/* --idle: K connections that send 2 of the 4 LENGTH bytes of a header, then nothing. */
static int open_idle(int k, int *fds)
{
    static const char half_header[2] = { 0, 0 };
    int opened = 0;
    for (int i = 0; i < k; i++) {
        char err[96];
        int fd = connect_to(g_host, g_port, err, sizeof err);
        if (fd < 0) {
            fprintf(stderr, "bot: idle connection %d: %s\n", i + 1, err);
            break;
        }
        ssize_t sent = send(fd, half_header, sizeof half_header, MSG_NOSIGNAL);
        if (sent != (ssize_t)sizeof half_header) {
            perror("bot: idle connection send");
            close(fd);
            break;
        }
        fds[opened++] = fd;
    }
    return opened;
}

static void format_errors(const Stats *s, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < CODE_SLOTS && len < cap; i++) {
        if (s->errors[i] == 0)
            continue;
        int w = snprintf(out + len, cap - len, "%s%d:%ld", len ? " " : "", CODE_MIN + i,
                         s->errors[i]);
        if (w < 0)
            break;
        len += (size_t)w;
    }
    if (len == 0)
        snprintf(out, cap, "none");
}

static void print_progress(long sec, long *last_sec, long *last_moves)
{
    Stats s = stats_snapshot();        /* copied under the lock, printed outside it */
    char errs[256];
    format_errors(&s, errs, sizeof errs);
    long dt = sec - *last_sec;
    printf("[%4lds] online %d | games %ld | moves %ld (%ld/s) | error responses %s\n",
           sec, s.online, s.games, s.moves, dt > 0 ? (s.moves - *last_moves) / dt : 0L, errs);
    fflush(stdout);
    *last_sec = sec;
    *last_moves = s.moves;
}

/* Waits for SIGINT / SIGTERM, the --secs deadline, or the end of every bot thread. All three
 * signals are blocked in every thread, so they stay pending until sigwait() takes them here. */
static const char *wait_for_end(const sigset_t *sigs, int secs, double t0)
{
    struct itimerval tick = { .it_interval = { .tv_sec = 1 }, .it_value = { .tv_sec = 1 } };
    if (setitimer(ITIMER_REAL, &tick, NULL) != 0) {
        perror("bot: setitimer");
        return "setitimer failed";
    }

    const char *why = NULL;
    long next_report = REPORT_EVERY_S, last_sec = 0, last_moves = 0;
    while (why == NULL) {
        int sig;
        int rc = sigwait(sigs, &sig);    /* returns an error number, does not set errno */
        if (rc != 0) {
            fprintf(stderr, "bot: sigwait: %s\n", strerror(rc));
            why = "sigwait failed";
        } else if (sig == SIGINT) {
            why = "SIGINT (Ctrl+C)";
        } else if (sig == SIGTERM) {
            why = "SIGTERM";
        } else {                         /* SIGALRM: one more second */
            long sec = (long)((now_ms() - t0) / 1000.0 + 0.5);
            if (sec >= next_report) {
                print_progress(sec, &last_sec, &last_moves);
                next_report = sec - sec % REPORT_EVERY_S + REPORT_EVERY_S;
            }
            if (secs > 0 && sec >= secs)
                why = "--secs reached";
            else if (atomic_load(&g_running) == 0)
                why = "every bot thread has ended";
        }
    }

    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);
    return why;
}

typedef struct { size_t n; double mean, p95, max; } LatSummary;

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Sorts v in place. P95 by nearest rank: the smallest sample with at least 95 % of the samples
 * at or below it. */
static LatSummary summarize(double *v, size_t n)
{
    LatSummary s = { n, 0.0, 0.0, 0.0 };
    if (n == 0)
        return s;
    qsort(v, n, sizeof *v, cmp_double);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += v[i];
    s.mean = sum / (double)n;
    s.p95 = v[(95 * n + 99) / 100 - 1];
    s.max = v[n - 1];
    return s;
}

static void print_row(const char *name, const char *games, long moves, LatSummary l,
                      const char *end)
{
    const char *sep = end[0] ? "  " : "";
    if (l.n == 0)
        printf("%-10s %6s %8ld %9s %9s %9s%s%s\n", name, games, moves, "-", "-", "-", sep, end);
    else
        printf("%-10s %6s %8ld %9.3f %9.3f %9.3f%s%s\n", name, games, moves, l.mean, l.p95,
               l.max, sep, end);
}

/* Called after every thread has been joined: the bots' private data is safe to read. */
static void final_report(Bot *bots, int created, int n, const char *why, double secs,
                         int n_idle)
{
    printf("\n== stopped: %s, after %.1f s (%d bot threads, %d idle connections)\n",
           why, secs, created, n_idle);
    if (created < n)
        printf("   %d bot threads were never created\n", n - created);

    size_t total = 0;
    long sum_moves = 0, dropped = 0;
    for (int i = 0; i < created; i++) {
        total += bots[i].lat_n;
        sum_moves += bots[i].moves;
        dropped += bots[i].lat_dropped;
    }

    if (created <= TABLE_MAX_ROWS) {
        printf("%-10s %6s %8s %9s %9s %9s  %s\n", "bot", "games", "moves", "mean ms",
               "p95 ms", "max ms", "end");
        for (int i = 0; i < created; i++) {
            char games[24];
            snprintf(games, sizeof games, "%ld", bots[i].games);
            print_row(bots[i].name, games, bots[i].moves,
                      summarize(bots[i].lat, bots[i].lat_n), bots[i].end);
        }
    }

    /* Merge every thread's samples into one array. */
    double *all = total > 0 ? malloc(total * sizeof *all) : NULL;
    LatSummary l = { 0, 0.0, 0.0, 0.0 };
    if (all != NULL) {
        size_t k = 0;
        for (int i = 0; i < created; i++) {
            if (bots[i].lat_n > 0)
                memcpy(all + k, bots[i].lat, bots[i].lat_n * sizeof *all);
            k += bots[i].lat_n;
        }
        l = summarize(all, total);
    } else if (total > 0) {
        fprintf(stderr, "bot: out of memory merging %zu latency samples\n", total);
    }
    if (created <= TABLE_MAX_ROWS)
        print_row("all", "", sum_moves, l, "");

    Stats s = stats_snapshot();
    char errs[256];
    format_errors(&s, errs, sizeof errs);
    printf("shared counters: online %d, games %ld, moves %ld, error responses %s\n",
           s.online, s.games, s.moves, errs);
    printf("check: sum of the per-thread move counters %ld %s shared moves %ld%s\n",
           sum_moves, sum_moves == s.moves ? "==" : "!=", s.moves,
           sum_moves == s.moves ? " (mutex OK)" : " (LOST UPDATES)");
    if (l.n > 0)
        printf("latency MOVE -> 200|MOVE over %zu moves: mean %.3f ms, P95 %.3f ms, max %.3f ms\n",
               l.n, l.mean, l.p95, l.max);
    else
        printf("latency MOVE -> 200|MOVE: no move was played\n");
    if (dropped > 0)
        printf("   (%ld samples not kept: out of memory)\n", dropped);
    free(all);
}

static int parse_range(const char *s, int min, int max, int *out)
{
    int v;
    if (!parse_int(s, &v) || v < min || v > max)
        return 0;
    *out = v;
    return 1;
}

static int parse_args(int argc, char *argv[], Options *o)
{
    memset(o, 0, sizeof *o);
    if (argc < 4)
        return -1;
    o->host = argv[1];
    o->port = argv[2];
    int port;
    if (!parse_range(argv[2], 1, 65535, &port)) {
        fprintf(stderr, "bot: bad port \"%s\"\n", argv[2]);
        return -1;
    }
    if (!parse_range(argv[3], 2, BOT_MAX, &o->n) || o->n % 2 != 0) {
        fprintf(stderr, "bot: N must be an even number from 2 to %d\n", BOT_MAX);
        return -1;
    }
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--match") == 0) {
            o->match = 1;
        } else if (strcmp(argv[i], "--secs") == 0 && i + 1 < argc) {
            if (!parse_range(argv[++i], 1, 86400, &o->secs)) {
                fprintf(stderr, "bot: --secs takes 1..86400\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--idle") == 0 && i + 1 < argc) {
            if (!parse_range(argv[++i], 0, IDLE_MAX, &o->idle)) {
                fprintf(stderr, "bot: --idle takes 0..%d\n", IDLE_MAX);
                return -1;
            }
        } else {
            fprintf(stderr, "bot: unknown or incomplete option \"%s\"\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    Options o;
    if (parse_args(argc, argv, &o) != 0) {
        fprintf(stderr, "usage: %s <host> <port> <N> [--secs S] [--idle K] [--match]\n", argv[0]);
        return 2;
    }
    g_host = o.host;
    g_port = o.port;
    g_match = o.match;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("bot: signal");
        return 1;
    }

    /* sigwait() can only take a signal that is not ignored: a signal whose action is SIG_IGN is
     * thrown away as soon as it is generated, even while blocked. A shell that starts us in the
     * background without job control (`./build/bot ... &` in a script) sets SIGINT to SIG_IGN,
     * so the default action is restored. It never runs: the signals stay blocked. */
    if (signal(SIGINT, SIG_DFL) == SIG_ERR || signal(SIGTERM, SIG_DFL) == SIG_ERR
        || signal(SIGALRM, SIG_DFL) == SIG_ERR) {
        perror("bot: signal");
        return 1;
    }

    /* Block the signals BEFORE the first pthread_create(): every bot thread inherits the mask,
     * so only main's sigwait() ever receives them. pthread_* functions return an error number
     * instead of setting errno, hence strerror(rc) and not perror(). */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGALRM);
    int rc = pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    if (rc != 0) {
        fprintf(stderr, "bot: pthread_sigmask: %s\n", strerror(rc));
        return 1;
    }

    Bot *bots = calloc((size_t)o.n, sizeof *bots);       /* lives until every join is done */
    pthread_t *tids = calloc((size_t)o.n, sizeof *tids);
    int *idle = calloc((size_t)(o.idle > 0 ? o.idle : 1), sizeof *idle);
    if (bots == NULL || tids == NULL || idle == NULL) {
        fprintf(stderr, "bot: out of memory\n");
        free(bots);
        free(tids);
        free(idle);
        return 1;
    }

    char duration[32];
    if (o.secs > 0)
        snprintf(duration, sizeof duration, "for %d s", o.secs);
    else
        snprintf(duration, sizeof duration, "until stopped");
    printf("bot: %d bots (%d pair%s, paired by %s) -> %s:%s, %s. Ctrl+C stops.\n", o.n,
           o.n / 2, o.n == 2 ? "" : "s", o.match ? CMD_FIND_MATCH : CMD_CHALLENGE, o.host,
           o.port, duration);

    int n_idle = open_idle(o.idle, idle);
    if (o.idle > 0)
        printf("bot: %d/%d idle connections open (2 header bytes each, then silence)\n",
               n_idle, o.idle);

    unsigned seed_base = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
    for (int i = 0; i < o.n; i++)
        bot_init(&bots[i], i, seed_base);

    int created = 0;
    for (int i = 0; i < o.n; i++) {
        atomic_fetch_add(&g_running, 1);                  /* before: the thread may end at once */
        rc = pthread_create(&tids[i], NULL, bot_thread, &bots[i]);
        if (rc != 0) {
            atomic_fetch_sub(&g_running, 1);
            fprintf(stderr, "bot: pthread_create(%s): %s\n", bots[i].name, strerror(rc));
            break;
        }
        created++;
    }

    double t0 = now_ms();
    const char *why = created == o.n ? wait_for_end(&sigs, o.secs, t0)
                                     : "pthread_create failed";

    /* Stop: one flag for every thread, then wait for each of them. */
    atomic_store(&g_stop, 1);
    int join_errors = 0;
    for (int i = 0; i < created; i++) {
        rc = pthread_join(tids[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "bot: pthread_join(%s): %s\n", bots[i].name, strerror(rc));
            join_errors++;
        }
    }
    if (join_errors > 0)
        return 1;      /* a thread may still use bots[]: do not read or free it */

    final_report(bots, created, o.n, why, (now_ms() - t0) / 1000.0, n_idle);

    for (int i = 0; i < n_idle; i++)
        close(idle[i]);
    for (int i = 0; i < o.n; i++)
        free(bots[i].lat);
    free(idle);
    free(tids);
    free(bots);
    return created == o.n ? 0 : 1;
}
