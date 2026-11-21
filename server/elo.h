
/*
 * server/elo.h — pure ELO rating logic.
 * Owner: Hân.
 */
#ifndef CARO_ELO_H
#define CARO_ELO_H

/* Calculate the expected score of player A against player B. */
double elo_expected_score(int rating_a, int rating_b);

/*
 * Calculate the new rating of one player.
 * score: 1.0 = win, 0.5 = draw, 0.0 = loss.
 */
int elo_update(int current_rating, int opponent_rating, double score);

#endif /* CARO_ELO_H */
