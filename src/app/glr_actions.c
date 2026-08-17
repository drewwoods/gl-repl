/*
 * glr_actions.c -- Side-effecting editor actions and config dispatch.
 *
 * Input modules decide which key or menu row was activated. UI modules decide
 * what was clicked. This module owns the mutation that follows: config-row
 * cycling, F-key/Ctrl-key config shortcuts, startup config defaults, and menu
 * item actions that touch scenes, files, replay, audio, or presentation state.
 */
#include "app/glr_actions.h"
#include "app/glr_extedit.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"                  /* DEFAULT_SCENE_FILE */
#include "app/glr_ctrl.h"            /* glr_ctrl_sync_ui_chrome */
#include "app/glr_ctrl_export.h"
#include "app/glr_mesh_export.h"     /* glr_export_mesh_ply (File -> Export .ply) */
#include "app/glr_state.h"           /* presentation/render storage */
#include "app/glr_defaults.h"        /* CFG_DEFAULT_* (scene-subset baseline) */
#include "app/glr_camera.h"          /* camera focus-origin / reset (eased) */
#include "ui/app/layout.h"           /* CODE_PANEL_LAYOUT_* enum values */
#include "subsystems/color_picker/color_picker_state.h"
#include "app/glr_audio.h"
#include "app/glr_modal.h"
#include "app/glr_workspaces.h"
#include "repl/workspace_io.h"
#include <stdio.h>
#include <errno.h>
#if defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "app/glr_config.h"
#include "app/glr_tours.h"           /* Tours menu catalog + start */
#include "render3d/postprocess_filter.h"
#include "editor/input.h"
#include "editor/commit.h"
#include "editor/completion.h"
#include "keys.h"
#include "c_compat.h"                /* ARRAY_LEN */
#include "repl/help_text.h"
#include "repl/tutorials.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "editor/help_session.h"
#include "repl/pipeline.h"
#include "repl/state_views.h"
#include "repl/time.h"
#include "ui/app/menu_bar.h"
#include "ui/support/memprof.h"
#include "ui/support/cpuprof.h"
#include "ui/app/state.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/undo.h"
#include "render3d/themes.h"
#include "render3d/view_mode.h"         /* RENDER3D_VIEW_LIST - derives the view_mode cfg symbols */
#include "render3d/projection_mode.h"   /* RENDER3D_PROJ_LIST - derives the projection cfg symbols */
#include "render3d/lights.h"           /* render3d_lights_apply_theme, render3d_light_theme_names */
#include "subsystems/edit_overlays/edit_overlays.h"
#include "ui/app/repl_code_panel.h"

static int glr_action_modal_commit(GlrModalKind kind, const char *text,
                                   int context);

#if defined(__APPLE__)
extern FILE *popen(const char *command, const char *mode);
extern int pclose(FILE *stream);
#endif

static const char *replay_mode_names[] = { "Polygon", "Vertex" };
static const char *replay_expand_names[] = { "Off", "Expanded", "Verbose" };
static const char *backdrop_mode_names[RENDER3D_BACKDROP_COUNT] = {
    RENDER3D_BACKDROP_LIST(RENDER3D_BACKDROP_NAME_ENTRY)
};
static const char *xform_guide_mode_names[RENDER3D_XFORM_GUIDE_COUNT] = {
    [RENDER3D_XFORM_GUIDE_OFF]   = "Off",
    [RENDER3D_XFORM_GUIDE_WORLD] = "World",
    [RENDER3D_XFORM_GUIDE_FRAME] = "Frame",
};
static const char *profile_panel_mode_names[] = {
    "Off", "FPS", "Sections", "Histogram"
};
static const char *memory_panel_mode_names[]  = { "Off", "On" };
static const char *code_panel_layout_names[] = {
    "Left", "Top", "Bottom", "Hidden"
};
static const char *grid_theme_names[GRID_THEME_COUNT] = {
    GRID_THEME_LIST(GRID_THEME_NAME_ENTRY)
};
static const char *grid_major_names[GRID_MAJOR_COUNT] = {
    GRID_MAJOR_LIST(GRID_MAJOR_NAME_ENTRY)
};
static const char *grid_extent_names[GRID_EXTENT_COUNT] = {
    GRID_EXTENT_LIST(GRID_EXTENT_NAME_ENTRY)
};
static const char *grid_brightness_names[GRID_BRIGHTNESS_COUNT] = {
    GRID_BRIGHTNESS_LIST(GRID_BRIGHTNESS_NAME_ENTRY)
};
static const char *axes_theme_names[AXES_THEME_COUNT] = {
    AXES_THEME_LIST(AXES_THEME_NAME_ENTRY)
};
static const char *post_fx_scope_names[GLR_POST_FX_SCOPE_COUNT] = {
#define POST_FX_SCOPE_NAME_ENTRY(suffix, name_str, symbol_str) [GLR_POST_FX_SCOPE_##suffix] = name_str,
    POST_FX_SCOPE_LIST(POST_FX_SCOPE_NAME_ENTRY)
#undef POST_FX_SCOPE_NAME_ENTRY
};
static const char *post_fx_effect_names[GLR_POST_FX_EFFECT_COUNT] = {
#define POST_FX_EFFECT_NAME_ENTRY(suffix, render_mode, name_str, symbol_str) [GLR_POST_FX_EFFECT_##suffix] = name_str,
    POST_FX_EFFECT_LIST(POST_FX_EFFECT_NAME_ENTRY)
#undef POST_FX_EFFECT_NAME_ENTRY
};
static char cfg_status_buf[REPL_STATUS_TEXT_MAX];

static const char *workspace_dir_or_app_default(void) {
    const char *dir = repl_workspace_dir();
    if (dir && dir[0])
        return dir;
    return glr_paths_default_workspace_dir();
}

static int ensure_default_managed_workspace(char *err, size_t err_sz) {
    const char *dir = glr_paths_default_workspace_dir();
    WorkspaceManifest manifest;
    if (workspace_io_manifest_exists(dir))
        return workspace_io_manifest_read(dir, &manifest, err, err_sz);
    if (!glr_paths_ensure_dir(dir, NULL)) {
        snprintf(err, err_sz, "Could not create default workspace");
        return 0;
    }
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "%s",
             GLR_DEFAULT_WORKSPACE_NAME);
    return workspace_io_manifest_write(dir, &manifest, err, err_sz);
}

static int bind_app_workspace_for_scene_save_if_needed(void) {
    const char *dir = repl_workspace_dir();
    if (dir && dir[0])
        return 1;
    if (glr_paths_cwd_supports_relative_saves())
        return 1;
    char err[REPL_STATUS_TEXT_MAX];
    if (ensure_default_managed_workspace(err, sizeof(err))) {
        repl_set_workspace_dir(glr_paths_default_workspace_dir());
        return 1;
    }
    repl_set_status_error(err);
    return 0;
}

/* The UI presents examples and retained post-tutorial documents as tabs even
 * though they remain transient until the first edit. A user-facing workspace
 * save must therefore promote that visible tab before serializing the scene
 * catalog; otherwise the low-level writer quite correctly sees zero occupied
 * user-scene slots and commits an empty manifest. Keep this policy in the app
 * action layer so repl_save_workspace() remains a literal catalog writer. */
static int save_workspace_including_visible_scene(
    const char *dir, const ReplExportLayout *layout, int adopt_bindings) {
    int promotable = repl_active_user_scene() < 0 &&
        (repl_state_active_example_idx() >= 0 ||
         repl_state_tutorial_origin_idx() >= 0);
    int written;
    if (promotable && repl_promote_transient_if_needed() < 0)
        return -1;
    written = repl_save_workspace(dir, layout);
    if (written >= 0) {
        if (adopt_bindings)
            repl_scenes_adopt_workspace_bindings();
        glr_extedit_note_saved();
    }
    return written;
}

/* Unified audio cfg: two-state on/off toggle.
 * Indices:
 *   0 = Off  - paused
 *   1 = On   - playing, preserving the current loop mode
 * Old 4-state ini values (1/2/3) are all remapped to On in the audio-session
 * restore performed by glr_actions_apply_defaults(), via the > AUDIO_CFG_ALL
 * clamp. */
static const char *syntax_hl_names[SYNTAX_HIGHLIGHT_COUNT] = {
    SYNTAX_HIGHLIGHT_LIST(SYNTAX_HIGHLIGHT_NAME_ENTRY)
};
static const char *view_mode_names[] = { "3D", "2D" };
static const char *projection_names[] = { "Perspective", "Ortho" };
static const char *wireframe_mode_names[] = { "Off", "Wireframe", "Hidden-line" };
static const char *vertex_label_names[OVERLAY_VERTEX_LABEL_COUNT] = {
    OVERLAY_VERTEX_LABEL_LIST(OVERLAY_VERTEX_LABEL_NAME_ENTRY)
};
static const char *vertex_outline_style_names[VERTEX_OUTLINE_STYLE_COUNT] = {
    VERTEX_OUTLINE_STYLE_LIST(VERTEX_OUTLINE_STYLE_NAME_ENTRY)
};

static const char *poly_highlight_names[POLY_HIGHLIGHT_COUNT] = {
    POLY_HIGHLIGHT_LIST(POLY_HIGHLIGHT_NAME_ENTRY)
};

static int audio_menu_group_differs(const char *a, const char *b) {
    if (a == b)
        return 0;
    if (!a || !b)
        return 1;
    return strcmp(a, b) != 0;
}

/* Mirror of menu_bar.c's audio_visible_group_count(): the number of
 * source-group parent rows the Audio dropdown renders. glr_action_menu_
 * item_activate subtracts this from the clicked item_idx to recover the
 * control-row offset (Play/Next/Prev/Loop), so it MUST match the menu's
 * layout count exactly. Both read the same glr_audio_track_* accessors on
 * the main thread; keep the two in lockstep if the grouping rule changes. */
static int audio_menu_group_count(void) {
    int count = glr_audio_track_count();
    int groups = 0;
    const char *prev = NULL;
    for (int i = 0; i < count; i++) {
        const char *group = glr_audio_track_group(i);
        if (!group || !group[0])
            group = "Music";
        if (i == 0 || audio_menu_group_differs(group, prev))
            groups++;
        prev = group;
    }
    return groups;
}

static const char *audio_loop_status_label(int mode) {
    if (mode == GLR_AUDIO_LOOP_OFF)
        return "Off";
    if (mode == GLR_AUDIO_LOOP_SONG)
        return "Song";
    return "All";
}
static const char *overlay_scope_names[OVERLAY_SCOPE_COUNT] = {
    OVERLAY_SCOPE_LIST(OVERLAY_SCOPE_NAME_ENTRY)
};
/* Where labels sit, orthogonal to which vertices get one (overlay_scope_names
 * above) -- the same split as Vertex outlines vs Vertex outline style. */
static const char *vertex_label_placement_names[OVERLAY_LABEL_PLACEMENT_COUNT] = {
    OVERLAY_LABEL_PLACEMENT_LIST(OVERLAY_LABEL_PLACEMENT_NAME_ENTRY)
};
/* Accumulation buffer split into two rows: the effect mode (Off / AA
 * jitter / motion Blur) and the sample/pass count. The passes cycle maps
 * its state index to an actual count on the supported ladder in
 * glr_config.c (accum_passes_*_cycle); these labels and that int table
 * both expand from GLR_ACCUM_PASS_LADDER (glr_config.h). */
static const char *accum_effect_names[] = { "Off", "AA", "Blur", "Blur Cam" };
/* Depth view states mirror BufferVizDepthMode (subsystems/buffer_viz/depth_viz.h): Linear
 * maps eye depth across the full near/far range, Scene normalizes to
 * the user geometry's own depth extent, Split overlays the right half
 * of the scene with the scene-normalized image. */
static const char *depth_viz_names[] = { "Off", "Linear", "Scene", "Split" };
/* Stencil view states mirror BufferVizStencilMode (stencil_viz.h). */
static const char *stencil_viz_names[] = { "Off", "Palette", "Ramp", "Split" };
static const char *accum_passes_names[] = { GLR_ACCUM_PASS_LADDER(GLR_ACCUM_PASS_NAME_ENTRY) };
/* Auto-normal states come straight off ReplAutoNormalMode
 * (repl/pipeline.h): Face gives each primitive one normal, Smooth averages
 * the faces meeting at a vertex. */
static const char *auto_normal_names[REPL_AUTONORMAL_COUNT] = {
#define AUTO_NORMAL_NAME_ENTRY(name, str) [REPL_AUTONORMAL_##name] = str,
    REPL_AUTONORMAL_LIST(AUTO_NORMAL_NAME_ENTRY)
#undef AUTO_NORMAL_NAME_ENTRY
};

/* Hidden session toggles - intentionally NOT rows in this table (no
 * menu entry, no keyboard-shortcut field here, no @cfg persistence).
 * They are session-only state flipped by a dedicated router handler,
 * mirroring the F1 help overlay. Listed here because g_cfg_items[] is
 * the table a reader scans for "what config exists"; these live
 * elsewhere on purpose:
 *
 *   Ctrl+Shift+F  code-panel focus (hide boilerplate chrome)
 *                 -> glr_ctrl_router_handle_code_focus_key /
 *                    glr_ctrl_toggle_code_focus (glr_ctrl.c);
 *                    GlrPresentationState.code_focus; also surfaced as
 *                    the clickable statusbar "focus" keycap
 *                    (UI_HIT_CODE_FOCUS_TOGGLE) and the F1 help catalog
 *                    (src/repl/help_text.c).
 *
 * This binding is defined in keymap.h (GLR_CODE_FOCUS). */

/* Runtime display label for the MSAA row ("MSAAx<n>" once the GL sample
 * count is known); set by glr_actions_set_msaa_label(). */
static const char *g_msaa_display_label = NULL;

/* Designated initializers keep each row down to the fields it uses. Every
 * actionable row also declares its explicit persistence slug; labels are
 * presentation text and must not become serialized identifiers:
 * chrome rows ("### " headers / "---" separators) set only .label +
 * .section_header; rows with no keyboard shortcut leave key_code /
 * modifiers / is_special at 0. A keymap.h binding pair `GLR_X`
 * (= key, mods) is split per-field with KM_KEY() / KM_MODS();
 * .is_special = 1 marks the GLUT special-callback (F-key) bindings. */
const GlrConfigItem g_cfg_items[] = {
    { .label = "### RENDERING", .section_header = 1 },
    { .label = "MSAA", .slug = "msaa", .key = GLR_CONFIG_MSAA, .state_count = 2,
      .key_code = KM_KEY(GLR_MSAA), .modifiers = KM_MODS(GLR_MSAA),
      .display_label_override = &g_msaa_display_label },
    { .label = "Line smooth", .slug = "line_smooth", .key = GLR_CONFIG_LINE_SMOOTH, .state_count = 2,
      .key_code = KM_KEY(GLR_LINE_SMOOTH), .modifiers = KM_MODS(GLR_LINE_SMOOTH) },
    { .label = "Accum effect", .slug = "accum_effect", .key = GLR_CONFIG_ACCUM_EFFECT,
      .state_count = ARRAY_LEN(accum_effect_names), .state_names = accum_effect_names,
      .key_code = KM_KEY(GLR_ACCUM_EFFECT), .modifiers = KM_MODS(GLR_ACCUM_EFFECT) },
    { .label = "Accum passes", .slug = "accum_passes", .key = GLR_CONFIG_ACCUM_PASSES,
      .state_count = ARRAY_LEN(accum_passes_names), .state_names = accum_passes_names },
    { .label = "Point attenuation", .slug = "point_attenuation", .key = GLR_CONFIG_POINT_ATTENUATION, .state_count = 2 },
    { .label = "Post FX Scope", .slug = "post_fx_scope", .key = GLR_CONFIG_POST_FX_SCOPE,
      .state_count = GLR_POST_FX_SCOPE_COUNT, .state_names = post_fx_scope_names,
      .key_code = KM_KEY(GLR_POST_FX_SCOPE_CYCLE), .modifiers = KM_MODS(GLR_POST_FX_SCOPE_CYCLE),
      .is_special = 1 },
    { .label = "Post FX Effect", .slug = "post_fx_effect", .key = GLR_CONFIG_POST_FX_EFFECT,
      .state_count = GLR_POST_FX_EFFECT_COUNT, .state_names = post_fx_effect_names },
    { .label = "---", .section_header = 1 },

    { .label = "### TIME & REPLAY", .section_header = 1 },
    { .label = "Auto time", .slug = "auto_time", .key = GLR_CONFIG_AUTO_TIME, .state_count = 2,
      .key_code = KM_KEY(GLR_AUTO_TIME), .modifiers = KM_MODS(GLR_AUTO_TIME) },
    { .label = "Replay", .slug = "replay", .key = GLR_CONFIG_REPLAY, .state_count = 2,
      .key_code = KM_KEY(GLR_REPLAY), .modifiers = KM_MODS(GLR_REPLAY) },
    { .label = "Replay mode", .slug = "replay_mode", .key = GLR_CONFIG_REPLAY_MODE,
      .state_count = ARRAY_LEN(replay_mode_names), .state_names = replay_mode_names },
    { .label = "Replay expand", .slug = "replay_expand", .key = GLR_CONFIG_REPLAY_EXPAND,
      .state_count = REPLAY_EXPAND_COUNT, .state_names = replay_expand_names },
    { .label = "---", .section_header = 1 },

    { .label = "### SCENE", .section_header = 1 },
    { .label = "Grid", .slug = "grid", .key = GLR_CONFIG_GRID_THEME,
      .state_count = GRID_THEME_COUNT, .state_names = grid_theme_names,
      .key_code = KM_KEY(GLR_GRID), .modifiers = KM_MODS(GLR_GRID), .is_special = 1 },
    { .label = "Grid major", .slug = "grid_major", .key = GLR_CONFIG_GRID_MAJOR,
      .state_count = GRID_MAJOR_COUNT, .state_names = grid_major_names,
      .key_code = KM_KEY(GLR_GRID_MAJOR), .modifiers = KM_MODS(GLR_GRID_MAJOR) },
    { .label = "Grid extent", .slug = "grid_extent", .key = GLR_CONFIG_GRID_EXTENT,
      .state_count = GRID_EXTENT_COUNT, .state_names = grid_extent_names,
      .key_code = KM_KEY(GLR_GRID_EXTENT), .modifiers = KM_MODS(GLR_GRID_EXTENT),
      .is_special = 1 },
    { .label = "Grid brightness", .slug = "grid_brightness", .key = GLR_CONFIG_GRID_BRIGHTNESS,
      .state_count = GRID_BRIGHTNESS_COUNT, .state_names = grid_brightness_names,
      .key_code = KM_KEY(GLR_GRID_BRIGHTNESS), .modifiers = KM_MODS(GLR_GRID_BRIGHTNESS),
      .is_special = 1 },
    { .label = "Axes", .slug = "axes", .key = GLR_CONFIG_AXES_THEME,
      .state_count = AXES_THEME_COUNT, .state_names = axes_theme_names,
      .key_code = KM_KEY(GLR_AXES), .modifiers = KM_MODS(GLR_AXES), .is_special = 1 },
    { .label = "Backdrop", .slug = "backdrop", .key = GLR_CONFIG_BACKDROP,
      .state_count = RENDER3D_BACKDROP_COUNT, .state_names = backdrop_mode_names,
      .key_code = KM_KEY(GLR_BACKDROP), .modifiers = KM_MODS(GLR_BACKDROP), .is_special = 1 },
    { .label = "Light theme", .slug = "light_theme", .key = GLR_CONFIG_LIGHT_THEME,
      .state_count = LIGHT_THEME_COUNT, .state_names = render3d_light_theme_names,
      .key_code = KM_KEY(GLR_LIGHT_THEME), .modifiers = KM_MODS(GLR_LIGHT_THEME),
      .is_special = 1 },
    { .label = "Light indicators", .slug = "light_indicators", .key = GLR_CONFIG_LIGHT_INDICATORS, .state_count = 2,
      .key_code = KM_KEY(GLR_LIGHT_INDICATORS), .modifiers = KM_MODS(GLR_LIGHT_INDICATORS) },
    { .label = "---", .section_header = 1 },

    { .label = "### CAMERA", .section_header = 1 },
    { .label = "View mode", .slug = "view_mode", .key = GLR_CONFIG_ORTHO_MODE,
      .state_count = ARRAY_LEN(view_mode_names), .state_names = view_mode_names,
      .key_code = KM_KEY(GLR_VIEW_MODE), .modifiers = KM_MODS(GLR_VIEW_MODE) },
    /* Projection matrix (perspective/ortho) with a free camera - distinct
     * from "View mode", which flattens & locks the camera to a top-down 2D
     * view. Keybound to Ctrl+Shift+E. */
    { .label = "Projection", .slug = "projection", .key = GLR_CONFIG_PROJECTION,
      .state_count = PROJ_COUNT, .state_names = projection_names,
      .key_code = KM_KEY(GLR_PROJECTION), .modifiers = KM_MODS(GLR_PROJECTION) },
    { .label = "Camera rotate", .slug = "camera_rotate", .key = GLR_CONFIG_CAMERA_ROTATE, .state_count = 2,
      .key_code = KM_KEY(GLR_CAMERA_ROTATE), .modifiers = KM_MODS(GLR_CAMERA_ROTATE) },
    /* Action rows: no state to cycle (state_count 0); activation fires
     * the camera move. */
    { .label = "Focus origin", .slug = "focus_origin", .key = GLR_CONFIG_FOCUS_ORIGIN,
      .key_code = KM_KEY(GLR_FOCUS_ORIGIN), .modifiers = KM_MODS(GLR_FOCUS_ORIGIN) },
    /* Head-on view down Z, zoom preserved. */
    { .label = "Look down Z", .slug = "look_down_z", .key = GLR_CONFIG_LOOK_DOWN_Z,
      .key_code = KM_KEY(GLR_LOOK_DOWN_Z), .modifiers = KM_MODS(GLR_LOOK_DOWN_Z) },
    { .label = "Reset camera", .slug = "reset_camera", .key = GLR_CONFIG_RESET_CAMERA,
      .key_code = KM_KEY(GLR_RESET_CAMERA), .modifiers = KM_MODS(GLR_RESET_CAMERA) },
    { .label = "---", .section_header = 1 },

    { .label = "### GEOMETRY", .section_header = 1 },
    { .label = "Wireframe", .slug = "wireframe", .key = GLR_CONFIG_WIREFRAME,
      .state_count = WIREFRAME_COUNT, .state_names = wireframe_mode_names,
      .key_code = KM_KEY(GLR_WIREFRAME), .modifiers = KM_MODS(GLR_WIREFRAME) },
    { .label = "Winding", .slug = "winding", .key = GLR_CONFIG_WINDING_VIEW, .state_count = 2,
      .key_code = KM_KEY(GLR_WINDING_VIEW), .modifiers = KM_MODS(GLR_WINDING_VIEW) },
    { .label = "Depth view", .slug = "depth_view", .key = GLR_CONFIG_DEPTH_VIZ,
      .state_count = ARRAY_LEN(depth_viz_names), .state_names = depth_viz_names,
      .key_code = KM_KEY(GLR_DEPTH_VIZ), .modifiers = KM_MODS(GLR_DEPTH_VIZ) },
    { .label = "Stencil view", .slug = "stencil_view", .key = GLR_CONFIG_STENCIL_VIZ,
      .state_count = ARRAY_LEN(stencil_viz_names), .state_names = stencil_viz_names,
      .key_code = KM_KEY(GLR_STENCIL_VIZ), .modifiers = KM_MODS(GLR_STENCIL_VIZ) },
    { .label = "Call depth", .slug = "call_depth_tint", .key = GLR_CONFIG_CALL_DEPTH_TINT,
      .state_count = 2,
      .key_code = KM_KEY(GLR_CALL_DEPTH_TINT), .modifiers = KM_MODS(GLR_CALL_DEPTH_TINT) },
    { .label = "Auto-normals", .slug = "auto_normals", .key = GLR_CONFIG_AUTO_NORMALS,
      .state_count = ARRAY_LEN(auto_normal_names), .state_names = auto_normal_names },
    { .label = "---", .section_header = 1 },

    { .label = "### OVERLAYS", .section_header = 1 },
    { .label = "Overlay scope", .slug = "overlay_scope", .key = GLR_CONFIG_OVERLAY_SCOPE,
      .state_count = OVERLAY_SCOPE_COUNT,
      .state_names = overlay_scope_names,
      .key_code = KM_KEY(GLR_OVERLAY_SCOPE), .modifiers = KM_MODS(GLR_OVERLAY_SCOPE),
      .is_special = 1 },
    { .label = "Vertex labels", .slug = "vertex_labels", .key = GLR_CONFIG_VERTEX_LABELS,
      .state_count = OVERLAY_VERTEX_LABEL_COUNT, .state_names = vertex_label_names,
      .key_code = KM_KEY(GLR_VERTEX_LABELS), .modifiers = KM_MODS(GLR_VERTEX_LABELS),
      .is_special = 1 },
    { .label = "Vertex label placement", .slug = "vertex_label_placement", .key = GLR_CONFIG_VERTEX_LABEL_PLACEMENT,
      .state_count = OVERLAY_LABEL_PLACEMENT_COUNT,
      .state_names = vertex_label_placement_names },
    { .label = "Vertex points", .slug = "vertex_points", .key = GLR_CONFIG_VERTEX_POINTS, .state_count = 2,
      .key_code = KM_KEY(GLR_VERTEX_POINTS), .modifiers = KM_MODS(GLR_VERTEX_POINTS) },
    { .label = "Vertex outlines", .slug = "vertex_outlines", .key = GLR_CONFIG_VERTEX_OUTLINES, .state_count = 2,
      .key_code = KM_KEY(GLR_VERTEX_OUTLINES), .modifiers = KM_MODS(GLR_VERTEX_OUTLINES) },
    { .label = "Vertex outline style", .slug = "vertex_outline_style", .key = GLR_CONFIG_VERTEX_OUTLINE_STYLE,
      .state_count = VERTEX_OUTLINE_STYLE_COUNT, .state_names = vertex_outline_style_names },
    { .label = "Normal vectors", .slug = "normal_vectors", .key = GLR_CONFIG_NORMAL_VECTORS, .state_count = 2,
      .key_code = KM_KEY(GLR_NORMAL_VECTORS), .modifiers = KM_MODS(GLR_NORMAL_VECTORS) },
    { .label = "Polygon highlight", .slug = "polygon_highlight", .key = GLR_CONFIG_POLY_HIGHLIGHT,
      .state_count = POLY_HIGHLIGHT_COUNT, .state_names = poly_highlight_names,
      .key_code = KM_KEY(GLR_POLY_HIGHLIGHT), .modifiers = KM_MODS(GLR_POLY_HIGHLIGHT) },
    { .label = "Transform guides", .slug = "transform_guides", .key = GLR_CONFIG_XFORM_GUIDE_MODE,
      .state_count = RENDER3D_XFORM_GUIDE_COUNT, .state_names = xform_guide_mode_names,
      .key_code = KM_KEY(GLR_XFORM_GUIDES), .modifiers = KM_MODS(GLR_XFORM_GUIDES) },
    { .label = "---", .section_header = 1 },

    { .label = "### INTERFACE", .section_header = 1 },
    { .label = "Variable panel", .slug = "variable_panel", .key = GLR_CONFIG_VARIABLE_PANEL, .state_count = 2,
      .key_code = KM_KEY(GLR_VARIABLE_PANEL), .modifiers = KM_MODS(GLR_VARIABLE_PANEL) },
    { .label = "Compute profile", .slug = "compute_profile", .key = GLR_CONFIG_CPU_PROFILE,
      .state_count = PROFILE_PANEL_MODE_COUNT, .state_names = profile_panel_mode_names,
      .key_code = KM_KEY(GLR_CPU_PROFILE), .modifiers = KM_MODS(GLR_CPU_PROFILE) },
    /* Ctrl+Shift+B is the memory-profile shortcut; exact ASCII matching
     * keeps Ctrl+B on Code panel and Ctrl+Shift+B here. */
    { .label = "Memory profile", .slug = "memory_profile", .key = GLR_CONFIG_MEMORY_PROFILE,
      .state_count = MEMORY_PANEL_MODE_COUNT, .state_names = memory_panel_mode_names,
      .key_code = KM_KEY(GLR_MEMORY_PROFILE), .modifiers = KM_MODS(GLR_MEMORY_PROFILE) },
    { .label = "Code panel", .slug = "code_panel", .key = GLR_CONFIG_CODE_PANEL_LAYOUT,
      .state_count = CODE_PANEL_LAYOUT_COUNT, .state_names = code_panel_layout_names,
      .key_code = KM_KEY(GLR_CODE_PANEL), .modifiers = KM_MODS(GLR_CODE_PANEL) },
    { .label = "Wrap at commas", .slug = "wrap_at_commas", .key = GLR_CONFIG_WRAP_AT_COMMA, .state_count = 2 },
    { .label = "Syntax highlight", .slug = "syntax_highlight", .key = GLR_CONFIG_SYNTAX_HIGHLIGHT,
      .state_count = SYNTAX_HIGHLIGHT_COUNT, .state_names = syntax_hl_names,
      .key_code = KM_KEY(GLR_SYNTAX_HL), .modifiers = KM_MODS(GLR_SYNTAX_HL) },
    { .label = "Paren match", .slug = "paren_match", .key = GLR_CONFIG_PAREN_MATCH, .state_count = 2 },
    { .label = "Paren scope", .slug = "paren_scope", .key = GLR_CONFIG_PAREN_SCOPE, .state_count = 2 },
};

const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items) / sizeof(g_cfg_items[0]));

/* ---- Export-config bridge --------------------------------------------- *
 *
 * Lets src/repl/export.c emit/parse @cfg header lines without touching
 * glr_config_* directly; src/repl/scenes.c uses the same bridge for
 * per-scene cfg snapshots. The controller owns the bridge so the REPL
 * modules can stay independent of app config storage. */

#include "repl/cfg_baseline.h"

/* THE scene-local config roster: the cfg keys a scene owns (presentation
 * toggles plus camera_rotate and the variable panel), each with the default
 * value glr_ctrl_reset_example_chrome() lands on before a scene's own leading
 * `@cfg` is applied. The controller owns this knowledge so src/repl/scenes.c
 * stays neutral.
 *
 * Membership and defaults are one table on purpose - cfg_key_in_scene_subset()
 * is derived from it below, so a new per-scene key cannot end up saved but
 * never reset (or reset but never saved). Values come from the CFG_DEFAULT_*
 * macros in glr_defaults.h (the single source of truth), never from literals,
 * so a default change moves both the reset and this table together.
 *
 * The third shape of the same roster is
 * glr_state_presentation_reset_example_defaults() in glr_state.c, which writes
 * the GlrPresentationState fields directly (a pure-storage reset must not run
 * glr_config_set()'s side effects) plus the two peer writes for camera
 * autorotation and variable-panel visibility in glr_ctrl_reset_example_chrome().
 * test_glr_ctrl.c pins that reset against this table, key by key
 * (test_scene_local_reset_covers_whole_roster); test_glr_actions.c pins the
 * subset/defaults bag agreement the .glr writer diffs against. */
typedef struct {
    GlrConfigKey key;
    int          value;
} GlrCfgSceneDefault;

static const GlrCfgSceneDefault k_cfg_scene_defaults[] = {
    /* key,                                  value */
    { GLR_CONFIG_WIREFRAME,                  CFG_DEFAULT_WIREFRAME               },
    { GLR_CONFIG_GRID_THEME,                 CFG_DEFAULT_GRID_THEME              },
    { GLR_CONFIG_GRID_MAJOR,                 CFG_DEFAULT_GRID_MAJOR_IDX          },
    { GLR_CONFIG_GRID_EXTENT,                CFG_DEFAULT_GRID_EXTENT_IDX         },
    { GLR_CONFIG_GRID_BRIGHTNESS,            CFG_DEFAULT_GRID_BRIGHTNESS_IDX     },
    { GLR_CONFIG_AXES_THEME,                 CFG_DEFAULT_AXES_THEME              },
    { GLR_CONFIG_VERTEX_LABELS,              CFG_DEFAULT_VERTEX_LABELS           },
    { GLR_CONFIG_VERTEX_LABEL_PLACEMENT,     CFG_DEFAULT_VERTEX_LABEL_PLACEMENT  },
    { GLR_CONFIG_OVERLAY_SCOPE,              CFG_DEFAULT_OVERLAY_SCOPE           },
    { GLR_CONFIG_NORMAL_VECTORS,             CFG_DEFAULT_NORMAL_VECTORS          },
    { GLR_CONFIG_VERTEX_OUTLINES,            CFG_DEFAULT_VERTEX_OUTLINES         },
    { GLR_CONFIG_VERTEX_OUTLINE_STYLE,       CFG_DEFAULT_VERTEX_OUTLINE_STYLE    },
    { GLR_CONFIG_VERTEX_POINTS,              CFG_DEFAULT_VERTEX_POINTS           },
    { GLR_CONFIG_POLY_HIGHLIGHT,             CFG_DEFAULT_HIGHLIGHT_POLY          },
    { GLR_CONFIG_XFORM_GUIDE_MODE,           CFG_DEFAULT_XFORM_GUIDE_MODE        },
    { GLR_CONFIG_LIGHT_INDICATORS,           CFG_DEFAULT_LIGHT_INDICATORS        },
    { GLR_CONFIG_LIGHT_THEME,                CFG_DEFAULT_LIGHT_THEME             },
    { GLR_CONFIG_BACKDROP,                   CFG_DEFAULT_BACKDROP_MODE           },
    { GLR_CONFIG_ORTHO_MODE,                 CFG_DEFAULT_ORTHO_MODE              },
    { GLR_CONFIG_PROJECTION,                 CFG_DEFAULT_PROJECTION              },
    { GLR_CONFIG_CAMERA_ROTATE,              CFG_DEFAULT_CAMERA_ROTATE           },
    { GLR_CONFIG_VARIABLE_PANEL,             CFG_DEFAULT_VARIABLE_PANEL          },
};

static int cfg_scene_default_for_key(GlrConfigKey key, int *out_value) {
    for (int i = 0; i < (int)ARRAY_LEN(k_cfg_scene_defaults); i++) {
        if (k_cfg_scene_defaults[i].key != key)
            continue;
        if (out_value)
            *out_value = k_cfg_scene_defaults[i].value;
        return 1;
    }
    return 0;
}

/* Membership *is* having a row in the roster above - deliberately not a
 * second switch to keep in sync. */
static int cfg_key_in_scene_subset(GlrConfigKey key) {
    if (key == GLR_CONFIG_NONE)
        return 0;
    return cfg_scene_default_for_key(key, NULL);
}

static void glr_export_cfg_normalize_legacy_alias(const char **slug, int *val,
                                                  char *slug_buf,
                                                  size_t slug_buf_sz) {
    if (!slug || !*slug || !val || !slug_buf || slug_buf_sz == 0)
        return;

    if (strcmp(*slug, "top_code_panel") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "code_panel");
        *slug = slug_buf;
        *val = *val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT;
    }

    /* The "CPU profile" row was renamed "Compute profile" (it carries GPU
     * timings too); files saved before the rename still say cpu_profile.
     * Those files also predate the FPS-plot mode inserted at level 1, so
     * their non-zero values shift up one (old On -> Sections, old
     * Details -> Histogram). */
    if (strcmp(*slug, "cpu_profile") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "compute_profile");
        *slug = slug_buf;
        if (*val >= 1) {
            *val += 1;
            if (*val >= PROFILE_PANEL_MODE_COUNT)
                *val = PROFILE_PANEL_MODE_COUNT - 1;
        }
    }

    if (strcmp(*slug, "label_highlight_scope") == 0 ||
        strcmp(*slug, "label_scope") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "overlay_scope");
        *slug = slug_buf;
    }

    if (strcmp(*slug, "xform_guides") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "transform_guides");
        *slug = slug_buf;
    }

    if (strcmp(*slug, "poly_highlight") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "polygon_highlight");
        *slug = slug_buf;
    }
}

static const GlrConfigItem *glr_export_cfg_find_item_by_slug(const char *slug) {
    if (!slug)
        return NULL;

    int dummy_val = 1;
    char normalized_slug[REPL_CFG_KEY_MAX];
    glr_export_cfg_normalize_legacy_alias(&slug, &dummy_val,
                                          normalized_slug,
                                          sizeof(normalized_slug));

    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE)
            continue;
        if (strcmp(glr_config_item_slug(item), slug) == 0)
            return item;
    }
    return NULL;
}

static int glr_export_cfg_slug_is_hidden_audio(const char *slug) {
    return slug && strcmp(slug, "audio") == 0;
}

/* Symbolic-name -> enum-value tables. Built-in catalogs (examples,
 * tutorials) record values like "GRID_THEME_RADAR" rather than the raw
 * `10`; this lookup resolves them at apply time so reordering the
 * underlying enum (src/render3d/themes.h) doesn't silently shift which
 * value the catalog selects.
 *
 * The slug->table map below covers every enum-valued slug the catalogs
 * actually use today (grid / axes / grid_extent / grid_major / backdrop /
 * light_theme / view_mode / overlay_scope / vertex_labels / vertex_outline_style).
 * Other enum-shaped slugs - replay, code_panel_layout, etc. - stay integer-only
 * in their saved form because no catalog literal carries them
 * symbolically. Add a table here if a new catalog needs symbolic
 * support for one of those slugs.
 *
 * Each table is keyed by enum index so STATIC_ASSERT can pin
 * length-against-count, catching "new enum value but missed the
 * table" at build time. */
/* The string tables share their entry list with the enum definitions
 * in src/render3d/themes.h via X-macros, so adding a theme/backdrop there
 * adds its symbol here automatically. */
static const char *cfg_grid_theme_symbols[GRID_THEME_COUNT] = {
#define GRID_THEME_SYMBOL_ENTRY(name, str) [GRID_THEME_##name] = "GRID_THEME_" #name,
    GRID_THEME_LIST(GRID_THEME_SYMBOL_ENTRY)
#undef GRID_THEME_SYMBOL_ENTRY
};
static const char *cfg_axes_theme_symbols[AXES_THEME_COUNT] = {
#define AXES_THEME_SYMBOL_ENTRY(name, str) [AXES_THEME_##name] = "AXES_THEME_" #name,
    AXES_THEME_LIST(AXES_THEME_SYMBOL_ENTRY)
#undef AXES_THEME_SYMBOL_ENTRY
};
static const char *cfg_grid_extent_symbols[GRID_EXTENT_COUNT] = {
#define GRID_EXTENT_SYMBOL_ENTRY(name, str) [GRID_EXTENT_##name] = "GRID_EXTENT_" #name,
    GRID_EXTENT_LIST(GRID_EXTENT_SYMBOL_ENTRY)
#undef GRID_EXTENT_SYMBOL_ENTRY
};
static const char *cfg_grid_major_symbols[GRID_MAJOR_COUNT] = {
#define GRID_MAJOR_SYMBOL_ENTRY(name, str) [GRID_MAJOR_##name] = "GRID_MAJOR_" #name,
    GRID_MAJOR_LIST(GRID_MAJOR_SYMBOL_ENTRY)
#undef GRID_MAJOR_SYMBOL_ENTRY
};
static const char *cfg_grid_brightness_symbols[GRID_BRIGHTNESS_COUNT] = {
#define GRID_BRIGHTNESS_SYMBOL_ENTRY(name, str) [GRID_BRIGHTNESS_##name] = "GRID_BRIGHTNESS_" #name,
    GRID_BRIGHTNESS_LIST(GRID_BRIGHTNESS_SYMBOL_ENTRY)
#undef GRID_BRIGHTNESS_SYMBOL_ENTRY
};
/* RENDER3D_VIEW_LIST is single-arg - X(name) - unlike the (name, str) theme
 * lists above, so its symbol-entry macro takes one parameter. */
static const char *cfg_view_mode_symbols[RENDER3D_VIEW_COUNT] = {
#define SCENE_VIEW_SYMBOL_ENTRY(name) [RENDER3D_VIEW_##name] = "RENDER3D_VIEW_" #name,
    RENDER3D_VIEW_LIST(SCENE_VIEW_SYMBOL_ENTRY)
#undef SCENE_VIEW_SYMBOL_ENTRY
};
static const char *cfg_projection_symbols[PROJ_COUNT] = {
#define SCENE_PROJ_SYMBOL_ENTRY(name) [PROJ_##name] = "PROJ_" #name,
    PROJ_LIST(SCENE_PROJ_SYMBOL_ENTRY)
#undef SCENE_PROJ_SYMBOL_ENTRY
};
static const char *cfg_wireframe_symbols[WIREFRAME_COUNT] = {
#define SCENE_WIREFRAME_SYMBOL_ENTRY(name) [WIREFRAME_##name] = "WIREFRAME_" #name,
    WIREFRAME_LIST(SCENE_WIREFRAME_SYMBOL_ENTRY)
#undef SCENE_WIREFRAME_SYMBOL_ENTRY
};
static const char *cfg_backdrop_mode_symbols[RENDER3D_BACKDROP_COUNT] = {
#define SCENE_BACKDROP_SYMBOL_ENTRY(name, str) [RENDER3D_BACKDROP_##name] = "RENDER3D_BACKDROP_" #name,
    RENDER3D_BACKDROP_LIST(SCENE_BACKDROP_SYMBOL_ENTRY)
#undef SCENE_BACKDROP_SYMBOL_ENTRY
};
static const char *cfg_light_theme_symbols[LIGHT_THEME_COUNT] = {
#define LIGHT_THEME_SYMBOL_ENTRY(name, str) [LIGHT_THEME_##name] = "LIGHT_THEME_" #name,
    LIGHT_THEME_LIST(LIGHT_THEME_SYMBOL_ENTRY)
#undef LIGHT_THEME_SYMBOL_ENTRY
};
static const char *cfg_overlay_scope_symbols[OVERLAY_SCOPE_COUNT] = {
#define OVERLAY_SCOPE_SYMBOL_ENTRY(name, str) [OVERLAY_SCOPE_##name] = "OVERLAY_SCOPE_" #name,
    OVERLAY_SCOPE_LIST(OVERLAY_SCOPE_SYMBOL_ENTRY)
#undef OVERLAY_SCOPE_SYMBOL_ENTRY
};
static const char *cfg_vertex_label_symbols[OVERLAY_VERTEX_LABEL_COUNT] = {
#define OVERLAY_VERTEX_LABEL_SYMBOL_ENTRY(name, str) [OVERLAY_VERTEX_LABEL_##name] = "OVERLAY_VERTEX_LABEL_" #name,
    OVERLAY_VERTEX_LABEL_LIST(OVERLAY_VERTEX_LABEL_SYMBOL_ENTRY)
#undef OVERLAY_VERTEX_LABEL_SYMBOL_ENTRY
};
static const char *cfg_vertex_label_placement_symbols[OVERLAY_LABEL_PLACEMENT_COUNT] = {
#define OVERLAY_LABEL_PLACEMENT_SYMBOL_ENTRY(name, str) [OVERLAY_LABEL_PLACEMENT_##name] = "OVERLAY_LABEL_PLACEMENT_" #name,
    OVERLAY_LABEL_PLACEMENT_LIST(OVERLAY_LABEL_PLACEMENT_SYMBOL_ENTRY)
#undef OVERLAY_LABEL_PLACEMENT_SYMBOL_ENTRY
};
static const char *cfg_vertex_outline_style_symbols[VERTEX_OUTLINE_STYLE_COUNT] = {
#define VERTEX_OUTLINE_STYLE_SYMBOL_ENTRY(name, str) [VERTEX_OUTLINE_STYLE_##name] = "VERTEX_OUTLINE_STYLE_" #name,
    VERTEX_OUTLINE_STYLE_LIST(VERTEX_OUTLINE_STYLE_SYMBOL_ENTRY)
#undef VERTEX_OUTLINE_STYLE_SYMBOL_ENTRY
};
static const char *cfg_syntax_highlight_symbols[SYNTAX_HIGHLIGHT_COUNT] = {
#define SYNTAX_HIGHLIGHT_SYMBOL_ENTRY(name, str) [SYNTAX_HIGHLIGHT_##name] = "SYNTAX_HIGHLIGHT_" #name,
    SYNTAX_HIGHLIGHT_LIST(SYNTAX_HIGHLIGHT_SYMBOL_ENTRY)
#undef SYNTAX_HIGHLIGHT_SYMBOL_ENTRY
};
static const char *cfg_auto_normal_symbols[REPL_AUTONORMAL_COUNT] = {
#define AUTO_NORMAL_SYMBOL_ENTRY(name, str) [REPL_AUTONORMAL_##name] = "REPL_AUTONORMAL_" #name,
    REPL_AUTONORMAL_LIST(AUTO_NORMAL_SYMBOL_ENTRY)
#undef AUTO_NORMAL_SYMBOL_ENTRY
};
static const char *cfg_post_fx_scope_symbols[GLR_POST_FX_SCOPE_COUNT] = {
#define POST_FX_SCOPE_SYMBOL_ENTRY(suffix, name_str, symbol_str) [GLR_POST_FX_SCOPE_##suffix] = symbol_str,
    POST_FX_SCOPE_LIST(POST_FX_SCOPE_SYMBOL_ENTRY)
#undef POST_FX_SCOPE_SYMBOL_ENTRY
};
static const char *cfg_post_fx_effect_symbols[GLR_POST_FX_EFFECT_COUNT] = {
#define POST_FX_EFFECT_SYMBOL_ENTRY(suffix, render_mode, name_str, symbol_str) [GLR_POST_FX_EFFECT_##suffix] = symbol_str,
    POST_FX_EFFECT_LIST(POST_FX_EFFECT_SYMBOL_ENTRY)
#undef POST_FX_EFFECT_SYMBOL_ENTRY
};

/* The slug->table map shared by the symbolic resolver (read side) and the
 * value-to-string emitter (write side). Returns NULL (count untouched)
 * for slugs with no symbolic form. */
static const char *const *cfg_symbol_table_for_slug(const char *slug,
                                                    int *count) {
    if (strcmp(slug, "grid") == 0) {
        *count = GRID_THEME_COUNT;
        return cfg_grid_theme_symbols;
    }
    if (strcmp(slug, "axes") == 0) {
        *count = AXES_THEME_COUNT;
        return cfg_axes_theme_symbols;
    }
    if (strcmp(slug, "grid_extent") == 0) {
        *count = GRID_EXTENT_COUNT;
        return cfg_grid_extent_symbols;
    }
    if (strcmp(slug, "grid_major") == 0) {
        *count = GRID_MAJOR_COUNT;
        return cfg_grid_major_symbols;
    }
    if (strcmp(slug, "grid_brightness") == 0) {
        *count = GRID_BRIGHTNESS_COUNT;
        return cfg_grid_brightness_symbols;
    }
    if (strcmp(slug, "view_mode") == 0) {
        *count = RENDER3D_VIEW_COUNT;
        return cfg_view_mode_symbols;
    }
    if (strcmp(slug, "projection") == 0) {
        *count = PROJ_COUNT;
        return cfg_projection_symbols;
    }
    if (strcmp(slug, "wireframe") == 0) {
        *count = WIREFRAME_COUNT;
        return cfg_wireframe_symbols;
    }
    if (strcmp(slug, "backdrop") == 0) {
        *count = RENDER3D_BACKDROP_COUNT;
        return cfg_backdrop_mode_symbols;
    }
    if (strcmp(slug, "light_theme") == 0) {
        *count = LIGHT_THEME_COUNT;
        return cfg_light_theme_symbols;
    }
    if (strcmp(slug, "overlay_scope") == 0 || strcmp(slug, "label_scope") == 0 || strcmp(slug, "label_highlight_scope") == 0) {
        *count = OVERLAY_SCOPE_COUNT;
        return cfg_overlay_scope_symbols;
    }
    if (strcmp(slug, "vertex_labels") == 0) {
        *count = OVERLAY_VERTEX_LABEL_COUNT;
        return cfg_vertex_label_symbols;
    }
    if (strcmp(slug, "vertex_label_placement") == 0) {
        *count = OVERLAY_LABEL_PLACEMENT_COUNT;
        return cfg_vertex_label_placement_symbols;
    }
    if (strcmp(slug, "vertex_outline_style") == 0) {
        *count = VERTEX_OUTLINE_STYLE_COUNT;
        return cfg_vertex_outline_style_symbols;
    }
    if (strcmp(slug, "syntax_highlight") == 0) {
        *count = SYNTAX_HIGHLIGHT_COUNT;
        return cfg_syntax_highlight_symbols;
    }
    if (strcmp(slug, "auto_normals") == 0) {
        *count = REPL_AUTONORMAL_COUNT;
        return cfg_auto_normal_symbols;
    }
    if (strcmp(slug, "post_fx_scope") == 0) {
        *count = GLR_POST_FX_SCOPE_COUNT;
        return cfg_post_fx_scope_symbols;
    }
    if (strcmp(slug, "post_fx_effect") == 0) {
        *count = GLR_POST_FX_EFFECT_COUNT;
        return cfg_post_fx_effect_symbols;
    }
    return NULL;
}

static int glr_export_cfg_resolve_text(const char *slug,
                                       const char *value_name,
                                       int *out_value) {
    if (!slug || !value_name || !out_value) return 0;
    int count = 0;
    const char *const *table = cfg_symbol_table_for_slug(slug, &count);
    if (!table) return 0;
    for (int i = 0; i < count; i++) {
        if (table[i] && strcmp(value_name, table[i]) == 0) {
            *out_value = i;
            return 1;
        }
    }
    return 0;
}

/* True iff `s` is a clean integer literal (`-?[0-9]+` plus trailing
 * whitespace). The fallback path below uses this to decide whether
 * strtol's "0 on no conversion" is meaningful: an identifier-shaped
 * string like "GRID_THEME_RADRA" (a typo'd enum name) is NOT a
 * legacy integer-form value - strtol would silently land it at 0,
 * collapsing every misspelled symbol onto the first enum entry. */
static int glr_export_cfg_value_is_integer_literal(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '+' || *s == '-') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '\0';
}

/* Resolve a bag value to its integer form. Tries the symbolic
 * resolver first (so catalog literals like "GRID_THEME_RADAR" land at
 * the right enum value); falls back to strtol ONLY for clean integer
 * literals so legacy integer-form saved files keep loading. Returns
 * 1 on success and writes the int into *out_value; returns 0 for an
 * identifier-shaped value that doesn't resolve (the apply caller
 * then drops the row instead of silently landing it at 0). */
static int glr_export_cfg_resolve_value(const char *slug,
                                        const char *value_text,
                                        int *out_value) {
    if (!out_value) return 0;
    if (glr_export_cfg_resolve_text(slug, value_text, out_value))
        return 1;
    if (!glr_export_cfg_value_is_integer_literal(value_text))
        return 0;
    *out_value = (int)strtol(value_text, NULL, 10);
    return 1;
}

static void glr_export_cfg_value_to_string(const char *slug, int value, char *buf, size_t buf_sz) {
    int count = 0;
    const char *const *table = cfg_symbol_table_for_slug(slug, &count);
    const char *symbol = (table && value >= 0 && value < count) ? table[value] : NULL;

    if (symbol) {
        snprintf(buf, buf_sz, "%s", symbol);
    } else {
        snprintf(buf, buf_sz, "%d", value);
    }
}

static void glr_export_cfg_fill_all(ReplConfigBag *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (item->state_count <= 0) continue; /* action row: nothing to persist */
        char val_str[REPL_CFG_VALUE_MAX];
        glr_export_cfg_value_to_string(glr_config_item_slug(item),
                                       glr_config_get(item->key),
                                       val_str, sizeof(val_str));
        repl_config_bag_set(cfg, glr_config_item_slug(item), val_str);
    }
    {
        char val_str[REPL_CFG_VALUE_MAX];
        glr_export_cfg_value_to_string("audio",
                                       glr_config_get(GLR_CONFIG_AUDIO_MODE),
                                       val_str, sizeof(val_str));
        repl_config_bag_set(cfg, "audio", val_str);
    }
}

static void glr_export_cfg_fill_scene_subset(ReplConfigBag *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (!cfg_key_in_scene_subset(item->key))                  continue;
        char val_str[REPL_CFG_VALUE_MAX];
        glr_export_cfg_value_to_string(glr_config_item_slug(item),
                                       glr_config_get(item->key),
                                       val_str, sizeof(val_str));
        repl_config_bag_set(cfg, glr_config_item_slug(item), val_str);
    }
}

/* Same rows, same order as glr_export_cfg_fill_scene_subset, but carrying
 * each slug's default instead of its live value. Membership and defaults come
 * from the same k_cfg_scene_defaults[] roster, so the two bags always carry
 * the same slug set (test_glr_actions.c asserts it); the cfg_scene_default_-
 * for_key() miss below is therefore unreachable and stays only as a guard. */
static void glr_export_cfg_fill_scene_defaults(ReplConfigBag *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        int def_value = 0;
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (!cfg_key_in_scene_subset(item->key))                  continue;
        if (!cfg_scene_default_for_key(item->key, &def_value))    continue;
        char val_str[REPL_CFG_VALUE_MAX];
        glr_export_cfg_value_to_string(glr_config_item_slug(item), def_value,
                                       val_str, sizeof(val_str));
        repl_config_bag_set(cfg, glr_config_item_slug(item), val_str);
    }
}

static void glr_export_cfg_apply(const ReplConfigBag *cfg) {
    if (!cfg) return;
    for (int idx = 0; idx < cfg->count; idx++) {
        const char *slug  = cfg->items[idx].key;
        const char *value = cfg->items[idx].value;
        int val;
        if (!glr_export_cfg_resolve_value(slug, value, &val)) {
            /* Unresolved symbolic value - drop the row rather than
             * silently land it at 0 (GRID_THEME_OFF / AXES_THEME_OFF
             * etc.). The pre-bridge parse_cfg path did the
             * equivalent via strtol; tightening it here is what
             * makes catalog typos fail loud instead of muting the
             * showcase. */
            fprintf(stderr,
                    "repl_cfg: dropping '%s = %s' (unknown symbolic value)\n",
                    slug, value);
            continue;
        }
        if (strcmp(slug, "top_code_panel") == 0) {
            val = val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT;
        }
        if (strcmp(slug, "cpu_profile") == 0 && val >= 1) {
            /* Legacy slug predates the FPS level inserted at 1:
             * old On -> Sections, old Details -> Histogram. */
            val += 1;
            if (val >= PROFILE_PANEL_MODE_COUNT)
                val = PROFILE_PANEL_MODE_COUNT - 1;
        }
        const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
        if (item) {
            glr_config_set(item->key, val);
        } else if (glr_export_cfg_slug_is_hidden_audio(slug)) {
            glr_config_set(GLR_CONFIG_AUDIO_MODE, val);
        }
        /* Unknown slugs silently ignored - same behaviour as the pre-bridge
         * parse_cfg path: drop unrecognised cfg keys. */
    }
}

static int glr_export_cfg_get_int(const char *slug, int fallback) {
    if (glr_export_cfg_slug_is_hidden_audio(slug))
        return glr_config_get(GLR_CONFIG_AUDIO_MODE);
    const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
    if (item)
        return glr_config_get(item->key);
    return fallback;
}

static int glr_export_cfg_is_known(const char *slug) {
    if (glr_export_cfg_slug_is_hidden_audio(slug))
        return 1;
    return glr_export_cfg_find_item_by_slug(slug) ? 1 : 0;
}

static int glr_export_cfg_slug_is_scene_subset(const char *slug) {
    const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
    return item && cfg_key_in_scene_subset(item->key) ? 1 : 0;
}

static const ReplConfigBridge g_glr_export_cfg_bridge = {
    .fill_all          = glr_export_cfg_fill_all,
    .fill_scene_subset = glr_export_cfg_fill_scene_subset,
    .fill_scene_defaults = glr_export_cfg_fill_scene_defaults,
    .apply             = glr_export_cfg_apply,
    .get_int           = glr_export_cfg_get_int,
    .is_known          = glr_export_cfg_is_known,
    .slug_is_scene_subset = glr_export_cfg_slug_is_scene_subset,
    .resolve_text      = glr_export_cfg_resolve_text,
};

void glr_actions_install_export_cfg_bridge(void) {
    repl_config_install_bridge(&g_glr_export_cfg_bridge);
}

void glr_actions_set_msaa_label(int samples) {
    static char msaa_label[32];
    if (samples > 1) {
        snprintf(msaa_label, sizeof(msaa_label), "MSAAx%d", samples);
    } else {
        snprintf(msaa_label, sizeof(msaa_label), "MSAA");
    }
    g_msaa_display_label = msaa_label;
}

const char *glr_actions_audio_mode_status_string(int mode) {
    if (!glr_audio_is_enabled())
        return "Audio: disabled";
    return mode == AUDIO_CFG_PAUSE ? "Audio: paused" : "Audio: playing";
}

void glr_actions_apply_audio_cfg_mode(int mode) {
    if (mode == AUDIO_CFG_PAUSE) {
        glr_audio_set_paused(1);
    } else {
        glr_audio_set_paused(0);
    }
}

void glr_action_toggle_audio_play_pause(void) {
    if (!glr_audio_is_enabled()) {
        repl_set_status(glr_actions_audio_mode_status_string(AUDIO_CFG_PAUSE));
        return;
    }
    int next = glr_config_get(GLR_CONFIG_AUDIO_MODE) ? AUDIO_CFG_PAUSE
                                                     : AUDIO_CFG_ALL;
    glr_config_set(GLR_CONFIG_AUDIO_MODE, next);
    repl_set_status(glr_actions_audio_mode_status_string(next));
}

int glr_scene_menu_slot_for_dense_index(int scene_idx) {
    int seen = 0;
    for (int slot = 0; slot < MAX_USER_SCENES; slot++) {
        if (!repl_user_scene_slot_used(slot))
            continue;
        if (seen == scene_idx)
            return slot;
        seen++;
    }
    return -1;
}

int glr_scene_example_count(void) {
    return repl_example_count();
}

const char *glr_scene_example_name(int idx) {
    return repl_example_name(idx);
}

/* Shared scene-load sequences. The Scene menu and the scene tab strip
 * both switch scenes; keep the load sequence (and its load-bearing
 * subtleties) in one place rather than duplicating the statements.
 * Neither helper self-no-ops on "already active" - the Scene menu
 * always reloads, so callers that want a no-op (the tab router) check
 * before calling. */
void glr_scene_load_example(int example_idx) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    editor_state_edit_line_set(repl_load_example(example_idx));
}

void glr_scene_load_user_slot(int slot) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    glr_camera_clear_scene_default();
    if (repl_load_user_scene_idx(slot))
        editor_load_line_to_input(editor_state_edit_line());
}

/* Toggle the 2D/3D view mode via the same Config-row cycle the Ctrl+Shift+V
 * binding and the menu use, so the click path gets the status message and
 * (via the next view-transition tick observing ortho_mode) the animated
 * 2D<->3D transition. Used by the menu-bar view-mode swatch. */
void glr_action_toggle_view_mode(void) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        if (items[i].key == GLR_CONFIG_ORTHO_MODE) {
            glr_cfg_cycle_row(i, +1);
            return;
        }
    }
}

void glr_cfg_cycle_row(int row, int delta) {
    const GlrConfigItem *item = glr_config_item_at(row);

    if (!item || item->section_header)
        return;

    /* Replay is special-cased: its cfg toggle kicks off/ends the replay
     * machinery rather than flipping the int directly. Both directions
     * collapse to "toggle". */
    if (item->key == GLR_CONFIG_REPLAY) {
        int turn_on = !glr_config_get(GLR_CONFIG_REPLAY);
        glr_config_set(GLR_CONFIG_REPLAY, turn_on);
        if (!turn_on)
            repl_set_status("Replay: off");
        return;
    }

    /* Camera action rows: momentary, no state to cycle. Both directions
     * collapse to "do it". Easing keeps the move smooth, not jarring. */
    if (item->key == GLR_CONFIG_FOCUS_ORIGIN) {
        glr_camera_focus_origin();
        repl_set_status("Camera: focus origin");
        return;
    }
    if (item->key == GLR_CONFIG_LOOK_DOWN_Z) {
        glr_camera_look_down_z();
        repl_set_status("Camera: look down Z");
        return;
    }
    if (item->key == GLR_CONFIG_RESET_CAMERA) {
        glr_camera_ease_to_default();
        repl_set_status("Camera: reset to default");
        return;
    }

    /* Depth view needs depth readback (glReadPixels GL_DEPTH_COMPONENT);
     * WebGL forbids it and some contexts fail the init-GL probe. Refuse the
     * interactive cycle with the reason instead of silently cycling a row
     * whose render-config copy is forced Off every frame. @cfg header loads
     * bypass this (they write via glr_config_set), so files round-trip. */
    if (item->key == GLR_CONFIG_DEPTH_VIZ) {
        const char *reason = glr_ctrl_depth_readback_unsupported_reason();
        if (reason) {
            repl_set_status(reason);
            return;
        }
    }
    if (item->key == GLR_CONFIG_STENCIL_VIZ) {
        const char *reason = glr_ctrl_stencil_readback_unsupported_reason();
        if (reason) {
            repl_set_status(reason);
            return;
        }
    }

    if (item->key == GLR_CONFIG_AUTO_NORMALS || item->key == GLR_CONFIG_POINT_ATTENUATION) {
        if (replay_active())
            replay_stop();
    }

    int grid_locked = 0;
    int locked_backdrop = 0;
    Render3dGridTheme locked_grid = GRID_THEME_OFF;
    if (item->key == GLR_CONFIG_GRID_THEME) {
        locked_backdrop = glr_config_get(GLR_CONFIG_BACKDROP);
        grid_locked = glr_config_backdrop_forces_grid(
            (Render3dBackdropMode)locked_backdrop, &locked_grid);
    }

    int new_value = glr_config_cycle(item->key, delta);
    glr_ctrl_sync_ui_chrome();  /* refresh ui_state.code_panel mirrors */

    if (item->key == GLR_CONFIG_CODE_PANEL_LAYOUT) {
        ui_state_code_panel_mut()->panel_frac = CFG_DEFAULT_PANEL_FRAC;
        GlrPresentationState p = glr_state_presentation();
        if (p.code_panel_layout == CODE_PANEL_LAYOUT_TOP) {
            repl_set_status("Layout: top code panel");
        } else if (p.code_panel_layout == CODE_PANEL_LAYOUT_BOTTOM) {
            repl_set_status("Layout: bottom code panel");
        } else if (p.code_panel_layout == CODE_PANEL_LAYOUT_HIDDEN) {
            ui_menu_bar_close();
            color_picker_stop();
            editor_completion_clear();
            repl_set_status("Layout: code panel hidden");
        } else {
            repl_set_status("Layout: left code panel");
        }
    } else if (item->key == GLR_CONFIG_AUTO_NORMALS) {
        if (glr_state_presentation().autonormal == REPL_AUTONORMAL_OFF) {
            /* Off removes what the pass generated. Leaving the rows behind
             * stranded them: the pass only ever added, so there was no
             * non-manual way back, and because it mutates the document from
             * the display frame there was no undo entry to fall back on
             * either. Undo cannot serve here while the mode is on - the next
             * frame just re-inserts - so Off is the only possible way out.
             *
             * The snapshot is what makes the removal safe rather than merely
             * symmetric: Ctrl+Z restores the rows, and with the mode now off
             * nothing re-runs to take them away again. */
            int edit_line = editor_state_edit_line();
            int removed;

            editor_undo_push_snapshot();
            removed = repl_strip_auto_normals(&edit_line);
            editor_state_edit_line_set(edit_line);
            if (removed > 0) {
                repl_mark_source_dirty();
                snprintf(cfg_status_buf, sizeof(cfg_status_buf),
                         "Auto-normals: Off (%d generated normal%s removed, "
                         "Ctrl+Z to restore)",
                         removed, removed == 1 ? "" : "s");
                repl_set_status(cfg_status_buf);
            } else {
                repl_set_status("Auto-normals: Off");
            }
        } else {
            /* Face <-> Smooth also needs the recompute: the rows are
             * already there, but every one of them changes value. */
            repl_mark_source_dirty();
            snprintf(cfg_status_buf, sizeof(cfg_status_buf),
                     "Auto-normals: %s",
                     glr_config_state_name(item->key, new_value));
            repl_set_status(cfg_status_buf);
        }
    } else if (item->key == GLR_CONFIG_POINT_ATTENUATION) {
        repl_apply_init_bootstrap();
        repl_set_status(glr_config_get(GLR_CONFIG_POINT_ATTENUATION) ? "Point attenuation: ON"
                                                                  : "Point attenuation: OFF");
    } else if (item->key == GLR_CONFIG_LIGHT_THEME) {
        /* render3d_lights_apply_theme already ran inside glr_config_set above
         * (so @cfg-driven theme loads get the same treatment). The cycle
         * handler just needs to re-apply the init bootstrap so the exporter /
         * code-panel light lines pick up the new positions and colors. */
        repl_apply_init_bootstrap();
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 glr_config_state_name(item->key, new_value));
        repl_set_status(cfg_status_buf);
    } else if (grid_locked) {
        const char *backdrop_name =
            glr_config_state_name(GLR_CONFIG_BACKDROP, locked_backdrop);
        const char *grid_name =
            glr_config_state_name(GLR_CONFIG_GRID_THEME, locked_grid);
        snprintf(cfg_status_buf, sizeof(cfg_status_buf),
                 "Warning: grid locked by %s backdrop (%s)",
                 backdrop_name ? backdrop_name : "paired",
                 grid_name ? grid_name : "paired grid");
        repl_set_status(cfg_status_buf);
    } else if (item->state_names) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 glr_config_state_name(item->key, new_value));
        repl_set_status(cfg_status_buf);
    } else if (item->state_count == 2) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 new_value ? "ON" : "OFF");
        repl_set_status(cfg_status_buf);
    }
}

void glr_action_cursor_blink_reset(void) {
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();

    cb->cursor_visible = 1;
    cb->blink_tick = 0;
}

/* Highest valid help tab index, derived from the actual content so
 * adding/removing a tab in help_text.c needs no edit here. */
static int glr_help_max_tab_idx(void) {
    const ReplHelpContent *help = repl_help_text_build();
    if (!help || help->tab_count <= 0)
        return 0;
    return help->tab_count - 1;
}

void glr_action_help_tab_next(void) {
    int tab = editor_help_session_tab_idx();
    if (tab < glr_help_max_tab_idx()) {
        editor_help_session_set_tab(tab + 1);
        editor_help_session_set_scroll(0);
    }
}

void glr_action_help_tab_prev(void) {
    int tab = editor_help_session_tab_idx();
    if (tab > 0) {
        editor_help_session_set_tab(tab - 1);
        editor_help_session_set_scroll(0);
    }
}

void glr_action_reset_time_to_zero(void) {
    repl_reset_time_to_zero();
    repl_set_status(repl_state_variables().time_playing ? "Time: reset to 0"
                                                   : "Time: reset to 0 (paused)");
}

/* Ctrl+<key> shortcut dispatch. GLUT delivers Ctrl+letter as the same
 * control code with or without Shift, so keymap_event_is reads the live
 * modifier state. A config row is an exact binding: extra modifiers never
 * fall through to a nearby plain row. The descriptor table is the single
 * source of truth - no separate router. */
static int cfg_match_row(unsigned char key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (!item || item->section_header || item->is_special)
            continue;
        /* reject printable ASCII - except backtick (the only printable ASCII
         * that can be a control code) */
        if ((item->key_code != '`' && (item->key_code <= 0 || item->key_code >= 32)) ||
            item->key_code != key)
            continue;
        if (!keymap_event_is(key, item->key_code, item->modifiers))
            continue;
        glr_cfg_cycle_row(i, 1);
        return 1;
    }
    return 0;
}

int glr_cfg_handle_ascii_shortcut(unsigned char key) {
    return cfg_match_row(key);
}

int glr_cfg_handle_special_shortcut(int key) {
    /* F<n> steps the bound row forward; Shift+F<n> steps it backward.
     * For 2-state toggles the direction is immaterial (both flip); for
     * the multi-state cycles (grid / axes / vertex labels / xform
     * guides / light theme) Shift reverses the wrap. No F-key row uses
     * the descriptor's `modifiers` field, so Shift is free to mean
     * "backward" here. Modifiers are read in the actions layer, matching
     * glr_cfg_handle_ascii_shortcut. */
    int delta = (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) ? -1 : 1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && !item->section_header &&
            item->is_special && item->key_code == key) {
            glr_cfg_cycle_row(i, delta);
            return 1;
        }
    }
    return 0;
}

static void format_and_set_seek_status(const char *action_name) {
    float current_time = glr_audio_current_cursor_seconds();
    int track_idx = glr_audio_current_index();
    float duration = (track_idx >= 0) ? glr_audio_track_duration_seconds(track_idx) : -1.0f;
    char time_str[64];
    char msg[128];
    int cur_mins = (int)current_time / 60;
    int cur_secs = (int)current_time % 60;
    if (duration >= 0.0f) {
        int dur_mins = (int)duration / 60;
        int dur_secs = (int)duration % 60;
        snprintf(time_str, sizeof(time_str), "%d:%02d / %d:%02d",
                 cur_mins, cur_secs, dur_mins, dur_secs);
    } else {
        snprintf(time_str, sizeof(time_str), "%d:%02d",
                 cur_mins, cur_secs);
    }
    snprintf(msg, sizeof(msg), "%s (%s)", action_name, time_str);
    ui_state_status_set_music(msg);
}

int glr_action_open_workspace_path(const char *path) {
    ReplExportLayout layout;
    const char *current = repl_workspace_dir();
    char old_workspace[REPL_WORKSPACE_DIR_MAX];
    snprintf(old_workspace, sizeof(old_workspace), "%s", current ? current : "");
    glr_ctrl_fill_export_layout(&layout);

    if (!path || !path[0]) {
        repl_set_status_error("Open workspace: no folder provided");
        return 0;
    }
    if (old_workspace[0] && glr_paths_same_dir(old_workspace, path)) {
        repl_set_status("Workspace is already open");
        return 1;
    }

    /* Nothing to rescue when the visible document is an unpromoted
     * example - skipping the write is success, not a failed save. */
    if (glr_ctrl_recovery_has_user_work() && !glr_ctrl_save_recovery_file()) {
        repl_set_status_error("Workspace switch cancelled: recovery save failed");
        return 0;
    }

    if (old_workspace[0] && repl_workspace_is_managed()) {
        if (save_workspace_including_visible_scene(
                old_workspace, &layout, 0) < 0)
            return 0;
    } else if (repl_user_scene_count() > 0) {
        char recovery_dir[GLR_PATH_MAX];
        if (!glr_paths_app_state_path("recovery-workspace",
                                      recovery_dir, sizeof(recovery_dir)) ||
            save_workspace_including_visible_scene(
                recovery_dir, &layout, 0) < 0) {
            repl_set_workspace_dir(old_workspace);
            repl_set_status_error(
                "Workspace switch cancelled: collection recovery failed");
            return 0;
        }
        repl_set_workspace_dir(old_workspace);
    }

    ReplWorkspaceLoadResult loaded = repl_load_workspace_ex(path);
    if (!loaded.ok)
        return 0;
    editor_undo_note_wholesale_replacement();
    glr_camera_clear_scene_default();
    if (loaded.scenes_loaded > 0)
        repl_scenes_activate_first_loaded_slot();
    else if (repl_scenes_create_empty_user_scene() < 0)
        return 0;
    editor_load_line_to_input(editor_state_edit_line());
    glr_workspaces_refresh();
    return 1;
}

int glr_action_open_workspace_index(int idx) {
    const char *path = glr_workspaces_path(idx);
    return path && path[0] ? glr_action_open_workspace_path(path) : 0;
}

void glr_action_begin_open_workspace_path(void) {
    glr_modal_begin(GLR_MODAL_WORKSPACE_OPEN_PATH, "", -1,
                    glr_action_modal_commit);
}

int glr_action_save_active_scene(void) {
    ReplExportLayout layout;
    int slot = repl_active_user_scene();
    if (slot < 0 && !glr_paths_cwd_supports_relative_saves()) {
        return glr_modal_begin(GLR_MODAL_SCENE_SAVE_AS, "", -1,
                               glr_action_modal_commit);
    }
    if (!bind_app_workspace_for_scene_save_if_needed())
        return 0;
    glr_ctrl_fill_export_layout(&layout);
    /* A per-slot File-Open/CLI binding is newer than the collection-level
     * workspace binding. Save that file directly; an explicit Save Workspace
     * adopts it back into the workspace and clears source_path on success. */
    if (repl_workspace_is_managed() && !repl_active_scene_source_path())
        return save_workspace_including_visible_scene(
                   repl_workspace_dir(), &layout, 1) >= 0;
    if (!repl_save_active_scene(&layout))
        return 0;
    glr_extedit_note_saved();
    return 1;
}

/* Explicit standalone-C export. Save Scene follows a loaded source file's
 * format, so a scene opened from `.glr` quite deliberately saves back to
 * `.glr`. This action is the format escape hatch: resolve the normal scene
 * name/directory with a C extension and let the path-selected writer emit
 * the standalone program. */
static int glr_action_save_scene_as_c(void) {
    ReplExportLayout layout;
    char path[REPL_WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    const char *resolved;

    if (!bind_app_workspace_for_scene_save_if_needed())
        return 0;
    glr_ctrl_fill_export_layout(&layout);
    resolved = repl_active_scene_export_path("c");
    snprintf(path, sizeof(path), "%s", resolved ? resolved : "");
    if (!repl_save_active_scene_to_path(path, &layout))
        return 0;
    glr_extedit_note_wrote(path);
    return 1;
}

static int glr_action_reveal_workspace(void) {
    const char *dir = repl_workspace_dir();
    if (!repl_workspace_is_managed() || !dir || !dir[0])
        return 0;
#if defined(__APPLE__)
    pid_t pid;
    char *const argv[] = { "open", (char *)dir, NULL };
    int rc = posix_spawn(&pid, "/usr/bin/open", NULL, NULL, argv, environ);
    if (rc == 0)
        rc = waitpid(pid, NULL, 0) < 0 ? errno : 0;
    if (rc != 0) {
        repl_set_status_error("Could not reveal workspace in Finder");
        return 0;
    }
    return 1;
#else
    (void)dir;
    repl_set_status_error("Reveal Workspace Folder is macOS-only");
    return 0;
#endif
}

static int glr_action_delete_scene_commit(int slot) {
    if (!repl_workspace_is_managed() || slot < 0) {
        glr_modal_set_error("Scene deletion requires a managed workspace");
        return 0;
    }
    ReplExportLayout layout;
    glr_ctrl_fill_export_layout(&layout);
    if (save_workspace_including_visible_scene(
            repl_workspace_dir(), &layout, 1) < 0)
        return 0;
    ReplScenesSnapshot *stash = repl_scenes_snapshot_capture();
    if (!stash) {
        glr_modal_set_error("Out of memory while deleting scene");
        return 0;
    }
    char name[USER_SCENE_NAME_MAX];
    snprintf(name, sizeof(name), "%s", repl_user_scene_name(slot));
    if (!repl_user_scene_delete(slot) ||
        save_workspace_including_visible_scene(
            repl_workspace_dir(), &layout, 1) < 0) {
        repl_scenes_snapshot_restore(stash);
        repl_scenes_snapshot_destroy(stash);
        glr_modal_set_error("Could not commit scene deletion");
        return 0;
    }
    repl_scenes_snapshot_destroy(stash);
    if (repl_scenes_activate_first_loaded_slot() < 0)
        repl_scenes_create_empty_user_scene();
    editor_undo_note_wholesale_replacement();
    editor_load_line_to_input(editor_state_edit_line());
    glr_workspaces_refresh();
    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Deleted scene: %s", name);
    repl_set_status(msg);
    return 1;
}

/* Commit half of the modal contract: one arm per GlrModalKind, no
 * `default:`, so a new kind cannot compile into a prompt that ignores Enter.
 * Returning 0 keeps the modal open (with an error set); 1 closes it. */
static int glr_action_modal_commit(GlrModalKind kind, const char *text,
                                   int context) {
    char path[GLR_PATH_MAX];
    char err[REPL_STATUS_TEXT_MAX];
    ReplExportLayout layout;
    switch (kind) {
    case GLR_MODAL_WORKSPACE_NEW: {
        /* An unbound collection has no managed home yet, so opening the fresh
         * (empty) workspace would clear it from view - glr_action_open_-
         * workspace_path() keeps the bytes in the recovery workspace, but
         * nothing surfaces them. Adopt those scenes into the new workspace
         * instead. Creating a workspace *from* a managed one still starts
         * empty: those scenes stay saved where they already live. */
        int adopt = !repl_workspace_is_managed() && repl_user_scene_count() > 0;
        if (!glr_workspaces_create(text, path, sizeof(path),
                                   err, sizeof(err))) {
            glr_modal_set_error(err);
            return 0;
        }
        if (adopt) {
            glr_ctrl_fill_export_layout(&layout);
            if (save_workspace_including_visible_scene(
                    path, &layout, 1) < 0) {
                (void)glr_workspaces_discard_empty(path);
                glr_modal_set_error("Could not move scenes into the new workspace");
                return 0;
            }
            glr_workspaces_refresh();
            return 1;
        }
        if (!glr_action_open_workspace_path(path)) {
            (void)glr_workspaces_discard_empty(path);
            glr_modal_set_error("Could not open the new workspace");
            return 0;
        }
        return 1;
    }
    case GLR_MODAL_WORKSPACE_SAVE_AS:
        if (!glr_workspaces_create(text, path, sizeof(path),
                                   err, sizeof(err))) {
            glr_modal_set_error(err);
            return 0;
        }
        glr_ctrl_fill_export_layout(&layout);
        if (save_workspace_including_visible_scene(path, &layout, 1) < 0) {
            (void)glr_workspaces_discard_empty(path);
            glr_modal_set_error("Could not save the new workspace");
            return 0;
        }
        glr_workspaces_refresh();
        return 1;
    case GLR_MODAL_WORKSPACE_OPEN_PATH:
        if (!workspace_io_manifest_exists(text)) {
            glr_modal_set_error("Folder is not a managed gl-repl workspace");
            return 0;
        }
        return glr_action_open_workspace_path(text);
    case GLR_MODAL_SCENE_SAVE_AS: {
        char old_workspace[REPL_WORKSPACE_DIR_MAX];
        snprintf(old_workspace, sizeof(old_workspace), "%s", repl_workspace_dir());
        if (!text || !text[0]) {
            glr_modal_set_error("Scene name cannot be empty");
            return 0;
        }
        if (!ensure_default_managed_workspace(err, sizeof(err))) {
            glr_modal_set_error(err);
            return 0;
        }
        repl_set_workspace_dir(glr_paths_default_workspace_dir());
        int slot = repl_promote_transient_if_needed();
        if (slot < 0 || !repl_user_scene_rename(slot, text)) {
            repl_set_workspace_dir(old_workspace);
            glr_modal_set_error(slot < 0 ? "Could not reserve a scene slot"
                                         : "Scene name cannot be empty");
            return 0;
        }
        glr_ctrl_fill_export_layout(&layout);
        if (save_workspace_including_visible_scene(
                repl_workspace_dir(), &layout, 1) < 0) {
            repl_set_workspace_dir(old_workspace);
            glr_modal_set_error("Could not save scene");
            return 0;
        }
        glr_workspaces_refresh();
        return 1;
    }
    case GLR_MODAL_CONFIRM_DELETE_SCENE:
        return glr_action_delete_scene_commit(context);
    case GLR_MODAL_CONFIRM_WIP_RECOVER:
        /* Opened by src/app/glr_extedit.c with its own commit callback, so it
         * never reaches this dispatcher. The arm exists because the switch is
         * default-less on purpose. */
        return 0;
    case GLR_MODAL_NONE:
    case GLR_MODAL_COUNT:
        return 0;   /* not open: glr_modal_begin() rejects these */
    }
    return 0;
}

int glr_action_menu_item_activate(int menu_id, int item_idx) {
    switch (menu_id) {
    case GLR_MENU_FILE:
        switch (item_idx) {
        case GLR_FILE_ITEM_NEW_SCENE:
            if (repl_scenes_create_empty_user_scene() >= 0) {
                editor_undo_note_wholesale_replacement();
                glr_camera_clear_scene_default();
            }
            return 1;
        case GLR_FILE_ITEM_SAVE_SCENE: {
            glr_action_save_active_scene();
            return 1;
        }
        case GLR_FILE_ITEM_SAVE_C:
            glr_action_save_scene_as_c();
            return 1;
        case GLR_FILE_ITEM_SAVE_GLR: {
            /* Editing a runtime `--examples-dir` catalog: write straight back
             * to the file the catalog names, so iterating on a scene updates
             * it in place instead of dropping a differently-named export into
             * a workspace the author then has to reconcile. No workspace is
             * bound for this path - the catalog dir is the destination. */
            const char *write_back = repl_active_scene_glr_write_back_path();
            if (write_back) {
                if (repl_export_save_glr(write_back, source_document_view()))
                    glr_extedit_note_wrote(write_back);
                return 1;
            }
            /* Otherwise: same target directory and base name as Save Scene /
             * Export .ply, just the authoring format - a .glr you can drop
             * into examples/scenes/ instead of a standalone C program. */
            if (!bind_app_workspace_for_scene_save_if_needed())
                return 1;
            {
                const char *glr_path = repl_active_scene_export_path("glr");
                char owned[SCENE_GLR_ORIGIN_PATH_MAX];
                /* export_path returns a rotating static buffer the writer's
                 * own status formatting can reuse; copy before handing it on. */
                snprintf(owned, sizeof(owned), "%s", glr_path);
                if (repl_export_save_glr(owned, source_document_view()))
                    glr_extedit_note_wrote(owned);
            }
            return 1;
        }
        case GLR_FILE_ITEM_LOAD_SCENE:
            glr_modal_cancel();
            editor_inline_file_prompt_begin(DEFAULT_SCENE_FILE);
            return 1;
        case GLR_FILE_ITEM_RENAME_SCENE: {
            int slot = repl_active_user_scene();
            if (tutorial_active()) {
                repl_set_status_error("Rename Scene is unavailable during a tutorial");
                return 1;
            }
            if (slot < 0) {
                repl_set_status_error("No active scene to rename");
                return 1;
            }
            glr_modal_cancel();
            editor_inline_rename_begin(slot);
            return 1;
        }
        case GLR_FILE_ITEM_DELETE_SCENE: {
            int slot = repl_active_user_scene();
            if (tutorial_active()) {
                repl_set_status_error(
                    "Delete Workspace Scene is unavailable during a tutorial");
                return 1;
            }
            if (slot < 0 || !repl_workspace_is_managed()) {
                repl_set_status_error("No managed workspace scene to delete");
                return 1;
            }
            char question[256];
            snprintf(question, sizeof(question), "Delete scene '%s' and %s?",
                     repl_user_scene_name(slot), repl_user_scene_file_name(slot));
            glr_modal_begin(GLR_MODAL_CONFIRM_DELETE_SCENE, question, slot,
                            glr_action_modal_commit);
            return 1;
        }
        case GLR_FILE_ITEM_EXPORT_PLY:
            if (!bind_app_workspace_for_scene_save_if_needed())
                return 1;
            glr_export_mesh_ply(repl_active_scene_export_path("ply"), 0);
            return 1;
        case GLR_FILE_ITEM_SPLIT_DECL:
            editor_split_decl_at_cursor();
            return 1;
        case GLR_FILE_ITEM_REVEAL_WORKSPACE:
            glr_action_reveal_workspace();
            return 1;
        case GLR_FILE_ITEM_NEW_WORKSPACE:
            glr_modal_begin(GLR_MODAL_WORKSPACE_NEW, "", -1,
                            glr_action_modal_commit);
            return 1;
        case GLR_FILE_ITEM_SAVE_WORKSPACE: {
            const char *dir = workspace_dir_or_app_default();
            ReplExportLayout layout;
            char err[REPL_STATUS_TEXT_MAX];
            if (!repl_workspace_dir()[0] &&
                !ensure_default_managed_workspace(err, sizeof(err))) {
                repl_set_status_error(err);
                return 1;
            }
            glr_ctrl_fill_export_layout(&layout);
            save_workspace_including_visible_scene(dir, &layout, 1);
            glr_workspaces_refresh();
            return 1;
        }
        case GLR_FILE_ITEM_SAVE_WORKSPACE_AS:
            glr_modal_begin(GLR_MODAL_WORKSPACE_SAVE_AS, "", -1,
                            glr_action_modal_commit);
            return 1;
        case GLR_FILE_ITEM_OPEN_WORKSPACE:
            /* Hover-only flyout parent. */
            return 0;
        case GLR_FILE_ITEM_SCENE_SEP:
        case GLR_FILE_ITEM_WORKSPACE_HDR:
        case GLR_FILE_ITEM_QUIT_SEP:
            return 1;
        case GLR_FILE_ITEM_QUIT:
            glr_ctrl_request_quit();
            return 1;
        default:
            /* Out-of-range or unknown file menu item. Return 1 (consumed)
             * to keep backward compatibility with existing tests. */
            return 1;
        }

    case GLR_MENU_SCENE: {
        int tag_count = repl_example_visible_tag_count();
        if (item_idx >= 1 && item_idx <= tag_count)
            return 0;

        /* "Next" / "Previous" cycle the example-or-scene selection (the
         * F12 / Shift+F12 path). Keep the dropdown open (return 0) so the
         * user can step through examples with repeated clicks, like the
         * Config cycle rows. The cycle runs glr_ctrl_reset_transients(),
         * which closes the menu, so capture/restore the open state around
         * it to leave the dropdown (and its hover highlight) intact. */
        if (item_idx == tag_count + GLR_SCENE_OFF_NEXT) {
            UiMenuBarOpenState menu_state = ui_menu_bar_open_state_capture();
            glr_ctrl_scene_cycle_next();
            ui_menu_bar_open_state_restore(menu_state);
            return 0;
        }
        if (item_idx == tag_count + GLR_SCENE_OFF_PREV) {
            UiMenuBarOpenState menu_state = ui_menu_bar_open_state_capture();
            glr_ctrl_scene_cycle_prev();
            ui_menu_bar_open_state_restore(menu_state);
            return 0;
        }

        int scene_idx = item_idx - (tag_count + GLR_SCENE_OFF_SCENES);
        if (scene_idx >= 0 && scene_idx < repl_user_scene_count()) {
            int slot = glr_scene_menu_slot_for_dense_index(scene_idx);
            if (slot >= 0) {
                glr_scene_load_user_slot(slot);
                return 1;
            }
        }
        /* Returns 1 (consumed) for out-of-range, header row, and negative indexes,
         * matching test expectations. */
        return 1;
    }

    case GLR_MENU_TUTORIALS: {
        /* Top-level Tutorials rows after the Phase-B hierarchical menu:
         *   [0..t-1]   tag rows (inert hover-only, like Scene tag rows
         *              - the actual tutorial flyout-item activation
         *              flows through route_submenu_item_hit which
         *              dispatches directly to tutorial_start, NOT
         *              through this function).
         *   [t]        "---" (chrome row, filtered before activation).
         *   [t+1]      "Next" (via F11).
         *   [t+2]      "Previous" (via Shift+F11).
         *   If active:
         *     [t+3]      "---" (chrome row, filtered before activation).
         *     [t+4]      "Restart Tutorial".
         *     [t+5]      "Exit Tutorial". */
        int tag_count = repl_tutorial_visible_tag_count();
        if (item_idx < tag_count)
            return 0;   /* tag row -> keep menu open, no action */
        int active = tutorial_active();
        if (item_idx == tag_count + GLR_TUTORIAL_OFF_NEXT) {
            glr_ctrl_tutorial_cycle_next();
            return 1;
        }
        if (item_idx == tag_count + GLR_TUTORIAL_OFF_PREV) {
            glr_ctrl_tutorial_cycle_prev();
            return 1;
        }
        if (active && item_idx == tag_count + GLR_TUTORIAL_OFF_RESTART) {
            TutorialRuntimeState tutorial = tutorial_state_view();
            if (tutorial.tutorial_idx >= 0) {
                /* Restart replaces the transient document directly rather
                 * than taking a scene/workspace load path that bumps the undo
                 * generation. Do not expose the outgoing lesson's depth to
                 * the restarted lesson's first label pass. */
                glr_ctrl_invalidate_depth_snapshot();
                tutorial_start(tutorial.tutorial_idx);
            }
            return 1;
        }
        if (active && item_idx == tag_count + GLR_TUTORIAL_OFF_EXIT) {
            tutorial_stop();
            return 1;
        }
        /* Returns 1 (consumed) for out-of-range or separators, matching test expectations. */
        return 1;
    }

    case GLR_MENU_TOURS:
        /* Tour rows start the guided tour and close the menu - the script
         * needs the dropdown out of the way before its first glide. Out of
         * range falls through to the same consumed-return as File/Scene. */
        if (item_idx >= 0 && item_idx < glr_tours_count())
            glr_tours_start(item_idx);
        return 1;

    case GLR_MENU_CONFIG:
        /* Config top-level rows are section / "All" PARENT rows: they
         * hover-open a flyout, and a click on the parent itself is
         * inert (mirrors the MENU_SCENE tag-row guard above). `item_idx`
         * here is a section parent row, NOT a g_cfg_items[] index, so
         * it must never be cycled. Leaf config-item activation arrives
         * via UI_HIT_SUBMENU_ITEM and is dispatched straight to
         * glr_cfg_cycle_row() on the absolute g_cfg_items[] index
         * (route_submenu_item_hit) - it never reaches this branch.
         * Return 0 so the dropdown stays open, matching the old
         * per-toggle feel. */
        (void)item_idx;
        return 0;

    case GLR_MENU_AUDIO: {
        int group_count = audio_menu_group_count();
        int rel = item_idx - group_count;
        char msg[48];

        if (group_count <= 0)
            return 1;
        if (item_idx >= 0 && item_idx < group_count)
            return 0;
        if (rel == GLR_AUDIO_OFF_PLAY) {
            glr_action_toggle_audio_play_pause();
            return 0;
        }
        if (rel == GLR_AUDIO_OFF_BACK10) {
            glr_audio_seek_relative(-10.0f);
            format_and_set_seek_status("Jump Back 10s");
            return 0;
        }
        if (rel == GLR_AUDIO_OFF_FWD10) {
            glr_audio_seek_relative(10.0f);
            format_and_set_seek_status("Jump Forward 10s");
            return 0;
        }
        /* Play/Pause, Loop, and Jump Back/Forward keep the dropdown open (repeated toggles);
         * Next/Prev are one-shot navigation and close it. This split is
         * intentional and pinned by test_glr_actions ("audio menu next/
         * previous closes"). */
        if (rel == GLR_AUDIO_OFF_NEXT) {
            glr_audio_next_track();
            return 1;
        }
        if (rel == GLR_AUDIO_OFF_PREV) {
            glr_audio_prev_track();
            return 1;
        }
        if (rel == GLR_AUDIO_OFF_LOOP) {
            int mode = glr_audio_get_loop_mode();
            int next = mode + 1;
            if (next > GLR_AUDIO_LOOP_ALL)
                next = GLR_AUDIO_LOOP_OFF;
            glr_audio_set_loop_mode(next);
            snprintf(msg, sizeof(msg), "Loop: %s",
                     audio_loop_status_label(next));
            ui_state_status_set_music(msg);
            return 0;
        }
        return 1;
    }

    default:
        return 1;
    }
}

void glr_actions_apply_defaults(void) {
    if (!glr_audio_is_enabled()) {
        repl_set_status(glr_actions_audio_mode_status_string(AUDIO_CFG_PAUSE));
        return;
    }
    /* This function restores the audio mode persisted from the previous
     * session; it does not seed presentation defaults.
     * glr_audio_play_playlist() calls load_state() which stores the cfg_mode
     * in the audio module; pull it here so the UI config and the actual audio
     * engine agree before the first frame. */
    int saved_mode = glr_audio_get_cfg_mode();
    if (saved_mode < AUDIO_CFG_PAUSE || saved_mode > AUDIO_CFG_ALL)
        saved_mode = AUDIO_CFG_ALL;
    glr_config_set(GLR_CONFIG_AUDIO_MODE, saved_mode);
    if (saved_mode == AUDIO_CFG_PAUSE) {
        repl_set_status(glr_actions_audio_mode_status_string(saved_mode));
    }
}
