/*
 * client/dispatch.c — routes every incoming message (real or fake) to the right code.
 * Owner: Nghĩa.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C1): dispatch(msg): msg_split(); first field is a number -> response: pop the head of
 *           the FIFO, check that its COMMAND field matches the queued request (log a warning if
 *           not), call cb(code, fields, n, data).
 * TODO(C1): otherwise -> event: look it up in the table filled by dispatch_on(name, handler);
 *           unknown event -> log and ignore.
 * TODO(C1): CARO_DEBUG=1 -> print every sent/received message to stderr (for the demo).
 * TODO(C2–C5): each page registers its event handlers (PLAYER_STATUS, CHALLENGE_*, GAME_START,
 *           OPP_MOVE, DRAW_*, REMATCH_*, GAME_END, OPP_LEFT).
 */
