#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_state.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", label, got, exp, __LINE__); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    declare_predef_var("x", err, sizeof(err));
    declare_predef_var("y", err, sizeof(err));
    declare_predef_var("z", err, sizeof(err));
    declare_predef_var("i", err, sizeof(err));
    declare_predef_var("j", err, sizeof(err));
    declare_predef_var("k", err, sizeof(err));
    declare_predef_var("n", err, sizeof(err));
}

int main() {
    init_predef_vars();
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
        
        ASSERT_INT("find_block_end(0)", find_block_end(0), 2);
        ASSERT_INT("block_depth_at(1)", block_depth_at(1), 1);
        ASSERT_INT("block_depth_at(2)", block_depth_at(2), 1); /* Still 1 at the closing brace */
        ASSERT_INT("nearest_open_block_at(1)", nearest_open_block_at(1), CMD_FOR_BEGIN);
    }

    /* 7. collect_visible_vars */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        repl_feed_line_public("  for(j, 0, 1) {");
        
        ExprVar vars[8];
        int n = collect_visible_vars(2, vars, 8);
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
            .source_cmds = g_cmds,
            .source_cmd_count = g_num_cmds,
            .flat_cmds = temp_flat,
            .flat_local_vars = temp_locals,
            .flat_capacity = 8
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
                   g_cmds[temp_flat[1].src_cmd_idx].type, CMD_VERTEX3F);
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
        ASSERT_INT("command_store_load pre-cache depth", block_depth_at(1), 1);

        memset(loaded, 0, sizeof(loaded));
        loaded[0].type = CMD_VERTEX3F;
        loaded[0].valid = 1;
        snprintf(loaded[0].source, sizeof(loaded[0].source),
                 "glVertex3f(1, 0, 0);");
        loaded[1].type = CMD_COLOR3F;
        loaded[1].valid = 1;
        snprintf(loaded[1].source, sizeof(loaded[1].source),
                 "glColor3f(1, 0, 0);");

        store = repl_command_store_live();
        ASSERT_TRUE("command_store uses document cmds",
                    store.cmds == repl_state_document_cmds_mut());
        ASSERT_TRUE("command_store uses document count",
                    store.count == repl_state_document_mut()->cmd_count);
        repl_state_normals_dirty_clear();
        ASSERT_INT("document normals dirty clear",
                   repl_state_normals_dirty(), 0);
        ASSERT_INT("command_store_load ok",
                   repl_command_store_load(&store, loaded, 2, 99), 1);
        ASSERT_INT("command_store_load count", g_num_cmds, 2);
        ASSERT_INT("command_store_load state count",
                   repl_state_document_count(), 2);
        ASSERT_INT("command_store_load edit clamp", g_edit_line, 2);
        ASSERT_INT("command_store_load state edit clamp",
                   repl_state_edit_line(), 2);
        ASSERT_STR("command_store_load source", g_cmds[1].source,
                   "glColor3f(1, 0, 0);");
        ASSERT_STR("command_store_load state source",
                   repl_state_document_cmd_at(1)->source,
                   "glColor3f(1, 0, 0);");
        ASSERT_INT("command_store_load marks normals dirty",
                   repl_state_normals_dirty(), 1);
        ASSERT_INT("command_store_load invalidates depth",
                   block_depth_at(1), 0);

        ASSERT_INT("command_store_load rejects missing cmds",
                   repl_command_store_load(&store, NULL, 1, 0), 0);
        ASSERT_INT("command_store_load reject keeps count", g_num_cmds, 2);
        ASSERT_INT("command_store_load rejects overflow",
                   repl_command_store_load(&store, loaded,
                                           MAX_COMMANDS + 1, 0), 0);
        ASSERT_INT("command_store_load overflow keeps count", g_num_cmds, 2);
        ASSERT_INT("command_store_load empty ok",
                   repl_command_store_load(&store, NULL, 0, -5), 1);
        ASSERT_INT("command_store_load empty count", g_num_cmds, 0);
        ASSERT_INT("command_store_load empty edit clamp", g_edit_line, 0);
    }

    /* 11. editor input/selection/clipboard state facade */
    {
        ReplEditorInputState *input;
        ReplEditorState editor;
        GLCmd *clipboard;

        repl_reset_state(); declare_test_vars();

        input = repl_state_editor_input_mut();
        ASSERT_TRUE("editor input facade uses input buffer",
                    input->input == g_input);
        ASSERT_TRUE("editor input facade uses input len",
                    input->input_len == &g_input_len);
        ASSERT_TRUE("editor input facade uses cursor",
                    input->cursor_pos == &g_cursor_pos);

        repl_state_input_set_text("abc");
        ASSERT_STR("state input set text", g_input, "abc");
        ASSERT_INT("state input set len", g_input_len, 3);
        ASSERT_INT("state input cursor at end", g_cursor_pos, 3);
        repl_state_cursor_pos_set(99);
        ASSERT_INT("state cursor clamps high", g_cursor_pos, 3);
        repl_state_cursor_pos_set(-5);
        ASSERT_INT("state cursor clamps low", g_cursor_pos, 0);

        repl_state_pending_newline_set_text("next line");
        ASSERT_STR("state newline set text", g_newline_buf, "next line");
        ASSERT_INT("state newline len", g_newline_len, 9);
        repl_state_insert_mode_set(42);
        ASSERT_INT("state insert mode set", g_inserting, 1);

        repl_state_selection_set(4, 2);
        ASSERT_INT("state selection anchor", repl_state_selection_anchor(), 4);
        ASSERT_INT("state selection end", repl_state_selection_end_idx(), 2);
        repl_state_selection_clear();
        ASSERT_INT("state selection clear anchor", repl_state_selection_anchor(), -1);
        ASSERT_INT("state selection clear end", repl_state_selection_end_idx(), -1);

        clipboard = repl_state_clipboard_cmds_mut();
        clipboard[0].type = CMD_COLOR3F;
        clipboard[0].valid = 1;
        repl_state_clipboard_count_set(1);
        ASSERT_INT("state clipboard count", repl_state_clipboard_count(), 1);
        ASSERT_INT("state clipboard count accessor",
                   repl_state_clipboard_count(), 1);
        repl_state_clipboard_clear();
        ASSERT_INT("state clipboard clear", repl_state_clipboard_count(), 0);

        editor = repl_editor_state_live();
        ASSERT_TRUE("editor live bundle uses state input",
                    editor.input == repl_state_input_buffer_mut());
        ASSERT_TRUE("editor live bundle uses state selection",
                    editor.sel_anchor == repl_state_selection_mut()->anchor_idx);
        ASSERT_TRUE("editor live bundle uses state clipboard",
                    editor.clipboard == repl_state_clipboard_cmds_mut());

        repl_state_editor_input_reset();
        ASSERT_STR("state input reset text", g_input, "");
        ASSERT_INT("state input reset inserting", g_inserting, 0);
        ASSERT_STR("state newline reset text", g_newline_buf, "");
    }

    /* 12. camera/pointer/viewport state facade */
    {
        ReplCameraState *camera;
        ReplPointerState *pointer;
        ReplViewportState *viewport;
        ReplViewState view;

        repl_state_camera_set(11.0f, -22.0f, 7.5f,
                              1.0f, 2.0f, 3.0f, 0.25f);
        ASSERT_TRUE("state camera rx", g_cam_rx == 11.0f);
        ASSERT_TRUE("state camera ry", g_cam_ry == -22.0f);
        ASSERT_TRUE("state camera dist", g_cam_dist == 7.5f);
        ASSERT_TRUE("state camera tx", g_cam_tx == 1.0f);
        ASSERT_TRUE("state camera ty", g_cam_ty == 2.0f);
        ASSERT_TRUE("state camera tz", g_cam_tz == 3.0f);
        ASSERT_TRUE("state camera glow", g_cam_motion_glow == 0.25f);

        repl_state_camera_set_orbit(33.0f, 44.0f);
        ASSERT_TRUE("state camera orbit rx", g_cam_rx == 33.0f);
        ASSERT_TRUE("state camera orbit ry", g_cam_ry == 44.0f);
        repl_state_camera_set_pan(-1.0f, -2.0f, -3.0f);
        ASSERT_TRUE("state camera pan tx", g_cam_tx == -1.0f);
        ASSERT_TRUE("state camera pan ty", g_cam_ty == -2.0f);
        ASSERT_TRUE("state camera pan tz", g_cam_tz == -3.0f);
        repl_state_camera_set_distance(9.0f);
        ASSERT_TRUE("state camera distance", g_cam_dist == 9.0f);
        repl_state_camera_set_motion_glow(0.5f);
        ASSERT_TRUE("state camera motion glow", g_cam_motion_glow == 0.5f);

        camera = repl_state_camera_mut();
        ASSERT_TRUE("state camera facade rx", camera->rx == &g_cam_rx);
        ASSERT_TRUE("state camera facade dist", camera->dist == &g_cam_dist);

        repl_state_pointer_set(10, 20, 3);
        ASSERT_INT("state pointer x", g_mouse_x, 10);
        ASSERT_INT("state pointer y", g_mouse_y, 20);
        ASSERT_INT("state pointer button", g_mouse_btn, 3);
        repl_state_pointer_set_pos(30, 40);
        ASSERT_INT("state pointer pos x", g_mouse_x, 30);
        ASSERT_INT("state pointer pos y", g_mouse_y, 40);
        repl_state_pointer_set_button(-1);
        ASSERT_INT("state pointer button set", g_mouse_btn, -1);

        pointer = repl_state_pointer_mut();
        ASSERT_TRUE("state pointer facade x", pointer->mouse_x == &g_mouse_x);
        ASSERT_TRUE("state pointer facade button",
                    pointer->mouse_button == &g_mouse_btn);

        repl_state_viewport_set_size(1024, 768);
        ASSERT_INT("state viewport width", g_win_w, 1024);
        ASSERT_INT("state viewport height", g_win_h, 768);

        viewport = repl_state_viewport_mut();
        ASSERT_TRUE("state viewport facade width",
                    viewport->window_w == &g_win_w);
        ASSERT_TRUE("state viewport facade height",
                    viewport->window_h == &g_win_h);

        view = repl_view_state_live();
        ASSERT_TRUE("view live bundle camera", view.cam_rx == camera->rx);
        ASSERT_TRUE("view live bundle pointer",
                    view.mouse_btn == pointer->mouse_button);
        ASSERT_TRUE("view live bundle viewport",
                    view.win_w == viewport->window_w);

        repl_state_camera_reset_default();
        ASSERT_TRUE("state camera reset rx", g_cam_rx == 20.0f);
        ASSERT_TRUE("state camera reset ry", g_cam_ry == 30.0f);
        ASSERT_TRUE("state camera reset dist", g_cam_dist == 5.0f);
        ASSERT_TRUE("state camera reset glow", g_cam_motion_glow == 0.0f);
    }

    /* 13. presentation state facade */
    {
        ReplPresentationState *presentation;

        presentation = repl_state_presentation_mut();
        ASSERT_TRUE("presentation facade wireframe",
                    presentation->wireframe == &g_wireframe);
        ASSERT_TRUE("presentation facade grid",
                    presentation->grid_theme == &g_grid_theme);
        ASSERT_TRUE("presentation facade focus vertex",
                    presentation->focus_vertex == g_focus_vtx);
        ASSERT_TRUE("presentation facade focus valid",
                    presentation->focus_vertex_valid == &g_focus_vtx_valid);

        g_wireframe = 1;
        g_grid_theme = GRID_THEME_TRON;
        g_grid_major_idx = GRID_MAJOR_10;
        g_grid_extent_idx = GRID_EXTENT_CLOSE;
        g_axes_theme = AXES_THEME_NEON;
        g_show_vnums = 1;
        g_show_normals = 1;
        g_show_indices = 1;
        g_show_outlines = 1;
        g_show_vpoints = 1;
        g_show_guides = 1;
        g_xform_guide_mode = 1;
        g_autonormal = 1;
        g_show_lights = 0;
        g_backdrop_mode = 0;
        g_cam_rotate = 1;
        g_highlight_current_poly = 0;
        g_ortho_mode = 1;
        g_wrap_at_comma = 0;
        g_code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;

        repl_state_presentation_reset_defaults();
        ASSERT_INT("presentation reset wireframe",
                   g_wireframe, CFG_DEFAULT_WIREFRAME);
        ASSERT_INT("presentation reset grid",
                   g_grid_theme, CFG_DEFAULT_GRID_THEME);
        ASSERT_INT("presentation reset grid major",
                   g_grid_major_idx, CFG_DEFAULT_GRID_MAJOR_IDX);
        ASSERT_INT("presentation reset grid extent",
                   g_grid_extent_idx, CFG_DEFAULT_GRID_EXTENT_IDX);
        ASSERT_INT("presentation reset axes",
                   g_axes_theme, CFG_DEFAULT_AXES_THEME);
        ASSERT_INT("presentation reset labels",
                   g_show_vnums, CFG_DEFAULT_VERTEX_LABELS);
        ASSERT_INT("presentation reset normals",
                   g_show_normals, CFG_DEFAULT_NORMAL_VECTORS);
        ASSERT_INT("presentation reset indices",
                   g_show_indices, CFG_DEFAULT_VERTEX_INDICES);
        ASSERT_INT("presentation reset outlines",
                   g_show_outlines, CFG_DEFAULT_VERTEX_OUTLINES);
        ASSERT_INT("presentation reset points",
                   g_show_vpoints, CFG_DEFAULT_VERTEX_POINTS);
        ASSERT_INT("presentation reset guides",
                   g_show_guides, CFG_DEFAULT_VERTEX_GUIDES);
        ASSERT_INT("presentation reset xform guide",
                   g_xform_guide_mode, CFG_DEFAULT_XFORM_GUIDE_MODE);
        ASSERT_INT("presentation reset lights",
                   g_show_lights, CFG_DEFAULT_LIGHT_INDICATORS);
        ASSERT_INT("presentation reset backdrop",
                   g_backdrop_mode, CFG_DEFAULT_BACKDROP_MODE);
        ASSERT_INT("presentation reset camera rotate",
                   g_cam_rotate, CFG_DEFAULT_CAMERA_ROTATE);
        ASSERT_INT("presentation reset highlight", g_highlight_current_poly, 1);
        ASSERT_INT("presentation reset ortho", g_ortho_mode, 0);
        ASSERT_INT("presentation reset wrap",
                   g_wrap_at_comma, CFG_DEFAULT_WRAP_AT_COMMA);
        ASSERT_INT("presentation reset layout",
                   g_code_panel_layout, CFG_DEFAULT_CODE_PANEL_LAYOUT);
    }

    /* 14. render state facade */
    {
        ReplRenderState *render;
        ReplRenderDerivedState *derived;

        render = repl_state_render_mut();
        ASSERT_TRUE("render facade use accum",
                    render->use_accum == &g_use_accum);
        ASSERT_TRUE("render facade accum aa",
                    render->accum_aa_enabled == &g_accum_aa_enabled);
        ASSERT_TRUE("render facade accum samples",
                    render->accum_samples == &g_accum_samples);
        ASSERT_TRUE("render facade accum jitter x",
                    render->accum_jitter_x == &g_accum_jitter_x);
        ASSERT_TRUE("render facade accum jitter y",
                    render->accum_jitter_y == &g_accum_jitter_y);
        ASSERT_TRUE("render facade multisample",
                    render->multisample_enabled == &g_multisample_enabled);
        ASSERT_TRUE("render facade line smooth",
                    render->line_smooth_enabled == &g_line_smooth_enabled);
        ASSERT_TRUE("render facade point attenuation",
                    render->point_attenuation_enabled == &g_init_attenuate_points);
        ASSERT_TRUE("render facade quadric",
                    render->quadric == &g_quadric);
        ASSERT_TRUE("render facade tess", render->tess == &g_tess);
        ASSERT_TRUE("render facade tess verts",
                    render->tess_verts == g_tess_verts);
        ASSERT_TRUE("render facade tess vert count",
                    render->tess_vert_count == &g_tess_vert_count);
        ASSERT_TRUE("render facade lights", render->lights == g_lights);
        ASSERT_TRUE("render facade clear color",
                    render->clear_color == g_clear_color);

        derived = repl_state_render_derived_mut();
        ASSERT_TRUE("render derived facade vertex",
                    derived->focus_vertex == g_focus_vtx);
        ASSERT_TRUE("render derived facade valid",
                    derived->focus_vertex_valid == &g_focus_vtx_valid);

        g_multisample_enabled = 0;
        g_line_smooth_enabled = 0;
        g_init_attenuate_points = 0;
        g_clear_color[0] = 0.0f;
        g_clear_color[1] = 0.0f;
        g_clear_color[2] = 0.0f;
        g_clear_color[3] = 0.0f;
        g_quadric = NULL;
        g_tess = NULL;
        g_tess_vert_count = 7;

        repl_state_render_reset_defaults();
        ASSERT_INT("render reset multisample",
                   g_multisample_enabled, CFG_DEFAULT_MULTISAMPLE);
        ASSERT_INT("render reset line smooth",
                   g_line_smooth_enabled, CFG_DEFAULT_LINE_SMOOTH);
        ASSERT_INT("render reset point attenuation",
                   g_init_attenuate_points, CFG_DEFAULT_ATTENUATE_POINTS);
        ASSERT_TRUE("render reset clear color r", g_clear_color[0] == 0.10f);
        ASSERT_TRUE("render reset clear color g", g_clear_color[1] == 0.10f);
        ASSERT_TRUE("render reset clear color b", g_clear_color[2] == 0.13f);
        ASSERT_TRUE("render reset clear color a", g_clear_color[3] == 1.0f);

        repl_state_render_init_resources();
        ASSERT_TRUE("render init quadric live", *render->quadric != NULL);
        ASSERT_TRUE("render init tess live", *render->tess != NULL);
        ASSERT_INT("render init tess vert count", g_tess_vert_count, 0);

        repl_state_render_destroy_resources();
        ASSERT_TRUE("render destroy quadric null", g_quadric == NULL);
        ASSERT_TRUE("render destroy tess null", g_tess == NULL);
        ASSERT_INT("render destroy tess vert count", g_tess_vert_count, 0);
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
