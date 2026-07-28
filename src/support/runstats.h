/*
 * runstats.h - Running min/max/mean/variance over a stream of values.
 *
 * The numbers half of a distribution, with no opinion about what the values
 * mean or how they are bucketed. src/support/histogram.c owns one of these
 * beside its log-spaced bins; the assignment-value plot
 * (src/subsystems/assign_plot/) owns one over signed, unitless floats that no
 * duration histogram could bin. Neutral utility in the sense of src/support:
 * no app dependency, no units, no notion of a frame.
 *
 * Values are unconstrained doubles - negative, zero, and huge are all fine.
 * That is the whole reason this is separate from Histogram, whose bins only
 * span positive microseconds.
 *
 * mean/variance use Welford's online update rather than accumulating a sum of
 * squares: over a long run sum^2 dwarfs the spread, and the textbook
 * E[x^2] - E[x]^2 form cancels away the significant digits that carry it. sum
 * is kept as its own plain running total, which is exact enough for a total
 * and is the one figure that cannot be recovered from the others.
 */
#ifndef RUNSTATS_H
#define RUNSTATS_H

/* The accumulator. Fields are private to runstats.c - read them through
 * runstats_read(). It is a value type rather than an opaque pointer so owners
 * can embed it (or hold an array of them) with no allocation. Zero-initialize
 * (static storage or runstats_clear) before the first record. */
typedef struct {
    unsigned long long count;
    double             min, max;
    double             sum;
    /* Welford's online accumulators: mean, and m2 = sum of squared deviations
     * from the running mean. */
    double             mean;
    double             m2;
} RunStats;

/* Derived read-out. variance/stddev are the *sample* statistics (the n-1
 * divisor): this is an ongoing sample of a process that keeps running, not a
 * closed population. Both are 0 until the second value arrives. With no values
 * recorded yet, every field is 0. */
typedef struct {
    unsigned long long count;
    double min, max;
    double sum;
    double mean;
    double variance;
    double stddev;      /* sqrt(variance) */
} RunStatsSummary;

/* Fold one value into the running statistics. No-op on a NULL argument. */
void runstats_record(RunStats *s, double value);

/* Reset to the no-values-yet state. No-op on a NULL argument. */
void runstats_clear(RunStats *s);

/* Derive the summary. No-op on a NULL argument. */
void runstats_read(const RunStats *s, RunStatsSummary *out);

#endif /* RUNSTATS_H */
