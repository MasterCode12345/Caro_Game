/*
 * server/log.h — server.log.
 * Owner: Hân.
 *
 * One line per request (log_line) and per server event (log_event), in server.log and on stdout.
 */
#ifndef CARO_SERVER_LOG_H
#define CARO_SERVER_LOG_H

#include "server.h"

/* Append to path and log "server started". 0 ok, -1 error (log lines then only go to stdout). */
int log_open(const char *path);

/* Log "server stopped" and close the file. */
void log_close(void);

/* time | IP:port | username or - | command | status code */
void log_line(const Client *c, const char *cmd, int code);

/* time | EVENT | message */
void log_event(const char *fmt, ...);

#endif /* CARO_SERVER_LOG_H */
