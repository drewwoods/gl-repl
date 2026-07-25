/*
 * stencil_limits.h - Shared scalar policy for Phase-1 stencil commands.
 *
 * GLCmd stores args as float, so keep the REPL surface within the exactly
 * representable 8-bit range. The helper deliberately truncates like C's
 * GLfloat-to-GLint conversion in glStencilFunc.
 */
#ifndef REPL_STENCIL_LIMITS_H
#define REPL_STENCIL_LIMITS_H

#define REPL_STENCIL_VALUE_MIN 0
#define REPL_STENCIL_VALUE_MAX 255

static inline int repl_stencil_clamp_ref(float value, int *out_value) {
    int in_range = 1;
    int result;

    if (!(value >= (float)REPL_STENCIL_VALUE_MIN)) {
        result = REPL_STENCIL_VALUE_MIN;
        in_range = 0;
    } else if (value > (float)REPL_STENCIL_VALUE_MAX) {
        result = REPL_STENCIL_VALUE_MAX;
        in_range = 0;
    } else {
        result = (int)value; /* C and glStencilFunc both truncate toward zero. */
    }
    if (out_value)
        *out_value = result;
    return in_range;
}

#endif /* REPL_STENCIL_LIMITS_H */
