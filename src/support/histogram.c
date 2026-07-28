/*
 * histogram.c - Log-spaced duration histogram with running statistics.
 */
#include "support/histogram.h"

#include <math.h>
#include <stdint.h>

/* Decades per bin. The whole log-bin layout derives from this one constant. */
#define HISTOGRAM_DECADES_PER_BIN \
    (HISTOGRAM_DECADES / (double)HISTOGRAM_BIN_COUNT)

int histogram_bin_for_us(double us) {
    /* Underflow (including zero and the negative that a non-monotonic clock
     * could hand us) folds into bin 0; overflow into the last bin. */
    if (!(us > HISTOGRAM_MIN_US)) return 0;
    if (us >= HISTOGRAM_MAX_US) return HISTOGRAM_BIN_COUNT - 1;

    /* The epsilon makes a bin's exact lower edge land in that bin rather than
     * the one below: histogram_bin_lo_us() reaches the edge through pow(), and
     * log10(pow(10, x)) comes back a hair under x, so the floor would drop it.
     * 1e-9 decades is ~1e-9 of a bin — far below any real timing resolution —
     * so it can only ever fix that boundary. */
    int bin = (int)(log10(us / HISTOGRAM_MIN_US)
                    / HISTOGRAM_DECADES_PER_BIN + 1e-9);
    if (bin < 0) bin = 0;
    if (bin > HISTOGRAM_BIN_COUNT - 1) bin = HISTOGRAM_BIN_COUNT - 1;
    return bin;
}

double histogram_bin_lo_us(int bin) {
    if (bin <= 0) return HISTOGRAM_MIN_US;
    if (bin > HISTOGRAM_BIN_COUNT - 1) bin = HISTOGRAM_BIN_COUNT - 1;
    return HISTOGRAM_MIN_US * pow(10.0, (double)bin * HISTOGRAM_DECADES_PER_BIN);
}

double histogram_bin_hi_us(int bin) {
    if (bin < 0) bin = 0;
    if (bin >= HISTOGRAM_BIN_COUNT - 1) return HISTOGRAM_MAX_US;
    return histogram_bin_lo_us(bin + 1);
}

void histogram_record(Histogram *h, double elapsed_us) {
    int bin;
    double delta, delta2;

    if (!h) return;
    bin = histogram_bin_for_us(elapsed_us);

    if (h->bins[bin] < UINT32_MAX)
        h->bins[bin]++;

    /* Statistics take the raw value, so they stay exact where the bins do not:
     * at the two open-ended edge bins, and below the ~1.36% bin width. The bin
     * saturation above deliberately does not gate this — a saturated bin stops
     * counting, the distribution's statistics should not. */
    if (h->count == 0 || elapsed_us < h->min_us) h->min_us = elapsed_us;
    if (h->count == 0 || elapsed_us > h->max_us) h->max_us = elapsed_us;
    h->sum += elapsed_us;
    h->count++;

    delta   = elapsed_us - h->mean;
    h->mean += delta / (double)h->count;
    delta2  = elapsed_us - h->mean;   /* deviation from the *updated* mean */
    h->m2  += delta * delta2;
}

void histogram_clear(Histogram *h) {
    if (!h) return;
    for (int bin_idx = 0; bin_idx < HISTOGRAM_BIN_COUNT; bin_idx++)
        h->bins[bin_idx] = 0;
    h->count  = 0;
    h->min_us = 0.0;
    h->max_us = 0.0;
    h->sum    = 0.0;
    h->mean   = 0.0;
    h->m2     = 0.0;
}

int histogram_bins(const Histogram *h, HistogramBin *out, int max_bins) {
    if (!h || !out || max_bins <= 0) return 0;
    int n = max_bins < HISTOGRAM_BIN_COUNT ? max_bins : HISTOGRAM_BIN_COUNT;
    for (int i = 0; i < n; i++)
        out[i] = h->bins[i];
    return n;
}

void histogram_read_stats(const Histogram *h, HistogramStats *out) {
    if (!h || !out) return;
    out->count        = h->count;
    out->min_us       = h->min_us;
    out->max_us       = h->max_us;
    out->sum_us       = h->sum;
    out->mean_us      = (h->count > 0) ? h->mean : 0.0;
    /* n-1 divisor: an ongoing sample of a running process, not a population.
     * m2 is a sum of squares and so cannot go negative, but a long run of
     * identical samples can leave it at -0.0; max() keeps sqrt() honest. */
    out->variance_us2 = (h->count > 1)
                      ? ((h->m2 > 0.0) ? h->m2 / (double)(h->count - 1) : 0.0)
                      : 0.0;
    out->stddev_us    = sqrt(out->variance_us2);
}
