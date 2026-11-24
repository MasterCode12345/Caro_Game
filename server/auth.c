/*
 * server/auth.c — REGISTER, LOGIN, LOGOUT.
 * Owner: Hân.
 *
 * Never log or store a plain-text password.
 */
#include "auth.h"
#include "db.h"
#include "sha256.h"
#include "lobby.h"
#include "net.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Validate username: 3-20 characters, letters, digits or underscore. */
static int valid_username(const char *username)
{
    if (username == NULL)
        return 0;

    size_t len = strlen(username);

    if (len < USERNAME_MIN || len > USERNAME_MAX)
        return 0;

    for (size_t i = 0; i < len; i++) {
        char ch = username[i];

        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_')) {
            return 0;
        }
    }

    return 1;
}

/* Validate password: printable ASCII except '|', length 6-32. */
static int valid_password(const char *password)
{
    if (password == NULL)
        return 0;

    size_t len = strlen(password);

    if (len < PASSWORD_MIN || len > PASSWORD_MAX)
        return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)password[i];

        if (ch < 0x21 || ch > 0x7E || ch == FIELD_SEP)
            return 0;
    }

    return 1;
}

/* Generate 16 random bytes from /dev/urandom and encode as 32 hex chars. */
static int generate_salt(char salt[33])
{
    unsigned char bytes[16];
    FILE *fp = fopen("/dev/urandom", "rb");

    if (fp == NULL)
        return -1;

    size_t count = fread(bytes, 1, sizeof(bytes), fp);

    if (fclose(fp) != 0 || count != sizeof(bytes))
        return -1;

    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < sizeof(bytes); i++) {
        salt[i * 2]     = hex[bytes[i] >> 4];
        salt[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }

    salt[32] = '\0';
    return 0;
}

/* Hash salt_hex + password using SHA-256. */
static int password_hash(const char *salt, const char *password,
                         char hash[65])
{
    if (salt == NULL || password == NULL)
        return -1;

    char input[33 + PASSWORD_MAX];
    int len = snprintf(input, sizeof(input), "%s%s", salt, password);

    if (len < 0 || (size_t)len >= sizeof(input))
        return -1;

    sha256_hex((const unsigned char *)input, (size_t)len, hash);
    return 0;
}

/* Compare hash strings without early exit on the first mismatch. */
static int hash_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return 0;

    if (strlen(a) != 64 || strlen(b) != 64)
        return 0;

    unsigned char diff = 0;

    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];

    return diff == 0;
}

/* Check whether another client is already logged in with this username. */
static int username_online(const Client *exclude, const char *username)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *other = &clients[i];

        if (other == exclude || other->fd < 0)
            continue;

        if (other->state != SESSION_LOBBY &&
            other->state != SESSION_IN_QUEUE &&
            other->state != SESSION_IN_GAME)
            continue;

        if (strcmp(other->username, username) == 0)
            return 1;
    }

    return 0;
}

/* REGISTER|username|password */
void handle_register(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    const char *username = fields[1];
    const char *password = fields[2];

    if (!valid_username(username)) {
        reply(c, ST_BAD_REQUEST, CMD_REGISTER, ERR_INVALID_USERNAME);
        return;
    }

    if (!valid_password(password)) {
        reply(c, ST_BAD_REQUEST, CMD_REGISTER, ERR_INVALID_PASSWORD);
        return;
    }

    char salt[33];
    char hash[65];

    if (generate_salt(salt) != 0 ||
        password_hash(salt, password, hash) != 0) {
        reply(c, ST_SERVER_ERROR, CMD_REGISTER, ERR_SERVER_ERROR);
        return;
    }

    int result = db_user_create(username, salt, hash);

    if (result == DB_EXISTS) {
        reply(c, ST_CONFLICT, CMD_REGISTER, ERR_USERNAME_TAKEN);
        return;
    }

    if (result != 0) {
        reply(c, ST_SERVER_ERROR, CMD_REGISTER, ERR_SERVER_ERROR);
        return;
    }

    reply(c, ST_CREATED, CMD_REGISTER, NULL);
}

/* LOGIN|username|password */
void handle_login(Client *c, char *fields[], int nfields)
{
    (void)nfields;

    const char *username = fields[1];
    const char *password = fields[2];

    if (!valid_username(username) || !valid_password(password)) {
        reply(c, ST_NOT_AUTHENTICATED, CMD_LOGIN,
              ERR_INVALID_CREDENTIALS);
        return;
    }

    if (username_online(c, username)) {
        reply(c, ST_CONFLICT, CMD_LOGIN, ERR_ALREADY_LOGGED_IN);
        return;
    }

    UserRow user;

    int result = db_user_get(username, &user);

    if (result < 0) {                   /* database error: not the user's fault */
        reply(c, ST_SERVER_ERROR, CMD_LOGIN, ERR_SERVER_ERROR);
        return;
    }

    if (result == 0) {
        reply(c, ST_NOT_AUTHENTICATED, CMD_LOGIN,
              ERR_INVALID_CREDENTIALS);
        return;
    }

    char hash[65];

    if (password_hash(user.salt, password, hash) != 0) {
        reply(c, ST_SERVER_ERROR, CMD_LOGIN, ERR_SERVER_ERROR);
        return;
    }

    if (!hash_equal(hash, user.pass_hash)) {
        reply(c, ST_NOT_AUTHENTICATED, CMD_LOGIN,
              ERR_INVALID_CREDENTIALS);
        return;
    }

    /* Update the client's session. */
    c->user_id = user.id;
    snprintf(c->username, sizeof(c->username), "%s", user.username);
    c->elo = user.elo;
    c->wins = user.wins;
    c->losses = user.losses;
    c->draws = user.draws;
    c->state = SESSION_LOBBY;

    /* Response before the event (protocol.md section 3). If this send fails, session_end() has
     * already reset c: broadcasting now would send PLAYER_STATUS with an empty name. */
    if (reply(c, ST_OK, CMD_LOGIN, "%s|%d|%d|%d|%d",
              c->username, c->elo, c->wins, c->losses, c->draws) < 0)
        return;

    broadcast_status(c, c->username, PS_IDLE, c->elo);
}

/* LOGOUT (LOBBY or IN_QUEUE) */
void handle_logout(Client *c, char *fields[], int nfields)
{
    (void)fields;
    (void)nfields;

    char username[USERNAME_MAX + 1];
    int elo = c->elo;

    snprintf(username, sizeof(username), "%s", c->username);

    /* Response before the events. If this send fails, session_end() has done everything below. */
    if (reply(c, ST_OK, CMD_LOGOUT, NULL) < 0)
        return;

    /* Pending challenges involving this player are cancelled (protocol.md 6.2). A failed send to
     * the other side can end sessions, c's own included: then there is nothing left to do. */
    challenges_drop_for(c);

    if (c->fd < 0)
        return;

    c->state = SESSION_CONNECTED;
    c->user_id = 0;
    c->username[0] = '\0';
    c->elo = 0;
    c->wins = 0;
    c->losses = 0;
    c->draws = 0;
    c->game = NULL;
    c->queued_at = 0;

    broadcast_status(c, username, PS_OFFLINE, elo);
}
