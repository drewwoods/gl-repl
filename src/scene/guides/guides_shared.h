/*
 * guides_shared.h - Shared guide snapshot and planning types.
 *
 * The controller builds a SceneGuideSnapshot once per frame from the current
 * editor/replay context, then guide renderers consume it without reaching back
 * into editor or REPL globals. SceneTransformGuidePlan is the small per-frame
 * cache transform guides use between prepare and render.
 */
#ifndef SCENE_GUIDES_SHARED_H
#define SCENE_GUIDES_SHARED_H

#include "repl/command.h"
#include "repl/flatten.h"

typedef struct SceneGuideSnapshot {
    int show_guides;
    int replaying;
    int xform_guide_mode;
    int user_lighting_enabled;
    float anim_time;

    const char *input;
    int input_len;
    int cursor_pos;
    int edit_line_idx;
    int inserting;
    const char *edit_line_committed_text; /* editor buffer text for edit_line_idx */

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
} SceneGuideSnapshot;

typedef struct SceneTransformGuidePlan {
    int active;
    int consumed;
    int cursor_flat_idx;
    int after_flat_idx;
} SceneTransformGuidePlan;

#endif /* SCENE_GUIDES_SHARED_H */