#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_camera.h"
#include "repl/command.h"
#include "repl/text_helpers.h"
#include "repl/eval.h"
#include "repl/visible_vars.h"
#include "repl/command_store.h"
#include "repl/executor.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include <stdbool.h>
#include "repl/flatten.h"
#include "repl/flatten_query.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "editor/input.h"
#include "ui/app/state.h"
#include "ui/app/layout.h"           /* CODE_PANEL_LAYOUT_* */
#include "app/glr_defaults.h"   /* CFG_DEFAULT_* */
#include "support/test_harness.h"
#include "render3d/render.h"
#include "subsystems/edit_overlays/edit_overlays.h"  /* OverlayWalkCtx, edit_overlays_render_outlines */
#include "source_document.h"        /* source_document_insert_line */
#include "repl/cfg_baseline.h"      /* ReplConfigBag */

#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif

/* Render-config toggles (msaa, line_smooth, accum_*, point_attenuation)
 * moved to glr_state.render in step 7a, and the dimensional lights[] table
 * (positions/colors/eye-space) moved there too in the light-split. The REPL
 * render slice now keeps only the executor-mutated halves: the light-enable
 * bitmask and clear_color[]. */
#define g_use_accum            (glr_state_render_mut()->use_accum)
#define g_accum_effect         (glr_state_render_mut()->accum_effect)
#define g_accum_passes         (glr_state_render_mut()->accum_passes)
#define g_multisample_enabled  (glr_state_render_mut()->multisample_enabled)
#define g_line_smooth_enabled  (glr_state_render_mut()->line_smooth_enabled)
#define g_init_attenuate_points (glr_state_render_mut()->point_attenuation_enabled)
#define g_lights               (glr_state_render_mut()->lights)
#define g_clear_color          (repl_state_render_mut()->clear_color)

#include <stdio.h>
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
        char params[4][REPL_PREDEF_NAME_MAX];
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
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 1) {");     /* 0 */
        editor_feed_line("  glVertex3f(0,0,0);"); /* 1 */
        editor_feed_line("}");                    /* 2 */

        ASSERT_INT("repl_source_scope_find_block_end(0)", repl_source_scope_find_block_end(0), 2);
        ASSERT_INT("repl_source_scope_block_depth_at(1)", repl_source_scope_block_depth_at(1), 1);
        ASSERT_INT("repl_source_scope_block_depth_at(2)", repl_source_scope_block_depth_at(2), 1); /* Still 1 at the closing brace */
        ASSERT_INT("repl_source_scope_nearest_open_block_at(1)", repl_source_scope_nearest_open_block_at(1), CMD_FOR_BEGIN);
    }

    /* 6b. indent helpers with buf_sz == 0 must not write past the buffer.
     * Surround a 1-byte target with sentinels and confirm they survive.
     * Pre-fix the cmd_indent / cmd_tess_indent overloads wrote '\0' to
     * buf[0] (= the byte just past the zero-length buffer). */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_feed_line("  glVertex3f(0,0,0);");
        editor_feed_line("glEnd();");

        char guarded[3] = { '<', 'X', '>' };
        char *buf = &guarded[1];

        repl_source_scope_cmd_indent(1, buf, 0);
        ASSERT_INT("cmd_indent zero: left sentinel",  guarded[0], '<');
        ASSERT_INT("cmd_indent zero: target untouched", guarded[1], 'X');
        ASSERT_INT("cmd_indent zero: right sentinel", guarded[2], '>');

        repl_source_scope_cmd_tess_indent(1, buf, 0);
        ASSERT_INT("cmd_tess_indent zero: left sentinel",  guarded[0], '<');
        ASSERT_INT("cmd_tess_indent zero: target untouched", guarded[1], 'X');
        ASSERT_INT("cmd_tess_indent zero: right sentinel", guarded[2], '>');

        repl_source_scope_begin_indent(1, buf, 0);
        ASSERT_INT("begin_indent zero: left sentinel",  guarded[0], '<');
        ASSERT_INT("begin_indent zero: target untouched", guarded[1], 'X');
        ASSERT_INT("begin_indent zero: right sentinel", guarded[2], '>');

        repl_source_scope_tess_close_indent(1, buf, 0);
        ASSERT_INT("tess_close_indent zero: left sentinel",  guarded[0], '<');
        ASSERT_INT("tess_close_indent zero: target untouched", guarded[1], 'X');
        ASSERT_INT("tess_close_indent zero: right sentinel", guarded[2], '>');

        char small[1] = { 'Y' };
        repl_source_scope_cmd_indent(1, small, 1);
        ASSERT_INT("cmd_indent buf_sz=1 writes only '\\0'", small[0], '\0');
    }

    /* 6c. matrix_close_indent and block_extent helper tests */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glPushMatrix();");      /* 0 */
        editor_feed_line("  glTranslatef(1,2,3);"); /* 1 */
        editor_feed_line("  glPushMatrix();");    /* 2 */
        editor_feed_line("    glScalef(2,2,2);");   /* 3 */
        editor_feed_line("  glPopMatrix();");     /* 4 */
        editor_feed_line("glPopMatrix();");       /* 5 */

        /* Matrix close indent tests */
        char indent_buf[32];
        repl_source_scope_matrix_close_indent(4, indent_buf, sizeof(indent_buf));
        ASSERT_STR("matrix close indent 1", indent_buf, "    ");

        repl_source_scope_matrix_close_indent(5, indent_buf, sizeof(indent_buf));
        ASSERT_STR("matrix close indent 2", indent_buf, "  ");

        /* block_extent tests with if-else-if chains */
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("if (x > 0) {");         /* 0 */
        editor_feed_line("  y = 1;");              /* 1 */
        editor_feed_line("} else if (x < 0) {");   /* 2 */
        editor_feed_line("  y = 2;");              /* 3 */
        editor_feed_line("} else {");              /* 4 */
        editor_feed_line("  y = 3;");              /* 5 */
        editor_feed_line("}");                     /* 6 */

        int start_idx = -1, count = 0;
        int ok = repl_source_scope_block_extent(2, &start_idx, &count);
        ASSERT_TRUE("block extent on else if", ok);
        ASSERT_INT("block extent start on else if", start_idx, 0);
        ASSERT_INT("block extent count on else if", count, 7);

        ok = repl_source_scope_block_extent(4, &start_idx, &count);
        ASSERT_TRUE("block extent on else", ok);
        ASSERT_INT("block extent start on else", start_idx, 0);
        ASSERT_INT("block extent count on else", count, 7);
    }

    /* 7. collect_visible_vars */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 1) {");
        editor_feed_line("  for(j, 0, 1) {");

        ExprVar vars[8];
        int n = collect_visible_vars(2, vars, 8, NULL);
        ASSERT_INT("collect_visible_vars count", n, 2);
        /* Note: variables are collected inner-to-outer */
        ASSERT_STR("var0 name", vars[0].name, "j");
        ASSERT_STR("var1 name", vars[1].name, "i");
    }

    /* 7b. if-block at top level with predef-var condition must
     * re-evaluate at execute time (not blindly enter the body). */
    {
        char err[128];
        glr_ctrl_reset_all();
        repl_eval_declare_predef_var("tmp", err, sizeof(err));
        repl_eval_declare_predef_var("tmp2", err, sizeof(err));
        editor_feed_line("if(tmp == 1) {");
        editor_feed_line("tmp2 = tmp2 + 1;");
        editor_feed_line("}");

        int tmp_idx  = repl_eval_find_predef_var_idx("tmp");
        int tmp2_idx = repl_eval_find_predef_var_idx("tmp2");
        ASSERT_TRUE("tmp declared", tmp_idx >= 0);
        ASSERT_TRUE("tmp2 declared", tmp2_idx >= 0);

        ExprVar *vars_arr = repl_state_variables_mut()->predef_vars;
        vars_arr[tmp_idx].value  = 0.0f;
        vars_arr[tmp2_idx].value = 0.0f;

        repl_flatten_commands(editor_state_edit_line());
        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = repl_state_flat_program_count();
        opts.program = repl_state_flat_program_view();
        opts.text = source_document_view();
        repl_execute_program(&opts);

        ASSERT_TRUE("tmp still 0 after run", vars_arr[tmp_idx].value == 0.0f);
        ASSERT_TRUE("tmp2 still 0 (if-body skipped)",
                    vars_arr[tmp2_idx].value == 0.0f);
    }

    /* 7c. if-block at top level with predef-var condition that IS true:
     * body should run AND subsequent commands in the same frame must see
     * the updated predef var. */
    {
        char err[128];
        glr_ctrl_reset_all();
        repl_eval_declare_predef_var("tmp", err, sizeof(err));
        repl_eval_declare_predef_var("tmp2", err, sizeof(err));
        editor_feed_line("if(tmp > 1) {");
        editor_feed_line("tmp2 = tmp2 + 1;");
        editor_feed_line("}");
        /* CMD_VERTEX3F (tmp2, tmp2, tmp2) reads tmp2 *after* the if-block. */
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glVertex3f(tmp2, tmp2, tmp2);");
        editor_feed_line("glEnd();");

        int tmp_idx  = repl_eval_find_predef_var_idx("tmp");
        int tmp2_idx = repl_eval_find_predef_var_idx("tmp2");
        ExprVar *vars_arr = repl_state_variables_mut()->predef_vars;
        vars_arr[tmp_idx].value  = 2.0f;
        vars_arr[tmp2_idx].value = 0.0f;

        /* Flatten alone is enough for both assertions: it mutates the
         * predef table when it executes the if-body's CMD_VAR_ASSIGN,
         * and it stores the post-if tmp2 value into the downstream
         * CMD_VERTEX3F's args[0]. We deliberately don't call
         * repl_execute_program here — the source contains glBegin/
         * glVertex3f/glEnd, and the live test runs without a GL
         * context (this binary links real libGL, not the stubs). */
        repl_flatten_commands(editor_state_edit_line());

        ASSERT_TRUE("if-true: tmp2 incremented once",
                    vars_arr[tmp2_idx].value == 1.0f);

        /* Find the CMD_VERTEX3F in the flat array and check its args. */
        const GLCmd *fcmds = repl_state_flat_program_cmds();
        int fcount = repl_state_flat_program_count();
        float vx = -9999.0f;
        for (int fi = 0; fi < fcount; fi++) {
            if (fcmds[fi].type == CMD_VERTEX3F) {
                vx = fcmds[fi].args[0];
                break;
            }
        }
        ASSERT_TRUE("vertex3f sees post-if tmp2 (==1)", vx == 1.0f);
    }

    /* 7d. self-referential assignment at top-level: tmp2 = tmp2 + 1
     * should increment by 1 per frame, not 2. Flatten owns the eval
     * and the executor applies the precomputed value; no double-apply. */
    {
        char err[128];
        glr_ctrl_reset_all();
        repl_eval_declare_predef_var("tmp2", err, sizeof(err));
        editor_feed_line("tmp2 = tmp2 + 1;");

        int tmp2_idx = repl_eval_find_predef_var_idx("tmp2");
        ExprVar *vars_arr = repl_state_variables_mut()->predef_vars;
        vars_arr[tmp2_idx].value = 0.0f;

        repl_flatten_commands(editor_state_edit_line());
        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = repl_state_flat_program_count();
        opts.program = repl_state_flat_program_view();
        opts.text = source_document_view();
        repl_execute_program(&opts);

        ASSERT_TRUE("top-level self-ref: 1 increment per frame",
                    vars_arr[tmp2_idx].value == 1.0f);
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

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glEnable(GL_LIGHTING);");
        editor_feed_line("func0(r) {");
        editor_feed_line("  glVertex3f(r, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("func0(2);");
        repl_flatten_commands(editor_state_edit_line());
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
            .text = source_document_view()
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

    /* 8b. user_lighting_enabled respects control flow (regression for #10).
     * Pre-fix walked the source array, so glEnable(GL_LIGHTING) inside an
     * if(0) block or an unreferenced funcN body counted as effective. */
    {
        GLCmd temp_flat[16];
        FlatCmdLocalVars temp_locals[16];
        ReplFlattenOptions opts;
        ReplFlattenResult result;

        /* (a) unreached if(0) body — source sees an enable; flat doesn't. */
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("if(0) {");
        editor_feed_line("  glEnable(GL_LIGHTING);");
        editor_feed_line("}");
        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts = (ReplFlattenOptions){
            .source_cmds = repl_state_document_cmds_mut(),
            .source_cmd_count = repl_state_document_count(),
            .flat_cmds = temp_flat,
            .flat_local_vars = temp_locals,
            .flat_capacity = 16,
            .text = source_document_view()
        };
        ASSERT_INT("if(0) flatten ok", repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("if(0) lighting is OFF (flat walk)",
                   result.user_lighting_enabled, 0);

        /* (b) unreferenced funcN body — source sees the enable; flat
         * skips because the function is never called. */
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("func0() {");
        editor_feed_line("  glEnable(GL_LIGHTING);");
        editor_feed_line("}");
        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts.source_cmds = repl_state_document_cmds_mut();
        opts.source_cmd_count = repl_state_document_count();
        ASSERT_INT("unreferenced func flatten ok",
                   repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("unreferenced func lighting is OFF (flat walk)",
                   result.user_lighting_enabled, 0);

        /* (c) glDisable(GL_LIGHTING) inside an unreached for-loop body
         * — source walk would see the disable and report OFF; flat walk
         * does not enter the empty range and reports ON. */
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glEnable(GL_LIGHTING);");
        editor_feed_line("for(i, 0, 0) {");
        editor_feed_line("  glDisable(GL_LIGHTING);");
        editor_feed_line("}");
        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts.source_cmds = repl_state_document_cmds_mut();
        opts.source_cmd_count = repl_state_document_count();
        ASSERT_INT("dead-disable flatten ok",
                   repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("dead-disable lighting is ON (flat walk)",
                   result.user_lighting_enabled, 1);

        /* (d) enable behind a control-flow gate that DOES execute — for-loop
         * runs once and visits glEnable. Confirms the flat walk picks up
         * legitimate enables reached via expansion. */
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 1) {");
        editor_feed_line("  glEnable(GL_LIGHTING);");
        editor_feed_line("}");
        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts.source_cmds = repl_state_document_cmds_mut();
        opts.source_cmd_count = repl_state_document_count();
        ASSERT_INT("for-reached flatten ok",
                   repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("for-reached lighting is ON",
                   result.user_lighting_enabled, 1);
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

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 1) {");
        editor_feed_line("}");
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
        /* _load no longer touches the cursor (Phase 1 of
         * docs/plans/done/edit-line-ownership.md); cursor adjustment
         * post-load is caller policy. Set explicitly here to mirror
         * the previous test's intent (clamp to count). */
        ASSERT_INT("command_store_load ok",
                   repl_command_store_load(&store, loaded, 2), 1);
        if (editor_state_edit_line() > repl_state_document_count())
            editor_state_edit_line_set(repl_state_document_count());
        editor_buffer_load_lines(loaded_lines, 2);
        ASSERT_INT("command_store_load count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load state count",
                   repl_state_document_count(), 2);
        ASSERT_INT("command_store_load edit clamp", editor_state_edit_line(), 2);
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
                   repl_command_store_load(&store, NULL, 1), 0);
        ASSERT_INT("command_store_load reject keeps count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load rejects overflow",
                   repl_command_store_load(&store, loaded,
                                           MAX_EDITOR_COMMANDS + 1), 0);
        ASSERT_INT("command_store_load overflow keeps count", repl_state_document_count(), 2);
        ASSERT_INT("command_store_load empty ok",
                   repl_command_store_load(&store, NULL, 0), 1);
        ASSERT_INT("command_store_load empty count", repl_state_document_count(), 0);
    }

    /* 11. editor input/selection/clipboard state facade */
    {
        EditorInputState *input;
        EditorClipboardState *clipboard;

        glr_ctrl_reset_all(); declare_test_vars();

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
        GlrCameraState *camera;
        UiPointerState *pointer;
        UiViewportState *viewport;

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
        ui_state_pointer_mut()->mouse_button = -1;
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
        GlrPresentationState *presentation;

        presentation = glr_state_presentation_mut();
        ASSERT_TRUE("presentation mut is live state",
                    presentation == glr_state_presentation_mut());

        glr_state_presentation_mut()->wireframe = 1;
        glr_state_presentation_mut()->grid_theme = GRID_THEME_TRON;
        glr_state_presentation_mut()->grid_major_idx = GRID_MAJOR_10;
        glr_state_presentation_mut()->grid_extent_idx = GRID_EXTENT_CLOSE;
        glr_state_presentation_mut()->axes_theme = AXES_THEME_NEON;
        glr_state_presentation_mut()->show_vertex_labels = 1;
        glr_state_presentation_mut()->show_normal_vectors = 1;
        glr_state_presentation_mut()->show_vertex_indices = 1; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_outlines = 1;
        glr_state_presentation_mut()->vertex_outline_style =
            VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
        glr_state_presentation_mut()->show_vertex_points = 1;
        glr_state_presentation_mut()->xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        glr_state_presentation_mut()->autonormal = 1;
        glr_state_presentation_mut()->show_light_indicators = 0;
        glr_state_presentation_mut()->backdrop_mode = 0;
        glr_camera_mut()->auto_rotate = 1;
        glr_state_presentation_mut()->highlight_current_poly = 0;
        glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
        glr_state_presentation_mut()->wrap_at_comma = 0;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM; glr_ctrl_sync_ui_chrome();
        /* focus_vertex storage was deleted in step 7a — no live
         * readers; per-frame compute lives in glr_ctrl. */

        glr_state_presentation_reset_defaults();
        ASSERT_INT("presentation reset wireframe",
                   glr_state_presentation().wireframe, CFG_DEFAULT_WIREFRAME);
        ASSERT_INT("presentation reset grid",
                   glr_state_presentation().grid_theme, CFG_DEFAULT_GRID_THEME);
        ASSERT_INT("presentation reset grid major",
                   glr_state_presentation().grid_major_idx, CFG_DEFAULT_GRID_MAJOR_IDX);
        ASSERT_INT("presentation reset grid extent",
                   glr_state_presentation().grid_extent_idx, CFG_DEFAULT_GRID_EXTENT_IDX);
        ASSERT_INT("presentation reset axes",
                   glr_state_presentation().axes_theme, CFG_DEFAULT_AXES_THEME);
        ASSERT_INT("presentation reset labels",
                   glr_state_presentation().show_vertex_labels, CFG_DEFAULT_VERTEX_LABELS);
        ASSERT_INT("presentation reset normals",
                   glr_state_presentation().show_normal_vectors, CFG_DEFAULT_NORMAL_VECTORS);
        ASSERT_INT("presentation reset indices",
                   glr_state_presentation().show_vertex_indices, CFG_DEFAULT_VERTEX_INDICES);
        ASSERT_INT("presentation reset outlines",
                   glr_state_presentation().show_vertex_outlines, CFG_DEFAULT_VERTEX_OUTLINES);
        ASSERT_INT("presentation reset vertex outline style",
                   glr_state_presentation().vertex_outline_style,
                   CFG_DEFAULT_VERTEX_OUTLINE_STYLE);
        ASSERT_INT("presentation reset points",
                   glr_state_presentation().show_vertex_points, CFG_DEFAULT_VERTEX_POINTS);
        ASSERT_INT("presentation reset xform guide",
                   glr_state_presentation().xform_guide_mode, CFG_DEFAULT_XFORM_GUIDE_MODE);
        ASSERT_INT("presentation reset lights",
                   glr_state_presentation().show_light_indicators, CFG_DEFAULT_LIGHT_INDICATORS);
        ASSERT_INT("presentation reset backdrop",
                   glr_state_presentation().backdrop_mode, CFG_DEFAULT_BACKDROP_MODE);
        /* Step 7a removed the camera reset from
         * glr_state_presentation_reset_defaults — the camera resets
         * itself via glr_camera_reset_default in glr_ctrl_reset_all. */
        ASSERT_INT("presentation reset highlight", glr_state_presentation().highlight_current_poly, 1);
        ASSERT_INT("presentation reset ortho", glr_state_presentation().ortho_mode, 0);
        ASSERT_INT("presentation reset wrap",
                   glr_state_presentation().wrap_at_comma, CFG_DEFAULT_WRAP_AT_COMMA);
        ASSERT_INT("presentation reset layout",
                   glr_state_presentation().code_panel_layout, CFG_DEFAULT_CODE_PANEL_LAYOUT);
        /* focus_vertex storage deleted in step 7a — no asserts. */
    }

    /* 14. render state facade */
    {
        ReplRenderState *render;

        render = repl_state_render_mut();
        ASSERT_TRUE("render mut is live state",
                    render == repl_state_render_mut());

        g_use_accum = 0;
        g_accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
        g_accum_passes = 8;
        g_multisample_enabled = 0;
        g_line_smooth_enabled = 0;
        g_init_attenuate_points = 0;
        g_lights[0].id     = 0;
        g_lights[0].pos[0] = -99.0f;
        g_clear_color[0] = 0.0f;
        g_clear_color[1] = 0.0f;
        g_clear_color[2] = 0.0f;
        g_clear_color[3] = 0.0f;

        glr_state_render_reset_defaults();
        repl_state_render_reset_defaults();
        ASSERT_INT("render reset use accum", g_use_accum, 1);
        ASSERT_INT("render reset accum effect", g_accum_effect, RENDER3D_ACCUM_EFFECT_AA);
        ASSERT_INT("render reset accum passes", g_accum_passes, 1);
        ASSERT_INT("render reset multisample",
                   g_multisample_enabled, CFG_DEFAULT_MULTISAMPLE);
        ASSERT_INT("render reset line smooth",
                   g_line_smooth_enabled, CFG_DEFAULT_LINE_SMOOTH);
        ASSERT_INT("render reset point attenuation",
                   g_init_attenuate_points, CFG_DEFAULT_ATTENUATE_POINTS);
        /* The dimensional light table is app-owned now; its slot ids are
         * seeded by glr_state defaults and restored by
         * glr_state_render_reset_defaults (positions/colors come from the
         * controller's render3d_lights_apply_theme). The REPL render reset
         * only zeroes the enable bitmask + clear_color. */
        ASSERT_INT("render reset light id", (int)g_lights[0].id, GL_LIGHT0);
        ASSERT_TRUE("render reset clear color r", g_clear_color[0] == 0.10f);
        ASSERT_TRUE("render reset clear color g", g_clear_color[1] == 0.10f);
        ASSERT_TRUE("render reset clear color b", g_clear_color[2] == 0.10f);
        ASSERT_TRUE("render reset clear color a", g_clear_color[3] == 1.0f);

    #ifdef GL_STUBS
        gl_stub_counts_reset();
    #endif
        repl_executor_init_resources();
    #ifdef GL_STUBS
        ASSERT_INT("executor init quadric removed", (int)gl_stub_counts[GL_STUB_gluNewQuadric], 0);
        ASSERT_INT("executor init quadric normals removed", (int)gl_stub_counts[GL_STUB_gluQuadricNormals], 0);
        ASSERT_INT("executor init quadric texture removed", (int)gl_stub_counts[GL_STUB_gluQuadricTexture], 0);
        ASSERT_INT("executor init tess", (int)gl_stub_counts[GL_STUB_gluNewTess], 1);
        ASSERT_INT("executor init tess callbacks", (int)gl_stub_counts[GL_STUB_gluTessCallback], 6);
    #endif

    #ifdef GL_STUBS
        gl_stub_counts_reset();
    #endif
        repl_executor_destroy_resources();
    #ifdef GL_STUBS
        ASSERT_INT("executor destroy quadric removed", (int)gl_stub_counts[GL_STUB_gluDeleteQuadric], 0);
        ASSERT_INT("executor destroy tess", (int)gl_stub_counts[GL_STUB_gluDeleteTess], 1);
    #endif
    }

    /* 14b. GLUT-solid outline overlay: glutSolid* shapes emit no
     * REPL-tracked vertices, so edit_overlays_render_outlines can't
     * trace them with glVertex; instead it re-draws each shape under
     * glPolygonMode(GL_LINE) + polygon offset (the GL_LINE wireframe
     * redraw pass). Drive the pass and assert the shape is redrawn and
     * the polygon-mode/offset state is toggled. The render call issues
     * real GL, so it (and its count assertions) only run under stubs. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glutSolidCube(0.5);");
        editor_feed_line("glutSolidSphere(0.3, 8, 8);");
        repl_flatten_commands(editor_state_edit_line());

    #ifdef GL_STUBS
        OverlayWalkCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.program = repl_state_flat_program_view();
        ctx.cursor.edit_line_idx = -1;          /* no cursor highlight */
        ctx.cursor.cursor_block_begin = -1;
        ctx.cursor.cursor_block_end = -1;

        /* outlines on, no current-poly highlight → both shapes redraw */
        ctx.show_vertex_outlines = 1;
        ctx.highlight_current_poly = 0;
        gl_stub_counts_reset();
        edit_overlays_render_outlines(&ctx, 0, 0);
        ASSERT_INT("outline redraws the cube wireframe",
                   (int)gl_stub_counts[GL_STUB_glutSolidCube], 1);
        ASSERT_INT("outline redraws the sphere wireframe",
                   (int)gl_stub_counts[GL_STUB_glutSolidSphere], 1);
        ASSERT_TRUE("outline switches polygon mode (to GL_LINE and back)",
                    gl_stub_counts[GL_STUB_glPolygonMode] >= 2);
        ASSERT_TRUE("outline enables polygon offset",
                    gl_stub_counts[GL_STUB_glPolygonOffset] >= 1);

        /* both link flags off → outer guard skips the redraw entirely */
        ctx.show_vertex_outlines = 0;
        ctx.highlight_current_poly = 0;
        gl_stub_counts_reset();
        edit_overlays_render_outlines(&ctx, 0, 0);
        ASSERT_INT("no redraw when outlines + highlight both off (cube)",
                   (int)gl_stub_counts[GL_STUB_glutSolidCube], 0);
        ASSERT_INT("no redraw when outlines + highlight both off (sphere)",
                   (int)gl_stub_counts[GL_STUB_glutSolidSphere], 0);

        /* cursor on the cube line, current-poly highlight on, outlines
         * off → only the cube redraws (the highlighted shape). */
        ctx.show_vertex_outlines = 0;
        ctx.highlight_current_poly = 1;
        ctx.cursor.edit_line_idx = 0;   /* the glutSolidCube source line */
        gl_stub_counts_reset();
        edit_overlays_render_outlines(&ctx, 0, 0);
        ASSERT_INT("highlight redraws only the cursor's cube",
                   (int)gl_stub_counts[GL_STUB_glutSolidCube], 1);
        ASSERT_INT("highlight leaves the non-cursor sphere alone",
                   (int)gl_stub_counts[GL_STUB_glutSolidSphere], 0);
    #endif
    }

    /* 15. MAX_EXPR_VARS truncation warning - verify collect_visible_vars tracks total */
    {
        /* Test A: verify collect_visible_vars returns correct total when not truncated */
        {
            glr_ctrl_reset_all(); declare_test_vars();
            editor_feed_line("for(i, 0, 1) {");
            editor_feed_line("  for(j, 0, 1) {");

            ExprVar vars[8];
            int total = 0;
            int count = collect_visible_vars(2, vars, 8, &total);
            ASSERT_INT("2 nested loops: count", count, 2);
            ASSERT_INT("2 nested loops: total", total, 2);
            ASSERT_TRUE("total equals count (not truncated)", total == count);
        }

        /* Test B: verify collect_visible_vars returns total > max_vars when truncated */
        {
            glr_ctrl_reset_all(); declare_test_vars();
            /* Manually verify the logic: if we can have 32 visible vars,
               and we ask for only 8, we should see total > 8 if we had more scope */
            ExprVar vars[8];
            int total = 0;
            editor_feed_line("for(i, 0, 1) {");
            int count1 = collect_visible_vars(1, vars, 8, &total);
            ASSERT_INT("1 loop with max_vars=8: count", count1, 1);
            ASSERT_INT("1 loop with max_vars=8: total", total, 1);
            ASSERT_TRUE("1 loop: total equals count", total == count1);
        }

        /* Test C: verify total is well below MAX_EXPR_VARS for shallow
         * nesting (the truncation guard should never fire here). */
        {
            glr_ctrl_reset_all();
            editor_feed_line("for(i, 0, 1) {");
            editor_feed_line("  for(j, 0, 1) {");
            editor_feed_line("    for(k, 0, 1) {");
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
            glr_ctrl_reset_all();
            editor_feed_line("for(a, 0, 1) {");
            editor_feed_line("  for(b, 0, 1) {");
            editor_feed_line("    for(c, 0, 1) {");
            editor_feed_line("      for(d, 0, 1) {");
            editor_feed_line("        for(e, 0, 1) {");
            /* Query at position 5 with max_vars=4 */
            int count = collect_visible_vars(5, vars, 4, &total);
            ASSERT_INT("5 nested, max_vars=4: count capped", count, 4);
            ASSERT_INT("5 nested, max_vars=4: total uncapped", total, 5);
            ASSERT_TRUE("5 nested: total > max_vars", total > 4);
        }
    }

    /* funcN duplicate-def lookup: first-wins.
     *
     * The commit pipeline rejects any user-facing path that would create
     * two CMD_FUNC_DEFs for the same slot (compile.c:1775 / commit.c:654),
     * so this corruption can only arise from a malformed loaded file or
     * a future code change that bypasses validation. Even then, the
     * flatten contract is that the FIRST def wins — matching the
     * pre-cbe73d2 linear-scan, which `break`-ed out of its scan on the
     * first match. Any other choice would silently switch the body bound
     * to a slot when a malformed file is loaded.
     *
     * Setup: declare a valid func0 with CMD_VERTEX3F body + a call via
     * editor_feed_line, then directly append a SECOND CMD_FUNC_DEF for
     * the same slot 0 with a CMD_COLOR3F body to the document arrays +
     * editor buffer. The flatten path scans and matches both defs; the
     * first must win. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("func0(r) {");
        editor_feed_line("  glVertex3f(r, 0, 0);");
        editor_feed_line("}");
        editor_feed_line("func0(7);");

        GLCmd *cmds = repl_state_document_cmds_mut();
        int n = repl_state_document_count();
        ASSERT_INT("dup-funcN setup: doc count", n, 4);
        ASSERT_INT("dup-funcN setup: row 0 is FUNC_DEF",
                   (int)cmds[0].type, CMD_FUNC_DEF);
        ASSERT_INT("dup-funcN setup: row 3 is CALL",
                   (int)cmds[3].type, CMD_CALL);

        /* Append a SECOND CMD_FUNC_DEF at slot 0 with a body that emits
         * CMD_COLOR3F (different from func0's CMD_VERTEX3F body). The
         * text on the dup def is "func9(s) {" so parse_repl_func_signature
         * pulls one parameter; flatten does NOT validate that the text's
         * funcN token matches args[0], so the args[0]=0 is what flatten
         * matches against the call. */
        source_document_insert_line(n,     "func9(s) {");
        source_document_insert_line(n + 1, "  glColor3f(s, 0, 0);");
        source_document_insert_line(n + 2, "}");

        GLCmd dup_def    = {0};
        GLCmd dup_body   = {0};
        GLCmd dup_end    = {0};
        dup_def.type     = CMD_FUNC_DEF;
        dup_def.args[0]  = 0;          /* collide with func0's slot */
        dup_def.num_args = 1;
        dup_def.valid    = 1;
        dup_body.type    = CMD_COLOR3F;
        dup_body.valid   = 1;
        dup_body.num_args = 3;         /* r, g, b — flatten resolves args from text */
        dup_body.has_vars = 1;         /* references "s" param */
        dup_end.type     = CMD_FUNC_END;
        dup_end.valid    = 1;
        cmds[n]     = dup_def;
        cmds[n + 1] = dup_body;
        cmds[n + 2] = dup_end;
        repl_state_document_count_set(n + 3);

        /* Mark source dirty so flatten rebuilds against the appended rows. */
        repl_state_mark_source_dirty();
        repl_flatten_commands(editor_state_edit_line());

        const FlatProgramView fv = repl_state_flat_program_view();
        int found_vertex = 0;
        int found_color  = 0;
        for (int k = 0; k < fv.cmd_count; k++) {
            if (fv.cmds[k].type == CMD_VERTEX3F) found_vertex = 1;
            if (fv.cmds[k].type == CMD_COLOR3F)  found_color  = 1;
        }
        /* First-wins: func0's body (CMD_VERTEX3F at source row 1) is the
         * expansion of func0(7). The duplicate def's body (CMD_COLOR3F at
         * source row 5) is ignored even though it appears later. */
        ASSERT_TRUE("dup-funcN first-wins: vertex emitted",  found_vertex);
        ASSERT_TRUE("dup-funcN first-wins: color suppressed", !found_color);
    }

    /* repl_flatten_cost_at_line: the statusbar budget readout.
     *
     * Program shape: an aliased func (box, slot 0) called both from
     * inside a for-loop and at top level, plus a plain line and a
     * comment. Flat layout: box body = 5 cmds (begin + 3 verts + end);
     * loop = 4 iters x (1 color + 5 box) = 24; top-level box(9) = 5;
     * plain color = 1. Total 30. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("box(s) {");                 /* 0  */
        editor_feed_line("  glBegin(GL_TRIANGLES);"); /* 1  */
        editor_feed_line("  glVertex3f(s, 0, 0);");   /* 2  */
        editor_feed_line("  glVertex3f(0, s, 0);");   /* 3  */
        editor_feed_line("  glVertex3f(0, 0, s);");   /* 4  */
        editor_feed_line("  glEnd();");               /* 5  */
        editor_feed_line("}");                        /* 6  */
        editor_feed_line("for(i, 0, 4) {");           /* 7  */
        editor_feed_line("  glColor3f(i, 0, 0);");    /* 8  */
        editor_feed_line("  box(i);");                /* 9  */
        editor_feed_line("}");                        /* 10 */
        editor_feed_line("box(9);");                  /* 11 */
        editor_feed_line("glColor3f(1, 0, 0);");      /* 12 */
        editor_feed_line("// budget note");           /* 13 */
        repl_flatten_commands(editor_state_edit_line());
        ASSERT_INT("flat-cost setup: flat total",
                   repl_state_flat_program_count(), 30);

        ReplFlatCost c;

        /* Function attribution (func_scope_mask): every box expansion,
         * across both call sites, from the def head / body / end. */
        c = repl_flatten_cost_at_line(0);
        ASSERT_INT("fn cost kind (def head)", (int)c.kind, REPL_FLAT_COST_FUNC);
        ASSERT_INT("fn cost count (def head)", c.count, 25);
        c = repl_flatten_cost_at_line(3);
        ASSERT_INT("fn cost kind (body)", (int)c.kind, REPL_FLAT_COST_FUNC);
        ASSERT_INT("fn cost count (body)", c.count, 25);
        c = repl_flatten_cost_at_line(6);
        ASSERT_INT("fn cost kind (end line)", (int)c.kind, REPL_FLAT_COST_FUNC);
        ASSERT_INT("fn cost count (end line)", c.count, 25);

        /* Block attribution: direct body (4 colors) + the box calls
         * made inside the loop (20), from head / body / end. */
        c = repl_flatten_cost_at_line(7);
        ASSERT_INT("block cost kind (for head)", (int)c.kind, REPL_FLAT_COST_BLOCK);
        ASSERT_INT("block cost count (for head)", c.count, 24);
        c = repl_flatten_cost_at_line(8);
        ASSERT_INT("block cost kind (inside)", (int)c.kind, REPL_FLAT_COST_BLOCK);
        ASSERT_INT("block cost count (inside)", c.count, 24);
        c = repl_flatten_cost_at_line(10);
        ASSERT_INT("block cost kind (end line)", (int)c.kind, REPL_FLAT_COST_BLOCK);
        ASSERT_INT("block cost count (end line)", c.count, 24);

        /* Call-site attribution: the loop call counts once per
         * iteration; the top-level call counts its single expansion. */
        c = repl_flatten_cost_at_line(9);
        ASSERT_INT("call cost kind (loop call)", (int)c.kind, REPL_FLAT_COST_CALL);
        ASSERT_INT("call cost count (loop call)", c.count, 20);
        c = repl_flatten_cost_at_line(11);
        ASSERT_INT("call cost kind (top call)", (int)c.kind, REPL_FLAT_COST_CALL);
        ASSERT_INT("call cost count (top call)", c.count, 5);

        /* Plain line and comment. */
        c = repl_flatten_cost_at_line(12);
        ASSERT_INT("line cost kind", (int)c.kind, REPL_FLAT_COST_LINE);
        ASSERT_INT("line cost count", c.count, 1);
        c = repl_flatten_cost_at_line(13);
        ASSERT_INT("comment cost kind", (int)c.kind, REPL_FLAT_COST_NONE);
        ASSERT_INT("comment cost count", c.count, 0);

        /* Out of range is NONE. */
        c = repl_flatten_cost_at_line(-1);
        ASSERT_INT("oob cost kind", (int)c.kind, REPL_FLAT_COST_NONE);
        c = repl_flatten_cost_at_line(repl_state_document_count() + 5);
        ASSERT_INT("oob cost kind (past end)", (int)c.kind, REPL_FLAT_COST_NONE);
    }

    /* repl_flatten_cursor_polygon: SINGLE_POLYGON overlay-scope resolution.
     * Two GL_QUADS quads (+Y face, -Y face) with comment/color/normal state
     * lines interleaved, mirroring a real cube example so the cursor-line ->
     * vertex-ordinal mapping is exercised against non-vertex lines too. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_QUADS);");         /* 0  */
        editor_feed_line("// +Y face");                 /* 1  */
        editor_feed_line("glColor3f(0.95, 0.25, 1);");  /* 2  */
        editor_feed_line("glNormal3f(0, 1, 0);");        /* 3  */
        editor_feed_line("glVertex3f(-1, 1, 1);");       /* 4  vtx 0 */
        editor_feed_line("glVertex3f(1, 1, 1);");        /* 5  vtx 1 */
        editor_feed_line("glVertex3f(1, 1, -1);");       /* 6  vtx 2 */
        editor_feed_line("glVertex3f(-1, 1, -1);");      /* 7  vtx 3 */
        editor_feed_line("// -Y face");                 /* 8  */
        editor_feed_line("glNormal3f(0, -1, 0);");       /* 9  */
        editor_feed_line("glVertex3f(-1, -1, -1);");     /* 10 vtx 4 */
        editor_feed_line("glVertex3f(1, -1, -1);");      /* 11 vtx 5 */
        editor_feed_line("glVertex3f(1, -1, 1);");       /* 12 vtx 6 */
        editor_feed_line("glVertex3f(-1, -1, 1);");      /* 13 vtx 7 */
        editor_feed_line("glEnd();");                    /* 14 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;

        p = repl_flatten_cursor_polygon(0);
        ASSERT_INT("cursor on glBegin line: whole block (invalid)", p.valid, 0);

        /* Comment/state lines before the first vertex belong to quad 0. */
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("leading comment maps to quad 0", p.valid && p.first == 0 && p.last == 3);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("glNormal3f before v0 maps to quad 0", p.valid && p.first == 0 && p.last == 3);

        /* Every vertex line 0..3 selects quad 0 (ordinals 0..3). */
        p = repl_flatten_cursor_polygon(4);
        ASSERT_TRUE("v0 maps to quad 0", p.valid && p.first == 0 && p.last == 3);
        p = repl_flatten_cursor_polygon(7);
        ASSERT_TRUE("v3 (last of quad 0) maps to quad 0", p.valid && p.first == 0 && p.last == 3);

        /* State lines after v3 and before v4 already belong to quad 1. */
        p = repl_flatten_cursor_polygon(8);
        ASSERT_TRUE("comment after v3 maps to quad 1", p.valid && p.first == 4 && p.last == 7);
        p = repl_flatten_cursor_polygon(9);
        ASSERT_TRUE("glNormal3f before v4 maps to quad 1", p.valid && p.first == 4 && p.last == 7);

        /* Vertex lines 4..7 select quad 1 (ordinals 4..7). */
        p = repl_flatten_cursor_polygon(10);
        ASSERT_TRUE("v4 maps to quad 1", p.valid && p.first == 4 && p.last == 7);
        p = repl_flatten_cursor_polygon(13);
        ASSERT_TRUE("v7 (last of quad 1) maps to quad 1", p.valid && p.first == 4 && p.last == 7);

        /* The cursor-block source-extent cache (shared with the other
         * scopes' block highlighting) treats the CMD_END line itself as
         * outside the block — edit_line_idx < end_line, not <=. So the
         * cursor sitting exactly on glEnd() resolves no block at all,
         * same as the pre-existing FIRST_INSTANCE/ALL_INSTANCES behavior. */
        p = repl_flatten_cursor_polygon(14);
        ASSERT_INT("glEnd line itself is outside the block", p.valid, 0);
        ASSERT_INT("no fan anchor for GL_QUADS", p.fan_anchor, 0);

        /* Outside any glBegin block: invalid. */
        editor_feed_line("glColor3f(1, 1, 1);");          /* 15 */
        repl_flatten_commands(editor_state_edit_line());
        p = repl_flatten_cursor_polygon(15);
        ASSERT_INT("top-level line outside block is invalid", p.valid, 0);
    }

    /* repl_flatten_cursor_polygon: per-mode grouping (GL_TRIANGLES,
     * GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUAD_STRIP, GL_LINES,
     * GL_LINE_STRIP, GL_POINTS) and the whole-block modes (GL_POLYGON,
     * GL_LINE_LOOP) that never resolve a single primitive. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");      /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");      /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");      /* 3 v2 */
        editor_feed_line("glVertex3f(0,0,1);");      /* 4 v3 */
        editor_feed_line("glVertex3f(1,1,0);");      /* 5 v4 */
        editor_feed_line("glVertex3f(0,1,1);");      /* 6 v5 */
        editor_feed_line("glEnd();");                /* 7 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_TRIANGLES v0 -> triangle 0", p.valid && p.first == 0 && p.last == 2);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("GL_TRIANGLES v2 -> triangle 0", p.valid && p.first == 0 && p.last == 2);
        p = repl_flatten_cursor_polygon(4);
        ASSERT_TRUE("GL_TRIANGLES v3 -> triangle 1", p.valid && p.first == 3 && p.last == 5);
        p = repl_flatten_cursor_polygon(6);
        ASSERT_TRUE("GL_TRIANGLES v5 -> triangle 1", p.valid && p.first == 3 && p.last == 5);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLE_STRIP);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");            /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");            /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");            /* 3 v2 */
        editor_feed_line("glVertex3f(1,1,0);");            /* 4 v3 */
        editor_feed_line("glEnd();");                       /* 5 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        /* v0 and v1 are before the first triangle completes: both fall
         * back to the earliest triangle {0,1,2}. */
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_TRIANGLE_STRIP v0 -> tri {0,1,2}", p.valid && p.first == 0 && p.last == 2);
        p = repl_flatten_cursor_polygon(2);
        ASSERT_TRUE("GL_TRIANGLE_STRIP v1 -> tri {0,1,2}", p.valid && p.first == 0 && p.last == 2);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("GL_TRIANGLE_STRIP v2 -> tri {0,1,2}", p.valid && p.first == 0 && p.last == 2);
        p = repl_flatten_cursor_polygon(4);
        ASSERT_TRUE("GL_TRIANGLE_STRIP v3 -> tri {1,2,3}", p.valid && p.first == 1 && p.last == 3);
        ASSERT_INT("no fan anchor for GL_TRIANGLE_STRIP", p.fan_anchor, 0);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLE_FAN);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");          /* 1 v0 (center) */
        editor_feed_line("glVertex3f(1,0,0);");          /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");          /* 3 v2 */
        editor_feed_line("glVertex3f(-1,0,0);");         /* 4 v3 */
        editor_feed_line("glEnd();");                     /* 5 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(2);
        ASSERT_TRUE("GL_TRIANGLE_FAN v1 -> tri {center,1,2}",
                    p.valid && p.first == 1 && p.last == 2 && p.fan_anchor);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("GL_TRIANGLE_FAN v2 -> tri {center,1,2}",
                    p.valid && p.first == 1 && p.last == 2 && p.fan_anchor);
        p = repl_flatten_cursor_polygon(4);
        ASSERT_TRUE("GL_TRIANGLE_FAN v3 -> tri {center,2,3}",
                    p.valid && p.first == 2 && p.last == 3 && p.fan_anchor);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_QUAD_STRIP);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");        /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");        /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");        /* 3 v2 */
        editor_feed_line("glVertex3f(1,1,0);");        /* 4 v3 */
        editor_feed_line("glVertex3f(0,2,0);");        /* 5 v4 */
        editor_feed_line("glVertex3f(1,2,0);");        /* 6 v5 */
        editor_feed_line("glEnd();");                   /* 7 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_QUAD_STRIP v0 -> quad {0..3}", p.valid && p.first == 0 && p.last == 3);
        p = repl_flatten_cursor_polygon(4);
        ASSERT_TRUE("GL_QUAD_STRIP v3 -> quad {0..3}", p.valid && p.first == 0 && p.last == 3);
        p = repl_flatten_cursor_polygon(5);
        ASSERT_TRUE("GL_QUAD_STRIP v4 -> quad {2..5}", p.valid && p.first == 2 && p.last == 5);
        p = repl_flatten_cursor_polygon(6);
        ASSERT_TRUE("GL_QUAD_STRIP v5 -> quad {2..5}", p.valid && p.first == 2 && p.last == 5);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_LINES);");   /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");    /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");    /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");    /* 3 v2 */
        editor_feed_line("glVertex3f(1,1,0);");    /* 4 v3 */
        editor_feed_line("glEnd();");               /* 5 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_LINES v0 -> segment {0,1}", p.valid && p.first == 0 && p.last == 1);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("GL_LINES v2 -> segment {2,3}", p.valid && p.first == 2 && p.last == 3);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_LINE_STRIP);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");        /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");        /* 2 v1 */
        editor_feed_line("glVertex3f(0,1,0);");        /* 3 v2 */
        editor_feed_line("glEnd();");                   /* 4 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_LINE_STRIP v0 -> segment {0,1}", p.valid && p.first == 0 && p.last == 1);
        p = repl_flatten_cursor_polygon(2);
        ASSERT_TRUE("GL_LINE_STRIP v1 -> segment {0,1}", p.valid && p.first == 0 && p.last == 1);
        p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("GL_LINE_STRIP v2 -> segment {1,2}", p.valid && p.first == 1 && p.last == 2);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_POINTS);");  /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");    /* 1 v0 */
        editor_feed_line("glVertex3f(1,0,0);");    /* 2 v1 */
        editor_feed_line("glEnd();");               /* 3 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p;
        p = repl_flatten_cursor_polygon(1);
        ASSERT_TRUE("GL_POINTS v0 -> single point {0,0}", p.valid && p.first == 0 && p.last == 0);
        p = repl_flatten_cursor_polygon(2);
        ASSERT_TRUE("GL_POINTS v1 -> single point {1,1}", p.valid && p.first == 1 && p.last == 1);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_POLYGON);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");    /* 1 */
        editor_feed_line("glVertex3f(1,0,0);");    /* 2 */
        editor_feed_line("glVertex3f(0,1,0);");    /* 3 */
        editor_feed_line("glEnd();");               /* 4 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p = repl_flatten_cursor_polygon(2);
        ASSERT_INT("GL_POLYGON never resolves a single primitive", p.valid, 0);
    }
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_LINE_LOOP);"); /* 0 */
        editor_feed_line("glVertex3f(0,0,0);");      /* 1 */
        editor_feed_line("glVertex3f(1,0,0);");      /* 2 */
        editor_feed_line("glVertex3f(0,1,0);");      /* 3 */
        editor_feed_line("glEnd();");                 /* 4 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p = repl_flatten_cursor_polygon(2);
        ASSERT_INT("GL_LINE_LOOP never resolves a single primitive", p.valid, 0);
    }

    /* repl_flatten_cursor_polygon inside a for-loop: first-instance
     * semantics — the cursor's polygon is resolved against the FIRST
     * unrolled iteration only, matching the existing current-block cache. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 2) {");           /* 0 */
        editor_feed_line("  glBegin(GL_QUADS);");     /* 1 */
        editor_feed_line("  glVertex3f(i,0,0);");     /* 2 v0 */
        editor_feed_line("  glVertex3f(i,1,0);");     /* 3 v1 */
        editor_feed_line("  glVertex3f(i,1,1);");     /* 4 v2 */
        editor_feed_line("  glVertex3f(i,0,1);");     /* 5 v3 */
        editor_feed_line("  glEnd();");                /* 6 */
        editor_feed_line("}");                         /* 7 */
        repl_flatten_commands(editor_state_edit_line());

        ReplCursorPolygon p = repl_flatten_cursor_polygon(3);
        ASSERT_TRUE("looped GL_QUADS body resolves against first iteration",
                    p.valid && p.first == 0 && p.last == 3);
    }

    /* #4 regression: repl_config_bag_set must detect snprintf truncation
     * and refuse the write. Pre-fix, an over-long key truncated silently
     * and the function returned 1 — subsequent gets against the
     * untruncated slug would miss the stored value. */
    {
        ReplConfigBag bag;
        repl_config_bag_clear(&bag);

        /* Sanity: a fitting key/value goes in. */
        ASSERT_INT("set ok: short key+value",
                   repl_config_bag_set(&bag, "wireframe", "1"), 1);
        ASSERT_INT("count after fitting set", repl_config_bag_count(&bag), 1);
        ASSERT_STR("get back the value",
                   repl_config_bag_get(&bag, "wireframe"), "1");

        /* REPL_CFG_KEY_MAX = 24, so a 30-char key is 6 over the limit. */
        char long_key[64];
        memset(long_key, 'k', sizeof(long_key) - 1);
        long_key[30] = '\0';
        ASSERT_INT("set fail: oversized key returns 0",
                   repl_config_bag_set(&bag, long_key, "9"), 0);
        ASSERT_INT("count unchanged after oversized key",
                   repl_config_bag_count(&bag), 1);

        /* REPL_CFG_VALUE_MAX = 40, so a 44-char value is 4 over the
         * limit. The width was bumped from 16 to 32 to fit symbolic
         * enum names like "RENDER3D_BACKDROP_CITY_AND_STARS", and then to 40. */
        char long_value[64];
        memset(long_value, 'v', sizeof(long_value) - 1);
        long_value[44] = '\0';
        ASSERT_INT("set fail: oversized value returns 0",
                   repl_config_bag_set(&bag, "axes", long_value), 0);
        ASSERT_INT("count unchanged after oversized value",
                   repl_config_bag_count(&bag), 1);
        ASSERT_TRUE("axes not added",
                    repl_config_bag_get(&bag, "axes") == NULL);

        /* Replace-path: too-long value must NOT overwrite the existing one. */
        ASSERT_INT("set fail: replace with oversized value returns 0",
                   repl_config_bag_set(&bag, "wireframe", long_value), 0);
        ASSERT_STR("replace-path preserves prior value on truncation",
                   repl_config_bag_get(&bag, "wireframe"), "1");

        /* set_int path: with REPL_CFG_VALUE_MAX >= 12 every 32-bit int
         * fits, but pin the contract — overlong key still fails. */
        ASSERT_INT("set_int ok: short key",
                   repl_config_bag_set_int(&bag, "grid", 42), 1);
        ASSERT_INT("set_int fail: oversized key returns 0",
                   repl_config_bag_set_int(&bag, long_key, 42), 0);

        /* Boundary case: a key exactly at REPL_CFG_KEY_MAX - 1 should
         * fit; one at REPL_CFG_KEY_MAX must not. */
        char max_key[REPL_CFG_KEY_MAX + 4];
        memset(max_key, 'm', sizeof(max_key) - 1);
        max_key[REPL_CFG_KEY_MAX - 1] = '\0';   /* exactly fills the slot */
        ASSERT_INT("boundary: REPL_CFG_KEY_MAX-1 key fits",
                   repl_config_bag_set(&bag, max_key, "1"), 1);

        memset(max_key, 'M', sizeof(max_key) - 1);
        max_key[REPL_CFG_KEY_MAX] = '\0';        /* one too many */
        ASSERT_INT("boundary: REPL_CFG_KEY_MAX key truncates → 0",
                   repl_config_bag_set(&bag, max_key, "1"), 0);
    }

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
