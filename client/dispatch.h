/*
 * client/dispatch.h — response FIFO matching + event table.
 * Owner: Nghĩa.
 */
#ifndef CARO_CLIENT_DISPATCH_H
#define CARO_CLIENT_DISPATCH_H

#include <glib.h>

/* code given to a ResponseCb when the connection is lost before the response arrived.
 * fields are then { "0", COMMAND, "disconnected" }, n = 3. Real status codes are >= 100. */
#define RESP_DISCONNECTED 0

/* fields[0] = status code, fields[1] = COMMAND, fields[2..n-1] = data or the reason token.
 * The strings live only during the call: copy what you keep. */
typedef void (*ResponseCb)(int code, char **fields, int n, gpointer user_data);

/* fields[0] = event name, fields[1..n-1] = its arguments. Same lifetime rule. */
typedef void (*EventHandler)(char **fields, int n);

void dispatch_on(const char *event, EventHandler handler);   /* register at start-up */

/* Called by net_request() just before the request is sent: remembers who gets the response. */
void dispatch_expect(const char *payload, ResponseCb cb, gpointer user_data);

/* One complete incoming message (real or fake). msg is split in place. */
void dispatch(char *msg);

/* Connection lost: every waiting callback gets RESP_DISCONNECTED, oldest first. */
void dispatch_fail_pending(void);

/* CARO_DEBUG=1 trace: dir is ">>" (sent) or "<<" (received). Passwords are masked. */
void dispatch_trace(const char *dir, const char *msg);

/* Decimal field -> int (strtol + end pointer). FALSE if s is not exactly an integer. */
gboolean field_int(const char *s, int *out);

void dispatch_cleanup(void);   /* at exit: drop pending requests WITHOUT calling them */

#endif /* CARO_CLIENT_DISPATCH_H */
