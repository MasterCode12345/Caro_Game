/*
 * server/elo.c — ELO rating (docs/protocol.md §9).
 * Owner: Hân.
 *
 * PURE: no sockets, no DB, unit-tested in server/tests/test_elo.c (build/test_elo).
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(S5): elo_delta(): E_A = 1 / (1 + 10^((R_B - R_A) / 400)),
 *           delta_A = round(ELO_K * (S_A - E_A)), delta_B = -delta_A;
 *           S = 1 win, 0.5 draw, 0 loss. Uses pow() and round() -> linked with -lm.
 */
