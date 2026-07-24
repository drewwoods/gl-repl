/*
 * glr_init_trace.c - Startup stall diagnostic implementation.
 *
 * See glr_init_trace.h for the phase-granularity contract. State is two
 * file statics: the log origin (lazily captured on the first elapsed query)
 * and the detailed-phase flag.
 */
#include "app/boot/glr_init_trace.h"

#include <stdio.h>
#include <sys/time.h>

static double g_init_t0 = -1.0;
static int    g_detailed_prof = 0;

static double init_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

double glr_init_trace_elapsed_seconds(void) {
    if (g_init_t0 < 0.0) g_init_t0 = init_now();
    return init_now() - g_init_t0;
}

void glr_init_trace(const char *phase) {
    fprintf(stderr, "[init +%6.3fs] %s\n", glr_init_trace_elapsed_seconds(), phase);
}

void glr_init_trace_detail(const char *phase) {
    if (g_detailed_prof) glr_init_trace(phase);
}

void glr_init_trace_set_detailed(int on) {
    g_detailed_prof = on ? 1 : 0;
}

int glr_init_trace_detailed(void) {
    return g_detailed_prof;
}
