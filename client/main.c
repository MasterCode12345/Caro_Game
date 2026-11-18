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
 * TODO(C2–C5): real lobby, game and history pages; each registers its events with dispatch_on().
 * TODO(C4): on a lost connection, also close every open dialog.
 * TODO(C6): minimum window size.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
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
    gtk_stack_set_visible_child_name(app.stack, name);
}

void app_go_login(const char *error)
{
    app.state = SESSION_CONNECTED;
    app.username[0] = '\0';
    app.elo = app.wins = app.losses = app.draws = 0;
    memset(&app.game, 0, sizeof app.game);
    ui_login_reset(error);
    app_show_page("login");
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
    gtk_stack_add_named(app.stack, placeholder_page("TODO (C3): game page"), "game");
    gtk_stack_add_named(app.stack, placeholder_page("TODO (C5): history page"), "history");
    gtk_container_add(GTK_CONTAINER(window), stack);

    gtk_widget_show_all(window);
    app_show_page("login");
    if (app.fake_mode)
        g_printerr("caro_client: offline mode (CARO_FAKE=1), no server is used\n");

    gtk_main();

    /* The widgets are gone: drop waiting requests without calling their callbacks, then close. */
    dispatch_cleanup();
    net_close();
    return 0;
}
