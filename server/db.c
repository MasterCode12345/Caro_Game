/*
 * server/db.c — SQLite storage: users, matches, moves. Server only; the client never opens
 * the database. Linked with -lsqlite3.
 * Owner: Hân.
 *
 * All SQL uses sqlite3_prepare_v2 + sqlite3_bind_*. Never build SQL from user strings.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S1): open the database file, create the tables if missing; users holds the username
 *           (unique), salt, password hash, elo (starts at ELO_START), wins, losses, draws.
 * TODO(S1): create a user; find a user by username.
 * TODO(S5): store a finished match (players, size, result, reason, end time) and its moves in
 *           play order; update both players' elo and W/L/D. Data survives a server restart.
 * TODO(S6): HISTORY (caller's games, newest first, HISTORY_PAGE_SIZE per page), REPLAY (moves in
 *           play order), LEADERBOARD (ELO desc then username, LEADERBOARD_DEFAULT /
 *           LEADERBOARD_MAX), STATS.
 */
