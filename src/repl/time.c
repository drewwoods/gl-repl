/*
 * src/repl/time.c - Public helpers for the predefined animation clock.
 */

#include "repl/time.h"

#include "repl/state_owners.h"

void repl_advance_time(float dt) {
    repl_state_time_advance(dt);
}

void repl_reset_time_to_zero(void) {
    repl_state_time_reset_to_zero();
}

void repl_set_time(float value) {
    repl_state_time_set(value);
}
