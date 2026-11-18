/*
 * server/protocol.c — command table, state checks, dispatch to handlers.
 * Owner: Hân.
 *
 * The server answers every request with exactly one response, in the order received
 * (docs/protocol.md §3). Events caused by a request are sent after its response.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S0): msg_split() the payload (MAX_FIELDS); look up COMMAND in a table
 *           { name, allowed states C/L/Q/G, handler }; unknown -> 400|<COMMAND>|unknown_command.
 * TODO(S0): state rule (§4): command not allowed in the current state ->
 *           401|CMD|not_logged_in if not logged in, otherwise 403|CMD|forbidden_state.
 * TODO(S0): PING (any state) -> 200|PING.
 * TODO(S1): REGISTER, LOGIN, LOGOUT (auth.c).
 * TODO(S2): LIST_PLAYERS, CHALLENGE, CHALLENGE_REPLY, CHALLENGE_CANCEL (lobby.c).
 * TODO(S3): MOVE (match.c).
 * TODO(S4): RESIGN, DRAW_OFFER, DRAW_REPLY, REMATCH, REMATCH_REPLY, LEAVE_GAME (match.c).
 * TODO(S6): HISTORY, REPLAY, LEADERBOARD, STATS.
 * TODO(S7): FIND_MATCH, CANCEL_MATCH (lobby.c).
 * TODO(S8): wrong field count or non-numeric field -> 400 bad_args, never crash.
 */
