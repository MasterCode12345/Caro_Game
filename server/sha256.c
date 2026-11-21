/*
 * server/sha256.c — bundled SHA-256 (FIPS 180-4).
 * Owner: Hân.
 *
 * Used by auth.c: password hash = SHA-256(salt_hex + password).
 */
#include "sha256.h"

#include <stdint.h>
#include <string.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define EP0(x) (ROTR((x), 2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define EP1(x) (ROTR((x), 6) ^ ROTR((x), 11) ^ ROTR((x), 25))

#define SIG0(x) (ROTR((x), 7) ^ ROTR((x), 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void transform(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8)
             | ((uint32_t)block[i * 4 + 3]);
    }

    for (int i = 16; i < 64; i++) {
        w[i] = SIG1(w[i - 2]) + w[i - 7]
             + SIG0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_hex(const unsigned char *data, size_t len, char out[65])
{
    static const char hex[] = "0123456789abcdef";

    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    unsigned char block[64];
    size_t offset = 0;

    /* Process full blocks. */
    while (len - offset >= 64) {
        transform(state, data + offset);
        offset += 64;
    }

    /* Copy remaining bytes and append padding. */
    size_t remaining = len - offset;
    memcpy(block, data + offset, remaining);
    block[remaining] = 0x80;

    if (remaining >= 56) {
        memset(block + remaining + 1, 0, 63 - remaining);
        transform(state, block);

        memset(block, 0, 56);
    } else {
        memset(block + remaining + 1, 0, 55 - remaining);
    }

    /* Append original message length in bits, big-endian. */
    uint64_t bit_len = (uint64_t)len * 8U;

    for (int i = 0; i < 8; i++) {
        block[63 - i] = (unsigned char)(bit_len >> (i * 8));
    }

    transform(state, block);

    /* Convert state to hexadecimal. */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            unsigned char byte =
                (unsigned char)(state[i] >> (24 - j * 8));

            out[(i * 4 + j) * 2]     = hex[byte >> 4];
            out[(i * 4 + j) * 2 + 1] = hex[byte & 0x0f];
        }
    }

    out[64] = '\0';
}