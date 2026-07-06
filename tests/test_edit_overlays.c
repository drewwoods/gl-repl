/*
 * tests/test_edit_overlays.c - Unit tests for edit overlays subsystem.
 */
#include "editor/state.h"
#include "editor/input.h"
#include "repl/state_views.h"
#include "repl/state_owners.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "app/glr_ctrl.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

/* Include the .c file directly to test its static functions */
#include "subsystems/edit_overlays/edit_overlays.c"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

/* ---------------------------------------------------------------------------
 * GL-stub call trace harness.
 *
 * The stub GL/GLU/GLUT headers (under tests/gl-stubs/include/GL/) record every
 * call's scalar arguments to a trace file when gl_stub_trace_open() is
 * active — e.g. glVertex3f(1, 2, 3) writes the line "glVertex3f 1 2 3"
 * (floats via %g, C locale). The render functions below emit raw
 * command args straight into glVertex3f/glVertex2f (the no-op stubs apply
 * no modelview transform), so we can assert the *exact* coordinates the
 * overlay passes feed to GL. We pair that with the per-symbol counters in
 * gl_stub_counts[] for "was this path taken at all" checks.
 * ------------------------------------------------------------------------- */
#define TRACE_PATH "build/test_edit_overlays_trace.txt"

typedef struct {
    char lines[2048][128];
    int  n;
} TraceLog;

static void trace_begin(void) {
    gl_stub_counts_reset();
    gl_stub_trace_open(TRACE_PATH);
}

static void trace_end(TraceLog *log) {
    gl_stub_trace_close();
    log->n = 0;
    FILE *f = fopen(TRACE_PATH, "r");
    if (!f) return;
    char buf[256];
    while (log->n < 2048 && fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len && buf[len - 1] == '\n') buf[--len] = '\0';
        strncpy(log->lines[log->n], buf, sizeof(log->lines[0]) - 1);
        log->lines[log->n][sizeof(log->lines[0]) - 1] = '\0';
        log->n++;
    }
    fclose(f);
}

/* Count trace lines that match `exact` verbatim (e.g. "glVertex3f 1 2 3"). */
static int trace_count_line(const TraceLog *log, const char *exact) {
    int c = 0;
    for (int i = 0; i < log->n; i++)
        if (strcmp(log->lines[i], exact) == 0) c++;
    return c;
}

/* Count trace lines whose symbol is `sym` (args ignored). */
static int trace_count_sym(const TraceLog *log, const char *sym) {
    size_t sl = strlen(sym);
    int c = 0;
    for (int i = 0; i < log->n; i++) {
        if (strncmp(log->lines[i], sym, sl) == 0 &&
            (log->lines[i][sl] == '\0' || log->lines[i][sl] == ' '))
            c++;
    }
    return c;
}

static void trace_bitmap_text(const TraceLog *log, char *out, size_t out_sz) {
    size_t used = 0;
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    for (int i = 0; i < log->n && used + 1 < out_sz; i++) {
        int ch;
        if (sscanf(log->lines[i], "glutBitmapCharacter %d", &ch) == 1) {
            out[used++] = (char)ch;
            out[used] = '\0';
        }
    }
}

/* Initialize one valid GLCmd of `type` with up to three float args. */
static void mk_cmd(GLCmd *c, CmdType type, float a0, float a1, float a2) {
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->valid = 1;
    c->args[0] = a0;
    c->args[1] = a1;
    c->args[2] = a2;
    c->num_args = 3;
    c->src_cmd_idx = -1;
    c->call_src_cmd_idx = -1;
    c->root_call_src_cmd_idx = -1;
}

static void test_outline_begin_mode_has_overlay(void) {
    printf("--- edit_overlays outline_begin_mode_has_overlay ---\n");
    ASSERT_INT("GL_POINTS has no overlay", outline_begin_mode_has_overlay(GL_POINTS), 0);
    ASSERT_INT("GL_LINES has no overlay", outline_begin_mode_has_overlay(GL_LINES), 0);
    ASSERT_INT("GL_LINE_STRIP has no overlay", outline_begin_mode_has_overlay(GL_LINE_STRIP), 0);
    ASSERT_INT("GL_LINE_LOOP has no overlay", outline_begin_mode_has_overlay(GL_LINE_LOOP), 0);
    ASSERT_INT("GL_TRIANGLES has overlay", outline_begin_mode_has_overlay(GL_TRIANGLES), 1);
    ASSERT_INT("GL_QUADS has overlay", outline_begin_mode_has_overlay(GL_QUADS), 1);
    ASSERT_INT("GL_POLYGON has overlay", outline_begin_mode_has_overlay(GL_POLYGON), 1);
}

static void test_outline_cmd_matches_cursor(void) {
    printf("--- edit_overlays outline_cmd_matches_cursor ---\n");
    GLCmd cmds[5];
    memset(cmds, 0, sizeof(cmds));
    for (int i = 0; i < 5; i++) {
        cmds[i].valid = 1;
    }

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 5;
    ctx.cursor.edit_line_idx = 10;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;

    // Out of bounds check
    ASSERT_INT("OOB index negative", outline_cmd_matches_cursor(-1, &ctx), 0);
    ASSERT_INT("OOB index too large", outline_cmd_matches_cursor(5, &ctx), 0);

    // Invalid command
    cmds[0].valid = 0;
    cmds[0].src_cmd_idx = 10;
    ASSERT_INT("invalid command", outline_cmd_matches_cursor(0, &ctx), 0);
    cmds[0].valid = 1;

    // Valid matching src_cmd_idx
    cmds[0].src_cmd_idx = 10;
    ASSERT_INT("matches src_cmd_idx", outline_cmd_matches_cursor(0, &ctx), 1);

    // Valid mismatching src_cmd_idx
    cmds[0].src_cmd_idx = 9;
    ASSERT_INT("mismatching src_cmd_idx", outline_cmd_matches_cursor(0, &ctx), 0);

    // Match via call_src_cmd_idx
    cmds[0].call_src_cmd_idx = 10;
    ASSERT_INT("matches call_src_cmd_idx", outline_cmd_matches_cursor(0, &ctx), 1);
    cmds[0].call_src_cmd_idx = 0;

    // Match via root_call_src_cmd_idx
    cmds[0].root_call_src_cmd_idx = 10;
    ASSERT_INT("matches root_call_src_cmd_idx", outline_cmd_matches_cursor(0, &ctx), 1);
    cmds[0].root_call_src_cmd_idx = -1;

    // Match via func_scope_mask
    ctx.cursor.cursor_func_scope_mask = 2;
    cmds[0].func_scope_mask = 2;
    ASSERT_INT("matches func_scope_mask", outline_cmd_matches_cursor(0, &ctx), 1);
    cmds[0].func_scope_mask = 4;
    ASSERT_INT("mismatching func_scope_mask", outline_cmd_matches_cursor(0, &ctx), 0);
    ctx.cursor.cursor_func_scope_mask = 0;

    // Match via cursor block bounds
    ctx.cursor.cursor_block_begin = 1;
    ctx.cursor.cursor_block_end = 3;
    ASSERT_INT("matches within cursor block", outline_cmd_matches_cursor(2, &ctx), 1);
    ASSERT_INT("mismatch outside cursor block", outline_cmd_matches_cursor(0, &ctx), 0);
}

static void test_outline_block_matches_cursor(void) {
    printf("--- edit_overlays outline_block_matches_cursor ---\n");
    GLCmd cmds[10];
    memset(cmds, 0, sizeof(cmds));

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 10;
    ctx.cursor.edit_line_idx = 5;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;

    // Setup a non-tessellation block: BEGIN, VERTEX, END
    cmds[0].valid = 1; cmds[0].type = CMD_BEGIN;
    cmds[1].valid = 1; cmds[1].type = CMD_VERTEX3F; cmds[1].src_cmd_idx = 4;
    cmds[2].valid = 1; cmds[2].type = CMD_END;

    // cursor.edit_line_idx is 5, block contains only src_cmd_idx 4, so no match
    ASSERT_INT("no match in block", outline_block_matches_cursor(0, 0, &ctx), 0);

    // Now make it match
    cmds[1].src_cmd_idx = 5;
    ASSERT_INT("match in block", outline_block_matches_cursor(0, 0, &ctx), 1);

    // Setup a tessellation block: TESS_BEGIN_POLYGON, TESS_BEGIN_CONTOUR, TESS_VERTEX, TESS_END
    memset(cmds, 0, sizeof(cmds));
    cmds[0].valid = 1; cmds[0].type = CMD_TESS_BEGIN_POLYGON;
    cmds[1].valid = 1; cmds[1].type = CMD_TESS_BEGIN_CONTOUR;
    cmds[2].valid = 1; cmds[2].type = CMD_TESS_VERTEX; cmds[2].src_cmd_idx = 4;
    cmds[3].valid = 1; cmds[3].type = CMD_TESS_END;

    ASSERT_INT("tess no match", outline_block_matches_cursor(0, 1, &ctx), 0);

    cmds[2].src_cmd_idx = 5;
    ASSERT_INT("tess match", outline_block_matches_cursor(0, 1, &ctx), 1);
}

static void test_build_vertex_walk_context(void) {
    printf("--- edit_overlays edit_overlays_build_vertex_walk_context ---\n");
    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program = repl_state_flat_program_view();
    ctx.cursor.edit_line_idx = 42;
    ctx.cursor.cursor_block_begin = 10;
    ctx.cursor.cursor_block_end = 20;

    ReplayVertexWalkContext walk = edit_overlays_build_vertex_walk_context(&ctx, 1);
    ASSERT_INT("selected block only matches input", walk.selected_block_only, 1);
    ASSERT_INT("edit_line_idx matches input ctx", walk.cursor.edit_line_idx, 42);
    ASSERT_INT("block begin matches input ctx", walk.cursor.cursor_block_begin, 10);
    ASSERT_INT("block end matches input ctx", walk.cursor.cursor_block_end, 20);
    ASSERT_INT("source block valid defaults off", walk.cursor.cursor_source_block_valid, 0);
    ASSERT_INT("func_scope_mask starts at 0", walk.cursor.cursor_func_scope_mask, 0u);
}

/* edit_overlays_render_vertex_points: walks the flat program and emits one
 * glBegin(GL_POINTS)/glVertex3f/glEnd per vertex, with glPointSize keyed on
 * whether the enclosing primitive is a line mode. Validate the exact
 * coordinates and the point sizes via the call trace. */
static void test_render_vertex_points(void) {
    printf("--- edit_overlays edit_overlays_render_vertex_points ---\n");

    GLCmd cmds[4];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 1.0f, 2.0f, 3.0f);
    mk_cmd(&cmds[2], CMD_VERTEX2F, 0.5f, -0.5f, 0);  /* z forced to 0 */
    mk_cmd(&cmds[3], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 4;
    ctx.show_vertex_points = 1;

    TraceLog log;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);

    ASSERT_INT("emits glVertex3f for the 3f vertex",
               trace_count_line(&log, "glVertex3f 1 2 3"), 1);
    ASSERT_INT("emits glVertex3f for the 2f vertex (z=0)",
               trace_count_line(&log, "glVertex3f 0.5 -0.5 0"), 1);
    ASSERT_INT("two point batches drawn",
               trace_count_sym(&log, "glVertex3f"), 2);
    ASSERT_INT("triangle vertices use the large point size",
               trace_count_line(&log, "glPointSize 4"), 2);
    ASSERT_INT("counter agrees with trace",
               (int)gl_stub_counts[GL_STUB_glVertex3f], 2);

    /* Neither show_vertex_points nor replay_vertex_points -> early return. */
    ctx.show_vertex_points = 0;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("disabled -> no vertices emitted",
               trace_count_sym(&log, "glVertex3f"), 0);
    ASSERT_INT("disabled -> no glPushAttrib (returned before any GL)",
               trace_count_sym(&log, "glPushAttrib"), 0);

    /* Line primitive -> the smaller point size. replay_vertex_points path. */
    GLCmd line_cmds[3];
    mk_cmd(&line_cmds[0], CMD_BEGIN, (float)GL_LINE_STRIP, 0, 0);
    mk_cmd(&line_cmds[1], CMD_VERTEX3F, 4.0f, 5.0f, 6.0f);
    mk_cmd(&line_cmds[2], CMD_END, 0, 0, 0);
    ctx.program.cmds = line_cmds;
    ctx.program.cmd_count = 3;
    ctx.replay_vertex_points = 1;
    ctx.show_vertex_points = 1;  /* user toggle on -> all vertices drawn */
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("line-mode vertex uses the small point size",
               trace_count_line(&log, "glPointSize 2"), 1);
    ASSERT_INT("line-mode vertex coords emitted",
               trace_count_line(&log, "glVertex3f 4 5 6"), 1);

    /* A leading transform is tracked during the walk (the matrix is the
     * point's modelview, even though the no-op stub doesn't transform the
     * recorded coordinate). */
    GLCmd xf_cmds[3];
    mk_cmd(&xf_cmds[0], CMD_TRANSLATE3F, 9.0f, 0.0f, 0.0f);
    mk_cmd(&xf_cmds[1], CMD_VERTEX3F, 0.0f, 0.0f, 0.0f);
    mk_cmd(&xf_cmds[2], CMD_END, 0, 0, 0);
    ctx.program.cmds = xf_cmds;
    ctx.program.cmd_count = 3;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("vertex-points walk tracks the transform",
               trace_count_line(&log, "glTranslatef 9 0 0"), 1);

    /* Replay-only (show_vertex_points=0, replay_vertex_points=1): only the
     * anchor vertex should be drawn, not every vertex in the program. */
    GLCmd multi_cmds[6];
    mk_cmd(&multi_cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&multi_cmds[1], CMD_VERTEX3F, 1.0f, 0.0f, 0.0f);
    mk_cmd(&multi_cmds[2], CMD_VERTEX3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&multi_cmds[3], CMD_VERTEX3F, 0.0f, 0.0f, 1.0f);
    mk_cmd(&multi_cmds[4], CMD_END, 0, 0, 0);
    mk_cmd(&multi_cmds[5], CMD_END, 0, 0, 0); /* padding */
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = multi_cmds;
    ctx.program.cmd_count = 5;
    ctx.replay_vertex_points = 1;
    ctx.show_vertex_points = 0;  /* user toggle off -> replay-only mode */
    ctx.replay_anchor_flat_idx = 2;  /* the second vertex (0,1,0) */
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("replay-only draws exactly one vertex",
               trace_count_sym(&log, "glVertex3f"), 1);
    ASSERT_INT("replay-only draws the anchor vertex",
               trace_count_line(&log, "glVertex3f 0 1 0"), 1);
    ASSERT_INT("replay-only skips non-anchor vertex (1,0,0)",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("replay-only skips non-anchor vertex (0,0,1)",
               trace_count_line(&log, "glVertex3f 0 0 1"), 0);

    /* replay_anchor_flat_idx = -1 -> no anchor -> no dots drawn. */
    ctx.replay_anchor_flat_idx = -1;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("replay-only with no anchor emits no vertices",
               trace_count_sym(&log, "glVertex3f"), 0);
}

/* glutSolid* shapes generate their mesh vertices inside GLUT, so the manual
 * glVertex* walk above cannot mark them. Vertex-points redraws those shapes
 * under polygon GL_POINT mode when the user toggle is on. */
static void test_render_vertex_points_glut(void) {
    printf("--- edit_overlays edit_overlays_render_vertex_points glut pass ---\n");

    GLCmd cmds[2];
    mk_cmd(&cmds[0], CMD_TRANSLATE3F, 2.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[1], CMD_GLUT_SPHERE, 0.5f, 8.0f, 6.0f);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 2;
    ctx.show_vertex_points = 1;

    char polygon_point[64];
    char polygon_fill[64];
    snprintf(polygon_point, sizeof(polygon_point), "glPolygonMode %u %u",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_POINT);
    snprintf(polygon_fill, sizeof(polygon_fill), "glPolygonMode %u %u",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_FILL);

    TraceLog log;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);

    ASSERT_INT("glut solid re-drawn for vertex points",
               (int)gl_stub_counts[GL_STUB_glutSolidSphere], 1);
    ASSERT_INT("glut vertex points switch polygon mode to GL_POINT",
               trace_count_line(&log, polygon_point), 1);
    ASSERT_INT("glut vertex points restore polygon mode to GL_FILL",
               trace_count_line(&log, polygon_fill), 1);
    ASSERT_INT("glut vertex points use the large point size",
               trace_count_line(&log, "glPointSize 4") >= 1, 1);
    ASSERT_INT("glut vertex points track the modelview transform",
               trace_count_line(&log, "glTranslatef 2 0 0") >= 1, 1);

    /* Replay-only mode marks a single authored anchor. A glutSolid* anchor
     * has no authored vertex to emit, so do not redraw the whole mesh unless
     * the user explicitly enables vertex points. */
    ctx.show_vertex_points = 0;
    ctx.replay_vertex_points = 1;
    ctx.replay_anchor_flat_idx = 1;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("replay-only does not redraw a whole glut mesh",
               (int)gl_stub_counts[GL_STUB_glutSolidSphere], 0);
    ASSERT_INT("replay-only does not switch to GL_POINT polygon mode",
               trace_count_line(&log, polygon_point), 0);
}

/* render_outlines_glbegin_pass (via the public entry): traces an outline
 * around overlay-eligible primitives but leaves line modes alone. */
static void test_render_outlines_glbegin(void) {
    printf("--- edit_overlays render_outlines (glBegin pass) ---\n");

    GLCmd cmds[4];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0.1f, 0.2f, 0.3f);
    mk_cmd(&cmds[2], CMD_VERTEX2F, 0.4f, 0.5f, 0);
    mk_cmd(&cmds[3], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 4;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_INT("outline traces the 3f vertex",
               trace_count_line(&log, "glVertex3f 0.1 0.2 0.3"), 1);
    ASSERT_INT("outline traces the 2f vertex",
               trace_count_line(&log, "glVertex2f 0.4 0.5"), 1);
    ASSERT_INT("edge outline width applied",
               trace_count_line(&log, "glLineWidth 1.2"), 1);
    ASSERT_INT("polygon mode toggled to lines and back",
               trace_count_sym(&log, "glPolygonMode") >= 2, 1);

    /* GL_LINES has no overlay: with only show_vertex_outlines, the begin
     * block is skipped entirely (no vertices traced). */
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_LINES, 0, 0);
    trace_begin();
    edit_overlays_render_outlines(&ctx, 1, 1);
    trace_end(&log);
    ASSERT_INT("line primitive draws no outline vertices",
               trace_count_sym(&log, "glVertex3f"), 0);

    /* highlight_current_poly: the block matching the cursor gets the
     * thick active-outline width even for a line primitive. */
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_LINES, 0, 0);
    cmds[1].src_cmd_idx = 7;
    ctx.show_vertex_outlines = 0;
    ctx.highlight_current_poly = 1;
    ctx.cursor.edit_line_idx = 7;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("current-poly highlight uses the thick width",
               trace_count_line(&log, "glLineWidth 3"), 1);
    ASSERT_INT("current-poly highlight still traces its vertex",
               trace_count_line(&log, "glVertex3f 0.1 0.2 0.3"), 1);

    /* A begin block left open at end of program triggers the trailing
     * glEnd cleanup after the loop. */
    GLCmd open_cmds[2];
    mk_cmd(&open_cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&open_cmds[1], CMD_VERTEX3F, 0.6f, 0.7f, 0.8f);
    OverlayWalkCtx octx;
    memset(&octx, 0, sizeof(octx));
    octx.program.cmds = open_cmds;
    octx.program.cmd_count = 2;
    octx.cursor.edit_line_idx = -1;
    octx.cursor.cursor_block_begin = -1;
    octx.cursor.cursor_block_end = -1;
    octx.show_vertex_outlines = 1;
    trace_begin();
    edit_overlays_render_outlines(&octx, 0, 0);
    trace_end(&log);
    ASSERT_INT("unterminated begin still traces its vertex",
               trace_count_line(&log, "glVertex3f 0.6 0.7 0.8"), 1);
    ASSERT_TRUE("unterminated begin is closed by trailing glEnd",
                trace_count_sym(&log, "glEnd") >= 1);
}

static void test_current_poly_highlight_respects_label_scope(void) {
    printf("--- edit_overlays current-poly highlight label scope ---\n");

    GLCmd cmds[6];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 1.0f, 0.0f, 0.0f);
    cmds[1].src_cmd_idx = 7;
    mk_cmd(&cmds[2], CMD_END, 0, 0, 0);
    mk_cmd(&cmds[3], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[4], CMD_VERTEX3F, 2.0f, 0.0f, 0.0f);
    cmds[4].src_cmd_idx = 7;
    mk_cmd(&cmds[5], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 6;
    ctx.cursor.edit_line_idx = 7;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.show_vertex_outlines = 0;

    TraceLog log;
    ctx.cursor_label_scope = OVERLAY_VERTEX_LABEL_SCOPE_ONE_INSTANCE;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("one-instance highlights only the first matching block",
               trace_count_line(&log, "glLineWidth 3"), 1);
    ASSERT_INT("one-instance draws first highlighted block",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
    ASSERT_INT("one-instance skips later matching blocks",
               trace_count_line(&log, "glVertex3f 2 0 0"), 0);

    ctx.cursor_label_scope = OVERLAY_VERTEX_LABEL_SCOPE_ALL_INSTANCES;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("all-instances highlights both matching blocks",
               trace_count_line(&log, "glLineWidth 3"), 2);
    ASSERT_INT("all-instances draws later matching block",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);
}

/* render_outlines_tess_pass: draws each tess contour as a GL_LINE_LOOP. */
static void test_render_outlines_tess(void) {
    printf("--- edit_overlays render_outlines (tess pass) ---\n");

    GLCmd cmds[5];
    mk_cmd(&cmds[0], CMD_TESS_BEGIN_POLYGON, 0, 0, 0);
    mk_cmd(&cmds[1], CMD_TESS_BEGIN_CONTOUR, 0, 0, 0);
    mk_cmd(&cmds[2], CMD_TESS_VERTEX, 1.5f, 2.5f, 3.5f);
    mk_cmd(&cmds[3], CMD_TESS_VERTEX, -1.0f, -2.0f, 0.0f);
    mk_cmd(&cmds[4], CMD_TESS_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 5;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_INT("tess contour vertex 0 traced",
               trace_count_line(&log, "glVertex3f 1.5 2.5 3.5"), 1);
    ASSERT_INT("tess contour vertex 1 traced",
               trace_count_line(&log, "glVertex3f -1 -2 0"), 1);
    ASSERT_INT("contour outline width applied",
               trace_count_line(&log, "glLineWidth 1.5"), 1);

    /* replay_tess_preview suppresses the tess outline entirely. */
    ctx.replay_tess_preview = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("tess preview suppresses contour vertices",
               trace_count_sym(&log, "glVertex3f"), 0);

    /* A two-contour polygon with the polygon matching the cursor: the
     * second BEGIN_CONTOUR closes the first loop, and the active-poly
     * highlight color is used. */
    GLCmd poly[7];
    mk_cmd(&poly[0], CMD_TESS_BEGIN_POLYGON, 0, 0, 0);
    poly[0].src_cmd_idx = 3;
    mk_cmd(&poly[1], CMD_TESS_BEGIN_CONTOUR, 0, 0, 0);
    mk_cmd(&poly[2], CMD_TESS_VERTEX, 0.0f, 0.0f, 0.0f);
    mk_cmd(&poly[3], CMD_TESS_BEGIN_CONTOUR, 0, 0, 0);
    mk_cmd(&poly[4], CMD_TESS_VERTEX, 1.0f, 1.0f, 1.0f);
    mk_cmd(&poly[5], CMD_TESS_END, 0, 0, 0);
    poly[6] = poly[5]; /* unused padding */

    OverlayWalkCtx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.program.cmds = poly;
    tctx.program.cmd_count = 6;
    tctx.cursor.edit_line_idx = 3;     /* matches the polygon's src line */
    tctx.cursor.cursor_block_begin = -1;
    tctx.cursor.cursor_block_end = -1;
    tctx.highlight_current_poly = 1;
    trace_begin();
    edit_overlays_render_outlines(&tctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("first contour vertex traced", trace_count_sym(&log, "glVertex3f") >= 2, 1);
    ASSERT_TRUE("multi-contour opens two line loops",
                trace_count_line(&log, "glLineWidth 1.5") >= 2);
}

/* render_outlines_glut_pass: re-draws glutSolid* shapes in wireframe under
 * the active polygon-line mode. The shape call is the observable effect. */
static void test_render_outlines_glut(void) {
    printf("--- edit_overlays render_outlines (glut pass) ---\n");

    GLCmd cmds[2];
    mk_cmd(&cmds[0], CMD_TRANSLATE3F, 2.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[1], CMD_GLUT_CUBE, 1.0f, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 2;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_INT("glut solid re-drawn for wireframe outline",
               (int)gl_stub_counts[GL_STUB_glutSolidCube], 1);
    /* render_outlines runs all three passes (glbegin/tess/glut), each of
     * which tracks the transform, so the translate is seen once per pass. */
    ASSERT_INT("modelview transform tracked before the shape",
               trace_count_line(&log, "glTranslatef 2 0 0") >= 1, 1);

    /* highlight_current_poly: a glut solid matching the cursor gets the
     * thick active outline width. */
    cmds[1].src_cmd_idx = 5;
    ctx.show_vertex_outlines = 0;
    ctx.highlight_current_poly = 1;
    ctx.cursor.edit_line_idx = 5;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("active glut solid still drawn",
               (int)gl_stub_counts[GL_STUB_glutSolidCube], 1);
    ASSERT_TRUE("active glut solid uses the thick width",
                trace_count_line(&log, "glLineWidth 3") >= 1);
}

/* find_next_vertex_args_in_flat: scans forward for the next vertex, stopping
 * at block boundaries. Pure function — assert the recovered coords directly. */
static void test_find_next_vertex_args(void) {
    printf("--- edit_overlays find_next_vertex_args_in_flat ---\n");

    GLCmd cmds[4];
    mk_cmd(&cmds[0], CMD_NORMAL3F, 0, 0, 1);
    mk_cmd(&cmds[1], CMD_COLOR3F, 1, 1, 1);
    mk_cmd(&cmds[2], CMD_VERTEX2F, 3.0f, 4.0f, 99.0f); /* 2f -> z forced 0 */
    mk_cmd(&cmds[3], CMD_END, 0, 0, 0);

    FlatProgramView flat = { .cmds = cmds, .local_vars = NULL, .cmd_count = 4 };
    float out[3] = { -1, -1, -1 };
    ASSERT_INT("finds the next vertex after a normal",
               find_next_vertex_args_in_flat(&flat, 0, out), 1);
    ASSERT_TRUE("recovered x", out[0] == 3.0f);
    ASSERT_TRUE("recovered y", out[1] == 4.0f);
    ASSERT_TRUE("2f vertex z forced to zero", out[2] == 0.0f);

    /* A CMD_END before any vertex stops the scan. */
    mk_cmd(&cmds[1], CMD_END, 0, 0, 0);
    out[0] = -1;
    ASSERT_INT("block boundary stops the scan",
               find_next_vertex_args_in_flat(&flat, 0, out), 0);

    /* NULL guards. */
    ASSERT_INT("NULL flat returns 0",
               find_next_vertex_args_in_flat(NULL, 0, out), 0);
    ASSERT_INT("NULL out returns 0",
               find_next_vertex_args_in_flat(&flat, 0, NULL), 0);
}

/* cursor_guide_snapshot_with_flat_args: overrides vertex/normal args from
 * the flat command, and for a normal command anchors normal_base_pos at the
 * next vertex. */
static void test_cursor_guide_snapshot_with_flat_args(void) {
    printf("--- edit_overlays cursor_guide_snapshot_with_flat_args ---\n");

    Render3dGuideSnapshot base;
    memset(&base, 0, sizeof(base));

    /* Vertex command -> vertex_args populated. */
    GLCmd v;
    mk_cmd(&v, CMD_VERTEX3F, 7.0f, 8.0f, 9.0f);
    Render3dGuideSnapshot s = cursor_guide_snapshot_with_flat_args(&base, &v, 0);
    ASSERT_TRUE("vertex arg x copied", s.vertex_args[0] == 7.0f);
    ASSERT_TRUE("vertex arg y copied", s.vertex_args[1] == 8.0f);
    ASSERT_TRUE("vertex arg z copied", s.vertex_args[2] == 9.0f);

    /* 2f vertex -> z forced to 0. */
    mk_cmd(&v, CMD_VERTEX2F, 1.0f, 2.0f, 3.0f);
    s = cursor_guide_snapshot_with_flat_args(&base, &v, 0);
    ASSERT_TRUE("2f vertex z forced to 0", s.vertex_args[2] == 0.0f);

    /* Normal command followed by a vertex -> normal_args + base pos. */
    GLCmd prog[2];
    mk_cmd(&prog[0], CMD_NORMAL3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&prog[1], CMD_VERTEX3F, 5.0f, 6.0f, 7.0f);
    base.flat_program.cmds = prog;
    base.flat_program.cmd_count = 2;
    s = cursor_guide_snapshot_with_flat_args(&base, &prog[0], 0);
    ASSERT_TRUE("normal arg y copied", s.normal_args[1] == 1.0f);
    ASSERT_INT("normal base pos located", s.normal_base_pos_valid, 1);
    ASSERT_TRUE("normal base anchored at next vertex",
                s.normal_base_pos[0] == 5.0f && s.normal_base_pos[2] == 7.0f);
    ASSERT_INT("world mode leaves frame normal unset",
               s.normal_frame_args_valid, 0);

    /* In Frame xform-guide mode, the normal label gets the resultant
     * direction after in-scope model transforms. A -90 degree Z rotation maps
     * authored (0, 1, 0) to frame (1, 0, 0). */
    GLCmd rotated[3];
    mk_cmd(&rotated[0], CMD_ROTATEF, -90.0f, 0.0f, 0.0f);
    rotated[0].args[3] = 1.0f;
    rotated[0].num_args = 4;
    mk_cmd(&rotated[1], CMD_NORMAL3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&rotated[2], CMD_VERTEX3F, 5.0f, 6.0f, 7.0f);
    base.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
    base.flat_program.cmds = rotated;
    base.flat_program.cmd_count = 3;
    s = cursor_guide_snapshot_with_flat_args(&base, &rotated[1], 1);
    ASSERT_INT("frame mode computes transformed normal",
               s.normal_frame_args_valid, 1);
    ASSERT_TRUE("frame normal x rotated",
                fabsf(s.normal_frame_args[0] - 1.0f) < 0.001f);
    ASSERT_TRUE("frame normal y rotated",
                fabsf(s.normal_frame_args[1]) < 0.001f);
    ASSERT_TRUE("frame normal z rotated",
                fabsf(s.normal_frame_args[2]) < 0.001f);

    /* NULL flat returns the snapshot unchanged. */
    s = cursor_guide_snapshot_with_flat_args(&base, NULL, 0);
    ASSERT_INT("NULL flat leaves base pos unset", s.normal_base_pos_valid, 0);
}

/* mat4_transform_point and mat4_invert mathematical verification. */
static void test_mat4_math(void) {
    printf("--- edit_overlays mat4_math ---\n");

    /* Identity matrix */
    float id[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    float out[16];

    ASSERT_INT("invert identity succeeds", mat4_invert(id, out), 1);
    ASSERT_TRUE("invert identity is identity", out[0] == 1.0f && out[5] == 1.0f && out[10] == 1.0f && out[15] == 1.0f);
    ASSERT_TRUE("invert identity off-diagonal 0", out[1] == 0.0f && out[12] == 0.0f);

    /* Translation matrix */
    float tr[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        5, 6, 7, 1
    };
    ASSERT_INT("invert translation succeeds", mat4_invert(tr, out), 1);
    ASSERT_TRUE("invert translation diagonal 1", out[0] == 1.0f && out[5] == 1.0f && out[10] == 1.0f && out[15] == 1.0f);
    ASSERT_TRUE("invert translation tx is -5", out[12] == -5.0f);
    ASSERT_TRUE("invert translation ty is -6", out[13] == -6.0f);
    ASSERT_TRUE("invert translation tz is -7", out[14] == -7.0f);

    /* Transform point */
    float pt[3];
    mat4_transform_point(tr, 1.0f, 2.0f, 3.0f, pt);
    ASSERT_TRUE("transform point tx", pt[0] == 6.0f);
    ASSERT_TRUE("transform point ty", pt[1] == 8.0f);
    ASSERT_TRUE("transform point tz", pt[2] == 10.0f);

    /* Singular matrix */
    float sing[16] = { 0 };
    ASSERT_INT("invert singular fails", mat4_invert(sing, out), 0);
}

static void test_sanitize_zero(void) {
    printf("--- edit_overlays sanitize_zero ---\n");
    ASSERT_TRUE("sanitize_zero(0.0f) is 0.0f", sanitize_zero(0.0f) == 0.0f);
    ASSERT_TRUE("sanitize_zero(-0.0f) is 0.0f", sanitize_zero(-0.0f) == 0.0f);
    ASSERT_TRUE("sanitize_zero(-0.0049f) is 0.0f", sanitize_zero(-0.0049f) == 0.0f);
    ASSERT_TRUE("sanitize_zero(0.0049f) is 0.0f", sanitize_zero(0.0049f) == 0.0f);
    ASSERT_TRUE("sanitize_zero(-0.0051f) is -0.0051f", sanitize_zero(-0.0051f) == -0.0051f);
    ASSERT_TRUE("sanitize_zero(0.0051f) is 0.0051f", sanitize_zero(0.0051f) == 0.0051f);
}

/* on_vertex_number_label callback: builds the index / index+pos label text,
 * projects the vertex to screen space, and *collects* the label into the ctx
 * (the actual draw happens later in vertex_labels_layout_and_draw). The stub
 * projection is identity over a {0,0,1024,768} viewport, so screen coords are
 * ((x*0.5+0.5)*1024, (y*0.5+0.5)*768). Also exercises the mode guard and the
 * off-screen cull. */
static void test_on_vertex_number_label_callback(void) {
    printf("--- edit_overlays on_vertex_number_label ---\n");

    ReplayVertexWalkState state;
    memset(&state, 0, sizeof(state));
    state.vertex_idx_in_block = 2;

    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, id, sizeof(id));

    /* INDEX mode: the vertex at the origin projects to the viewport center. */
    VertexLabelCtx idx_ctx;
    memset(&idx_ctx, 0, sizeof(idx_ctx));
    idx_ctx.mode = OVERLAY_VERTEX_LABEL_INDEX;
    memcpy(idx_ctx.proj, id, sizeof(id));
    idx_ctx.vw = 1024;
    idx_ctx.vh = 768;
    idx_ctx.block_instances = 1;   /* inside the first selected block */
    on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, &idx_ctx);
    ASSERT_INT("index label collected", idx_ctx.count, 1);
    ASSERT_TRUE("index label anchored at projected vertex",
                idx_ctx.labels[0].anchor_x == 512.0f &&
                idx_ctx.labels[0].anchor_y == 384.0f);
    ASSERT_TRUE("index label text is the vertex index",
                strcmp(idx_ctx.labels[0].idx, " v2") == 0);

    /* INDEX_POS mode (2D ortho): detail carries the ortho XY. */
    VertexLabelCtx pos_ctx;
    memset(&pos_ctx, 0, sizeof(pos_ctx));
    pos_ctx.mode = OVERLAY_VERTEX_LABEL_INDEX_POS;
    pos_ctx.is_ortho = 1;
    memcpy(pos_ctx.proj, id, sizeof(id));
    pos_ctx.vw = 1024;
    pos_ctx.vh = 768;
    pos_ctx.block_instances = 1;
    on_vertex_number_label(&state, 0.25f, 0.5f, 0.0f, &pos_ctx);
    ASSERT_INT("index+pos label collected", pos_ctx.count, 1);
    ASSERT_TRUE("index+pos detail carries ortho coords",
                strcmp(pos_ctx.labels[0].detail, " (0.25, 0.50)") == 0);

    /* INDEX_WORLD mode: maps through modelview then view_inv (both identity). */
    VertexLabelCtx world_ctx;
    memset(&world_ctx, 0, sizeof(world_ctx));
    world_ctx.mode = OVERLAY_VERTEX_LABEL_INDEX_WORLD;
    world_ctx.view_inv_ok = 1;
    memcpy(world_ctx.view_inv, id, sizeof(id));
    memcpy(world_ctx.proj, id, sizeof(id));
    world_ctx.vw = 1024;
    world_ctx.vh = 768;
    world_ctx.block_instances = 1;
    on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, &world_ctx);
    ASSERT_INT("index+world label collected", world_ctx.count, 1);
    ASSERT_TRUE("index+world detail carries world coords",
                strcmp(world_ctx.labels[0].detail, " (0.00, 0.00, 0.00)") == 0);

    /* INDEX_WORLD_FINE mode: maps through modelview then view_inv with 6 decimal places. */
    VertexLabelCtx fine_ctx;
    memset(&fine_ctx, 0, sizeof(fine_ctx));
    fine_ctx.mode = OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE;
    fine_ctx.view_inv_ok = 1;
    memcpy(fine_ctx.view_inv, id, sizeof(id));
    memcpy(fine_ctx.proj, id, sizeof(id));
    fine_ctx.vw = 1024;
    fine_ctx.vh = 768;
    fine_ctx.block_instances = 1;
    on_vertex_number_label(&state, 0.123456f, 0.789012f, 0.345678f, &fine_ctx);
    ASSERT_INT("index+world fine label collected", fine_ctx.count, 1);
    ASSERT_TRUE("index+world fine detail carries high-precision world coords",
                strcmp(fine_ctx.labels[0].detail, " (0.123456, 0.789012, 0.345678)") == 0);

    /* OFF mode and NULL ctx collect nothing. */
    VertexLabelCtx off_ctx;
    memset(&off_ctx, 0, sizeof(off_ctx));
    off_ctx.mode = OVERLAY_VERTEX_LABEL_OFF;
    memcpy(off_ctx.proj, id, sizeof(id));
    off_ctx.vw = 1024;
    off_ctx.vh = 768;
    on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, &off_ctx);
    on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, NULL);
    ASSERT_INT("off / NULL ctx collects nothing", off_ctx.count, 0);

    /* A vertex that projects outside the viewport is culled (the old direct
     * glRasterPos3f would have been invalid there too). */
    VertexLabelCtx cull_ctx;
    memset(&cull_ctx, 0, sizeof(cull_ctx));
    cull_ctx.mode = OVERLAY_VERTEX_LABEL_INDEX;
    memcpy(cull_ctx.proj, id, sizeof(id));
    cull_ctx.vw = 1024;
    cull_ctx.vh = 768;
    cull_ctx.block_instances = 1;
    on_vertex_number_label(&state, 5.0f, 5.0f, 0.0f, &cull_ctx);
    ASSERT_INT("off-screen vertex culled", cull_ctx.count, 0);
}

/* Label scope: one-instance mode labels only the first unrolled copy of a
 * looped primitive (the parametric-torus duplicate-label fix), while
 * all-instances mode labels every vertex with a globally-unique number. The
 * walk resets vertex_idx_in_block to 0 at each block, which is how the callback
 * detects block (loop-iteration) boundaries. */
static void test_vertex_label_scope(void) {
    printf("--- edit_overlays vertex label scope ---\n");

    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, id, sizeof(id));

    ReplayVertexWalkState state;
    memset(&state, 0, sizeof(state));

    /* Two unrolled blocks of two vertices each (v0,v1 then v0,v1 again), all
     * projecting on-screen. */
    const float verts[4][3] = {
        { 0.0f, 0.0f, 0.0f }, { 0.25f, 0.0f, 0.0f },   /* block 0: idx 0,1 */
        { -0.25f, 0.0f, 0.0f }, { 0.5f, 0.0f, 0.0f },  /* block 1: idx 0,1 */
    };
    const int vidx[4] = { 0, 1, 0, 1 };

    /* One-instance: only block 0's two vertices, numbered by in-block index. */
    VertexLabelCtx one;
    memset(&one, 0, sizeof(one));
    one.mode = OVERLAY_VERTEX_LABEL_INDEX;
    one.label_options = OVERLAY_VERTEX_LABEL_SCOPE_ONE_INSTANCE;
    memcpy(one.proj, id, sizeof(id));
    one.vw = 1024;
    one.vh = 768;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = vidx[i];
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &one);
    }
    ASSERT_INT("one-instance collects only the first block", one.count, 2);
    ASSERT_TRUE("one-instance numbers by in-block index",
                strcmp(one.labels[0].idx, " v0") == 0 &&
                strcmp(one.labels[1].idx, " v1") == 0);

    /* All-instances: every vertex, numbered globally and uniquely. */
    VertexLabelCtx all;
    memset(&all, 0, sizeof(all));
    all.mode = OVERLAY_VERTEX_LABEL_INDEX;
    all.label_options = OVERLAY_VERTEX_LABEL_SCOPE_ALL_INSTANCES;
    memcpy(all.proj, id, sizeof(id));
    all.vw = 1024;
    all.vh = 768;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = vidx[i];
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &all);
    }
    ASSERT_INT("all-instances collects every vertex", all.count, 4);
    ASSERT_TRUE("all-instances numbers globally (no duplicates)",
                strcmp(all.labels[0].idx, " v0") == 0 &&
                strcmp(all.labels[1].idx, " v1") == 0 &&
                strcmp(all.labels[2].idx, " v2") == 0 &&
                strcmp(all.labels[3].idx, " v3") == 0);

    /* At-vertex: every vertex, numbered by in-block index, bypass layout. */
    VertexLabelCtx at_vert;
    memset(&at_vert, 0, sizeof(at_vert));
    at_vert.mode = OVERLAY_VERTEX_LABEL_INDEX;
    at_vert.label_options = OVERLAY_VERTEX_LABEL_SCOPE_AT_VERTEX;
    memcpy(at_vert.proj, id, sizeof(id));
    at_vert.vw = 1024;
    at_vert.vh = 768;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = vidx[i];
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &at_vert);
    }
    ASSERT_INT("at-vertex collects every vertex", at_vert.count, 4);
    ASSERT_TRUE("at-vertex numbers by in-block index",
                strcmp(at_vert.labels[0].idx, " v0") == 0 &&
                strcmp(at_vert.labels[1].idx, " v1") == 0 &&
                strcmp(at_vert.labels[2].idx, " v0") == 0 &&
                strcmp(at_vert.labels[3].idx, " v1") == 0);

    vertex_labels_layout_and_draw(&at_vert);
    ASSERT_INT("label 0 drawn in at-vertex", at_vert.labels[0].drawn, 1);
    ASSERT_INT("label 1 drawn in at-vertex", at_vert.labels[1].drawn, 1);
    ASSERT_INT("label 2 drawn in at-vertex", at_vert.labels[2].drawn, 1);
    ASSERT_INT("label 3 drawn in at-vertex", at_vert.labels[3].drawn, 1);
    ASSERT_TRUE("label 0 draw_y equals anchor_y", at_vert.labels[0].draw_y == at_vert.labels[0].anchor_y);
    ASSERT_TRUE("label 1 draw_y equals anchor_y", at_vert.labels[1].draw_y == at_vert.labels[1].anchor_y);
    ASSERT_TRUE("label 2 draw_y equals anchor_y", at_vert.labels[2].draw_y == at_vert.labels[2].anchor_y);
    ASSERT_TRUE("label 3 draw_y equals anchor_y", at_vert.labels[3].draw_y == at_vert.labels[3].anchor_y);
}

/* Visible-only scope: a vertex is dropped when the scene depth buffer holds
 * nearer geometry at its pixel. Uses a tiny 4x4 depth grid so the projected
 * pixel lookup is easy to reason about (identity proj over a 4x4 viewport maps
 * object (x,y) -> ((x*0.5+0.5)*4, (y*0.5+0.5)*4)). */
static void test_vertex_label_visible_occlusion(void) {
    printf("--- edit_overlays vertex label visible-only occlusion ---\n");

    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, id, sizeof(id));

    ReplayVertexWalkState state;
    memset(&state, 0, sizeof(state));

    /* Far everywhere (1.0) except pixel (1,1), which has near geometry in
     * front (0.0). */
    float depth[16];
    for (int i = 0; i < 16; i++) depth[i] = 1.0f;
    depth[1 * 4 + 1] = 0.0f;

    VertexLabelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = OVERLAY_VERTEX_LABEL_INDEX;
    ctx.label_options = OVERLAY_VERTEX_LABEL_SCOPE_VISIBLE;
    memcpy(ctx.proj, id, sizeof(id));
    ctx.vw = 4;
    ctx.vh = 4;
    ctx.depthbuf = depth;

    /* (0,0,0) -> pixel (2,2), window depth 0.5 vs scene 1.0 -> visible. */
    on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, &ctx);
    /* (-0.5,-0.5,0) -> pixel (1,1), window depth 0.5 vs scene 0.0 -> occluded. */
    on_vertex_number_label(&state, -0.5f, -0.5f, 0.0f, &ctx);

    ASSERT_INT("visible-only keeps the unoccluded vertex and drops the occluded one",
               ctx.count, 1);
}

/* on_normal_vector_arrow callback: draws a GL_LINES arrow from the vertex to
 * vertex + normal*scale (see render3d_draw_normal_vector_arrow). */
static void test_on_normal_vector_arrow_callback(void) {
    printf("--- edit_overlays on_normal_vector_arrow ---\n");

    ReplayVertexWalkState state;
    memset(&state, 0, sizeof(state));
    state.normal[0] = 0.0f;
    state.normal[1] = 0.0f;
    state.normal[2] = 1.0f;

    NormalVectorRenderCtx ctx = {
        .scale = 2.0f,
        .show_all = 1,
        .replay_display = REPLAY_NORMAL_DISPLAY_OFF,
        .anchor_idx = -1,
        .stop_flag = NULL,
    };
    TraceLog log;
    trace_begin();
    on_normal_vector_arrow(&state, 1.0f, 1.0f, 1.0f, &ctx);
    trace_end(&log);

    ASSERT_INT("arrow starts at the vertex",
               trace_count_line(&log, "glVertex3f 1 1 1"), 1);
    /* The tip (vertex + normal*scale = (1,1,1)+(0,0,1)*2) is emitted twice:
     * once as the line endpoint, once as the arrowhead point. */
    ASSERT_INT("arrow tip at vertex + normal*scale",
               trace_count_line(&log, "glVertex3f 1 1 3"), 2);
}

static void test_render_normal_vectors_replay_only(void) {
    printf("--- edit_overlays replay-only normal vectors ---\n");

    GLCmd cmds[11];
    mk_cmd(&cmds[0], CMD_ENABLE, (float)GL_LIGHTING, 0, 0);
    mk_cmd(&cmds[1], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[2], CMD_NORMAL3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&cmds[3], CMD_VERTEX3F, 1.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[4], CMD_NORMAL3F, 0.0f, 0.0f, 1.0f);
    mk_cmd(&cmds[5], CMD_VERTEX3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&cmds[6], CMD_END, 0, 0, 0);
    mk_cmd(&cmds[7], CMD_DISABLE, (float)GL_LIGHTING, 0, 0);
    mk_cmd(&cmds[8], CMD_BEGIN, (float)GL_POINTS, 0, 0);
    mk_cmd(&cmds[9], CMD_VERTEX3F, 0.0f, 0.0f, 1.0f);
    mk_cmd(&cmds[10], CMD_END, 0, 0, 0);

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 11;
    walk.replay_normal_display = REPLAY_NORMAL_DISPLAY_VECTOR;
    walk.replay_anchor_flat_idx = 3;

    TraceLog log;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);

    ASSERT_TRUE("replay normal draws the focused glyph at the anchor",
                trace_count_line(&log, "glVertex3f 1 0 0") >= 1);
    ASSERT_TRUE("replay normal uses the anchor's current normal",
                trace_count_line(&log, "glVertex3f 1 0.35 0") >= 1);
    ASSERT_INT("replay normal skips lit non-anchor vertices",
               trace_count_line(&log, "glVertex3f 0 1 0"), 0);

    walk.replay_normal_display = REPLAY_NORMAL_DISPLAY_DIRECTION;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("replay normal direction label is anchored at the arrow tip",
               trace_count_line(&log, "glRasterPos3f 1 0.35 0"), 1);
    char label_text[128];
    trace_bitmap_text(&log, label_text, sizeof(label_text));
    ASSERT_TRUE("replay normal direction label uses %.2f precision",
                strstr(label_text, " n=(0.00, 1.00, 0.00)") != NULL);

    GLCmd rotated[6];
    mk_cmd(&rotated[0], CMD_ROTATEF, -90.0f, 0.0f, 0.0f);
    rotated[0].args[3] = 1.0f;
    rotated[0].num_args = 4;
    mk_cmd(&rotated[1], CMD_ENABLE, (float)GL_LIGHTING, 0, 0);
    mk_cmd(&rotated[2], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&rotated[3], CMD_NORMAL3F, 0.0f, 1.0f, 0.0f);
    mk_cmd(&rotated[4], CMD_VERTEX3F, 1.0f, 0.0f, 0.0f);
    mk_cmd(&rotated[5], CMD_END, 0, 0, 0);
    OverlayWalkCtx rotated_walk;
    memset(&rotated_walk, 0, sizeof(rotated_walk));
    rotated_walk.program.cmds = rotated;
    rotated_walk.program.cmd_count = 6;
    rotated_walk.replay_normal_display = REPLAY_NORMAL_DISPLAY_DIRECTION;
    rotated_walk.replay_anchor_flat_idx = 4;
    rotated_walk.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
    trace_begin();
    edit_overlays_render_normal_vectors(&rotated_walk);
    trace_end(&log);
    trace_bitmap_text(&log, label_text, sizeof(label_text));
    ASSERT_TRUE("replay normal frame label shows transformed direction",
                strstr(label_text, "-> frame=(1.00, 0.00, 0.00)") != NULL);

    walk.replay_anchor_flat_idx = 9;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("replay normal skips an unlit anchor",
               trace_count_sym(&log, "glVertex3f"), 0);

    walk.replay_anchor_flat_idx = -1;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("replay normal with no anchor returns before GL setup",
               trace_count_sym(&log, "glPushAttrib"), 0);
}

static void test_render_replay_vertex_label(void) {
    printf("--- edit_overlays replay vertex label ---\n");

    GLCmd cmds[5];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[2], CMD_VERTEX3F, 0.25f, 0.0f, 0.0f);
    mk_cmd(&cmds[3], CMD_VERTEX3F, 0.50f, 0.0f, 0.0f);
    mk_cmd(&cmds[4], CMD_END, 0, 0, 0);

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 5;
    walk.replay_vertex_label = 1;
    walk.replay_anchor_flat_idx = 2;

    TraceLog log;
    trace_begin();
    edit_overlays_render_replay_vertex_label(&walk);
    trace_end(&log);

    ASSERT_INT("replay vertex label anchors at the focused vertex",
               trace_count_line(&log, "glRasterPos3f 0.25 0 0"), 1);
    char label_text[128];
    trace_bitmap_text(&log, label_text, sizeof(label_text));
    ASSERT_TRUE("replay vertex label uses the global all-instances index",
                strstr(label_text, " v1") != NULL);

    walk.replay_anchor_flat_idx = -1;
    trace_begin();
    edit_overlays_render_replay_vertex_label(&walk);
    trace_end(&log);
    ASSERT_INT("replay vertex label with no anchor returns before GL setup",
               trace_count_sym(&log, "glPushAttrib"), 0);
}

static void test_vertex_numbers_use_source_begin_block(void) {
    printf("--- edit_overlays vertex labels source block selection ---\n");

    /* Identity modelview so the stub's identity projection over a {0,0,1024,768}
     * viewport gives deterministic screen coords: x=(0.25,-0.5,0) -> sx=640,256,512. */
    float idm[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, idm, sizeof(idm));

    GLCmd cmds[7];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_POINTS, 0, 0);
    cmds[0].src_cmd_idx = 0;
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0.25f, 0.0f, 0.0f);
    cmds[1].src_cmd_idx = 2;
    mk_cmd(&cmds[2], CMD_VERTEX3F, -0.5f, 0.0f, 0.0f);
    cmds[2].src_cmd_idx = 2;
    mk_cmd(&cmds[3], CMD_END, 0, 0, 0);
    cmds[3].src_cmd_idx = 4;
    mk_cmd(&cmds[4], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    cmds[4].src_cmd_idx = 10;
    mk_cmd(&cmds[5], CMD_VERTEX3F, 0.0f, 0.0f, 0.0f);
    cmds[5].src_cmd_idx = 11;
    mk_cmd(&cmds[6], CMD_END, 0, 0, 0);
    cmds[6].src_cmd_idx = 12;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 7;
    walk.cursor.edit_line_idx = 11;
    /* Simulate the flattened range landing inside the earlier GL_POINTS
     * expansion; source block metadata must keep labels on the real cursor
     * block instead. */
    walk.cursor.cursor_block_begin = 1;
    walk.cursor.cursor_block_end = 2;
    walk.cursor.cursor_source_block_valid = 1;
    walk.cursor.cursor_source_block_begin = 10;
    walk.cursor.cursor_source_block_end = 12;

    TraceLog log;
    trace_begin();
    edit_overlays_render_vertex_numbers(&walk, OVERLAY_VERTEX_LABEL_INDEX, 0,
                                        OVERLAY_VERTEX_LABEL_SCOPE_ALL_INSTANCES);
    trace_end(&log);

    ASSERT_INT("earlier GL_POINTS vertex 0 not labelled",
               trace_count_line(&log, "glRasterPos2f 640 384"), 0);
    ASSERT_INT("earlier GL_POINTS vertex 1 not labelled",
               trace_count_line(&log, "glRasterPos2f 256 384"), 0);
    ASSERT_INT("cursor source block vertex labelled",
               trace_count_line(&log, "glRasterPos2f 512 384"), 1);
}

/* End-to-end: feed a real REPL program, flatten it, then drive the
 * replay-walk-based overlays (vertex numbers, normals, cursor guides) and
 * the post_overlays orchestrator. Validates the overlays render the live
 * flattened vertex coordinates. */
static void test_render_via_repl_program(void) {
    printf("--- edit_overlays overlays over a real flattened program ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("glNormal3f(0, 0, 1);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0.25, 0.5, 0.75);");
    editor_feed_line("glVertex3f(-0.25, -0.5, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());

    TraceLog log;

    /* edit_overlays_render_vertex_numbers walks with selected_block_only=1,
     * so only vertices in the cursor's "current block" are labelled. Mark
     * the whole flat program as the current block so the triangle is in
     * scope. */
    repl_state_flat_program_set_current_block(
        0, repl_state_flat_program_view().cmd_count - 1, 0);

    /* Vertex numbers: each vertex is projected to screen space and labelled
     * there. Identity modelview + the stub's identity projection over a
     * {0,0,1024,768} viewport maps (x,y) -> ((x*0.5+0.5)*1024, (y*0.5+0.5)*768),
     * so (0.25,0.5,*) -> (640,576) and (-0.25,-0.5,*) -> (384,192). */
    float idm[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, idm, sizeof(idm));

    trace_begin();
    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program = repl_state_flat_program_view();
    walk.cursor.edit_line_idx = editor_state_edit_line();
    walk.cursor.cursor_block_begin = repl_state_flat_program_current_block_begin();
    walk.cursor.cursor_block_end = repl_state_flat_program_current_block_end();

    edit_overlays_render_vertex_numbers(&walk, OVERLAY_VERTEX_LABEL_INDEX_POS, 0,
                                        OVERLAY_VERTEX_LABEL_SCOPE_ALL_INSTANCES);
    trace_end(&log);
    ASSERT_INT("vertex-number label at first vertex",
               trace_count_line(&log, "glRasterPos2f 640 576"), 1);
    ASSERT_INT("vertex-number label at second vertex",
               trace_count_line(&log, "glRasterPos2f 384 192"), 1);

    /* Normal vectors: arrow base at each vertex (scale GLR_NORMAL_ARROW_SCALE). */
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("normal arrow anchored at first vertex",
               trace_count_line(&log, "glVertex3f 0.25 0.5 0.75"), 1);
    ASSERT_TRUE("normal arrows draw line segments",
                trace_count_sym(&log, "glVertex3f") >= 2);

    /* Cursor guides: place the cursor on the first vertex line and render. */
    Render3dGuideSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.show_guides = 1;
    snap.flat_program = repl_state_flat_program_view();
    snap.source_cmds = repl_state_document_cmds();
    snap.source_cmd_count = repl_state_document_count();
    snap.input = "";
    snap.edit_line_idx = editor_state_edit_line();
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_INT("cursor guides save/restore the modelview",
               trace_count_sym(&log, "glPushMatrix"),
               trace_count_sym(&log, "glPopMatrix"));

    /* show_guides == 0 is an early return (no GL at all). */
    snap.show_guides = 0;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_INT("guides off -> no GL emitted",
               trace_count_sym(&log, "glPushAttrib"), 0);

    /* post_overlays orchestrator: runs outlines + points + guides + labels
     * + normals in one pass. Build the snapshot pack and verify it emits the
     * flattened vertices through at least one overlay path. */
    OverlaySnapshotPack pack;
    memset(&pack, 0, sizeof(pack));
    pack.walk.program = repl_state_flat_program_view();
    pack.walk.cursor.edit_line_idx = editor_state_edit_line();
    pack.walk.cursor.cursor_block_begin = repl_state_flat_program_current_block_begin();
    pack.walk.cursor.cursor_block_end = repl_state_flat_program_current_block_end();
    pack.walk.show_vertex_outlines = 1;
    pack.walk.show_vertex_points = 1;
    pack.snapshot.show_guides = 0;
    pack.vertex_label_mode = OVERLAY_VERTEX_LABEL_INDEX;
    pack.show_normal_vectors = 1;
    trace_begin();
    edit_overlays_post_overlays(&pack);
    trace_end(&log);
    ASSERT_TRUE("post_overlays emits the flattened vertices",
                trace_count_line(&log, "glVertex3f 0.25 0.5 0.75") >= 1);
    ASSERT_INT("post_overlays draws vertex-number labels",
               trace_count_sym(&log, "glRasterPos2f") >= 1, 1);

    /* NULL pack is a safe no-op. */
    edit_overlays_post_overlays(NULL);
}

static void test_cursor_guides_render_for_unterminated_begin(void) {
    printf("--- edit_overlays cursor guides with unterminated glBegin ---\n");

    GLCmd cmds[2];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    cmds[0].src_cmd_idx = 0;
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0.25f, 0.5f, 0.75f);
    cmds[1].src_cmd_idx = 1;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 2;
    walk.cursor.edit_line_idx = 1;
    walk.cursor.cursor_block_begin = -1;
    walk.cursor.cursor_block_end = -1;

    const char *input = "glVertex3f(0.25, 0.5, 0.75)";
    Render3dGuideSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.show_guides = 1;
    snap.input = input;
    snap.input_len = (int)strlen(input);
    snap.edit_line_idx = 1;
    snap.flat_program = walk.program;
    snap.vertex_args[0] = 0.25f;
    snap.vertex_args[1] = 0.5f;
    snap.vertex_args[2] = 0.75f;
    snap.vertex_filled[0] = snap.vertex_filled[1] = snap.vertex_filled[2] = 1;
    snap.vertex_n_filled = 3;
    snap.alpha_scale = 1.0f;

    TraceLog log;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);

    ASSERT_INT("unterminated begin still draws cursor vertex guide",
               trace_count_line(&log, "glPointSize 8"), 1);
    ASSERT_INT("cursor guide uses the flat vertex coords",
               trace_count_line(&log, "glVertex3f 0.25 0.5 0.75"), 1);

    input = "glVertex3f(-0.25, -0.5, 0)";
    walk.cursor.edit_line_idx = 2;
    snap.input = input;
    snap.input_len = (int)strlen(input);
    snap.edit_line_idx = 2;
    snap.vertex_args[0] = -0.25f;
    snap.vertex_args[1] = -0.5f;
    snap.vertex_args[2] = 0.0f;

    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);

    ASSERT_INT("unterminated begin draws append-row vertex guide",
               trace_count_line(&log, "glPointSize 8"), 1);
    ASSERT_INT("append-row guide uses live input coords",
               trace_count_line(&log, "glVertex3f -0.25 -0.5 0"), 1);
}

/* The transform analog of the vertex append-row test: typing a brand-new
 * transform line (no committed source / flat expansion yet) should draw its
 * guide live, just like a first-time glVertex does. Two cases:
 *  (a) tail append into a non-empty program, and
 *  (b) the very first line in an empty program. */
static void test_cursor_guides_render_for_new_transform_line(void) {
    printf("--- edit_overlays cursor guides for a new transform line ---\n");

    /* (a) One committed vertex; cursor parked on a fresh append row (src 1)
     * typing glScalef(2,2,2). */
    GLCmd cmds[1];
    mk_cmd(&cmds[0], CMD_VERTEX3F, 0.0f, 0.0f, 0.0f);
    cmds[0].src_cmd_idx = 0;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 1;
    walk.cursor.edit_line_idx = 1;
    walk.cursor.cursor_block_begin = -1;
    walk.cursor.cursor_block_end = -1;

    const char *input = "glScalef(2,2,2)";
    Render3dGuideSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.show_guides = 1;
    snap.input = input;
    snap.input_len = (int)strlen(input);
    snap.edit_line_idx = 1;        /* past the one committed source line */
    snap.source_cmd_count = 1;
    snap.flat_program = walk.program;
    snap.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
    snap.xform_args[0] = snap.xform_args[1] = snap.xform_args[2] = 2.0f;
    snap.xform_filled[0] = snap.xform_filled[1] = snap.xform_filled[2] = 1;
    snap.xform_n_filled = 3;
    snap.alpha_scale = 1.0f;

    TraceLog log;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_TRUE("new tail transform line draws a guide",
                trace_count_sym(&log, "glBegin") > 0);

    /* (b) Empty program: the first line typed is a transform. */
    OverlayWalkCtx empty_walk;
    memset(&empty_walk, 0, sizeof(empty_walk));
    empty_walk.program.cmds = cmds;   /* cmds present but count 0 */
    empty_walk.program.cmd_count = 0;
    empty_walk.cursor.edit_line_idx = 0;

    snap.flat_program = empty_walk.program;
    snap.edit_line_idx = 0;
    snap.source_cmd_count = 0;

    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &empty_walk);
    trace_end(&log);
    ASSERT_TRUE("first transform line in an empty program draws a guide",
                trace_count_sym(&log, "glBegin") > 0);
}

int main(void) {
    printf("--- edit_overlays tests ---\n");
    test_outline_begin_mode_has_overlay();
    test_outline_cmd_matches_cursor();
    test_outline_block_matches_cursor();
    test_build_vertex_walk_context();
    test_render_vertex_points();
    test_render_vertex_points_glut();
    test_render_outlines_glbegin();
    test_current_poly_highlight_respects_label_scope();
    test_render_outlines_tess();
    test_render_outlines_glut();
    test_find_next_vertex_args();
    test_cursor_guide_snapshot_with_flat_args();
    test_mat4_math();
    test_sanitize_zero();
    test_on_vertex_number_label_callback();
    test_vertex_label_scope();
    test_vertex_label_visible_occlusion();
    test_on_normal_vector_arrow_callback();
    test_render_normal_vectors_replay_only();
    test_render_replay_vertex_label();
    test_vertex_numbers_use_source_begin_block();
    test_render_via_repl_program();
    test_cursor_guides_render_for_unterminated_begin();
    test_cursor_guides_render_for_new_transform_line();

    return test_harness_report(&g_harness, "test_edit_overlays");
}
