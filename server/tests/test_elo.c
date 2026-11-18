/*
 * server/tests/test_elo.c — unit tests for server/elo.c (pure: no sockets, no DB).
 * Built as build/test_elo, run by `make test`.
 * Owner: Hân.
 *
 * SKELETON: no tests yet.
 * TODO(S5): the checks of docs/protocol.md §9:
 *           1000 vs 1000, A wins -> +16 / -16; 1000 beats 1200 -> +24; 1200 beats 1000 -> +8.
 * TODO(S5): a draw between equal ratings -> 0; delta_B == -delta_A in every case.
 * TODO(S5): print PASS/FAIL per test; return non-zero if any test fails.
 */
int main(void)
{
    return 0;
}
