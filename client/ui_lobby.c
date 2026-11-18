/*
 * client/ui_lobby.c — lobby page: players, challenges, quick match, leaderboard.
 * Owner: Nghĩa.
 *
 * Dialogs: GtkMessageDialog + "response" signal, never gtk_dialog_run().
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C2): top bar "<username> · ELO <elo> · W/L/D" + Logout (LOGOUT -> login, net_close()).
 * TODO(C2): GtkTreeView + GtkListStore (name, ELO, status), filled by LIST_PLAYERS on entry;
 *           PLAYER_STATUS events add/update rows, OFFLINE removes the row.
 * TODO(C2): Challenge button only for an IDLE row -> CHALLENGE|user|15|30; 200 -> "waiting"
 *           dialog with Cancel (CHALLENGE_CANCEL|cid); 404/409 -> Vietnamese message.
 * TODO(C2): CHALLENGE_FROM -> one Accept/Decline dialog per cid (CHALLENGE_REPLY);
 *           CHALLENGE_DECLINED / _EXPIRED / _CANCELLED -> close that cid's dialog + short notice.
 * TODO(C2): GAME_START -> store the game in app, close every challenge dialog, show game page.
 * TODO(C4): after LEAVE_GAME: back here, LIST_PLAYERS again, refresh the ELO bar.
 * TODO(C5): "Lịch sử" button; "Bảng xếp hạng" (LEADERBOARD|10 dialog, own row in bold);
 *           "Tìm trận nhanh" (FIND_MATCH -> "searching" dialog with Cancel = CANCEL_MATCH;
 *           GAME_START closes it).
 */
