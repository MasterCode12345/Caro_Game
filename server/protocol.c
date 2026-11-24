/*
 * server/protocol.c — protocol command dispatch.
 * Owner: Hân.
 *
 * handle_message() splits one request, finds the command in the table, checks the sender's
 * state, then the number of fields, and calls the handler. The handlers live in auth.c,
 * lobby.c, match.c and history.c; they check the values of the fields.
 */
#include "protocol.h"
#include "auth.h"
#include "net.h"
#include "lobby.h"
#include "match.h"
#include "history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* State permission masks. */
#define STATE_BIT(s) (1U << (s))
#define ALL_STATES (STATE_BIT(SESSION_CONNECTED) | \
                   STATE_BIT(SESSION_LOBBY) | \
                   STATE_BIT(SESSION_IN_QUEUE) | \
                   STATE_BIT(SESSION_IN_GAME))

#define AUTH_STATES (STATE_BIT(SESSION_CONNECTED))
#define LOBBY_OR_QUEUE (STATE_BIT(SESSION_LOBBY) | \
                       STATE_BIT(SESSION_IN_QUEUE))
#define LOBBY_ONLY    (STATE_BIT(SESSION_LOBBY))
#define QUEUE_ONLY    (STATE_BIT(SESSION_IN_QUEUE))
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

    reply(c, ST_OK, CMD_PING, NULL);
}

/* Command table. Field counts include the command name. */
static const Command commands[] = {
    { CMD_PING,             ALL_STATES,   1, 1, handle_ping },
    { CMD_REGISTER,         AUTH_STATES,  3, 3, handle_register },
    { CMD_LOGIN,            AUTH_STATES,  3, 3, handle_login },
    { CMD_LOGOUT,           LOBBY_OR_QUEUE, 1, 1, handle_logout },
    { CMD_LIST_PLAYERS,     LOGGED_IN,    1, 1, handle_list_players },
    { CMD_CHALLENGE,        LOBBY_ONLY,   4, 4, handle_challenge },
    { CMD_CHALLENGE_REPLY,  LOBBY_ONLY,   3, 3, handle_challenge_reply },
    { CMD_CHALLENGE_CANCEL, LOBBY_ONLY,   2, 2, handle_challenge_cancel },
    { CMD_FIND_MATCH,       LOBBY_ONLY,   1, 1, handle_find_match },
    { CMD_CANCEL_MATCH,     QUEUE_ONLY,   1, 1, handle_cancel_match },
    { CMD_MOVE,             IN_GAME_ONLY, 4, 4, handle_move },
    { CMD_LEAVE_GAME,       IN_GAME_ONLY, 2, 2, handle_leave_game },
    { CMD_RESIGN,           IN_GAME_ONLY, 2, 2, handle_resign },
    { CMD_DRAW_OFFER,       IN_GAME_ONLY, 2, 2, handle_draw_offer },
    { CMD_DRAW_REPLY,       IN_GAME_ONLY, 3, 3, handle_draw_reply },
    { CMD_REMATCH,          IN_GAME_ONLY, 2, 2, handle_rematch },
    { CMD_REMATCH_REPLY,    IN_GAME_ONLY, 3, 3, handle_rematch_reply },
    { CMD_STATS,            LOBBY_OR_QUEUE, 1, 2, handle_stats },
    { CMD_HISTORY,          LOBBY_OR_QUEUE, 1, 2, handle_history },
    { CMD_REPLAY,           LOBBY_OR_QUEUE, 2, 2, handle_replay },
    { CMD_LEADERBOARD,      LOBBY_OR_QUEUE, 1, 2, handle_leaderboard }
};

/* Longest unknown command that can be echoed: "400|" + command + "|unknown_command" must fit
 * in one frame. sizeof counts the final '\0', hence the + 1. */
#define MAX_UNKNOWN_CMD (FRAME_MAX_PAYLOAD - sizeof("400||" ERR_UNKNOWN_COMMAND) + 1)

/* strtol() alone is too generous: it skips leading spaces and accepts '+'. */
int parse_long(const char *s, long *out)
{
    const char *digits = (s[0] == '-') ? s + 1 : s;

    if (digits[0] < '0' || digits[0] > '9')
        return 0;

    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);

    if (errno != 0 || *end != '\0')     /* too big for a long, or text after the number */
        return 0;

    *out = v;
    return 1;
}

void handle_message(Client *c, char *msg)
{
    if (c == NULL || msg == NULL || c->fd < 0)
        return;

    char *fields[MAX_FIELDS];
    int nfields = msg_split(msg, fields, MAX_FIELDS);

    if (nfields <= 0)
        return;

    char *cmd_name = fields[0];

    const Command *cmd = NULL;

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            cmd = &commands[i];
            break;
        }
    }

    if (cmd == NULL) {
        /* The command is echoed as received (section 3); one that fills the whole frame is cut,
         * because every request must still get its one response. */
        if (strlen(cmd_name) > MAX_UNKNOWN_CMD)
            cmd_name[MAX_UNKNOWN_CMD] = '\0';

        reply(c, ST_BAD_REQUEST, cmd_name, ERR_UNKNOWN_COMMAND);
        return;
    }

    /* State rule (section 4) first, whatever the arguments are: a client that is not logged in
     * learns nothing else about a command. */
    if (!(cmd->states & STATE_BIT(c->state))) {
        if (c->state == SESSION_CONNECTED) {
            reply(c, ST_NOT_AUTHENTICATED, cmd->name, ERR_NOT_LOGGED_IN);
        } else {
            reply(c, ST_FORBIDDEN, cmd->name, ERR_FORBIDDEN_STATE);
        }
        return;
    }

    if (nfields < cmd->min_fields || nfields > cmd->max_fields) {
        reply(c, ST_BAD_REQUEST, cmd->name, ERR_BAD_ARGS);
        return;
    }

    cmd->handler(c, fields, nfields);
}
