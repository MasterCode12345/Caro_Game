/*
 * server/log.h — server.log.
 * Owner: Hân.
 *
 * S0: open/close log, log requests and server events.
 */
#ifndef CARO_SERVER_LOG_H
#define CARO_SERVER_LOG_H

#include "server.h"

int log_open(const char *path);
void log_close(void);
void log_line(Client *c, const char *cmd, int code);
void log_event(const char *fmt, ...);

#endif /* CARO_SERVER_LOG_H */