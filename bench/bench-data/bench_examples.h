/* Frozen benchmark scene corpus metadata. */
#ifndef BENCH_EXAMPLES_H
#define BENCH_EXAMPLES_H

typedef struct {
    const char *name;
    const char *const *lines;
} BenchExample;

extern const BenchExample g_bench_examples[];
extern const int g_bench_example_count;

#endif /* BENCH_EXAMPLES_H */
