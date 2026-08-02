/*
 * stencil_limits.h - Shared scalar policy for Phase-1 stencil commands.
 *
 * GLCmd stores args as float, so keep the REPL surface within the exactly
 * representable 8-bit range. The helper deliberately truncates like C's
 * GLfloat-to-GLint conversion in glStencilFunc.
 *
 * Range handling splits by who can still be told about it. A *literal* out
 * of range is a parse rejection; an *animated* ref that leaves the range is
 * clamped silently, because a per-frame parse error is not a usable failure
 * mode for a value that only misbehaves on some frames.
 *
 * That clamp is the one place the REPL and its exported C differ at the API
 * boundary: export writes the expression verbatim, so the standalone program
 * hands glStencilFunc the raw value. The rendered results still agree -
 * GL clamps ref to [0, 2^bits - 1] itself - so this is a difference in the
 * call trace only, and is why the truncation parity tests compare values
 * rather than going through test_export_trace_parity (which compares call
 * counts and would be blind to it either way).
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
