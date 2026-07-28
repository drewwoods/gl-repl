/*
 * histogram.h - Log-spaced duration histogram with running statistics.
 *
 * One distribution of elapsed times: the bins that give it a shape, plus the
 * count/min/max/sum/mean/variance that give it numbers. Both are fed from the
 * same histogram_record() call and cleared by the same histogram_clear(), so a
 * sample can never reach one without the other and the two can never describe
 * different windows.
 *
 * Neutral utility, in the sense of src/support: no app dependency, no notion
 * of a "section" or a frame. src/support/cpuprof.c owns an array of these (one
 * per profiled section plus one for frame time) and decides when to feed them;
 * everything about what a duration *means* stays there.
 *
 * --- Bin layout ---
 *
 * HISTOGRAM_BIN_COUNT bins spaced *logarithmically* over 1 us .. 1 s, so every
 * bin is the same constant ratio (~1.36%) wide rather than the same number of
 * microseconds. Bin counts are 32-bit and saturate at UINT32_MAX.
 *
 * Log spacing, not linear, because the durations being recorded span four or
 * five decades at once: a whole frame runs in milliseconds while the cheap
 * sections around it run in microseconds. Linear bins wide enough to reach a
 * 100 ms stall (~100 us each) drop every sub-100 us section into bin 0, where
 * they are mutually indistinguishable and no plot can pull them apart. Constant
 * *relative* resolution measures a 3 us section as precisely as a 30 ms one,
 * and demotes an outlier from "blows out the whole scale" to "one more bin".
 *
 * Bin 0 also absorbs the underflow (samples below 1 us, i.e. the sections that
 * are effectively free); the last bin also absorbs the overflow (>= 1 s).
 */
#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <stdint.h>

#define HISTOGRAM_BIN_COUNT 1024
#define HISTOGRAM_MIN_US    1.0        /* bin 0's nominal lower edge   */
#define HISTOGRAM_MAX_US    1000000.0  /* last bin's upper edge (1 s)  */
#define HISTOGRAM_DECADES   6.0        /* log10(MAX_US / MIN_US)       */
typedef uint32_t HistogramBin;

/* The accumulator. Fields are private to histogram.c — read them through
 * histogram_bins() / histogram_read_stats(). It is a value type rather than an
 * opaque pointer so the owner can hold an array of them with no allocation:
 * cpuprof.c has one per section, statically. Zero-initialize (static storage or
 * histogram_clear) before the first record. */
typedef struct {
    HistogramBin       bins[HISTOGRAM_BIN_COUNT];
    unsigned long long count;
    double             min_us, max_us;
    double             sum;
    /* Welford's online accumulators: mean, and m2 = sum of squared deviations
     * from the running mean. See the HistogramStats block below for why. */
    double             mean;
    double             m2;
} Histogram;

/* --- Running statistics ---
 *
 * Accumulated over the same window as the bins, but from the raw microsecond
 * values rather than read back out of the bins: the bins quantize to ~1.36%
 * and fold their two outer bins into open-ended underflow/overflow buckets, so
 * a bin-derived min/max/mean would be approximate at best and simply wrong at
 * the edges. A sub-microsecond sample stream reports its true mean here while
 * sitting entirely in bin 0.
 *
 * variance_us2 / stddev_us are the *sample* statistics (the n-1 divisor):
 * these are an ongoing sample of a process that keeps running, not a closed
 * population. Both are 0 until the second sample arrives.
 *
 * mean/variance use Welford's online update rather than accumulating a sum of
 * squares — over a long run sum_us^2 dwarfs the spread, and the textbook
 * E[x^2] - E[x]^2 form cancels away the significant digits that carry it
 * (a section that runs 16 ms every frame for an hour would report a negative
 * variance). sum_us is kept as its own plain running total, which is exact
 * enough for a total and is the one figure that cannot be recovered from the
 * others.
 *
 * With no samples yet, every field is 0. */
typedef struct {
    unsigned long long count;      /* samples recorded since the last clear */
    double min_us, max_us;         /* extremes of the raw samples           */
    double sum_us;                 /* running total of every sample         */
    double mean_us;
    double variance_us2;           /* sample variance, in us^2              */
    double stddev_us;              /* sqrt(variance_us2), in us             */
} HistogramStats;

/* Record one sample: bumps its bin (unless that bin has saturated) and folds
 * it into the running statistics (which never saturate — a stalled bin stops
 * counting, the distribution's statistics should not). */
void histogram_record(Histogram *h, double elapsed_us);

/* Zero the bins and the statistics together. */
void histogram_clear(Histogram *h);

/* Copy up to max_bins bin counts into out, low bin first. Returns the number
 * written (0 for a NULL argument or max_bins <= 0). */
int  histogram_bins(const Histogram *h, HistogramBin *out, int max_bins);

/* Derive the statistics. No-op on a NULL argument. */
void histogram_read_stats(const Histogram *h, HistogramStats *out);

/* Bin containing `us`, clamped into [0, HISTOGRAM_BIN_COUNT - 1]. */
int histogram_bin_for_us(double us);

/* The bin's time bounds in us: [lo, hi). bin 0's true lower bound is 0 and
 * the last bin's true upper bound is infinite (see the underflow/overflow
 * note above); these report the nominal log-spaced edges. */
double histogram_bin_lo_us(int bin);
double histogram_bin_hi_us(int bin);

#endif /* HISTOGRAM_H */
