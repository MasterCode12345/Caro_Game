/*
 * server/auth.h — REGISTER, LOGIN, LOGOUT handlers.
 * Owner: Hân.
 */
#ifndef CARO_SERVER_AUTH_H
#define CARO_SERVER_AUTH_H

#include "server.h"

void handle_register(Client *c, char *fields[], int nfields);
void handle_login(Client *c, char *fields[], int nfields);
void handle_logout(Client *c, char *fields[], int nfields);

#endif /* CARO_SERVER_AUTH_H */