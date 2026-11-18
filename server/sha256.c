/*
 * server/sha256.c — bundled SHA-256 (FIPS 180-4), so the server has no OpenSSL dependency.
 * Owner: Hân.
 *
 * Used by auth.c: password hash = SHA-256(salt_hex + password).
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S1): SHA-256 of a byte buffer -> 32 bytes; hex helper -> 64 chars + '\0'.
 * TODO(S1): check against known vectors (compare with `shasum -a 256` / `sha256sum`):
 *           ""    -> e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 *           "abc" -> ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 */
