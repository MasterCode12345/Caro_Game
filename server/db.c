/*
 * server/db.c — SQLite storage: users, matches, moves.
 * Server only; the client never opens the database.
 * Owner: Hân.
 *
 * All SQL uses sqlite3_prepare_v2 + sqlite3_bind_*. Never build SQL from user strings.
 */
#include "db.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Single SQLite connection: the server uses one select() loop. */
static sqlite3 *db = NULL;

/* Execute a SQL statement without parameters. */
static int db_exec(const char *sql)
{
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] SQL error: %s\n",
                err_msg ? err_msg : sqlite3_errmsg(db));
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

/* Open database and create tables if they do not exist. */
int db_open(const char *path)
{
    if (db != NULL) {
        db_close();
    }

    int rc = sqlite3_open(path, &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Cannot open database: %s\n",
                db ? sqlite3_errmsg(db) : "allocation failed");

        if (db != NULL) {
            sqlite3_close(db);
            db = NULL;
        }

        return -1;
    }

    /* Enable foreign key enforcement. */
    if (db_exec("PRAGMA foreign_keys = ON;") != 0) {
        db_close();
        return -1;
    }

    /* Create schema. */
    const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT UNIQUE NOT NULL,"
        " pass_hash TEXT NOT NULL,"
        " salt TEXT NOT NULL,"
        " elo INTEGER NOT NULL DEFAULT 1000,"
        " wins INTEGER NOT NULL DEFAULT 0,"
        " losses INTEGER NOT NULL DEFAULT 0,"
        " draws INTEGER NOT NULL DEFAULT 0,"
        " created_at TEXT DEFAULT (datetime('now','localtime'))"
        ");"

        "CREATE TABLE IF NOT EXISTS matches ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " x_user INTEGER NOT NULL REFERENCES users(id),"
        " o_user INTEGER NOT NULL REFERENCES users(id),"
        " board_size INTEGER,"
        " sec_per_move INTEGER,"
        " result TEXT,"
        " reason TEXT,"
        " elo_delta_x INTEGER,"
        " elo_delta_o INTEGER,"
        " started_at TEXT,"
        " ended_at TEXT"
        ");"

        "CREATE TABLE IF NOT EXISTS moves ("
        " match_id INTEGER NOT NULL REFERENCES matches(id),"
        " move_no INTEGER NOT NULL,"
        " symbol TEXT NOT NULL,"
        " row INTEGER NOT NULL,"
        " col INTEGER NOT NULL,"
        " PRIMARY KEY(match_id, move_no)"
        ");";

    if (db_exec(schema) != 0) {
        db_close();
        return -1;
    }

    fprintf(stderr, "[DB] Database opened successfully.\n");
    return 0;
}

/* Close database connection. */
void db_close(void)
{
    if (db == NULL)
        return;

    /* SQLITE_BUSY here means a statement was not finalized: a leak in this file. */
    if (sqlite3_close(db) != SQLITE_OK)
        fprintf(stderr, "[DB] Close failed: %s\n", sqlite3_errmsg(db));

    db = NULL;
}

/* Create a new user. */
int db_user_create(const char *username, const char *salt, const char *hash)
{
    if (db == NULL || username == NULL || salt == NULL || hash == NULL) {
        return -1;
    }

    const char *sql =
        "INSERT INTO users (username, pass_hash, salt, elo, wins, losses, draws)"
        " VALUES (?, ?, ?, ?, 0, 0, 0);";

    sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare user insert failed: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, salt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, ELO_START);

    rc = sqlite3_step(stmt);

    int result;

    if (rc == SQLITE_DONE) {
        result = 0;
    } else if (rc == SQLITE_CONSTRAINT ||
               rc == SQLITE_CONSTRAINT_UNIQUE ||
               rc == SQLITE_CONSTRAINT_PRIMARYKEY) {
        result = DB_EXISTS;
    } else {
        fprintf(stderr, "[DB] Insert user failed: %s\n",
                sqlite3_errmsg(db));
        result = -1;
    }

    sqlite3_finalize(stmt);
    return result;
}

/* Retrieve user by username. */
int db_user_get(const char *username, UserRow *out)
{
    if (db == NULL || username == NULL || out == NULL) {
        return -1;
    }

    const char *sql =
        "SELECT id, username, pass_hash, salt, elo, wins, losses, draws"
        " FROM users WHERE username = ?;";

    sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare user query failed: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0; /* User not found */
    }

    if (rc != SQLITE_ROW) {
        fprintf(stderr, "[DB] Query user failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    out->id = sqlite3_column_int(stmt, 0);

    const unsigned char *text;

    text = sqlite3_column_text(stmt, 1);
    snprintf(out->username, sizeof(out->username), "%s",
             text ? (const char *)text : "");

    text = sqlite3_column_text(stmt, 2);
    snprintf(out->pass_hash, sizeof(out->pass_hash), "%s",
             text ? (const char *)text : "");

    text = sqlite3_column_text(stmt, 3);
    snprintf(out->salt, sizeof(out->salt), "%s",
             text ? (const char *)text : "");

    out->elo = sqlite3_column_int(stmt, 4);
    out->wins = sqlite3_column_int(stmt, 5);
    out->losses = sqlite3_column_int(stmt, 6);
    out->draws = sqlite3_column_int(stmt, 7);

    sqlite3_finalize(stmt);
    return 1; /* User found */
}

/* ---------------------------------------------------------------------------------------
 * Finished matches
 * ------------------------------------------------------------------------------------- */

/* t as local time 'YYYY-MM-DD HH:MM:SS' (19 characters + '\0'; cap must be >= 20). */
static void format_local_time(time_t t, char *out, size_t cap)
{
    struct tm tm_local;

    if (localtime_r(&t, &tm_local) == NULL ||
        strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm_local) == 0)
        snprintf(out, cap, "1970-01-01 00:00:00");
}

/* Add the result of one game to one user: elo += d_elo and one of wins / losses / draws += 1.
 * Relative updates, so the row is right even if it changed since the user logged in. */
static int user_apply_result(int user_id, int d_elo, int d_win, int d_loss, int d_draw)
{
    const char *sql =
        "UPDATE users SET elo = elo + ?, wins = wins + ?, losses = losses + ?,"
        " draws = draws + ? WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare user update failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, d_elo);
    sqlite3_bind_int(stmt, 2, d_win);
    sqlite3_bind_int(stmt, 3, d_loss);
    sqlite3_bind_int(stmt, 4, d_draw);
    sqlite3_bind_int(stmt, 5, user_id);

    int ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1);

    if (!ok)
        fprintf(stderr, "[DB] Update user %d failed: %s\n", user_id, sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    return ok ? 0 : -1;
}

int db_match_save(int x_user, int o_user, int board_size, int sec_per_move,
                  const char *result, const char *reason,
                  int delta_x, int delta_o,
                  time_t started_at, time_t ended_at,
                  const int moves[][2], int move_count)
{
    if (db == NULL || result == NULL || reason == NULL || move_count < 0 ||
        (move_count > 0 && moves == NULL))
        return -1;

    /* Who gained what: X_WIN / O_WIN / DRAW decide wins, losses and draws of both users. */
    int x_win = 0, x_loss = 0, draw = 0;

    if (strcmp(result, RES_X_WIN) == 0)
        x_win = 1;
    else if (strcmp(result, RES_O_WIN) == 0)
        x_loss = 1;
    else if (strcmp(result, RES_DRAW) == 0)
        draw = 1;
    else
        return -1;

    char started[20], ended[20];

    format_local_time(started_at, started, sizeof(started));
    format_local_time(ended_at, ended, sizeof(ended));

    if (db_exec("BEGIN;") != 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 match_id;

    /* 1. the match row */
    const char *sql_match =
        "INSERT INTO matches (x_user, o_user, board_size, sec_per_move, result, reason,"
        " elo_delta_x, elo_delta_o, started_at, ended_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, sql_match, -1, &stmt, NULL) != SQLITE_OK)
        goto fail;

    sqlite3_bind_int(stmt, 1, x_user);
    sqlite3_bind_int(stmt, 2, o_user);
    sqlite3_bind_int(stmt, 3, board_size);
    sqlite3_bind_int(stmt, 4, sec_per_move);
    sqlite3_bind_text(stmt, 5, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, delta_x);
    sqlite3_bind_int(stmt, 8, delta_o);
    sqlite3_bind_text(stmt, 9, started, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, ended, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        goto fail;

    sqlite3_finalize(stmt);
    stmt = NULL;
    match_id = sqlite3_last_insert_rowid(db);

    /* 2. every move: X plays the odd move numbers, O the even ones */
    const char *sql_move =
        "INSERT INTO moves (match_id, move_no, symbol, row, col) VALUES (?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, sql_move, -1, &stmt, NULL) != SQLITE_OK)
        goto fail;

    for (int i = 0; i < move_count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, match_id);
        sqlite3_bind_int(stmt, 2, i + 1);
        sqlite3_bind_text(stmt, 3, (i % 2 == 0) ? "X" : "O", -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, moves[i][0]);
        sqlite3_bind_int(stmt, 5, moves[i][1]);

        if (sqlite3_step(stmt) != SQLITE_DONE)
            goto fail;
    }

    sqlite3_finalize(stmt);
    stmt = NULL;

    /* 3. elo and win / loss / draw counters of both players */
    if (user_apply_result(x_user, delta_x, x_win, x_loss, draw) != 0 ||
        user_apply_result(o_user, delta_o, x_loss, x_win, draw) != 0)
        goto fail;

    if (db_exec("COMMIT;") != 0)
        goto fail;

    return (int)match_id;

fail:
    fprintf(stderr, "[DB] Saving match failed: %s\n", sqlite3_errmsg(db));

    if (stmt != NULL)
        sqlite3_finalize(stmt);

    db_exec("ROLLBACK;");
    return -1;
}

/* ---------------------------------------------------------------------------------------
 * HISTORY, REPLAY, LEADERBOARD (read only)
 * ------------------------------------------------------------------------------------- */

/* Copy a column text into a fixed buffer (NULL -> empty string, too long -> cut). */
static void copy_column(char *dst, size_t cap, sqlite3_stmt *stmt, int col)
{
    const unsigned char *text = sqlite3_column_text(stmt, col);

    snprintf(dst, cap, "%s", text ? (const char *)text : "");
}

int db_history(int user_id, int page, HistoryRow *out, int max_rows)
{
    if (db == NULL || out == NULL || page < 1 || max_rows < 1)
        return -1;

    /* ids grow with every saved match, so id DESC is "newest first". */
    const char *sql =
        "SELECT m.id, m.x_user, ux.username, uo.username, m.result, m.reason,"
        " substr(m.ended_at, 1, 16)"
        " FROM matches m"
        " JOIN users ux ON ux.id = m.x_user"
        " JOIN users uo ON uo.id = m.o_user"
        " WHERE m.x_user = ?1 OR m.o_user = ?1"
        " ORDER BY m.id DESC LIMIT ?2 OFFSET ?3;";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare history failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, max_rows);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)(page - 1) * max_rows);

    int n = 0;
    int rc = SQLITE_DONE;

    while (n < max_rows && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        HistoryRow *row = &out[n];
        int i_am_x = (sqlite3_column_int(stmt, 1) == user_id);
        char result[8];

        row->match_id = sqlite3_column_int(stmt, 0);
        copy_column(row->opponent, sizeof(row->opponent), stmt, i_am_x ? 3 : 2);
        snprintf(row->my_symbol, sizeof(row->my_symbol), "%s", i_am_x ? SYM_X : SYM_O);
        copy_column(result, sizeof(result), stmt, 4);
        copy_column(row->reason, sizeof(row->reason), stmt, 5);
        copy_column(row->ended_at, sizeof(row->ended_at), stmt, 6);

        /* X_WIN is a win for X and a loss for O; O_WIN the other way round. */
        if (strcmp(result, RES_DRAW) == 0)
            snprintf(row->outcome, sizeof(row->outcome), "%s", OUT_DRAW);
        else
            snprintf(row->outcome, sizeof(row->outcome), "%s",
                     ((strcmp(result, RES_X_WIN) == 0) == i_am_x) ? OUT_WIN : OUT_LOSS);

        n++;
    }

    if (n < max_rows && rc != SQLITE_DONE) {
        fprintf(stderr, "[DB] Query history failed: %s\n", sqlite3_errmsg(db));
        n = -1;
    }

    sqlite3_finalize(stmt);
    return n;
}

int db_replay_header(int match_id, ReplayHeader *out)
{
    if (db == NULL || out == NULL)
        return -1;

    const char *sql =
        "SELECT m.id, m.board_size, ux.username, uo.username, m.result, m.reason"
        " FROM matches m"
        " JOIN users ux ON ux.id = m.x_user"
        " JOIN users uo ON uo.id = m.o_user"
        " WHERE m.id = ?;";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare replay header failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, match_id);

    int rc = sqlite3_step(stmt);
    int found = -1;

    if (rc == SQLITE_ROW) {
        out->match_id = sqlite3_column_int(stmt, 0);
        out->board_size = sqlite3_column_int(stmt, 1);
        copy_column(out->x_user, sizeof(out->x_user), stmt, 2);
        copy_column(out->o_user, sizeof(out->o_user), stmt, 3);
        copy_column(out->result, sizeof(out->result), stmt, 4);
        copy_column(out->reason, sizeof(out->reason), stmt, 5);
        found = 1;
    } else if (rc == SQLITE_DONE) {
        found = 0;
    } else {
        fprintf(stderr, "[DB] Query replay header failed: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    return found;
}

int db_replay_moves(int match_id, int moves[][2], int max_moves)
{
    if (db == NULL || moves == NULL || max_moves < 1)
        return -1;

    const char *sql = "SELECT row, col FROM moves WHERE match_id = ? ORDER BY move_no;";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare replay moves failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, match_id);

    int n = 0;
    int rc;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (n >= max_moves) {           /* more moves than any board can hold: bad data */
            sqlite3_finalize(stmt);
            return -1;
        }
        moves[n][0] = sqlite3_column_int(stmt, 0);
        moves[n][1] = sqlite3_column_int(stmt, 1);
        n++;
    }

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[DB] Query replay moves failed: %s\n", sqlite3_errmsg(db));
        n = -1;
    }

    sqlite3_finalize(stmt);
    return n;
}

int db_leaderboard(int n, RankRow *out)
{
    if (db == NULL || out == NULL || n < 1)
        return -1;

    const char *sql =
        "SELECT username, elo, wins, losses, draws FROM users"
        " ORDER BY elo DESC, username ASC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] Prepare leaderboard failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, n);

    int k = 0;
    int rc = SQLITE_DONE;

    while (k < n && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        copy_column(out[k].username, sizeof(out[k].username), stmt, 0);
        out[k].elo = sqlite3_column_int(stmt, 1);
        out[k].wins = sqlite3_column_int(stmt, 2);
        out[k].losses = sqlite3_column_int(stmt, 3);
        out[k].draws = sqlite3_column_int(stmt, 4);
        k++;
    }

    if (k < n && rc != SQLITE_DONE) {
        fprintf(stderr, "[DB] Query leaderboard failed: %s\n", sqlite3_errmsg(db));
        k = -1;
    }

    sqlite3_finalize(stmt);
    return k;
}
