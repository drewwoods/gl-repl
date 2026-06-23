/*
 * guides_shared.h - Shared guide snapshot and planning types.
 *
 * The controller builds a Render3dGuideSnapshot once per frame from the current
 * editor/replay context, then guide renderers consume it without reaching back
 * into editor or REPL globals. Render3dTransformGuidePlan is the small per-frame
 * cache transform guides use between prepare and render.
 */
#ifndef RENDER3D_GUIDES_SHARED_H
#define RENDER3D_GUIDES_SHARED_H

#include "repl/command.h"
#include "repl/flatten.h"
#include "render3d/guides/xform_guide_mode.h"

typedef struct Render3dGuideSnapshot {
    int show_guides;
    int replaying;
    /* Flat-program index of the draw the active replay step emitted, or -1 —
     * a glVertex / gluVertex *or* a glutSolid*. When replaying with guides on,
     * the live transform guide anchors on the transform shaping this draw. Set
     * from replay_focus_anchor_flat_idx(). */
    int replay_focus_anchor_flat_idx;
    Render3dXformGuideMode xform_guide_mode;
    int user_lighting_enabled;
    float anim_time;

    /* `input` is the live editor input buffer text — always a valid C
     * string, possibly empty (input_len == 0), never NULL. Consumers
     * may strncmp / strncpy / strchr against it without a NULL guard.
     * `edit_line_committed_text` MAY be NULL when the cursor is on a
     * line that has no committed source (e.g. a fresh empty
     * scratch row); transform_input_matches_committed handles that. */
    const char *input;
    int input_len;
    int cursor_pos;
    int edit_line_idx;
    int inserting;
    const char *edit_line_committed_text; /* editor buffer text; NULL = no committed line */

    const GLCmd *source_cmds;
    int source_cmd_count;
    FlatProgramView flat_program;

    /* Pre-parsed vertex / normal cursor args. The controller evaluates the
     * partial input string (e.g. `glVertex3f(1, t*2, `) using the live REPL
     * variable table and writes the floats here so the scene module can
     * draw guides without needing repl_eval. vertex_n_filled = 0 when the
     * input doesn't look like glVertex2f / glVertex3f / gluVertex(.
     * normal_n_filled < 3 means "don't draw a normal guide". */
    float vertex_args[3];
    int   vertex_filled[3];
    int   vertex_n_filled;
    float normal_args[3];
    int   normal_n_filled;

    /* Pre-evaluated transform cursor args, parallel to vertex_args. The
     * controller fills these when the live input line is glTranslatef( /
     * glScalef( / glRotatef(, so the transform guide can render live while
     * typing (before commit) with the same feel as the vertex guide. Slots
     * are positional: translate/scale use [0..2]; rotate uses [0]=angle,
     * [1..3]=axis. xform_filled[i] flags the slots the user has actually
     * typed; the scene module fills the rest with the transform identity
     * (0 for translate/rotate, 1 for scale). The transform *kind* is
     * re-derived in the scene module from `input` via strncmp (no eval),
     * mirroring geometry_guides.c's input_is_vertex_kind. */
    float xform_args[4];
    int   xform_filled[4];
    int   xform_n_filled;

    /* When the cursor is on a glNormal3f / gluNormal / CMD_TESS_NORMAL
     * line, the controller looks forward in the *flat* program for the
     * next vertex command and writes its evaluated position here. The
     * normal-guide renderer prefers this over its own forward search
     * through source_cmds — source args are frozen at parse time, so a
     * dynamic wave (where the surrounding x/y/z vars are reassigned
     * each frame inside a loop) would otherwise anchor the normal
     * arrow at the parse-time vertex position instead of the live one.
     * Zeroed / `_valid` = 0 means "fall back to the source search". */
    float normal_base_pos[3];
    int   normal_base_pos_valid;

    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
} Render3dGuideSnapshot;

typedef struct Render3dTransformGuidePlan {
    int active;
    int consumed;
    int cursor_flat_idx;
    int after_flat_idx;
} Render3dTransformGuidePlan;

#endif /* RENDER3D_GUIDES_SHARED_H */