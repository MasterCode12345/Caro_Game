/*
 * server/db.h — interface of db.c: SQLite storage (users, matches, moves).
 * Owner: Hân.
 *
 * S1: database initialization and user account storage.
 * S5: finished match storage and ELO / W-L/D update.
 * S6: HISTORY, REPLAY, LEADERBOARD, STATS queries.
 */
#ifndef CARO_SERVER_DB_H
#define CARO_SERVER_DB_H

#include <sqlite3.h>
#include <time.h>
/* Return codes */
#define DB_EXISTS 1

/* User data loaded from database */
typedef struct {
    int id;
    char username[21];
    char pass_hash[65];
    char salt[33];
    int elo;
    int wins;
    int losses;
    int draws;
} UserRow;

/* Open database and initialize schema. Returns 0 on success, -1 on error. */
int db_open(const char *path);

/* Close database connection. */
void db_close(void);

/*
 * Create a user.
 * Returns 0 on success, DB_EXISTS if username already exists, -1 on error.
 */
int db_user_create(const char *username, const char *salt, const char *hash);

/*
 * Retrieve a user by username.
 * Returns 1 if found, 0 if not found, -1 on error.
 */
int db_user_get(const char *username, UserRow *out);
/*
 * Save one finished match in ONE transaction: a row in matches, every move in moves, and the
 * elo / wins / losses / draws of both users (elo changes by delta_x / delta_o).
 * result: RES_X_WIN, RES_O_WIN or RES_DRAW. moves[i] = {row, col}, moves[0] is the first move
 * (X), the symbols alternate. Times are stored as local time 'YYYY-MM-DD HH:MM:SS'.
 * Returns the match id (> 0), or -1 on error; on error nothing is written (ROLLBACK).
 */
int db_match_save(int x_user, int o_user, int board_size, int sec_per_move,
                  const char *result, const char *reason,
                  int delta_x, int delta_o,
                  time_t started_at, time_t ended_at,
                  const int moves[][2], int move_count);
/* ---- S6: read-only queries for HISTORY, REPLAY, LEADERBOARD ---- */

/* One line of HISTORY, from the point of view of the user who asked. */
typedef struct {
    int match_id;
    char opponent[21];
    char my_symbol[2];      /* "X" or "O" */
    char outcome[5];        /* "WIN", "LOSS" or "DRAW" */
    char reason[16];
    char ended_at[17];      /* 'YYYY-MM-DD HH:MM' */
} HistoryRow;

/*
 * The games of user_id, newest first. page starts at 1; max_rows games per page.
 * Fills out[] and returns the number of rows (0 = empty page), or -1 on error.
 */
int db_history(int user_id, int page, HistoryRow *out, int max_rows);

/* The fixed part of REPLAY. */
typedef struct {
    int match_id;
    int board_size;
    char x_user[21];
    char o_user[21];
    char result[8];         /* X_WIN, O_WIN or DRAW */
    char reason[16];
} ReplayHeader;

/* Returns 1 if the match exists, 0 if not, -1 on error. */
int db_replay_header(int match_id, ReplayHeader *out);

/* The moves of a match in play order (X first) as {row, col}. Returns the number of moves,
 * or -1 on error (also when the match has more than max_moves moves). */
int db_replay_moves(int match_id, int moves[][2], int max_moves);

/* One line of LEADERBOARD. */
typedef struct {
    char username[21];
    int elo;
    int wins;
    int losses;
    int draws;
} RankRow;

/* The n best users: elo descending, then username. Returns the number of rows or -1. */
int db_leaderboard(int n, RankRow *out);
#endif /* CARO_SERVER_DB_H */