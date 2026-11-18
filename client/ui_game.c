/*
 * client/ui_game.c — game page: board, turn, controls, result.
 * Owner: Nghĩa.
 *
 * Rule: never draw a move before the server answers 200|MOVE.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C3): side panel: both names + ELO, my symbol, "Lượt của bạn" / "Đợi đối thủ".
 * TODO(C3): on_cell: ignore if not my turn, cell taken or a MOVE is pending; else send
 *           MOVE|gid|row|col and mark pending; place the mark only on 200|MOVE;
 *           430–433 -> short message, clear pending.
 * TODO(C3): OPP_MOVE (check gid) -> place the opponent's mark, my turn.
 * TODO(C3): GAME_END -> lock the board, highlight the winning line, result dialog
 *           (Thắng/Thua/Hòa + reason in Vietnamese + new ELO ±delta), update app's ELO.
 * TODO(C4): buttons Xin thua (confirm -> RESIGN), Cầu hòa (DRAW_OFFER), Đấu lại (after GAME_END
 *           -> REMATCH), Về sảnh (LEAVE_GAME, confirm if the game is still running).
 * TODO(C4): DRAW_OFFERED / REMATCH_REQUESTED -> Accept/Decline dialogs; DRAW_DECLINED /
 *           REMATCH_DECLINED -> notice; GAME_START here (rematch) -> clear board, new symbol;
 *           OPP_LEFT -> "Đối thủ đã rời", disable Đấu lại.
 * TODO(C4): reasons FIVE, BOARD_FULL, RESIGN, DRAW_AGREED, TIMEOUT, DISCONNECT in Vietnamese.
 * TODO(C5): countdown of the current move (g_timeout_add_seconds 1), reset after every move;
 *           display only, the server decides TIMEOUT.
 */
