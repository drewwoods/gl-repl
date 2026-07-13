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

#define RENDER3D_GUIDE_LABEL_MAX_RUNS 2

/* Optional immediate reporting channel for bitmap labels emitted by edit
 * guides. Text pointers only need to remain valid for the duration of the
 * callback. The sink records placement metadata; guide renderers remain the
 * owners of drawing and do not depend on the consumer. */
typedef struct Render3dGuideLabelRun {
    void       *font;
    const char *text;
} Render3dGuideLabelRun;

typedef struct Render3dGuideLabelSpec {
    float pos[3];
    Render3dGuideLabelRun runs[RENDER3D_GUIDE_LABEL_MAX_RUNS];
    int run_count;
} Render3dGuideLabelSpec;

typedef void (*Render3dGuideLabelRecordFn)(
    void *user_data,
    const Render3dGuideLabelSpec *label);

typedef struct Render3dGuideLabelSink {
    Render3dGuideLabelRecordFn record;
    void *user_data;
} Render3dGuideLabelSink;

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

    /* Pre-parsed vertex / raster-position / normal cursor args. The
     * controller evaluates the partial input string (e.g.
     * `glVertex3f(1, t*2, `) using the live REPL variable table and writes
     * the floats here so the scene module can draw guides without needing
     * repl_eval. vertex_n_filled = 0 when the input doesn't look like
     * glVertex2f / glVertex3f / gluVertex(. raster_pos_n_filled < 3 means
     * "don't draw a raster-position marker". normal_n_filled < 3 means
     * "don't draw a normal guide". */
    float vertex_args[3];
    int   vertex_filled[3];
    int   vertex_n_filled;
    float raster_pos_args[3];
    int   raster_pos_n_filled;
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

    /* Optional transformed normal direction for the normal label. Filled by
     * edit_overlays when the cursor's flat-program instance is known and
     * xform_guide_mode is FRAME. This is the authored normal transformed by
     * the in-scope model normal matrix and normalized for display. */
    float normal_frame_args[3];
    int   normal_frame_args_valid;

    /* When the cursor line is glClipPlane(GL_CLIP_PLANEi, ...):
     * clip_plane_idx is the plane slot (0..5; -1 = not a clip-plane
     * line), clip_plane_args are the evaluated equation coefficients
     * (a, b, c, d) typed so far, clip_plane_n_filled counts them, and
     * clip_plane_cap_enabled says whether the program's net
     * glEnable/glDisable state turns that plane's cap on (the guide
     * dims when it's off). Pre-parsed by the controller like the
     * vertex/xform slots; the flat-program walk overrides args from
     * the cursor's flat cmd so animated coefficients track. */
    int   clip_plane_idx;
    float clip_plane_args[4];
    int   clip_plane_n_filled;
    int   clip_plane_cap_enabled;

    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */

    /* Nullable observer for labels drawn by the active edit guide. The
     * controller leaves this empty; edit_overlays installs it only while
     * vertex-label decluttering needs fixed guide-label obstacles. */
    Render3dGuideLabelSink label_sink;
} Render3dGuideSnapshot;

typedef struct Render3dTransformGuidePlan {
    int active;
    int consumed;
    int cursor_flat_idx;
    int after_flat_idx;
} Render3dTransformGuidePlan;

#endif /* RENDER3D_GUIDES_SHARED_H */
