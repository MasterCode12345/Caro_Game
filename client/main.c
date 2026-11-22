/*
 * client/main.c — entry point of caro_client (GTK 3): the window, the 4 pages, the global `app`.
 * Owner: Nghĩa.
 *
 * Environment:  CARO_FAKE=1   offline mode, fake.c plays the server (no socket at all)
 *               CARO_DEBUG=1  every message sent/received is printed on stderr
 *
 * Single thread: everything runs in the GLib main loop started by gtk_main(). The socket is
 * watched by the same loop (g_io_add_watch in net.c), so no pthreads are needed.
 * gtk_init() + gtk_main() instead of GtkApplication: GtkApplication is single-instance by
 * default, and we run two clients on one machine for testing.
 *
 * Losing the server (net_on_lost) ends in app_go_login(): every page's reset function closes
 * its own dialogs first, so no dialog outlives the session.
 *
 * Page changes all go through app_show_page(): it tells the page that is shown what to load,
 * and tells the history page when it is left, so its replay timer never runs in the background.
 *
 * Closing the window during a running game asks first: the server sees the closed socket as a
 * lost connection and gives the game to the opponent (reason DISCONNECT, protocol.md §8).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
#include "fake.h"
#include "net.h"

#define APP_TITLE   "Caro Online – Group 13"
#define WIN_WIDTH   900
#define WIN_HEIGHT  700
/* Smallest window: the game page still has a 450-px board (30 px per cell on 15×15) next to its
 * 250-px side panel, and the lobby's buttons fit on one row. */
#define WIN_MIN_WIDTH   760
#define WIN_MIN_HEIGHT  560

AppState app;   /* the single global state (app.h) */

static GtkWidget *quit_dialog;   /* "Thoát bây giờ sẽ bị xử thua?" while a game runs */

void app_update_title(void)
{
    char *title = g_strdup_printf("%s%s%s%s", APP_TITLE, app.username[0] != '\0' ? " · " : "",
                                  app.username, app.fake_mode ? " [offline]" : "");
    gtk_window_set_title(app.window, title);
    g_free(title);
}

static void close_quit_dialog(void)
{
    GtkWidget *d = quit_dialog;
    if (d != NULL) {
        quit_dialog = NULL;
        gtk_widget_destroy(d);
    }
}

static gboolean env_is_one(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && strcmp(v, "1") == 0;
}

void app_show_page(const char *name)
{
    /* Leaving the history page (also when GAME_START pulls us into a game while we watch a
     * replay from the queue): its auto-play timer is removed with g_source_remove(). Harmless
     * when the history page was not shown. */
    if (strcmp(name, "history") != 0)
        ui_history_leave();
    if (strcmp(name, "lobby") == 0)
        ui_lobby_enter();
    else if (strcmp(name, "game") == 0)
        ui_game_enter();
    else if (strcmp(name, "history") == 0)
        ui_history_enter();
    gtk_stack_set_visible_child_name(app.stack, name);
}

void app_go_login(const char *error)
{
    close_quit_dialog();             /* no dialog may outlive the session */
    ui_lobby_reset();
    ui_game_reset();
    ui_history_reset();
    app.state = SESSION_CONNECTED;
    app.username[0] = '\0';
    app.elo = app.wins = app.losses = app.draws = 0;
    memset(&app.game, 0, sizeof app.game);
    app_update_title();
    ui_login_reset(error);
    app_show_page("login");
}

/* ---- closing the window -------------------------------------------------------------------------- */

static void on_quit_response(GtkDialog *dialog, gint response, gpointer data)
{
    (void)data;
    if (quit_dialog == GTK_WIDGET(dialog))
        quit_dialog = NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response != GTK_RESPONSE_ACCEPT)
        return;
    net_close();                     /* the server sees the FIN at once: DISCONNECT, we lose */
    gtk_widget_destroy(GTK_WIDGET(app.window));     /* "destroy" -> gtk_main_quit() */
}

/* "delete-event" = the window's close button (or Cmd+W / Alt+F4). TRUE keeps the window open. */
static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    if (!app.game.active || app.game.ended)
        return FALSE;                /* nothing to lose: close now */
    if (quit_dialog != NULL) {
        gtk_window_present(GTK_WINDOW(quit_dialog));
        return TRUE;
    }
    quit_dialog = gtk_message_dialog_new(app.window, GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                         "Thoát bây giờ sẽ bị xử thua");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(quit_dialog),
        "Ván #%d với %s đang diễn ra. Nếu thoát, server coi như bạn mất kết nối: bạn bị tính "
        "thua và bị trừ ELO.", app.game.gid, app.game.opponent);
    gtk_dialog_add_button(GTK_DIALOG(quit_dialog), "Ở lại", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(quit_dialog), "Thoát", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(quit_dialog), GTK_RESPONSE_REJECT);
    g_signal_connect(quit_dialog, "response", G_CALLBACK(on_quit_response), NULL);
    gtk_widget_show_all(quit_dialog);
    return TRUE;
}

/* Offline-mode test keys (Cmd also works on macOS).
 *   lobby page: Ctrl+I = receive a fake invitation, Ctrl+U = one withdrawn after 5 s,
 *               Ctrl+M = the next CANCEL_MATCH reaches the server just after it paired us.
 *   game page:  Ctrl+D the opponent offers a draw, Ctrl+R asks for a rematch, Ctrl+L leaves,
 *               Ctrl+K disconnects, Ctrl+T the player to move runs out of time. */
static gboolean game_key_action(guint key, FakeOpponentAction *action)
{
    switch (key) {
    case GDK_KEY_d: *action = FAKE_OPP_DRAW_OFFER; return TRUE;
    case GDK_KEY_r: *action = FAKE_OPP_REMATCH;    return TRUE;
    case GDK_KEY_l: *action = FAKE_OPP_LEAVE;      return TRUE;
    case GDK_KEY_k: *action = FAKE_OPP_DISCONNECT; return TRUE;
    case GDK_KEY_t: *action = FAKE_TIMEOUT;        return TRUE;
    default:        return FALSE;
    }
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
    const char *page = gtk_stack_get_visible_child_name(app.stack);
    if ((mods != GDK_CONTROL_MASK && mods != GDK_META_MASK) || page == NULL)
        return FALSE;
    guint key = gdk_keyval_to_lower(ev->keyval);
    FakeOpponentAction action;
    if (strcmp(page, "lobby") == 0 && (key == GDK_KEY_i || key == GDK_KEY_u)) {
        if (!fake_simulate_invite(key == GDK_KEY_u))
            g_printerr("caro_client: fake: nobody idle can invite you right now\n");
    } else if (strcmp(page, "lobby") == 0 && key == GDK_KEY_m) {
        if (fake_simulate_match_race())
            g_printerr("caro_client: fake: the next CANCEL_MATCH arrives after the match is made\n");
        else
            g_printerr("caro_client: fake: only in the lobby or the queue\n");
    } else if (strcmp(page, "game") == 0 && game_key_action(key, &action)) {
        if (!fake_simulate_opponent(action))
            g_printerr("caro_client: fake: not possible at this point of the game\n");
    } else {
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    /* send() on a socket the server has closed raises SIGPIPE, which kills the process by
     * default. Ignore it so send() just fails with EPIPE (CLAUDE.md "Platform notes"). */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        perror("signal(SIGPIPE)");

    gtk_init(&argc, &argv);
    app.fake_mode = env_is_one("CARO_FAKE");
    app.debug = env_is_one("CARO_DEBUG");
    app.state = SESSION_CONNECTED;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app.window = GTK_WINDOW(window);
    gtk_window_set_default_size(app.window, WIN_WIDTH, WIN_HEIGHT);
    gtk_widget_set_size_request(window, WIN_MIN_WIDTH, WIN_MIN_HEIGHT);  /* on a window: its minimum */
    g_signal_connect(window, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *stack = gtk_stack_new();
    app.stack = GTK_STACK(stack);
    gtk_stack_set_transition_type(app.stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(app.stack, ui_login_new(), "login");
    gtk_stack_add_named(app.stack, ui_lobby_new(), "lobby");
    gtk_stack_add_named(app.stack, ui_game_new(), "game");
    gtk_stack_add_named(app.stack, ui_history_new(), "history");
    gtk_container_add(GTK_CONTAINER(window), stack);

    gtk_widget_show_all(window);
    app_go_login(NULL);              /* title, empty pages, focus on Tên đăng nhập */
    if (app.fake_mode) {
        g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);
        g_printerr("caro_client: offline mode (CARO_FAKE=1), no server is used\n");
    }

    gtk_main();

    /* The widgets are gone: drop waiting requests without calling their callbacks, then close. */
    dispatch_cleanup();
    net_close();
    return 0;
}
