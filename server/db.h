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

#endif /* CARO_SERVER_DB_H */