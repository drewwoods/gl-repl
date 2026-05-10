#include "repl_core.h"
#include "repl_state.h"

// Include the C file directly to access its static callbacks.
// We must NOT link repl_executor.o into test_repl_executor!
#include "repl_executor.c"

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
    repl_execute_set_fade_context(0.5f, 5);
    ASSERT_TRUE("Fade alpha scale set", g_execute_alpha_scale == 0.5f);
    ASSERT_TRUE("Fade skip before set", g_execute_skip_geom_before_pc == 5);
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
    apply_state_cmd(NULL, 1.0f);

    GLCmd cmd = { .type = CMD_GOTO_LABEL }; // not a state cmd
    int ret = apply_state_cmd(&cmd, 1.0f);
    ASSERT_TRUE("Non-state cmd returns 0", ret == 0);

    // Test all branches in apply_state_cmd
    cmd.type = CMD_ENABLE; cmd.mode = GL_BLEND; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_DISABLE; cmd.mode = GL_BLEND; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_SHADE_MODEL; cmd.mode = GL_FLAT; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_COLOR_MATERIAL; cmd.mode = GL_FRONT; cmd.args[0] = GL_AMBIENT; apply_state_cmd(&cmd, 1.0f);

    cmd.type = CMD_MATERIALF; cmd.mode = GL_FRONT; cmd.num_args = 2; cmd.args[0] = GL_SHININESS; cmd.args[1] = 50; apply_state_cmd(&cmd, 1.0f);
    cmd.num_args = 5; apply_state_cmd(&cmd, 1.0f);

    cmd.type = CMD_LIGHT_MODEL_I; cmd.mode = GL_LIGHT_MODEL_TWO_SIDE; cmd.args[0] = 1; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_FRONT_FACE; cmd.mode = GL_CCW; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_DEPTH_MASK; cmd.mode = 1; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_POINT_PARAMETER_FV; cmd.mode = 0; apply_state_cmd(&cmd, 1.0f);
    cmd.type = CMD_BLEND_FUNC; cmd.mode = GL_SRC_ALPHA; cmd.args[0] = GL_ONE_MINUS_SRC_ALPHA; apply_state_cmd(&cmd, 1.0f);
}

static void test_execute_edge_cases(void) {
    repl_execute_program(NULL);

    ReplExecutionOptions opts;
    opts.flat_cmd_count = -1;
    opts.program.cmds = NULL;
    opts.program.cmd_count = -1;
    opts.program.local_vars = NULL;
    repl_execute_program(&opts);

    execute_commands();
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

    cmds[count].type = CMD_VAR_ASSIGN; cmds[count].valid = 1; cmds[count].has_vars = 1; cmds[count].num_args = 0;
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
 * legacy point-size fallback through a controller-installed callback,
 * not by reaching into glr_camera directly. The pipeline TU must
 * compile + link without glr_camera.h or glr_camera.c, even when
 * NO_POINT_PARAMETER=1 is defined. */
static float g_test_camera_distance_value = 0.0f;
static int   g_test_camera_distance_calls = 0;

static float test_camera_distance_source(void) {
    g_test_camera_distance_calls++;
    return g_test_camera_distance_value;
}

static void test_executor_camera_distance_source(void) {
    /* Default state: no source installed; getter returns NULL. */
    repl_executor_install_camera_distance_source(NULL);
    ASSERT_TRUE("default camera-distance source is NULL",
                repl_executor_camera_distance_source() == NULL);

    /* Install + readback. */
    repl_executor_install_camera_distance_source(test_camera_distance_source);
    ASSERT_TRUE("install + getter round-trips the same fn",
                repl_executor_camera_distance_source() == test_camera_distance_source);

    /* Clearing via NULL install reverts to default. */
    repl_executor_install_camera_distance_source(NULL);
    ASSERT_TRUE("install(NULL) clears the source",
                repl_executor_camera_distance_source() == NULL);

#ifdef NO_POINT_PARAMETER
    /* When NO_POINT_PARAMETER is set, the fallback consults the
     * source for cam_dist before scaling glPointSize. With no source
     * installed (the demo case) cam_dist defaults to 0 and the call
     * passes sz through unchanged — verified by the static
     * `_repl_point_size` not crashing. */
    g_test_camera_distance_calls = 0;
    g_test_camera_distance_value = 4.0f;
    repl_executor_install_camera_distance_source(test_camera_distance_source);
    _repl_point_size(2.0f);
    ASSERT_TRUE("fallback consulted the installed source",
                g_test_camera_distance_calls == 1);
    repl_executor_install_camera_distance_source(NULL);
    _repl_point_size(2.0f);  /* must not crash with no source */
#endif
}

int main(void) {
    test_tess_callbacks();
    test_fade_context();
    test_predef_edge_cases();
    test_transform_stack_edge_cases();
    test_apply_state_cmd_edge_cases();
    test_execute_edge_cases();
    test_execute_all_commands();
    test_glut_bitmap_string();
    test_executor_camera_distance_source();
    printf("repl_executor: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
