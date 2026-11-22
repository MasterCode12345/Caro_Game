/*
 * server/protocol.c — protocol command dispatch.
 * Owner: Hân.
 *
 * S2: LIST_PLAYERS, CHALLENGE, CHALLENGE_REPLY, CHALLENGE_CANCEL, LEAVE_GAME (stub).
 * S1: REGISTER, LOGIN, LOGOUT.
 * S0: command table, state checks and PING.
 */
#include "protocol.h"
#include "auth.h"
#include "net.h"
#include "lobby.h"
#include "match.h"

#include <string.h>

/* State permission masks. */
#define STATE_BIT(s) (1U << (s))
#define ALL_STATES (STATE_BIT(SESSION_CONNECTED) | \
                   STATE_BIT(SESSION_LOBBY) | \
                   STATE_BIT(SESSION_IN_QUEUE) | \
                   STATE_BIT(SESSION_IN_GAME))

#define AUTH_STATES (STATE_BIT(SESSION_CONNECTED))
#define LOGOUT_STATES (STATE_BIT(SESSION_LOBBY) | \
                      STATE_BIT(SESSION_IN_QUEUE))

#define LOBBY_ONLY    (STATE_BIT(SESSION_LOBBY))
#define IN_GAME_ONLY  (STATE_BIT(SESSION_IN_GAME))
#define LOGGED_IN     (STATE_BIT(SESSION_LOBBY) | \
                      STATE_BIT(SESSION_IN_QUEUE) | \
                      STATE_BIT(SESSION_IN_GAME))

typedef void (*CommandHandler)(Client *c, char *fields[], int nfields);

typedef struct {
    const char *name;
    unsigned int states;
    int min_fields;
    int max_fields;
    CommandHandler handler;
} Command;

/* PING -> 200|PING */
static void handle_ping(Client *c, char *fields[], int nfields)
{
    (void)fields;
    (void)nfields;

    reply(c, 200, "PING", NULL);
}

/* Command table. Field counts include the command name. */
static const Command commands[] = {
    { "PING",     ALL_STATES,    1, 1, handle_ping },
    { "REGISTER", AUTH_STATES,   3, 3, handle_register },
    { "LOGIN",    AUTH_STATES,   3, 3, handle_login },
    { "LOGOUT",   LOGOUT_STATES, 1, 1, handle_logout },
    { CMD_LIST_PLAYERS,     LOGGED_IN,    1, 1, handle_list_players },
    { CMD_CHALLENGE,        LOBBY_ONLY,   4, 4, handle_challenge },
    { CMD_CHALLENGE_REPLY,  LOBBY_ONLY,   3, 3, handle_challenge_reply },
    { CMD_CHALLENGE_CANCEL, LOBBY_ONLY,   2, 2, handle_challenge_cancel },
    { CMD_MOVE,             IN_GAME_ONLY, 4, 4, handle_move },
    { CMD_LEAVE_GAME,       IN_GAME_ONLY, 2, 2, handle_leave_game }
};

void handle_message(Client *c, char *msg)
{
    if (c == NULL || msg == NULL || c->fd < 0)
        return;

    char *fields[MAX_FIELDS];
    int nfields = msg_split(msg, fields, MAX_FIELDS);

    if (nfields <= 0)
        return;

    const char *cmd_name = fields[0];

    const Command *cmd = NULL;

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            cmd = &commands[i];
            break;
        }
    }

    if (cmd == NULL) {
        reply(c, 400, cmd_name, "unknown_command");
        return;
    }

    if (nfields < cmd->min_fields || nfields > cmd->max_fields) {
        reply(c, 400, cmd->name, "bad_args");
        return;
    }

    if (!(cmd->states & STATE_BIT(c->state))) {
        if (c->state == SESSION_CONNECTED) {
            reply(c, 401, cmd->name, "not_logged_in");
        } else {
            reply(c, 403, cmd->name, "forbidden_state");
        }
        return;
    }

    cmd->handler(c, fields, nfields);
}