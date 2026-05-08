/*
 * scene_guides_shared.h - shared guide snapshot/planning types.
 */
#ifndef SCENE_GUIDES_SHARED_H
#define SCENE_GUIDES_SHARED_H

#include "repl_command.h"
#include "repl_eval.h"
#include "repl_flatten.h"

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

    const ExprVar *predef_vars;
    int predef_var_count;
    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
} SceneGuideSnapshot;

typedef struct SceneTransformGuidePlan {
    int active;
    int consumed;
    int cursor_flat_idx;
    int after_flat_idx;
} SceneTransformGuidePlan;

#endif /* SCENE_GUIDES_SHARED_H */
