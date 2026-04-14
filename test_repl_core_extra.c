#include "repl_core_internal.h"
#include <stdio.h>
#include <string.h>
#include <GL/freeglut.h>
#include <unistd.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d\n", label, (int)(got), (int)(exp)); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\"\n", label, got, exp); \
} while (0)

/* Some functions are not in internal header but are non-static */
const char *mode_name(GLenum mode);
int in_begin_block(void);
int cmd_indent_chars(int pos);
GLenum current_begin_mode(void);
int count_vertices(void);
extern int g_num_flat_cmds;
extern int g_replay_pc;
extern int g_replay_state;
extern float g_anim_time;

void test_utils() {
    printf("--- Utility functions ---\n");

    ASSERT_STR("mode_name(GL_POINTS)", mode_name(GL_POINTS), "GL_POINTS");
    ASSERT_STR("mode_name(GL_TRIANGLES)", mode_name(GL_TRIANGLES), "GL_TRIANGLES");
    ASSERT_STR("mode_name(unknown)", mode_name(9999), "???");

    repl_reset_state();
    ASSERT_INT("count_vertices initial", count_vertices(), 0);
    ASSERT_INT("current_begin_mode initial", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block initial", in_begin_block(), 0);

    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,0,0);");
    repl_flatten_commands();

    ASSERT_INT("count_vertices after 2 vtx", count_vertices(), 2);
    ASSERT_INT("current_begin_mode in block", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block in block", in_begin_block(), 1);

    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    ASSERT_INT("current_begin_mode after end", current_begin_mode(), GL_TRIANGLES);
    ASSERT_INT("in_begin_block after end", in_begin_block(), 0);

    /* cmd_indent_chars */
    ASSERT_INT("cmd_indent_chars at 0", cmd_indent_chars(0), 2);
    ASSERT_INT("cmd_indent_chars at 1", cmd_indent_chars(1), 4);
    ASSERT_INT("cmd_indent_chars at 4", cmd_indent_chars(4), 2);

    /* debug dump */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) {
        repl_debug_dump_editor(devnull);
        fclose(devnull);
    }
}

void test_replay_advanced() {
    printf("--- Replay advanced functions ---\n");
    repl_reset_state();
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,1,1);");
    repl_feed_line_public("glVertex3f(2,2,2);");
    repl_flatten_commands();

    replay_start();
    ASSERT_INT("replay_exec_limit start", replay_exec_limit(), 0);

    replay_advance();
    ASSERT_INT("replay_exec_limit advance 1", replay_exec_limit(), 1);

    replay_advance();
    ASSERT_INT("replay_exec_limit advance 2", replay_exec_limit(), 2);

    replay_step_back();
    ASSERT_INT("replay_exec_limit step back", replay_exec_limit(), 1);

    replay_seek_to_src_line(2);
    ASSERT_INT("replay_exec_limit seek_to_src_line(2)", replay_exec_limit(), 3);

    replay_restart_from_beginning();
    ASSERT_INT("replay_exec_limit restart", replay_exec_limit(), 0);

    replay_stop();
}

void test_io() {
    printf("--- IO functions ---\n");
    repl_reset_state();
    repl_feed_line_public("glVertex3f(1,2,3);");

    const char *tmpf = "test_extra_io.c";
    repl_save_output(tmpf);

    repl_reset_state();
    ASSERT_INT("num_cmds after reset", count_vertices(), 0);

    int r = repl_load_from_file(tmpf);
    ASSERT_INT("load_from_file return", r, 1);
    repl_flatten_commands();
    ASSERT_INT("count_vertices after load", count_vertices(), 1);

    unlink(tmpf);

    repl_load_initial_commands(NULL);
    repl_save_default_output();
}

void test_execution() {
    printf("--- Execution functions ---\n");
    repl_reset_state();
    repl_feed_line_public("n = 1;");
    repl_flatten_commands();

    execute_commands();
}

void test_examples() {
    printf("--- Example functions ---\n");
    int count = repl_example_count();
    ASSERT_TRUE("example_count > 0", count > 0);

    const char *name = repl_example_name(0);
    ASSERT_TRUE("example_name(0) != NULL", name != NULL);

    repl_load_example(0);
    ASSERT_TRUE("g_num_cmds > 0 after load_example", g_num_cmds > 0);
}

void test_user_scene() {
    printf("--- User scene functions ---\n");
    repl_reset_state();
    repl_feed_line_public("glVertex3f(1,1,1);");

    /* Loading an example should save the user scene if it's the first time */
    repl_load_example(0);
    ASSERT_INT("user_scene_valid after example load", repl_user_scene_valid(), 1);

    repl_load_user_scene();
    repl_flatten_commands();
    ASSERT_INT("count_vertices after restore", count_vertices(), 1);
    ASSERT_INT("user_scene_valid after restore", repl_user_scene_valid(), 0);
}

void test_time() {
    printf("--- Time functions ---\n");
    repl_reset_state();
    g_anim_time = 0.0f;
    repl_advance_time(0.5f);
    ASSERT_TRUE("g_anim_time advanced", g_anim_time == 0.5f);

    repl_reset_time_to_zero();
}

int main(int argc, char **argv) {
    init_predef_vars();

    test_utils();
    test_replay_advanced();
    test_io();
    test_execution();
    test_examples();
    test_user_scene();
    test_time();

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
