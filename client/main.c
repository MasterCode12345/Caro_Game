/*
 * client/main.c — entry point of caro_client (GTK 3).
 * Owner: Nghĩa.
 *
 * P0: opens one 900x700 window; closing it quits.
 *
 * Single thread: everything runs in the GLib main loop started by gtk_main(). From C1 the
 * socket is watched by the same loop (g_io_add_watch in net.c), so no pthreads are needed.
 * gtk_init() + gtk_main() instead of GtkApplication: GtkApplication is single-instance by
 * default, and we run two clients on one machine for testing.
 *
 * TODO(C1): own the single global AppState `app` (app.h); read CARO_FAKE / CARO_DEBUG;
 *           GtkStack with 4 pages "login", "lobby", "game", "history"; app_show_page(name).
 * TODO(C1): register every page's event handlers with dispatch_on() at start-up.
 * TODO(C4): on a lost connection, any page goes back to "login" with a message.
 * TODO(C6): minimum window size.
 */
#include <signal.h>
#include <stdio.h>

#include <gtk/gtk.h>

#define APP_TITLE   "Caro Online – Group 13"
#define WIN_WIDTH   900
#define WIN_HEIGHT  700

int main(int argc, char *argv[])
{
    /* send() on a socket the server has closed raises SIGPIPE, which kills the process by
     * default. Ignore it so send() just fails with EPIPE (CLAUDE.md "Platform notes"). */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        perror("signal(SIGPIPE)");

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(window), WIN_WIDTH, WIN_HEIGHT);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *label = gtk_label_new(APP_TITLE "\n\n(skeleton — pages arrive in C1)");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_container_add(GTK_CONTAINER(window), label);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
