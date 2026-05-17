/*
 * gl_stub_counts.c - storage + helpers for the stub GL/GLU/GLUT call
 * counters declared in tests/gl-stubs/include/GL/gl_stub_counts.h.
 *
 * The file compiles to nothing when GL_STUBS is not
 * defined, so linking it into the normal real-GL build is a no-op.
 */

/* Keep this a non-empty translation unit even when GL_STUBS is
 * undefined: ISO C (enforced by the -std=c99 -pedantic-errors ratchet)
 * rejects an empty TU. Behavior-neutral — just a typedef. */
typedef int gl_stub_counts_tu_nonempty_t;

#ifdef GL_STUBS

#include <GL/gl_stub_counts.h>

#include <locale.h>

unsigned long long gl_stub_counts[GL_STUB_COUNT_MAX];

int g_gl_stub_samples = 4;

FILE *gl_stub_trace_fp = NULL;

static const char *const g_gl_stub_names[GL_STUB_COUNT_MAX] = {
#define GL_STUB_COUNT_NAME_ENTRY(name) [GL_STUB_##name] = #name,
    GL_STUB_COUNTER_LIST(GL_STUB_COUNT_NAME_ENTRY)
#undef GL_STUB_COUNT_NAME_ENTRY
};

const char *gl_stub_count_name(int idx) {
    if (idx < 0 || idx >= GL_STUB_COUNT_MAX) return "?";
    return g_gl_stub_names[idx];
}

void gl_stub_counts_reset(void) {
    for (int i = 0; i < GL_STUB_COUNT_MAX; i++)
        gl_stub_counts[i] = 0;
}

void gl_stub_trace_open(const char *path) {
    gl_stub_trace_close();
    if (!path || !*path) return;
    /* Force C locale for %g so the decimal separator stays '.' on hosts
     * configured otherwise — text traces are diffed byte-for-byte. */
    setlocale(LC_NUMERIC, "C");
    gl_stub_trace_fp = fopen(path, "w");
}

void gl_stub_trace_close(void) {
    if (gl_stub_trace_fp) {
        fclose(gl_stub_trace_fp);
        gl_stub_trace_fp = NULL;
    }
}

void gl_stub_counts_dump(FILE *out, const char *prefix, long long divisor) {
    if (!out) return;
    if (!prefix) prefix = "";
    if (divisor < 1) divisor = 1;

    for (int i = 0; i < GL_STUB_COUNT_MAX; i++) {
        unsigned long long total = gl_stub_counts[i];
        if (total == 0) continue;
        if (divisor == 1) {
            fprintf(out, "%s%-24s %12llu\n", prefix, g_gl_stub_names[i], total);
        } else {
            double per = (double)total / (double)divisor;
            fprintf(out, "%s%-24s %12llu  (%.2f / call)\n",
                    prefix, g_gl_stub_names[i], total, per);
        }
    }
}

#endif /* GL_STUBS */
