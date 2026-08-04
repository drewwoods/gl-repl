#include "editor/state.h"
#include <stdbool.h>
#include "repl/pipeline.h"
#include "editor/input.h"
#include "repl/state.h"
#include "repl/examples.h"
#include "repl/load.h"
#include "app/glr_ctrl.h"   /* glr_ctrl_reset_all (end-to-end P1 test) */
#include "support/repl_test_support.h"

// Include the C file directly to access its static callbacks.
// We must NOT link repl_executor.o into test_repl_executor!
#include "repl/executor.c"

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void test_tess_callbacks(void) {
    repl_executor_init_resources();

    repl_render_tess_vtx_begin_cb(GL_POLYGON);

    TessVertex v1;
    v1.pos[0] = 0.0; v1.pos[1] = 0.0; v1.pos[2] = 0.0;
    v1.normal[0] = 0.0; v1.normal[1] = 0.0; v1.normal[2] = 1.0;
    v1.color[0] = 1.0; v1.color[1] = 1.0; v1.color[2] = 1.0; v1.color[3] = 1.0;
    repl_render_tess_vtx_cb(&v1);

    GLdouble coords[3] = {1.0, 1.0, 0.0};
    void *vertex_data[4] = { &v1, NULL, NULL, NULL };
    GLfloat weight[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    void *out_data = NULL;

    // Normal combine call
    repl_render_tess_comb_cb(coords, vertex_data, weight, &out_data);
    ASSERT_TRUE("Combine returned new vertex", out_data != NULL);

    // Call combine overflow
    g_tess_vert_count = TESS_VERT_BUF_SIZE; // force overflow
    repl_render_tess_comb_cb(coords, vertex_data, weight, &out_data);
    ASSERT_TRUE("Combine returned NULL on overflow", out_data == NULL);

    // Reset for further tests
    g_tess_vert_count = 0;

    repl_render_tess_err_cb(100151); // GLU_TESS_ERROR

    repl_render_tess_vtx_end_cb();

    repl_executor_destroy_resources();
}

static void test_fade_context(void) {
    /* Since g_execute_alpha_scale and g_execute_skip_geom_before_pc were
     * refactored into local variables inside repl_execute_program, we test that
     * passing them in ReplExecutionOptions is correctly supported. */
    ReplExecutionOptions opts = {
        .flat_cmd_count = 0,
        .fade_alpha_scale = 0.5f,
        .skip_geom_before_pc = 5,
        .has_fade_context = 1
    };
    repl_execute_program(&opts);
    ASSERT_TRUE("fade context option fields set", opts.fade_alpha_scale == 0.5f && opts.skip_geom_before_pc == 5);
}

static void test_predef_edge_cases(void) {
    // These should return early safely without crashing
    repl_copy_predef_values(NULL, 10);
    repl_copy_predef_values(NULL, -1);

    float dummy[10];
    repl_copy_predef_values(dummy, 0);
    repl_copy_predef_values(dummy, -1);

    repl_restore_predef_values(NULL, 10);
    repl_restore_predef_values(NULL, -1);

    repl_restore_predef_values(dummy, 0);
    repl_restore_predef_values(dummy, -1);

    // Test the loop copying
    g_num_predef_vars_mut = 2;
    g_predef_vars_mut[0].value = 1.0f;
    g_predef_vars_mut[1].value = 2.0f;
    repl_copy_predef_values(dummy, 2);
    ASSERT_TRUE("Copy vals", dummy[0] == 1.0f && dummy[1] == 2.0f);
    repl_copy_predef_values(dummy, 1); // hits max_vals path

    dummy[0] = 3.0f;
    dummy[1] = 4.0f;
    repl_restore_predef_values(dummy, 2);
    ASSERT_TRUE("Restore vals", g_predef_vars[0].value == 3.0f && g_predef_vars[1].value == 4.0f);
    repl_restore_predef_values(dummy, 1); // hits max_vals path
}

static void test_transform_stack_edge_cases(void) {
    repl_executor_apply_tracked_transform_cmd(NULL, NULL);

    int depth = 0;
    repl_executor_apply_tracked_transform_cmd(NULL, &depth);

    GLCmd cmd = { .type = CMD_VERTEX3F };
    repl_executor_apply_tracked_transform_cmd(&cmd, NULL);

    GLCmd translate = { .type = CMD_TRANSLATE3F, .args = {1, 2, 3} };
    repl_executor_apply_tracked_transform_cmd(&translate, &depth);

    // Test other transforms to hit switch branches in apply_transform_cmd
    GLCmd tcmd = { .type = CMD_SCALEF, .args = {1, 1, 1} };
    repl_executor_apply_tracked_transform_cmd(&tcmd, NULL);

    tcmd.type = CMD_ROTATEF; tcmd.args[0] = 90;
    repl_executor_apply_tracked_transform_cmd(&tcmd, NULL);

    tcmd.type = CMD_PUSH_MATRIX;
    repl_executor_apply_tracked_transform_cmd(&tcmd, NULL);

    tcmd.type = CMD_POP_MATRIX;
    repl_executor_apply_tracked_transform_cmd(&tcmd, NULL);
}

static void test_apply_state_cmd_edge_cases(void) {
    repl_apply_state_cmd(NULL, 1.0f);

    GLCmd cmd = { .type = CMD_GOTO_LABEL }; // not a state cmd
    int ret = repl_apply_state_cmd(&cmd, 1.0f);
    ASSERT_TRUE("Non-state cmd returns 0", ret == 0);

    // Test all branches in repl_apply_state_cmd
    /* Enum-spec commands now use uniform args[] storage (no GLCmd.mode). */
    cmd.type = CMD_ENABLE; cmd.args[0] = GL_BLEND; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_DISABLE; cmd.args[0] = GL_BLEND; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_SHADE_MODEL; cmd.args[0] = GL_FLAT; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_COLOR_MATERIAL; cmd.args[0] = GL_FRONT; cmd.args[1] = GL_AMBIENT; repl_apply_state_cmd(&cmd, 1.0f);

    /* glMaterialfv args[]: [0]=face, [1]=pname, [2..]=value(s). */
    cmd.type = CMD_MATERIALFV; cmd.args[0] = GL_FRONT; cmd.args[1] = GL_SHININESS; cmd.args[2] = 50; cmd.num_args = 3; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.num_args = 6; repl_apply_state_cmd(&cmd, 1.0f);

    cmd.type = CMD_LIGHT_MODEL_I; cmd.args[0] = GL_LIGHT_MODEL_TWO_SIDE; cmd.args[1] = 1; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_FRONT_FACE; cmd.args[0] = GL_CCW; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_DEPTH_MASK; cmd.args[0] = 1; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_EDGE_FLAG; cmd.args[0] = GL_TRUE; repl_apply_state_cmd(&cmd, 1.0f);
    /* glPointParameterfv args[]: [0]=pname, [1..3]=coeffs. */
    cmd.type = CMD_POINT_PARAMETER_FV; cmd.args[0] = GL_POINT_DISTANCE_ATTENUATION; cmd.num_args = 4; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_BLEND_FUNC; cmd.args[0] = GL_SRC_ALPHA; cmd.args[1] = GL_ONE_MINUS_SRC_ALPHA; repl_apply_state_cmd(&cmd, 1.0f);
    /* glClipPlane args[]: [0]=plane, [1..4]=equation. */
    cmd.type = CMD_CLIP_PLANE; cmd.args[0] = GL_CLIP_PLANE0; cmd.args[1] = 0; cmd.args[2] = 1; cmd.args[3] = 0; cmd.args[4] = 0.5f; cmd.num_args = 5; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_FOG_I; cmd.args[0] = GL_FOG_MODE; cmd.args[1] = GL_EXP2; cmd.num_args = 2; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_FOG_F; cmd.args[0] = GL_FOG_DENSITY; cmd.args[1] = 0.1f; cmd.num_args = 2; repl_apply_state_cmd(&cmd, 1.0f);
    /* glFogfv args[]: [0]=pname, [1..4]=rgba. */
    cmd.type = CMD_FOG_FV; cmd.args[0] = GL_FOG_COLOR; cmd.args[1] = 0.05f; cmd.args[2] = 0.06f; cmd.args[3] = 0.08f; cmd.args[4] = 1; cmd.num_args = 5; repl_apply_state_cmd(&cmd, 1.0f);
}

/* Regression net for the enum-arg storage migration: assert the actual
 * GL arguments (not just call counts) for the multi-enum commands whose
 * arg order is the easy thing to get wrong. Closes the gap that let a
 * CMD_BLEND_FUNC arg-shift (cmd->mode vs args[]) pass silently - counts
 * alone never noticed sfactor/dfactor were swapped/zeroed.
 *
 * Expected lines are built from the in-scope GL_* macros, so this is
 * header-independent (real GL vs stub enum values both work). */
static void test_enum_arg_gl_trace(void) {
    char path[] = "/tmp/test_repl_executor_enum_trace.txt";
    gl_stub_trace_open(path);

    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_BLEND_FUNC; cmd.num_args = 2;
    cmd.args[0] = GL_SRC_ALPHA; cmd.args[1] = GL_ONE_MINUS_SRC_ALPHA;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_COLOR_MASK; cmd.num_args = 4;
    cmd.args[0] = GL_TRUE; cmd.args[1] = GL_FALSE;
    cmd.args[2] = GL_TRUE; cmd.args[3] = GL_FALSE;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_DEPTH_MASK; cmd.num_args = 1;
    cmd.args[0] = GL_FALSE;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_EDGE_FLAG; cmd.num_args = 1;
    cmd.args[0] = GL_FALSE;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_COLOR_MATERIAL; cmd.num_args = 2;
    cmd.args[0] = GL_FRONT_AND_BACK; cmd.args[1] = GL_AMBIENT_AND_DIFFUSE;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_CLIP_PLANE; cmd.num_args = 5;
    cmd.args[0] = GL_CLIP_PLANE1;
    cmd.args[1] = 0.25f; cmd.args[2] = 1; cmd.args[3] = 0; cmd.args[4] = 0.5f;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_FOG_I; cmd.num_args = 2;
    cmd.args[0] = GL_FOG_MODE; cmd.args[1] = GL_EXP2;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_FOG_F; cmd.num_args = 2;
    cmd.args[0] = GL_FOG_DENSITY; cmd.args[1] = 0.25f;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_FOG_FV; cmd.num_args = 5;
    cmd.args[0] = GL_FOG_COLOR;
    cmd.args[1] = 0.25f; cmd.args[2] = 0.5f; cmd.args[3] = 1; cmd.args[4] = 1;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_STENCIL_FUNC; cmd.num_args = 3;
    cmd.args[0] = GL_GREATER; cmd.args[1] = 123; cmd.args[2] = 0xF0;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_STENCIL_OP; cmd.num_args = 3;
    cmd.args[0] = GL_INVERT; cmd.args[1] = GL_DECR; cmd.args[2] = GL_INCR;
    repl_apply_state_cmd(&cmd, 1.0f);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_STENCIL_MASK; cmd.num_args = 1;
    cmd.args[0] = 0x7F;
    repl_apply_state_cmd(&cmd, 1.0f);

    gl_stub_trace_close();

    char buf[4096] = "";
    FILE *f = fopen(path, "r");
    ASSERT_TRUE("enum trace file opened", f != NULL);
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }

    char want[128];
    snprintf(want, sizeof(want), "glBlendFunc %u %u\n",
             (unsigned)GL_SRC_ALPHA, (unsigned)GL_ONE_MINUS_SRC_ALPHA);
    ASSERT_TRUE("glBlendFunc receives (sfactor, dfactor) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glColorMask %u %u %u %u\n",
             (unsigned)GL_TRUE, (unsigned)GL_FALSE,
             (unsigned)GL_TRUE, (unsigned)GL_FALSE);
    ASSERT_TRUE("glColorMask receives 4 channels in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glDepthMask %u\n", (unsigned)GL_FALSE);
    ASSERT_TRUE("glDepthMask receives flag", strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glEdgeFlag %u\n", (unsigned)GL_FALSE);
    ASSERT_TRUE("glEdgeFlag receives flag", strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glColorMaterial %u %u\n",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_AMBIENT_AND_DIFFUSE);
    ASSERT_TRUE("glColorMaterial receives (face, mode) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glClipPlane %u 0.25 1 0 0.5\n",
             (unsigned)GL_CLIP_PLANE1);
    ASSERT_TRUE("glClipPlane receives (plane, a, b, c, d) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glFogi %u %d\n",
             (unsigned)GL_FOG_MODE, (int)GL_EXP2);
    ASSERT_TRUE("glFogi receives (pname, mode) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glFogf %u 0.25\n",
             (unsigned)GL_FOG_DENSITY);
    ASSERT_TRUE("glFogf receives (pname, value) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glFogfv %u 0.25 0.5 1 1\n",
             (unsigned)GL_FOG_COLOR);
    ASSERT_TRUE("glFogfv receives (pname, r, g, b, a) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilFunc %u %d %u\n",
             (unsigned)GL_GREATER, 123, 0xF0u);
    ASSERT_TRUE("glStencilFunc receives (func, ref, mask) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilOp %u %u %u\n",
             (unsigned)GL_INVERT, (unsigned)GL_DECR, (unsigned)GL_INCR);
    ASSERT_TRUE("glStencilOp receives (sfail, dpfail, dppass) in order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilMask %u\n", 0x7Fu);
    ASSERT_TRUE("glStencilMask receives its write mask", strstr(buf, want) != NULL);
}

/* End-to-end P1 regression: drive the *full* path the real app uses -
 * source line -> parser -> commit -> flatten -> executor -> GL - and
 * assert the GL trace. test_enum_arg_gl_trace covers only the executor
 * half (hand-built GLCmd); this also pins parser arg ORDER into args[]
 * for the multi-arg enum commands, so a future args[] arg-shift on
 * either side is caught. Also pins glDepthMask(1) numeric -> GL_TRUE
 * end-to-end (bool-slot policy). */
static void test_enum_arg_end_to_end_trace(void) {
    repl_executor_init_resources();
    glr_ctrl_reset_all();

    char path[] = "/tmp/test_repl_executor_enum_e2e.txt";
    gl_stub_trace_open(path);

    editor_feed_line("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);");
    editor_feed_line("glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);");
    editor_feed_line("glDepthMask(1);");
    editor_feed_line("glEdgeFlag(1);");
    editor_feed_line("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
    editor_feed_line("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);");
    editor_feed_line("glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glStencilFunc(GL_GREATER, 7.9, 0xFE);");
    editor_feed_line("glStencilOp(GL_KEEP, GL_DECR, GL_INCR);");
    editor_feed_line("glStencilMask(0x7F);");
    editor_feed_line("glClearStencil(9.7);");
    repl_flatten_commands(editor_state_edit_line());
    repl_execute_commands();

    gl_stub_trace_close();

    char buf[8192] = "";
    FILE *f = fopen(path, "r");
    ASSERT_TRUE("e2e trace file opened", f != NULL);
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }

    char want[160];
    snprintf(want, sizeof(want), "glBlendFunc %u %u\n",
             (unsigned)GL_SRC_ALPHA, (unsigned)GL_ONE_MINUS_SRC_ALPHA);
    ASSERT_TRUE("e2e glBlendFunc(sfactor, dfactor) order preserved",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glColorMask %u %u %u %u\n",
             (unsigned)GL_TRUE, (unsigned)GL_FALSE,
             (unsigned)GL_TRUE, (unsigned)GL_FALSE);
    ASSERT_TRUE("e2e glColorMask 4-channel order preserved",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glDepthMask %u\n", (unsigned)GL_TRUE);
    ASSERT_TRUE("e2e glDepthMask(1) -> GL_TRUE", strstr(buf, want) != NULL);

    /* glEdgeFlag(1) must reach GL as GL_TRUE (bool-slot canonicalize). */
    snprintf(want, sizeof(want), "glEdgeFlag %u\n", (unsigned)GL_TRUE);
    ASSERT_TRUE("e2e glEdgeFlag(1) -> GL_TRUE", strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glColorMaterial %u %u\n",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_AMBIENT_AND_DIFFUSE);
    ASSERT_TRUE("e2e glColorMaterial(face, mode) order preserved",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glLightModeli %u %d\n",
             (unsigned)GL_LIGHT_MODEL_TWO_SIDE, (int)GL_TRUE);
    ASSERT_TRUE("e2e glLightModeli(pname, param) order preserved",
                strstr(buf, want) != NULL);

    /* The bitfield slot: both tokens must reach GL as one OR'd mask,
     * not as the last-wins single bit. */
    snprintf(want, sizeof(want), "glClear %u\n",
             (unsigned)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ASSERT_TRUE("e2e glClear(A | B) reaches GL as the OR'd mask",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilFunc %u %d %u\n",
             (unsigned)GL_GREATER, 7, 0xFEu);
    ASSERT_TRUE("e2e glStencilFunc truncates ref and preserves arg order",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilOp %u %u %u\n",
             (unsigned)GL_KEEP, (unsigned)GL_DECR, (unsigned)GL_INCR);
    ASSERT_TRUE("e2e glStencilOp preserves all three enum slots",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glStencilMask %u\n", 0x7Fu);
    ASSERT_TRUE("e2e glStencilMask preserves write mask", strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glClearStencil %d\n", 9);
    ASSERT_TRUE("e2e glClearStencil truncates its value like C does",
                strstr(buf, want) != NULL);
}

static void test_execute_edge_cases(void) {
    repl_execute_program(NULL);

    /* Zero-initialized, per the ReplExecutionOptions contract: the executor
     * reads every field, including pointers it calls or writes through
     * (state_filter, status_out, observation_out). What this case is actually
     * about is the negative counts below. */
    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = -1;
    opts.program.cmds = NULL;
    opts.program.cmd_count = -1;
    opts.program.local_vars = NULL;
    repl_execute_program(&opts);

    repl_execute_commands();
}

static void test_exec_cursor_step_tracks_state(void) {
    repl_executor_init_resources();

    GLCmd cmds[7];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_PUSH_MATRIX;         cmds[0].valid = 1;
    cmds[1].type = CMD_BEGIN;               cmds[1].valid = 1; cmds[1].args[0] = GL_TRIANGLES;
    cmds[2].type = CMD_END;                 cmds[2].valid = 1;
    cmds[3].type = CMD_TESS_BEGIN_POLYGON;  cmds[3].valid = 1;
    cmds[4].type = CMD_TESS_BEGIN_CONTOUR;  cmds[4].valid = 1;
    cmds[5].type = CMD_TESS_END;            cmds[5].valid = 1;
    cmds[6].type = CMD_TESS_END;            cmds[6].valid = 1;

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = 7;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 7;

    gl_stub_counts_reset();
    ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
    ASSERT_TRUE("cursor starts active", !repl_exec_cursor_done(&cursor));
    ASSERT_TRUE("cursor peek returns first command",
                repl_exec_cursor_peek(&cursor) == &cmds[0]);

    ASSERT_TRUE("cursor step push returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step push advances pc", cursor.pc == 1);
    ASSERT_TRUE("cursor step push tracks matrix depth",
                cursor.matrix_depth == 1);
    ASSERT_TRUE("cursor step push reaches GL",
                gl_stub_counts[GL_STUB_glPushMatrix] == 1);

    ASSERT_TRUE("cursor peek returns begin command",
                repl_exec_cursor_peek(&cursor) == &cmds[1]);
    ASSERT_TRUE("cursor step begin returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step begin tracks in_begin", cursor.in_begin == 1);
    ASSERT_TRUE("cursor step begin reaches GL",
                gl_stub_counts[GL_STUB_glBegin] == 1);

    ASSERT_TRUE("cursor step end returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step end clears in_begin", cursor.in_begin == 0);
    ASSERT_TRUE("cursor step end reaches GL",
                gl_stub_counts[GL_STUB_glEnd] == 1);

    ASSERT_TRUE("cursor step tess polygon returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step tess polygon tracks depth",
                cursor.tess_depth == 1);
    ASSERT_TRUE("cursor step tess polygon reaches GLU",
                gl_stub_counts[GL_STUB_gluTessBeginPolygon] == 1);

    ASSERT_TRUE("cursor step tess contour returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step tess contour tracks depth",
                cursor.tess_depth == 2);
    ASSERT_TRUE("cursor step tess contour reaches GLU",
                gl_stub_counts[GL_STUB_gluTessBeginContour] == 1);

    ASSERT_TRUE("cursor step tess end contour returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step tess end contour tracks depth",
                cursor.tess_depth == 1);
    ASSERT_TRUE("cursor step tess end contour reaches GLU",
                gl_stub_counts[GL_STUB_gluTessEndContour] == 1);

    ASSERT_TRUE("cursor step tess end polygon returns true",
                repl_exec_cursor_step(&cursor) == 1);
    ASSERT_TRUE("cursor step tess end polygon tracks depth",
                cursor.tess_depth == 0);
    ASSERT_TRUE("cursor step tess end polygon reaches GLU",
                gl_stub_counts[GL_STUB_gluTessEndPolygon] == 1);
    ASSERT_TRUE("cursor done after final command",
                repl_exec_cursor_done(&cursor));
    ASSERT_TRUE("cursor peek returns NULL when done",
                repl_exec_cursor_peek(&cursor) == NULL);
    ASSERT_TRUE("cursor step returns false when done",
                repl_exec_cursor_step(&cursor) == 0);

    repl_exec_cursor_end(&cursor);
    ASSERT_TRUE("cursor end unwinds tracked push",
                gl_stub_counts[GL_STUB_glPopMatrix] == 1);
    ASSERT_TRUE("cursor end does not re-finalize closed tess polygon",
                gl_stub_counts[GL_STUB_gluTessEndPolygon] == 1);

    repl_executor_destroy_resources();
}

static void test_exec_cursor_advance_skips_without_state(void) {
    repl_executor_init_resources();

    GLCmd cmds[3];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_PUSH_MATRIX;        cmds[0].valid = 1;
    cmds[1].type = CMD_BEGIN;              cmds[1].valid = 1; cmds[1].args[0] = GL_TRIANGLES;
    cmds[2].type = CMD_TESS_BEGIN_POLYGON; cmds[2].valid = 1;

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = 3;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 3;

    gl_stub_counts_reset();
    ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
    ASSERT_TRUE("advance cursor starts active",
                !repl_exec_cursor_done(&cursor));

    ASSERT_TRUE("advance cursor initially sees push",
                repl_exec_cursor_peek(&cursor) == &cmds[0]);
    repl_exec_cursor_advance(&cursor);
    ASSERT_TRUE("advance over push increments pc only", cursor.pc == 1);
    ASSERT_TRUE("advance over push leaves matrix depth untouched",
                cursor.matrix_depth == 0);
    ASSERT_TRUE("advance over push does not call glPushMatrix",
                gl_stub_counts[GL_STUB_glPushMatrix] == 0);

    ASSERT_TRUE("advance cursor now sees begin",
                repl_exec_cursor_peek(&cursor) == &cmds[1]);
    repl_exec_cursor_advance(&cursor);
    ASSERT_TRUE("advance over begin increments pc only", cursor.pc == 2);
    ASSERT_TRUE("advance over begin leaves in_begin untouched",
                cursor.in_begin == 0);
    ASSERT_TRUE("advance over begin does not call glBegin",
                gl_stub_counts[GL_STUB_glBegin] == 0);

    ASSERT_TRUE("advance cursor now sees tess begin",
                repl_exec_cursor_peek(&cursor) == &cmds[2]);
    repl_exec_cursor_advance(&cursor);
    ASSERT_TRUE("advance over tess begin reaches done",
                repl_exec_cursor_done(&cursor));
    ASSERT_TRUE("advance over tess begin leaves tess depth untouched",
                cursor.tess_depth == 0);
    ASSERT_TRUE("advance over tess begin does not call GLU",
                gl_stub_counts[GL_STUB_gluTessBeginPolygon] == 0);
    ASSERT_TRUE("advance cursor peek returns NULL when done",
                repl_exec_cursor_peek(&cursor) == NULL);

    repl_exec_cursor_end(&cursor);
    ASSERT_TRUE("advance cursor end has no begin to close",
                gl_stub_counts[GL_STUB_glEnd] == 0);
    ASSERT_TRUE("advance cursor end has no matrix to unwind",
                gl_stub_counts[GL_STUB_glPopMatrix] == 0);
    ASSERT_TRUE("advance cursor end has no tess to finalize",
                gl_stub_counts[GL_STUB_gluTessEndPolygon] == 0);

    repl_executor_destroy_resources();
}

static void test_execute_all_commands(void) {
    repl_executor_init_resources();

    GLCmd cmds[CMD_TYPE_COUNT + 20];
    memset(cmds, 0, sizeof(cmds));

    int count = 0;
    // Add all command types
    for (int i = 0; i < CMD_TYPE_COUNT; i++) {
        cmds[count].type = (CmdType)i;
        cmds[count].valid = 1;
        cmds[count].num_args = 5;
        for (int j = 0; j < 5; j++) cmds[count].args[j] = 1.0f;
        // Avoid skipping if block body
        if (cmds[count].type == CMD_IF_BEGIN) cmds[count].args[0] = 1.0f;
        count++;
    }

    // Add specific tessellation sequence
    cmds[count].type = CMD_TESS_BEGIN_POLYGON; cmds[count].valid = 1; count++;
    cmds[count].type = CMD_TESS_BEGIN_CONTOUR; cmds[count].valid = 1; count++;
    cmds[count].type = CMD_TESS_VERTEX; cmds[count].valid = 1; count++;
    cmds[count].type = CMD_TESS_END; cmds[count].valid = 1; count++; // ends contour
    cmds[count].type = CMD_TESS_END; cmds[count].valid = 1; count++; // ends polygon

    // Add IF_BEGIN with false condition
    cmds[count].type = CMD_IF_BEGIN; cmds[count].valid = 1; cmds[count].args[0] = 0.0f; count++;
    cmds[count].type = CMD_VERTEX3F; cmds[count].valid = 1; count++; // skipped
    cmds[count].type = CMD_IF_END; cmds[count].valid = 1; count++;

    // Add IF_BEGIN and VAR_ASSIGN with has_vars
    // Set up editor buffer entries so execution_flat_text() can resolve text.
    editor_buffer_set_line(0, "if (1.0) {");
    editor_buffer_set_line(1, "x = 5.0;");
    editor_buffer_set_line(2, "goto skip;");
    editor_buffer_set_line(3, "label skip;");
    repl_state_document_count_set(4);

    cmds[count].type = CMD_IF_BEGIN; cmds[count].valid = 1; cmds[count].has_vars = 1;
    cmds[count].src_cmd_idx = 0; count++;
    cmds[count].type = CMD_IF_END; cmds[count].valid = 1; count++;

    cmds[count].type = CMD_VAR_ASSIGN; cmds[count].valid = 1; cmds[count].has_vars = 1; cmds[count].var_idx = 0;
    cmds[count].src_cmd_idx = 1; count++;

    // Add GOTO to jump over a command
    cmds[count].type = CMD_GOTO; cmds[count].valid = 1;
    cmds[count].src_cmd_idx = 2; count++;
    cmds[count].type = CMD_VERTEX3F; cmds[count].valid = 1; count++; // skipped
    cmds[count].type = CMD_GOTO_LABEL; cmds[count].valid = 1;
    cmds[count].src_cmd_idx = 3; count++;

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = count;
    opts.program.cmds = cmds;
    opts.program.cmd_count = count;
    opts.program.local_vars = NULL;

    repl_execute_program(&opts);

    repl_executor_destroy_resources();
}

static void test_glut_bitmap_string(void) {
    /* `label("hi")` with no substitution: 2 chars emitted; the
     * primitive itself does NOT call glRasterPos3f - that's the
     * user's responsibility via a preceding glRasterPos3f command. */
    repl_executor_init_resources();
    gl_stub_counts_reset();

    GLCmd cmds[2];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_LABEL;
    cmds[0].valid = 1;
    cmds[0].num_args = 0;
    strcpy(cmds[0].payload.label.fmt, "hi");

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = 1;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 1;
    repl_execute_program(&opts);

    ASSERT_TRUE("label does NOT touch raster pos",
                gl_stub_counts[GL_STUB_glRasterPos3f] == 0);
    ASSERT_TRUE("label emits 2 chars",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] == 2);

    /* CMD_RASTER_POS3F followed by label: raster pos is set by the
     * separate primitive, label emits chars at that position. */
    gl_stub_counts_reset();
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_RASTER_POS3F;
    cmds[0].valid = 1;
    cmds[0].args[0] = 0.5f;
    cmds[0].args[1] = 1.5f;
    cmds[0].args[2] = 2.5f;
    cmds[0].num_args = 3;
    cmds[1].type = CMD_LABEL;
    cmds[1].valid = 1;
    cmds[1].num_args = 0;
    strcpy(cmds[1].payload.label.fmt, "hi");
    opts.flat_cmd_count = 2;
    opts.program.cmd_count = 2;
    repl_execute_program(&opts);

    ASSERT_TRUE("glRasterPos3f primitive fires once",
                gl_stub_counts[GL_STUB_glRasterPos3f] == 1);
    ASSERT_TRUE("label after raster-pos still emits 2 chars",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] == 2);

    /* Format with %f: substitution is applied at execute time. The
     * expanded text "v=1.25" is 6 chars. args[0..3] hold the
     * substitution values. */
    gl_stub_counts_reset();
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_LABEL;
    cmds[0].valid = 1;
    cmds[0].args[0] = 1.25f;
    cmds[0].num_args = 1;
    strcpy(cmds[0].payload.label.fmt, "v=%f");
    opts.flat_cmd_count = 1;
    opts.program.cmd_count = 1;
    repl_execute_program(&opts);

    ASSERT_TRUE("label %f substitution emits expanded length",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] == 6);

    /* %% renders as a single literal '%'. */
    gl_stub_counts_reset();
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_LABEL;
    cmds[0].valid = 1;
    cmds[0].num_args = 0;
    strcpy(cmds[0].payload.label.fmt, "100%% done");

    repl_execute_program(&opts);

    ASSERT_TRUE("label %% literal collapses to single '%'",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] == 9);

    repl_executor_destroy_resources();
}

/* Step 7e regression: the executor consumes camera distance for the
 * point-size fallback through a controller-installed callback, not by
 * reaching into glr_camera directly. The pipeline TU must compile +
 * link without glr_camera.h or glr_camera.c. Also pins the runtime
 * point-parameter capability behavior that replaced the compile-time
 * NO_POINT_PARAMETER macro (and the behavioral value of the deleted
 * check-no-point-parameter-builds.sh). */
static float g_test_camera_distance_value = 0.0f;
static int   g_test_camera_distance_calls = 0;

static float test_camera_distance_source(void) {
    g_test_camera_distance_calls++;
    return g_test_camera_distance_value;
}

static void test_executor_camera_distance_source(void) {
    /* Default state: no source installed. */
    repl_executor_install_camera_distance_source(NULL);
    repl_executor_install_point_parameter_proc(NULL);

    /* Runtime point-parameter capability (replaces the compile-time
     * NO_POINT_PARAMETER macro). Two cases, observed through the
     * public surface. */
    GLCmd pp = {0};
    pp.type = CMD_POINT_PARAMETER_FV;
    pp.args[0] = GL_POINT_DISTANCE_ATTENUATION;
    pp.args[1] = 1.0f; pp.args[2] = 0.0f; pp.args[3] = 0.02f;
    pp.num_args = 4;

    /* (a) Unsupported: CMD_POINT_PARAMETER_FV must NOT reach
     * glPointParameterfv, and the point-size path consults the
     * camera-distance source to scale the visual fallback. */
    repl_executor_set_point_parameter_supported(0);
    gl_stub_counts_reset();
    repl_apply_state_cmd(&pp, 1.0f);
    ASSERT_TRUE("unsupported: CMD_POINT_PARAMETER_FV does not call glPointParameterfv",
                gl_stub_counts[GL_STUB_glPointParameterfv] == 0);
    g_test_camera_distance_calls = 0;
    g_test_camera_distance_value = 4.0f;
    repl_executor_install_camera_distance_source(test_camera_distance_source);
    repl_exec_point_size(2.0f);
    ASSERT_TRUE("unsupported: point-size fallback consulted the camera-distance source",
                g_test_camera_distance_calls == 1);
    repl_executor_install_camera_distance_source(NULL);
    repl_exec_point_size(2.0f);  /* must not crash with no source */

    /* (b) Supported but no loaded proc: the executor must NOT fall back
     * to a direct glPointParameterfv symbol call. */
    repl_executor_set_point_parameter_supported(1);
    gl_stub_counts_reset();
    repl_apply_state_cmd(&pp, 1.0f);
    ASSERT_TRUE("supported without proc: CMD_POINT_PARAMETER_FV does not call glPointParameterfv",
                gl_stub_counts[GL_STUB_glPointParameterfv] == 0);

    /* (c) Supported with a loaded proc: CMD_POINT_PARAMETER_FV calls
     * through it, and the point-size path passes sz straight through
     * without consulting the camera-distance source. */
    repl_executor_install_point_parameter_proc((ReplExecutorPointParameterProc)glPointParameterfv);
    gl_stub_counts_reset();
    repl_apply_state_cmd(&pp, 1.0f);
    ASSERT_TRUE("supported with proc: CMD_POINT_PARAMETER_FV calls glPointParameterfv",
                gl_stub_counts[GL_STUB_glPointParameterfv] == 1);
    g_test_camera_distance_calls = 0;
    repl_executor_install_camera_distance_source(test_camera_distance_source);
    repl_exec_point_size(2.0f);
    ASSERT_TRUE("supported with proc: point-size does not consult the camera-distance source",
                g_test_camera_distance_calls == 0);
    repl_executor_install_camera_distance_source(NULL);

    /* Restore default for subsequent tests. */
    repl_executor_install_point_parameter_proc((ReplExecutorPointParameterProc)glPointParameterfv);
    repl_executor_set_point_parameter_supported(1);
}

/* Pin down two matrix-stack invariants the executor maintains on
 * behalf of the user. User-program GL calls can leave the modelview
 * stack imbalanced (extra pushes, or extra pops), but:
 *
 *   (a) the executor unwinds any net push imbalance at frame end so
 *       state doesn't leak across frames, and
 *   (b) a stray glPopMatrix is clamped before it reaches GL - the
 *       tracked-pop refuses to call glPopMatrix when its depth
 *       counter is at 0, preventing a GL_STACK_UNDERFLOW.
 *
 * Both behaviours are why render3d/render's outer push/pop bracket
 * around the user program is sufficient - the executor cooperates by
 * leaving the inner stack net-zero. Without these invariants, a
 * user's first frame with stray pushes would silently corrupt the
 * outer (camera_view) frame and every subsequent overlay would
 * render at the wrong transform. */
static void test_executor_matrix_balance_unwind(void) {
    repl_executor_init_resources();
    gl_stub_counts_reset();

    GLCmd cmds[4];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_PUSH_MATRIX; cmds[0].valid = 1;
    cmds[1].type = CMD_PUSH_MATRIX; cmds[1].valid = 1;
    cmds[2].type = CMD_PUSH_MATRIX; cmds[2].valid = 1;
    cmds[3].type = CMD_POP_MATRIX;  cmds[3].valid = 1;

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = 4;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 4;
    repl_execute_program(&opts);

    /* 3 user pushes + 1 user pop, plus the unwind that closes the
     * remaining 2 outstanding pushes -> expect 3 push, 3 pop calls. */
    ASSERT_TRUE("3 pushes recorded",
                gl_stub_counts[GL_STUB_glPushMatrix] == 3);
    ASSERT_TRUE("3 pops total (1 user + 2 unwind)",
                gl_stub_counts[GL_STUB_glPopMatrix] == 3);
}

static void test_executor_matrix_pop_underflow_clamped(void) {
    repl_executor_init_resources();
    gl_stub_counts_reset();

    GLCmd cmds[3];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_PUSH_MATRIX; cmds[0].valid = 1;
    cmds[1].type = CMD_POP_MATRIX;  cmds[1].valid = 1;
    cmds[2].type = CMD_POP_MATRIX;  cmds[2].valid = 1;  /* clamped */

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = 3;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 3;
    repl_execute_program(&opts);

    ASSERT_TRUE("1 push recorded",
                gl_stub_counts[GL_STUB_glPushMatrix] == 1);
    /* Second pop is clamped at depth=0, so only one glPopMatrix
     * actually reaches GL. Unwind has nothing to do. */
    ASSERT_TRUE("1 pop recorded (second was clamped)",
                gl_stub_counts[GL_STUB_glPopMatrix] == 1);
}

/* repl_executor_draw_glut_solid dispatches one glutSolid* call per
 * shape type and is a no-op for any other type. Shared by the live
 * render loop and the outline overlay's GL_LINE wireframe redraw, so
 * the dispatch must stay exact. */
static void test_draw_glut_solid_dispatch(void) {
    struct { CmdType type; int stub_idx; const char *name; } cases[] = {
        { CMD_GLUT_TORUS,  GL_STUB_glutSolidTorus,  "torus"  },
        { CMD_GLUT_CUBE,   GL_STUB_glutSolidCube,   "cube"   },
        { CMD_GLUT_SPHERE, GL_STUB_glutSolidSphere, "sphere" },
        { CMD_GLUT_TEAPOT, GL_STUB_glutSolidTeapot, "teapot" },
        { CMD_GLUT_CONE,   GL_STUB_glutSolidCone,   "cone"   },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        gl_stub_counts_reset();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = cases[i].type;
        cmd.valid = 1;
        repl_executor_draw_glut_solid(&cmd);

        char label[64];
        snprintf(label, sizeof label, "draw %s emits its glutSolid* call",
                 cases[i].name);
        ASSERT_TRUE(label, gl_stub_counts[cases[i].stub_idx] == 1);

        /* No cross-talk: exactly one glutSolid* call total. */
        unsigned long long others =
            gl_stub_counts[GL_STUB_glutSolidTorus] +
            gl_stub_counts[GL_STUB_glutSolidCube] +
            gl_stub_counts[GL_STUB_glutSolidSphere] +
            gl_stub_counts[GL_STUB_glutSolidTeapot] +
            gl_stub_counts[GL_STUB_glutSolidCone];
        snprintf(label, sizeof label, "draw %s emits no other shape",
                 cases[i].name);
        ASSERT_TRUE(label, others == 1);
    }

    /* No-op for non-glut and NULL. */
    gl_stub_counts_reset();
    GLCmd vtx;
    memset(&vtx, 0, sizeof(vtx));
    vtx.type = CMD_VERTEX3F;
    vtx.valid = 1;
    repl_executor_draw_glut_solid(&vtx);
    repl_executor_draw_glut_solid(NULL);
    unsigned long long any =
        gl_stub_counts[GL_STUB_glutSolidTorus] +
        gl_stub_counts[GL_STUB_glutSolidCube] +
        gl_stub_counts[GL_STUB_glutSolidSphere] +
        gl_stub_counts[GL_STUB_glutSolidTeapot] +
        gl_stub_counts[GL_STUB_glutSolidCone];
    ASSERT_TRUE("non-glut / NULL draws nothing", any == 0);
}

/* Regression: light_enabled_mask feeds only the light-indicator overlay.
 * It must track what the current program does (glEnable(GL_LIGHTn)),
 * not stay stuck at the default-on / last-run value. repl_execute_program
 * resets it each walk; this guards that reset + selective re-enable, plus
 * the glDisable path. The dimensional light data is app-owned now, so the
 * executor touches only this bitmask. */
static void run_one_cmd(CmdType type, double arg0) {
    GLCmd cmds[1];
    ReplExecutionOptions opts = {0};
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = type;
    cmds[0].args[0] = arg0;
    cmds[0].num_args = (type == CMD_VERTEX3F) ? 3 : 1;
    cmds[0].valid = 1;
    opts.flat_cmd_count = 1;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 1;
    repl_execute_program(&opts);
}

static void test_light_indicator_tracks_program(void) {
    ReplRenderState rs;

    /* Simulate the old sticky / default-on bookkeeping: all four slots
     * left enabled from a previous run. */
    repl_state_render_mut()->light_enabled_mask = 0xFu;

    /* Program enables only GL_LIGHT0. */
    run_one_cmd(CMD_ENABLE, GL_LIGHT0);

    rs = repl_state_render();
    ASSERT_TRUE("GL_LIGHT0 enabled by program",
                repl_light_enabled(rs.light_enabled_mask, 0) == 1);
    ASSERT_TRUE("GL_LIGHT1 reset off (program did not enable)",
                repl_light_enabled(rs.light_enabled_mask, 1) == 0);
    ASSERT_TRUE("GL_LIGHT2 reset off (program did not enable)",
                repl_light_enabled(rs.light_enabled_mask, 2) == 0);
    ASSERT_TRUE("GL_LIGHT3 reset off",
                repl_light_enabled(rs.light_enabled_mask, 3) == 0);

    /* glDisable(GL_LIGHT0) within the same program clears just that bit. */
    {
        GLCmd cmds[2];
        ReplExecutionOptions opts = {0};
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_ENABLE;  cmds[0].args[0] = GL_LIGHT2;
        cmds[0].num_args = 1;       cmds[0].valid = 1;
        cmds[1].type = CMD_ENABLE;  cmds[1].args[0] = GL_LIGHT0;
        cmds[1].num_args = 1;       cmds[1].valid = 1;
        opts.flat_cmd_count = 2;
        opts.program.cmds = cmds;
        opts.program.cmd_count = 2;
        repl_execute_program(&opts);
        rs = repl_state_render();
        ASSERT_TRUE("enable LIGHT0+LIGHT2 -> mask 0b0101",
                    rs.light_enabled_mask == 0x5u);

        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_ENABLE;   cmds[0].args[0] = GL_LIGHT0;
        cmds[0].num_args = 1;        cmds[0].valid = 1;
        cmds[1].type = CMD_DISABLE;  cmds[1].args[0] = GL_LIGHT0;
        cmds[1].num_args = 1;        cmds[1].valid = 1;
        opts.flat_cmd_count = 2;
        opts.program.cmds = cmds;
        opts.program.cmd_count = 2;
        repl_execute_program(&opts);
        rs = repl_state_render();
        ASSERT_TRUE("enable then disable LIGHT0 -> mask 0",
                    rs.light_enabled_mask == 0u);
    }

    /* Re-enable, then a program with no light commands must clear the
     * previously-enabled light (the sticky bug). */
    run_one_cmd(CMD_ENABLE, GL_LIGHT0);
    run_one_cmd(CMD_VERTEX3F, 0.0);

    rs = repl_state_render();
    ASSERT_TRUE("GL_LIGHT0 cleared when next program omits glEnable",
                repl_light_enabled(rs.light_enabled_mask, 0) == 0);
}

/* repl_apply_state_bookkeeping(): the single source of truth for the REPL
 * render-state side effects a state command carries (GL_LIGHTn enable mask,
 * clear color), with NO GL emitted. repl_apply_state_cmd() folds it in; the
 * hidden-line wireframe pass (which owns GL state and skips user state) calls
 * it directly, so this locks the shared contract. */
static void test_apply_state_bookkeeping(void) {
    GLCmd cmd;
    ReplRenderState rs;

    /* Enable sets the matching light-slot bit; nothing else. */
    repl_state_render_mut()->light_enabled_mask = 0u;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ENABLE; cmd.num_args = 1; cmd.args[0] = GL_LIGHT2;
    repl_apply_state_bookkeeping(&cmd);
    rs = repl_state_render();
    ASSERT_TRUE("bookkeeping enable sets GL_LIGHT2 mask bit",
                repl_light_enabled(rs.light_enabled_mask, 2) == 1);
    ASSERT_TRUE("bookkeeping enable leaves other slots off",
                repl_light_enabled(rs.light_enabled_mask, 0) == 0);

    /* Disable clears it. */
    cmd.type = CMD_DISABLE;
    repl_apply_state_bookkeeping(&cmd);
    rs = repl_state_render();
    ASSERT_TRUE("bookkeeping disable clears GL_LIGHT2 mask bit",
                repl_light_enabled(rs.light_enabled_mask, 2) == 0);

    /* Non-light enable (e.g. GL_DEPTH_TEST) touches no slot. */
    repl_state_render_mut()->light_enabled_mask = 0u;
    cmd.type = CMD_ENABLE; cmd.args[0] = GL_DEPTH_TEST;
    repl_apply_state_bookkeeping(&cmd);
    ASSERT_TRUE("bookkeeping ignores non-light enable",
                repl_state_render().light_enabled_mask == 0u);

    /* A clear color carries NO bookkeeping. This helper also runs for commands
     * whose GL a pass is skipping, so anything folded in here would describe
     * pixels nothing wrote - the background is observed by the cursor, paired
     * with the emission (repl_exec_cursor_emit_clear_color). */
    repl_state_render_mut()->light_enabled_mask = 0x3u;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_CLEAR_COLOR; cmd.num_args = 4;
    cmd.args[0] = 0.25f; cmd.args[1] = 0.5f; cmd.args[2] = 0.75f; cmd.args[3] = 1.0f;
    repl_apply_state_bookkeeping(&cmd);
    ASSERT_TRUE("bookkeeping carries nothing for a clear color",
                repl_state_render().light_enabled_mask == 0x3u);

    /* A command that carries no bookkeeping is a no-op (no crash, no state). */
    repl_state_render_mut()->light_enabled_mask = 0x5u;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_BLEND_FUNC; cmd.num_args = 2;
    cmd.args[0] = GL_SRC_ALPHA; cmd.args[1] = GL_ONE_MINUS_SRC_ALPHA;
    repl_apply_state_bookkeeping(&cmd);
    ASSERT_TRUE("bookkeeping no-op leaves mask untouched",
                repl_state_render().light_enabled_mask == 0x5u);

    /* NULL is safe. */
    repl_apply_state_bookkeeping(NULL);

#ifdef GL_STUBS
    /* Bookkeeping emits NO GL - only repl_apply_state_cmd() does that. */
    gl_stub_counts_reset();
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_ENABLE; cmd.num_args = 1; cmd.args[0] = GL_LIGHT0;
    repl_apply_state_bookkeeping(&cmd);
    cmd.type = CMD_CLEAR_COLOR; cmd.num_args = 4;
    repl_apply_state_bookkeeping(&cmd);
    ASSERT_TRUE("bookkeeping emits no glEnable",
                gl_stub_counts[GL_STUB_glEnable] == 0);
    ASSERT_TRUE("bookkeeping emits no glClearColor",
                gl_stub_counts[GL_STUB_glClearColor] == 0);
#endif
}

/* The .ply export pass (encode_feedback_normals) must, vs. a normal live
 * render: (1) suppress the program's glEnable(GL_LIGHTING)/glEnable(GL_CULL_FACE)
 * so feedback captures the raw glColor material color and all faces, and
 * (2) emit a per-vertex glTexCoord3f world normal plus glPassThrough markers
 * bracketing the begin/end block. This is the regression guard for the bug
 * where a lit scene exported shaded (non-constant) colors. Stub-mode, so it
 * checks the call contract via gl_stub_counts (real feedback needs a display). */
static void test_export_normal_encoding(void) {
    printf("test_export_normal_encoding\n");
    repl_executor_init_resources();

    GLCmd cmds[8];
    memset(cmds, 0, sizeof(cmds));
    int n = 0;
    cmds[n].type = CMD_ENABLE;   cmds[n].valid = 1; cmds[n].num_args = 1; cmds[n].args[0] = GL_LIGHTING;  n++;
    cmds[n].type = CMD_ENABLE;   cmds[n].valid = 1; cmds[n].num_args = 1; cmds[n].args[0] = GL_CULL_FACE; n++;
    cmds[n].type = CMD_BEGIN;    cmds[n].valid = 1; cmds[n].num_args = 1; cmds[n].args[0] = GL_TRIANGLES; n++;
    cmds[n].type = CMD_NORMAL3F; cmds[n].valid = 1; cmds[n].num_args = 3; cmds[n].args[2] = 1.0f; n++;
    cmds[n].type = CMD_VERTEX3F; cmds[n].valid = 1; cmds[n].num_args = 3; n++;
    cmds[n].type = CMD_VERTEX3F; cmds[n].valid = 1; cmds[n].num_args = 3; cmds[n].args[0] = 1.0f; n++;
    cmds[n].type = CMD_VERTEX3F; cmds[n].valid = 1; cmds[n].num_args = 3; cmds[n].args[1] = 1.0f; n++;
    cmds[n].type = CMD_END;      cmds[n].valid = 1; n++;

    ReplExecutionOptions opts = {0};
    opts.program.cmds = cmds;
    opts.program.cmd_count = n;
    opts.flat_cmd_count = n;

    /* Live render: the program's two enables fire; no encoding emitted. */
    gl_stub_counts_reset();
    opts.encode_feedback_normals = 0;
    repl_execute_program(&opts);
    ASSERT_TRUE("live: program glEnable(lighting,cull) both fire",
                gl_stub_counts[GL_STUB_glEnable] == 2);
    ASSERT_TRUE("live: no normal texcoords", gl_stub_counts[GL_STUB_glTexCoord3f] == 0);
    ASSERT_TRUE("live: no passthrough markers", gl_stub_counts[GL_STUB_glPassThrough] == 0);

    /* Export: lighting+cull enables suppressed (raw color + all faces); each
     * vertex gets a texcoord normal; begin/end bracketed by 2 markers. */
    gl_stub_counts_reset();
    opts.encode_feedback_normals = 1;
    repl_execute_program(&opts);
    ASSERT_TRUE("export: lighting+cull enables SUPPRESSED",
                gl_stub_counts[GL_STUB_glEnable] == 0);
    ASSERT_TRUE("export: 3 vertices get a texcoord normal",
                gl_stub_counts[GL_STUB_glTexCoord3f] == 3);
    ASSERT_TRUE("export: begin/end bracketed by 2 passthrough markers",
                gl_stub_counts[GL_STUB_glPassThrough] == 2);
}

static void test_orrery_phase_alignment(void) {
    printf("test_orrery_phase_alignment\n");

    int example_idx = -1;
    for (int i = 0; i < repl_example_count(); i++) {
        if (strcmp(repl_example_name(i), "Orrery (labels track 3D orbits)") == 0) {
            example_idx = i;
            break;
        }
    }
    ASSERT_TRUE("found orrery example", example_idx >= 0);
    const char *const *lines = repl_example_lines(example_idx);
    ASSERT_TRUE("example lines exists", lines != NULL);

    reset_repl();

    char err[256];

    /* Feed declarations and the planetKepler function from the example lines */
    int in_func = 0;
    int parsed_func = 0;
    int edit_line = 0;
    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];

        /* Skip leading spaces for parsing */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (strncmp(p, "static float", 12) == 0) {
            int ok = repl_load_apply_line(line, err, sizeof(err), &edit_line);
            ASSERT_TRUE("load static float", ok);
        } else if (!parsed_func && strncmp(p, "planetKepler(", 13) == 0) {
            in_func = 1;
            parsed_func = 1;
            int ok = repl_load_apply_line(line, err, sizeof(err), &edit_line);
            ASSERT_TRUE("load planetKepler signature", ok);
        } else if (in_func) {
            int ok = repl_load_apply_line(line, err, sizeof(err), &edit_line);
            ASSERT_TRUE("load planetKepler body line", ok);
            if (*p == '}') {
                in_func = 0;
            }
        }
    }

    /* Feed test calls */
    int ok;
    ok = repl_load_apply_line("planetKepler(1.00000261, 0.00000562, 0.01671123, -0.00004392, -0.00001531, -0.01294668, 100.46457166, 35999.37244981, 102.93768193, 0.32327364, 0, 0, 1.000, 0.3, 0.52, 0.95);", err, sizeof(err), &edit_line);
    ASSERT_TRUE("load Earth call", ok);
    ok = repl_load_apply_line("mon = px; day = pz;", err, sizeof(err), &edit_line);
    ASSERT_TRUE("load Earth save", ok);
    ok = repl_load_apply_line("planetKepler(1.52371034, 0.00001847, 0.09339410, 0.00007882, 1.84969142, -0.00813131, -4.55343205, 19140.30268499, -23.94362959, 0.44441088, 49.55953891, -0.29257343, 0.532, 0.88, 0.45, 0.26);", err, sizeof(err), &edit_line);
    ASSERT_TRUE("load Mars call", ok);

    /* Real-world opposition dates for Earth and Mars (2020 through 2040)
     * paired with their corresponding animation time 't' (in seconds).
     *
     * The J2000 epoch corresponds to t=0. The clock `t` maps to Julian centuries
     * since J2000.0 (`th`) inside planetKepler via: th = t * EARTH_RATE / TAU / 100.0.
     * Here, EARTH_RATE = 0.85 radians per year allows aligning the animation timeline
     * with physical calendars.
     *
     * Since an opposition physically occurs when Earth is directly collinear
     * between the Sun and Mars, their heliocentric ecliptic longitudes
     * must align. Evaluating NASA's secular orbital elements and rates at these
     * specific Julian centuries must therefore yield matching ecliptic longitudes
     * (i.e. atan2(z, x) phase difference close to 0 modulo 2*PI). */
    static const struct {
        int year, month, day;
        float t;
    } oppositions[] = {
        { 2020, 10, 13, 153.62776f },
        { 2022, 12, 8, 169.53495f },
        { 2025, 1, 16, 185.11832f },
        { 2027, 2, 19, 200.58026f },
        { 2029, 3, 25, 216.06244f },
        { 2031, 5, 4, 231.64581f },
        { 2033, 6, 27, 247.53276f },
        { 2035, 9, 15, 263.92566f },
        { 2037, 11, 19, 280.03522f },
        { 2040, 1, 2, 295.69955f }
    };

    for (int i = 0; i < 10; i++) {
        g_predef_vars_mut[0].value = oppositions[i].t;
        repl_execute_commands();

        /* Earth's px and pz were saved into mon and day.
         * Mars' px and pz are read directly since Mars runs last. */
        float ex_val = predef_val("mon");
        float ez_val = predef_val("day");
        float mx_val = predef_val("px");
        float mz_val = predef_val("pz");

        ASSERT_TRUE("positions are non-NaN", !isnan(ex_val) && !isnan(ez_val) && !isnan(mx_val) && !isnan(mz_val));

        float lon_earth = atan2f(ez_val, ex_val);
        float lon_mars = atan2f(mz_val, mx_val);
        float diff = lon_mars - lon_earth;
        while (diff > 3.14159265f) diff -= 2.0f * 3.14159265f;
        while (diff < -3.14159265f) diff += 2.0f * 3.14159265f;

        char msg[128];
        snprintf(msg, sizeof(msg), "opposition %d-%02d-%02d within tolerance: diff=%f deg",
                 oppositions[i].year, oppositions[i].month, oppositions[i].day, diff * 180.0f / 3.14159265f);
        ASSERT_TRUE(msg, fabsf(diff) < 0.0087f);
    }
}

/* glPushAttrib / glPopAttrib cursor semantics: pairing, orphan-pop safety,
 * end-of-cursor unwind, the REPL_ATTRIB_STACK_CAP real-GL cap while virtual
 * depth is unbounded, and the executor-side state restore (the light-enable
 * mask under GL_ENABLE_BIT, the clear colour + colour write mask under
 * GL_COLOR_BUFFER_BIT) - with and without suppress_attrib_gl.
 *
 * Returns the finished cursor by value: the clear-affecting state is the
 * cursor's own ReplClearScopedState, so the scoping assertions read it there
 * rather than through a render-state mirror. */
static ReplExecCursor run_attrib_cursor(GLCmd *cmds, int n, int suppress) {
    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = n;
    opts.program.cmds = cmds;
    opts.program.cmd_count = n;
    opts.suppress_attrib_gl = suppress;
    ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
    while (!repl_exec_cursor_done(&cursor))
        if (!repl_exec_cursor_step(&cursor))
            break;
    repl_exec_cursor_end(&cursor);
    return cursor;
}

/* goto labels are read out of the SourceTextView the caller handed in, bounded
 * by that view's own line_count. The executor used to bound it through the
 * live repl_state_document_count() instead, so a temporary program over a
 * temporary text view resolved its jumps only when unrelated global document
 * state happened to agree - tests had to synchronize both counts by hand. */
static void test_goto_uses_caller_text_view(void) {
    static char lines[3][MAX_LINE_LEN];
    GLCmd cmds[3];
    SourceTextView text;
    ReplExecutionOptions opts = {0};
    ReplExecCursor cursor;
    int saved_doc_count = repl_state_document_count();

    printf("--- executor: goto resolves against the caller's text view ---\n");

    snprintf(lines[0], sizeof(lines[0]), "goto skip;");
    snprintf(lines[1], sizeof(lines[1]), "glColor3f(1, 0, 0);");
    snprintf(lines[2], sizeof(lines[2]), "skip:");
    text.lines = (const char (*)[MAX_LINE_LEN])lines;
    text.line_count = 3;

    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_GOTO;       cmds[0].valid = 1; cmds[0].src_cmd_idx = 0;
    cmds[1].type = CMD_COLOR3F;    cmds[1].valid = 1; cmds[1].src_cmd_idx = 1;
    cmds[1].num_args = 3;
    cmds[2].type = CMD_GOTO_LABEL; cmds[2].valid = 1; cmds[2].src_cmd_idx = 2;

    /* Deliberately left describing a different (here: empty) document. */
    repl_state_document_count_set(0);

    opts.flat_cmd_count = 3;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 3;
    opts.text = text;
    cursor = repl_exec_cursor_begin(&opts);
    repl_exec_cursor_step(&cursor);
    /* The jump lands on the label and the step's own advance moves past it. */
    ASSERT_TRUE("goto lands on its label with a caller-owned text view",
                cursor.pc == 3);
    repl_exec_cursor_end(&cursor);

    /* The view's own count is still the bound: a command pointing past it
     * reads no text, so the jump is simply not taken. */
    text.line_count = 1;
    opts.text = text;
    cursor = repl_exec_cursor_begin(&opts);
    repl_exec_cursor_step(&cursor);
    ASSERT_TRUE("a label past the view's line_count is not found",
                cursor.pc == 1);
    repl_exec_cursor_end(&cursor);

    repl_state_document_count_set(saved_doc_count);
}

/* --- Background observation ---------------------------------------------
 *
 * The background the host presents against is what the *executor* observed
 * itself clearing, not a prediction made by a second walk. These are
 * differential tests around the real cursor: feed one flat program to
 * repl_execute_program() and assert the observation produced by the commands
 * it actually visits. */

static GLCmd *obs_cmd(GLCmd *cmds, int idx, CmdType type, int src_line) {
    cmds[idx].type = type;
    cmds[idx].valid = 1;
    cmds[idx].src_cmd_idx = src_line;
    return &cmds[idx];
}

static void obs_clear_color(GLCmd *cmds, int idx, float r, float g,
                            float b, float a) {
    GLCmd *c = obs_cmd(cmds, idx, CMD_CLEAR_COLOR, -1);
    c->num_args = 4;
    c->args[0] = r; c->args[1] = g; c->args[2] = b; c->args[3] = a;
}

static void obs_color_mask(GLCmd *cmds, int idx, int r, int g, int b, int a) {
    GLCmd *c = obs_cmd(cmds, idx, CMD_COLOR_MASK, -1);
    c->num_args = 4;
    c->args[0] = (float)r; c->args[1] = (float)g;
    c->args[2] = (float)b; c->args[3] = (float)a;
}

static void obs_clear(GLCmd *cmds, int idx, GLbitfield mask) {
    GLCmd *c = obs_cmd(cmds, idx, CMD_CLEAR, -1);
    c->num_args = 1;
    c->args[0] = (float)mask;
}

/* Run a program under a poisoned sink, so a missing publication is a failure
 * rather than a lucky zero. */
static ReplBackgroundObservation run_observed(GLCmd *cmds, int n,
                                              ReplExecutionOptions *opts) {
    static const float k_baseline[4] = {0.10f, 0.10f, 0.10f, 1.0f};
    ReplBackgroundObservation obs;

    memset(&obs, 0x5A, sizeof(obs));
    opts->flat_cmd_count = n;
    opts->program.cmds = cmds;
    opts->program.cmd_count = n;
    opts->observation_out = &obs;
    memcpy(opts->baseline_clear_rgba, k_baseline,
           sizeof(opts->baseline_clear_rgba));
    repl_execute_program(opts);
    return obs;
}

static ReplBackgroundObservation run_observed_plain(GLCmd *cmds, int n) {
    ReplExecutionOptions opts = {0};
    return run_observed(cmds, n, &opts);
}

static int obs_rgba_is(const ReplBackgroundObservation *obs,
                       float r, float g, float b, float a) {
    return obs->rgba[0] == r && obs->rgba[1] == g &&
           obs->rgba[2] == b && obs->rgba[3] == a;
}

/* Suppresses exactly the two clear-affecting commands, standing in for a
 * render pass that owns its own colour state. */
static int obs_suppress_clears(CmdType type, const GLCmd *cmd, void *ud) {
    (void)cmd; (void)ud;
    return !(type == CMD_CLEAR_COLOR || type == CMD_CLEAR);
}

static void test_background_observation(void) {
    printf("--- executor: background observation ---\n");

    /* A clear with every colour channel masked off writes no colour pixels.
     * Reporting the clear colour here is exactly the defect a source-only
     * resolver could not see. */
    {
        GLCmd cmds[3];
        memset(cmds, 0, sizeof(cmds));
        obs_color_mask(cmds, 0, 0, 0, 0, 0);
        obs_clear_color(cmds, 1, 0.05f, 0.0f, 0.0f, 1.0f);
        obs_clear(cmds, 2, GL_COLOR_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 3);
        ASSERT_TRUE("fully masked clear establishes no background",
                    obs.known == 0);
    }

    /* A partial mask over an earlier full clear: the enabled channels take
     * the new colour, the masked ones keep the value AND the knownness the
     * first clear gave them, so the result is still fully known. */
    {
        GLCmd cmds[5];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.1f, 0.2f, 0.3f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);
        obs_color_mask(cmds, 2, 1, 0, 0, 1);
        obs_clear_color(cmds, 3, 0.9f, 0.9f, 0.9f, 0.5f);
        obs_clear(cmds, 4, GL_COLOR_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 5);
        ASSERT_TRUE("partial mask over a full clear stays known",
                    obs.known == 1);
        ASSERT_TRUE("partial mask writes only the enabled channels",
                    obs_rgba_is(&obs, 0.9f, 0.2f, 0.3f, 0.5f));
    }

    /* The same partial mask with nothing behind it: the disabled channel
     * holds framebuffer history this design refuses to read, so the whole
     * background is unknown rather than three-quarters invented. */
    {
        GLCmd cmds[3];
        memset(cmds, 0, sizeof(cmds));
        obs_color_mask(cmds, 0, 1, 1, 1, 0);
        obs_clear_color(cmds, 1, 0.1f, 0.2f, 0.3f, 1.0f);
        obs_clear(cmds, 2, GL_COLOR_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 3);
        ASSERT_TRUE("partial mask with no earlier clear stays unknown",
                    obs.known == 0);
    }

    /* GL_COLOR_BUFFER_BIT scopes the clear colour AND the colour mask: the
     * clear inside the push writes nothing, and after the pop both are back
     * to the pre-push values, so the trailing clear takes them. */
    {
        GLCmd cmds[7];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.2f, 0.2f, 0.2f, 1.0f);
        GLCmd *push = obs_cmd(cmds, 1, CMD_PUSH_ATTRIB, -1);
        push->num_args = 1; push->args[0] = GL_COLOR_BUFFER_BIT;
        obs_clear_color(cmds, 2, 0.7f, 0.0f, 0.0f, 1.0f);
        obs_color_mask(cmds, 3, 0, 0, 0, 0);
        obs_clear(cmds, 4, GL_COLOR_BUFFER_BIT);
        obs_cmd(cmds, 5, CMD_POP_ATTRIB, -1);
        obs_clear(cmds, 6, GL_COLOR_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 7);
        ASSERT_TRUE("pop restores both the clear color and the color mask",
                    obs.known == 1 && obs_rgba_is(&obs, 0.2f, 0.2f, 0.2f, 1.0f));
    }

    /* The running observation is not attribute-scoped: a pop rewinds GL
     * state, never pixels already written. */
    {
        GLCmd cmds[4];
        memset(cmds, 0, sizeof(cmds));
        GLCmd *push = obs_cmd(cmds, 0, CMD_PUSH_ATTRIB, -1);
        push->num_args = 1; push->args[0] = GL_COLOR_BUFFER_BIT;
        obs_clear_color(cmds, 1, 0.7f, 0.1f, 0.1f, 1.0f);
        obs_clear(cmds, 2, GL_COLOR_BUFFER_BIT);
        obs_cmd(cmds, 3, CMD_POP_ATTRIB, -1);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 4);
        ASSERT_TRUE("the observation survives the pop unchanged",
                    obs.known == 1 && obs_rgba_is(&obs, 0.7f, 0.1f, 0.1f, 1.0f));
    }

    /* Same rule on the other restore path: the end-of-cursor unwind of an
     * unmatched push. */
    {
        GLCmd cmds[4];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.7f, 0.1f, 0.1f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);
        GLCmd *push = obs_cmd(cmds, 2, CMD_PUSH_ATTRIB, -1);
        push->num_args = 1; push->args[0] = GL_COLOR_BUFFER_BIT;
        obs_clear_color(cmds, 3, 0.9f, 0.9f, 0.9f, 1.0f);   /* never popped */
        ReplBackgroundObservation obs = run_observed_plain(cmds, 4);
        ASSERT_TRUE("the cursor-end unwind does not revert the observation",
                    obs.known == 1 && obs_rgba_is(&obs, 0.7f, 0.1f, 0.1f, 1.0f));
    }

    /* Several clears: the last effective write per channel decides, and a
     * clear colour set after the final clear reaches nothing. */
    {
        GLCmd cmds[6];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.2f, 0.2f, 0.2f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);
        obs_color_mask(cmds, 2, 0, 1, 0, 0);
        obs_clear_color(cmds, 3, 0.8f, 0.8f, 0.8f, 1.0f);
        obs_clear(cmds, 4, GL_COLOR_BUFFER_BIT);
        obs_clear_color(cmds, 5, 0.95f, 0.95f, 0.95f, 1.0f);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 6);
        ASSERT_TRUE("the final effective write per channel decides",
                    obs.known == 1 && obs_rgba_is(&obs, 0.2f, 0.8f, 0.2f, 1.0f));
    }

    /* A depth-only clear touches no colour channel. */
    {
        GLCmd cmds[2];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.5f, 0.0f, 0.0f, 1.0f);
        obs_clear(cmds, 1, GL_DEPTH_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 2);
        ASSERT_TRUE("a depth-only clear establishes no background",
                    obs.known == 0);
    }

    /* goto needs no observation code of its own: the cursor moves the PC, so
     * only the commands actually reached can affect the result. Forward - the
     * jump skips the clear a linear walk would call the last one. */
    {
        static char lines[6][MAX_LINE_LEN];
        GLCmd cmds[6];
        SourceTextView text;
        ReplExecutionOptions opts = {0};

        snprintf(lines[1], sizeof(lines[1]), "goto skip;");
        snprintf(lines[4], sizeof(lines[4]), "skip:");
        text.lines = (const char (*)[MAX_LINE_LEN])lines;
        text.line_count = 6;

        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.2f, 0.0f, 0.0f, 1.0f);
        obs_cmd(cmds, 1, CMD_GOTO, 1);
        obs_clear_color(cmds, 2, 0.6f, 0.0f, 0.0f, 1.0f);
        obs_clear(cmds, 3, GL_COLOR_BUFFER_BIT);
        obs_cmd(cmds, 4, CMD_GOTO_LABEL, 4);
        obs_clear(cmds, 5, GL_COLOR_BUFFER_BIT);

        opts.text = text;
        ReplBackgroundObservation obs = run_observed(cmds, 6, &opts);
        ASSERT_TRUE("a forward goto takes the cursor's path, not a linear one",
                    obs.known == 1 && obs.rgba[0] == 0.2f);
    }

    /* Backward - an unbounded loop, terminated by the cursor's own budget,
     * with the observation its last effective clear produced. */
    {
        static char lines[4][MAX_LINE_LEN];
        GLCmd cmds[4];
        SourceTextView text;
        ReplExecutionOptions opts = {0};
        char status[REPL_DIAG_TEXT_MAX] = "";

        snprintf(lines[1], sizeof(lines[1]), "again:");
        snprintf(lines[3], sizeof(lines[3]), "goto again;");
        text.lines = (const char (*)[MAX_LINE_LEN])lines;
        text.line_count = 4;

        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.3f, 0.0f, 0.0f, 1.0f);
        obs_cmd(cmds, 1, CMD_GOTO_LABEL, 1);
        obs_clear(cmds, 2, GL_COLOR_BUFFER_BIT);
        obs_cmd(cmds, 3, CMD_GOTO, 3);

        opts.text = text;
        opts.status_out = status;
        opts.status_out_sz = (int)sizeof(status);
        ReplBackgroundObservation obs = run_observed(cmds, 4, &opts);
        ASSERT_TRUE("a backward goto terminates under the cursor's budget",
                    status[0] != '\0');
        ASSERT_TRUE("a terminated goto loop still publishes what it cleared",
                    obs.known == 1 && obs.rgba[0] == 0.3f);
    }

    /* A state filter suppresses GL emission, and the observation must follow
     * the emission exactly: both or neither. */
    {
        GLCmd cmds[2];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.4f, 0.4f, 0.4f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);

        gl_stub_counts_reset();
        ReplBackgroundObservation emitted = run_observed_plain(cmds, 2);
        ASSERT_TRUE("unsuppressed: GL is emitted",
                    gl_stub_counts[GL_STUB_glClearColor] == 1 &&
                    gl_stub_counts[GL_STUB_glClear] == 1);
        ASSERT_TRUE("unsuppressed: the background is observed",
                    emitted.known == 1 &&
                    obs_rgba_is(&emitted, 0.4f, 0.4f, 0.4f, 1.0f));

        ReplExecutionOptions opts = {0};
        opts.state_filter = obs_suppress_clears;
        gl_stub_counts_reset();
        ReplBackgroundObservation suppressed = run_observed(cmds, 2, &opts);
        ASSERT_TRUE("suppressed: no GL is emitted",
                    gl_stub_counts[GL_STUB_glClearColor] == 0 &&
                    gl_stub_counts[GL_STUB_glClear] == 0);
        ASSERT_TRUE("suppressed: nothing is observed either",
                    suppressed.known == 0);
    }

    /* Replay fade batches suppress CMD_CLEAR outright (has_fade_context), so
     * they neither wipe the frame nor speak for its background. */
    {
        GLCmd cmds[2];
        ReplExecutionOptions opts = {0};
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.4f, 0.4f, 0.4f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);

        opts.has_fade_context = 1;
        opts.fade_alpha_scale = 0.5f;
        gl_stub_counts_reset();
        ReplBackgroundObservation obs = run_observed(cmds, 2, &opts);
        ASSERT_TRUE("a fade batch emits no clear",
                    gl_stub_counts[GL_STUB_glClear] == 0);
        ASSERT_TRUE("a fade batch publishes no background", obs.known == 0);
    }

    /* observation_out == NULL publishes nothing at all. */
    {
        GLCmd cmds[2];
        ReplExecutionOptions opts = {0};
        ReplBackgroundObservation sink;
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.4f, 0.4f, 0.4f, 1.0f);
        obs_clear(cmds, 1, GL_COLOR_BUFFER_BIT);

        memset(&sink, 0x5A, sizeof(sink));
        opts.flat_cmd_count = 2;
        opts.program.cmds = cmds;
        opts.program.cmd_count = 2;
        opts.observation_out = NULL;
        gl_stub_counts_reset();
        repl_execute_program(&opts);
        ASSERT_TRUE("a NULL sink still emits the clear",
                    gl_stub_counts[GL_STUB_glClear] == 1);
        ASSERT_TRUE("a NULL sink is not written through",
                    sink.rgba[0] != 0.4f);
    }

    /* baseline_clear_rgba is the state the walk starts in: a clear with no
     * preceding glClearColor takes it, and is a fully-known background. */
    {
        GLCmd cmds[1];
        memset(cmds, 0, sizeof(cmds));
        obs_clear(cmds, 0, GL_COLOR_BUFFER_BIT);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 1);
        ASSERT_TRUE("a bare clear observes the caller's baseline",
                    obs.known == 1 &&
                    obs_rgba_is(&obs, 0.10f, 0.10f, 0.10f, 1.0f));
    }

    /* A glClearColor with no glClear behind it repaints nothing. The frame
     * keeps whatever the rect already held, which this design refuses to
     * invent - so the program established no background at all, and the host
     * must fall back rather than take the colour it never used. */
    {
        GLCmd cmds[1];
        memset(cmds, 0, sizeof(cmds));
        obs_clear_color(cmds, 0, 0.9f, 0.0f, 0.0f, 1.0f);
        ReplBackgroundObservation obs = run_observed_plain(cmds, 1);
        ASSERT_TRUE("a program that never clears establishes no background",
                    obs.known == 0);
    }

    /* Attribute nesting past REPL_ATTRIB_STACK_CAP: frames above the cap are
     * counted but not saved, so the pops must not run off attrib_save[] or
     * restore garbage into the scoped clear state. The clear after the unwind
     * still observes the baseline. */
    {
        GLCmd cmds[2 * (REPL_ATTRIB_STACK_CAP + 4) + 1];
        int n = 0, i;
        memset(cmds, 0, sizeof(cmds));
        for (i = 0; i < REPL_ATTRIB_STACK_CAP + 4; i++, n++) {
            GLCmd *push = obs_cmd(cmds, n, CMD_PUSH_ATTRIB, -1);
            push->num_args = 1;
            push->args[0] = GL_COLOR_BUFFER_BIT;
        }
        for (i = 0; i < REPL_ATTRIB_STACK_CAP + 4; i++, n++)
            obs_cmd(cmds, n, CMD_POP_ATTRIB, -1);
        obs_clear(cmds, n, GL_COLOR_BUFFER_BIT);
        n++;
        ReplBackgroundObservation obs = run_observed_plain(cmds, n);
        ASSERT_TRUE("past-cap attrib nesting observes the baseline",
                    obs.known == 1 &&
                    obs_rgba_is(&obs, 0.10f, 0.10f, 0.10f, 1.0f));
    }
}

static void test_attrib_stack(void) {
    repl_executor_init_resources();

    /* Pairing: one push + one pop reach GL once each; depth back to 0. */
    {
        GLCmd cmds[2];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_PUSH_ATTRIB; cmds[0].valid = 1;
        cmds[0].num_args = 1; cmds[0].args[0] = GL_CURRENT_BIT;
        cmds[1].type = CMD_POP_ATTRIB;  cmds[1].valid = 1;

        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = 2; opts.program.cmds = cmds; opts.program.cmd_count = 2;
        gl_stub_counts_reset();
        ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
        repl_exec_cursor_step(&cursor);
        ASSERT_TRUE("attrib push increments depth", cursor.attrib_depth == 1);
        ASSERT_TRUE("attrib push reaches GL", gl_stub_counts[GL_STUB_glPushAttrib] == 1);
        repl_exec_cursor_step(&cursor);
        ASSERT_TRUE("attrib pop decrements depth", cursor.attrib_depth == 0);
        ASSERT_TRUE("attrib pop reaches GL", gl_stub_counts[GL_STUB_glPopAttrib] == 1);
        repl_exec_cursor_end(&cursor);
        ASSERT_TRUE("balanced pair: no extra pop at cursor end",
                    gl_stub_counts[GL_STUB_glPopAttrib] == 1);
    }

    /* Orphan pop: depth stays 0, no glPopAttrib reaches GL (protects the
     * controller's outer bracket from an unbalanced user program). */
    {
        GLCmd cmds[1];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_POP_ATTRIB; cmds[0].valid = 1;
        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = 1; opts.program.cmds = cmds; opts.program.cmd_count = 1;
        gl_stub_counts_reset();
        ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
        repl_exec_cursor_step(&cursor);
        ASSERT_TRUE("orphan pop leaves depth 0", cursor.attrib_depth == 0);
        ASSERT_TRUE("orphan pop reaches no GL", gl_stub_counts[GL_STUB_glPopAttrib] == 0);
        repl_exec_cursor_end(&cursor);
        ASSERT_TRUE("orphan pop: still no glPopAttrib at end",
                    gl_stub_counts[GL_STUB_glPopAttrib] == 0);
    }

    /* Unmatched push is unwound (real pop) at cursor end. */
    {
        GLCmd cmds[1];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_PUSH_ATTRIB; cmds[0].valid = 1;
        cmds[0].num_args = 1; cmds[0].args[0] = GL_CURRENT_BIT;
        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = 1; opts.program.cmds = cmds; opts.program.cmd_count = 1;
        gl_stub_counts_reset();
        ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
        repl_exec_cursor_step(&cursor);
        ASSERT_TRUE("unmatched push: depth 1 mid-cursor", cursor.attrib_depth == 1);
        repl_exec_cursor_end(&cursor);
        ASSERT_TRUE("unmatched push unwound at cursor end",
                    gl_stub_counts[GL_STUB_glPopAttrib] == 1);
        ASSERT_TRUE("unmatched push: depth back to 0", cursor.attrib_depth == 0);
    }

    /* Virtual depth past the cap: 12 pushes then 12 pops. Only
     * REPL_ATTRIB_STACK_CAP frames drive the real GL stack, and they pair
     * down cleanly (equal push/pop GL counts, final depth 0). */
    {
        GLCmd cmds[24];
        memset(cmds, 0, sizeof(cmds));
        for (int i = 0; i < 12; i++) {
            cmds[i].type = CMD_PUSH_ATTRIB; cmds[i].valid = 1;
            cmds[i].num_args = 1; cmds[i].args[0] = GL_CURRENT_BIT;
        }
        for (int i = 12; i < 24; i++) {
            cmds[i].type = CMD_POP_ATTRIB; cmds[i].valid = 1;
        }
        ReplExecutionOptions opts = {0};
        opts.flat_cmd_count = 24; opts.program.cmds = cmds; opts.program.cmd_count = 24;
        gl_stub_counts_reset();
        ReplExecCursor cursor = repl_exec_cursor_begin(&opts);
        while (!repl_exec_cursor_done(&cursor))
            repl_exec_cursor_step(&cursor);
        ASSERT_TRUE("past-cap: virtual depth pairs down to 0", cursor.attrib_depth == 0);
        ASSERT_TRUE("past-cap: only CAP real pushes reach GL",
                    gl_stub_counts[GL_STUB_glPushAttrib] == REPL_ATTRIB_STACK_CAP);
        ASSERT_TRUE("past-cap: real push/pop counts match",
                    gl_stub_counts[GL_STUB_glPopAttrib] ==
                        gl_stub_counts[GL_STUB_glPushAttrib]);
        repl_exec_cursor_end(&cursor);
        ASSERT_TRUE("past-cap: nothing left to unwind",
                    gl_stub_counts[GL_STUB_glPopAttrib] == REPL_ATTRIB_STACK_CAP);
    }

    /* Clear-affecting state under GL_COLOR_BUFFER_BIT: the pop restores both
     * halves of ReplClearScopedState - the clear colour and the colour write
     * mask - to their pre-push values. That group is exactly what a real
     * glPopAttrib rewinds inside GL where the REPL cannot see it, so the
     * cursor's own copy has to rewind with it or a later clear would be
     * observed against state GL no longer holds. */
    {
        float v1[4] = {0.2f, 0.3f, 0.4f, 1.0f};
        GLCmd cmds[6];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_CLEAR_COLOR; cmds[0].valid = 1; cmds[0].num_args = 4;
        cmds[0].args[0] = v1[0]; cmds[0].args[1] = v1[1];
        cmds[0].args[2] = v1[2]; cmds[0].args[3] = v1[3];
        cmds[1].type = CMD_PUSH_ATTRIB; cmds[1].valid = 1;
        cmds[1].num_args = 1; cmds[1].args[0] = GL_COLOR_BUFFER_BIT;
        cmds[2].type = CMD_CLEAR_COLOR; cmds[2].valid = 1; cmds[2].num_args = 4;
        cmds[2].args[0] = 0.9f; cmds[2].args[1] = 0.8f;
        cmds[2].args[2] = 0.7f; cmds[2].args[3] = 1.0f;
        cmds[3].type = CMD_COLOR_MASK; cmds[3].valid = 1; cmds[3].num_args = 4;
        cmds[3].args[0] = 0.0f; cmds[3].args[1] = 0.0f;
        cmds[3].args[2] = 0.0f; cmds[3].args[3] = 0.0f;
        cmds[4].type = CMD_POP_ATTRIB; cmds[4].valid = 1;

        gl_stub_counts_reset();
        ReplExecCursor c = run_attrib_cursor(cmds, 5, 0);
        ASSERT_TRUE("clear-color pop restores V1",
                    c.clear_state.clear_rgba[0] == v1[0] &&
                    c.clear_state.clear_rgba[1] == v1[1] &&
                    c.clear_state.clear_rgba[2] == v1[2] &&
                    c.clear_state.clear_rgba[3] == v1[3]);
        ASSERT_TRUE("color-mask pop restores all channels writable",
                    c.clear_state.color_write_mask == REPL_RGBA_ALL);

        /* suppress_attrib_gl restores the same state with zero GL push/pop. */
        gl_stub_counts_reset();
        c = run_attrib_cursor(cmds, 5, 1);
        ASSERT_TRUE("suppress: clear-color still restored to V1",
                    c.clear_state.clear_rgba[0] == v1[0] &&
                    c.clear_state.clear_rgba[3] == v1[3]);
        ASSERT_TRUE("suppress: color mask still restored",
                    c.clear_state.color_write_mask == REPL_RGBA_ALL);
        ASSERT_TRUE("suppress: no glPushAttrib GL call",
                    gl_stub_counts[GL_STUB_glPushAttrib] == 0);
        ASSERT_TRUE("suppress: no glPopAttrib GL call",
                    gl_stub_counts[GL_STUB_glPopAttrib] == 0);

        /* A push whose mask does not cover the colour-buffer group saves
         * nothing, so the scoped change survives its pop - same per-group gate
         * glPopAttrib itself applies. */
        cmds[1].args[0] = GL_CURRENT_BIT;
        c = run_attrib_cursor(cmds, 5, 0);
        ASSERT_TRUE("unrelated push mask leaves the clear color changed",
                    c.clear_state.clear_rgba[0] == 0.9f);
        ASSERT_TRUE("unrelated push mask leaves the color mask changed",
                    c.clear_state.color_write_mask == 0u);
    }

    /* Light-enable mask under GL_ENABLE_BIT: a light enabled inside the scope
     * is disabled again by the pop. */
    {
        repl_state_render_clear_light_enabled_mask();
        GLCmd cmds[4];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].type = CMD_ENABLE; cmds[0].valid = 1;
        cmds[0].num_args = 1; cmds[0].args[0] = GL_LIGHT0;
        cmds[1].type = CMD_PUSH_ATTRIB; cmds[1].valid = 1;
        cmds[1].num_args = 1; cmds[1].args[0] = GL_ENABLE_BIT;
        cmds[2].type = CMD_ENABLE; cmds[2].valid = 1;
        cmds[2].num_args = 1; cmds[2].args[0] = GL_LIGHT1;
        cmds[3].type = CMD_POP_ATTRIB; cmds[3].valid = 1;

        run_attrib_cursor(cmds, 4, 0);
        ReplRenderState r = repl_state_render();
        ASSERT_TRUE("light-enable pop keeps pre-push LIGHT0",
                    repl_light_enabled(r.light_enabled_mask, 0) == 1);
        ASSERT_TRUE("light-enable pop reverts scoped LIGHT1",
                    repl_light_enabled(r.light_enabled_mask, 1) == 0);
        repl_state_render_clear_light_enabled_mask();
    }

    repl_executor_destroy_resources();
}

int main(void) {
    repl_executor_install_point_parameter_proc((ReplExecutorPointParameterProc)glPointParameterfv);
    test_tess_callbacks();
    test_orrery_phase_alignment();
    test_export_normal_encoding();
    test_light_indicator_tracks_program();
    test_apply_state_bookkeeping();
    test_fade_context();
    test_predef_edge_cases();
    test_transform_stack_edge_cases();
    test_apply_state_cmd_edge_cases();
    test_enum_arg_gl_trace();
    test_enum_arg_end_to_end_trace();
    test_execute_edge_cases();
    test_exec_cursor_step_tracks_state();
    test_exec_cursor_advance_skips_without_state();
    test_attrib_stack();
    test_goto_uses_caller_text_view();
    test_background_observation();
    test_execute_all_commands();
    test_glut_bitmap_string();
    test_executor_camera_distance_source();
    test_executor_matrix_balance_unwind();
    test_executor_matrix_pop_underflow_clamped();
    test_draw_glut_solid_dispatch();
    printf("repl_executor: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
