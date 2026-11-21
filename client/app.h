/*
 * client/app.h — AppState: the client's single global state, owned by main.c, plus the
 * functions each page (ui_*.c) offers to the rest of the client.
 * Owner: Nghĩa.
 *
 * Everything runs in the one GLib main-loop thread, so `app` needs no locking.
 *
 * Page-local state (lobby list, challenge dialogs, ...) lives in its ui_*.c file.
 *
 * TODO(C5): history page and replay state; countdown timer source id.
 */
#ifndef CARO_CLIENT_APP_H
#define CARO_CLIENT_APP_H

#include <gtk/gtk.h>

#include "proto.h"

/* The game in progress: filled from GAME_START (ui_lobby.c), played in ui_game.c. The cells
 * themselves live in the game page's BoardView. */
typedef struct {
    gboolean active;
    int      gid;
    int      size;
    char     my_symbol;                    /* 'X' or 'O' */
    char     opponent[USERNAME_MAX + 1];
    int      opp_elo;
    int      sec_per_move;

    char     turn;                         /* whose move it is: 'X' or 'O' (X moves first) */
    int      move_no;                      /* moves played so far */
    gboolean move_pending;                 /* MOVE sent, its response not received yet */
    int      pending_row, pending_col;
    gboolean ended;                        /* GAME_END received: the board is locked */
    gboolean left;                         /* 200|LEAVE_GAME came before this game's GAME_END:
                                              active is FALSE, we are in the lobby, and that
                                              GAME_END only updates the ELO (ui_game.c) */
} GameInfo;

typedef struct {
    GtkWindow   *window;
    GtkStack    *stack;                    /* pages "login", "lobby", "game", "history" */
    gboolean     fake_mode;                /* CARO_FAKE=1: no socket, fake.c plays the server */
    gboolean     debug;                    /* CARO_DEBUG=1: every message sent/received -> stderr */

    SessionState state;                    /* our session as the server sees it (protocol.md §5) */
    char         username[USERNAME_MAX + 1];
    int          elo, wins, losses, draws;
    GameInfo     game;
} AppState;

extern AppState app;

/* main.c */
void app_show_page(const char *name);
void app_go_login(const char *error);      /* forget the session, show the login page + error */

/* ui_login.c — page "login" */
GtkWidget *ui_login_new(void);
void       ui_login_reset(const char *error);   /* unlock the form; error may be NULL */

/* ui_lobby.c — page "lobby" */
GtkWidget *ui_lobby_new(void);
void       ui_lobby_enter(void);           /* called by app_show_page("lobby") */
void       ui_lobby_reset(void);           /* session over: close its dialogs, empty the list */
void       ui_lobby_update_bar(void);      /* redraw "name · ELO · W/L/D" from app */
void       ui_lobby_request_stats(void);   /* STATS: the server's ELO and W/L/D -> app + bar */

/* ui_game.c — page "game" */
GtkWidget *ui_game_new(void);
void       ui_game_enter(void);            /* called by app_show_page("game"): new board */
void       ui_game_reset(void);            /* session over: close every dialog of the page */

#endif /* CARO_CLIENT_APP_H */
