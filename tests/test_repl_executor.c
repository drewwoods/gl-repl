#include "repl_core.h"
#include "repl_state.h"

// Include the C file directly to access its static callbacks.
// We must NOT link repl_executor.o into test_repl_executor!
#include "repl_executor.c"

#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) { \
        g_pass++; \
    } else { \
        printf("FAIL [%s] (line %d)\n", label, __LINE__); \
    } \
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

    GLCmd cmd = { .type = CMD_LABEL }; // not a state cmd
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
    cmds[count].type = CMD_IF_BEGIN; cmds[count].valid = 1; cmds[count].has_vars = 1;
    strcpy(cmds[count].source, "if (1.0) {"); count++;
    cmds[count].type = CMD_IF_END; cmds[count].valid = 1; count++;

    cmds[count].type = CMD_VAR_ASSIGN; cmds[count].valid = 1; cmds[count].has_vars = 1; cmds[count].num_args = 0;
    strcpy(cmds[count].source, "x = 5.0;"); count++;

    // Add GOTO to jump over a command
    cmds[count].type = CMD_GOTO; cmds[count].valid = 1;
    strcpy(cmds[count].source, "goto skip;"); count++;
    cmds[count].type = CMD_VERTEX3F; cmds[count].valid = 1; count++; // skipped
    cmds[count].type = CMD_LABEL; cmds[count].valid = 1;
    strcpy(cmds[count].source, "label skip;"); count++;

    ReplExecutionOptions opts = {0};
    opts.flat_cmd_count = count;
    opts.program.cmds = cmds;
    opts.program.cmd_count = count;
    opts.program.local_vars = NULL;

    repl_execute_program(&opts);

    repl_executor_destroy_resources();
}

int main(void) {
    test_tess_callbacks();
    test_fade_context();
    test_predef_edge_cases();
    test_transform_stack_edge_cases();
    test_apply_state_cmd_edge_cases();
    test_execute_edge_cases();
    test_execute_all_commands();
    printf("repl_executor: %d/%d passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
