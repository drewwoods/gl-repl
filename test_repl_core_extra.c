#include "repl_core_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Capture the output of repl_debug_dump_flat_commands() into a malloc'd string.
 * Returns NULL on failure; caller frees the buffer. */
static char *capture_flat_dump(void) {
    FILE *tmp = tmpfile();
    char *buf = NULL;
    long len;
    size_t nread;

    if (!tmp)
        return NULL;
    repl_debug_dump_flat_commands(tmp);
    fflush(tmp);
    if (fseek(tmp, 0, SEEK_END) != 0) goto done;
    len = ftell(tmp);
    if (len < 0) goto done;
    if (fseek(tmp, 0, SEEK_SET) != 0) goto done;
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) goto done;
    nread = fread(buf, 1, (size_t)len, tmp);
    buf[nread] = '\0';
done:
    fclose(tmp);
    return buf;
}

void test_utils() {
    printf("--- Utility functions ---\n");

    ASSERT_STR("mode_name(GL_POINTS)", mode_name(GL_POINTS), "GL_POINTS");
    ASSERT_STR("mode_name(GL_TRIANGLES)", mode_name(GL_TRIANGLES), "GL_TRIANGLES");
    ASSERT_STR("mode_name(unknown)", mode_name(9999), "???");

    repl_reset_state(); declare_test_vars();
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
    repl_reset_state(); declare_test_vars();
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
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(1,2,3);");

    const char *tmpf = "test_extra_io.c";
    repl_save_output(tmpf);

    repl_reset_state(); declare_test_vars();
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
    repl_reset_state(); declare_test_vars();
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
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(1,1,1);");

    /* Loading an example should save the user scene if it's the first time */
    repl_load_example(0);
    ASSERT_INT("user_scene_valid after example load", repl_user_scene_valid(), 1);

    repl_load_user_scene();
    repl_flatten_commands();
    ASSERT_INT("count_vertices after restore", count_vertices(), 1);
    ASSERT_INT("user_scene_valid after restore", repl_user_scene_valid(), 0);
}

void test_debug_dump_flat_commands() {
    printf("--- Debug dump flat commands ---\n");

    /* Empty state: header, count=0, end marker — and no crash on NULL out. */
    repl_reset_state(); declare_test_vars();
    repl_flatten_commands();
    char *empty = capture_flat_dump();
    ASSERT_TRUE("empty dump captured", empty != NULL);
    if (empty) {
        ASSERT_TRUE("empty dump header",
                    strstr(empty, "=== REPL Flattened Commands Dump ===") != NULL);
        ASSERT_TRUE("empty dump count=0",
                    strstr(empty, "num_flat_cmds=0\n") != NULL);
        ASSERT_TRUE("empty dump end marker",
                    strstr(empty, "=== End REPL Flattened Commands Dump ===") != NULL);
        free(empty);
    }

    /* NULL FILE* should fall back to stdout without crashing. Redirect stdout
     * to /dev/null via dup2 so the test output stays clean. */
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int stdout_redirected = 0;
    if (saved_stdout >= 0) {
        int devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            if (dup2(devnull_fd, STDOUT_FILENO) >= 0) {
                stdout_redirected = 1;
            }
            close(devnull_fd);
        }
    }
    repl_debug_dump_flat_commands(NULL);
    fflush(stdout);
    if (stdout_redirected) {
        if (dup2(saved_stdout, STDOUT_FILENO) >= 0) {
            clearerr(stdout);
        }
        close(saved_stdout);
    } else if (saved_stdout >= 0) {
        close(saved_stdout);
    }
    ASSERT_TRUE("NULL out falls back to stdout", 1);

    /* Basic source commands: each type name should appear in the flattened
     * dump, with one row per flat command. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glFrontFace(GL_CW);");
    repl_feed_line_public("glColor3f(1,0,0);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("glVertex3f(1,0,0);");
    repl_feed_line_public("glVertex3f(0,1,0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();

    char *basic = capture_flat_dump();
    ASSERT_TRUE("basic dump captured", basic != NULL);
    if (basic) {
        char count_line[64];
        snprintf(count_line, sizeof(count_line),
                 "num_flat_cmds=%d\n", g_num_flat_cmds);
        ASSERT_TRUE("basic count matches g_num_flat_cmds",
                    strstr(basic, count_line) != NULL);

        /* The fix in abccf5c3 aligned cmd_type_name with the CmdType enum and
         * added CMD_FRONT_FACE — ensure its label surfaces correctly. */
        ASSERT_TRUE("basic dump contains CMD_FRONT_FACE",
                    strstr(basic, "CMD_FRONT_FACE") != NULL);
        ASSERT_TRUE("basic dump contains CMD_COLOR3F",
                    strstr(basic, "CMD_COLOR3F") != NULL);
        ASSERT_TRUE("basic dump contains CMD_BEGIN",
                    strstr(basic, "CMD_BEGIN") != NULL);
        ASSERT_TRUE("basic dump contains CMD_VERTEX3F",
                    strstr(basic, "CMD_VERTEX3F") != NULL);
        ASSERT_TRUE("basic dump contains CMD_END",
                    strstr(basic, "CMD_END") != NULL);

        /* Per-row fields should be emitted in the documented order. */
        ASSERT_TRUE("basic dump row has valid field",
                    strstr(basic, "valid=1") != NULL);
        ASSERT_TRUE("basic dump row has has_vars field",
                    strstr(basic, "has_vars=0") != NULL);
        ASSERT_TRUE("basic dump row has src_idx field",
                    strstr(basic, "src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has call_src_idx field",
                    strstr(basic, "call_src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has root_call_src_idx field",
                    strstr(basic, "root_call_src_idx=") != NULL);
        ASSERT_TRUE("basic dump row has func_scope mask",
                    strstr(basic, "func_scope=0x00000000") != NULL);

        /* Row count should match num_flat_cmds + 3 fixed lines
         * (header, count, footer). */
        int newlines = 0;
        for (const char *p = basic; *p; p++)
            if (*p == '\n') newlines++;
        ASSERT_INT("basic dump line count",
                   newlines, g_num_flat_cmds + 3);

        free(basic);
    }

    /* For-loop expansion: flattening unrolls the loop body and records a
     * stable src_cmd_idx pointing back at the source line. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i,0,0);");
    repl_feed_line_public("}");
    repl_flatten_commands();

    int vertex_flats = 0;
    for (int i = 0; i < g_num_flat_cmds; i++)
        if (g_flat_cmds[i].type == CMD_VERTEX3F)
            vertex_flats++;
    ASSERT_INT("for-loop unrolled to 3 vertices", vertex_flats, 3);

    char *loop = capture_flat_dump();
    ASSERT_TRUE("loop dump captured", loop != NULL);
    if (loop) {
        /* Flattening unrolls the for-loop, so only the body commands survive
         * in g_flat_cmds[]. The FOR_BEGIN/FOR_END source markers do not
         * appear in the flat stream. */
        int hits = 0;
        const char *p = loop;
        while ((p = strstr(p, "CMD_VERTEX3F")) != NULL) { hits++; p++; }
        ASSERT_INT("loop dump lists all unrolled vertices", hits, 3);
        ASSERT_TRUE("loop dump omits FOR_BEGIN marker",
                    strstr(loop, "CMD_FOR_BEGIN") == NULL);
        ASSERT_TRUE("loop dump omits FOR_END marker",
                    strstr(loop, "CMD_FOR_END") == NULL);
        free(loop);
    }

    /* Function call inlining: flat commands inside the inlined call should
     * carry a non-zero func_scope_mask. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0() {");
    repl_feed_line_public("glVertex3f(0,0,0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0();");
    repl_flatten_commands();

    int scoped_hits = 0;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (g_flat_cmds[i].type == CMD_VERTEX3F &&
            g_flat_cmds[i].func_scope_mask != 0u)
            scoped_hits++;
    }
    ASSERT_TRUE("inlined call sets func_scope_mask", scoped_hits >= 1);

    char *call_dump = capture_flat_dump();
    ASSERT_TRUE("call dump captured", call_dump != NULL);
    if (call_dump) {
        /* At least one row should have a non-zero func_scope hex field. */
        int has_nonzero_scope = 0;
        const char *p = call_dump;
        while ((p = strstr(p, "func_scope=0x")) != NULL) {
            const char *hex = p + strlen("func_scope=0x");
            int all_zero = 1;
            for (int k = 0; k < 8 && hex[k]; k++)
                if (hex[k] != '0') { all_zero = 0; break; }
            if (!all_zero) { has_nonzero_scope = 1; break; }
            p++;
        }
        ASSERT_TRUE("call dump shows non-zero func_scope", has_nonzero_scope);
        free(call_dump);
    }

    /* Implicit flatten: even if the caller leaves g_flat_dirty set and stale
     * flat state behind, the dump should rebuild g_flat_cmds[] on demand. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glVertex3f(0,0,0);");
    g_flat_dirty = 1;
    g_num_flat_cmds = 0;
    FILE *dn = fopen("/dev/null", "w");
    if (dn) {
        repl_debug_dump_flat_commands(dn);
        fclose(dn);
    }
    ASSERT_TRUE("dump re-flattens commands", g_num_flat_cmds >= 1);
}

void test_time() {
    printf("--- Time functions ---\n");
    repl_reset_state(); declare_test_vars();
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
    test_debug_dump_flat_commands();
    test_time();

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
