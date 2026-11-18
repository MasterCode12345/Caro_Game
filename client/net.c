/*
 * client/net.c — the only file that touches the socket.
 * Owner: Nghĩa.
 *
 * Single thread: the socket is watched by the GLib main loop (g_io_add_watch), so the UI never
 * blocks on recv(). UI code never calls send()/recv() directly.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap, "Client" section):
 * TODO(C1): net_connect(): getaddrinfo() + connect(); signal(SIGPIPE, SIG_IGN);
 *           g_io_channel_unix_new() + g_io_add_watch(G_IO_IN | G_IO_HUP | G_IO_ERR).
 * TODO(C1): watch callback: frame_recv() once; <= 0 -> net_on_lost(); else drain frame_next()
 *           and pass each message to dispatch(); bad frame -> net_on_lost().
 * TODO(C1): net_request(payload, cb, data): push {command, cb, data} on the FIFO (GQueue),
 *           then frame_send(). The server answers in order, so the head owns the next response.
 * TODO(C1): net_close(): remove the watch, close the fd, empty the FIFO (each cb gets a
 *           "disconnected" error). net_on_lost(): net_close() + back to login with a message.
 * TODO(C1): CARO_FAKE=1 -> no socket; net_request() goes to fake.c instead.
 * TODO(C4): losing the server on any page, also while a response is pending: close every
 *           dialog, back to login, never crash, never hang.
 */
