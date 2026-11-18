/*
 * server/match.c — MOVE / RESIGN / DRAW / REMATCH / LEAVE handlers, game_end()
 * (docs/protocol.md §6.3, §7, §8).
 * Owner: Hân.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S3): start a game: new gid, both players IN_GAME,
 *           GAME_START|gid|size|your_symbol|opponent|opp_elo|sec_per_move to both,
 *           PLAYER_STATUS IN_GAME to the others.
 * TODO(S3): MOVE|gid|row|col (G), checks in the exact order of §6.3: strtol + end pointer
 *           (400 bad_args), sender plays game gid (403 not_in_game), then game.c (433, 430,
 *           431, 432). Success: 200|MOVE|move_no to the mover, then OPP_MOVE|gid|row|col|move_no
 *           to the opponent, then GAME_END to both if the game is over. A move clears a
 *           pending draw offer.
 * TODO(S3): game_end(): mark the game ended, send each player their own
 *           GAME_END|gid|result|reason|new_elo|elo_delta (+ 5 "r,c" fields only for FIVE);
 *           both players stay IN_GAME until LEAVE_GAME or rematch.
 * TODO(S4): RESIGN|gid -> GAME_END reason RESIGN (433 on an ended game).
 * TODO(S4): DRAW_OFFER|gid (409 offer_pending) -> opponent gets DRAW_OFFERED|gid;
 *           DRAW_REPLY|gid|ACCEPT -> GAME_END DRAW / DRAW_AGREED;
 *           DRAW_REPLY|gid|DECLINE -> offerer gets DRAW_DECLINED|gid; 404 no_pending_offer.
 * TODO(S4): REMATCH|gid (ended game; 404 opponent_left, 409 offer_pending) ->
 *           REMATCH_REQUESTED|gid; REMATCH_REPLY|gid|ACCEPT -> new gid, X and O swapped,
 *           GAME_START to both; DECLINE -> requester gets REMATCH_DECLINED|gid.
 * TODO(S4): LEAVE_GAME|gid -> 200, state LOBBY; if still running, GAME_END (RESIGN) to both
 *           first; then OPP_LEFT|gid to the opponent.
 * TODO(S4): disconnect (from session_end()): running game -> opponent wins, reason DISCONNECT;
 *           ended game -> OPP_LEFT|gid.
 * TODO(S5): game_end() rates every finished game (incl. RESIGN, TIMEOUT, DISCONNECT) with
 *           elo.c, stores the match and its moves (db.c), PLAYER_STATUS with the new ELO.
 * TODO(S7): move timer from tick(): the player to move loses, reason TIMEOUT, after
 *           sec_per_move seconds.
 */
