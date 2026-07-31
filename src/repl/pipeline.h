/*
 * src/repl/pipeline.h - REPL pipeline and lifecycle operations for the controller.
 *
 * These entry points are driven from glr_ctrl.c and other frame-level
 * orchestration code. They are not test internals.
 */
#ifndef REPL_PIPELINE_H
#define REPL_PIPELINE_H

#include "repl/expr_program.h" /* ReplExprDepMask */

/* Result of bringing the live flat program current. Callers request
 * freshness; this boundary alone chooses no-op, in-place value rebake, or
 * full topology rebuild and owns the matching profiler section. */
typedef enum {
    REPL_FLAT_REFRESH_NONE = 0,
    REPL_FLAT_REFRESH_REBAKE,
    REPL_FLAT_REFRESH_FULL
} ReplFlatRefreshKind;

void repl_flatten_commands(int edit_line_idx);
ReplFlatRefreshKind repl_refresh_flat_program(int edit_line_idx);
/* Transient changes (currently motion-blur time samples) do not enter the
 * normal dirty router. Supply the changed predef-root mask and this uses the
 * same structural-vs-value decision as the pending refresh above. */
ReplFlatRefreshKind repl_refresh_flat_program_for_deps(
    int edit_line_idx, ReplExprDepMask changed_deps);
/* Fold the frame's initial refresh and any accumulation-blur subframe
 * refreshes into one published PROF_FLATTEN / PROF_REBAKE sample. Standalone
 * refresh callers need no scope: they continue to publish one sample per
 * call. The display controller must close every scope it opens. */
void repl_flat_refresh_profile_frame_begin(void);
void repl_flat_refresh_profile_frame_end(void);
/* Auto-normal shading modes. The value is owned by glr_state's config
 * (GLR_CONFIG_AUTO_NORMALS) and passed in by the caller — see the comment
 * on repl_recompute_autonormals. FACE gives every vertex of a primitive
 * its face's normal; SMOOTH averages the faces meeting at each vertex,
 * welding coincident positions within one glBegin block. */
#define REPL_AUTONORMAL_LIST(X) \
    X(OFF,    "Off")            \
    X(FACE,   "Face")           \
    X(SMOOTH, "Smooth")

typedef enum ReplAutoNormalMode {
#define REPL_AUTONORMAL_ENUM_ENTRY(name, str) REPL_AUTONORMAL_##name,
    REPL_AUTONORMAL_LIST(REPL_AUTONORMAL_ENUM_ENTRY)
#undef REPL_AUTONORMAL_ENUM_ENTRY
    REPL_AUTONORMAL_COUNT
} ReplAutoNormalMode;

void repl_recompute_autonormals(int autonormal_mode,
                                int *edit_line_inout);

/* Delete every generated normal row (immediate-mode and tessellator),
 * returning the number removed. Call on the transition to
 * REPL_AUTONORMAL_OFF — not on every OFF-mode recompute; see the comment
 * on the definition. Rows the user typed are left alone. */
int repl_strip_auto_normals(int *edit_line_inout);
void repl_refresh_camera_lines(void);
void repl_refresh_render_state_strings(void);
void repl_ensure_init_bootstrap_ready(void);
void repl_apply_init_bootstrap(void);
void repl_ensure_flat_program_with_live_vars(int edit_line_idx);

/* Predef-value copy/restore helpers are declared in repl/eval.h. */

#endif /* REPL_PIPELINE_H */
