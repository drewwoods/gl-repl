#include "glr_camera.h"
#include "repl_core_internal.h"
#include "glr_camera.h"
#include "repl_command_store.h"
#include "glr_camera.h"
#include "repl_executor.h"
#include "glr_camera.h"
#include "repl_source_scope.h"
#include "glr_camera.h"
#include "repl_state.h"
#include "glr_camera.h"
#include "repl_core.h"
#include "glr_camera.h"
#include "ui/state.h"
#include "glr_camera.h"
#include "ui/layout.h"           /* CODE_PANEL_LAYOUT_* */
#include "glr_camera.h"
#include "repl_presentation.h"   /* CFG_DEFAULT_* */
#include "glr_camera.h"
#include "support/test_harness.h"
#include "glr_camera.h"
#include "scene/render.h"

#ifdef OPENGL_VIBE_USE_GL_STUBS
#include "glr_camera.h"
#include <GL/gl_stub_counts.h>
#endif

#define g_use_accum            (repl_state_render_mut()->use_accum)
#define g_accum_aa_enabled     (repl_state_render_mut()->accum_aa_enabled)
#define g_accum_samples        (repl_state_render_mut()->accum_samples)
#define g_accum_jitter_x       (repl_state_render_mut()->accum_jitter_x)
#define g_accum_jitter_y       (repl_state_render_mut()->accum_jitter_y)
#define g_multisample_enabled  (repl_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled  (repl_state_render_mut()->line_smooth_enabled)
#define g_init_attenuate_points (repl_state_render_mut()->point_attenuation_enabled)
#define g_lights               (repl_state_render_mut()->lights)
#define g_clear_color          (repl_state_render_mut()->clear_color)

#include "glr_camera.h"
#include <stdio.h>
#include "glr_camera.h"
#include <string.h>

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

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

int main() {
    repl_eval_init_predef_vars();
    printf("--- repl_core internal tests ---\n");

    /* 1. trim_in_place */
    {
        char s1[] = "  hello  ";
        trim_in_place(s1);
        ASSERT_STR("trim normal", s1, "hello");

        char s2[] = "   ";
        trim_in_place(s2);
        ASSERT_STR("trim empty", s2, "");

        char s3[] = "a";
        trim_in_place(s3);
        ASSERT_STR("trim single", s3, "a");
    }

    /* 2. extract_for_args_text */
    {
        char var[16], args[64];
        int r = extract_for_args_text("for(i, 0, 10)", var, sizeof(var), args, sizeof(args));
        ASSERT_INT("extract_for_args ok", r, 1);
        ASSERT_STR("extract_for_args var", var, "i");
        ASSERT_STR("extract_for_args args", args, "0, 10");

        r = extract_for_args_text("for ( j , 1 , 5 , 0.5 )", var, sizeof(var), args, sizeof(args));
        ASSERT_INT("extract_for_args spaced ok", r, 1);
        ASSERT_STR("extract_for_args spaced var", var, "j");
        ASSERT_STR("extract_for_args spaced args", args, "1 , 5 , 0.5");

        ASSERT_INT("extract_for_args fail", extract_for_args_text("not a for", var, 16, args, 64), 0);
    }

    /* 3. parse_expr_list_exact */
    {
        float vals[4];
        int count = 0;
        int r = parse_expr_list_exact("1, 2, 3", vals, 4, NULL, 0, &count);
        ASSERT_INT("parse_expr_list ok", r, 1);
        ASSERT_INT("parse_expr_list count", count, 3);
        ASSERT_TRUE("val[0]", vals[0] == 1.0f);
        ASSERT_TRUE("val[1]", vals[1] == 2.0f);
        ASSERT_TRUE("val[2]", vals[2] == 3.0f);

        r = parse_expr_list_exact("1, 2", vals, 1, NULL, 0, &count);
        ASSERT_INT("parse_expr_list overflow", r, 0);
    }

    /* 4. Func signatures */
    {
        int fn = -1;
        char params[4][16];
        int count = 0;
        int r = parse_repl_func_signature("func0(r, g, b) {", &fn, params, 4, &count);
        ASSERT_INT("parse_signature ok", r, 1);
        ASSERT_INT("parse_signature fn", fn, 0);
        ASSERT_INT("parse_signature count", count, 3);
        ASSERT_STR("param0", params[0], "r");
        ASSERT_STR("param1", params[1], "g");
        ASSERT_STR("param2", params[2], "b");

        r = parse_repl_func_signature("func1 {", &fn, params, 4, &count);
        ASSERT_INT("parse_signature no params", r, 1);
        ASSERT_INT("parse_signature no params fn", fn, 1);
        ASSERT_INT("parse_signature no params count", count, 0);
    }

    /* 5. Func call args */
    {
        int fn = -1;
        char args[64];
        int r = extract_func_call_args_text("func2(1, x+y)", &fn, args, sizeof(args));
        ASSERT_INT("extract_call ok", r, 1);
        ASSERT_INT("extract_call fn", fn, 2);
        ASSERT_STR("extract_call args", args, "1, x+y");
    }

    /* 6. Block helpers */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");     /* 0 */
        repl_feed_line_public("  glVertex3f(0,0,0);"); /* 1 */
        repl_feed_line_public("}");                    /* 2 */

        ASSERT_INT("repl_source_scope_find_block_end(0)", repl_source_scope_find_block_end(0), 2);
        ASSERT_INT("repl_source_scope_block_depth_at(1)", repl_source_scope_block_depth_at(1), 1);
        ASSERT_INT("repl_source_scope_block_depth_at(2)", repl_source_scope_block_depth_at(2), 1); /* Still 1 at the closing brace */
        ASSERT_INT("repl_source_scope_nearest_open_block_at(1)", repl_source_scope_nearest_open_block_at(1), CMD_FOR_BEGIN);
    }

    /* 7. collect_visible_vars */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        repl_feed_line_public("  for(j, 0, 1) {");

        ExprVar vars[8];
        int n = collect_visible_vars(2, vars, 8, NULL);
        ASSERT_INT("collect_visible_vars count", n, 2);
        /* Note: variables are collected inner-to-outer */
        ASSERT_STR("var0 name", vars[0].name, "j");
        ASSERT_STR("var1 name", vars[1].name, "i");
    }

    /* 8. explicit flatten destination */
    {
        GLCmd temp_flat[8];
        FlatCmdLocalVars temp_locals[8];
        ReplFlattenOptions opts;
        ReplFlattenResult result;
        int live_count;
        GLCmd live_first;
        FlatProgramView live_view;

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glEnable(GL_LIGHTING);");
        repl_feed_line_public("func0(r) {");
        repl_feed_line_public("  glVertex3f(r, 0, 0);");
        repl_feed_line_public("}");
        repl_feed_line_public("func0(2);");
        repl_flatten_commands();
        live_count = repl_state_flat_program_count();
        live_first = repl_state_flat_program_cmds()[0];

        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts = (ReplFlattenOptions){
            .source_cmds = repl_state_document_cmds_mut(),
            .source_cmd_count = repl_state_document_count(),
            .flat_cmds = temp_flat,
            .flat_local_vars = temp_locals,
            .flat_capacity = 8,
            .text = editor_buffer_view()
        };
        ASSERT_INT("flatten_program ok",
                   repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("flatten_program result ok", result.ok, 1);
        ASSERT_INT("flatten_program count", result.flat_cmd_count, 2);
        ASSERT_INT("flatten_program lighting result",
                   result.user_lighting_enabled, 1);
        ASSERT_INT("flatten_program first type", temp_flat[0].type, CMD_ENABLE);
        ASSERT_INT("flatten_program second type", temp_flat[1].type, CMD_VERTEX3F);
        ASSERT_TRUE("flatten_program arg eval",
                    fabsf(temp_flat[1].args[0] - 2.0f) < 1e-6f);
        ASSERT_INT("flatten_program provenance source type",
                   repl_state_document_cmds_mut()[temp_flat[1].src_cmd_idx].type, CMD_VERTEX3F);
        ASSERT_INT("flatten_program live count unchanged",
                   repl_state_flat_program_count(), live_count);
        ASSERT_INT("flatten_program live first unchanged",
                   repl_state_flat_program_cmds()[0].type, live_first.type);
        live_view = repl_state_flat_program_view();
        ASSERT_TRUE("flatten_program live view uses flat cmds",
                    live_view.cmds == repl_state_flat_program_cmds());
        ASSERT_INT("flatten_program live view count",
                   live_view.cmd_count, live_count);

        opts.flat_capacity = 1;
        ASSERT_INT("flatten_program capacity fail",
                   repl_flatten_program(&opts, &result), 0);
        ASSERT_INT("flatten_program capacity count", result.flat_cmd_count, 0);
        ASSERT_TRUE("flatten_program capacity status",
                    strstr(result.status, "limit") != NULL);
        ASSERT_INT("flatten_program fail leaves live count",
                   repl_state_flat_program_count(), live_count);
    }

    /* 9. input_has_expr_vars */
    {
        ExprVar vars[2] = { { "radius", 1.0f }, { "height", 2.0f } };
        ASSERT_INT("has_expr_vars true", input_has_expr_vars("radius + 1", vars, 2), 1);
        ASSERT_INT("has_expr_vars false", input_has_expr_vars("x + 1", vars, 2), 0);
        ASSERT_INT("has_any_visible true (predef)", input_has_any_visible_vars("x + 1", vars, 2), 1);
    }

    /* 10. command-store bulk load */
    {
        GLCmd loaded[2];
        ReplCommandStore store;

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        repl_feed_line_public("}");
        ASSERT_INT("command_store_load pre-cache depth", repl_source_scope_block_depth_at(1), 1);

        memset(loaded, 0, sizeof(loaded));
        loaded[0].type = CMD_VERTEX3F;
        loaded[0].valid = 1;
        loaded[1].type = CMD_COLOR3F;
        loaded[1].valid = 1;
        const char *loaded_lines[2] = { "glVertex3f(1, 0, 0);",
                                        "glColor3f(1, 0, 0);" };

        store = repl_command_store_live();
        ASSERT_TRUE("command_store uses document cmds",
                    store.cmds == repl_state_document_cmds_mut());
        ASSERT_TRUE("command_store uses document count",
                    store.count == &repl_state_document_mut()->cmd_count);
        repl_state_normals_dirty_clear();
        ASSERT_INT("document normals dirty clear",
                   repl_state_normals_dirty(), 0);
        ASSERT_INT("command_store_load ok",
                   repl_command_store_load(&store, loaded, 2, 99), 1);
        editor_buffer_load_lines(loaded_lines, 2);
        ASSERT_INT("command_store_load count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load state count",
                   repl_state_document_count(), 2);
        ASSERT_INT("command_store_load edit clamp", repl_state_edit_line(), 2);
        ASSERT_INT("command_store_load state edit clamp",
                   repl_state_edit_line(), 2);
        ASSERT_STR("command_store_load source", editor_buffer_line(1),
                   "glColor3f(1, 0, 0);");
        ASSERT_STR("command_store_load state source",
                   editor_buffer_line(1),
                   "glColor3f(1, 0, 0);");
        ASSERT_INT("command_store_load marks normals dirty",
                   repl_state_normals_dirty(), 1);
        ASSERT_INT("command_store_load invalidates depth",
                   repl_source_scope_block_depth_at(1), 0);

        ASSERT_INT("command_store_load rejects missing cmds",
                   repl_command_store_load(&store, NULL, 1, 0), 0);
        ASSERT_INT("command_store_load reject keeps count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load rejects overflow",
                   repl_command_store_load(&store, loaded,
                                           MAX_COMMANDS + 1, 0), 0);
        ASSERT_INT("command_store_load overflow keeps count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load empty ok",
                   repl_command_store_load(&store, NULL, 0, -5), 1);
        ASSERT_INT("command_store_load empty count", repl_state_document_count(), 0);
        ASSERT_INT("command_store_load empty edit clamp", repl_state_edit_line(), 0);
    }

    /* 11. editor input/selection/clipboard state facade */
    {
        ReplEditorInputState *input;
        ReplClipboardState *clipboard;

        repl_reset_state(); declare_test_vars();

        input = editor_state_input_mut();
        ASSERT_TRUE("editor input facade uses input buffer",
                    input->input == editor_input_buffer_mut());
        editor_input_set_text("xyz");
        ASSERT_TRUE("editor input len ptr reflects state", input->input_len == 3);
        ASSERT_TRUE("editor input cursor ptr reflects state",
                    input->cursor_pos == editor_cursor_pos());

        editor_input_set_text("abc");
        ASSERT_STR("state input set text", editor_state_input().input, "abc");
        ASSERT_INT("state input set len", editor_input_len(), 3);
        ASSERT_INT("state input cursor at end", editor_cursor_pos(), 3);
        editor_cursor_pos_set(99);
        ASSERT_INT("state cursor clamps high", editor_cursor_pos(), 3);
        editor_cursor_pos_set(-5);
        ASSERT_INT("state cursor clamps low", editor_cursor_pos(), 0);

        editor_pending_newline_set_text("next line");
        ASSERT_STR("state newline set text", editor_pending_newline_buffer_mut(), "next line");
        ASSERT_INT("state newline len", editor_pending_newline_len(), 9);
        editor_insert_mode_set(42);
        ASSERT_INT("state insert mode set", editor_insert_mode(), 1);

        editor_state_selection_set(4, 2);
        ASSERT_INT("state selection anchor", editor_state_selection_anchor(), 4);
        ASSERT_INT("state selection end", editor_state_selection_end_idx(), 2);
        editor_state_selection_clear();
        ASSERT_INT("state selection clear anchor", editor_state_selection_anchor(), -1);
        ASSERT_INT("state selection clear end", editor_state_selection_end_idx(), -1);

        clipboard = editor_state_clipboard_mut();
        snprintf(clipboard->lines[0], MAX_LINE_LEN, "glColor3f(1, 0, 0);");
        editor_state_clipboard_count_set(1);
        ASSERT_INT("state clipboard count", editor_state_clipboard_count(), 1);
        ASSERT_INT("state clipboard count accessor",
                   editor_state_clipboard_count(), 1);
        editor_state_clipboard_clear();
        ASSERT_INT("state clipboard clear", editor_state_clipboard_count(), 0);

        editor_state_input_reset();
        ASSERT_STR("state input reset text", editor_state_input().input, "");
        ASSERT_INT("state input reset inserting", editor_insert_mode(), 0);
        ASSERT_STR("state newline reset text", editor_pending_newline_buffer_mut(), "");
    }

    /* 12. camera/pointer/viewport state facade */
    {
        ReplCameraState *camera;
        ReplPointerState *pointer;
        ReplViewportState *viewport;

        glr_camera_set(11.0f, -22.0f, 7.5f,
                              1.0f, 2.0f, 3.0f, 0.25f);
        ASSERT_TRUE("state camera rx", glr_camera().rx == 11.0f);
        ASSERT_TRUE("state camera ry", glr_camera().ry == -22.0f);
        ASSERT_TRUE("state camera dist", glr_camera().dist == 7.5f);
        ASSERT_TRUE("state camera tx", glr_camera().tx == 1.0f);
        ASSERT_TRUE("state camera ty", glr_camera().ty == 2.0f);
        ASSERT_TRUE("state camera tz", glr_camera().tz == 3.0f);
        ASSERT_TRUE("state camera glow", glr_camera().motion_glow == 0.25f);

        glr_camera_set_orbit(33.0f, 44.0f);
        ASSERT_TRUE("state camera orbit rx", glr_camera().rx == 33.0f);
        ASSERT_TRUE("state camera orbit ry", glr_camera().ry == 44.0f);
        glr_camera_set_pan(-1.0f, -2.0f, -3.0f);
        ASSERT_TRUE("state camera pan tx", glr_camera().tx == -1.0f);
        ASSERT_TRUE("state camera pan ty", glr_camera().ty == -2.0f);
        ASSERT_TRUE("state camera pan tz", glr_camera().tz == -3.0f);
        glr_camera_set_distance(9.0f);
        ASSERT_TRUE("state camera distance", glr_camera().dist == 9.0f);
        glr_camera_set_motion_glow(0.5f);
        ASSERT_TRUE("state camera motion glow", glr_camera().motion_glow == 0.5f);
        glr_camera_mut()->auto_rotate = 1;

        camera = glr_camera_mut();
        ASSERT_TRUE("state camera facade rx", camera->rx == 33.0f);
        ASSERT_TRUE("state camera facade dist", camera->dist == 9.0f);

        ui_state_pointer_set(10, 20, 3);
        ASSERT_INT("state pointer x", ui_state_pointer().mouse_x, 10);
        ASSERT_INT("state pointer y", ui_state_pointer().mouse_y, 20);
        ASSERT_INT("state pointer button", ui_state_pointer().mouse_button, 3);
        ui_state_pointer_set_pos(30, 40);
        ASSERT_INT("state pointer pos x", ui_state_pointer().mouse_x, 30);
        ASSERT_INT("state pointer pos y", ui_state_pointer().mouse_y, 40);
        ui_state_pointer_set_button(-1);
        ASSERT_INT("state pointer button set", ui_state_pointer().mouse_button, -1);

        pointer = ui_state_pointer_mut();
        ASSERT_TRUE("state pointer facade x", pointer->mouse_x == 30);
        ASSERT_TRUE("state pointer facade button", pointer->mouse_button == -1);

        ui_state_viewport_set_size(1024, 768);
        ASSERT_INT("state viewport width", ui_state_viewport().window_w, 1024);
        ASSERT_INT("state viewport height", ui_state_viewport().window_h, 768);

        viewport = ui_state_viewport_mut();
        ASSERT_TRUE("state viewport facade width", viewport->window_w == 1024);
        ASSERT_TRUE("state viewport facade height", viewport->window_h == 768);

        glr_camera_reset_default();
        ASSERT_TRUE("state camera reset rx", glr_camera().rx == 20.0f);
        ASSERT_TRUE("state camera reset ry", glr_camera().ry == 30.0f);
        ASSERT_TRUE("state camera reset dist", glr_camera().dist == 5.0f);
        ASSERT_TRUE("state camera reset tx", glr_camera().tx == 0.0f);
        ASSERT_TRUE("state camera reset ty", glr_camera().ty == 0.0f);
        ASSERT_TRUE("state camera reset tz", glr_camera().tz == 0.0f);
        ASSERT_TRUE("state camera reset glow", glr_camera().motion_glow == 0.0f);
        ASSERT_INT("state camera reset auto rotate",
                   glr_camera().auto_rotate, CFG_DEFAULT_CAMERA_ROTATE);
    }

    /* 13. presentation state facade */
    {
        ReplPresentationState *presentation;

        presentation = repl_state_presentation_mut();
        ASSERT_TRUE("presentation mut is live state",
                    presentation == repl_state_presentation_mut());

        repl_state_presentation_mut()->wireframe = 1;
        repl_state_presentation_mut()->grid_theme = GRID_THEME_TRON;
        repl_state_presentation_mut()->grid_major_idx = GRID_MAJOR_10;
        repl_state_presentation_mut()->grid_extent_idx = GRID_EXTENT_CLOSE;
        repl_state_presentation_mut()->axes_theme = AXES_THEME_NEON;
        repl_state_presentation_mut()->show_vertex_labels = 1;
        repl_state_presentation_mut()->show_normal_vectors = 1;
        repl_state_presentation_mut()->show_vertex_indices = 1; repl_state_sync_ui_chrome();
        repl_state_presentation_mut()->show_vertex_outlines = 1;
        repl_state_presentation_mut()->show_vertex_points = 1;
        repl_state_presentation_mut()->show_vertex_guides = 1;
        repl_state_presentation_mut()->xform_guide_mode = 1;
        repl_state_presentation_mut()->autonormal = 1;
        repl_state_presentation_mut()->show_light_indicators = 0;
        repl_state_presentation_mut()->backdrop_mode = 0;
        glr_camera_mut()->auto_rotate = 1;
        repl_state_presentation_mut()->highlight_current_poly = 0;
        repl_state_presentation_mut()->ortho_mode = 1;
        repl_state_presentation_mut()->wrap_at_comma = 0;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM; repl_state_sync_ui_chrome();
        presentation->focus_vertex[0] = 2.0f;
        presentation->focus_vertex[1] = -1.0f;
        presentation->focus_vertex[2] = 0.5f;
        presentation->focus_vertex_valid = 1;

        repl_state_presentation_reset_defaults();
        ASSERT_INT("presentation reset wireframe",
                   repl_state_presentation().wireframe, CFG_DEFAULT_WIREFRAME);
        ASSERT_INT("presentation reset grid",
                   repl_state_presentation().grid_theme, CFG_DEFAULT_GRID_THEME);
        ASSERT_INT("presentation reset grid major",
                   repl_state_presentation().grid_major_idx, CFG_DEFAULT_GRID_MAJOR_IDX);
        ASSERT_INT("presentation reset grid extent",
                   repl_state_presentation().grid_extent_idx, CFG_DEFAULT_GRID_EXTENT_IDX);
        ASSERT_INT("presentation reset axes",
                   repl_state_presentation().axes_theme, CFG_DEFAULT_AXES_THEME);
        ASSERT_INT("presentation reset labels",
                   repl_state_presentation().show_vertex_labels, CFG_DEFAULT_VERTEX_LABELS);
        ASSERT_INT("presentation reset normals",
                   repl_state_presentation().show_normal_vectors, CFG_DEFAULT_NORMAL_VECTORS);
        ASSERT_INT("presentation reset indices",
                   repl_state_presentation().show_vertex_indices, CFG_DEFAULT_VERTEX_INDICES);
        ASSERT_INT("presentation reset outlines",
                   repl_state_presentation().show_vertex_outlines, CFG_DEFAULT_VERTEX_OUTLINES);
        ASSERT_INT("presentation reset points",
                   repl_state_presentation().show_vertex_points, CFG_DEFAULT_VERTEX_POINTS);
        ASSERT_INT("presentation reset guides",
                   repl_state_presentation().show_vertex_guides, CFG_DEFAULT_VERTEX_GUIDES);
        ASSERT_INT("presentation reset xform guide",
                   repl_state_presentation().xform_guide_mode, CFG_DEFAULT_XFORM_GUIDE_MODE);
        ASSERT_INT("presentation reset lights",
                   repl_state_presentation().show_light_indicators, CFG_DEFAULT_LIGHT_INDICATORS);
        ASSERT_INT("presentation reset backdrop",
                   repl_state_presentation().backdrop_mode, CFG_DEFAULT_BACKDROP_MODE);
        ASSERT_INT("presentation reset camera rotate",
               glr_camera().auto_rotate, CFG_DEFAULT_CAMERA_ROTATE);
        ASSERT_INT("presentation reset highlight", repl_state_presentation().highlight_current_poly, 1);
        ASSERT_INT("presentation reset ortho", repl_state_presentation().ortho_mode, 0);
        ASSERT_INT("presentation reset wrap",
                   repl_state_presentation().wrap_at_comma, CFG_DEFAULT_WRAP_AT_COMMA);
        ASSERT_INT("presentation reset layout",
                   repl_state_presentation().code_panel_layout, CFG_DEFAULT_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("presentation reset focus x",
                    repl_state_presentation().focus_vertex[0] == 0.0f);
        ASSERT_TRUE("presentation reset focus y",
                    repl_state_presentation().focus_vertex[1] == 0.0f);
        ASSERT_TRUE("presentation reset focus z",
                    repl_state_presentation().focus_vertex[2] == 0.0f);
        ASSERT_INT("presentation reset focus valid",
                   repl_state_presentation().focus_vertex_valid, 0);
    }

    /* 14. render state facade */
    {
        ReplRenderState *render;

        render = repl_state_render_mut();
        ASSERT_TRUE("render mut is live state",
                    render == repl_state_render_mut());

        g_use_accum = 0;
        g_accum_aa_enabled = 0;
        g_accum_samples = 8;
        g_accum_jitter_x = 0.5f;
        g_accum_jitter_y = -0.25f;
        g_multisample_enabled = 0;
        g_line_smooth_enabled = 0;
        g_init_attenuate_points = 0;
        g_lights[0].enabled = 0;
        g_clear_color[0] = 0.0f;
        g_clear_color[1] = 0.0f;
        g_clear_color[2] = 0.0f;
        g_clear_color[3] = 0.0f;

        repl_state_render_reset_defaults();
        ASSERT_INT("render reset use accum", g_use_accum, 1);
        ASSERT_INT("render reset accum aa", g_accum_aa_enabled, 1);
        ASSERT_INT("render reset accum samples", g_accum_samples, 2);
        ASSERT_TRUE("render reset jitter x", g_accum_jitter_x == 0.0f);
        ASSERT_TRUE("render reset jitter y", g_accum_jitter_y == 0.0f);
        ASSERT_INT("render reset multisample",
                   g_multisample_enabled, CFG_DEFAULT_MULTISAMPLE);
        ASSERT_INT("render reset line smooth",
                   g_line_smooth_enabled, CFG_DEFAULT_LINE_SMOOTH);
        ASSERT_INT("render reset point attenuation",
                   g_init_attenuate_points, CFG_DEFAULT_ATTENUATE_POINTS);
        ASSERT_INT("render reset light enabled", g_lights[0].enabled, 1);
        ASSERT_TRUE("render reset clear color r", g_clear_color[0] == 0.10f);
        ASSERT_TRUE("render reset clear color g", g_clear_color[1] == 0.10f);
        ASSERT_TRUE("render reset clear color b", g_clear_color[2] == 0.13f);
        ASSERT_TRUE("render reset clear color a", g_clear_color[3] == 1.0f);

    #ifdef OPENGL_VIBE_USE_GL_STUBS
        gl_stub_counts_reset();
    #endif
        repl_executor_init_resources();
    #ifdef OPENGL_VIBE_USE_GL_STUBS
        ASSERT_INT("executor init quadric removed", (int)gl_stub_counts[GL_STUB_gluNewQuadric], 0);
        ASSERT_INT("executor init quadric normals removed", (int)gl_stub_counts[GL_STUB_gluQuadricNormals], 0);
        ASSERT_INT("executor init quadric texture removed", (int)gl_stub_counts[GL_STUB_gluQuadricTexture], 0);
        ASSERT_INT("executor init tess", (int)gl_stub_counts[GL_STUB_gluNewTess], 1);
        ASSERT_INT("executor init tess callbacks", (int)gl_stub_counts[GL_STUB_gluTessCallback], 6);
    #endif

    #ifdef OPENGL_VIBE_USE_GL_STUBS
        gl_stub_counts_reset();
    #endif
        repl_executor_destroy_resources();
    #ifdef OPENGL_VIBE_USE_GL_STUBS
        ASSERT_INT("executor destroy quadric removed", (int)gl_stub_counts[GL_STUB_gluDeleteQuadric], 0);
        ASSERT_INT("executor destroy tess", (int)gl_stub_counts[GL_STUB_gluDeleteTess], 1);
    #endif
    }

    /* 15. MAX_EXPR_VARS truncation warning - verify collect_visible_vars tracks total */
    {
        /* Test A: verify collect_visible_vars returns correct total when not truncated */
        {
            repl_reset_state(); declare_test_vars();
            repl_feed_line_public("for(i, 0, 1) {");
            repl_feed_line_public("  for(j, 0, 1) {");

            ExprVar vars[8];
            int total = 0;
            int count = collect_visible_vars(2, vars, 8, &total);
            ASSERT_INT("2 nested loops: count", count, 2);
            ASSERT_INT("2 nested loops: total", total, 2);
            ASSERT_TRUE("total equals count (not truncated)", total == count);
        }

        /* Test B: verify collect_visible_vars returns total > max_vars when truncated */
        {
            repl_reset_state(); declare_test_vars();
            /* Manually verify the logic: if we can have 32 visible vars,
               and we ask for only 8, we should see total > 8 if we had more scope */
            ExprVar vars[8];
            int total = 0;
            repl_feed_line_public("for(i, 0, 1) {");
            int count1 = collect_visible_vars(1, vars, 8, &total);
            ASSERT_INT("1 loop with max_vars=8: count", count1, 1);
            ASSERT_INT("1 loop with max_vars=8: total", total, 1);
            ASSERT_TRUE("1 loop: total equals count", total == count1);
        }

        /* Test C: verify total is well below MAX_EXPR_VARS for shallow
         * nesting (the truncation guard should never fire here). */
        {
            repl_reset_state();
            repl_feed_line_public("for(i, 0, 1) {");
            repl_feed_line_public("  for(j, 0, 1) {");
            repl_feed_line_public("    for(k, 0, 1) {");
            ExprVar vars[MAX_EXPR_VARS];
            int total = 0;
            int count = collect_visible_vars(3, vars, MAX_EXPR_VARS, &total);
            ASSERT_INT("3 nested loops: total", total, 3);
            ASSERT_INT("3 nested loops: count == total", count, total);
            ASSERT_TRUE("3 nested loops: total <= MAX_EXPR_VARS",
                        total <= MAX_EXPR_VARS);
        }


        /* Test D: verify warning capability works (programmatic truncation check) */
        {
            ExprVar vars[4];  /* Limit to 4 vars */
            int total = 0;

            /* Simulate 5 nested scopes */
            repl_reset_state();
            repl_feed_line_public("for(a, 0, 1) {");
            repl_feed_line_public("  for(b, 0, 1) {");
            repl_feed_line_public("    for(c, 0, 1) {");
            repl_feed_line_public("      for(d, 0, 1) {");
            repl_feed_line_public("        for(e, 0, 1) {");
            /* Query at position 5 with max_vars=4 */
            int count = collect_visible_vars(5, vars, 4, &total);
            ASSERT_INT("5 nested, max_vars=4: count capped", count, 4);
            ASSERT_INT("5 nested, max_vars=4: total uncapped", total, 5);
            ASSERT_TRUE("5 nested: total > max_vars", total > 4);
        }
    }

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
