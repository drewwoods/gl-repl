#include "editor/state.h"
#include "repl/core.h"
#include "editor/input.h"
#include "repl/state.h"
#include "app/glr_ctrl.h"   /* glr_ctrl_reset_all (end-to-end P1 test) */

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
    g_num_predef_vars = 2;
    g_predef_vars[0].value = 1.0f;
    g_predef_vars[1].value = 2.0f;
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
    /* glPointParameterfv args[]: [0]=pname, [1..3]=coeffs. */
    cmd.type = CMD_POINT_PARAMETER_FV; cmd.args[0] = GL_POINT_DISTANCE_ATTENUATION; cmd.num_args = 4; repl_apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_BLEND_FUNC; cmd.args[0] = GL_SRC_ALPHA; cmd.args[1] = GL_ONE_MINUS_SRC_ALPHA; repl_apply_state_cmd(&cmd, 1.0f);
}

/* Regression net for the enum-arg storage migration: assert the actual
 * GL arguments (not just call counts) for the multi-enum commands whose
 * arg order is the easy thing to get wrong. Closes the gap that let a
 * CMD_BLEND_FUNC arg-shift (cmd->mode vs args[]) pass silently — counts
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
    cmd.type = CMD_COLOR_MATERIAL; cmd.num_args = 2;
    cmd.args[0] = GL_FRONT_AND_BACK; cmd.args[1] = GL_AMBIENT_AND_DIFFUSE;
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

    snprintf(want, sizeof(want), "glColorMaterial %u %u\n",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_AMBIENT_AND_DIFFUSE);
    ASSERT_TRUE("glColorMaterial receives (face, mode) in order",
                strstr(buf, want) != NULL);
}

/* End-to-end P1 regression: drive the *full* path the real app uses —
 * source line -> parser -> commit -> flatten -> executor -> GL — and
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
    editor_feed_line("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
    editor_feed_line("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);");
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

    /* glDepthMask(1) must reach GL as GL_TRUE (bool-slot canonicalize). */
    snprintf(want, sizeof(want), "glDepthMask %u\n", (unsigned)GL_TRUE);
    ASSERT_TRUE("e2e glDepthMask(1) -> GL_TRUE", strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glColorMaterial %u %u\n",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_AMBIENT_AND_DIFFUSE);
    ASSERT_TRUE("e2e glColorMaterial(face, mode) order preserved",
                strstr(buf, want) != NULL);

    snprintf(want, sizeof(want), "glLightModeli %u %d\n",
             (unsigned)GL_LIGHT_MODEL_TWO_SIDE, (int)GL_TRUE);
    ASSERT_TRUE("e2e glLightModeli(pname, param) order preserved",
                strstr(buf, want) != NULL);
}

static void test_execute_edge_cases(void) {
    repl_execute_program(NULL);

    ReplExecutionOptions opts;
    opts.flat_cmd_count = -1;
    opts.program.cmds = NULL;
    opts.program.cmd_count = -1;
    opts.program.local_vars = NULL;
    repl_execute_program(&opts);

    repl_execute_commands();
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
     * primitive itself does NOT call glRasterPos3f — that's the
     * user's responsibility via a preceding glRasterPos3f command. */
    repl_executor_init_resources();
    gl_stub_counts_reset();

    GLCmd cmds[2];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_LABEL;
    cmds[0].valid = 1;
    cmds[0].num_args = 0;
    strcpy(cmds[0].text, "hi");

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
    strcpy(cmds[1].text, "hi");
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
    strcpy(cmds[0].text, "v=%f");
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
    strcpy(cmds[0].text, "100%% done");

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

    /* (b) Supported (default): CMD_POINT_PARAMETER_FV calls
     * glPointParameterfv, and the point-size path passes sz straight
     * through without consulting the camera-distance source. */
    repl_executor_set_point_parameter_supported(1);
    gl_stub_counts_reset();
    repl_apply_state_cmd(&pp, 1.0f);
    ASSERT_TRUE("supported: CMD_POINT_PARAMETER_FV calls glPointParameterfv",
                gl_stub_counts[GL_STUB_glPointParameterfv] == 1);
    g_test_camera_distance_calls = 0;
    repl_executor_install_camera_distance_source(test_camera_distance_source);
    repl_exec_point_size(2.0f);
    ASSERT_TRUE("supported: point-size does not consult the camera-distance source",
                g_test_camera_distance_calls == 0);
    repl_executor_install_camera_distance_source(NULL);

    /* Restore default for subsequent tests. */
    repl_executor_set_point_parameter_supported(1);
}

/* Pin down two matrix-stack invariants the executor maintains on
 * behalf of the user. User-program GL calls can leave the modelview
 * stack imbalanced (extra pushes, or extra pops), but:
 *
 *   (a) the executor unwinds any net push imbalance at frame end so
 *       state doesn't leak across frames, and
 *   (b) a stray glPopMatrix is clamped before it reaches GL — the
 *       tracked-pop refuses to call glPopMatrix when its depth
 *       counter is at 0, preventing a GL_STACK_UNDERFLOW.
 *
 * Both behaviours are why scene/render's outer push/pop bracket
 * around the user program is sufficient — the executor cooperates by
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
     * remaining 2 outstanding pushes → expect 3 push, 3 pop calls. */
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

/* Regression: lights[].enabled feeds only the light-indicator overlay.
 * It must track what the current program does (glEnable(GL_LIGHTn)),
 * not stay stuck at the default-on / last-run value. repl_execute_program
 * resets it each walk; this guards that reset + selective re-enable. */
static int light_enabled_by_id(const SceneLight *lights, int id) {
    for (int i = 0; i < MAX_LIGHTS; i++)
        if (lights[i].id == id)
            return lights[i].enabled;
    return -1;
}

static void test_light_indicator_tracks_program(void) {
    SceneLight *L = repl_state_render_mut()->lights;
    GLCmd cmds[1];
    ReplExecutionOptions opts = {0};
    ReplRenderState rs;

    /* Simulate the old sticky / default-on bookkeeping. */
    for (int i = 0; i < MAX_LIGHTS; i++) {
        L[i].id = GL_LIGHT0 + i;
        L[i].enabled = 1;
    }

    /* Program enables only GL_LIGHT0. */
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_ENABLE;
    cmds[0].args[0] = GL_LIGHT0;
    cmds[0].num_args = 1;
    cmds[0].valid = 1;
    opts.flat_cmd_count = 1;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 1;
    repl_execute_program(&opts);

    rs = repl_state_render();
    ASSERT_TRUE("GL_LIGHT0 enabled by program",
                light_enabled_by_id(rs.lights, GL_LIGHT0) == 1);
    ASSERT_TRUE("GL_LIGHT1 reset off (program did not enable)",
                light_enabled_by_id(rs.lights, GL_LIGHT1) == 0);
    ASSERT_TRUE("GL_LIGHT2 reset off (program did not enable)",
                light_enabled_by_id(rs.lights, GL_LIGHT2) == 0);
    ASSERT_TRUE("GL_LIGHT3 reset off",
                light_enabled_by_id(rs.lights, GL_LIGHT3) == 0);

    /* A subsequent program with no light commands must clear the
     * previously-enabled light (the sticky bug). */
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_VERTEX3F;
    cmds[0].valid = 1;
    cmds[0].num_args = 3;
    opts.flat_cmd_count = 1;
    opts.program.cmds = cmds;
    opts.program.cmd_count = 1;
    repl_execute_program(&opts);

    rs = repl_state_render();
    ASSERT_TRUE("GL_LIGHT0 cleared when next program omits glEnable",
                light_enabled_by_id(rs.lights, GL_LIGHT0) == 0);
}

int main(void) {
    test_tess_callbacks();
    test_light_indicator_tracks_program();
    test_fade_context();
    test_predef_edge_cases();
    test_transform_stack_edge_cases();
    test_apply_state_cmd_edge_cases();
    test_enum_arg_gl_trace();
    test_enum_arg_end_to_end_trace();
    test_execute_edge_cases();
    test_execute_all_commands();
    test_glut_bitmap_string();
    test_executor_camera_distance_source();
    test_executor_matrix_balance_unwind();
    test_executor_matrix_pop_underflow_clamped();
    printf("repl_executor: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
