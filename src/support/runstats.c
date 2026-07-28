/*
 * runstats.c - Running min/max/mean/variance over a stream of values.
 */
#include "support/runstats.h"

#include <math.h>

void runstats_record(RunStats *s, double value) {
    double delta, delta2;

    if (!s) return;

    /* count == 0 seeds both extremes, so a first value that is negative (or
     * anything else that would lose to a zero-initialized min/max) still
     * lands correctly. */
    if (s->count == 0 || value < s->min) s->min = value;
    if (s->count == 0 || value > s->max) s->max = value;
    s->sum += value;
    s->count++;

    delta   = value - s->mean;
    s->mean += delta / (double)s->count;
    delta2  = value - s->mean;   /* deviation from the *updated* mean */
    s->m2  += delta * delta2;
}

void runstats_clear(RunStats *s) {
    if (!s) return;
    s->count = 0;
    s->min   = 0.0;
    s->max   = 0.0;
    s->sum   = 0.0;
    s->mean  = 0.0;
    s->m2    = 0.0;
}

void runstats_read(const RunStats *s, RunStatsSummary *out) {
    if (!s || !out) return;
    out->count = s->count;
    out->min   = s->min;
    out->max   = s->max;
    out->sum   = s->sum;
    out->mean  = (s->count > 0) ? s->mean : 0.0;
    /* n-1 divisor: an ongoing sample of a running process, not a population.
     * m2 is a sum of squares and so cannot go negative, but a long run of
     * identical values can leave it at -0.0; max() keeps sqrt() honest. */
    out->variance = (s->count > 1)
                  ? ((s->m2 > 0.0) ? s->m2 / (double)(s->count - 1) : 0.0)
                  : 0.0;
    out->stddev   = sqrt(out->variance);
}
