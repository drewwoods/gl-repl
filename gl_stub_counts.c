/*
 * gl_stub_counts.c — storage + helpers for the stub GL/GLU/GLUT call
 * counters declared in include/GL/gl_stub_counts.h.
 *
 * The file compiles to nothing when OPENGL_VIBE_USE_GL_STUBS is not
 * defined, so linking it into the normal real-GL build is a no-op.
 */

#ifdef OPENGL_VIBE_USE_GL_STUBS

#include <GL/gl_stub_counts.h>

unsigned long long gl_stub_counts[GL_STUB_COUNT_MAX];

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

#endif /* OPENGL_VIBE_USE_GL_STUBS */
