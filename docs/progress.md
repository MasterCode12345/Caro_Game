# Progress

Stage checklist from the Roadmap in `CLAUDE.md`. Update it at the end of every session: tick a
stage only when `make` has zero warnings, `make test` passes and the manual checks are done,
then add one line to the log at the bottom.

Checkpoint 2 ≈ 01/12/2026 · code freeze 29/11 evening.

## Client — Nghĩa (`client/`, `tools/`, `common/`)

- [x] **C0** (17–18/11) `common/` framing, `test_frame`, `cli`, GTK window
- [x] **C1** (19–20/11) net layer, dispatch, fake mode, login page
- [x] **C2** (21–22/11) lobby page, challenge dialogs
- [x] **C3** (23–24/11) board view, moves, result
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
  GUI click-through in fake mode passed (format errors, 409/401 messages, button lock,
  login → lobby → logout); C1 ticked. M1 waits for S1.
- 2026-11-18 · Nghĩa · C2 code: lobby page (top bar + logout, sorted player list from one
  LIST_PLAYERS, then PLAYER_STATUS add/update/remove), outgoing challenge with waiting/cancel
  dialog, incoming invitations one dialog per cid, DECLINED/EXPIRED/CANCELLED, GAME_START ->
  app.game + game page (info only). Fake: 4 scripted players, timed status changes, Ctrl+I /
  Ctrl+U invitations. Lobby driven headless against fake under ASan (34 checks, incl. 30 s
  expiry and lost connection with dialogs open). GUI click-through in fake mode passed;
  C2 ticked. The 2-window test with the real server waits for S2.
- 2026-11-18 · Nghĩa · C3 code: `board_view` (cairo board that scales with the window: grid,
  X/O, last-move outline, green winning cells, hover, click -> row/col), game page (players +
  ELO, turn, move count; mark drawn only on 200|MOVE; 430–433 messages; OPP_MOVE; GAME_END ->
  locked board, winning line, result dialog with Vietnamese reason and ELO ±delta, app ELO and
  W/L/D updated). Fake: MOVE checked in §6.3 order, greedy opponent 0.5 s later, five / full
  board -> GAME_END. Headless under ASan (27 checks: client board == fake board after every
  step, a won and a lost game, forced 430, RESIGN/BOARD_FULL texts, lost connection while a
  move is pending) + rendered PNGs checked. GUI click-through in fake mode passed (win, loss,
  win on the board edge); C3 ticked. The 2-window test with the real server waits for S3.
