/*
 * server/protocol.c — command table, state checks, dispatch to handlers.
 * Owner: Hân.
 *
 * The server answers every request with exactly one response, in the order received
 * (docs/protocol.md §3). Events caused by a request are sent after its response.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S0): msg_split() the payload (MAX_FIELDS); look up COMMAND in a table
 *           { name, allowed states C/L/Q/G, handler }; unknown -> 400|<COMMAND>|unknown_command.
 * TODO(S0): state rule (§4): command not allowed in the current state ->
 *           401|CMD|not_logged_in if not logged in, otherwise 403|CMD|forbidden_state.
 * TODO(S0): PING (any state) -> 200|PING.
 * TODO(S1): REGISTER, LOGIN, LOGOUT (auth.c).
 * TODO(S2): LIST_PLAYERS, CHALLENGE, CHALLENGE_REPLY, CHALLENGE_CANCEL (lobby.c).
 * TODO(S3): MOVE (match.c).
 * TODO(S4): RESIGN, DRAW_OFFER, DRAW_REPLY, REMATCH, REMATCH_REPLY, LEAVE_GAME (match.c).
 * TODO(S6): HISTORY, REPLAY, LEADERBOARD, STATS.
 * TODO(S7): FIND_MATCH, CANCEL_MATCH (lobby.c).
 * TODO(S8): wrong field count or non-numeric field -> 400 bad_args, never crash.
 */
/*
 * server/protocol.c — protocol command dispatch.
 * Owner: Hân.
 *
 * S0: command table and PING handler.
 */
#include "protocol.h"
#include "net.h"

#include <string.h>

/* State permission masks. */
#define STATE_BIT(s) (1U << (s))
#define ALL_STATES (STATE_BIT(SESSION_CONNECTED) | \
                   STATE_BIT(SESSION_LOBBY) | \
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

/* S0 handler: PING -> 200|PING */
static void handle_ping(Client *c, char *fields[], int nfields)
{
    (void)fields;
    (void)nfields;

    reply(c, 200, "PING", NULL);
}

/* S0: only PING is registered. */
static const Command commands[] = {
    { "PING", ALL_STATES, 1, 1, handle_ping }
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