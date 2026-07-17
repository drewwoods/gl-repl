#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "repl/flatten.h"
#include "repl/gl_state_inspector.h"
#include "repl/export.h"
#include "repl/scenes.h"
#include "repl/example_loader.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "ui/app/state.h"
#include "app/glr_camera.h"
#include "ui/support/cpuprof.h"
#include "ui/app/layout.h"            /* CODE_PANEL_LAYOUT_* enum values */
#include "subsystems/variable_panel/variable_panel_state.h"
#include "subsystems/replay/replay.h"               /* REPLAY_PAUSED, REPLAY_MODE_* enums */
#include "subsystems/replay/replay_state.h"
#include "editor/help_session.h"
#include "app/glr_defaults.h"    /* CFG_DEFAULT_* macros */
#include "source_document.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

static GLCmd gl_state_test_cmd(CmdType type, int source_line_idx) {
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.valid = 1;
    cmd.src_cmd_idx = source_line_idx;
    cmd.call_src_cmd_idx = -1;
    cmd.root_call_src_cmd_idx = -1;
    return cmd;
}

static const ReplGlStateReportRow *gl_state_test_find_row(
    const ReplGlStateReport *report, const char *name) {
    int i;
    for (i = 0; report && i < report->count; i++)
        if (strcmp(report->rows[i].name, name) == 0)
            return &report->rows[i];
    return NULL;
}

static void gl_state_test_fill_light(int slot, ReplExportLightInfo *out) {
    memset(out, 0, sizeof(*out));
    out->pos[0] = (float)(slot + 1);
    out->pos[1] = 2.0f;
    out->pos[2] = 3.0f;
    out->pos[3] = 1.0f;
    out->ambient[0] = 0.1f;
    out->ambient[1] = 0.2f;
    out->ambient[2] = 0.3f;
    out->ambient[3] = 1.0f;
    out->diffuse[0] = 0.4f;
    out->diffuse[1] = 0.5f;
    out->diffuse[2] = 0.6f;
    out->diffuse[3] = 1.0f;
    out->specular[0] = 0.7f;
    out->specular[1] = 0.8f;
    out->specular[2] = 0.9f;
    out->specular[3] = 1.0f;
}

static void gl_state_test_fill_eye_light(int slot,
                                         ReplExportLightInfo *out) {
    gl_state_test_fill_light(slot, out);
    out->pos_is_eye_space = 1;
}

static void gl_state_test_fill_camera(ReplExportCameraBlock *block) {
    memset(block, 0, sizeof(*block));
    block->present = 1;
    snprintf(block->lines[0], sizeof(block->lines[0]),
             "  glTranslatef(10.25, 0, 0);");
}

static void populate_runtime_snapshot_fixture(const char *scene_hint) {
    char err[128];
    int foo_idx;
    GLCmd *doc_cmds;
    GLCmd *flat_cmds;
    FlatCmdLocalVars *flat_locals;
    EditorClipboardState *clipboard;
    UiCodePanelRuntimeState *code_panel;
    UiStatusState *status;
    EditorAutocompleteState *ac;
    VariablePanelDragState *drag;
    ReplImportExportState *io;
    ReplSceneRuntimeState *scenes;
    GlrPresentationState *presentation;
    GlrRenderState *glr_render;
    ReplRenderState *render;
    ReplayRuntimeState *replay;
    ReplFlatProgramState *flat_program;

    editor_input_set_text("glVertex3f(1, 2, 3)");
    editor_cursor_pos_set(6);
    repl_state_document_count_set(2);
    editor_state_edit_line_set(1);
    doc_cmds = repl_state_document_cmds_mut();
    doc_cmds[0].type = CMD_BEGIN;
    doc_cmds[0].valid = 1;
    editor_buffer_set_line(0, "  glBegin(GL_TRIANGLES);");
    doc_cmds[1].type = CMD_END;
    doc_cmds[1].valid = 1;
    editor_buffer_set_line(1, "  glEnd();");

    repl_state_flat_program_set_count(1);
    flat_program = repl_state_flat_program_mut();
    flat_program->overflow_cmd_count = 9000;
    flat_cmds = repl_state_flat_program_cmds_mut();
    flat_locals = repl_state_flat_program_local_vars_mut();
    flat_cmds[0].type = CMD_VERTEX3F;
    flat_cmds[0].valid = 1;
    flat_cmds[0].src_cmd_idx = 7;
    flat_locals[0].num_vars = 1;
    snprintf(flat_locals[0].vars[0].name, sizeof(flat_locals[0].vars[0].name),
             "%s", "i");
    flat_locals[0].vars[0].value = 3.0f;
    flat_program->dirty = 0;
    flat_program->user_lighting_enabled = 1;
    repl_state_flat_program_set_current_block(2, 5, 4);

    editor_state_selection_set(4, 7);
    clipboard = editor_state_clipboard_mut();
    clipboard->line_count = 1;
    snprintf(clipboard->lines[0], MAX_LINE_LEN, "glColor3f(1, 0, 0);");

    ui_state_help_mut()->visible = 1;
    editor_help_session_set_tab(1);
    editor_help_session_set_scroll(3);
    code_panel = ui_state_code_panel_mut();
    code_panel->panel_frac = 0.61f;
    code_panel->resizing_panel = 1;
    editor_scroll_set(9);
    editor_scroll_follow_cursor_set(1);
    editor_state_cursor_blink_mut()->cursor_visible = 0;
    editor_state_cursor_blink_mut()->blink_tick = 12;
    variable_panel_state_mut()->visible = 0;

    drag = variable_panel_drag_mut();
    drag->var_idx = 3;
    drag->log_mode = 1;
    drag->start_value = 2.5f;
    ui_state_profile_panel_mut()->mode = PROFILE_PANEL_HISTOGRAM;

    status = ui_state_status_mut();
    snprintf(status->text, sizeof(status->text), "%s", "state snapshot");
    status->ttl = 17;

    editor_state_search_mut()->active = 1;
    snprintf(editor_state_search_mut()->query,
             sizeof(editor_state_search_mut()->query),
             "%s", "vertex");
    editor_state_search_mut()->query_len = 6;
    editor_state_search_mut()->cursor_pos = 2;
    editor_state_search_mut()->hit_line_idx = 5;
    editor_state_search_mut()->hit_char_idx = 8;
    editor_state_search_mut()->hit_ordinal = 2;
    editor_state_search_mut()->match_count = 4;

    ac = editor_state_autocomplete_mut();
    ac->matches[0] = "glVertex3f";
    ac->matches[1] = "glVertex2f";
    ac->insert_matches[0] = "glVertex3f(";
    ac->insert_matches[1] = "glVertex2f(";
    ac->match_count = 2;
    ac->selected_idx = 1;
    snprintf(ac->ghost, sizeof(ac->ghost), "%s", "ghost text");
    snprintf(ac->hint, sizeof(ac->hint), "%s", "hint text");

    repl_state_variables_mut()->time_playing = 0;
    repl_state_variables_mut()->anim_time = 1.25f;
    foo_idx = repl_eval_find_predef_var_idx("foo");
    if (foo_idx < 0)
        repl_eval_declare_predef_var("foo", err, sizeof(err));
    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var declared", foo_idx >= 0);
    if (foo_idx >= 0)
        g_predef_vars_mut[foo_idx].value = 42.0f;

    glr_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    glr_camera_mut()->auto_rotate = 1;
    ui_state_pointer_set(321, 654, 2);
    ui_state_viewport_set_size(1440, 900);

    presentation = glr_state_presentation_mut();
    presentation->wireframe = 1;
    presentation->grid_theme = GRID_THEME_TRON;
    presentation->grid_major_idx = GRID_MAJOR_10;
    presentation->grid_extent_idx = GRID_EXTENT_CLOSE;
    presentation->axes_theme = AXES_THEME_NEON;
    presentation->show_normal_vectors = 1;
    presentation->show_vertex_indices = 0;
    presentation->show_vertex_points = 0;
    presentation->xform_guide_mode = RENDER3D_XFORM_GUIDE_OFF;
    presentation->show_light_indicators = 0;
    presentation->backdrop_mode = 1;
    presentation->highlight_current_poly = 0;
    presentation->ortho_mode = RENDER3D_VIEW_2D;
    presentation->wrap_at_comma = 0;
    presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
    /* focus_vertex storage was deleted in step 7a — no live readers
     * existed; the per-frame snapshot is computed from the document
     * inside glr_ctrl. */

    glr_render = glr_state_render_mut();
    glr_render->use_accum = 0;
    glr_render->accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    glr_render->accum_passes = 8;
    glr_render->multisample_enabled = 0;
    glr_render->line_smooth_enabled = 1;
    glr_render->point_attenuation_enabled = 0;

    render = repl_state_render_mut();
    render->light_enabled_mask = (1u << 1);  /* GL_LIGHT1 enabled */
    glr_render->lights[2].pos[0] = 7.5f;     /* dimensional data is app-owned now */
    render->clear_color[0] = 0.20f;
    render->clear_color[1] = 0.25f;
    render->clear_color[2] = 0.30f;
    render->clear_color[3] = 1.0f;

    replay = replay_state_mut();
    replay->active = 1;
    replay->state = REPLAY_PAUSED;
    replay->pc = 9;
    replay->mode = REPLAY_MODE_POLYGON;
    replay->speed = 12.5f;
    replay->accum = 0.75f;
    replay->fade_speed = 6.0f;
    replay->src_line_idx = 7;
    replay->total_flat_cmds = 13;
    replay->expand_args = 0;
    replay->normal_display = REPLAY_NORMAL_DISPLAY_DIRECTION;
    replay->vertex_label = 1;

    scenes = repl_state_scenes_mut();
    scenes->active_example_idx = 3;
    repl_set_workspace_dir("/tmp/repl-state-stage1");
    io = repl_state_import_export_mut();
    io->export_scene_name_hint = scene_hint;
    snprintf(io->pending_scene_name, sizeof(io->pending_scene_name), "%s",
             "pending scene");
    snprintf(io->pending_workspace_dir, sizeof(io->pending_workspace_dir), "%s",
             "/tmp/pending-workspace");
}

static void test_capture_restore_round_trip(void) {
    static ReplRuntimeState snapshot;
    static ReplRuntimeState round_trip;
    static GlrState glr_snap;
    static GlrState glr_round_trip;
    static GlrCameraState camera_snap;
    static GlrCameraState camera_round_trip;
    static EditorState editor_snap;
    static EditorState editor_round_trip;
    static UiState ui_snap;
    static UiState ui_round_trip;
    static VariablePanelState varpanel_snap;
    static VariablePanelState varpanel_round_trip;
    static ReplayRuntimeState replay_snap;
    static ReplayRuntimeState replay_round_trip;
    static EditorHelpSession help_snap;
    static EditorHelpSession help_round_trip;
    static const char *scene_hint = "Captured Scene";
    int foo_idx;

    glr_ctrl_reset_all();
    populate_runtime_snapshot_fixture(scene_hint);
    repl_state_capture(&snapshot);
    /* repl_state_restore intentionally does NOT preserve the flat program's
     * derived invalidation state: it forces a full re-flatten (dirty=1) and
     * drops rebake eligibility / pending args-dirt, because the restored
     * document invalidates the compiled-expression cache wholesale. So
     * normalize those three fields in the pre-restore snapshot to the values
     * a restore produces, leaving the whole-struct memcmp below to assert the
     * genuinely-preserved state round-trips. */
    snapshot.flat_program.dirty = 1;
    snapshot.flat_program.args_dirty_mask = 0;
    snapshot.flat_program.rebake_ok = 0;
    glr_state_capture(&glr_snap);
    glr_camera_capture(&camera_snap);
    editor_state_capture(&editor_snap);
    ui_state_capture(&ui_snap);
    variable_panel_state_capture(&varpanel_snap);
    replay_state_capture(&replay_snap);
    editor_help_session_capture(&help_snap);
    glr_ctrl_reset_all();
    repl_state_restore(&snapshot);
    glr_state_restore(&glr_snap);
    glr_camera_restore(&camera_snap);
    editor_state_restore(&editor_snap);
    ui_state_restore(&ui_snap);
    variable_panel_state_restore(&varpanel_snap);
    replay_state_restore(&replay_snap);
    editor_help_session_restore(&help_snap);
    repl_state_capture(&round_trip);
    glr_state_capture(&glr_round_trip);
    glr_camera_capture(&camera_round_trip);
    editor_state_capture(&editor_round_trip);
    ui_state_capture(&ui_round_trip);
    variable_panel_state_capture(&varpanel_round_trip);
    replay_state_capture(&replay_round_trip);
    editor_help_session_capture(&help_round_trip);

    ASSERT_STR("input restored",
               editor_input_text(),
               "glVertex3f(1, 2, 3)");
    ASSERT_INT("cursor restored", editor_cursor_pos(), 6);
    ASSERT_INT("document count restored", repl_state_document_count(), 2);
    ASSERT_INT("edit line restored", editor_state_edit_line(), 1);
    ASSERT_INT("document cmd type restored", repl_state_document_cmds()[0].type, CMD_BEGIN);
    ASSERT_STR("document cmd source restored",
               editor_buffer_line(0),
               "  glBegin(GL_TRIANGLES);");
    ASSERT_INT("flat count restored", repl_state_flat_program_count(), 1);
    ASSERT_INT("flat overflow count restored",
               repl_state_flat_program_view().overflow_cmd_count, 9000);
    ASSERT_INT("flat cmd type restored", repl_state_flat_program_cmds()[0].type, CMD_VERTEX3F);
    ASSERT_INT("flat local var count restored",
               repl_state_flat_program_local_vars_mut()[0].num_vars, 1);
    ASSERT_INT("flat lighting restored",
               repl_state_flat_program_user_lighting_enabled(), 1);
    ASSERT_INT("selection anchor restored", editor_state_selection().anchor_idx, 4);
    ASSERT_INT("selection end restored", editor_state_selection().end_idx, 7);
    ASSERT_INT("clipboard count restored", editor_state_clipboard()->line_count, 1);
    ASSERT_TRUE("clipboard line restored",
                strcmp(editor_state_clipboard()->lines[0], "glColor3f(1, 0, 0);") == 0);
    ASSERT_INT("help restored", ui_state_help().visible, 1);
    ASSERT_INT("help tab restored", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help scroll restored", editor_help_session_scroll(), 3);
    ASSERT_TRUE("code panel frac restored", ui_state_code_panel().panel_frac == 0.61f);
    ASSERT_INT("code panel resizing restored", ui_state_code_panel().resizing_panel, 1);
    ASSERT_INT("code panel scroll restored", editor_scroll(), 9);
    ASSERT_INT("code panel follow restored",
               editor_scroll_follow_cursor(), 1);
    ASSERT_INT("code panel cursor visible restored",
               editor_state_cursor_blink().cursor_visible, 0);
    ASSERT_INT("code panel blink restored", editor_state_cursor_blink().blink_tick, 12);
    ASSERT_INT("variable panel restored", variable_panel_view().visible, 0);
    ASSERT_INT("variable drag idx restored", variable_panel_drag().var_idx, 3);
    ASSERT_INT("variable drag log restored", variable_panel_drag().log_mode, 1);
    ASSERT_TRUE("variable drag value restored",
                variable_panel_drag().start_value == 2.5f);
    ASSERT_INT("profile panel restored",
               ui_state_profile_panel().mode, PROFILE_PANEL_HISTOGRAM);
    {
        UiStatusState status = ui_state_status();
        const EditorSearchState *search = editor_state_search();
        const EditorAutocompleteState *autocomplete = editor_state_autocomplete();

        ASSERT_STR("status text restored", status.text, "state snapshot");
        ASSERT_INT("status ttl restored", status.ttl, 17);
        ASSERT_INT("search active restored", search->active, 1);
        ASSERT_STR("search query restored", search->query, "vertex");
        ASSERT_INT("search cursor restored", search->cursor_pos, 2);
        ASSERT_INT("search hit line restored", search->hit_line_idx, 5);
        ASSERT_INT("search hit char restored", search->hit_char_idx, 8);
        ASSERT_INT("search ordinal restored", search->hit_ordinal, 2);
        ASSERT_INT("search count restored", search->match_count, 4);
        ASSERT_INT("autocomplete count restored", autocomplete->match_count, 2);
        ASSERT_INT("autocomplete selection restored",
               autocomplete->selected_idx, 1);
        ASSERT_STR("autocomplete ghost restored", autocomplete->ghost, "ghost text");
        ASSERT_STR("autocomplete hint restored", autocomplete->hint, "hint text");
        ASSERT_STR("autocomplete match restored",
               autocomplete->matches[0], "glVertex3f");
        ASSERT_STR("autocomplete insert restored",
               autocomplete->insert_matches[1], "glVertex2f(");
    }
    ASSERT_INT("time playing restored", repl_state_variables().time_playing, 0);
    ASSERT_TRUE("anim time restored", repl_state_variables().anim_time == 1.25f);
    ASSERT_TRUE("camera rx restored", glr_camera().rx == 11.0f);
    ASSERT_TRUE("camera ry restored", glr_camera().ry == 22.0f);
    ASSERT_TRUE("camera dist restored", glr_camera().dist == 7.5f);
    ASSERT_TRUE("camera tx restored", glr_camera().tx == 0.5f);
    ASSERT_TRUE("camera ty restored", glr_camera().ty == -0.25f);
    ASSERT_TRUE("camera tz restored", glr_camera().tz == 1.75f);
    ASSERT_TRUE("camera glow restored", glr_camera().motion_glow == 0.9f);
    ASSERT_INT("camera rotate restored", glr_camera().auto_rotate, 1);
    ASSERT_INT("pointer x restored", ui_state_pointer().mouse_x, 321);
    ASSERT_INT("pointer y restored", ui_state_pointer().mouse_y, 654);
    ASSERT_INT("pointer button restored", ui_state_pointer().mouse_button, 2);
    ASSERT_INT("viewport width restored", ui_state_viewport().window_w, 1440);
    ASSERT_INT("viewport height restored", ui_state_viewport().window_h, 900);
    ASSERT_INT("presentation wireframe restored", glr_state_presentation().wireframe, 1);
    ASSERT_INT("presentation grid restored", glr_state_presentation().grid_theme, GRID_THEME_TRON);
    ASSERT_INT("presentation layout restored",
               glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
    /* focus_vertex storage was deleted in step 7a; per-frame compute
     * lives on glr_ctrl. */
    ASSERT_INT("render use accum restored", glr_state_render().use_accum, 0);
    ASSERT_INT("render accum effect restored", glr_state_render().accum_effect, RENDER3D_ACCUM_EFFECT_OFF);
    ASSERT_INT("render passes restored", glr_state_render().accum_passes, 8);
    ASSERT_INT("render light enable mask restored",
               (int)repl_state_render().light_enabled_mask, (int)(1u << 1));
    ASSERT_TRUE("render light pos restored (app-owned)",
                glr_state_render().lights[2].pos[0] == 7.5f);
    ASSERT_TRUE("render clear color restored",
                repl_state_render().clear_color[2] == 0.30f);
    ASSERT_INT("replay active restored", replay_state_view().active, 1);
    ASSERT_INT("replay state restored", replay_state_view().state, REPLAY_PAUSED);
    ASSERT_INT("replay mode restored", replay_state_view().mode, REPLAY_MODE_POLYGON);
    ASSERT_INT("replay pc restored", replay_state_view().pc, 9);
    ASSERT_TRUE("replay speed restored", replay_state_view().speed == 12.5f);
    ASSERT_INT("replay src line restored", replay_state_view().src_line_idx, 7);
    ASSERT_INT("replay expand restored", replay_state_view().expand_args, 0);
    ASSERT_INT("replay normal display restored",
               replay_state_view().normal_display,
               REPLAY_NORMAL_DISPLAY_DIRECTION);
    ASSERT_INT("replay vertex label restored",
               replay_state_view().vertex_label, 1);
    ASSERT_INT("active example restored", repl_state_scenes().active_example_idx, 3);
    ASSERT_STR("workspace restored",
               repl_workspace_dir(),
               "/tmp/repl-state-stage1");
    ASSERT_STR("pending scene restored",
               repl_state_import_export().pending_scene_name,
               "pending scene");
    ASSERT_STR("pending workspace restored",
               repl_state_import_export().pending_workspace_dir,
               "/tmp/pending-workspace");
    ASSERT_STR("scene hint restored",
               repl_state_import_export().export_scene_name_hint,
               scene_hint);
    ASSERT_TRUE("workspace header count restored",
                repl_state_import_export().workspace_header_line_count >= 3);
    ASSERT_STR("workspace header banner restored",
               repl_state_import_export().workspace_header_lines[0],
               "/* @workspace: REPL state (auto-saved) */");
    ASSERT_STR("workspace header scene restored",
               repl_state_import_export().workspace_header_lines[1],
               "/* @scene-name Captured Scene */");
    ASSERT_STR("workspace header dir restored",
               repl_state_import_export().workspace_header_lines[2],
               "/* @workspace-dir /tmp/repl-state-stage1 */");

    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var restored", foo_idx >= 0);
    if (foo_idx >= 0)
        ASSERT_TRUE("foo value restored", g_predef_vars[foo_idx].value == 42.0f);
    ASSERT_TRUE("runtime snapshot round trip",
                memcmp(&snapshot, &round_trip, sizeof(snapshot)) == 0);
    ASSERT_TRUE("glr snapshot round trip",
                memcmp(&glr_snap, &glr_round_trip, sizeof(glr_snap)) == 0);
    ASSERT_TRUE("camera snapshot round trip",
                memcmp(&camera_snap, &camera_round_trip, sizeof(camera_snap)) == 0);
    ASSERT_TRUE("editor snapshot round trip",
                memcmp(&editor_snap, &editor_round_trip, sizeof(editor_snap)) == 0);
    ASSERT_TRUE("ui snapshot round trip",
                memcmp(&ui_snap, &ui_round_trip, sizeof(ui_snap)) == 0);
}

static void test_reset_all_restores_default_runtime(void) {
    static ReplRuntimeState defaults;
    static ReplRuntimeState reset_state;

    glr_ctrl_reset_all();
    repl_state_capture(&defaults);

    populate_runtime_snapshot_fixture("Reset Scene");
    glr_ctrl_reset_all();
    repl_state_capture(&reset_state);

    ASSERT_TRUE("reset_all restores default snapshot",
                memcmp(&defaults, &reset_state, sizeof(defaults)) == 0);
    ASSERT_INT("reset_all help hidden", ui_state_help().visible, 0);
    ASSERT_INT("reset_all panel scroll", editor_scroll(), 0);
    ASSERT_INT("reset_all replay mode", replay_state_view().mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("reset_all replay expand", replay_state_view().expand_args, 1);
    ASSERT_INT("reset_all replay normals",
               replay_state_view().normal_display,
               REPLAY_NORMAL_DISPLAY_OFF);
    ASSERT_INT("reset_all replay vertex label",
               replay_state_view().vertex_label, 0);
    ASSERT_STR("reset_all workspace dir", repl_workspace_dir(), "");
    ASSERT_INT("reset_all workspace header count",
               repl_state_import_export().workspace_header_line_count,
               defaults.import_export.workspace_header_line_count);
    ASSERT_STR("reset_all workspace header banner",
               repl_state_import_export().workspace_header_lines[0],
               "/* @workspace: REPL state (auto-saved) */");
}

/* Regression (feedback P3): the per-line override list cap must
 * cover the full document size — otherwise layout (which reads the
 * snapshot list) and render (formerly recomputing live) drift past
 * the cap, breaking wrap-row counts / scroll / hit-testing.
 *
 * Push 600 distinct overrides; verify all 600 are retrievable. The
 * pre-fix cap of 512 silently dropped entries 512..599; this test
 * fails on that build (override_for(513) returns NULL) and passes
 * after MAX_LINE_OVERRIDES bumps to MAX_EDITOR_COMMANDS. */
static void test_line_override_cap_covers_busy_replay(void) {
    const int N = 600;
    char buf[64];
    int append_failures = 0;

    editor_state_line_overrides_clear();
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "// override-row %d", i);
        if (!editor_state_line_overrides_append(i, buf))
            append_failures++;
    }
    ASSERT_INT("override list: all 600 appends succeed",
               append_failures, 0);
    ASSERT_INT("override list: count tracks total",
               editor_state_line_overrides()->count, N);

    /* Past the old 512 cap. Pre-fix this returns NULL because the
     * append at 513 silently failed; post-fix the cap covers the
     * whole document so the lookup succeeds. Guard against NULL
     * before strcmp so the test fails cleanly on pre-fix instead
     * of segfaulting in ASSERT_STR. */
    {
        const char *got = editor_state_line_override_for(513);
        snprintf(buf, sizeof(buf), "// override-row %d", 513);
        ASSERT_TRUE("override-for(513) returns the pushed text past old cap",
                    got != NULL);
        ASSERT_STR("override-for(513) text matches",
                   got ? got : "<null>", buf);
    }

    /* Sanity: lookups for an entry that was never pushed return NULL. */
    ASSERT_TRUE("override-for(out-of-range) returns NULL",
                editor_state_line_override_for(N + 50) == NULL);

    editor_state_line_overrides_clear();
}

/* Audit #10/#36: glr_camera_restore must clear stale momentum velocities.
 * Without the fix, zoom velocity injected before the capture persists
 * through restore and causes dist to drift on subsequent ticks. */
static void test_camera_restore_clears_momentum(void) {
    glr_ctrl_reset_all();

    glr_camera_set(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_add_zoom_velocity(5.0f);

    GlrCameraState snap;
    glr_camera_capture(&snap);

    glr_camera_restore(&snap);

    float dist_before = glr_camera().dist;
    for (int i = 0; i < 10; i++)
        glr_camera_tick();

    ASSERT_TRUE("camera dist unchanged after restore+tick (no residual momentum)",
                fabsf(glr_camera().dist - dist_before) < 1e-4f);
}

/* Audit #9: SOURCE_TEXT_LOAD_ALL with oversized count must fail rather than
 * silently clamp/truncate. */
static void test_source_document_load_all_rejects_oversized(void) {
    glr_ctrl_reset_all();

    source_document_insert_line(0, "glBegin(GL_TRIANGLES);");
    source_document_insert_line(1, "glEnd();");

    SourceTextChange change;
    memset(&change, 0, sizeof(change));
    change.kind = SOURCE_TEXT_LOAD_ALL;
    change.count = MAX_COMMIT_CMDS + 5;
    for (int i = 0; i < MAX_COMMIT_CMDS; i++) {
        snprintf(change.text[i], MAX_LINE_LEN, "glVertex3f(%d,0,0);", i);
    }

    int ok = source_document_apply_change(&change);
    ASSERT_INT("LOAD_ALL with oversized count is rejected", ok, 0);
    ASSERT_INT("LOAD_ALL reject keeps line count unchanged",
               source_document_view().line_count, 2);
    ASSERT_TRUE("LOAD_ALL reject keeps first line",
                strcmp(source_text_line(source_document_view(), 0),
                       "glBegin(GL_TRIANGLES);") == 0);
    ASSERT_TRUE("LOAD_ALL reject keeps second line",
                strcmp(source_text_line(source_document_view(), 1),
                       "glEnd();") == 0);
}

/* Audit #8: apply_change combined shape must be atomic on failure. If the
 * insert-many leg cannot fit, neither the pre-delete nor any insert may stick. */
static void test_source_document_apply_change_combined_atomic_on_failure(void) {
    glr_ctrl_reset_all();

    editor_buffer_set_count(MAX_EDITOR_COMMANDS - 1);
    editor_buffer_set_line(0, "line0");
    editor_buffer_set_line(1, "line1");
    editor_buffer_set_line(2, "line2");
    editor_buffer_set_line(3, "line3");
    editor_buffer_set_line(MAX_EDITOR_COMMANDS - 2, "tail");

    SourceTextChange change;
    memset(&change, 0, sizeof(change));
    change.kind = SOURCE_TEXT_INSERT_MANY;
    change.delete_pos = 1;
    change.delete_count = 2;
    change.pos = 1;
    change.count = 4; /* final size would exceed MAX_EDITOR_COMMANDS by one */
    snprintf(change.text[0], MAX_LINE_LEN, "new1");
    snprintf(change.text[1], MAX_LINE_LEN, "new2");
    snprintf(change.text[2], MAX_LINE_LEN, "new3");
    snprintf(change.text[3], MAX_LINE_LEN, "new4");

    int ok = source_document_apply_change(&change);
    ASSERT_INT("combined delete+insert-many rejects overflow", ok, 0);
    ASSERT_INT("combined failure keeps line count unchanged",
               source_document_view().line_count, MAX_EDITOR_COMMANDS - 1);
    ASSERT_TRUE("combined failure keeps line1",
                strcmp(source_text_line(source_document_view(), 1),
                       "line1") == 0);
    ASSERT_TRUE("combined failure keeps line2",
                strcmp(source_text_line(source_document_view(), 2),
                       "line2") == 0);
    ASSERT_TRUE("combined failure keeps tail",
                strcmp(source_text_line(source_document_view(), MAX_EDITOR_COMMANDS - 2),
                       "tail") == 0);
}

/* Audit #8: apply_change combined shape (pre-insert delete + INSERT_MANY). */
static void test_source_document_apply_change_combined(void) {
    glr_ctrl_reset_all();

    source_document_insert_line(0, "glBegin(GL_TRIANGLES);");
    source_document_insert_line(1, "glVertex3f(0,0,0);");
    source_document_insert_line(2, "glVertex3f(1,0,0);");
    source_document_insert_line(3, "glEnd();");
    ASSERT_INT("setup: 4 lines", source_document_view().line_count, 4);

    SourceTextChange change;
    memset(&change, 0, sizeof(change));
    change.kind = SOURCE_TEXT_INSERT_MANY;
    change.delete_pos = 1;
    change.delete_count = 2;
    change.pos = 1;
    change.count = 3;
    snprintf(change.text[0], MAX_LINE_LEN, "glVertex3f(0,1,0);");
    snprintf(change.text[1], MAX_LINE_LEN, "glVertex3f(1,1,0);");
    snprintf(change.text[2], MAX_LINE_LEN, "glVertex3f(0.5,0,1);");

    int ok = source_document_apply_change(&change);
    ASSERT_INT("combined delete+insert succeeds", ok, 1);
    ASSERT_INT("line count after replace-2-with-3",
               source_document_view().line_count, 5);

    SourceTextView v = source_document_view();
    ASSERT_TRUE("line 0 unchanged",
                strcmp(v.lines[0], "glBegin(GL_TRIANGLES);") == 0);
    ASSERT_TRUE("line 1 is first insert",
                strcmp(v.lines[1], "glVertex3f(0,1,0);") == 0);
    ASSERT_TRUE("line 2 is second insert",
                strcmp(v.lines[2], "glVertex3f(1,1,0);") == 0);
    ASSERT_TRUE("line 3 is third insert",
                strcmp(v.lines[3], "glVertex3f(0.5,0,1);") == 0);
    ASSERT_TRUE("line 4 is original glEnd",
                strcmp(v.lines[4], "glEnd();") == 0);
}

/* Audit follow-up: INSERT_MANY must reject invalid count values before any
 * pre-delete can mutate the document. */
static void test_source_document_insert_many_rejects_invalid_count(void) {
    glr_ctrl_reset_all();

    source_document_insert_line(0, "line0");
    source_document_insert_line(1, "line1");
    source_document_insert_line(2, "line2");
    ASSERT_INT("setup: 3 lines", source_document_view().line_count, 3);

    SourceTextChange change;
    memset(&change, 0, sizeof(change));
    change.kind = SOURCE_TEXT_INSERT_MANY;
    change.delete_pos = 1;
    change.delete_count = 1;
    change.pos = 1;

    /* count <= 0 is invalid for INSERT_MANY. */
    change.count = 0;
    int ok = source_document_apply_change(&change);
    ASSERT_INT("insert-many count=0 rejected", ok, 0);
    ASSERT_INT("count=0 reject keeps line count", source_document_view().line_count, 3);
    ASSERT_TRUE("count=0 reject keeps middle line",
                strcmp(source_text_line(source_document_view(), 1), "line1") == 0);

    /* count > MAX_COMMIT_CMDS is invalid and must not read past text[]. */
    change.count = MAX_COMMIT_CMDS + 1;
    for (int i = 0; i < MAX_COMMIT_CMDS; i++)
        snprintf(change.text[i], MAX_LINE_LEN, "new-%d", i);

    ok = source_document_apply_change(&change);
    ASSERT_INT("insert-many oversized count rejected", ok, 0);
    ASSERT_INT("oversized reject keeps line count", source_document_view().line_count, 3);
    ASSERT_TRUE("oversized reject keeps middle line",
                strcmp(source_text_line(source_document_view(), 1), "line1") == 0);
}

static void test_camera_ease_to_default_uses_scene_default(void) {
    glr_ctrl_reset_all();

    GlrCameraState pose = {
        .rx = 45.0f, .ry = 90.0f, .dist = 10.0f,
        .tx = 1.0f, .ty = 2.0f, .tz = 3.0f,
        .motion_glow = 0.0f, .auto_rotate = 0
    };
    glr_camera_set_scene_default(&pose);
    glr_camera_set(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    glr_camera_ease_to_default();

    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("ease_to_default reaches scene rx",
                fabsf(cam.rx - 45.0f) < 0.5f);
    ASSERT_TRUE("ease_to_default reaches scene ry",
                fabsf(cam.ry - 90.0f) < 0.5f);
    ASSERT_TRUE("ease_to_default reaches scene dist",
                fabsf(cam.dist - 10.0f) < 0.5f);
    ASSERT_TRUE("ease_to_default reaches scene tx",
                fabsf(cam.tx - 1.0f) < 0.1f);
}

static void test_camera_clear_scene_default_falls_back(void) {
    glr_ctrl_reset_all();

    GlrCameraState pose = {
        .rx = 45.0f, .ry = 90.0f, .dist = 10.0f,
        .tx = 1.0f, .ty = 2.0f, .tz = 3.0f,
        .motion_glow = 0.0f, .auto_rotate = 0
    };
    glr_camera_set_scene_default(&pose);
    glr_camera_clear_scene_default();
    glr_camera_set(0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    glr_camera_ease_to_default();

    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("after clear, ease_to_default reaches built-in rx (20)",
                fabsf(cam.rx - 20.0f) < 0.5f);
    ASSERT_TRUE("after clear, ease_to_default reaches built-in ry (30)",
                fabsf(cam.ry - 30.0f) < 0.5f);
    ASSERT_TRUE("after clear, ease_to_default reaches built-in dist (5)",
                fabsf(cam.dist - 5.0f) < 0.5f);
}

static const char *const k_test_camera_example[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -8.0f);",
    "glRotatef(30.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(40.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(1.0f, 2.0f, 3.0f);",
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    NULL
};

static void load_test_camera_example(void) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    editor_state_edit_line_set(repl_load_example_lines_for_test(k_test_camera_example));
}

static void test_example_load_sets_scene_camera_default(void) {
    glr_ctrl_reset_all();

    load_test_camera_example();

    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState after_load = glr_camera();
    ASSERT_TRUE("example camera dist ~8",
                fabsf(after_load.dist - 8.0f) < 0.5f);
    ASSERT_TRUE("example camera rx ~30",
                fabsf(after_load.rx - 30.0f) < 0.5f);

    glr_camera_set(0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    glr_camera_ease_to_default();

    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("Ctrl+Shift+C reaches example dist (8), not built-in (5)",
                fabsf(cam.dist - 8.0f) < 0.5f);
    ASSERT_TRUE("Ctrl+Shift+C reaches example rx (30)",
                fabsf(cam.rx - 30.0f) < 0.5f);
    ASSERT_TRUE("Ctrl+Shift+C reaches example ry (40)",
                fabsf(cam.ry - 40.0f) < 0.5f);
}

static void test_user_scene_load_clears_scene_camera_default(void) {
    glr_ctrl_reset_all();

    load_test_camera_example();
    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    glr_scene_load_user_slot(0);

    glr_camera_set(0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_ease_to_default();
    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("after user-scene load, falls back to built-in dist (5)",
                fabsf(cam.dist - 5.0f) < 0.5f);
    ASSERT_TRUE("after user-scene load, falls back to built-in rx (20)",
                fabsf(cam.rx - 20.0f) < 0.5f);
    ASSERT_TRUE("after user-scene load, falls back to built-in ry (30)",
                fabsf(cam.ry - 30.0f) < 0.5f);
}

/* Sibling of test_user_scene_load_clears_scene_camera_default: the
 * LOAD_WORKSPACE menu path (glr_actions.c) must also clear the per-scene
 * camera default. Without this, after load-example-with-camera →
 * load-workspace, a Ctrl+Shift+C still eases to the previous example's
 * pose instead of the built-in default. */
static void test_workspace_load_clears_scene_camera_default(void) {
    char temp_dir[] = "/tmp/test_workspace_camera.XXXXXX";
    char *dir = mkdtemp(temp_dir);
    ASSERT_TRUE("mkdtemp workspace dir", dir != NULL);
    if (!dir) return;

    glr_ctrl_reset_all();

    /* Test camera example has a // camera block → cam_apply_example_block records
     * the pose as the per-scene camera default. */
    load_test_camera_example();
    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    /* Bind the workspace dir without saving anything to it; the menu
     * handler reads repl_workspace_dir() and falls through to
     * repl_load_workspace(dir), which returns 0 on an empty dir. The
     * bug is whether the menu handler clears the scene camera default
     * *before* calling repl_load_workspace, not in load itself. */
    repl_set_workspace_dir(dir);

    int activated = glr_action_menu_item_activate(GLR_MENU_FILE,
                                                  GLR_FILE_ITEM_LOAD_WORKSPACE);
    ASSERT_INT("menu LOAD_WORKSPACE activated", activated, 1);

    /* Ctrl+Shift+C → ease_to_default. With the scene default still set
     * (bug), the camera converges to the test example's pose (dist=8, rx=30,
     * ry=40). After the fix, it falls back to built-in (dist=5, rx=20,
     * ry=30). */
    glr_camera_set(0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_ease_to_default();
    for (int i = 0; i < 500; i++)
        glr_camera_tick();

    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("after workspace load, falls back to built-in dist (5)",
                fabsf(cam.dist - 5.0f) < 0.5f);
    ASSERT_TRUE("after workspace load, falls back to built-in rx (20)",
                fabsf(cam.rx - 20.0f) < 0.5f);
    ASSERT_TRUE("after workspace load, falls back to built-in ry (30)",
                fabsf(cam.ry - 30.0f) < 0.5f);

    rmdir(dir);
}

/* glr_camera_set_target_decay overrides the per-ease decay for the
 * currently-active ease. Verify the override applies on the next tick
 * by setting a fast decay (0.5) and watching half the distance fall
 * away in a single frame — well beyond what the global default (0.93,
 * ~7% per frame) could produce. The 3D->2D view-mode transition uses
 * the same override mechanism via GLR_VIEW_CAMERA_TO_2D_DECAY. */
static void test_camera_target_decay_override_applies(void) {
    glr_ctrl_reset_all();

    glr_camera_set(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_ease_to(100.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_set_target_decay(0.5f);
    glr_camera_tick();

    GlrCameraState cam = glr_camera();
    /* k = 1 - 0.5 = 0.5, so one tick covers 50% of the 100 deg gap;
     * window the assertion away from both endpoints so a revert to
     * default decay (would yield rx ~7) fails clearly. */
    ASSERT_TRUE("override decay 0.5 covers ~50% of rx distance in 1 tick",
                cam.rx > 40.0f && cam.rx < 60.0f);
}

/* Every fresh glr_camera_ease_to call must reset the per-ease decay
 * back to the global default; otherwise a fast override from a prior
 * ease (e.g. the 3D->2D leg) could leak into unrelated motion like
 * Ctrl+Shift+C or a scene-default restore. */
static void test_camera_target_decay_override_resets_on_new_ease(void) {
    glr_ctrl_reset_all();

    glr_camera_set(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_ease_to(50.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_set_target_decay(0.5f);

    /* Start a brand-new ease before any ticks. The decay override
     * should reset to the global default, so the next tick covers
     * only ~(1 - 0.93) = 7% of the 100 deg distance. */
    glr_camera_ease_to(100.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_tick();

    GlrCameraState cam = glr_camera();
    /* Default decay 0.93 → ~7% per frame of 100 = ~7 deg of progress.
     * The 0.5 override (if it had leaked) would put rx near 50, so a
     * wide window around 7 is enough to catch the regression. */
    ASSERT_TRUE("new ease resets decay (rx ~7 deg, not ~50)",
                cam.rx > 4.0f && cam.rx < 12.0f);
}

/* Time advance/set route t's change by the flat program's dependency
 * masks (repl_state_notify_predef_value_changed) — value-only roots
 * accumulate into args_dirty_mask, structural roots take the full flag,
 * unused roots are a no-op. Seed each mask state directly; the
 * source-to-mask derivation is covered by the flatten dep tests. */
static void test_time_dirty_gate_routes_by_dep_masks(void) {
    glr_ctrl_reset_all();

    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("time dirty gate has t predef", t_idx >= 0);
    ReplExprDepMask t_bit = (ReplExprDepMask)1u << t_idx;

    {
        /* t unused by the current flat program: advancing is a no-op. */
        repl_state_flat_program_set_dep_state(0, 0, 1);
        repl_state_flat_program_clear_dirty();

        repl_state_time_advance(0.25f);
        ASSERT_INT("time advance with unused t leaves flat clean",
                   repl_state_flat_program_dirty(), 0);
        ASSERT_INT("time advance with unused t leaves args-dirty clean",
                   (int)repl_state_flat_program_args_dirty_mask(), 0);
        ASSERT_TRUE("time advance still updates visible t",
                    fabsf(g_predef_vars[t_idx].value - 0.25f) < 1e-6f);

        repl_state_time_set(2.0f);
        ASSERT_INT("set_time with unused t leaves flat clean",
                   repl_state_flat_program_dirty(), 0);
    }

    {
        /* t as a value-only root: the change routes to t's args-dirty
         * bit (an in-place rebake suffices), not the full flag. */
        repl_state_flat_program_set_dep_state(0, t_bit, 1);
        repl_state_flat_program_clear_dirty();

        repl_state_time_advance(0.25f);
        ASSERT_INT("time advance with value-only t leaves full flag clean",
                   repl_state_flat_program_dirty(), 0);
        ASSERT_INT("time advance with value-only t sets t's args-dirty bit",
                   (int)((repl_state_flat_program_args_dirty_mask()
                          >> t_idx) & 1u), 1);

        repl_state_flat_program_set_dep_state(0, t_bit, 1);
        repl_state_time_set(4.0f);
        ASSERT_INT("set_time with value-only t sets t's args-dirty bit",
                   (int)((repl_state_flat_program_args_dirty_mask()
                          >> t_idx) & 1u), 1);
    }

    {
        /* t as a structural root: full dirty, pending args dirt cleared. */
        repl_state_flat_program_set_dep_state(t_bit, t_bit, 1);
        repl_state_flat_program_clear_dirty();

        repl_state_time_advance(0.25f);
        ASSERT_INT("time advance with structural t marks flat dirty",
                   repl_state_flat_program_dirty(), 1);
        ASSERT_INT("time advance with structural t clears args-dirty",
                   (int)repl_state_flat_program_args_dirty_mask(), 0);
    }

    {
        /* Value-only root without rebake support escalates to full. */
        repl_state_flat_program_set_dep_state(0, t_bit, 0);
        repl_state_flat_program_clear_dirty();

        repl_state_time_advance(0.25f);
        ASSERT_INT("value change without rebake escalates to full dirty",
                   repl_state_flat_program_dirty(), 1);
        ASSERT_INT("value change without rebake leaves args-dirty clean",
                   (int)repl_state_flat_program_args_dirty_mask(), 0);
    }
}

static void test_gl_state_report_tracks_explicit_writes_before_checkpoint(void) {
    GLCmd cmds[5];
    FlatProgramView program;
    ReplGlStateReport report;
    const ReplGlStateReportRow *row;

    printf("--- repl_state OpenGL checkpoint report ---\n");

    cmds[0] = gl_state_test_cmd(CMD_DEPTH_FUNC, 0);
    cmds[0].args[0] = (float)GL_LESS;
    cmds[0].num_args = 1;
    cmds[1] = gl_state_test_cmd(CMD_ENABLE, 2);
    cmds[1].args[0] = (float)GL_BLEND;
    cmds[1].num_args = 1;
    cmds[2] = gl_state_test_cmd(CMD_DISABLE, 3);
    cmds[2].args[0] = (float)GL_BLEND;
    cmds[2].num_args = 1;
    cmds[3] = gl_state_test_cmd(CMD_ENABLE, 4);
    cmds[3].args[0] = (float)GL_MULTISAMPLE;
    cmds[3].num_args = 1;
    cmds[4] = gl_state_test_cmd(CMD_COLOR4F, 5);
    cmds[4].args[0] = 1.0f;
    cmds[4].args[1] = 1.0f;
    cmds[4].args[2] = 1.0f;
    cmds[4].args[3] = 1.0f;
    cmds[4].num_args = 4;

    memset(&program, 0, sizeof(program));
    program.cmds = cmds;
    program.cmd_count = 5;

    repl_gl_state_report_at_line(program, 1, &report);
    row = gl_state_test_find_row(&report, "GL_DEPTH_FUNC");
    ASSERT_TRUE("explicit default depth func remains in report", row != NULL);
    if (row) {
        ASSERT_STR("depth func current", row->current, "GL_LESS");
        ASSERT_STR("depth func default", row->default_value, "GL_LESS");
        ASSERT_INT("explicit default depth func marked equal",
                   row->differs_from_default, 0);
        ASSERT_INT("depth func source is display",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("depth func source line is retained",
                   row->source.source_line_idx, 0);
    }
    row = gl_state_test_find_row(&report, "GL_BLEND");
    ASSERT_TRUE("init blend state precedes display checkpoint", row != NULL);
    if (row) {
        ASSERT_STR("init blend current", row->current, "GL_TRUE");
        ASSERT_INT("init blend source is init",
                   row->source.kind, REPL_GL_STATE_SOURCE_INIT);
        ASSERT_INT("init source has no display line",
                   row->source.source_line_idx, -1);
    }
    row = gl_state_test_find_row(&report, "GL_COLOR_CLEAR_VALUE");
    ASSERT_TRUE("init clear color is included", row != NULL);
    if (row) {
        ASSERT_STR("init clear color current", row->current,
                   "(0.1, 0.1, 0.1, 1)");
        ASSERT_INT("init clear color source is init",
                   row->source.kind, REPL_GL_STATE_SOURCE_INIT);
    }

    repl_gl_state_report_at_line(program, 3, &report);
    row = gl_state_test_find_row(&report, "GL_BLEND");
    ASSERT_TRUE("enable before checkpoint is reported", row != NULL);
    if (row) {
        ASSERT_STR("enabled blend current", row->current, "GL_TRUE");
        ASSERT_STR("blend default", row->default_value, "GL_FALSE");
        ASSERT_INT("enabled blend differs", row->differs_from_default, 1);
        ASSERT_INT("explicit same-value blend write becomes latest source",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("blend display source line", row->source.source_line_idx, 2);
    }

    repl_gl_state_report_at_line(program, 4, &report);
    row = gl_state_test_find_row(&report, "GL_BLEND");
    ASSERT_TRUE("write restored to default remains reported", row != NULL);
    if (row) {
        ASSERT_INT("disabled blend equals default",
                   row->differs_from_default, 0);
        ASSERT_INT("disabled blend source is display",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("disabled blend source line",
                   row->source.source_line_idx, 3);
    }

    repl_gl_state_report_at_line(program, 5, &report);
    row = gl_state_test_find_row(&report, "GL_MULTISAMPLE");
    ASSERT_TRUE("explicit multisample write is reported", row != NULL);
    if (row) {
        ASSERT_STR("multisample OpenGL default", row->default_value, "GL_TRUE");
        ASSERT_INT("enabled multisample equals initial state",
                   row->differs_from_default, 0);
        ASSERT_INT("multisample source is display",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("multisample source line", row->source.source_line_idx, 4);
    }

    repl_gl_state_report_at_line(program, 6, &report);
    row = gl_state_test_find_row(&report, "GL_CURRENT_COLOR");
    ASSERT_TRUE("explicit default color is reported", row != NULL);
    if (row) {
        ASSERT_INT("white current color equals default",
                   row->differs_from_default, 0);
        ASSERT_INT("color source is display",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("color source line", row->source.source_line_idx, 5);
    }
}

static void test_gl_state_report_tracks_fog(void) {
    GLCmd cmds[5];
    FlatProgramView program;
    ReplGlStateReport report;
    const ReplGlStateReportRow *row;

    printf("--- repl_state OpenGL fog report ---\n");

    cmds[0] = gl_state_test_cmd(CMD_ENABLE, 0);
    cmds[0].args[0] = (float)GL_FOG;
    cmds[0].num_args = 1;
    cmds[1] = gl_state_test_cmd(CMD_FOG_I, 1);
    cmds[1].args[0] = (float)GL_FOG_MODE;
    cmds[1].args[1] = (float)GL_EXP2;
    cmds[1].num_args = 2;
    cmds[2] = gl_state_test_cmd(CMD_FOG_F, 2);
    cmds[2].args[0] = (float)GL_FOG_DENSITY;
    cmds[2].args[1] = 0.25f;
    cmds[2].num_args = 2;
    cmds[3] = gl_state_test_cmd(CMD_FOG_FV, 3);
    cmds[3].args[0] = (float)GL_FOG_COLOR;
    cmds[3].args[1] = 0.05f;
    cmds[3].args[2] = 0.06f;
    cmds[3].args[3] = 0.08f;
    cmds[3].args[4] = 1.0f;
    cmds[3].num_args = 5;
    cmds[4] = gl_state_test_cmd(CMD_FOG_F, 4);
    cmds[4].args[0] = (float)GL_FOG_END;
    cmds[4].args[1] = 1.0f;
    cmds[4].num_args = 2;

    memset(&program, 0, sizeof(program));
    program.cmds = cmds;
    program.cmd_count = 5;

    repl_gl_state_report_at_line(program, 5, &report);
    row = gl_state_test_find_row(&report, "GL_FOG");
    ASSERT_TRUE("fog cap reported", row != NULL);
    if (row) {
        ASSERT_STR("fog cap current", row->current, "GL_TRUE");
        ASSERT_STR("fog cap default", row->default_value, "GL_FALSE");
        ASSERT_INT("fog cap differs", row->differs_from_default, 1);
    }
    row = gl_state_test_find_row(&report, "GL_FOG_MODE");
    ASSERT_TRUE("fog mode reported", row != NULL);
    if (row) {
        ASSERT_STR("fog mode current", row->current, "GL_EXP2");
        ASSERT_STR("fog mode default", row->default_value, "GL_EXP");
        ASSERT_INT("fog mode differs", row->differs_from_default, 1);
        ASSERT_INT("fog mode source line", row->source.source_line_idx, 1);
    }
    row = gl_state_test_find_row(&report, "GL_FOG_DENSITY");
    ASSERT_TRUE("fog density reported", row != NULL);
    if (row) {
        ASSERT_STR("fog density current", row->current, "0.25");
        ASSERT_STR("fog density default", row->default_value, "1");
        ASSERT_INT("fog density differs", row->differs_from_default, 1);
    }
    row = gl_state_test_find_row(&report, "GL_FOG_COLOR");
    ASSERT_TRUE("fog color reported", row != NULL);
    if (row) {
        ASSERT_STR("fog color current", row->current, "(0.05, 0.06, 0.08, 1)");
        ASSERT_INT("fog color differs", row->differs_from_default, 1);
    }
    row = gl_state_test_find_row(&report, "GL_FOG_END");
    ASSERT_TRUE("fog end reported", row != NULL);
    if (row) {
        ASSERT_INT("explicit-default fog end marked equal",
                   row->differs_from_default, 0);
    }
    row = gl_state_test_find_row(&report, "GL_FOG_START");
    ASSERT_TRUE("untouched fog start absent", row == NULL);

    /* Before any fog write, no fog rows appear. */
    repl_gl_state_report_at_line(program, 0, &report);
    ASSERT_TRUE("no fog cap row before writes",
                gl_state_test_find_row(&report, "GL_FOG") == NULL);
    ASSERT_TRUE("no fog mode row before writes",
                gl_state_test_find_row(&report, "GL_FOG_MODE") == NULL);
}

static void test_gl_state_report_uses_flat_call_provenance(void) {
    GLCmd cmd = gl_state_test_cmd(CMD_CLEAR_COLOR, 20);
    FlatProgramView program;
    ReplGlStateReport report;
    const ReplGlStateReportRow *row;

    cmd.root_call_src_cmd_idx = 0;
    cmd.call_src_cmd_idx = 10;
    cmd.num_args = 4;
    memset(&program, 0, sizeof(program));
    program.cmds = &cmd;
    program.cmd_count = 1;

    repl_gl_state_report_at_line(program, 1, &report);
    row = gl_state_test_find_row(&report, "GL_COLOR_CLEAR_VALUE");
    ASSERT_TRUE("expanded command uses outer call before checkpoint", row != NULL);
    if (row) {
        ASSERT_INT("expanded command records display source",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("expanded command records lexical source line",
                   row->source.source_line_idx, 20);
    }

    cmd.root_call_src_cmd_idx = 3;
    repl_gl_state_report_at_line(program, 1, &report);
    row = gl_state_test_find_row(&report, "GL_COLOR_CLEAR_VALUE");
    ASSERT_TRUE("init clear color remains when expanded command is excluded",
                row != NULL);
    if (row)
        ASSERT_INT("excluded expanded command leaves init as latest source",
                   row->source.kind, REPL_GL_STATE_SOURCE_INIT);
}

static void test_gl_state_report_includes_generated_fixed_function_state(void) {
    static const ReplExportLightBridge light_bridge = {
        gl_state_test_fill_light
    };
    static const ReplExportCameraBridge camera_bridge = {
        .fill_display_block = gl_state_test_fill_camera
    };
    const ReplExportLightBridge *saved_light_bridge =
        repl_export_light_bridge();
    const ReplExportCameraBridge *saved_camera_bridge =
        repl_export_camera_bridge();
    FlatProgramView program;
    ReplGlStateReport report;
    const ReplGlStateReportRow *row;

    printf("--- repl_state generated fixed-function state ---\n");
    repl_export_install_light_bridge(&light_bridge);
    repl_export_install_camera_bridge(&camera_bridge);
    memset(&program, 0, sizeof(program));
    repl_gl_state_report_at_line(program, 0, &report);

    row = gl_state_test_find_row(&report, "GL_LINE_WIDTH");
    ASSERT_TRUE("generated init line width is reported", row != NULL);
    if (row) {
        ASSERT_STR("generated init line width current", row->current, "1.5");
        ASSERT_STR("generated init line width default", row->default_value, "1");
        ASSERT_INT("line width source is init", row->source.kind,
                   REPL_GL_STATE_SOURCE_INIT);
    }

    row = gl_state_test_find_row(&report, "GL_LIGHT_MODEL_AMBIENT");
    ASSERT_TRUE("generated global light ambient is reported", row != NULL);
    if (row) {
        ASSERT_STR("global light ambient current", row->current,
                   "(0.15, 0.15, 0.2, 1)");
        ASSERT_STR("global light ambient OpenGL default", row->default_value,
                   "(0.2, 0.2, 0.2, 1)");
        ASSERT_INT("global ambient source is init", row->source.kind,
                   REPL_GL_STATE_SOURCE_INIT);
    }

    row = gl_state_test_find_row(&report, "GL_LIGHT0_DIFFUSE");
    ASSERT_TRUE("generated light diffuse is reported", row != NULL);
    if (row) {
        ASSERT_STR("light diffuse current", row->current,
                   "(0.4, 0.5, 0.6, 1)");
        ASSERT_STR("light0 diffuse OpenGL default", row->default_value,
                   "(1, 1, 1, 1)");
        ASSERT_INT("light diffuse source is init", row->source.kind,
                   REPL_GL_STATE_SOURCE_INIT);
    }

    row = gl_state_test_find_row(&report, "GL_LIGHT0_AMBIENT");
    ASSERT_TRUE("generated light ambient is reported", row != NULL);
    if (row)
        ASSERT_STR("light ambient current", row->current,
                   "(0.1, 0.2, 0.3, 1)");

    row = gl_state_test_find_row(&report, "GL_LIGHT0_SPECULAR");
    ASSERT_TRUE("generated light specular is reported", row != NULL);
    if (row)
        ASSERT_STR("light specular current", row->current,
                   "(0.7, 0.8, 0.9, 1)");

    row = gl_state_test_find_row(&report, "GL_MODELVIEW_MATRIX");
    ASSERT_TRUE("generated camera modelview is reported", row != NULL);
    if (row) {
        ASSERT_STR("generated camera modelview current", row->current,
                   "[  1.0000   0.0000   0.0000  10.2500; "
                   "  0.0000   1.0000   0.0000   0.0000; "
                   "  0.0000   0.0000   1.0000   0.0000; "
                   "  0.0000   0.0000   0.0000   1.0000]");
        ASSERT_STR("generated camera modelview aligned default",
                   row->default_value,
                   "[  1.0000   0.0000   0.0000   0.0000; "
                   "  0.0000   1.0000   0.0000   0.0000; "
                   "  0.0000   0.0000   1.0000   0.0000; "
                   "  0.0000   0.0000   0.0000   1.0000]");
        ASSERT_INT("camera modelview source is display", row->source.kind,
                   REPL_GL_STATE_SOURCE_DISPLAY);
        ASSERT_INT("generated display has no user line",
                   row->source.source_line_idx, -1);
    }

    row = gl_state_test_find_row(&report, "GL_LIGHT0_POSITION (eye)");
    ASSERT_TRUE("generated light position is reported", row != NULL);
    if (row) {
        ASSERT_STR("world light is stored after modelview transform",
                   row->current, "(11.25, 2, 3, 1)");
        ASSERT_STR("light position OpenGL default", row->default_value,
                   "(0, 0, 1, 0)");
        ASSERT_INT("light position source is display", row->source.kind,
                   REPL_GL_STATE_SOURCE_DISPLAY);
    }

    row = gl_state_test_find_row(&report, "GL_LIGHT0_POSITION (world)");
    ASSERT_TRUE("generated world light position is reported", row != NULL);
    if (row) {
        ASSERT_STR("world row reverses the camera modelview",
                   row->current, "(1, 2, 3, 1)");
        ASSERT_STR("world row converts the OpenGL default",
                   row->default_value, "(0, 0, 1, 0)");
        ASSERT_INT("world light position source is display",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
    }

    row = gl_state_test_find_row(&report, "GL_ATTRIB_STACK_DEPTH");
    ASSERT_TRUE("generated display attribute push is reported", row != NULL);
    if (row) {
        ASSERT_STR("attribute stack depth current", row->current, "1");
        ASSERT_STR("attribute stack depth default", row->default_value, "0");
        ASSERT_INT("attribute stack source is display", row->source.kind,
                   REPL_GL_STATE_SOURCE_DISPLAY);
    }

    repl_export_install_light_bridge(saved_light_bridge);
    repl_export_install_camera_bridge(saved_camera_bridge);
}

static void test_gl_state_report_converts_eye_light_position_to_world(void) {
    static const ReplExportLightBridge light_bridge = {
        gl_state_test_fill_eye_light
    };
    static const ReplExportCameraBridge camera_bridge = {
        .fill_display_block = gl_state_test_fill_camera
    };
    const ReplExportLightBridge *saved_light_bridge =
        repl_export_light_bridge();
    const ReplExportCameraBridge *saved_camera_bridge =
        repl_export_camera_bridge();
    FlatProgramView program;
    ReplGlStateReport report;
    const ReplGlStateReportRow *row;

    printf("--- repl_state eye light world position ---\n");
    repl_export_install_light_bridge(&light_bridge);
    repl_export_install_camera_bridge(&camera_bridge);
    memset(&program, 0, sizeof(program));
    repl_gl_state_report_at_line(program, 0, &report);

    row = gl_state_test_find_row(&report, "GL_LIGHT0_POSITION (eye)");
    ASSERT_TRUE("eye-space light keeps submitted eye position", row != NULL);
    if (row)
        ASSERT_STR("eye-space light eye value", row->current,
                   "(1, 2, 3, 1)");

    row = gl_state_test_find_row(&report, "GL_LIGHT0_POSITION (world)");
    ASSERT_TRUE("eye-space light gains derived world position", row != NULL);
    if (row) {
        ASSERT_STR("eye-space light follows camera into world",
                   row->current, "(-9.25, 2, 3, 1)");
        ASSERT_INT("derived world position retains display source",
                   row->source.kind, REPL_GL_STATE_SOURCE_DISPLAY);
    }

    repl_export_install_light_bridge(saved_light_bridge);
    repl_export_install_camera_bridge(saved_camera_bridge);
}

int main(void) {
    printf("--- repl_state tests ---\n");
    test_capture_restore_round_trip();
    test_reset_all_restores_default_runtime();
    test_line_override_cap_covers_busy_replay();
    test_camera_restore_clears_momentum();
    test_source_document_load_all_rejects_oversized();
    test_source_document_apply_change_combined_atomic_on_failure();
    test_source_document_apply_change_combined();
    test_source_document_insert_many_rejects_invalid_count();
    test_camera_ease_to_default_uses_scene_default();
    test_camera_clear_scene_default_falls_back();
    test_example_load_sets_scene_camera_default();
    test_user_scene_load_clears_scene_camera_default();
    test_workspace_load_clears_scene_camera_default();
    test_camera_target_decay_override_applies();
    test_camera_target_decay_override_resets_on_new_ease();
    test_time_dirty_gate_routes_by_dep_masks();
    test_gl_state_report_tracks_explicit_writes_before_checkpoint();
    test_gl_state_report_tracks_fog();
    test_gl_state_report_uses_flat_call_provenance();
    test_gl_state_report_includes_generated_fixed_function_state();
    test_gl_state_report_converts_eye_light_position_to_world();
    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
