
/*
 * server/tests/test_elo.c — tests for pure ELO logic.
 * Owner: Hân.
 */
#include "../elo.h"

#include <stdio.h>
#include <stdlib.h>

static int total_tests = 0;
static int failed_tests = 0;

#define TEST(name) \
    static void name(void)

#define RUN(name) do { \
    total_tests++; \
    int before = failed_tests; \
    name(); \
    if (failed_tests == before) \
        printf("[PASS] %s\n", #name); \
} while (0)

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("[FAIL] %s:%d: %s\n", \
               __FILE__, __LINE__, #condition); \
        failed_tests++; \
        return; \
    } \
} while (0)

/* Equal ratings should produce an expected score of 0.5. */
TEST(test_equal_rating_expected)
{
    double expected = elo_expected_score(1000, 1000);

    CHECK(expected > 0.4999 && expected < 0.5001);
}

/* Higher rated player should have a higher expected score. */
TEST(test_higher_rating_expected)
{
    double expected = elo_expected_score(1200, 1000);

    CHECK(expected > 0.5);
    CHECK(expected < 1.0);
}

/* Expected scores of two players should sum to 1. */
TEST(test_expected_score_symmetry)
{
    double a = elo_expected_score(1200, 1000);
    double b = elo_expected_score(1000, 1200);

    CHECK(a + b > 0.9999 && a + b < 1.0001);
}

/* Winner against an equally rated opponent gains 16 points. */
TEST(test_win_update)
{
    int new_rating = elo_update(1000, 1000, 1.0);

    CHECK(new_rating == 1016);
}

/* Loser against an equally rated opponent loses 16 points. */
TEST(test_loss_update)
{
    int new_rating = elo_update(1000, 1000, 0.0);

    CHECK(new_rating == 984);
}

/* Draw against an equally rated opponent leaves rating unchanged. */
TEST(test_draw_update)
{
    int new_rating = elo_update(1000, 1000, 0.5);

    CHECK(new_rating == 1000);
}

/* Winning against a higher rated opponent gains more points. */
TEST(test_win_against_higher_rating)
{
    int new_rating = elo_update(1000, 1200, 1.0);

    CHECK(new_rating > 1016);
}

/* Losing against a lower rated opponent loses more points. */
TEST(test_loss_against_lower_rating)
{
    int new_rating = elo_update(1000, 800, 0.0);

    CHECK(new_rating < 984);
}

/* Invalid score must not change the rating. */
TEST(test_invalid_score)
{
    CHECK(elo_update(1000, 1000, -1.0) == 1000);
    CHECK(elo_update(1000, 1000, 2.0) == 1000);
}

/* Rating updates for equally rated players should be zero-sum. */
TEST(test_zero_sum_update)
{
    int a = elo_update(1000, 1000, 1.0);
    int b = elo_update(1000, 1000, 0.0);

    CHECK((a - 1000) + (b - 1000) == 0);
}

int main(void)
{
    RUN(test_equal_rating_expected);
    RUN(test_higher_rating_expected);
    RUN(test_expected_score_symmetry);
    RUN(test_win_update);
    RUN(test_loss_update);
    RUN(test_draw_update);
    RUN(test_win_against_higher_rating);
    RUN(test_loss_against_lower_rating);
    RUN(test_invalid_score);
    RUN(test_zero_sum_update);

    printf("\n%d/%d tests passed.\n",
           total_tests - failed_tests, total_tests);

    return failed_tests == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
