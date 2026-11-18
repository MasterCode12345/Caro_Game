# Progress

Stage checklist from the Roadmap in `CLAUDE.md`. Update it at the end of every session: tick a
stage only when `make` has zero warnings, `make test` passes and the manual checks are done,
then add one line to the log at the bottom.

Checkpoint 2 ≈ 01/12/2026 · code freeze 29/11 evening.

## Client — Nghĩa (`client/`, `tools/`, `common/`)

- [x] **C0** (17–18/11) `common/` framing, `test_frame`, `cli`, GTK window
- [ ] **C1** (19–20/11) net layer, dispatch, fake mode, login page
- [ ] **C2** (21–22/11) lobby page, challenge dialogs
- [ ] **C3** (23–24/11) board view, moves, result
- [ ] **C4** (25–26/11) game controls, disconnect handling
- [ ] **C5** (27–28/11) history, replay, leaderboard, quick match, countdown
- [ ] **C6** (29/11) bot, LAN test, polish

## Server — Hân (`server/`)

- [ ] **S0** (17–18/11) select loop, framing, PING, log
- [ ] **S1** (19–20/11) SQLite, REGISTER/LOGIN/LOGOUT, sessions
- [ ] **S2** (21–22/11) lobby, challenges, expiry
- [ ] **S3** (23–24/11) game, MOVE validation, win/draw, GAME_END
- [ ] **S4** (25–26/11) resign/draw/rematch/leave/disconnect
- [ ] **S5** (25–26/11) DB + ELO
- [ ] **S6** (27–28/11) history/replay/leaderboard
- [ ] **S7** (27–28/11) matchmaking, timers
- [ ] **S8** (29/11) valgrind, malformed input, load

## Integration milestones — both, evening

- [ ] **M0** (18/11) 3 `cli` PING concurrently; split/glued frames OK
- [ ] **M1** (20/11) register/login from GTK; 401, 409 shown
- [ ] **M2** (22/11) two machines see each other, challenge → GAME_START
- [ ] **M3** (24/11) full game between two machines; bad moves rejected
- [ ] **M4** (26/11) every ending correct, ELO persists after restart
- [ ] **M5** (28/11) all graded features work
- [ ] **Code freeze** (29/11) after the full test table passes

## Log

- 2026-11-18 · Nghĩa · P0 repository skeleton: Makefile, `common/proto.h`, `common/frame.h`
  (`frame.c` stubbed for C0), stub files with TODOs in `server/`, `client/`, `tools/`;
  `caro_client` opens an empty 900×700 GTK window.
- 2026-11-18 · Nghĩa · C0: `common/frame.c` implemented (length prefix, partial-send loop,
  EINTR, MSG_NOSIGNAL); `tools/test_frame` 6 tests (byte-by-byte, 3 glued, LENGTH 0/16385/16384,
  msg_split, frame_send checks, max-size frame), clean under ASan/UBSan; `tools/cli` with
  `/bytes`, `/burst`, `/zero`, `/huge`, `/quit`, checked against a throwaway framing server.
  M0 waits for S0 (real server).
- 2026-11-18 · Nghĩa · C1 code: `net.c` (getaddrinfo + non-blocking connect with 3 s timeout,
  g_io_add_watch, FIFO via `dispatch_expect`, lost-connection handling), `dispatch.c` (response
  FIFO + COMMAND check, event table, CARO_DEBUG trace with masked passwords), `fake.c` (PING,
  REGISTER, LOGIN, LOGOUT; time-ordered outbox), 4-page GtkStack, login page, placeholder lobby
  with Logout. Net/dispatch/fake checked headless under ASan (16 checks, real socket + fake).
  C1 is ticked after the GUI click-through in fake mode; M1 waits for S1.
