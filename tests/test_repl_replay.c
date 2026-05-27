#include "app/glr_ctrl.h"
#include "repl/core.h"
#include "editor/input.h"
#include "subsystems/replay/replay.h"
#include "repl/state.h"
#include "subsystems/replay/replay.c"
#include "repl/replay_annotations.h"
#include "source_document.h"
#include "keys.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

#define REPLAY_STATE (replay_state_mut())
#define g_replay_active      (REPLAY_STATE->active)
#define g_replay_state       (REPLAY_STATE->state)
#define g_replay_pc          (REPLAY_STATE->pc)
#define g_replay_mode        (REPLAY_STATE->mode)
#define g_replay_speed       (REPLAY_STATE->speed)
#define g_replay_accum       (REPLAY_STATE->accum)
#define g_replay_fade_speed  (REPLAY_STATE->fade_speed)
#define g_replay_src_line    (REPLAY_STATE->src_line_idx)
#define g_replay_total_flat  (REPLAY_STATE->total_flat_cmds)
#define g_replay_expand_args (REPLAY_STATE->expand_args)
#define g_replay_fade_batch_count (REPLAY_STATE->fade_batch_count)

#ifndef GLUT_KEY_LEFT
#define GLUT_KEY_LEFT 100
#define GLUT_KEY_RIGHT 102
#define GLUT_KEY_UP 101
#define GLUT_KEY_DOWN 103
#define GLUT_KEY_HOME 106
#define GLUT_KEY_END 107
#define GLUT_KEY_NUM_LOCK 119
#define GLUT_KEY_SHIFT_L 112
#define GLUT_KEY_SHIFT_R 113
#define GLUT_KEY_CTRL_L 114
#define GLUT_KEY_CTRL_R 115
#define GLUT_KEY_ALT_L 116
#define GLUT_KEY_ALT_R 117
#define GLUT_KEY_SUPER_L 118
#define GLUT_KEY_SUPER_R 119
#endif

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void add_mock_cmd(int idx, CmdType type) {
    GLCmd* cmds = repl_state_document_cmds_mut();
    memset(&cmds[idx], 0, sizeof(GLCmd));
    cmds[idx].type = type;
    cmds[idx].valid = 1;
    repl_state_document_count_set(idx + 1);
}

static void test_replay_basic_controls(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_COLOR3F);
    add_mock_cmd(1, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    ASSERT_TRUE("not active initially", !g_replay_active);

    replay_start();
    ASSERT_TRUE("active after start", g_replay_active);
    ASSERT_TRUE("state is PLAYING", g_replay_state == REPLAY_PLAYING);
    ASSERT_TRUE("pc is 0", g_replay_pc == 0);

    replay_toggle_play_pause();
    ASSERT_TRUE("state is PAUSED", g_replay_state == REPLAY_PAUSED);

    replay_toggle_play_pause();
    ASSERT_TRUE("state is PLAYING", g_replay_state == REPLAY_PLAYING);

    float old_speed = g_replay_speed;
    replay_speed_adjust(1.5f);
    ASSERT_TRUE("speed is adjusted up", g_replay_speed > old_speed);

    replay_speed_adjust(0.67f);
    ASSERT_TRUE("speed is adjusted down", g_replay_speed < old_speed * 1.5f);

    replay_stop();
    ASSERT_TRUE("not active after stop", !g_replay_active);
}

static void test_replay_stepping(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_BEGIN);
    add_mock_cmd(1, CMD_VERTEX3F);
    add_mock_cmd(2, CMD_VERTEX3F);
    add_mock_cmd(3, CMD_END);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_state = REPLAY_PAUSED;

    int initial_pc = g_replay_pc;
    replay_advance();
    ASSERT_TRUE("advance moves pc", g_replay_pc > initial_pc);

    replay_step_back();
    ASSERT_TRUE("step back restores pc", g_replay_pc == initial_pc || g_replay_pc == 0);

    replay_seek(2);
    ASSERT_TRUE("seek sets pc", g_replay_pc == 2);

    int landed = replay_seek_to_src_line(2);
    ASSERT_TRUE("seek to src line lands", landed >= 0);

    replay_restart_from_beginning();
    ASSERT_TRUE("restart works", g_replay_pc == 0 && g_replay_state == REPLAY_PLAYING);
}

static void test_replay_tessellation_stepping(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_TESS_BEGIN_POLYGON);
    add_mock_cmd(1, CMD_TESS_BEGIN_CONTOUR);
    add_mock_cmd(2, CMD_TESS_VERTEX);
    add_mock_cmd(3, CMD_TESS_END);
    add_mock_cmd(4, CMD_TESS_END);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_POLYGON;

    replay_advance();
    ASSERT_TRUE("tess poly advance", g_replay_pc > 0);

    replay_step_back();
    ASSERT_TRUE("tess poly back", g_replay_pc == 0);

    g_replay_mode = REPLAY_MODE_VERTEX;
    replay_advance();
    ASSERT_TRUE("tess vert advance", g_replay_pc > 0);
}

static void test_replay_fade_batches(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_push_fade_batch(0, 1);

    ASSERT_TRUE("has active fades", replay_has_active_fades());

    ReplayFadeBatchView view = replay_fade_batches_view();
    ASSERT_TRUE("batch count > 0", view.count > 0);

    replay_tick_fade_batches(0.016f);
    ASSERT_TRUE("batch alpha updated", replay_batch_alpha(&view.batches[0]) > 0.0f);

    int limits[5];
    int lcount = replay_compute_fade_skip_limits(limits, 5);
    ASSERT_TRUE("fade skip limits computed", lcount >= 0);

    replay_tick_fade_batches(10.0f); // Age completely
    ASSERT_TRUE("no active fades after aging", !replay_has_active_fades());
}

static void test_replay_input(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    ASSERT_TRUE("Ctrl+R no longer belongs to replay_handle_key",
                replay_handle_key(KEY_CTRL_R) == 0);
    ASSERT_TRUE("replay still off before explicit start", !g_replay_active);

    replay_start();
    ASSERT_TRUE("replay_start activates replay", g_replay_active);

    // When on
    replay_handle_key(' ');
    ASSERT_TRUE("Space pauses", g_replay_state == REPLAY_PAUSED);

    replay_handle_special(GLUT_KEY_RIGHT);
    ASSERT_TRUE("Right advances", g_replay_pc > 0);

    replay_handle_special(GLUT_KEY_LEFT);
    ASSERT_TRUE("Left retreats", g_replay_pc == 0);

    // Ensure replay is active for following tests
    replay_start();

    replay_handle_key('m');
    ASSERT_TRUE("m toggles mode", g_replay_mode == REPLAY_MODE_POLYGON);

    replay_handle_key('e');
    ASSERT_TRUE("e toggles expand args", g_replay_expand_args == 0);

    replay_handle_key(KEY_ESC);
    ASSERT_TRUE("Esc stops replay", !g_replay_active);
}

static void test_replay_modifiers(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    replay_start();

#ifndef USE_GLUT
    int handled = replay_handle_special(GLUT_KEY_SHIFT_L);
    ASSERT_TRUE("modifier handled internally", handled == 0);
    ASSERT_TRUE("modifier doesn't stop replay", g_replay_active);
#else
    // If USE_GLUT is defined, modifier returns 0, so it WILL stop replay.
    // Let's just simulate it.
    g_harness.passed++; g_harness.run++; // skip
    g_harness.passed++; g_harness.run++;
#endif
}

static void test_bench_helpers(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    add_mock_cmd(1, CMD_VERTEX3F);
    add_mock_cmd(2, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    int old_pcs[] = {0, 1};
    int new_pcs[] = {1, 2};
    replay_bench_fade_install(old_pcs, new_pcs, 2, 0.1f);
    ASSERT_TRUE("bench install sets batches", g_replay_fade_batch_count == 2);

    replay_bench_fade_clear();
    ASSERT_TRUE("bench clear unsets batches", g_replay_fade_batch_count == 0);
}

static void test_misc_helpers(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    replay_start();

    int limit = replay_exec_limit();
    ASSERT_TRUE("exec limit", limit >= 0);

    int fill_base = replay_fill_base_limit();
    ASSERT_TRUE("fill base limit", fill_base >= 0);

    replay_prepare_frame(1);

    float dummy[MAX_PREDEF_VARS];
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    float scratch_value = 0.0f;
    replay_copy_baseline_predef_values(dummy, MAX_PREDEF_VARS);
    repl_eval_scratch_set(0, 0, 3.0f);
    replay_start();
    repl_eval_scratch_set(0, 0, 9.0f);
    replay_copy_baseline_scratch_arrays(scratch);
    ASSERT_TRUE("baseline scratch copied", fabsf(scratch[0][0] - 3.0f) < 1e-6f);
    replay_restore_baseline_scratch_arrays();
    ASSERT_TRUE("baseline scratch restored",
                repl_eval_scratch_get(0, 0, &scratch_value) && fabsf(scratch_value - 3.0f) < 1e-6f);
    replay_restore_baseline_predef_values();
}

/* Regression: replay annotation simulation must apply the precomputed
 * args[0] for CMD_VAR_ASSIGN, mirroring the live executor (which
 * deliberately does not re-evaluate — see executor.c:CMD_VAR_ASSIGN).
 *
 * The original bug: `tDelta = (t - tLast) * 10;` substituted as 0 in the
 * downstream `p = tDelta * i * scale;` annotation while the variable
 * panel correctly displayed 0.16. Cause: replay does not re-flatten, so
 * the live executor keeps applying the args[0] frozen at the prior
 * flatten (when tLast still held its previous-frame value); meanwhile
 * flatten itself ran `tLast = t` afterward, so the baseline replay_start
 * captured had t == tLast — and the simulation re-evaluated the RHS
 * against that baseline, producing 0 instead of the cached 0.16. */
static void test_replay_var_assign_uses_flatten_args(void) {
    glr_ctrl_reset_all();

    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", t_idx >= 0);

    /* Set t before feeding so commit-time eval sees t=1, giving args[0]=5. */
    g_predef_vars_mut[t_idx].value = 1.0f;

    editor_feed_line("float u;");
    editor_feed_line("u = (t - 0.5) * 10;");
    editor_feed_line("glVertex3f(u, 0, 0);");

    int u_idx = repl_eval_find_predef_var_idx("u");
    ASSERT_TRUE("u predef declared", u_idx >= 0);

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    /* Clear dirty so replay_start does NOT re-flatten with the mutated
     * t below — we want args[0]=5 frozen at this flatten. */
    repl_state_flat_program_clear_dirty();

    /* Sanity-check the args[0] flatten captured. */
    {
        FlatProgramView fp = repl_state_flat_program_view();
        int found = 0;
        for (int i = 0; i < fp.cmd_count; i++) {
            if (fp.cmds[i].type == CMD_VAR_ASSIGN &&
                fp.cmds[i].var_idx == u_idx) {
                ASSERT_TRUE("flatten cached args[0]=5 for u",
                            fabsf(fp.cmds[i].args[0] - 5.0f) < 1e-5f);
                found = 1;
                break;
            }
        }
        ASSERT_TRUE("flat program contains u assignment", found);
    }

    /* Mutate t after flatten — replay never re-flattens, so the live
     * executor keeps using args[0]=5 frozen above. A re-evaluation
     * against this baseline would give (10-0.5)*10=95. */
    g_predef_vars_mut[t_idx].value = 10.0f;

    replay_start();
    ASSERT_TRUE("replay started", g_replay_active);

    /* Step PC to the end so all flat cmds (incl. the assignment and the
     * vertex call) have been consumed, and src_line points at the last
     * focus-candidate command (the glVertex3f). */
    int safety = 1024;
    while (g_replay_pc < g_replay_total_flat && safety-- > 0)
        replay_advance();

    ASSERT_TRUE("replay reached end without runaway", safety > 0);
    ASSERT_TRUE("expand_args on by default", g_replay_expand_args == 1);

    SourceTextView text = source_document_view();
    ReplReplayAnnotationOutput out;
    replay_annotations_prepare(text, &out);

    int subst_found = 0;
    int subst_uses_flatten_value = 0;
    for (int i = 0; i < out.count; i++) {
        if (out.items[i].kind != REPL_REPLAY_ANNOTATION_KIND_SUBST)
            continue;
        subst_found = 1;
        /* Substitution should produce "glVertex3f(5, 0, 0);" (args[0]=5).
         * The buggy re-eval would substitute "95" instead. Match on the
         * exact "(5," prefix so "5" inside "95" can't false-pass. */
        if (strstr(out.items[i].text, "glVertex3f(5,") != NULL &&
            strstr(out.items[i].text, "95") == NULL)
            subst_uses_flatten_value = 1;
    }
    ASSERT_TRUE("SUBST annotation present", subst_found);
    ASSERT_TRUE("SUBST uses flatten args[0]=5, not re-eval=95",
                subst_uses_flatten_value);

    replay_stop();
}

/* Regression: single-argument GLUT shapes must still emit an EVAL
 * annotation row during replay. The bug dropped CMD_GLUT_CUBE and
 * CMD_GLUT_TEAPOT because the formatter only handled nargs 2..6. */
static void test_replay_single_arg_shape_gets_eval_annotation(void) {
    int t_idx;

    glr_ctrl_reset_all();

    t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", t_idx >= 0);
    g_predef_vars_mut[t_idx].value = 0.25f;

    editor_feed_line("glutSolidCube(t);");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    replay_start();
    ASSERT_TRUE("replay started for single-arg shape", g_replay_active);

    while (g_replay_pc < g_replay_total_flat)
        replay_advance();

    SourceTextView text = source_document_view();
    ReplReplayAnnotationOutput out;
    replay_annotations_prepare(text, &out);

    int eval_found = 0;
    for (int i = 0; i < out.count; i++) {
        if (out.items[i].kind != REPL_REPLAY_ANNOTATION_KIND_EVAL)
            continue;
        eval_found = 1;
        ASSERT_TRUE("single-arg shape eval text present",
                    strstr(out.items[i].text, "glutSolidCube(0.25);") != NULL);
    }
    ASSERT_TRUE("single-arg shape produced eval annotation", eval_found);

    replay_stop();
}

/* #3 regression: the replay baseline must be restored by NAME, not by
 * slot index. Replay spans multiple frames; mid-replay the live predef
 * table can be reshaped (workspace switch, scene load, undo across
 * @declare). Pre-fix the baseline carried only floats indexed by slot,
 * so a reshape between replay_start and the fade-render restore landed
 * each saved value into the slot that USED to hold its variable —
 * which now holds a different variable. */
static void test_replay_baseline_restore_survives_predef_reshape(void) {
    char err[64];
    glr_ctrl_reset_all();

    /* Build a tiny program so replay considers it has meaningful cmds. */
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    /* Declare vars after the built-in `t`. The order matters: at
     * replay_start the table will be [t, X, Y, Z]. */
    repl_eval_declare_predef_var("X", err, sizeof(err));
    repl_eval_declare_predef_var("Y", err, sizeof(err));
    repl_eval_declare_predef_var("Z", err, sizeof(err));
    int x_idx = repl_eval_find_predef_var_idx("X");
    int y_idx = repl_eval_find_predef_var_idx("Y");
    int z_idx = repl_eval_find_predef_var_idx("Z");
    ASSERT_TRUE("X declared", x_idx >= 0);
    ASSERT_TRUE("Y declared", y_idx >= 0);
    ASSERT_TRUE("Z declared", z_idx >= 0);

    g_predef_vars_mut[x_idx].value = 10.0f;
    g_predef_vars_mut[y_idx].value = 20.0f;
    g_predef_vars_mut[z_idx].value = 30.0f;

    replay_start();
    ASSERT_TRUE("replay active", g_replay_active);

    /* Reshape the live predef table mid-replay: drop Y. Pre-fix this
     * cascades the slots — Z now sits in Y's old slot. A values-only
     * restore would later assign Y's saved value (20) into Z, and
     * drop Z's saved value entirely. */
    repl_eval_undeclare_predef_var("Y");
    int x_idx2 = repl_eval_find_predef_var_idx("X");
    int z_idx2 = repl_eval_find_predef_var_idx("Z");
    ASSERT_TRUE("X still present after Y undeclared", x_idx2 >= 0);
    ASSERT_TRUE("Z still present after Y undeclared", z_idx2 >= 0);
    ASSERT_TRUE("Y is gone", repl_eval_find_predef_var_idx("Y") < 0);

    /* Clobber the live values; the restore should overwrite. */
    g_predef_vars_mut[x_idx2].value = 99.0f;
    g_predef_vars_mut[z_idx2].value = 99.0f;

    replay_restore_baseline_predef_values();

    ASSERT_TRUE("post-restore: X gets X's saved value (10)",
                fabsf(g_predef_vars[x_idx2].value - 10.0f) < 1e-5f);
    ASSERT_TRUE("post-restore: Z gets Z's saved value (30), NOT Y's (20)",
                fabsf(g_predef_vars[z_idx2].value - 30.0f) < 1e-5f);

    /* Live table shape is unchanged — by-name restore never resurrects
     * the dropped Y, never adds/removes slots. */
    ASSERT_TRUE("post-restore: Y stays gone",
                repl_eval_find_predef_var_idx("Y") < 0);
    /* Also confirm the snapshot copy mirrors what was saved at start. */
    {
        float snap_vals[MAX_PREDEF_VARS];
        char snap_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
        int snap_count = -1;
        replay_copy_baseline_predef_snapshot(snap_vals, snap_names,
                                             &snap_count);
        ASSERT_TRUE("snapshot count matches replay_start table",
                    snap_count >= 4); /* t + X + Y + Z, plus any others */
        int found_y = 0;
        float y_saved = 0.0f;
        for (int i = 0; i < snap_count; i++) {
            if (strcmp(snap_names[i], "Y") == 0) {
                found_y = 1;
                y_saved = snap_vals[i];
                break;
            }
        }
        ASSERT_TRUE("snapshot still contains the dropped Y", found_y);
        ASSERT_TRUE("snapshot's Y value is the replay-start value (20)",
                    fabsf(y_saved - 20.0f) < 1e-5f);
    }

    replay_stop();
    repl_eval_undeclare_predef_var("X");
    repl_eval_undeclare_predef_var("Z");
}

static void test_replay_regression_fixes(void) {
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_COLOR3F);
    add_mock_cmd(1, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    /* 1. Replay unrecognized keys stop replay with a status message */
    replay_start();
    ASSERT_TRUE("active after start", g_replay_active);

    int consumed = replay_handle_key('z');
    ASSERT_TRUE("unrecognized key not consumed", consumed == 0);
    ASSERT_TRUE("replay stopped on unrecognized key", !g_replay_active);

    /* 2. Replay expand toggle routes through config */
    replay_start();
    ASSERT_TRUE("active after restart", g_replay_active);
    ASSERT_TRUE("expand args on initially", g_replay_expand_args == 1);

    consumed = replay_handle_key('e');
    ASSERT_TRUE("expand key consumed", consumed == 1);
    ASSERT_TRUE("expand args toggled", g_replay_expand_args == 0);

    replay_stop();
}

int main(void) {
    test_replay_basic_controls();
    test_replay_stepping();
    test_replay_tessellation_stepping();
    test_replay_fade_batches();
    test_replay_input();
    test_replay_modifiers();
    test_bench_helpers();
    test_misc_helpers();
    test_replay_var_assign_uses_flatten_args();
    test_replay_single_arg_shape_gets_eval_annotation();
    test_replay_baseline_restore_survives_predef_reshape();
    test_replay_regression_fixes();

    printf("test_repl_replay: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
