# Test report — Caro Online (Group 13)

The full test table for checkpoint 2. Run each scenario, then fill **Result** (`Pass`, or
`Fail: what happened`) and **Tester**. "Expected" quotes `docs/protocol.md` (§ = section).

Setup used below unless a row says otherwise:

```bash
make
./build/caro_server 5500 /tmp/test.db
./build/caro_client
./build/cli 127.0.0.1 5500
```

Two GUI clients on one machine: start `./build/caro_client` twice. Over a LAN: see README,
"Running over a LAN".

| # | Scenario | How to run | Expected | Result | Tester |
| --- | --- | --- | --- | --- | --- |
| 1 | Cut frame (one frame over many `recv()`) | `printf '/bytes PING\n' \| ./build/cli 127.0.0.1 5500` (one byte every 50 ms) | §1: "the receiver keeps one buffer per connection and only handles complete frames" → exactly one `<< 200\|PING`, after the last byte | | |
| 2 | Glued frames (many frames in one `send()`) | `printf '/burst PING ;; PING ;; PING\n' \| ./build/cli 127.0.0.1 5500` | §1 + §3: one response per frame, "in the order they were received" → three `<< 200\|PING` | | |
| 3 | Malformed frame: LENGTH 0, LENGTH > 16384 | `printf '/zero\n' \| ./build/cli 127.0.0.1 5500`, then the same with `/huge`; keep a second cli connected and send `PING` | §1: "A frame with LENGTH 0 or larger than 16384 is malformed: the server logs it and closes that connection" → `-- server closed the connection` at once (no wait for 20000 bytes), a line in `server.log`; the second cli still gets `200\|PING` | | |
| 4 | 20 bots + 2 GTK clients at once | Terminal A: `./build/bot 127.0.0.1 5500 20 --secs 180`. Then two `./build/caro_client` (users `alice`, `bob`): log in, play a full game against each other | §3: every request gets exactly one response; §7: the lobby list shows `bot_0`…`bot_19` changing between Rảnh / Đang chơi. The GUI game is not slowed down; the bot's final table shows only 404/409 retries as errors and "mutex OK" | | |
| 5 | Client quits abruptly: in the lobby, in a running game, window closed in a game | (a) lobby: `kill -9 $(pgrep -n caro_client)`; (b) same during a game; (c) click the window's close button during a game, answer **Thoát** | §8: "`recv()` returning 0 … ends the session: … running game → opponent wins, reason `DISCONNECT` … `PLAYER_STATUS\|user\|OFFLINE\|elo` to others". (c) first asks "Thoát bây giờ sẽ bị xử thua"; **Ở lại** keeps playing | | |
| 6 | Server stops while clients are in the lobby | Two clients in the lobby, then Ctrl+C in the server terminal | §1: the client "treats it as a lost connection" → both back to the login page with "Mất kết nối tới server"; no crash, no frozen window | | |
| 7 | Server stops during a game | Two clients in a game, then `kill -9 $(pgrep -x caro_server)` | Same as #6, from the game page: every dialog closed, both clients on the login page with "Mất kết nối tới server" | Pass: both clients went back to the login page (M3) | Nghĩa + Hân |
| 8 | Duplicate login | Log in as `alice` in client 1, then as `alice` in client 2 (or `LOGIN\|alice\|<pass>` in cli) | §6.1: `409\|LOGIN\|already_logged_in` → "Tài khoản đang đăng nhập ở nơi khác"; client 1 is not affected | Pass (M1) | Nghĩa + Hân |
| 9 | Command in the wrong state (401 / 403) | cli, not logged in: `CHALLENGE\|bob\|15\|30`, `MOVE\|1\|7\|7`. cli logged in and in a game: `LOGOUT`. Logged in, in the lobby: `CANCEL_MATCH` | §4 state rule: "`401\|CMD\|not_logged_in` if the client is not logged in, otherwise `403\|CMD\|forbidden_state`"; §6.1: "LOGOUT in G is forbidden" | | |
| 10 | The 4 bad moves (430–433) | Two cli sessions in game `gid` (A = X, B = O): B `MOVE\|gid\|7\|7` first (430); A `MOVE\|gid\|15\|0` (431); A `MOVE\|gid\|7\|7`, then B `MOVE\|gid\|7\|7` (432); after the game ended, `MOVE\|gid\|0\|0` (433). In the GUI: click out of turn and on an occupied cell | §6.3, checks in this exact order: `430 not_your_turn`, `431 out_of_board`, `432 cell_occupied`, `433 game_not_active`; "the board never changes on error". The GUI blocks the clicks before sending | Pass: the client blocks both clicks; via cli the server answered 430 and 432 (M3). 431 and 433 still to record | Nghĩa + Hân |
| 11 | Win in each of the 4 directions | Four games (GUI or cli): five in a row horizontally, vertically, on the `\` diagonal and on the `/` diagonal; one of them on the board edge | §7: `GAME_END\|gid\|X_WIN\|FIVE\|…` with "the five `r,c` fields (winning line)"; §9 ELO +16 / −16 at 1000 vs 1000; the winning line is drawn on both clients | Pass: five in a row, winning line shown on both clients (M3). Each direction still to record | Nghĩa + Hân |
| 12 | Two players challenge the same player | A and B both challenge C; C sees two invitations and accepts A | §6.2: "Each user … may receive several"; "When a player enters a game …, every pending challenge involving them is removed and the other side gets `CHALLENGE_CANCELLED\|cid`" → A and C in a game; B's waiting dialog closes with "Lời mời tới C đã bị hủy (người đó không còn rảnh)" | | |
| 13 | Invitation expires after 30 s | A challenges B; B does not answer | §6.2: "A challenge expires after 30 seconds: both sides get `CHALLENGE_EXPIRED\|cid`" → both dialogs close with a notice | | |
| 14 | Resign, draw, rematch | In a game: Xin thua; next game Cầu hòa (declined, then accepted); after the end Đấu lại (declined, then accepted) | §6.3: `GAME_END` reason `RESIGN` / `DRAW_AGREED`; `DRAW_DECLINED`, `REMATCH_DECLINED`; an accepted rematch sends `GAME_START` with a new gid and "X and O swapped" | | |
| 15 | Leave a running game | Về sảnh during a game, confirm | §6.3 `LEAVE_GAME`: "if the game was still running: both `GAME_END` (reason `RESIGN`) first; then opponent: `OPP_LEFT\|gid`"; the leaver is in the lobby, ELO updated | | |
| 16 | Quick match, and cancel just as the server pairs | Two clients: Tìm trận nhanh on both → paired. Again, and press Hủy at the moment the second player joins (cli: `FIND_MATCH`, then `CANCEL_MATCH` about 1 s later while another cli sends `FIND_MATCH`) | §6.2: pairing every second, ELO gap ≤ 100 + 50 × ⌊waited/10⌋; matched games 15×15, 30 s per move. A `CANCEL_MATCH` that arrives after `GAME_START` gets `403 forbidden_state` and the client stays in the game | | |
| 17 | Data kept after a server restart | Register, play one game, note ELO and W/L/D; Ctrl+C the server; restart with the same DB; log in again; open Lịch sử, replay the game | §9: "Every finished game is rated"; §6.4: `STATS`, `HISTORY` and `REPLAY` return the same numbers and moves as before the restart | | |
| 18 | Memory checks: ASan, valgrind | macOS: `make debug`, then #4 and #14 with the debug binaries. Linux: `valgrind --leak-check=full ./build/caro_server 5500 /tmp/test.db` during #4, then Ctrl+C | No ASan/UBSan report; valgrind: "definitely lost: 0 bytes", no invalid read/write | | |
| 19 | 50 silent connections (`bot --idle`) | `./build/bot 127.0.0.1 5500 20 --secs 60 --idle 0`, then `./build/bot 127.0.0.1 5500 20 --secs 60 --idle 50`; compare the `latency` lines | §1: a frame is handled only when complete; the `select()` server only reads a socket that is readable, so 50 clients stuck in the middle of a header must not slow the others | Pass. macOS, 22/11, Hân's server (S3), 20 bots, 60 s each. No silent connection: mean 0.145 ms, P95 0.320 ms. 50 silent connections: mean 0.090 ms, P95 0.190 ms. The `select()` server is not blocked by slow clients (see note 1) | Nghĩa |
| 20 | Latency under load before / after `TCP_NODELAY` (Linux) | On Linux, #19 with `--idle 0`, once without and once with `TCP_NODELAY` on the server's client sockets | MOVE → `200\|MOVE` stays well under 1 ms; without `TCP_NODELAY`, Nagle + delayed ACK can add ≈ 40 ms | see Hân's log in docs/progress.md | |
| 21 | Two machines over a LAN | Server on Hân's Linux machine, a client on another machine: README "Running over a LAN" (find the IP, open port 5500, `cli <IP> 5500` + `PING`, then the GUI) | §1: TCP port 5500; M2/M3 again across machines: lobby, challenge, a full game | | |

## Notes

1. Row 19: during this measurement the server printed its log to the terminal, so the `max`
   values are noisy and are not reported. On macOS no 40 ms delay caused by Nagle's algorithm
   was seen; on Linux it does appear (row 20).
2. Rows 14, 15, 16 and 17 need server stages S4–S7. Row 5's `DISCONNECT` result for the
   opponent needs S4: the client side (confirmation, socket closed at once) already works.
