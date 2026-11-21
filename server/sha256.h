/*server/sha256.h — interface of sha256.c: bundled SHA-256.
- Owner: Hân.
  */
#ifndef CARO_SERVER_SHA256_H
#define CARO_SERVER_SHA256_H

#include <stddef.h>

/* Compute SHA-256 and write 64 lowercase hexadecimal characters + '\0'. */
void sha256_hex(const unsigned char *data, size_t len, char out[65]);

#endif /* CARO_SERVER_SHA256_H */