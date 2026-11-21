
/*
 * server/elo.c — pure ELO rating calculation.
 * Owner: Hân.
 */
#include "elo.h"
#include "proto.h"

#include <math.h>

/* Expected score based on the rating difference. */
double elo_expected_score(int rating_a, int rating_b)
{
    return 1.0 / (1.0 + pow(10.0, (rating_b - rating_a) / 400.0));
}

/* Calculate the updated rating after a match. */
int elo_update(int current_rating, int opponent_rating, double score)
{
    if (score < 0.0 || score > 1.0)
        return current_rating;

    double expected = elo_expected_score(current_rating, opponent_rating);

    return (int)round(current_rating + ELO_K * (score - expected));
}
