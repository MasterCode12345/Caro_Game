# Caro Online — Application Protocol v1.1

This document is the contract between `client/` and `server/`. Change it first, with both
members agreeing, then change the code (commit prefix `protocol:`).

## 1. Transport and framing

- TCP, default port **5500**. One persistent connection per client.
- Every message is one frame:

```text
+----------------------------+---------------------------------------+
| LENGTH: 4 bytes            | PAYLOAD: LENGTH bytes, UTF-8 text     |
| uint32, network byte order | COMMAND|arg1|arg2|...                 |
+----------------------------+---------------------------------------+
Example: 00 00 00 12  "LOGIN|nghia|123456"   (0x12 = 18 bytes)
```

- `1 <= LENGTH <= 16384`. A frame with LENGTH 0 or larger than 16384 is malformed: the server
  logs it and closes that connection (`session_end`). The client treats it as a lost connection.
- The payload has no terminating `\0` and no trailing newline.
- The receiver keeps one buffer per connection and only handles complete frames, so frames that
  arrive glued together or split across several `recv()` calls are both handled (Lec04, byte
  stream problem, solution 3).

## 2. Payload format

- Fields are separated by `|`. No field may contain `|`, `\r` or `\n`.
- Lists: each list item is one field; attributes inside an item are separated by `,`.
  Example: `200|LIST_PLAYERS|2|han,1016,IDLE|minh,980,IN_GAME`
- Optional trailing arguments are simply omitted.
- Integers are decimal ASCII. Coordinates are `row` then `col`, counted from 0.
- Signed deltas are written with a sign: `+16`, `-16`, `0`.
- `username`: 3–20 characters `[A-Za-z0-9_]`.
- `password`: 6–32 printable ASCII characters (0x21–0x7E) except `|`.

## 3. Message kinds

| Kind | Direction | Form | Example |
| --- | --- | --- | --- |
| Request | client → server | `COMMAND|args` | `MOVE|5|7|7` |
| Response | server → the sender | `code|COMMAND|data` | `200|MOVE|1` |
| Error response | server → the sender | `code|COMMAND|reason` | `432|MOVE|cell_occupied` |
| Event | server → any client, unrequested | `EVENT|args` | `OPP_MOVE|5|7|7|1` |

- A client tells them apart by the first field: a number means a response.
- The server answers **every** request with exactly one response, and answers the requests of one
  client **in the order they were received**. The client matches responses to its pending
  requests with a FIFO queue.
- When a request causes events, the response to the sender is sent first, then the events
  (e.g. `200|MOVE|9` to the mover, then `OPP_MOVE` to the opponent, then `GAME_END` to both).
- For an unknown command the response is `400|<COMMAND as received>|unknown_command`.

## 4. Status codes

| Code | Meaning | Typical reason tokens |
| --- | --- | --- |
| 200 | OK | — |
| 201 | Created | — (REGISTER) |
| 400 | Bad request | `unknown_command`, `bad_args`, `invalid_username`, `invalid_password` |
| 401 | Not authenticated | `not_logged_in`, `invalid_credentials` |
| 403 | Not allowed in the current state | `forbidden_state`, `not_in_game` |
| 404 | Not found | `user_not_found`, `challenge_not_found`, `match_not_found`, `no_pending_offer`, `opponent_left` |
| 409 | Conflict | `username_taken`, `already_logged_in`, `player_busy`, `challenge_pending`, `offer_pending` |
| 430 | Not your turn | `not_your_turn` |
| 431 | Out of board | `out_of_board` |
| 432 | Cell occupied | `cell_occupied` |
| 433 | Game not active | `game_not_active` |
| 500 | Server error | `server_error` |

State rule: a command sent in a state where it is not allowed gets `401|CMD|not_logged_in` if the
client is not logged in, otherwise `403|CMD|forbidden_state`.

## 5. Session states

```text
CONNECTED --LOGIN--> LOBBY --FIND_MATCH--> IN_QUEUE --matched--> IN_GAME
    ^                  |  ^                   |                     |
    +------LOGOUT------+  +----CANCEL_MATCH---+                     |
                       ^                                            |
                       +----------------LEAVE_GAME------------------+
LOBBY --challenge accepted--> IN_GAME
```

- After `GAME_END` both players stay `IN_GAME` (game ended) until `LEAVE_GAME` or a rematch.
- Losing the TCP connection in any state ends the session (see section 8).

Player status shown to others (`PLAYER_STATUS`, `LIST_PLAYERS`): `IDLE` (LOBBY), `IN_QUEUE`,
`IN_GAME`, `OFFLINE` (only in `PLAYER_STATUS`).

## 6. Requests

States: C = CONNECTED, L = LOBBY, Q = IN_QUEUE, G = IN_GAME. "Any" = every state.

### 6.1 Account and session

| Request | States | Success response | Errors | Events |
| --- | --- | --- | --- | --- |
| `PING` | Any | `200|PING` | — | — |
| `REGISTER|user|pass` | C | `201|REGISTER` | 400 `invalid_username` / `invalid_password` / `bad_args`, 409 `username_taken` | — |
| `LOGIN|user|pass` | C | `200|LOGIN|user|elo|wins|losses|draws` | 400 `bad_args`, 401 `invalid_credentials`, 409 `already_logged_in` | `PLAYER_STATUS|user|IDLE|elo` to all other logged-in clients |
| `LOGOUT` | L, Q | `200|LOGOUT` | 403 | `PLAYER_STATUS|user|OFFLINE|elo` to others |

- `invalid_credentials` is used both for an unknown username and a wrong password.
- LOGOUT from Q also removes the player from the queue. LOGOUT in G is forbidden (leave first).

### 6.2 Lobby and challenges

| Request | States | Success response | Errors | Events |
| --- | --- | --- | --- | --- |
| `LIST_PLAYERS` | L, Q, G | `200|LIST_PLAYERS|n|user,elo,status|...` | — | — |
| `CHALLENGE|user|size|sec` | L | `200|CHALLENGE|cid` | 400 `bad_args` (self, bad size/sec), 404 `user_not_found` (offline), 409 `player_busy` (target not IDLE), 409 `challenge_pending` (you already have an outgoing one) | target: `CHALLENGE_FROM|cid|from|from_elo|size|sec` |
| `CHALLENGE_REPLY|cid|ACCEPT` | L | `200|CHALLENGE_REPLY` | 404 `challenge_not_found`, 409 `player_busy` (challenger no longer idle) | both: `GAME_START` |
| `CHALLENGE_REPLY|cid|DECLINE` | L | `200|CHALLENGE_REPLY` | 404 `challenge_not_found` | challenger: `CHALLENGE_DECLINED|cid|by_user` |
| `CHALLENGE_CANCEL|cid` | L | `200|CHALLENGE_CANCEL` | 404 `challenge_not_found` | target: `CHALLENGE_CANCELLED|cid` |
| `FIND_MATCH` | L | `200|FIND_MATCH` (state → Q) | — | both, when paired: `GAME_START` |
| `CANCEL_MATCH` | Q | `200|CANCEL_MATCH` (state → L) | — | — |

- `LIST_PLAYERS` lists every logged-in user except the caller, sorted by username.
- `size`: only `15` is accepted for now (10 and 19 are reserved for a later version). `sec`: 15, 30 or 60.
- Each user has at most one outgoing pending challenge but may receive several.
- A challenge expires after 30 seconds: both sides get `CHALLENGE_EXPIRED|cid`.
- Only the target can `CHALLENGE_REPLY`; only the challenger can `CHALLENGE_CANCEL`.
  Anyone else gets `404 challenge_not_found`.
- When a player enters a game or the queue, logs out or disconnects, every pending challenge
  involving them is removed and the other side gets `CHALLENGE_CANCELLED|cid`.
- Matchmaking (`FIND_MATCH`): every second the server pairs two queued players whose ELO
  difference is at most `100 + 50 × floor(waited_seconds / 10)`, capped at 400. Matched games
  use size 15 and 30 seconds per move. The player who waited longer plays X.

### 6.3 In a game

| Request | States | Success response | Errors | Events |
| --- | --- | --- | --- | --- |
| `MOVE|gid|row|col` | G | `200|MOVE|move_no` | 400 `bad_args`, 403 `not_in_game`, 433, 430, 431, 432 | opponent: `OPP_MOVE|gid|row|col|move_no`; if the game ends: both `GAME_END` |
| `RESIGN|gid` | G | `200|RESIGN` | 403 `not_in_game`, 433 | both: `GAME_END` (reason RESIGN) |
| `DRAW_OFFER|gid` | G | `200|DRAW_OFFER` | 403, 433, 409 `offer_pending` | opponent: `DRAW_OFFERED|gid` |
| `DRAW_REPLY|gid|ACCEPT` | G | `200|DRAW_REPLY` | 403, 433, 404 `no_pending_offer` | both: `GAME_END` (DRAW, DRAW_AGREED) |
| `DRAW_REPLY|gid|DECLINE` | G | `200|DRAW_REPLY` | 403, 433, 404 `no_pending_offer` | offerer: `DRAW_DECLINED|gid` |
| `REMATCH|gid` | G (game ended) | `200|REMATCH` | 403, 404 `opponent_left`, 409 `offer_pending` | opponent: `REMATCH_REQUESTED|gid` |
| `REMATCH_REPLY|gid|ACCEPT` | G (game ended) | `200|REMATCH_REPLY` | 403, 404 `no_pending_offer` / `opponent_left` | both: `GAME_START` (new gid, X and O swapped) |
| `REMATCH_REPLY|gid|DECLINE` | G (game ended) | `200|REMATCH_REPLY` | 403, 404 | requester: `REMATCH_DECLINED|gid` |
| `LEAVE_GAME|gid` | G | `200|LEAVE_GAME` (state → L) | 403 `not_in_game` | if the game was still running: both `GAME_END` (reason RESIGN) first; then opponent: `OPP_LEFT|gid` |

MOVE checks, in this exact order (first failure wins, the board never changes on error):

1. `gid`, `row`, `col` are valid integers → else `400 bad_args`
2. the sender is a player of game `gid` → else `403 not_in_game`
3. the game is running (not ended) → else `433 game_not_active`
4. it is the sender's turn → else `430 not_your_turn`
5. `0 <= row, col < size` → else `431 out_of_board`
6. the cell is empty → else `432 cell_occupied`
7. place the mark, increment `move_no` (first move = 1), switch the turn, reset the move timer,
   clear any pending draw offer, check the result.

A draw offer is cleared by the next move. RESIGN, DRAW_OFFER and DRAW_REPLY on an ended game → 433.

### 6.4 History and ranking

| Request | States | Success response | Errors |
| --- | --- | --- | --- |
| `HISTORY` or `HISTORY|page` | L, Q | `200|HISTORY|page|n|mid,opponent,my_symbol,outcome,reason,ended_at|...` | 400 `bad_args` |
| `REPLAY|mid` | L, Q | `200|REPLAY|mid|size|x_user|o_user|result|reason|n|r,c|r,c|...` | 400, 404 `match_not_found` |
| `LEADERBOARD` or `LEADERBOARD|n` | L, Q | `200|LEADERBOARD|k|rank,user,elo,wins,losses,draws|...` | 400 |
| `STATS` or `STATS|user` | L, Q | `200|STATS|user|elo|wins|losses|draws` | 404 `user_not_found` |

- HISTORY: the caller's games, newest first, 10 per page, `page` starts at 1 (default 1).
  `outcome` is from the caller's point of view: `WIN`, `LOSS` or `DRAW`.
  `ended_at` format: `YYYY-MM-DD HH:MM` (local server time).
- REPLAY: moves in play order (X first). Any logged-in user may replay any match.
- LEADERBOARD: `n` default 10, maximum 20, ordered by ELO descending then username.

## 7. Events (server → client)

| Event | Sent to | Meaning |
| --- | --- | --- |
| `PLAYER_STATUS|user|status|elo` | every logged-in client except `user` | user logged in, logged out, entered/left queue or game, ELO changed |
| `CHALLENGE_FROM|cid|from|from_elo|size|sec` | target | new invitation |
| `CHALLENGE_DECLINED|cid|by_user` | challenger | target declined |
| `CHALLENGE_CANCELLED|cid` | the other side | challenge withdrawn or no longer possible |
| `CHALLENGE_EXPIRED|cid` | both | 30 s passed without an answer |
| `GAME_START|gid|size|your_symbol|opponent|opp_elo|sec_per_move` | both players | a game begins; X moves first |
| `OPP_MOVE|gid|row|col|move_no` | the opponent of the mover | validated move |
| `DRAW_OFFERED|gid` / `DRAW_DECLINED|gid` | opponent / offerer | draw offer and refusal |
| `REMATCH_REQUESTED|gid` / `REMATCH_DECLINED|gid` | opponent / requester | rematch request and refusal |
| `GAME_END|gid|result|reason|new_elo|elo_delta|r,c|r,c|r,c|r,c|r,c` | both players | game over; each player gets their own `new_elo` and `elo_delta`; the five `r,c` fields (winning line) are present only when `reason` is `FIVE` |
| `OPP_LEFT|gid` | remaining player | opponent left the finished game or disconnected after it ended |

- `result`: `X_WIN`, `O_WIN`, `DRAW`.
- `reason`: `FIVE`, `BOARD_FULL`, `RESIGN`, `DRAW_AGREED`, `TIMEOUT`, `DISCONNECT`.
- For FIVE with a line longer than five, the server sends five consecutive cells that include the last move.

## 8. Disconnects and timers

- `recv()` returning 0 or an error, a malformed frame, or a failed `send()` ends the session:
  pending challenges → `CHALLENGE_CANCELLED` to the other side; queue entry removed;
  running game → opponent wins, reason `DISCONNECT`; ended game → opponent gets `OPP_LEFT`;
  logged-in user → `PLAYER_STATUS|user|OFFLINE|elo` to others.
- Move timer (advanced): if the player to move has not moved within `sec_per_move` seconds, they
  lose with reason `TIMEOUT`. The client's countdown is display only; the server decides.

## 9. ELO

- New accounts start at 1000. K = 32. `S` = 1 win, 0.5 draw, 0 loss.
- `E_A = 1 / (1 + 10^((R_B - R_A) / 400))`, `delta_A = round(32 × (S_A − E_A))`, `delta_B = −delta_A`.
- Checks: 1000 vs 1000, A wins → +16 / −16. 1000 beats 1200 → +24. 1200 beats 1000 → +8.
- Every finished game is rated (including RESIGN, TIMEOUT, DISCONNECT).

## 10. Sample session

```text
A C>S  LOGIN|nghia|123456
A S>C  200|LOGIN|nghia|1000|0|0|0
B S>C  PLAYER_STATUS|nghia|IDLE|1000
A C>S  CHALLENGE|han|15|30
A S>C  200|CHALLENGE|17
B S>C  CHALLENGE_FROM|17|nghia|1000|15|30
B C>S  CHALLENGE_REPLY|17|ACCEPT
B S>C  200|CHALLENGE_REPLY
A S>C  GAME_START|5|15|X|han|1000|30
B S>C  GAME_START|5|15|O|nghia|1000|30
A C>S  MOVE|5|7|7
A S>C  200|MOVE|1
B S>C  OPP_MOVE|5|7|7|1
B C>S  MOVE|5|7|7
B S>C  432|MOVE|cell_occupied
...
A C>S  MOVE|5|7|11
A S>C  200|MOVE|9
B S>C  OPP_MOVE|5|7|11|9
A S>C  GAME_END|5|X_WIN|FIVE|1016|+16|7,7|7,8|7,9|7,10|7,11
B S>C  GAME_END|5|X_WIN|FIVE|984|-16|7,7|7,8|7,9|7,10|7,11
```

## Changelog

- v1.1 (18/11/2026): `|` separator, full message list, response ordering rule, LOGIN returns username.
- v1.0 (11/10/2026): checkpoint 1 draft (space separator).
