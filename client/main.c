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
 * TODO(C5): real history page; it registers its events with dispatch_on().
 * TODO(C6): minimum window size.
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

AppState app;   /* the single global state (app.h) */

static gboolean env_is_one(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && strcmp(v, "1") == 0;
}

static GtkWidget *placeholder_page(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    return label;
}

void app_show_page(const char *name)
{
    if (strcmp(name, "lobby") == 0)
        ui_lobby_enter();
    else if (strcmp(name, "game") == 0)
        ui_game_enter();
    gtk_stack_set_visible_child_name(app.stack, name);
}

void app_go_login(const char *error)
{
    ui_lobby_reset();                /* no dialog may outlive the session */
    ui_game_reset();
    app.state = SESSION_CONNECTED;
    app.username[0] = '\0';
    app.elo = app.wins = app.losses = app.draws = 0;
    memset(&app.game, 0, sizeof app.game);
    ui_login_reset(error);
    app_show_page("login");
}

/* Offline-mode test keys (Cmd also works on macOS).
 *   lobby page: Ctrl+I = receive a fake invitation, Ctrl+U = one withdrawn after 5 s.
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
    gtk_window_set_title(app.window, app.fake_mode ? APP_TITLE " [offline]" : APP_TITLE);
    gtk_window_set_default_size(app.window, WIN_WIDTH, WIN_HEIGHT);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *stack = gtk_stack_new();
    app.stack = GTK_STACK(stack);
    gtk_stack_set_transition_type(app.stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(app.stack, ui_login_new(), "login");
    gtk_stack_add_named(app.stack, ui_lobby_new(), "lobby");
    gtk_stack_add_named(app.stack, ui_game_new(), "game");
    gtk_stack_add_named(app.stack, placeholder_page("TODO (C5): history page"), "history");
    gtk_container_add(GTK_CONTAINER(window), stack);

    gtk_widget_show_all(window);
    app_show_page("login");
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
