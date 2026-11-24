/*
 * server/log.c — server.log, one line per request.
 * Owner: Hân.
 *
 * Requests and server events, to server.log and stdout. Never log passwords: log_line() gets the
 * command name only, never the arguments, and copy_command() cuts anything that does not look
 * like a command name.
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

    if (localtime_r(&now, &tm_now) == NULL ||
        strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_now) == 0)
        snprintf(buf, cap, "0000-00-00 00:00:00");
}

/* Command name for the log. An unknown command is text chosen by the client: "LOGIN han 123456"
 * (spaces instead of '|') would put a password in the log, "X\n2026-..." would forge a log line,
 * and escape characters would reach the terminal that shows the log. So only the leading
 * [A-Z_] part is kept, "..." marks a cut. */
static void copy_command(char *out, size_t cap, const char *cmd)
{
    size_t n = 0;

    while (n + 4 < cap && ((cmd[n] >= 'A' && cmd[n] <= 'Z') || cmd[n] == '_')) {
        out[n] = cmd[n];
        n++;
    }

    snprintf(out + n, cap - n, "%s", cmd[n] == '\0' ? "" : "...");
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

/* Last line of the run, then close the log file. */
void log_close(void)
{
    log_event("server stopped");

    if (log_file != NULL) {
        if (fclose(log_file) != 0)
            perror("fclose server.log");
        log_file = NULL;
    }
}

/* Write one request line. */
void log_line(const Client *c, const char *cmd, int code)
{
    char ts[32];
    timestamp(ts, sizeof(ts));

    const char *addr = (c != NULL) ? c->addr : "-";
    const char *username =
        (c != NULL && c->username[0] != '\0') ? c->username : "-";
    char command[40] = "-";

    if (cmd != NULL)
        copy_command(command, sizeof(command), cmd);

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
