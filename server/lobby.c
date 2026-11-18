/*
 * server/lobby.c — LIST_PLAYERS, PLAYER_STATUS, challenges, matchmaking (docs/protocol.md §6.2).
 * Owner: Hân.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S2): LIST_PLAYERS (L, Q, G): every logged-in user except the caller, sorted by username:
 *           200|LIST_PLAYERS|n|user,elo,status|...
 * TODO(S2): PLAYER_STATUS|user|status|elo to every logged-in client except user (login, logout,
 *           queue/game enter and leave, ELO change).
 * TODO(S2): CHALLENGE|user|size|sec (L): 400 bad_args (self, size != 15, sec not 15/30/60);
 *           404 user_not_found (offline); 409 player_busy (target not IDLE);
 *           409 challenge_pending (caller already has an outgoing one) -> 200|CHALLENGE|cid,
 *           target gets CHALLENGE_FROM|cid|from|from_elo|size|sec.
 * TODO(S2): CHALLENGE_REPLY|cid|ACCEPT|DECLINE (L, target only, anyone else 404
 *           challenge_not_found): ACCEPT -> 409 player_busy if the challenger is no longer
 *           idle, else start the game (match.c), both get GAME_START;
 *           DECLINE -> challenger gets CHALLENGE_DECLINED|cid|by_user.
 * TODO(S2): CHALLENGE_CANCEL|cid (L, challenger only) -> target gets CHALLENGE_CANCELLED|cid.
 * TODO(S2): expiry from tick(): after CHALLENGE_EXPIRE_SEC both get CHALLENGE_EXPIRED|cid.
 * TODO(S2): a player entering a game or the queue, logging out or disconnecting loses every
 *           pending challenge; the other side gets CHALLENGE_CANCELLED|cid.
 * TODO(S6): HISTORY, REPLAY, LEADERBOARD, STATS handlers (§6.4) — the layout gives them no
 *           file of their own; here or a new file, Hân decides.
 * TODO(S7): FIND_MATCH (L) -> 200, state Q; CANCEL_MATCH (Q) -> 200, state L.
 * TODO(S7): matchmaking pass every second from tick(): pair two queued players whose ELO gap
 *           <= MM_BASE_GAP + MM_STEP_GAP * floor(waited / MM_STEP_SEC), capped at MM_MAX_GAP;
 *           BOARD_SIZE, MATCH_SEC_PER_MOVE; the player who waited longer plays X.
 */
