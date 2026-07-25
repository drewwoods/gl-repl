/*
 * test_replay_walk.c - regression tests for replay_walk_user_vertices.
 *
 * These tests guard the invariants the cursor-edit-guide stack depends
 * on:
 *
 *   1. The walker fires `on_each_cmd` at the cursor's first flat
 *      occurrence even when no transform guide is active. Earlier a
 *      fast-path in glr_ctrl_render_cursor_guides skipped the walk in
 *      that case, so the geometry guide ran with the caller's
 *      (un-transformed) modelview — guide marker landed at world
 *      origin instead of at the vertex.
 *
 *   2. The flat command's `args[]` carry the *evaluated* coordinates
 *      after funcN inlining (with the call site's parameter values
 *      substituted in). The cursor-guide fix overrides the input-text
 *      snapshot args with these, which is only correct if the flat
 *      args are real numeric positions, not zeros.
 *
 *   3. ctx->stop_flag halts the walker. The cursor-guide path uses
 *      stop_flag to bail out as soon as both guides have rendered, so
 *      a regression in stop_flag would silently re-introduce the
 *      O(flat_count) walk every frame.
 *
 * The walker calls glPushMatrix / glTranslatef directly, so this test
 * needs the no-op GL stubs — it lives under the USE_GL_STUBS=1 gate in
 * the Makefile.
 */
#include "app/glr_ctrl.h"
#include "repl/command_spec.h"
#include <math.h>
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "editor/input.h"
#include "repl/state_owners.h"
#include "repl/transform_utils.h"
#include "subsystems/replay/replay.h"
#include "support/test_harness.h"

#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

/* Pulled in as a translation unit so we can call the static
 * cursor_guide_snapshot_with_flat_args helper directly. Same pattern
 * used by tests/test_glr_ctrl.c. */
#include "app/glr_ctrl.c"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_NEAR(label, got, exp) \
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, got, exp)

#define MAX_RECORDED 64

typedef struct {
    int   flat_idx;
    int   src_idx;
    float args[3];
} RecordedCmd;

typedef struct {
    /* Records every flat cmd whose src_cmd_idx matches edit_line_idx,
     * so we can verify the walker reaches the cursor and the args it
     * sees come from the flatten-time evaluation. */
    RecordedCmd cursor_hits[MAX_RECORDED];
    int         cursor_hit_count;

    /* Total cb fires (for stop_flag regression). */
    int cmd_visits;
    int vertex_visits;

    /* Optional: set by on_each_cmd to test early-stop. */
    int   *stop_after_first_cursor;
    int    edit_line_idx;
} Recorder;

static void on_each_cmd_record(const ReplayVertexWalkState *state, void *user) {
    Recorder *rec = (Recorder *)user;
    rec->cmd_visits++;
    if (state->src_cmd_idx == rec->edit_line_idx) {
        if (rec->cursor_hit_count < MAX_RECORDED) {
            RecordedCmd *r = &rec->cursor_hits[rec->cursor_hit_count++];
            r->flat_idx = state->flat_cmd_idx;
            r->src_idx  = state->src_cmd_idx;
            /* args copied below in on_vertex (or filled in by caller via
             * the flat program directly). */
            r->args[0] = r->args[1] = r->args[2] = 0.0f;
        }
        if (rec->stop_after_first_cursor)
            *rec->stop_after_first_cursor = 1;
    }
}

static void on_vertex_record(const ReplayVertexWalkState *state,
                             float vx, float vy, float vz, void *user) {
    Recorder *rec = (Recorder *)user;
    rec->vertex_visits++;
    if (state->src_cmd_idx == rec->edit_line_idx &&
        rec->cursor_hit_count > 0) {
        RecordedCmd *r = &rec->cursor_hits[rec->cursor_hit_count - 1];
        r->args[0] = vx; r->args[1] = vy; r->args[2] = vz;
    }
}

static ReplayVertexWalkContext make_ctx(int edit_line_idx) {
    ReplayVertexWalkContext ctx = {
        .program             = repl_state_flat_program_view(),
        .cursor = {
            .edit_line_idx       = edit_line_idx,
            .cursor_block_begin  = -1,
            .cursor_block_end    = -1,
            .cursor_func_scope_mask = 0,
        },
        .selected_block_only = 0,
        .stop_flag           = NULL,
    };
    return ctx;
}

/* Locate the source-line index of the (commit-sequence-th) commit
 * matching `prefix`. Lets the tests resolve the cursor line without
 * hard-coding indices that change as the program grows. */
static int find_source_line(const char *prefix) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid) continue;
        /* The document doesn't expose the original text directly here;
         * fall back to matching by cmd type for our cases. */
        (void)prefix;
        if (cmds[i].type == CMD_VERTEX3F) return i;
    }
    return -1;
}

/* --- 1. Walker reaches cursor flat-idx with funcN-substituted args -----
 *
 * Program shape:
 *
 *   func0(s, p){ glBegin(GL_TRIANGLES); glVertex3f(s, p, 0); glEnd(); }
 *   glPushMatrix();
 *   glTranslatef(-2, 0, 0);
 *   func0(0.5, 0.25);
 *   glPopMatrix();
 *   glPushMatrix();
 *   glTranslatef(2, 0, 0);
 *   func0(0.75, -0.4);
 *   glPopMatrix();
 *
 * Putting the cursor on `glVertex3f(s, p, 0)` (a line inside func0
 * that references function-local params), the walker should reach
 * the flat cmd at *each* call site with args carrying the
 * call-site's substituted values:
 *
 *   call A: args = (0.5, 0.25, 0)
 *   call B: args = (0.75, -0.4, 0)
 *
 * If those came back as (0, 0, 0) — which is what the input-text
 * parser produces for funcN locals — the cursor guide would render
 * at the local origin and we'd be back in the bug.
 */
static void test_walker_resolves_funcn_args_at_cursor(void) {
    glr_ctrl_reset_all();
    editor_feed_line("func0(s, p){");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(s, p, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glTranslatef(-2, 0, 0);");
    editor_feed_line("func0(0.5, 0.25);");
    editor_feed_line("glPopMatrix();");
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glTranslatef(2, 0, 0);");
    editor_feed_line("func0(0.75, -0.4);");
    editor_feed_line("glPopMatrix();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    int cursor_line = find_source_line("glVertex3f");
    ASSERT_TRUE("found a CMD_VERTEX3F to use as cursor", cursor_line >= 0);
    editor_state_edit_line_set(cursor_line);

    Recorder rec = {0};
    rec.edit_line_idx = cursor_line;

    static const ReplayVertexWalkCallbacks cb = {
        .on_each_cmd = on_each_cmd_record,
        .on_vertex   = on_vertex_record,
    };
    ReplayVertexWalkContext ctx = make_ctx(cursor_line);
    replay_walk_user_vertices(&ctx, &cb, &rec);

    ASSERT_INT("walker reaches cursor at both call sites",
               rec.cursor_hit_count, 2);
    /* Call A: glTranslatef(-2,0,0) then func0(0.5, 0.25) → args (0.5, 0.25, 0). */
    ASSERT_NEAR("call A: cursor flat-cmd args[0] (= scale)",
                rec.cursor_hits[0].args[0], 0.5f);
    ASSERT_NEAR("call A: cursor flat-cmd args[1] (= phase)",
                rec.cursor_hits[0].args[1], 0.25f);
    ASSERT_NEAR("call A: cursor flat-cmd args[2]",
                rec.cursor_hits[0].args[2], 0.0f);
    /* Call B: func0(0.75, -0.4) → args (0.75, -0.4, 0). */
    ASSERT_NEAR("call B: cursor flat-cmd args[0] (= scale)",
                rec.cursor_hits[1].args[0], 0.75f);
    ASSERT_NEAR("call B: cursor flat-cmd args[1] (= phase)",
                rec.cursor_hits[1].args[1], -0.4f);

    /* Crucial regression guard: if the cursor-guide path ever falls
     * back to the predef-only text parser for vertex args, those args
     * collapse to (0,0,0). Assert nonzero magnitude. */
    ASSERT_TRUE("flat args carry resolved funcN params, not zero",
                rec.cursor_hits[0].args[0] != 0.0f &&
                rec.cursor_hits[1].args[0] != 0.0f);
}

/* --- 2. on_each_cmd fires at cursor even with no transform guide -------
 *
 * Guards the 23c9e1a fast-path regression: a program with no transform
 * cursor and no replay would previously short-circuit the walker.
 * The cursor-guide path now always walks, so on_each_cmd MUST fire at
 * the cursor's flat-cmd index.
 */
static void test_walker_fires_on_each_cmd_at_cursor(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glTranslatef(1, 2, 3);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0.5, 0.5, 0);");
    editor_feed_line("glVertex3f(0.7, 0.7, 0);");
    editor_feed_line("glVertex3f(0.9, 0.9, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glPopMatrix();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    int cursor_line = find_source_line("glVertex3f");
    ASSERT_TRUE("found cursor line", cursor_line >= 0);
    editor_state_edit_line_set(cursor_line);

    Recorder rec = {0};
    rec.edit_line_idx = cursor_line;

    static const ReplayVertexWalkCallbacks cb = {
        .on_each_cmd = on_each_cmd_record,
        .on_vertex   = on_vertex_record,
    };
    ReplayVertexWalkContext ctx = make_ctx(cursor_line);
    replay_walk_user_vertices(&ctx, &cb, &rec);

    ASSERT_TRUE("on_each_cmd fires at least once for cursor src",
                rec.cursor_hit_count >= 1);
    ASSERT_NEAR("cursor flat-cmd args[0] preserved",
                rec.cursor_hits[0].args[0], 0.5f);
    ASSERT_NEAR("cursor flat-cmd args[1] preserved",
                rec.cursor_hits[0].args[1], 0.5f);
}

/* --- 3. stop_flag halts the walker -------------------------------------
 *
 * Two sub-cases:
 *   (a) Pre-set stop_flag: walker exits after at most one on_each_cmd
 *       fire, with no vertex visits.
 *   (b) on_each_cmd sets stop_flag when it hits the cursor: walker
 *       does NOT continue past the cursor flat-cmd.
 *
 * The cursor-guide path uses pattern (b) so a regression in stop_flag
 * would silently restore the O(flat_count) walk every frame.
 */
static void test_walker_stop_flag_halts(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glTranslatef(0, 1, 0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0.1, 0.2, 0);");
    editor_feed_line("glVertex3f(0.3, 0.4, 0);");
    editor_feed_line("glVertex3f(0.5, 0.6, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glPopMatrix();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    int cursor_line = find_source_line("glVertex3f");
    ASSERT_TRUE("found cursor line", cursor_line >= 0);
    editor_state_edit_line_set(cursor_line);

    static const ReplayVertexWalkCallbacks cb = {
        .on_each_cmd = on_each_cmd_record,
        .on_vertex   = on_vertex_record,
    };

    /* Baseline: no stop_flag → walker emits all three vertices. */
    Recorder rec_full = { .edit_line_idx = cursor_line };
    ReplayVertexWalkContext ctx_full = make_ctx(cursor_line);
    replay_walk_user_vertices(&ctx_full, &cb, &rec_full);
    ASSERT_INT("baseline: walker emits all 3 vertices",
               rec_full.vertex_visits, 3);

    /* (a) Pre-set stop_flag=1: walker breaks before doing any work. */
    int stop = 1;
    Recorder rec_pre = { .edit_line_idx = cursor_line };
    ReplayVertexWalkContext ctx_pre = make_ctx(cursor_line);
    ctx_pre.stop_flag = &stop;
    replay_walk_user_vertices(&ctx_pre, &cb, &rec_pre);
    ASSERT_INT("stop_flag=1 emits zero vertices",
               rec_pre.vertex_visits, 0);
    ASSERT_TRUE("stop_flag=1 fires on_each_cmd at most once",
                rec_pre.cmd_visits <= 1);

    /* (b) on_each_cmd sets stop_flag once it sees the cursor. The
     * walker should stop emitting vertices from THAT POINT onward
     * (vertices before the cursor in the flat program may still have
     * fired, but vertices AFTER the cursor should not). */
    int stop_b = 0;
    Recorder rec_b = { .edit_line_idx = cursor_line,
                       .stop_after_first_cursor = &stop_b };
    ReplayVertexWalkContext ctx_b = make_ctx(cursor_line);
    ctx_b.stop_flag = &stop_b;
    replay_walk_user_vertices(&ctx_b, &cb, &rec_b);

    ASSERT_TRUE("dynamic stop_flag fires on_each_cmd at the cursor",
                rec_b.cursor_hit_count == 1);
    /* The first cursor hit is at the first glVertex3f; the walker
     * stops there, so it should not have visited the later glVertex3f
     * lines (vertex_visits at most 1). */
    ASSERT_TRUE("dynamic stop_flag halts walk after cursor",
                rec_b.vertex_visits <= 1);
    ASSERT_TRUE("dynamic stop_flag walks fewer cmds than baseline",
                rec_b.cmd_visits < rec_full.cmd_visits);
}

/* --- 4. cursor_guide_snapshot_with_flat_args overrides correctly -------
 *
 * Direct unit test of the helper that the cursor-guide path uses to
 * fix up vertex_args before handing them to
 * render3d_geometry_guides_render_for_cursor. This is the actual fix for the
 * "guide rendered at object center" bug: if someone reverts the helper
 * to just `return *snapshot`, this test fails.
 */
static void test_cursor_guide_snapshot_override(void) {
    Render3dGuideSnapshot base = {0};
    base.vertex_args[0] = 0.0f;  /* what the predef-only text parser
                                  * would write for a funcN-local
                                  * expression like `cos(phase)*scale` */
    base.vertex_args[1] = 0.0f;
    base.vertex_args[2] = 0.0f;
    base.vertex_n_filled = 3;
    base.vertex_filled[0] = base.vertex_filled[1] = base.vertex_filled[2] = 1;

    GLCmd vertex3f = {0};
    vertex3f.type = CMD_VERTEX3F;
    vertex3f.valid = 1;
    vertex3f.args[0] = 0.42f;
    vertex3f.args[1] = -0.17f;
    vertex3f.args[2] = 0.99f;

    Render3dGuideSnapshot snap =
        cursor_guide_snapshot_with_flat_args(&base, &vertex3f, -1);
    ASSERT_NEAR("VERTEX3F: x overridden from flat args",
                snap.vertex_args[0], 0.42f);
    ASSERT_NEAR("VERTEX3F: y overridden from flat args",
                snap.vertex_args[1], -0.17f);
    ASSERT_NEAR("VERTEX3F: z overridden from flat args",
                snap.vertex_args[2], 0.99f);

    /* VERTEX2F: z should be forced to 0 regardless of flat->args[2]. */
    GLCmd vertex2f = {0};
    vertex2f.type = CMD_VERTEX2F;
    vertex2f.valid = 1;
    vertex2f.args[0] = 0.5f;
    vertex2f.args[1] = 0.5f;
    vertex2f.args[2] = 9999.0f;  /* garbage; helper should ignore it */
    Render3dGuideSnapshot snap2 =
        cursor_guide_snapshot_with_flat_args(&base, &vertex2f, -1);
    ASSERT_NEAR("VERTEX2F: x overridden", snap2.vertex_args[0], 0.5f);
    ASSERT_NEAR("VERTEX2F: y overridden", snap2.vertex_args[1], 0.5f);
    ASSERT_NEAR("VERTEX2F: z forced to 0", snap2.vertex_args[2], 0.0f);

    /* TESS_VERTEX: should also pass through args (path goes through
     * draw_vertex_guides like glVertex). */
    GLCmd tess = {0};
    tess.type = CMD_TESS_VERTEX;
    tess.valid = 1;
    tess.args[0] = -1.2f; tess.args[1] = 0.45f; tess.args[2] = -0.5f;
    Render3dGuideSnapshot snap3 =
        cursor_guide_snapshot_with_flat_args(&base, &tess, -1);
    ASSERT_NEAR("TESS_VERTEX: x", snap3.vertex_args[0], -1.2f);
    ASSERT_NEAR("TESS_VERTEX: y", snap3.vertex_args[1],  0.45f);
    ASSERT_NEAR("TESS_VERTEX: z", snap3.vertex_args[2], -0.5f);

    /* NORMAL3F: override normal_args (not vertex_args) since cursor is
     * on a glNormal3f line. Same root cause as the vertex bug — text
     * parser collapses funcN-local refs to 0; we recover from flat
     * cmd's evaluated args. */
    GLCmd nrm = {0};
    nrm.type = CMD_NORMAL3F;
    nrm.valid = 1;
    nrm.args[0] = 0.0f; nrm.args[1] = 1.0f; nrm.args[2] = 0.0f;
    base.normal_args[0] = base.normal_args[1] = base.normal_args[2] = 0.0f;
    Render3dGuideSnapshot snap_n =
        cursor_guide_snapshot_with_flat_args(&base, &nrm, -1);
    ASSERT_NEAR("NORMAL3F: nx overridden", snap_n.normal_args[0], 0.0f);
    ASSERT_NEAR("NORMAL3F: ny overridden", snap_n.normal_args[1], 1.0f);
    ASSERT_NEAR("NORMAL3F: nz overridden", snap_n.normal_args[2], 0.0f);
    /* vertex_args should NOT be touched when cursor is on a normal. */
    ASSERT_NEAR("NORMAL3F: vertex_args x untouched",
                snap_n.vertex_args[0], base.vertex_args[0]);
    /* normal_base_pos stays invalid when no flat program / index. */
    ASSERT_INT("NORMAL3F: base_pos invalid without flat",
               snap_n.normal_base_pos_valid, 0);

    /* NORMAL3F + flat program with a following vertex: base_pos picks
     * up the next vertex's args from FLAT (not source), so the normal
     * arrow tracks the dynamic position even when the surrounding
     * vars are reassigned per loop iteration. */
    GLCmd flat_seq[3] = {0};
    flat_seq[0].type = CMD_NORMAL3F; flat_seq[0].valid = 1;
    flat_seq[0].args[0] = 0; flat_seq[0].args[1] = 1; flat_seq[0].args[2] = 0;
    flat_seq[1].type = CMD_COLOR3F;  flat_seq[1].valid = 1;
    flat_seq[2].type = CMD_VERTEX3F; flat_seq[2].valid = 1;
    flat_seq[2].args[0] = -1.5f; flat_seq[2].args[1] = 0.42f; flat_seq[2].args[2] = 0.25f;

    Render3dGuideSnapshot base_with_flat = base;
    base_with_flat.flat_program.cmds      = flat_seq;
    base_with_flat.flat_program.cmd_count = 3;

    Render3dGuideSnapshot snap_n2 =
        cursor_guide_snapshot_with_flat_args(&base_with_flat, &flat_seq[0], 0);
    ASSERT_INT("NORMAL3F: base_pos valid when next flat vertex exists",
               snap_n2.normal_base_pos_valid, 1);
    ASSERT_NEAR("NORMAL3F: base_pos x from flat vertex",
                snap_n2.normal_base_pos[0], -1.5f);
    ASSERT_NEAR("NORMAL3F: base_pos y from flat vertex",
                snap_n2.normal_base_pos[1], 0.42f);
    ASSERT_NEAR("NORMAL3F: base_pos z from flat vertex",
                snap_n2.normal_base_pos[2], 0.25f);

    /* Block boundary between normal and vertex → base_pos stays
     * invalid (the vertex doesn't belong to this normal's block). */
    GLCmd flat_blocked[3] = {0};
    flat_blocked[0].type = CMD_NORMAL3F; flat_blocked[0].valid = 1;
    flat_blocked[1].type = CMD_END;      flat_blocked[1].valid = 1;
    flat_blocked[2].type = CMD_VERTEX3F; flat_blocked[2].valid = 1;
    flat_blocked[2].args[0] = 99.0f;

    Render3dGuideSnapshot base_blocked = base;
    base_blocked.flat_program.cmds      = flat_blocked;
    base_blocked.flat_program.cmd_count = 3;

    Render3dGuideSnapshot snap_blocked =
        cursor_guide_snapshot_with_flat_args(&base_blocked, &flat_blocked[0], 0);
    ASSERT_INT("NORMAL3F: CMD_END stops the forward search",
               snap_blocked.normal_base_pos_valid, 0);

    /* TESS_NORMAL: same as NORMAL3F. */
    GLCmd tnorm = {0};
    tnorm.type = CMD_TESS_NORMAL;
    tnorm.valid = 1;
    tnorm.args[0] = 1.0f; tnorm.args[1] = 0.0f; tnorm.args[2] = 0.0f;
    Render3dGuideSnapshot snap_tn =
        cursor_guide_snapshot_with_flat_args(&base, &tnorm, -1);
    ASSERT_NEAR("TESS_NORMAL: nx", snap_tn.normal_args[0], 1.0f);
    ASSERT_NEAR("TESS_NORMAL: ny", snap_tn.normal_args[1], 0.0f);
    ASSERT_NEAR("TESS_NORMAL: nz", snap_tn.normal_args[2], 0.0f);

    /* Non-vertex/non-normal cmd: snapshot should pass through unchanged. */
    GLCmd translate = {0};
    translate.type = CMD_TRANSLATE3F;
    translate.valid = 1;
    translate.args[0] = 5.0f; translate.args[1] = 5.0f; translate.args[2] = 5.0f;
    Render3dGuideSnapshot snap4 =
        cursor_guide_snapshot_with_flat_args(&base, &translate, -1);
    ASSERT_NEAR("non-vertex/normal cmd: vx untouched",
                snap4.vertex_args[0], base.vertex_args[0]);
    ASSERT_NEAR("non-vertex/normal cmd: vy untouched",
                snap4.vertex_args[1], base.vertex_args[1]);
    ASSERT_NEAR("non-vertex/normal cmd: vz untouched",
                snap4.vertex_args[2], base.vertex_args[2]);
    ASSERT_NEAR("non-vertex/normal cmd: nx untouched",
                snap4.normal_args[0], base.normal_args[0]);

    /* NULL flat (e.g. cursor flat_cmd_idx out of bounds): also a
     * pass-through. */
    Render3dGuideSnapshot snap5 =
        cursor_guide_snapshot_with_flat_args(&base, NULL, -1);
    ASSERT_NEAR("NULL flat cmd: x untouched",
                snap5.vertex_args[0], base.vertex_args[0]);
}

/* --- 5. parse_arg_slots handles nested parens ------------------
 *
 * Regression for the "wrong guide branch on funcN locals" bug:
 * glVertex3f(cos(i*TAU/sides + phase)*radius,
 *            sin(i*TAU/sides + phase)*radius, 0)
 * collapses to n_filled=1 if the slot scanner stops at the first inner
 * `)`. The cursor guide then takes the n==1 branch and draws the YZ
 * plane instead of a red point at the vertex.
 *
 * Note: this tests parse_arg_slots (static in glr_ctrl.c, reached
 * via the include-as-unit pattern at the top of this file). With no
 * predefined-variable values, the args evaluate to 0 — that's the
 * normal outcome for funcN-local references; n_filled=3 is what we
 * actually care about so the n==3 branch fires and the cursor-guide
 * snapshot override (test #4) provides correct positions.
 */
static void test_parse_arg_slots_nested_parens(void) {
    float out[3] = { 99.0f, 99.0f, 99.0f };
    int   filled[3] = { 0, 0, 0 };

    /* The exact pattern from g_example_ring's glVertex3f line, sans the
     * "glVertex3f(" prefix (which the caller strips before invoking
     * parse_arg_slots). */
    int n = parse_arg_slots(
        "cos(i*TAU/sides + phase)*radius, "
        "sin(i*TAU/sides + phase)*radius, 0)",
        NULL, 0, out, filled, 3);

    ASSERT_INT("nested parens: all 3 slots filled", n, 3);
    ASSERT_INT("nested parens: filled[0]", filled[0], 1);
    ASSERT_INT("nested parens: filled[1]", filled[1], 1);
    ASSERT_INT("nested parens: filled[2]", filled[2], 1);
    ASSERT_NEAR("nested parens: slot 2 is the literal 0",
                out[2], 0.0f);

    /* Sanity: a single-arg input still parses as 1. */
    out[0] = 99.0f;
    filled[0] = filled[1] = filled[2] = 0;
    n = parse_arg_slots("0.5)", NULL, 0, out, filled, 3);
    ASSERT_INT("single literal: n_filled", n, 1);
    ASSERT_NEAR("single literal: out[0]", out[0], 0.5f);
    ASSERT_INT("single literal: filled[1] still 0", filled[1], 0);

    /* Sanity: nested literal expression doesn't trip the scanner. */
    out[0] = out[1] = out[2] = 99.0f;
    filled[0] = filled[1] = filled[2] = 0;
    n = parse_arg_slots("(1+2), (3*4), (5))", NULL, 0, out, filled, 3);
    ASSERT_INT("parenthesised args: n_filled", n, 3);
    ASSERT_NEAR("parenthesised args: out[0]", out[0], 3.0f);
    ASSERT_NEAR("parenthesised args: out[1]", out[1], 12.0f);
    ASSERT_NEAR("parenthesised args: out[2]", out[2], 5.0f);
}

/* --- 6. predicate/category agreement (drift guard) --------------------
 *
 * src/repl/command.h holds inline control-flow predicates
 * (repl_cmd_is_transform, repl_cmd_emits_vertex). src/repl/command_spec.h
 * holds a CmdSyntaxCategory enum used for code-panel syntax highlighting.
 * Both name the same sets of types from different angles.
 *
 * They're intentionally on separate axes (visual taxonomy vs control-flow
 * shape) but for the predicates where a corresponding category exists,
 * the two MUST agree on which types fall in the set. If somebody adds a
 * new vertex-emitting command type, updates the spec table to
 * CMD_CAT_VERTEX, but forgets the predicate (or vice versa), this test
 * pins the discrepancy down at the responsible CmdType.
 *
 * Not every predicate has a category twin: repl_cmd_is_block_head /
 * _is_block_end span three CmdSyntaxCategory values each, so no single
 * category equals those predicates. Those don't get a drift test.
 */
static void test_predicate_category_agreement(void) {
    for (int t_int = 0; t_int < CMD_TYPE_COUNT; t_int++) {
        CmdType t = (CmdType)t_int;
        CmdSyntaxCategory cat = repl_cmd_type_category(t);

        int pred_vertex = repl_cmd_emits_vertex(t);
        int cat_vertex  = (cat == CMD_CAT_VERTEX);
        char label[96];
        snprintf(label, sizeof(label),
                 "type=%d: emits_vertex predicate matches CMD_CAT_VERTEX",
                 t_int);
        ASSERT_INT(label, pred_vertex, cat_vertex);

        int pred_xform = repl_cmd_is_transform(t);
        int cat_xform  = (cat == CMD_CAT_TRANSFORM);
        snprintf(label, sizeof(label),
                 "type=%d: is_transform predicate matches CMD_CAT_TRANSFORM",
                 t_int);
        ASSERT_INT(label, pred_xform, cat_xform);
    }
}

static void test_display_name_for_not_in_begin(void) {
    for (int t_int = 0; t_int < CMD_TYPE_COUNT; t_int++) {
        CmdType t = (CmdType)t_int;
        const ReplCommandTypeSpec *spec = repl_command_type_spec(t);
        if (!spec)
            continue;
        if (spec->valid_in_begin)
            continue;
        char label[128];
        snprintf(label, sizeof(label),
                 "type=%d (%s): not-in-begin command has display_name",
                 t_int, spec->name);
        ASSERT_TRUE(label, spec->display_name != NULL);
    }
}

/* --- 7. transform-dispatch drift guard --------------------------------
 *
 * Two parallel switches dispatch transform CmdTypes:
 *   - apply_tracked_transform (repl/transform_utils.h) — used by the
 *     replay walkers, edit-overlay walks, and transform-guide pre-passes
 *   - repl_executor_apply_tracked_transform_cmd (repl/executor.c) — used
 *     by the live executor and replay paths
 *
 * Both must cover the same set of CmdTypes as repl_cmd_is_transform —
 * any drift between the three is a silent guide-position bug. Detection:
 * GL stub counters tick exactly one of the six transform entrypoints
 * (glPushMatrix/glPopMatrix/glLoadIdentity/glTranslatef/glScalef/
 * glRotatef) when a transform CmdType is dispatched; non-transform
 * CmdTypes tick none.
 *
 * If a new transform CmdType lands and only the executor's switch
 * grows a branch (or vice versa), this test pinpoints the offending
 * type and the offending dispatcher.
 */
static void test_transform_dispatch_drift_guard(void) {
    static const int xform_stubs[] = {
        GL_STUB_glPushMatrix,
        GL_STUB_glPopMatrix,
        GL_STUB_glLoadIdentity,
        GL_STUB_glTranslatef,
        GL_STUB_glScalef,
        GL_STUB_glRotatef,
        GL_STUB_glMultMatrixf,
    };
    const int n_xform_stubs = (int)(sizeof xform_stubs / sizeof xform_stubs[0]);

    for (int t_int = 0; t_int < CMD_TYPE_COUNT; t_int++) {
        CmdType t = (CmdType)t_int;
        int pred = repl_cmd_is_transform(t);

        GLCmd cmd = {0};
        cmd.type = t;
        cmd.num_args = 4;  /* All transforms read at most 4 args */

        /* Depth pre-loaded so POP_MATRIX takes its non-no-op branch. */
        int depth_a = 1;
        unsigned long long before_a[sizeof xform_stubs / sizeof xform_stubs[0]];
        unsigned long long after_a[sizeof xform_stubs / sizeof xform_stubs[0]];
        for (int j = 0; j < n_xform_stubs; j++)
            before_a[j] = gl_stub_counts[xform_stubs[j]];

        apply_tracked_transform(&cmd, &depth_a);

        for (int j = 0; j < n_xform_stubs; j++)
            after_a[j] = gl_stub_counts[xform_stubs[j]];

        int ticked_a = 0;
        for (int j = 0; j < n_xform_stubs; j++)
            if (after_a[j] > before_a[j]) ticked_a++;

        char label[160];
        snprintf(label, sizeof label,
                 "type=%d: apply_tracked_transform tick-count matches "
                 "repl_cmd_is_transform (pred=%d, ticked=%d)",
                 t_int, pred, ticked_a);
        ASSERT_INT(label, ticked_a > 0, pred);

        int depth_b = 1;
        unsigned long long before_b[sizeof xform_stubs / sizeof xform_stubs[0]];
        unsigned long long after_b[sizeof xform_stubs / sizeof xform_stubs[0]];
        for (int j = 0; j < n_xform_stubs; j++)
            before_b[j] = gl_stub_counts[xform_stubs[j]];

        repl_executor_apply_tracked_transform_cmd(&cmd, &depth_b);

        for (int j = 0; j < n_xform_stubs; j++)
            after_b[j] = gl_stub_counts[xform_stubs[j]];

        int ticked_b = 0;
        for (int j = 0; j < n_xform_stubs; j++)
            if (after_b[j] > before_b[j]) ticked_b++;

        snprintf(label, sizeof label,
                 "type=%d: repl_executor_apply_tracked_transform_cmd "
                 "tick-count matches repl_cmd_is_transform (pred=%d, ticked=%d)",
                 t_int, pred, ticked_b);
        ASSERT_INT(label, ticked_b > 0, pred);
    }
}

/* --- 8. geometry-emit predicate coverage --------------------------
 *
 * repl_cmd_starts_geometry_emit names the commands that anchor a draw
 * using the current modelview — glBegin opens a vertex stream, the
 * glut solid primitives close a shape, and CMD_TESS_BEGIN_POLYGON opens
 * a tessellated draw. transform-guide planning uses this predicate as
 * its "stop walking forward" signal in the after-cursor anchor search.
 *
 * Pin the exact set so a new CmdType that should anchor a draw can't
 * silently skip the predicate. Every transform that lives inside
 * the predicate set must also have a CmdSyntaxCategory consistent
 * with its visual role; we don't test that here because the visual
 * taxonomy intentionally diverges (CMD_BEGIN is a primitive-state
 * setter, not a CMD_CAT_VERTEX entry).
 */
static void test_starts_geometry_emit_predicate(void) {
    static const CmdType expected[] = {
        CMD_BEGIN,
        CMD_GLUT_TORUS,
        CMD_GLUT_CUBE,
        CMD_GLUT_SPHERE,
        CMD_GLUT_TEAPOT,
        CMD_GLUT_CONE,
        CMD_TESS_BEGIN_POLYGON,
    };
    const int n_expected = (int)(sizeof expected / sizeof expected[0]);

    for (int t_int = 0; t_int < CMD_TYPE_COUNT; t_int++) {
        CmdType t = (CmdType)t_int;
        int is_expected = 0;
        for (int j = 0; j < n_expected; j++)
            if (expected[j] == t) { is_expected = 1; break; }

        char label[128];
        snprintf(label, sizeof label,
                 "type=%d: repl_cmd_starts_geometry_emit matches "
                 "expected set", t_int);
        ASSERT_INT(label, repl_cmd_starts_geometry_emit(t), is_expected);
    }
}

/* --- 9. glut-solid predicate coverage ----------------------------
 *
 * repl_cmd_is_glut_solid names the five glutSolid* primitive commands.
 * It is the single source feeding repl_cmd_starts_geometry_emit and
 * repl_cmd_consumes_current_color (both OR it in) and the outline
 * overlay's GL_LINE wireframe redraw pass. Pin the exact set so a new
 * glutSolid* CmdType can't be added without joining the predicate (and
 * thus the outline redraw + color-link), and so a non-shape type can't
 * drift into it. */
static void test_is_glut_solid_predicate(void) {
    static const CmdType expected[] = {
        CMD_GLUT_TORUS,
        CMD_GLUT_CUBE,
        CMD_GLUT_SPHERE,
        CMD_GLUT_TEAPOT,
        CMD_GLUT_CONE,
    };
    const int n_expected = (int)(sizeof expected / sizeof expected[0]);

    for (int t_int = 0; t_int < CMD_TYPE_COUNT; t_int++) {
        CmdType t = (CmdType)t_int;
        int is_expected = 0;
        for (int j = 0; j < n_expected; j++)
            if (expected[j] == t) { is_expected = 1; break; }

        char label[128];
        snprintf(label, sizeof label,
                 "type=%d: repl_cmd_is_glut_solid matches expected set",
                 t_int);
        ASSERT_INT(label, repl_cmd_is_glut_solid(t), is_expected);

        /* Every glut solid must also be a geometry-emit anchor and a
         * color consumer — the two predicates that OR it in. */
        if (is_expected) {
            ASSERT_INT("glut solid anchors a draw",
                       repl_cmd_starts_geometry_emit(t), 1);
            ASSERT_INT("glut solid consumes current color",
                       repl_cmd_consumes_current_color(t), 1);
        }
    }
}

/* --- 10. GLU tessellator vs immediate-mode normal separation ---------
 *
 * Verify that glNormal3f (standard immediate mode) does not overwrite
 * the active gluNormal (GLU tessellator) normal state for subsequent
 * gluVertex calls.
 */
typedef struct {
    float normal_at_vertex3f[3];
    float normal_at_gluvertex[3];
    int   vertex3f_count;
    int   gluvertex_count;
} NormalRecorder;

static void on_vertex_capture_normals(const ReplayVertexWalkState *state,
                                     float vx, float vy, float vz, void *user) {
    NormalRecorder *rec = (NormalRecorder *)user;
    const GLCmd *cmd = &repl_state_flat_program_view().cmds[state->flat_cmd_idx];
    if (cmd->type == CMD_VERTEX3F) {
        rec->normal_at_vertex3f[0] = state->normal[0];
        rec->normal_at_vertex3f[1] = state->normal[1];
        rec->normal_at_vertex3f[2] = state->normal[2];
        rec->vertex3f_count++;
    } else if (cmd->type == CMD_TESS_VERTEX) {
        rec->normal_at_gluvertex[0] = state->normal[0];
        rec->normal_at_gluvertex[1] = state->normal[1];
        rec->normal_at_gluvertex[2] = state->normal[2];
        rec->gluvertex_count++;
    }
}

static void test_walker_separates_glu_and_immediate_normals(void) {
    glr_ctrl_reset_all();
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("gluNormal(0, 0, 1);");
    editor_feed_line("glBegin(GL_QUAD_STRIP);");
    editor_feed_line("glNormal3f(-1, 0, 0);");
    editor_feed_line("glVertex3f(-1.2, -0.45, 0);");
    editor_feed_line("gluVertex(-1.2, -0.45, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("gluEnd();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    NormalRecorder rec = {0};
    ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_vertex_capture_normals
    };
    ReplayVertexWalkContext ctx = make_ctx(-1);
    replay_walk_user_vertices(&ctx, &cb, &rec);

    ASSERT_INT("visited glVertex3f", rec.vertex3f_count, 1);
    ASSERT_INT("visited gluVertex", rec.gluvertex_count, 1);

    /* glVertex3f should have normal (-1, 0, 0) */
    ASSERT_NEAR("glVertex3f normal.x", rec.normal_at_vertex3f[0], -1.0f);
    ASSERT_NEAR("glVertex3f normal.y", rec.normal_at_vertex3f[1], 0.0f);
    ASSERT_NEAR("glVertex3f normal.z", rec.normal_at_vertex3f[2], 0.0f);

    /* gluVertex should have normal (0, 0, 1) */
    ASSERT_NEAR("gluVertex normal.x", rec.normal_at_gluvertex[0], 0.0f);
    ASSERT_NEAR("gluVertex normal.y", rec.normal_at_gluvertex[1], 0.0f);
    ASSERT_NEAR("gluVertex normal.z", rec.normal_at_gluvertex[2], 1.0f);
}

typedef struct {
    int lighting_at_vertex[8];
    int vertex_count;
} LightingRecorder;

static void on_vertex_capture_lighting(const ReplayVertexWalkState *state,
                                       float vx, float vy, float vz,
                                       void *user) {
    LightingRecorder *rec = (LightingRecorder *)user;
    (void)vx;
    (void)vy;
    (void)vz;
    if (rec->vertex_count < 8)
        rec->lighting_at_vertex[rec->vertex_count] = state->lighting_enabled;
    rec->vertex_count++;
}

static void test_walker_tracks_lighting_state_for_vertices(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glEnable(GL_LIGHTING);");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glDisable(GL_LIGHTING);");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(2, 0, 0);");
    editor_feed_line("glEnd();");

    repl_state_mark_flat_dirty();
    repl_flatten_commands(editor_state_edit_line());

    LightingRecorder rec = {0};
    ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_vertex_capture_lighting
    };
    ReplayVertexWalkContext ctx = make_ctx(-1);
    replay_walk_user_vertices(&ctx, &cb, &rec);

    ASSERT_INT("visited all lighting-state vertices", rec.vertex_count, 3);
    ASSERT_INT("lighting defaults off at first vertex",
               rec.lighting_at_vertex[0], 0);
    ASSERT_INT("lighting is on after glEnable(GL_LIGHTING)",
               rec.lighting_at_vertex[1], 1);
    ASSERT_INT("lighting is off after glDisable(GL_LIGHTING)",
               rec.lighting_at_vertex[2], 0);
}

int main(void) {
    test_walker_resolves_funcn_args_at_cursor();
    test_walker_fires_on_each_cmd_at_cursor();
    test_walker_stop_flag_halts();
    test_cursor_guide_snapshot_override();
    test_parse_arg_slots_nested_parens();
    test_predicate_category_agreement();
    test_display_name_for_not_in_begin();
    test_transform_dispatch_drift_guard();
    test_starts_geometry_emit_predicate();
    test_is_glut_solid_predicate();
    test_walker_separates_glu_and_immediate_normals();
    test_walker_tracks_lighting_state_for_vertices();

    printf("test_replay_walk: %d/%d passed\n",
           g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
