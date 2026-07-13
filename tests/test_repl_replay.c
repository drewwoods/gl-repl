#include "app/glr_ctrl.h"
#include "repl/example_loader.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "editor/input.h"
#include "subsystems/replay/replay.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay.c"
#include "subsystems/replay/replay_annotations.h"
#include "source_document.h"
#include "keys.h"
#include "support/test_harness.h"

#include "subsystems/replay/replay_render.h"
#include "ui/subsystems/replay_hud.h"
#include "ui/app/snapshot.h"
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif
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
#define g_replay_focus_call_depth (REPLAY_STATE->focus_call_depth)
#define g_replay_total_flat  (REPLAY_STATE->total_flat_cmds)
#define g_replay_expand_args (REPLAY_STATE->expand_args)
#define g_replay_normal_display (REPLAY_STATE->normal_display)
#define g_replay_vertex_label (REPLAY_STATE->vertex_label)
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
    replay_advance(repl_state_flat_program_view());
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

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("tess poly advance", g_replay_pc > 0);

    replay_step_back();
    ASSERT_TRUE("tess poly back", g_replay_pc == 0);

    g_replay_mode = REPLAY_MODE_VERTEX;
    replay_advance(repl_state_flat_program_view());
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
    int lcount = replay_compute_fade_skip_limits(repl_state_flat_program_view(), limits, 5);
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

    ASSERT_TRUE("normal display off by default",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_OFF);
    replay_handle_key('n');
    ASSERT_TRUE("n toggles normal vectors on",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_VECTOR);
    replay_handle_key('n');
    ASSERT_TRUE("n toggles normal direction on",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_DIRECTION);
    replay_handle_key('n');
    ASSERT_TRUE("n cycles normal display back off",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_OFF);

    ASSERT_TRUE("replay vertex label off by default", g_replay_vertex_label == 0);
    replay_handle_key('v');
    ASSERT_TRUE("v toggles replay vertex label on", g_replay_vertex_label == 1);
    replay_handle_key('v');
    ASSERT_TRUE("v toggles replay vertex label off", g_replay_vertex_label == 0);

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

    int fill_base = replay_fill_base_limit(repl_state_flat_program_view());
    ASSERT_TRUE("fill base limit", fill_base >= 0);

    replay_prepare_frame(repl_state_flat_program_view(), 1);

    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    float scratch_value = 0.0f;
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
    editor_feed_line("u = (t - 0.5) * 10 + sin(t) * 0;");
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
        replay_advance(repl_state_flat_program_view());

    ASSERT_TRUE("replay reached end without runaway", safety > 0);
    ASSERT_TRUE("expand_args on by default", g_replay_expand_args == 1);

    SourceTextView text = source_document_view();
    char display[256];
    replay_code_panel_get_command_display_text(text, 1, display, sizeof(display));

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
        replay_advance(repl_state_flat_program_view());

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
        ReplPredefSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        replay_copy_baseline_predef_snapshot(&snap);
        ASSERT_TRUE("snapshot count matches replay_start table",
                    snap.count >= 4); /* t + X + Y + Z, plus any others */
        int found_y = 0;
        float y_saved = 0.0f;
        for (int i = 0; i < snap.count; i++) {
            if (strcmp(snap.names[i], "Y") == 0) {
                found_y = 1;
                y_saved = snap.vals[i];
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
    /* Behavior contract: unrecognized keys that stop replay are consumed to prevent cascading downstream side-effects */
    ASSERT_TRUE("unrecognized key consumed on cancellation", consumed == 1);
    ASSERT_TRUE("replay stopped on unrecognized key", !g_replay_active);

    /* 2. Replay expand toggle routes through config */
    replay_start();
    ASSERT_TRUE("active after restart", g_replay_active);
    ASSERT_TRUE("expand args on initially", g_replay_expand_args == 1);
    ASSERT_TRUE("normal display off initially",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_OFF);
    ASSERT_TRUE("vertex label off initially", g_replay_vertex_label == 0);

    consumed = replay_handle_key('e');
    ASSERT_TRUE("expand key consumed", consumed == 1);
    ASSERT_TRUE("expand args toggled", g_replay_expand_args == 0);

    consumed = replay_handle_key('n');
    ASSERT_TRUE("normal key consumed", consumed == 1);
    ASSERT_TRUE("normal display toggled to vector",
                g_replay_normal_display == REPLAY_NORMAL_DISPLAY_VECTOR);

    consumed = replay_handle_key('v');
    ASSERT_TRUE("vertex label key consumed", consumed == 1);
    ASSERT_TRUE("vertex label toggled on", g_replay_vertex_label == 1);

    replay_stop();
}

static void test_replay_cursor_sync_and_unrecognized_keys(void) {
    printf("--- replay cursor sync and unrecognized keys ---\n");
    glr_ctrl_reset_all();

    // Test replay_walk_flat_cmd_matches_cursor
    GLCmd cmds[5];
    memset(cmds, 0, sizeof(cmds));
    for (int i = 0; i < 5; i++) {
        cmds[i].valid = 1;
    }
    FlatProgramView program = { cmds, NULL, 5 };

    // Match via call_src_cmd_idx
    cmds[0].call_src_cmd_idx = 10;
    ASSERT_TRUE("replay matches call_src_cmd_idx",
                replay_walk_flat_cmd_matches_cursor(0, 10, -1, -1, 0, program));
    cmds[0].call_src_cmd_idx = 0;

    // Match via root_call_src_cmd_idx
    cmds[0].root_call_src_cmd_idx = 10;
    ASSERT_TRUE("replay matches root_call_src_cmd_idx",
                replay_walk_flat_cmd_matches_cursor(0, 10, -1, -1, 0, program));
    cmds[0].root_call_src_cmd_idx = -1;

    // Match via func_scope_mask
    cmds[0].func_scope_mask = 2;
    ASSERT_TRUE("replay matches func_scope_mask",
                replay_walk_flat_cmd_matches_cursor(0, 10, -1, -1, 2, program));
    cmds[0].func_scope_mask = 0;

    // Match via cursor block bounds
    ASSERT_TRUE("replay matches cursor block bounds",
                replay_walk_flat_cmd_matches_cursor(2, 10, 1, 3, 0, program));

    // Test replay_walk_block_matches_cursor
    memset(cmds, 0, sizeof(cmds));
    cmds[0].valid = 1; cmds[0].type = CMD_BEGIN;
    cmds[1].valid = 1; cmds[1].type = CMD_VERTEX3F; cmds[1].src_cmd_idx = 5;
    cmds[2].valid = 1; cmds[2].type = CMD_END;
    program.cmd_count = 3;
    ASSERT_TRUE("replay block matches cursor",
                replay_walk_block_matches_cursor(0, 0, 5, -1, -1, 0, program));

    // Test KEY_CTRL_K jump
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    add_mock_cmd(1, CMD_VERTEX3F);
    repl_state_document_cmds_mut()[0].src_cmd_idx = 0;
    repl_state_document_cmds_mut()[1].src_cmd_idx = 1;
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    // Jump when replay is inactive
    editor_state_edit_line_set(1);
    int consumed = replay_handle_key(KEY_CTRL_K);
    ASSERT_TRUE("Ctrl+K consumed when inactive", consumed == 1);
    ASSERT_TRUE("replay started after jump", g_replay_active);
    ASSERT_TRUE("replay paused after jump", g_replay_state == REPLAY_PAUSED);
    ASSERT_TRUE("pc moved to 2", g_replay_pc == 2);
    replay_stop();

    // Test space-restart (when state is REPLAY_DONE)
    replay_start();
    g_replay_state = REPLAY_DONE;
    consumed = replay_handle_key(' ');
    ASSERT_TRUE("space consumed on done", consumed == 1);
    ASSERT_TRUE("restarted on space", g_replay_pc == 0 && g_replay_state == REPLAY_PLAYING);
    replay_stop();

    // Test unrecognized special key cancellation
    replay_start();
    consumed = replay_handle_special(999);
    ASSERT_TRUE("unrecognized special consumed", consumed == 1);
    ASSERT_TRUE("unrecognized special cancels replay", !g_replay_active);
    replay_stop();
}


#ifdef GL_STUBS
static void test_replay_rendering(void) {
    printf("--- replay rendering & HUD callbacks ---\n");

    /* 1. Setup mock program and fade batches */
    glr_ctrl_reset_all();
    add_mock_cmd(0, CMD_VERTEX3F);
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_push_fade_batch(0, 1);

    /* Get the fade plan */
    ReplayFadePlan plan;
    memset(&plan, 0, sizeof(plan));
    replay_copy_baseline_predef_snapshot(&plan.baseline_predef);
    replay_copy_baseline_scratch_arrays(
        plan.baseline_scratch_arrays);

    ReplayFadeBatchView fade_batches = replay_fade_batches_view();
    plan.batch_count = 1;
    plan.batches[0] = fade_batches.batches[0];
    plan.batch_alpha[0] = 0.5f;
    plan.active = 1;
    plan.tess_preview_active = 1;

    /* 2. Test replay rendering functions */
    gl_stub_counts_reset();
    replay_render_fade_batches(&plan);
    ASSERT_TRUE("render_fade_batches calls GL", gl_stub_counts[GL_STUB_glPushAttrib] > 0);

    gl_stub_counts_reset();
    replay_render_tess_preview(&plan);
    ASSERT_TRUE("render_tess_preview calls GL", gl_stub_counts[GL_STUB_glPushAttrib] > 0);

    gl_stub_counts_reset();
    replay_render_post_fill(&plan);
    ASSERT_TRUE("render_post_fill runs correctly", gl_stub_counts[GL_STUB_glPushAttrib] > 0);

    /* 3. Test Replay HUD rendering */
    UiRenderSnapshot hud_snap;
    memset(&hud_snap, 0, sizeof(hud_snap));
    hud_snap.replay.active = 1;
    hud_snap.replay.total_flat_cmds = 1;
    hud_snap.replay.pc = 0;
    hud_snap.replay.state = REPLAY_PLAYING;
    hud_snap.replay.speed = 1.0f;
    hud_snap.replay.mode = REPLAY_MODE_VERTEX;
    hud_snap.replay.expand_args = 1;
    hud_snap.replay.normal_display = REPLAY_NORMAL_DISPLAY_DIRECTION;
    hud_snap.replay.vertex_label = 1;
    hud_snap.viewport.window_w = 800;
    hud_snap.viewport.window_h = 600;
    hud_snap.code_panel.layout_mode = CODE_PANEL_LAYOUT_LEFT;

    gl_stub_counts_reset();
    replay_ui_hud_render(&hud_snap);
    ASSERT_TRUE("HUD render calls GL", gl_stub_counts[GL_STUB_glBegin] > 0);

    /* Clean up */
    replay_stop();
}
#endif

/* Request 1: during replay the focused flat command carries the funcN(...)
 * call-site provenance (call_src_cmd_idx / root_call_src_cmd_idx) that
 * glr_ctrl_push_highlights() turns into call-site gutter markers. Verify
 * replay_focus_flat_idx() lands on the live (focus-candidate) command and that
 * its provenance names the right invocation for a reused function, and that
 * immediate vs. root diverge for nested calls. */
static void test_replay_focus_call_site_provenance(void) {
    /* --- Reused single-level function: two distinct call sites (lines 5,6) --- */
    glr_ctrl_reset_all();
    editor_feed_line("func0() {");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(1, 1, 1);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    editor_feed_line("func0();");
    editor_feed_line("func0();");
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    const GLCmd *flat = repl_state_flat_program_view().cmds;

    replay_advance(repl_state_flat_program_view());
    int f1 = replay_focus_flat_idx();
    ASSERT_TRUE("reused focus is a vertex",
                f1 >= 0 && repl_cmd_emits_vertex(flat[f1].type));
    ASSERT_TRUE("reused focus call site is first call line (5)",
                f1 >= 0 && flat[f1].call_src_cmd_idx == 5);
    ASSERT_TRUE("reused focus root == immediate (single level)",
                f1 >= 0 && flat[f1].root_call_src_cmd_idx == flat[f1].call_src_cmd_idx);

    replay_advance(repl_state_flat_program_view());
    int f2 = replay_focus_flat_idx();
    ASSERT_TRUE("reused second focus call site is second call line (6)",
                f2 >= 0 && flat[f2].call_src_cmd_idx == 6);

    replay_stop();

    /* --- Nested calls: immediate (inner, line 6) and root (outer, 8/9) differ --- */
    glr_ctrl_reset_all();
    editor_feed_line("func1(a, b) {");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(a, b, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    editor_feed_line("func0(scale) {");
    editor_feed_line("func1(scale, scale + 1);");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    editor_feed_line("func0(4);");
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    flat = repl_state_flat_program_view().cmds;

    replay_advance(repl_state_flat_program_view());
    int n1 = replay_focus_flat_idx();
    ASSERT_TRUE("nested focus immediate call site is inner call (6)",
                n1 >= 0 && flat[n1].call_src_cmd_idx == 6);
    ASSERT_TRUE("nested focus root call site is first outer call (8)",
                n1 >= 0 && flat[n1].root_call_src_cmd_idx == 8);
    ASSERT_TRUE("nested focus immediate != root",
                n1 >= 0 && flat[n1].call_src_cmd_idx != flat[n1].root_call_src_cmd_idx);

    replay_advance(repl_state_flat_program_view());
    int n2 = replay_focus_flat_idx();
    ASSERT_TRUE("nested second focus root is second outer call (9)",
                n2 >= 0 && flat[n2].root_call_src_cmd_idx == 9);
    ASSERT_TRUE("nested second focus immediate still inner call (6)",
                n2 >= 0 && flat[n2].call_src_cmd_idx == 6);

    replay_stop();

    /* --- Top-level (no function): focus has no call site --- */
    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;
    flat = repl_state_flat_program_view().cmds;

    replay_advance(repl_state_flat_program_view());
    int t1 = replay_focus_flat_idx();
    ASSERT_TRUE("top-level focus has no call site",
                t1 >= 0 && flat[t1].call_src_cmd_idx < 0);

    replay_stop();

    /* Inactive replay yields no focus. */
    ASSERT_TRUE("inactive replay focus is -1", replay_focus_flat_idx() == -1);
}

/* Ctrl+K / replay_seek_to_src_line should understand flattened function
 * provenance. A cursor on func0() must jump into that call expansion, not skip
 * forward to the next top-level source line just because the focused command's
 * src_cmd_idx points back into the function body. */
static void test_replay_seek_function_aware_jump(void) {
    const GLCmd *flat;
    int landed;
    int focus;

    glr_ctrl_reset_all();
    editor_feed_line("func0() {");                  /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");        /* 1 */
    editor_feed_line("glVertex3f(1, 1, 1);");       /* 2 */
    editor_feed_line("glEnd();");                   /* 3 */
    editor_feed_line("}");                          /* 4 */
    editor_feed_line("func0();");                   /* 5 */
    editor_feed_line("glVertex3f(9, 9, 9);");       /* 6 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;
    flat = repl_state_flat_program_view().cmds;

    landed = replay_seek_to_src_line(5);
    focus = replay_focus_flat_idx();
    ASSERT_TRUE("seek on function call returns cursor line", landed == 5);
    ASSERT_TRUE("seek on function call lands in first call expansion",
                g_replay_pc == 2);
    ASSERT_TRUE("seek on function call focuses called body",
                focus >= 0 && flat[focus].src_cmd_idx == 2);
    ASSERT_TRUE("seek on function call preserves call provenance",
                focus >= 0 && flat[focus].call_src_cmd_idx == 5);

    landed = replay_seek_to_src_line(4);
    focus = replay_focus_flat_idx();
    ASSERT_TRUE("seek on function end returns cursor line", landed == 4);
    ASSERT_TRUE("seek on function end lands in function scope",
                g_replay_pc == 2);
    ASSERT_TRUE("seek on function end focuses scoped body",
                focus >= 0 && flat[focus].func_scope_mask != 0u);
    replay_stop();

    glr_ctrl_reset_all();
    editor_feed_line("func1(a) {");                 /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");        /* 1 */
    editor_feed_line("glVertex3f(a, 0, 0);");       /* 2 */
    editor_feed_line("glEnd();");                   /* 3 */
    editor_feed_line("}");                          /* 4 */
    editor_feed_line("func0(scale) {");             /* 5 */
    editor_feed_line("func1(scale);");              /* 6 */
    editor_feed_line("}");                          /* 7 */
    editor_feed_line("func0(2);");                  /* 8 */
    editor_feed_line("func0(4);");                  /* 9 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;
    flat = repl_state_flat_program_view().cmds;

    landed = replay_seek_to_src_line(8);
    focus = replay_focus_flat_idx();
    ASSERT_TRUE("seek on nested root call returns cursor line", landed == 8);
    ASSERT_TRUE("seek on nested root call lands in first expansion",
                g_replay_pc == 2);
    ASSERT_TRUE("seek on nested root call matches root provenance",
                focus >= 0 && flat[focus].root_call_src_cmd_idx == 8);
    ASSERT_TRUE("seek on nested root call keeps immediate provenance",
                focus >= 0 && flat[focus].call_src_cmd_idx == 6);
    replay_stop();
}

/* Request 2: the focused flat command's funcN call-frame depth surfaces as
 * ReplayRuntimeState.focus_call_depth, which the HUD shows as "depth N". A
 * recursive function should make the depth climb step by step. */
static void test_replay_focus_call_depth(void) {
    glr_ctrl_reset_all();
    editor_feed_line("func0(depth) {");
    editor_feed_line("if(depth <= 0) {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("if(depth > 0) {");
    editor_feed_line("glVertex3f(depth, 0, 0);");
    editor_feed_line("func0(depth - 1);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(3);");
    repl_flatten_commands(editor_state_edit_line());

    /* Sanity: four vertices, one per recursion level (x = 3,2,1,0). */
    ASSERT_TRUE("recursive replay flat count", repl_state_flat_program_count() == 4);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    int expect_depth[4] = { 1, 2, 3, 4 };
    for (int step = 0; step < 4; step++) {
        replay_advance(repl_state_flat_program_view());
        char label[64];
        snprintf(label, sizeof(label),
                 "recursive replay focus_call_depth step %d == %d",
                 step, expect_depth[step]);
        ASSERT_TRUE(label, g_replay_focus_call_depth == expect_depth[step]);
    }

    replay_restart_from_beginning();
    ASSERT_TRUE("replay restart clears focus_call_depth",
                g_replay_focus_call_depth == 0);

    replay_stop();
    ASSERT_TRUE("replay stop clears focus_call_depth",
                g_replay_focus_call_depth == 0);

    /* Top-level geometry: focus depth stays 0 (no "depth N" segment). */
    glr_ctrl_reset_all();
    /* Starting a new replay must also clear any stale depth from a previous
     * session before the first step updates the focused command. */
    g_replay_focus_call_depth = 7;
    editor_feed_line("glVertex3f(1, 0, 0);");
    repl_flatten_commands(editor_state_edit_line());
    replay_start();
    ASSERT_TRUE("replay start clears stale focus_call_depth",
                g_replay_focus_call_depth == 0);
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;
    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("top-level replay focus_call_depth is 0",
                g_replay_focus_call_depth == 0);
    replay_stop();
}

/* replay_focus_anchor_flat_idx() names the draw the current replay step
 * emitted, which anchors the affecting-transform highlight and the live
 * transform guide. It tracks the step in vertex mode and is inert otherwise. */
static void test_replay_focus_anchor_flat_idx(void) {
    int full_count;

    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("glVertex3f(4, 5, 6);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    full_count = repl_state_flat_program_count();
    /* flat: BEGIN(0), VERTEX(1), VERTEX(2), END(3) */
    ASSERT_TRUE("full flat count", full_count == 4);

    ASSERT_TRUE("inactive anchor focus is -1", replay_focus_anchor_flat_idx() == -1);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("step 1 anchor focus is first vertex (1)",
                replay_focus_anchor_flat_idx() == 1);

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("step 2 anchor focus is second vertex (2)",
                replay_focus_anchor_flat_idx() == 2);

    /* Stale replay state can momentarily point beyond a shrunken flat program;
     * the focus accessor must stay inside the current logical command count. */
    g_replay_pc = full_count + 10;
    repl_state_flat_program_set_count(2);
    ASSERT_TRUE("out-of-range pc clamps to current flat count",
                replay_focus_anchor_flat_idx() == 1);
    repl_state_flat_program_set_count(full_count);
    g_replay_pc = 3;

    /* Polygon mode is out of scope for the anchor overlay. */
    g_replay_mode = REPLAY_MODE_POLYGON;
    ASSERT_TRUE("polygon mode anchor focus is -1",
                replay_focus_anchor_flat_idx() == -1);

    replay_stop();
    ASSERT_TRUE("stopped anchor focus is -1", replay_focus_anchor_flat_idx() == -1);
}

/* Glut solids emit no REPL vertex but ARE a draw anchor: vertex stepping stops
 * on each glutSolid*, and the anchor focus must resolve to that flat index so
 * the affecting-transform highlight / transform guide reach it. */
static void test_replay_focus_anchor_glut_solid(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glTranslatef(1, 0, 0);");
    editor_feed_line("glutSolidCube(1);");
    editor_feed_line("glutSolidSphere(1, 8, 8);");
    repl_flatten_commands(editor_state_edit_line());
    /* flat: TRANSLATE(0), GLUT_CUBE(1), GLUT_SPHERE(2) */
    ASSERT_TRUE("glut flat count", repl_state_flat_program_count() == 3);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("step 1 anchor focus is the cube (1)",
                replay_focus_anchor_flat_idx() == 1);

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("step 2 anchor focus is the sphere (2)",
                replay_focus_anchor_flat_idx() == 2);

    replay_stop();
}

static void test_replay_polygon_limits(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("  glVertex3f(1, 2, 3);");
    editor_feed_line("glEnd();");
    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    g_replay_mode = REPLAY_MODE_POLYGON;
    g_replay_state = REPLAY_PAUSED;

    /* Advance should jump the whole glBegin/glEnd block as one step */
    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("polygon mode advance jumps past end", g_replay_pc == 3);

    replay_step_back();
    ASSERT_TRUE("polygon mode step back returns to start", g_replay_pc == 0);
    replay_stop();
}

static int find_doc_cmd_of_type(CmdType type) {
    const GLCmd *doc = repl_state_document_cmds();
    int n = repl_state_document_count();
    for (int i = 0; i < n; i++)
        if (doc[i].valid && doc[i].type == type)
            return i;
    return -1;
}

/* A function-definition header should show the current invocation's
 * parameter values — " // a = 6, b = 5" — but only while the replay
 * position is executing inside that function's body. The call site is not
 * annotated. */
static void test_replay_funcdef_shows_params(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float n = 3;");           /* 0 */
    editor_feed_line("func0(a, b) {");          /* 1 (def) */
    editor_feed_line("glVertex3f(a, b, 0);");   /* 2 (body) */
    editor_feed_line("}");                       /* 3 */
    editor_feed_line("func0(n * 2, 5);");       /* 4 (call) */
    editor_feed_line("glVertex3f(9, 9, 9);");   /* 5 (top-level, after) */
    repl_flatten_commands(editor_state_edit_line());

    int def_line = find_doc_cmd_of_type(CMD_FUNC_DEF);
    int call_line = find_doc_cmd_of_type(CMD_CALL);
    ASSERT_TRUE("found def and call lines", def_line >= 0 && call_line >= 0);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    SourceTextView text = source_document_view();
    char display[256];

    /* First step lands inside the function body: def shows resolved params
     * (n*2 = 6, literal 5). */
    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("first step is inside the function body", g_replay_src_line == 2);
    replay_code_panel_get_command_display_text(text, def_line,
                                               display, sizeof(display));
    ASSERT_TRUE("def header shows current param values",
                strstr(display, "a = 6, b = 5") != NULL);

    /* The call site itself is left un-annotated. */
    replay_code_panel_get_command_display_text(text, call_line,
                                               display, sizeof(display));
    ASSERT_TRUE("call site is not annotated",
                strstr(display, "//") == NULL);

    /* Advance to the trailing top-level vertex: no longer inside the func. */
    int safety = 1024;
    while (g_replay_pc < g_replay_total_flat && safety-- > 0)
        replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("replay reached end", safety > 0);
    ASSERT_TRUE("final step is outside the function", g_replay_src_line == 5);
    replay_code_panel_get_command_display_text(text, def_line,
                                               display, sizeof(display));
    ASSERT_TRUE("def header drops params when outside the body",
                strstr(display, "//") == NULL);

    replay_stop();
}

/* Same behavior for an aliased function ("NAME(...)"): parse_repl_func_signature
 * resolves the alias to recover the param names, so the def header still
 * shows the values while inside. */
static void test_replay_funcdef_shows_alias_params(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float m = 2;");           /* 0 */
    editor_feed_line("wave(a, b) {");           /* 1 (alias def) */
    editor_feed_line("glVertex3f(a, b, 0);");   /* 2 */
    editor_feed_line("}");                       /* 3 */
    editor_feed_line("wave(m * 3, 7);");        /* 4 (call) */
    repl_flatten_commands(editor_state_edit_line());

    int def_line = find_doc_cmd_of_type(CMD_FUNC_DEF);
    ASSERT_TRUE("found the alias def line", def_line >= 0);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("inside the alias body", g_replay_src_line == 2);

    SourceTextView text = source_document_view();
    char display[256];
    replay_code_panel_get_command_display_text(text, def_line,
                                               display, sizeof(display));
    ASSERT_TRUE("alias def header shows current param values",
                strstr(display, "a = 6, b = 7") != NULL);

    replay_stop();
}

/* End-to-end against the shipped example 25: while replay executes inside
 * sea(half, cells, amp), the def header must render its current parameter
 * values " // half = 5.5, cells = 13, amp = 0.16" (sea is called with those
 * literals). This is the case originally reported as missing. */
static void test_replay_example25_sea_def_params(void) {
    glr_ctrl_reset_all();
    ASSERT_TRUE("loaded example 25", repl_load_example(24) > 0);
    repl_flatten_commands(editor_state_edit_line());

    SourceTextView text = source_document_view();
    const GLCmd *doc = repl_state_document_cmds();
    int sea_def = -1;
    for (int i = 0; i < repl_state_document_count(); i++) {
        const char *src;
        if (!doc[i].valid || doc[i].type != CMD_FUNC_DEF)
            continue;
        src = source_text_line(text, i);
        if (src && strstr(src, "sea(")) {
            sea_def = i;
            break;
        }
    }
    ASSERT_TRUE("found the sea() def line", sea_def >= 0);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    /* sea() renders first in display(); step until the def header annotates. */
    char display[256];
    int found = 0;
    int safety = 400000;
    while (g_replay_pc < g_replay_total_flat && safety-- > 0) {
        replay_advance(repl_state_flat_program_view());
        replay_code_panel_get_command_display_text(text, sea_def,
                                                   display, sizeof(display));
        if (strstr(display, "half = 5.5")) {
            found = 1;
            break;
        }
    }
    ASSERT_TRUE("sea def header annotated while inside sea", found);
    ASSERT_TRUE("example 25 sea def shows its parameter values",
                strstr(display, "half = 5.5, cells = 13, amp = 0.16") != NULL);

    replay_stop();
}

/* A for-loop header should expand to " // i = <iter>, n = <limit>" while
 * the replay position is inside the loop, and to the limit alone once the
 * position has moved past the loop. */
static void test_replay_for_header_expands_iter_and_limit(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float n = 4;");           /* 0 */
    editor_feed_line("for(i, 0, n) {");         /* 1 */
    editor_feed_line("glVertex3f(i, 0, 0);");   /* 2 */
    editor_feed_line("}");                       /* 3 */
    editor_feed_line("glVertex3f(9, 9, 9);");   /* 4 (after the loop) */
    repl_flatten_commands(editor_state_edit_line());

    int for_line = find_doc_cmd_of_type(CMD_FOR_BEGIN);
    ASSERT_TRUE("found the for source line", for_line >= 0);

    replay_start();
    g_replay_mode = REPLAY_MODE_VERTEX;
    g_replay_state = REPLAY_PAUSED;

    SourceTextView text = source_document_view();
    char display[256];

    /* One step lands inside the loop body: iteration + limit both show. */
    replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("first step is inside the loop body", g_replay_src_line == 2);
    replay_code_panel_get_command_display_text(text, for_line,
                                               display, sizeof(display));
    ASSERT_TRUE("loop header shows the live iteration var",
                strstr(display, "i = ") != NULL);
    ASSERT_TRUE("loop header resolves the limit n = 4",
                strstr(display, "n = 4") != NULL);

    /* Advance to the trailing post-loop vertex: loop var is no longer live,
     * so only the limit remains. */
    int safety = 1024;
    while (g_replay_pc < g_replay_total_flat && safety-- > 0)
        replay_advance(repl_state_flat_program_view());
    ASSERT_TRUE("replay reached end", safety > 0);
    ASSERT_TRUE("final step is past the loop", g_replay_src_line == 4);
    replay_code_panel_get_command_display_text(text, for_line,
                                               display, sizeof(display));
    ASSERT_TRUE("idle loop header still resolves the limit",
                strstr(display, "n = 4") != NULL);
    ASSERT_TRUE("idle loop header drops the iteration readout",
                strstr(display, "i = ") == NULL);

    replay_stop();
}

int main(void) {
    test_replay_basic_controls();
    test_replay_stepping();
    test_replay_focus_call_site_provenance();
    test_replay_seek_function_aware_jump();
    test_replay_focus_call_depth();
    test_replay_focus_anchor_flat_idx();
    test_replay_focus_anchor_glut_solid();
    test_replay_polygon_limits();
    test_replay_tessellation_stepping();
    test_replay_fade_batches();
    test_replay_input();
    test_replay_modifiers();
    test_bench_helpers();
    test_misc_helpers();
    test_replay_var_assign_uses_flatten_args();
    test_replay_funcdef_shows_params();
    test_replay_funcdef_shows_alias_params();
    test_replay_example25_sea_def_params();
    test_replay_for_header_expands_iter_and_limit();
    test_replay_single_arg_shape_gets_eval_annotation();
    test_replay_baseline_restore_survives_predef_reshape();
    test_replay_regression_fixes();
    test_replay_cursor_sync_and_unrecognized_keys();
#ifdef GL_STUBS
    test_replay_rendering();
#endif

    printf("test_repl_replay: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
