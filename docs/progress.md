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
- [x] **C4** (25–26/11) game controls, disconnect handling
- [ ] **C5** (27–28/11) history, replay, leaderboard, quick match, countdown
- [ ] **C6** (29/11) bot, LAN test, polish

## Server — Hân (`server/`)

- [x] **S0** (17–18/11) select loop, framing, PING, log
- [x] **S1** (19–20/11) SQLite, REGISTER/LOGIN/LOGOUT, sessions
- [ ] **S2** (21–22/11) lobby, challenges, expiry
- [ ] **S3** (23–24/11) game, MOVE validation, win/draw, GAME_END
- [ ] **S4** (25–26/11) resign/draw/rematch/leave/disconnect
- [ ] **S5** (25–26/11) DB + ELO
- [ ] **S6** (27–28/11) history/replay/leaderboard
- [ ] **S7** (27–28/11) matchmaking, timers
- [ ] **S8** (29/11) valgrind, malformed input, load

## Integration milestones — both, evening

- [x] **M0** (18/11) 3 `cli` PING concurrently; split/glued frames OK
- [x] **M1** (20/11) register/login from GTK; 401, 409 shown
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
- 2026-11-21 · Hân · S-pre code: implemented pure Caro board logic (`server/game.h/.c`,
  MAX_BOARD 19, board initialization, range/empty checks, move placement, win detection
  in four directions, winning-line coordinates, full-board detection); implemented pure
  ELO calculation (`server/elo.h/.c`, expected score and rating update, K=32).
  Added `test_game` (12 tests) and `test_elo` (10 tests); both test suites passed.
  S-pre pending clean `make` and manual checks before ticking.
- 2026-11-21 · Hân · S0: implemented select()-based server loop, socket accept,
  framing and PING handling; malformed frames (zero-length/oversize) close the
  offending client; logging for connect/disconnect and command responses.
  Manual CLI checks: PING, ABC (unknown command), /bytes, /burst (3 glued
  requests), /zero, /huge, reconnect after malformed frames, concurrent clients
  and disconnect. `make` clean (zero warnings); `make test` passed
  (test_frame 6/6, test_game 12/12, test_elo 10/10).
- 2026-11-21 · Hân · S1: implemented SQLite user database (`db.c/.h`), bundled SHA-256
  password hashing with random salt, REGISTER/LOGIN/LOGOUT handlers, authentication state
  validation and online-user checks; integrated login/logout `PLAYER_STATUS` broadcasts
  and disconnect cleanup. Manual CLI checks passed: successful registration/login/logout,
  re-login after logout, duplicate username, invalid password, invalid credentials and
  forbidden-state requests. `make` clean (zero warnings).
- 2026-11-21 · Nghĩa + Hân · M0 and M1 reached with Hân's real server: PING, REGISTER, LOGIN,
  wrong password -> 401 and duplicate login -> 409 shown, tested with two GTK client windows.
- 2026-11-21 · Nghĩa · C4 code: game page buttons Xin thua (confirm -> RESIGN), Cầu hòa
  (DRAW_OFFER; SENDING/PENDING so a move cancels only an offer the server already holds), Đấu lại
  (only after GAME_END -> REMATCH), Về sảnh (LEAVE_GAME, confirm while the game runs);
  DRAW_OFFERED / REMATCH_REQUESTED Accept/Decline dialogs, DRAW_DECLINED / REMATCH_DECLINED
  notices, OPP_LEFT (and a DISCONNECT win) disables Đấu lại, rematch GAME_START clears the board
  with swapped symbols; end reason kept in the side panel. After 200|LEAVE_GAME: lobby,
  LIST_PLAYERS, then STATS sets ELO and W/L/D (no client-side counting); the later GAME_END of a
  left game only updates the ELO; events with another gid are ignored. Lost server on any page
  closes every dialog. Fake: RESIGN, DRAW_*, REMATCH_*, LEAVE_GAME (200 before GAME_END), STATS;
  Ctrl+D/R/L/K/T on the game page. Headless under ASan/UBSan: 146 checks (14 fake games: every
  button, every end reason, 409/404 paths, old-gid events, lost connection with dialogs open and
  requests pending on login/lobby/game; a TCP mock server SIGKILLed in the lobby, in a game with
  2 requests pending and with LOGIN pending; LEAVE 200 then GAME_END 0.4 s later) + rendered
  PNGs checked. C4 not ticked yet: waits for the GUI click-through in fake mode. STATS needs the
  server's handler (S6): until then the lobby shows an error notice after a game.
- 2026-11-22 · Nghĩa · C4: GUI click-through in fake mode passed (draw declined then agreed,
  rematch declined then accepted with swapped symbols, Ctrl+D/R dialogs, resign and leave
  confirmations, lobby ELO and W/L/D after leaving, Ctrl+L/K/T end reasons), no ASan report;
  C4 ticked. Lost-server check in a game with the real server waits for S2–S4.
