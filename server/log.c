/*
 * server/log.c — server.log, one line per request.
 * Owner: Hân.
 *
 * S0: logging requests and server events to server.log and stdout.
 * Never log passwords.
 */
#include "log.h"

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

static FILE *log_file = NULL;

/* Return the current timestamp in YYYY-MM-DD HH:MM:SS format. */
static void timestamp(char *buf, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_now);
}

/* Open the log file in append mode. */
int log_open(const char *path)
{
    if (path == NULL)
        return -1;

    log_file = fopen(path, "a");

    if (log_file == NULL) {
        perror("fopen server.log");
        return -1;
    }

    log_event("server started");
    return 0;
}

/* Close the log file. */
void log_close(void)
{
    if (log_file != NULL) {
        log_event("server stopped");
        fclose(log_file);
        log_file = NULL;
    }
}

/* Write one request line. */
void log_line(Client *c, const char *cmd, int code)
{
    char ts[32];
    timestamp(ts, sizeof(ts));

    const char *addr = (c != NULL) ? c->addr : "-";
    const char *username =
        (c != NULL && c->username[0] != '\0') ? c->username : "-";
    const char *command = (cmd != NULL) ? cmd : "-";

    char line[512];

    snprintf(line, sizeof(line),
             "%s | %s | %s | %s | %d",
             ts, addr, username, command, code);

    printf("%s\n", line);
    fflush(stdout);

    if (log_file != NULL) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}

/* Write a server event. */
void log_event(const char *fmt, ...)
{
    char ts[32];
    timestamp(ts, sizeof(ts));

    char message[512];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    char line[600];
    snprintf(line, sizeof(line), "%s | EVENT | %s", ts, message);

    printf("%s\n", line);
    fflush(stdout);

    if (log_file != NULL) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}