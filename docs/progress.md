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
- [x] **C5** (27–28/11) history, replay, leaderboard, quick match, countdown
- [x] **C6** (29/11) bot, LAN test, polish

## Server — Hân (`server/`)

- [x] **S0** (17–18/11) select loop, framing, PING, log
- [x] **S1** (19–20/11) SQLite, REGISTER/LOGIN/LOGOUT, sessions
- [x] **S2** (21–22/11) lobby, challenges, expiry
- [x] **S3** (23–24/11) game, MOVE validation, win/draw, GAME_END
- [x] **S4** (25–26/11) resign/draw/rematch/leave/disconnect
- [x] **S5** (25–26/11) DB + ELO
- [x] **S6** (27–28/11) history/replay/leaderboard
- [x] **S7** (27–28/11) matchmaking, timers
- [x] **S8** (29/11) valgrind, malformed input, load

## Integration milestones — both, evening

- [x] **M0** (18/11) 3 `cli` PING concurrently; split/glued frames OK
- [x] **M1** (20/11) register/login from GTK; 401, 409 shown
- [x] **M2** (22/11) two machines see each other, challenge → GAME_START
- [x] **M3** (24/11) full game between two machines; bad moves rejected
- [x] **M4** (26/11) every ending correct, ELO persists after restart
- [x] **M5** (28/11) all graded features work
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
- 2026-11-22 · Nghĩa · C5 code: history page (`ui_history.c`): HISTORY|page table (opponent,
  symbol, Thắng/Thua/Hòa, reason, time), Trang trước off on page 1, Trang sau off under 10 rows;
  one click -> REPLAY|mid -> read-only BoardView with ⏮ ◀ ▶ ⏭ and Tự chạy (g_timeout_add 700 ms,
  removed on every way out, also when GAME_START comes while watching from the queue),
  "Nước k / n", X/O names, result + reason, winning line found again on the last move of a FIVE
  game; responses from an earlier visit dropped. Lobby: Lịch sử, Bảng xếp hạng (LEADERBOARD|10
  dialog, own row bold), Tìm trận nhanh (FIND_MATCH -> "Đang tìm đối thủ…" with seconds waited,
  Hủy -> CANCEL_MATCH; off while our challenge is pending; challenges off in the queue; received
  invitations closed at 200|FIND_MATCH; GAME_START then 403|CANCEL_MATCH -> the 403 is ignored).
  Game page: move countdown from sec_per_move (monotonic deadline, g_timeout_add_seconds 1),
  restarted on 200|MOVE, OPP_MOVE and a rematch, stopped at GAME_END / leave / lost server, red at
  ≤ 5 s. Fake: 12 scripted games (2 pages; mid 112 = 20 moves won by O with FIVE) + every fake
  game played, REPLAY, LEADERBOARD, FIND_MATCH (~3 s, 30 s per move, X and O in turn),
  CANCEL_MATCH, Ctrl+M race hook. Headless under ASan/UBSan: 217 checks (pages, every replay
  button, auto-play timing and removal, late and malformed HISTORY/REPLAY/LEADERBOARD, clock
  values and colour, quick match / cancel / race / blocked cases, queue + history + GAME_START,
  logout and lost server in the queue, in a game, while replaying) + rendered PNGs checked;
  `make debug` binary started clean. C5 not ticked yet: waits for the GUI click-through in fake
  mode. Checks with the real server wait for S6–S7.
- 2026-11-22 · Nghĩa · C5: GUI click-through in fake mode passed (leaderboard with own row in
  bold, history pages, replay buttons and Tự chạy, countdown reset and red at ≤ 5 s, quick match,
  cancel, Ctrl+M race, blocked cases, invitations closed when entering the queue), no ASan
  report; C5 ticked. Still to check with the real server once S7 is done (the fake pairs after
  3 s, too fast to try): opening Lịch sử while searching (GAME_START from the history page stops
  the replay) and Đăng xuất while searching.
- 2026-11-22 · Nghĩa · C6a: `tools/bot.c` rewritten with POSIX threads (Lec06): one pthread per
  bot (argument = an element of `bots[]`, which lives until the joins), own socket + FrameBuf,
  blocking I/O with SO_RCVTIMEO 1 s (EAGAIN -> look at the stop flag). REGISTER (409 ignored) ->
  LOGIN -> CHALLENGE pairs (bot 2i -> 2i+1, anyone else declined, 404/409 retried after 200 ms) or
  `--match` (FIND_MATCH) -> random moves on the bot's own board -> LEAVE_GAME -> again. MOVE ->
  200|MOVE latency with CLOCK_MONOTONIC in per-thread arrays, merged after pthread_join (mean,
  P95, max). Shared counters behind one mutex (additions only; main prints a copy every 10 s).
  main blocks SIGINT/SIGTERM/SIGALRM with pthread_sigmask before pthread_create and waits with
  sigwait + setitimer; `--secs`, `--idle K` (K connections that send 2 header bytes, then
  nothing); `-pthread` for the bot only. Checked: `make` 0 warnings (also gcc-15 -O2), `make test`;
  real server (S1): N=10 REGISTER/LOGIN, Ctrl+C (SIGINT) and SIGTERM stop at once, 400
  unknown_command to CHALLENGE -> the bots stay logged in; with `--idle 50` the server still
  answered PING at once. Games, DECLINE of a stranger, a bot vs a scripted human and `--match`
  checked against a throwaway Python mock server (~200 000 moves in 11 s, the per-thread move
  counters add up to the shared one). `make debug` (ASan/UBSan) clean with N=10; ThreadSanitizer
  clean (N=20 + `--idle 10`, `--match` + SIGINT), and it does report the race when the mutex is
  removed (the counter check then shows lost updates). Waiting: games and latency with the real
  server need S2–S4 (`--match`: S7); the N=20 `--idle 0` vs `--idle 50` comparison needs S3.
  Found with the bot (server side, to report in `docs/issues.md`): when several logged-in clients
  disconnect at once, `session_end()` broadcasts OFFLINE before closing its fd, a failed
  `send_event()` calls `session_end()` on the other client, which broadcasts back -> endless
  recursion -> SIGSEGV (ASan: stack-overflow, net.c:173 / net.c:198 / lobby.c:65), 3 of 3 runs.
- 2026-11-22 · Hân · S2: `LIST_PLAYERS` (sorted, caller excluded), `CHALLENGE` / `CHALLENGE_REPLY` /
  `CHALLENGE_CANCEL` with every error of protocol §6.2 (bad_args, user_not_found, player_busy,
  challenge_pending, challenge_not_found), 30 s expiry from `tick()` (`lobby_tick`),
  `challenges_drop_for()` called from game start, LOGOUT and `session_end()`. Temporary
  `game_start()` (challenger plays X, GAME_START to both, PLAYER_STATUS IN_GAME) and `LEAVE_GAME`
  stub in `match.c`; S3 replaces them with the real Game. Checked with a scripted 2–3 client
  test (validation, decline, cancel, accept, two incoming challenges, disconnect and logout
  cleanup, 30 s expiry) on an ASan/UBSan build: all passed, no sanitizer output.
  `make` zero warnings, `make test` passed. Note: the opponent also receives `PLAYER_STATUS`
  about the other player at game start (protocol §7: every logged-in client except the subject).
- 2026-11-22 · Hân · S3: real `struct Game` table (`match.h`), `game_start()` creates the Game
  (board, turn X, deadline), `MOVE` with the checks in the exact order of §6.3 (400, 403, 433,
  430, 431, 432; the board never changes on error), response → `OPP_MOVE` → `GAME_END`,
  `game_end()` (FIVE with the 5 cells, BOARD_FULL, delta "0" until S5), `LEAVE_GAME` checks the
  gid and ends a running game by RESIGN, `game_detach()` called from `session_end()`.
  Fixed the `session_end()` recursion found by Nghĩa's bot (see issues.md): the slot's fd is now
  set to -1 before anything is sent to other clients. Checked with 57 scripted checks (incl. a
  225-move draw and a disconnect in a running game) on an ASan/UBSan build, S2 regression 36/36,
  `make test` passed, zero warnings.
- 2026-11-22 · Nghĩa + Hân · M2 and M3 reached with Hân's real server (S2, S3), two GTK client
  windows: both players see each other in the lobby; challenge, decline, cancel and the 30 s
  expiry; accept -> GAME_START with the challenger as X; the client blocks clicks when it is not
  your turn and on an occupied cell; a five-in-a-row win shows the winning line on both sides;
  Về sảnh works. Stopping the server during a game sends both clients back to the login page
  (the lost-server check C4 was waiting for).
- 2026-11-22 · Nghĩa · C6b (client polish, test table, LAN guide): window minimum 760×560 (board
  still square, 30 px cells next to the side panel); login page focuses Tên đăng nhập, Tab goes
  top to bottom, Enter in any field logs in; one table in `dispatch.c` (`error_text()`) gives a
  Vietnamese sentence for every reason token and every error code of protocol.md §4, anything
  else shows `Lỗi <code>: <reason>`, used by every page; window title shows the logged-in user;
  closing the window during a running game asks "Thoát bây giờ sẽ bị xử thua", Thoát closes the
  socket at once and quits; 20-character names wrap (no hyphen) in the lobby bar, the game panel
  and the replay panel; Đăng xuất locked once sent and while an accepted invitation's GAME_START
  is on the way; no TODO left in `client/`. Checked with a throwaway GTK harness built with
  ASan/UBSan that drives the real widgets and saves PNGs: fake mode 15/15 checks and real server
  (S3, `/tmp/c6b.db`) 17/17 (min size, Tab order, 401 sentence, title, lobby and game with two
  20-character names, invitation dialog, quit dialog Ở lại / Thoát: server logs the disconnect
  at once; the opponent's GAME_END DISCONNECT waits for S4). `make` 0 warnings, `make test`
  passed, `make debug` client started in fake mode with no sanitizer report.
  `docs/test_report.md`: 21 scenarios with commands and the protocol.md section expected; rows
  7, 8, 10, 11 (M1–M3) and 19 (bot `--idle`: 0.145 / 0.320 ms without, 0.090 / 0.190 ms with 50
  silent connections) filled in. README: "Running over a LAN" (IP, ufw, WSL2 mirrored or
  portproxy, `cli` + PING first, phone hotspot against client isolation) and the Status line.
  C6 not ticked: the bot against the full server (S4–S7) and the two-machine LAN test wait.
- 2026-11-22 · Hân · Quick fix after S3: `TCP_NODELAY` on accepted sockets (Nagle + delayed ACK
  held responses ~40 ms), `listen()` backlog 16 → `SOMAXCONN`. Bot N=20, 30 s, before → after:
  mean 1.523 → 1.254 ms, P95 4.820 → 2.974 ms, max 95.194 → 49.061 ms, ~5 760 → ~6 930
  moves/s (one run each, so max is noisy). `make` zero warnings, `make test` passed.
  Issue #001 added to `docs/issues.md`.
- 2026-11-22 · Hân · S4: `RESIGN`, `DRAW_OFFER` / `DRAW_REPLY`, `REMATCH` / `REMATCH_REPLY` (new gid,
  X and O swapped), `LEAVE_GAME` cancels a pending rematch, `game_detach()` follows §8 (running
  game: opponent wins by DISCONNECT; ended game: OPP_LEFT). Every handler updates the Game first,
  sends the response, then re-checks the sender and the Game before sending events. Checked with
  50 scripted checks on an ASan/UBSan build (all five ways to end a game, declined draw, declined
  and accepted rematch, leave, disconnect of one or both players, 12 clients closing together),
  `make` zero warnings, `make test` passed.
- 2026-11-22 · Hân · S5: `db_match_save()` writes the match, all its moves and the elo / W-L-D of both
  users in ONE transaction (ROLLBACK on error). `game_end()` rates every finished game (FIVE,
  BOARD_FULL, RESIGN, DRAW_AGREED, DISCONNECT) with `elo_update()`, saves it, updates both Clients and
  sends GAME_END with the real new ELO and delta; a player who just disconnected is rated and saved
  too. `PLAYER_STATUS` after a game and OFFLINE carry the new ELO. `game_start()` / `game_end()` log
  one line each. Checked with 27 scripted checks (1000 vs 1000 → +16 / −16, RESIGN, DRAW,
  DISCONNECT, rows in matches / moves / users, restart keeps the ELO) on an ASan/UBSan build,
  S4 regression passed, `make` zero warnings, `make test` passed.
- 2026-11-23 · Nghĩa + Hân · M4 reached with Hân's real server (S4, S5), two GTK client windows:
  resign; draw offer declined, then accepted; rematch with X and O swapped; closing the window
  during a game -> the opponent wins by DISCONNECT; server stopped and restarted on the same DB
  keeps the ELO. Still missing: `STATS` on the server (Hân does it first in S6), so after a game
  the lobby shows "Không lấy được ELO và thành tích từ server: Server chưa hỗ trợ chức năng này".
- 2026-11-23 · Hân · S4 (STATS): `STATS` / `STATS|user` (L, Q) → `200|STATS|user|elo|wins|losses|draws`,
  read with `db_user_get()` (same function as LOGIN), 404 `user_not_found`. Checked with
  `test_stats.py` and a 10-bot 20 s run on an ASan/UBSan build, `make` zero warnings,
  `make test` passed.
- 2026-11-23 · Hân · S6: `HISTORY` / `HISTORY|page` (10 per page, newest first, outcome from the caller's
  side, `ended_at` as `YYYY-MM-DD HH:MM`), `REPLAY|mid` (404 `match_not_found`, moves in play order),
  `LEADERBOARD` / `LEADERBOARD|n` (default 10, max 20, elo desc then username); new `history.c`, queries in
  `db.c`; STATS was added in S4. Checked with 48 scripted checks (12-game history with paging, a 225-move
  replay of 1102 bytes, ranking order, every error code) on an ASan/UBSan build, S4/S5/STATS regression
  passed, `make` zero warnings, `make test` passed.
- 2026-11-23 · Hân · S7: `FIND_MATCH` / `CANCEL_MATCH`; the queue is the set of IN_QUEUE clients
  ordered by `queued_at` (no separate list, so LOGOUT, disconnect and CANCEL leave it by changing
  the state); matchmaking pass once per second from `lobby_tick()`: longest waiter first, partner
  with the closest ELO inside `min(400, 100 + 50 * floor(waited / 10))`, the longer waiter plays X,
  size 15, 30 s per move; move timer `match_tick()` from `tick()`: `deadline` passed → the player
  to move loses, reason TIMEOUT. Both use the 1 s `select()` timeout, no thread or alarm. Checked
  with 29 scripted checks plus the slow ones (gap 150 pairs at 10.0 s, gap 450 never pairs, gap
  400 pairs at 60.1 s, TIMEOUT after 16 s, a move restarts the clock) on an ASan/UBSan build, no
  sanitizer output; S2 regression passed; `make test` passed, zero warnings.
- 2026-11-24 · Nghĩa + Hân · M5 reached with Hân's real server (S6, S7), two GTK client windows:
  back in the lobby the top bar shows ELO and W/L/D from STATS; history and replay (Tự chạy)
  correct; leaderboard in the right order with our own row in bold; quick match pairs and Hủy
  cancels; C5 steps 18 and 20 (Lịch sử while searching, Đăng xuất while searching) now checked
  with the real server, as the C5 entry asked; sitting still loses by TIMEOUT when the clock
  reaches 0. cli on S7 (Linux): STATS / `STATS|user` / `STATS|nobody` (404 user_not_found);
  FIND_MATCH pairs at once, the longer waiter plays X, 30 s per move; CANCEL_MATCH 200, then 403;
  15 s per move, X moves at 10 s, O never moves -> GAME_END X_WIN TIMEOUT 16 s later; HISTORY /
  REPLAY / LEADERBOARD and their 400 / 404 errors; bot `--match` (10 bots, 15 s): 69 games, no
  error, P95 0.456 ms, server still running. `docs/test_report.md`: rows 16 and 17 updated, rows
  22 (TIMEOUT) and 23 (history, replay, leaderboard, stats) added. Still waiting: S8 (Hân) --
  #002 is not really fixed: `net_receive()` in net.c logs the malformed frame but nothing calls
  it, main.c still closes without a log; and the LAN test on Wednesday 25/11. Also restored what
  959344b overwrote with an older copy of this file: the M4 tick and log line, and the duplicate
  S3 entry and stray line removed on 23/11.
- 2026-11-24 · Nghĩa (thay Hân) · S8, done by Nghĩa with Hân's agreement (exception to the ownership
  rule, for this stage only). #002: `main.c` calls `net_receive()` (its duplicate receive loop is
  gone), which logs `malformed frame from <addr>` before closing. Clean stop: SIGINT / SIGTERM with
  `sigaction()` and no SA_RESTART, the handler only sets a `volatile sig_atomic_t` flag; after the
  loop every client fd, the listening socket and the database are closed, `server stopped` is
  logged, exit code 0 (running games are dropped, not rated). Checked with two real GTK windows in a
  game, SIGINT and SIGTERM: both back on the login page with "Mất kết nối tới server".
  Malformed requests, 108 checked against protocol.md, all right now; wrong before: a cid + 2^32 was
  cut to an int and matched the challenge; the number of fields was checked before the state rule
  of §4; ` 3` and `+3` were accepted as integers (one `parse_long()` in protocol.c instead of three
  copies); an unknown command of more than 16 364 bytes closed the connection instead of getting
  its 400. Also: LOGIN / LOGOUT stop when their response cannot be sent (LOGIN would have broadcast
  a PLAYER_STATUS with an empty name), LOGIN answers 500 on a database error, server.log keeps only
  the [A-Z_] part of an unknown command (`LOGIN han 123456` with spaces logged the password, `\n`
  forged a log line), fd >= FD_SETSIZE is refused in `accept_client()`. No TODO left in `server/`,
  stale stage comments rewritten, no uncalled function, every syscall checked.
  ASan/UBSan: 0 reports; `leaks --atExit`: 0 leaks for 0 total leaked bytes (84 scripted checks,
  108 malformed requests, bot 10 for 30 s, bot 10 `--match` for 20 s, SIGINT). Load, 5 minutes, 20
  bots + 50 silent connections: 124 065 games, 13 524 134 moves, mean 0.196 ms, P95 0.797 ms, RSS
  between 3.9 and 4.7 MB with no growth, matches in the database accounted for one by one. 98
  clients killed at the same moment: server keeps running. `make` 0 warnings (also gcc-15 -O0 and
  -O2 for `server/` and `common/`), `make test` passed. Not done: valgrind (optional).
  Checked on Linux after the push (24/11, gcc build, by Nghĩa): `make` 0 warnings, `make test`
  28/28, no TODO left; #002 logs `malformed frame from ...`; regression of test report rows 9, 11,
  12, 13 and of S4–S7 all right; SIGTERM while 20 bots + 30 idle connections were running ->
  `server stopped`, exit 0; restart on the same DB keeps the data, `PRAGMA integrity_check` = ok.
  Protocol questions from the malformed requests, decided by Nghĩa, protocol.md unchanged: a number
  beyond 64 bits stays 400 `bad_args` (431 is for valid numbers outside the board); HISTORY pages
  above 1 000 000 stay 400; LEADERBOARD|50 stays cut to 20 (§6.4); control characters (< 0x20, 0x7F)
  in an echoed unknown command are now replaced by `?` (§2), e.g. `HELLO\nX` ->
  `400|HELLO?X|unknown_command`; cutting an overlong command name stays out of §3.
  Seen on macOS only under full bot load, cause not found: in 3 of 100 stops one bot's connection
  stayed ESTABLISHED although the server process had exited; no crash, hang of the server or lost
  data (database consistent after the whole series). README: new section "Server", Status line updated.
- 2026-11-25 · Nghĩa + Hân · LAN test: server in an Ubuntu VM (VirtualBox, Bridged Adapter,
  172.20.10.5) on Hân's machine, clients on Nghĩa's MacBook (172.20.10.2) and in the VM, over an
  iPhone hotspot: lobby, challenge, full game to FIVE, resign, draw, rematch, timeout, server
  stopped during a game, all right. Bot over the LAN from the Mac (20 bots, 60 s) during a GUI
  game: 293 games, 33 464 moves, mean 17.06 ms, P95 29.8 ms, max 222 ms, errors only 404:4 409:1,
  "mutex OK". `docs/test_report.md`: rows 4, 7, 12 and 21 updated, no row open. C6 ticked.
