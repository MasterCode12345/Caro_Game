# Caro Online — Group 13 (Network Programming IT4062E, SoICT HUST)

Turn-based online Caro (Gomoku) game. 15×15 board, X moves first, five or more in a row
(horizontal, vertical, diagonal) wins, a full board is a draw. Client–server over TCP with our
own application-layer protocol. Read `docs/protocol.md` before touching any network code: it is
the contract between client and server.

## Hard rules from the course — never break these

1. The server is written in C and runs on Linux.
2. The protocol is ours, built directly on POSIX TCP sockets (`socket`, `bind`, `listen`,
   `accept`, `connect`, `send`, `recv`, `select`). Forbidden: WebSocket, Socket.IO, HTTP/REST
   frameworks, gRPC, ZeroMQ, libevent/libuv, GIO networking classes (`GSocketClient`,
   `GSocketConnection`, `GSocketService`), or any library that does networking or messaging for us.
3. No "proxy server" design: the C server is the real server, not a relay for something else.
4. GTK/GLib on the client are only for the GUI and for watching the socket's readiness
   (`g_io_add_watch`). SQLite is storage only, on the server only; the client never opens the DB.

## Team and ownership

| Folder | Owner | Notes |
| --- | --- | --- |
| `client/` | Nghĩa | GTK 3 client |
| `tools/` | Nghĩa | `cli`, `bot`, `test_frame` |
| `common/` | Nghĩa (writes), both (use) | changes must be agreed by both |
| `server/` | Hân | C server, its unit tests live in `server/tests/` |
| `docs/protocol.md` | both | change the doc first, then the code |

- A Claude Code session working for one owner must NOT edit the other owner's folder.
  A bug found on the other side goes into `docs/issues.md` (date, who found it, steps, expected, actual).
- Any change to `common/` or `docs/protocol.md` uses commit prefix `protocol:` and must be told to the other member.

## Repository layout

```text
Caro_Game/
├── CLAUDE.md                 # this file
├── Makefile                  # make | make server | make client | make tools | make test | make debug | make clean
├── README.md
├── common/
│   ├── proto.h               # command names, status codes, limits, states (shared)
│   ├── frame.h / frame.c     # 4-byte length framing + field splitting (shared)
├── server/                   # Hân
│   ├── main.c                # argv, listening socket, select() loop, tick()
│   ├── server.h              # Client / Challenge / Game structs, shared tables
│   ├── net.c/.h              # accept, per-client receive, reply(), session_end()
│   ├── protocol.c/.h         # command table, state checks, dispatch to handlers
│   ├── auth.c/.h             # REGISTER, LOGIN, LOGOUT
│   ├── lobby.c/.h            # LIST_PLAYERS, PLAYER_STATUS, challenges, matchmaking
│   ├── game.c/.h             # board, move validation, win/draw (pure: no sockets, no DB)
│   ├── match.c/.h            # MOVE/RESIGN/DRAW/REMATCH/LEAVE handlers, game_end()
│   ├── elo.c/.h              # elo_delta() (pure)
│   ├── db.c/.h               # SQLite: users, matches, moves
│   ├── sha256.c/.h           # bundled SHA-256 (no OpenSSL dependency)
│   ├── log.c/.h              # server.log
│   └── tests/                # test_game.c, test_elo.c
├── client/                   # Nghĩa
│   ├── main.c                # gtk init, window, GtkStack with 4 pages
│   ├── app.h                 # AppState (single global, owned by main.c)
│   ├── net.c/.h              # connect, g_io_add_watch, net_request(), disconnect handling
│   ├── dispatch.c/.h         # response FIFO matching + event table
│   ├── fake.c/.h             # offline mode (CARO_FAKE=1): scripted server for UI work
│   ├── board_view.c/.h       # cairo board drawing, shared by game and replay
│   ├── ui_login.c            # login / register page
│   ├── ui_lobby.c            # lobby page: players, challenges, quick match, leaderboard
│   ├── ui_game.c             # game page: board, turn, controls, result
│   └── ui_history.c          # history list + replay viewer
├── tools/                    # Nghĩa
│   ├── cli.c                 # interactive line client (manual protocol testing)
│   ├── bot.c                 # N bots in one process, one pthread each (load testing)
│   └── test_frame.c          # framing unit tests over socketpair()
└── docs/
    ├── protocol.md           # THE protocol
    ├── progress.md           # stage checklist, updated at the end of every session
    └── issues.md             # cross-team bug reports
```

## Build and run

```bash
make                      # everything into build/
./build/caro_server 5500  # server (default port 5500)
./build/caro_client       # GTK client
./build/cli 127.0.0.1 5500
./build/bot 127.0.0.1 5500 20 --secs 60 --idle 50   # load test: 20 bot threads + 50 slow clients
make test                 # test_frame + server unit tests
make debug                # same with -O0 -g -fsanitize=address,undefined
```

Dependencies
- Ubuntu: `sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev gdb valgrind`
- macOS: `xcode-select --install` and `brew install gtk+3 pkg-config` (SQLite ships with macOS)

Compiler flags: `-std=c11 -Wall -Wextra -g`, plus `-D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE` for POSIX
functions. Zero-warning policy for our own code.

## Platform notes

- The server's official target is Linux (demo runs there). `server/` and `common/` must stay
  POSIX-portable so they also build on macOS for local testing: no epoll, no Linux-only flags
  without `#ifdef`. `MSG_NOSIGNAL` does not exist on macOS → `common/frame.c` defines it to 0
  when missing, and every `main()` calls `signal(SIGPIPE, SIG_IGN)`.
- The client is developed on macOS (Homebrew GTK 3) and must also build on Ubuntu.
- No OpenSSL: password hashing = SHA-256(salt_hex + password) using `server/sha256.c`.

## Architecture

### Shared framing (`common/frame.h`) — Lec04 "byte stream problem", solution 3: length prefixing

```c
#define FRAME_MAX_PAYLOAD 16384
typedef struct { char buf[4 + FRAME_MAX_PAYLOAD]; size_t len; } FrameBuf;
void frame_init(FrameBuf *fb);
int  frame_send(int fd, const char *payload);              /* loops on partial send; 0 ok, -1 error */
int  frame_recv(FrameBuf *fb, int fd);                     /* one recv(); >0 bytes, 0 peer closed, -1 error */
int  frame_next(FrameBuf *fb, char *out, size_t out_cap);  /* 1 got message, 0 need more, -1 bad frame */
int  msg_split(char *msg, char *fields[], int max_fields); /* split on '|' in place, returns count */
```

Always drain `frame_next()` until it returns 0 before calling `frame_recv()` again.

### Server (Hân) — Lec05 I/O models: we use I/O multiplexing with `select()`

- One process, one thread, one `select()` loop over the listening socket and every client.
  Why not `fork()` (Lec05): child processes do not share memory, so the lobby, the challenges and
  the board could not be shared without IPC.
- Why not threads (Lec06). One thread per client would cost more than it gives for Caro:
  (a) a game has 2 players, so 2 threads would change the same `Game`: every game needs its own
  mutex, and when two players challenge each other at the same moment, A's thread locks A then B
  while B's thread locks B then A → deadlock;
  (b) a `PLAYER_STATUS` broadcast would hold the online-list lock while it `send()`s to many
  sockets, exactly what the lecture says to avoid (no blocking I/O inside a critical section);
  (c) most clients sit idle (in the lobby, or thinking up to 30 s per move), yet every thread
  still costs a stack;
  (d) a request costs a few µs of CPU, so running requests in parallel gains nothing.
  `tools/bot.c` does use pthreads, to generate load: its bots are independent clients that share
  only a counters struct behind one mutex.
- `select()` timeout = 1 s; after every loop iteration call `tick(now)` for challenge expiry,
  matchmaking and move timers. Re-initialise the `fd_set`s and `struct timeval` every iteration.
  Handle `EINTR` on `select()` and `accept()` (Lec05).
- Only `game.c` changes a board. `game.c` and `elo.c` never call socket or DB functions, so they
  are unit-tested in `server/tests/`.
- Never trust the client: validate every field (`strtol` + end pointer), every state, every id.
- Client sockets get `SO_SNDTIMEO` = 2 s so a client that stops reading cannot freeze the server.
- `session_end(c)` cleans up everything for that client (challenges, queue, game → opponent wins
  by DISCONNECT, PLAYER_STATUS OFFLINE), closes the fd and sets `c->fd = -1`.
- All SQL uses `sqlite3_prepare_v2` + `sqlite3_bind_*`. Never build SQL with user strings.
- `server.log`: one line per request: time, IP:port, username or `-`, command, status. Never log passwords.

### Client (Nghĩa)

- Single thread: everything runs in the GLib main loop. No pthreads.
- `net.c` connects with `getaddrinfo()` + `connect()`, wraps the fd with
  `g_io_channel_unix_new()` and `g_io_add_watch(G_IO_IN | G_IO_HUP | G_IO_ERR)`. The watch
  callback calls `frame_recv()` once, then drains `frame_next()` and passes each message to `dispatch()`.
- `net_request(payload, callback, user_data)` pushes the callback on a FIFO queue, then sends.
  The server answers each client's requests in order, so the head of the queue owns the next
  response. Check that the response's COMMAND field matches the queued request (sanity check).
- Messages whose first field is not a number are events → looked up in an event table
  (`dispatch_on("OPP_MOVE", handler)`).
- UI code never calls `send`/`recv` directly, and never draws a move before the server answers `200|MOVE`.
- Offline mode: `CARO_FAKE=1 ./build/caro_client` swaps the network for `fake.c`, which answers
  requests and pushes scripted events with `g_timeout_add()`, so the UI can be built before the
  server feature exists. Fake mode must go through the same `dispatch()` path as real messages.
- Losing the server at any time: remove the watch, close the fd, clear the pending queue,
  go back to the login page with a message. Never crash, never hang.
- Use `GtkMessageDialog` with the `"response"` signal; do not use `gtk_dialog_run()`.

## Coding conventions

- C11. Small functions. Every syscall's return value is checked. `snprintf`, never `sprintf`/`strcpy`.
- Free what you allocate; ASan (`make debug`) and valgrind (Linux) must be clean.
- Names: `snake_case` functions, `UPPER_CASE` macros, `PascalCase` typedefs.
- Comments may be English or Vietnamese. Explain every non-obvious networking decision in a
  comment: the students must be able to explain this code to the lecturer.

## Roadmap (Checkpoint 2 ≈ 01/12/2026, code freeze 29/11 evening)

| Dates | Server (Hân) | Client (Nghĩa) | Integration milestone (both, evening) |
| --- | --- | --- | --- |
| 17–18/11 | S0 select loop, framing, PING, log | C0 common/, test_frame, cli, GTK window | M0: 3 cli PING concurrently; split/glued frames OK |
| 19–20/11 | S1 SQLite, REGISTER/LOGIN/LOGOUT, sessions | C1 net layer, dispatch, fake mode, login page | M1: register/login from GTK; 401, 409 shown |
| 21–22/11 | S2 lobby, challenges, expiry | C2 lobby page, challenge dialogs | M2: two machines see each other, challenge → GAME_START |
| 23–24/11 | S3 game, MOVE validation, win/draw, GAME_END | C3 board view, moves, result | M3: full game between two machines; bad moves rejected |
| 25–26/11 | S4 resign/draw/rematch/leave/disconnect, S5 DB + ELO | C4 game controls, disconnect handling | M4: every ending correct, ELO persists after restart |
| 27–28/11 | S6 history/replay/leaderboard, S7 matchmaking, timers | C5 history, replay, leaderboard, quick match, countdown | M5: all graded features work |
| 29/11 | S8 valgrind, malformed input, load | C6 bot, LAN test, polish | Code freeze after the full test table passes |

## Workflow for every Claude Code session

1. Read this file, `docs/protocol.md` and `docs/progress.md` first.
2. Work on one stage only, in the owner's folders only.
3. If the protocol looks wrong or incomplete: STOP and ask. Never silently diverge from `docs/protocol.md`.
4. Before saying "done": `make` with zero warnings, `make test` passes, and give the user a short
   list of manual checks to run.
5. Update `docs/progress.md`, then commit with prefix `client:`, `server:`, `common:`, `protocol:` or
   `docs:`. Do not push unless the user asks.
6. End with a short explanation in Vietnamese of how the new code works and 2–3 questions the
   lecturer might ask about it, with answers.
