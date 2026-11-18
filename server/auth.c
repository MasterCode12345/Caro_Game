/*
 * server/auth.c — REGISTER, LOGIN, LOGOUT (docs/protocol.md §6.1).
 * Owner: Hân.
 *
 * Never log or store a plain-text password.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S1): REGISTER|user|pass (state C): username 3–20 [A-Za-z0-9_], password 6–32 printable
 *           ASCII except '|' (USERNAME_* / PASSWORD_* in common/proto.h) -> 400 invalid_username /
 *           invalid_password / bad_args; 409 username_taken; random salt,
 *           hash = SHA-256(salt_hex + password) (sha256.c); store (db.c); 201|REGISTER.
 * TODO(S1): LOGIN|user|pass (state C): 401 invalid_credentials for an unknown user AND for a
 *           wrong password; 409 already_logged_in; success -> state LOBBY,
 *           200|LOGIN|user|elo|wins|losses|draws, then PLAYER_STATUS|user|IDLE|elo to every
 *           other logged-in client.
 * TODO(S1): LOGOUT (states L, Q; G is 403): 200|LOGOUT, state CONNECTED,
 *           PLAYER_STATUS|user|OFFLINE|elo to the others.
 * TODO(S2): LOGOUT removes the user's pending challenges (CHALLENGE_CANCELLED to the other side).
 * TODO(S7): LOGOUT from Q also removes the user from the matchmaking queue.
 */
