/*
 * client/app.h — AppState: the client's single global state, owned by main.c.
 * Owner: Nghĩa.
 *
 * SKELETON: no declarations yet. Stages (CLAUDE.md Roadmap):
 * TODO(C1): typedef struct { GtkWindow *window; GtkStack *stack; username; elo; wins; losses;
 *           draws; SessionState state (common/proto.h); fake_mode; ... } AppState;
 *           extern AppState app; void app_show_page(const char *name);
 * TODO(C2): lobby data: outgoing challenge cid + its "waiting" dialog; incoming challenge
 *           dialogs kept by cid.
 * TODO(C3): current game: gid, size, my symbol, opponent, opp_elo, sec_per_move, whose turn,
 *           "waiting for 200|MOVE" flag.
 * TODO(C5): history page and replay state; countdown timer source id.
 */
#ifndef CARO_CLIENT_APP_H
#define CARO_CLIENT_APP_H

#endif /* CARO_CLIENT_APP_H */
