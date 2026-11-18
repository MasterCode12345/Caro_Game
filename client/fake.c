/*
 * client/fake.c — offline mode, CARO_FAKE=1 ./build/caro_client: a scripted server so the UI
 * can be built before the real server feature exists.
 * Owner: Nghĩa.
 *
 * Fake replies and events follow docs/protocol.md exactly and go through the same dispatch()
 * path as real messages (g_idle_add / g_timeout_add), never straight into the UI.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C1): PING; REGISTER (name "taken" -> 409); LOGIN (password "wrong" -> 401); LOGOUT.
 * TODO(C2): LIST_PLAYERS (3 fake players); PLAYER_STATUS of a new player after 3 s;
 *           CHALLENGE -> 200 then GAME_START after 2 s; a way to simulate CHALLENGE_FROM.
 * TODO(C3): MOVE -> 200 then OPP_MOVE on a random empty cell after 0.5 s; simple five-in-a-row
 *           check -> GAME_END.
 * TODO(C4): RESIGN, DRAW_OFFER/DRAW_REPLY, REMATCH/REMATCH_REPLY, LEAVE_GAME — enough to
 *           press every button.
 * TODO(C5): HISTORY, REPLAY (one 20-move game), LEADERBOARD, FIND_MATCH / CANCEL_MATCH.
 */
