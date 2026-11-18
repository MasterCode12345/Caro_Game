/*
 * common/proto.h — constants shared by client/, server/ and tools/.
 *
 * Source of truth: docs/protocol.md (v1.1). Rule: change the doc first, with both members
 * agreeing, then this file (commit prefix "protocol:").
 * Declarations only: macros and types, no code, no variables.
 *
 * Owner: Nghĩa (writes), Nghĩa + Hân (use).
 */
#ifndef CARO_PROTO_H
#define CARO_PROTO_H

/* ------------------------------------------------------------------------------------------
 * 1. Transport and framing (protocol.md §1)
 * ---------------------------------------------------------------------------------------- */
#define DEFAULT_PORT          5500
#define FRAME_MAX_PAYLOAD     16384   /* valid LENGTH: 1..16384; 0 or more is a malformed frame */

/* ------------------------------------------------------------------------------------------
 * 2. Payload format (protocol.md §2)
 * ---------------------------------------------------------------------------------------- */
#define FIELD_SEP             '|'     /* between fields */
#define ITEM_SEP              ','     /* between attributes inside one list item */

/* Maximum number of fields msg_split() returns from one payload.
 * protocol.md does not fix this value; it is sized from the longest messages:
 *   REPLAY on 15x15  = 9 fixed fields + up to 225 moves = 234  (19x19, reserved: 370)
 *   LIST_PLAYERS     = 3 fixed fields + one per other player; the server's select() loop
 *                      is capped by FD_SETSIZE (1024 on Linux and macOS) connections.
 * 1024 pointers = 8 KB on the stack, which is fine. */
#define MAX_FIELDS            1024

#define USERNAME_MIN          3       /* [A-Za-z0-9_] */
#define USERNAME_MAX          20
#define PASSWORD_MIN          6       /* printable ASCII 0x21..0x7E except '|' */
#define PASSWORD_MAX          32

/* ------------------------------------------------------------------------------------------
 * 3. Status codes (protocol.md §4). A response's first field is one of these.
 * ---------------------------------------------------------------------------------------- */
#define ST_OK                 200
#define ST_CREATED            201     /* REGISTER */
#define ST_BAD_REQUEST        400
#define ST_NOT_AUTHENTICATED  401
#define ST_FORBIDDEN          403
#define ST_NOT_FOUND          404
#define ST_CONFLICT           409
#define ST_NOT_YOUR_TURN      430
#define ST_OUT_OF_BOARD       431
#define ST_CELL_OCCUPIED      432
#define ST_GAME_NOT_ACTIVE    433
#define ST_SERVER_ERROR       500

/* Reason tokens: the third field of an error response (protocol.md §4). */
#define ERR_UNKNOWN_COMMAND       "unknown_command"
#define ERR_BAD_ARGS              "bad_args"
#define ERR_INVALID_USERNAME      "invalid_username"
#define ERR_INVALID_PASSWORD      "invalid_password"
#define ERR_NOT_LOGGED_IN         "not_logged_in"
#define ERR_INVALID_CREDENTIALS   "invalid_credentials"
#define ERR_FORBIDDEN_STATE       "forbidden_state"
#define ERR_NOT_IN_GAME           "not_in_game"
#define ERR_USER_NOT_FOUND        "user_not_found"
#define ERR_CHALLENGE_NOT_FOUND   "challenge_not_found"
#define ERR_MATCH_NOT_FOUND       "match_not_found"
#define ERR_NO_PENDING_OFFER      "no_pending_offer"
#define ERR_OPPONENT_LEFT         "opponent_left"
#define ERR_USERNAME_TAKEN        "username_taken"
#define ERR_ALREADY_LOGGED_IN     "already_logged_in"
#define ERR_PLAYER_BUSY           "player_busy"
#define ERR_CHALLENGE_PENDING     "challenge_pending"
#define ERR_OFFER_PENDING         "offer_pending"
#define ERR_NOT_YOUR_TURN         "not_your_turn"
#define ERR_OUT_OF_BOARD          "out_of_board"
#define ERR_CELL_OCCUPIED         "cell_occupied"
#define ERR_GAME_NOT_ACTIVE       "game_not_active"
#define ERR_SERVER_ERROR          "server_error"

/* ------------------------------------------------------------------------------------------
 * 4. Session states (protocol.md §5)
 * ---------------------------------------------------------------------------------------- */
typedef enum {
    SESSION_CONNECTED = 0,  /* C: TCP connected, not logged in */
    SESSION_LOBBY,          /* L: logged in, idle */
    SESSION_IN_QUEUE,       /* Q: waiting for matchmaking (FIND_MATCH) */
    SESSION_IN_GAME         /* G: in a game, running or ended, until LEAVE_GAME or rematch */
} SessionState;

/* Player status as shown to others (PLAYER_STATUS, LIST_PLAYERS). */
#define PS_IDLE               "IDLE"       /* = LOBBY */
#define PS_IN_QUEUE           "IN_QUEUE"
#define PS_IN_GAME            "IN_GAME"
#define PS_OFFLINE            "OFFLINE"    /* only in PLAYER_STATUS */

/* ------------------------------------------------------------------------------------------
 * 5. Requests: client -> server (protocol.md §6)
 * ---------------------------------------------------------------------------------------- */
/* 6.1 Account and session */
#define CMD_PING              "PING"
#define CMD_REGISTER          "REGISTER"
#define CMD_LOGIN             "LOGIN"
#define CMD_LOGOUT            "LOGOUT"
/* 6.2 Lobby and challenges */
#define CMD_LIST_PLAYERS      "LIST_PLAYERS"
#define CMD_CHALLENGE         "CHALLENGE"
#define CMD_CHALLENGE_REPLY   "CHALLENGE_REPLY"
#define CMD_CHALLENGE_CANCEL  "CHALLENGE_CANCEL"
#define CMD_FIND_MATCH        "FIND_MATCH"
#define CMD_CANCEL_MATCH      "CANCEL_MATCH"
/* 6.3 In a game */
#define CMD_MOVE              "MOVE"
#define CMD_RESIGN            "RESIGN"
#define CMD_DRAW_OFFER        "DRAW_OFFER"
#define CMD_DRAW_REPLY        "DRAW_REPLY"
#define CMD_REMATCH           "REMATCH"
#define CMD_REMATCH_REPLY     "REMATCH_REPLY"
#define CMD_LEAVE_GAME        "LEAVE_GAME"
/* 6.4 History and ranking */
#define CMD_HISTORY           "HISTORY"
#define CMD_REPLAY            "REPLAY"
#define CMD_LEADERBOARD       "LEADERBOARD"
#define CMD_STATS             "STATS"

/* Answer argument of CHALLENGE_REPLY, DRAW_REPLY, REMATCH_REPLY */
#define ARG_ACCEPT            "ACCEPT"
#define ARG_DECLINE           "DECLINE"

/* ------------------------------------------------------------------------------------------
 * 6. Events: server -> client, unrequested (protocol.md §7)
 * ---------------------------------------------------------------------------------------- */
#define EV_PLAYER_STATUS        "PLAYER_STATUS"
#define EV_CHALLENGE_FROM       "CHALLENGE_FROM"
#define EV_CHALLENGE_DECLINED   "CHALLENGE_DECLINED"
#define EV_CHALLENGE_CANCELLED  "CHALLENGE_CANCELLED"
#define EV_CHALLENGE_EXPIRED    "CHALLENGE_EXPIRED"
#define EV_GAME_START           "GAME_START"
#define EV_OPP_MOVE             "OPP_MOVE"
#define EV_DRAW_OFFERED         "DRAW_OFFERED"
#define EV_DRAW_DECLINED        "DRAW_DECLINED"
#define EV_REMATCH_REQUESTED    "REMATCH_REQUESTED"
#define EV_REMATCH_DECLINED     "REMATCH_DECLINED"
#define EV_GAME_END             "GAME_END"
#define EV_OPP_LEFT             "OPP_LEFT"

/* Symbols (GAME_START your_symbol, REPLAY, HISTORY my_symbol) */
#define SYM_X                 "X"          /* X always moves first */
#define SYM_O                 "O"

/* GAME_END / REPLAY result */
#define RES_X_WIN             "X_WIN"
#define RES_O_WIN             "O_WIN"
#define RES_DRAW              "DRAW"

/* GAME_END / REPLAY / HISTORY reason */
#define END_FIVE              "FIVE"         /* the only reason that carries the 5 r,c fields */
#define END_BOARD_FULL        "BOARD_FULL"
#define END_RESIGN            "RESIGN"
#define END_DRAW_AGREED       "DRAW_AGREED"
#define END_TIMEOUT           "TIMEOUT"
#define END_DISCONNECT        "DISCONNECT"

/* HISTORY outcome, from the caller's point of view */
#define OUT_WIN               "WIN"
#define OUT_LOSS              "LOSS"
#define OUT_DRAW              "DRAW"

/* ------------------------------------------------------------------------------------------
 * 7. Game rules and limits (protocol.md §6.2, §6.3, §6.4, §8, §9)
 * ---------------------------------------------------------------------------------------- */
#define BOARD_SIZE            15      /* the only size accepted in v1.1 */
#define BOARD_SIZE_MAX        19      /* 10 and 19 are reserved for a later version */
#define WIN_LENGTH            5       /* five or more in a row wins */

#define SEC_PER_MOVE_FAST     15      /* allowed "sec" values in CHALLENGE: 15, 30, 60 */
#define SEC_PER_MOVE_NORMAL   30
#define SEC_PER_MOVE_SLOW     60

#define CHALLENGE_EXPIRE_SEC  30      /* unanswered challenge -> CHALLENGE_EXPIRED to both */

/* Matchmaking: max ELO gap = MM_BASE_GAP + MM_STEP_GAP * floor(waited / MM_STEP_SEC),
 * capped at MM_MAX_GAP. Matched games use BOARD_SIZE and MATCH_SEC_PER_MOVE. */
#define MM_BASE_GAP           100
#define MM_STEP_GAP           50
#define MM_STEP_SEC           10
#define MM_MAX_GAP            400
#define MATCH_SEC_PER_MOVE    SEC_PER_MOVE_NORMAL

#define ELO_START             1000
#define ELO_K                 32

#define HISTORY_PAGE_SIZE     10      /* page starts at 1 */
#define LEADERBOARD_DEFAULT   10
#define LEADERBOARD_MAX       20

#endif /* CARO_PROTO_H */
