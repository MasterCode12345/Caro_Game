/*
 * server/db.c — SQLite storage: users, matches, moves.
 * Server only; the client never opens the database.
 * Owner: Hân.
 *
 * All SQL uses sqlite3_prepare_v2 + sqlite3_bind_*. Never build SQL from user strings.
 */
#include "db.h"

#include <stdio.h>
#include <string.h>

#define ELO_START 1000

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
    if (db != NULL) {
        sqlite3_close(db);
        db = NULL;
    }
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