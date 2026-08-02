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
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Include the .c file directly to test its static functions */
#include "subsystems/edit_overlays/edit_overlays.c"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, got, exp)

/* ---------------------------------------------------------------------------
 * GL-stub call trace harness.
 *
 * The stub GL/GLU/GLUT headers (under tests/gl-stubs/include/GL/) record every
 * call's scalar arguments to a trace file when gl_stub_trace_open() is
 * active - e.g. glVertex3f(1, 2, 3) writes the line "glVertex3f 1 2 3"
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
        snprintf(log->lines[log->n], sizeof(log->lines[0]), "%s", buf);
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

static int trace_count_line_width_near(const TraceLog *log, float expected) {
    int c = 0;
    for (int i = 0; i < log->n; i++) {
        float width;
        char tail;
        if (sscanf(log->lines[i], "glLineWidth %f %c", &width, &tail) == 1 &&
            fabsf(width - expected) < 0.0005f)
            c++;
    }
    return c;
}

/* Replay a GL cap's enable/disable state through the trace and report it as of
 * `before` (the first line matching it verbatim) - i.e. "was this cap live at
 * the moment that vertex was emitted?", which is the only thing that decides
 * whether the pixel survives. Ordering is what matters here, not call counts:
 * a suspend/resume pair around a highlight draw and a plainly clipped outline
 * issue the same calls in a different order. `before == NULL` reports the
 * state at the end of the trace (nothing left enabled). Returns -1 if the cap
 * is never touched, or `before` never appears. */
static int trace_cap_state_at(const TraceLog *log, const char *enable_line,
                              const char *disable_line, const char *before) {
    int state = -1;
    for (int i = 0; i < log->n; i++) {
        if (before && strcmp(log->lines[i], before) == 0)
            return state;
        if (strcmp(log->lines[i], enable_line) == 0) state = 1;
        else if (strcmp(log->lines[i], disable_line) == 0) state = 0;
    }
    return before ? -1 : state;
}

/* GL_CLIP_PLANE0 = 12288. */
static int trace_clip_cap_state_at(const TraceLog *log, const char *before) {
    return trace_cap_state_at(log, "glEnable 12288", "glDisable 12288", before);
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
 * GL_POINTS marker per vertex, sized by whether the enclosing primitive is a
 * line mode. Validate the exact coordinates and marker form via the call
 * trace. */
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
               trace_count_line(&log, "glPointSize 6"), 2);
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
               trace_count_line(&log, "glPointSize 3"), 1);
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
               trace_count_line(&log, "glPointSize 6") >= 1, 1);
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
               trace_count_line_width_near(&log, VERTEX_OUTLINE_EDGE_WIDTH), 1);
    ASSERT_INT("polygon mode toggled to lines and back",
               trace_count_sym(&log, "glPolygonMode") >= 2, 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("bold edge outline applies scale",
               trace_count_line_width_near(
                   &log, VERTEX_OUTLINE_EDGE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_INVERTED;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("inverted edge outline flips black to white",
               trace_count_line(&log, "glColor4f 1 1 1 1"), 1);
    ASSERT_INT("inverted edge outline keeps default width",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_EDGE_WIDTH), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("bold inverted edge outline applies scale",
               trace_count_line_width_near(
                   &log, VERTEX_OUTLINE_EDGE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE), 1);
    ASSERT_INT("bold inverted edge outline flips color",
               trace_count_line(&log, "glColor4f 1 1 1 1"), 1);
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_DEFAULT;

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
    ctx.show_vertex_outlines = 1;
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
    ctx.highlight_current_poly = 1;
    ctx.cursor.edit_line_idx = 7;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("current-poly highlight uses the thick width",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
    ASSERT_INT("current-poly highlight ignores inverted outline color",
               trace_count_line(&log, "glColor4f 1 1 1 1"), 0);
    ASSERT_INT("current-poly highlight keeps active color",
               trace_count_line(&log, "glColor4f 0 0.9 0.9 1"), 1);
    ASSERT_INT("current-poly highlight still traces its vertex",
               trace_count_line(&log, "glVertex3f 0.1 0.2 0.3"), 1);
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_DEFAULT;

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

static void test_current_poly_highlight_respects_overlay_scope(void) {
    printf("--- edit_overlays current-poly highlight overlay scope ---\n");

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
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_LAST_INSTANCE;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("last-instance highlights only the last matching block",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
    ASSERT_INT("last-instance skips earlier matching blocks",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("last-instance draws the last highlighted block",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);

    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("all-instances highlights both matching blocks",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 2);
    ASSERT_INT("all-instances draws later matching block",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);
}

/* Culling is plain GL state, and GL culls polygons in glPolygonMode(GL_LINE)
 * just as it does when filling - so the overlay passes only have to replay the
 * program's cull state and let GL apply the program's own facing rule. Same
 * split as the clip planes: outlines always, the highlight only under
 * POLY_HIGHLIGHT_CLIPPED_CULLED. */
static void test_overlays_honor_culling(void) {
    printf("--- edit_overlays cull replay ---\n");

    GLCmd cmds[8];
    mk_cmd(&cmds[0], CMD_FRONT_FACE, (float)GL_CW, 0, 0);
    cmds[0].num_args = 1;
    mk_cmd(&cmds[1], CMD_CULL_FACE, (float)GL_FRONT, 0, 0);
    cmds[1].num_args = 1;
    mk_cmd(&cmds[2], CMD_ENABLE, (float)GL_CULL_FACE, 0, 0);
    cmds[2].num_args = 1;
    mk_cmd(&cmds[3], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[4], CMD_VERTEX3F, 1, 0, 0); cmds[4].src_cmd_idx = 5;
    mk_cmd(&cmds[5], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&cmds[6], CMD_VERTEX3F, 3, 0, 0);
    mk_cmd(&cmds[7], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 8;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;

    /* GL_CULL_FACE = 2884, GL_FRONT = 1028, GL_CW = 2304, GL_CCW = 2305. */
    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("outlines replay the cull cap once per pass",
               trace_count_line(&log, "glEnable 2884"), 3);
    ASSERT_INT("outlines replay the culled face",
               trace_count_line(&log, "glCullFace 1028"), 3);
    ASSERT_INT("outlines replay the winding rule",
               trace_count_line(&log, "glFrontFace 2304"), 3);
    ASSERT_INT("outlines are drawn with culling live",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884",
                                  "glVertex3f 1 0 0"), 1);
    ASSERT_INT("the pass leaves culling off for later overlays",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884", NULL), 0);

    /* Vertex points track the state but keep drawing: GL never culls
     * GL_POINTS, and the walk does not second-guess it. */
    ctx.show_vertex_points = 1;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("authored vertex points still draw under culling",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
    ctx.show_vertex_points = 0;

    /* The highlight opts out under On and draws unculled... */
    ctx.cursor.edit_line_idx = 5;
    ctx.highlight_current_poly = 1;
    ctx.show_vertex_outlines = 0;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("On mode draws the highlight with culling suspended",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884",
                                  "glVertex3f 1 0 0"), 0);

    /* ...and follows the program under Clipped & culled. */
    ctx.highlight_clipped_culled = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("Clipped & culled draws the highlight with culling live",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884",
                                  "glVertex3f 1 0 0"), 1);
}

/* The stencil test is replayed like the clip planes - an outline around
 * geometry a mask threw away is a lie about what is on screen - but the
 * stencil WRITE state is deliberately not, because a pass that reports on a
 * buffer must not modify it. That split is the whole point of the stencil
 * handling, so it is what these assertions pin. */
static void test_overlays_honor_stencil_test(void) {
    printf("--- edit_overlays stencil test replay ---\n");

    /* GL_STENCIL_TEST = 2960, GL_EQUAL = 514, GL_KEEP = 7680,
     * GL_REPLACE = 7681, GL_ALWAYS = 519. */
    GLCmd cmds[10];
    mk_cmd(&cmds[0], CMD_STENCIL_FUNC, (float)GL_EQUAL, 1, 255);
    cmds[0].num_args = 3;
    /* Mask-building write state: the program's own, and exactly what an
     * overlay must NOT inherit. */
    mk_cmd(&cmds[1], CMD_STENCIL_OP, (float)GL_KEEP, (float)GL_KEEP,
           (float)GL_REPLACE);
    cmds[1].num_args = 3;
    mk_cmd(&cmds[2], CMD_STENCIL_MASK, 255, 0, 0);
    cmds[2].num_args = 1;
    mk_cmd(&cmds[3], CMD_ENABLE, (float)GL_STENCIL_TEST, 0, 0);
    cmds[3].num_args = 1;
    mk_cmd(&cmds[4], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[5], CMD_VERTEX3F, 1, 0, 0); cmds[5].src_cmd_idx = 5;
    mk_cmd(&cmds[6], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&cmds[7], CMD_VERTEX3F, 3, 0, 0);
    mk_cmd(&cmds[8], CMD_END, 0, 0, 0);
    mk_cmd(&cmds[9], CMD_DISABLE, (float)GL_STENCIL_TEST, 0, 0);
    cmds[9].num_args = 1;

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 9;   /* stop before the trailing disable */
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_TRUE("outlines replay the stencil comparison",
                trace_count_line(&log, "glStencilFunc 514 1 255") >= 1);
    ASSERT_INT("outlines are drawn with the stencil test live",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  "glVertex3f 1 0 0"), 1);
    ASSERT_INT("the pass leaves the stencil test off for later overlays",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  NULL), 0);

    /* Writes: each walk opens with them shut and never reopens them, so the
     * program's own glStencilOp / glStencilMask must not appear at all.
     * Three baselines because the outline render runs three walks - same
     * count the cull replay above sees for glEnable. */
    ASSERT_INT("every walk shuts stencil writes at entry",
               trace_count_line(&log, "glStencilMask 0"), 3);
    ASSERT_INT("the walk never reopens stencil writes",
               trace_count_line(&log, "glStencilMask 255"), 0);
    ASSERT_INT("every walk pins the stencil op to KEEP/KEEP/KEEP",
               trace_count_line(&log, "glStencilOp 7680 7680 7680"), 3);
    ASSERT_INT("the program's mask-building op is never replayed",
               trace_count_line(&log, "glStencilOp 7680 7680 7681"), 0);

    /* Vertex points take the same policy through their own walker. */
    ctx.show_vertex_outlines = 0;
    ctx.show_vertex_points = 1;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("vertex points are drawn with the stencil test live",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  "glVertex3f 1 0 0"), 1);
    ASSERT_INT("vertex points never reopen stencil writes",
               trace_count_line(&log, "glStencilMask 255"), 0);
    ctx.show_vertex_points = 0;

    /* The cursor highlight opts out under On: finding the line you are
     * editing matters even when a mask removed its geometry. */
    ctx.cursor.edit_line_idx = 5;
    ctx.highlight_current_poly = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("On mode draws the highlight with the stencil test suspended",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  "glVertex3f 1 0 0"), 0);

    /* ...and follows the program under Clipped & culled. */
    ctx.highlight_clipped_culled = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("Clipped & culled draws the highlight with the stencil live",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  "glVertex3f 1 0 0"), 1);

    /* A GL_ENABLE_BIT scope reverts the stencil-test enable at the pop,
     * exactly like the cull cap. Attrib membership is routed through
     * attrib_bits, so this is the branch that will pick up the stencil
     * attrib group for free when Phase 2 adds it. */
    GLCmd scoped[8];
    mk_cmd(&scoped[0], CMD_ENABLE, (float)GL_STENCIL_TEST, 0, 0);
    scoped[0].num_args = 1;
    mk_cmd(&scoped[1], CMD_PUSH_ATTRIB, (float)GL_ENABLE_BIT, 0, 0);
    scoped[1].num_args = 1;
    mk_cmd(&scoped[2], CMD_DISABLE, (float)GL_STENCIL_TEST, 0, 0);
    scoped[2].num_args = 1;
    mk_cmd(&scoped[3], CMD_POP_ATTRIB, 0, 0, 0);
    scoped[3].num_args = 0;
    mk_cmd(&scoped[4], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&scoped[5], CMD_VERTEX3F, 8, 0, 0);
    mk_cmd(&scoped[6], CMD_VERTEX3F, 9, 0, 0);
    mk_cmd(&scoped[7], CMD_END, 0, 0, 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = scoped;
    ctx.program.cmd_count = 8;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("a GL_ENABLE_BIT pop restores the stencil-test enable",
               trace_cap_state_at(&log, "glEnable 2960", "glDisable 2960",
                                  "glVertex3f 8 0 0"), 1);
}

/* glPushAttrib/glPopAttrib scope the state the overlay walks replay: a scoped
 * glDisable(GL_CULL_FACE) reverts at the pop, so geometry after the pop culls
 * like the real render - in EVERY walker (glBegin outline, glut-solid outline,
 * vertex points), not just the glBegin pass. A scoped glColorMask reverts too,
 * and a GL_TRANSFORM_BIT push captures the live plane equations for restore. */
static void test_overlays_scope_push_pop_attrib(void) {
    printf("--- edit_overlays glPushAttrib/glPopAttrib scoping ---\n");

    /* Cull on; a GL_ENABLE_BIT scope turns it off; triangle after the pop.
     * GL_CULL_FACE = 2884. */
    GLCmd cmds[9];
    mk_cmd(&cmds[0], CMD_ENABLE, (float)GL_CULL_FACE, 0, 0);
    cmds[0].num_args = 1;
    mk_cmd(&cmds[1], CMD_PUSH_ATTRIB, (float)GL_ENABLE_BIT, 0, 0);
    cmds[1].num_args = 1;
    mk_cmd(&cmds[2], CMD_DISABLE, (float)GL_CULL_FACE, 0, 0);
    cmds[2].num_args = 1;
    mk_cmd(&cmds[3], CMD_POP_ATTRIB, 0, 0, 0);
    cmds[3].num_args = 0;
    mk_cmd(&cmds[4], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[5], CMD_VERTEX3F, 1, 0, 0);
    mk_cmd(&cmds[6], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&cmds[7], CMD_VERTEX3F, 3, 0, 0);
    mk_cmd(&cmds[8], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 9;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("outline pass: cull re-enabled by the pop before the draw",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884",
                                  "glVertex3f 1 0 0"), 1);

    /* Same scope followed by a glutSolid shape: the glut outline pass is its
     * own walker and must honor the pop too. */
    GLCmd glut_cmds[5];
    glut_cmds[0] = cmds[0];
    glut_cmds[1] = cmds[1];
    glut_cmds[2] = cmds[2];
    glut_cmds[3] = cmds[3];
    mk_cmd(&glut_cmds[4], CMD_GLUT_SPHERE, 0.5f, 8.0f, 6.0f);
    ctx.program.cmds = glut_cmds;
    ctx.program.cmd_count = 5;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("glut outline pass: cull re-enabled by the pop before the shape",
               trace_cap_state_at(&log, "glEnable 2884", "glDisable 2884",
                                  "glutSolidSphere 0.5 8 6"), 1);

    /* A scoped glColorMask(0,0,0,...) reverts at the pop: the vertex-point
     * walk draws dots after the pop instead of inheriting the dead mask. */
    GLCmd mask_cmds[6];
    mk_cmd(&mask_cmds[0], CMD_PUSH_ATTRIB, (float)GL_COLOR_BUFFER_BIT, 0, 0);
    mask_cmds[0].num_args = 1;
    mk_cmd(&mask_cmds[1], CMD_COLOR_MASK, 0, 0, 0);
    mask_cmds[1].num_args = 4;
    mk_cmd(&mask_cmds[2], CMD_POP_ATTRIB, 0, 0, 0);
    mask_cmds[2].num_args = 0;
    mk_cmd(&mask_cmds[3], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&mask_cmds[4], CMD_VERTEX3F, 7, 0, 0);
    mk_cmd(&mask_cmds[5], CMD_END, 0, 0, 0);
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = mask_cmds;
    ctx.program.cmd_count = 6;
    ctx.show_vertex_points = 1;
    TraceLog mlog;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&mlog);
    ASSERT_INT("vertex points: scoped color mask reverts at the pop",
               trace_count_line(&mlog, "glVertex3f 7 0 0"), 1);

    /* Control: an unscoped dead mask still gates the dots off. */
    GLCmd mask2[4];
    mk_cmd(&mask2[0], CMD_COLOR_MASK, 0, 0, 0);
    mask2[0].num_args = 4;
    mk_cmd(&mask2[1], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&mask2[2], CMD_VERTEX3F, 7, 0, 0);
    mk_cmd(&mask2[3], CMD_END, 0, 0, 0);
    ctx.program.cmds = mask2;
    ctx.program.cmd_count = 4;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&mlog);
    ASSERT_INT("vertex points: unscoped dead color mask still gates dots off",
               trace_count_line(&mlog, "glVertex3f 7 0 0"), 0);

    /* A GL_TRANSFORM_BIT push captures the live eye-space plane equations
     * (glGetClipPlane per plane) and the pop restores them under an identity
     * modelview (the stub reports all-zero equations). GL_CLIP_PLANE0 =
     * 12288. */
    GLCmd clip_cmds[8];
    mk_cmd(&clip_cmds[0], CMD_ENABLE, (float)GL_CLIP_PLANE0, 0, 0);
    clip_cmds[0].num_args = 1;
    mk_cmd(&clip_cmds[1], CMD_PUSH_ATTRIB, (float)GL_TRANSFORM_BIT, 0, 0);
    clip_cmds[1].num_args = 1;
    mk_cmd(&clip_cmds[2], CMD_CLIP_PLANE, (float)GL_CLIP_PLANE0, 0, 1);
    clip_cmds[2].args[3] = 0; clip_cmds[2].args[4] = 0;
    clip_cmds[2].num_args = 5;
    mk_cmd(&clip_cmds[3], CMD_POP_ATTRIB, 0, 0, 0);
    clip_cmds[3].num_args = 0;
    mk_cmd(&clip_cmds[4], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&clip_cmds[5], CMD_VERTEX3F, 1, 0, 0);
    mk_cmd(&clip_cmds[6], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&clip_cmds[7], CMD_END, 0, 0, 0);
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = clip_cmds;
    ctx.program.cmd_count = 8;
    ctx.cursor.edit_line_idx = -1;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_TRUE("transform push captures the live plane equations",
                trace_count_sym(&log, "glGetClipPlane") >= 6);
    ASSERT_TRUE("pop restores captured equations under identity modelview",
                trace_count_sym(&log, "glLoadIdentity") >= 1);
    ASSERT_TRUE("pop re-issues the captured (stub-zero) plane equation",
                trace_count_line(&log, "glClipPlane 12288 0 0 0 0") >= 1);
}

/* GL reverses the winding of odd-numbered GL_TRIANGLE_STRIP triangles. A
 * single-polygon highlight re-issues the cursor's triangle as a fresh strip,
 * which loses that flip, so with culling live it would be culled exactly when
 * the real face is visible. The draw swaps glFrontFace to compensate. */
static void test_single_polygon_highlight_strip_winding(void) {
    printf("--- edit_overlays single-polygon strip winding ---\n");

    GLCmd cmds[8];
    mk_cmd(&cmds[0], CMD_ENABLE, (float)GL_CULL_FACE, 0, 0);
    cmds[0].num_args = 1;
    mk_cmd(&cmds[1], CMD_BEGIN, (float)GL_TRIANGLE_STRIP, 0, 0);
    mk_cmd(&cmds[2], CMD_VERTEX3F, 0, 0, 0);
    mk_cmd(&cmds[3], CMD_VERTEX3F, 1, 0, 0);
    mk_cmd(&cmds[4], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&cmds[5], CMD_VERTEX3F, 3, 0, 0); cmds[5].src_cmd_idx = 9;
    mk_cmd(&cmds[6], CMD_VERTEX3F, 4, 0, 0);
    mk_cmd(&cmds[7], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 8;
    ctx.cursor.edit_line_idx = 9;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.highlight_clipped_culled = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    ctx.cursor_poly_valid = 1;

    /* Odd strip triangle (index 1: ordinals 1..3) - winding compensated. */
    ctx.cursor_poly_first = 1;
    ctx.cursor_poly_last = 3;
    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("an odd strip triangle flips the winding for the highlight",
               trace_count_line(&log, "glFrontFace 2304"), 1);   /* GL_CW */
    ASSERT_INT("...and restores the program's winding after",
               trace_count_line(&log, "glFrontFace 2305"), 1);   /* GL_CCW */

    /* Even strip triangle (index 2: ordinals 2..4) - no compensation. */
    ctx.cursor_poly_first = 2;
    ctx.cursor_poly_last = 4;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("an even strip triangle needs no winding flip",
               trace_count_sym(&log, "glFrontFace"), 0);

    /* On mode suspends culling, so there is no facing to compensate for. */
    ctx.highlight_clipped_culled = 0;
    ctx.cursor_poly_first = 1;
    ctx.cursor_poly_last = 3;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("On mode leaves the winding alone (culling is suspended)",
               trace_count_sym(&log, "glFrontFace"), 0);
}

/* Geometry drawn under a color mask that writes no color (the depth-only
 * seed idiom) is invisible, so the plain outline and vertex-point passes skip
 * it. The cursor highlight is exempt: it locates the line you are editing. */
static void test_overlays_honor_color_mask(void) {
    printf("--- edit_overlays color-mask gate ---\n");

    /* Two blocks: the first masked off, the second visible. The cursor is on
     * the masked block's vertex, so it is also the highlight target. */
    GLCmd cmds[12];
    mk_cmd(&cmds[0], CMD_COLOR_MASK, 0, 0, 0);
    cmds[0].args[3] = 0;
    cmds[0].num_args = 4;
    mk_cmd(&cmds[1], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[2], CMD_VERTEX3F, 1, 0, 0); cmds[2].src_cmd_idx = 9;
    mk_cmd(&cmds[3], CMD_VERTEX3F, 2, 0, 0); cmds[3].src_cmd_idx = 9;
    mk_cmd(&cmds[4], CMD_VERTEX3F, 3, 0, 0); cmds[4].src_cmd_idx = 9;
    mk_cmd(&cmds[5], CMD_END, 0, 0, 0);
    mk_cmd(&cmds[6], CMD_COLOR_MASK, 1, 1, 1);
    cmds[6].args[3] = 1;
    cmds[6].num_args = 4;
    mk_cmd(&cmds[7], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[8], CMD_VERTEX3F, 7, 0, 0);
    mk_cmd(&cmds[9], CMD_VERTEX3F, 8, 0, 0);
    mk_cmd(&cmds[10], CMD_VERTEX3F, 9, 0, 0);
    mk_cmd(&cmds[11], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 12;
    ctx.cursor.edit_line_idx = -1;   /* no highlight target yet */
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.show_vertex_outlines = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("outlines skip a color-masked block",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("outlines still draw the visible block",
               trace_count_line(&log, "glVertex3f 7 0 0"), 1);

    /* Vertex points: same gate. */
    ctx.show_vertex_points = 1;
    trace_begin();
    edit_overlays_render_vertex_points(&ctx);
    trace_end(&log);
    ASSERT_INT("vertex points skip a color-masked block",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("vertex points still draw the visible block",
               trace_count_line(&log, "glVertex3f 7 0 0"), 1);
    ctx.show_vertex_points = 0;

    /* The highlight ignores the mask: park the cursor on the masked block. */
    ctx.cursor.edit_line_idx = 9;
    ctx.highlight_current_poly = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("the highlight draws a color-masked block anyway",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
    ASSERT_INT("...at the active-highlight width",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);

    /* An RGB-masked draw is invisible whatever alpha does, so alpha alone
     * does not resurrect the outline. */
    ctx.cursor.edit_line_idx = -1;
    ctx.highlight_current_poly = 0;
    cmds[0].args[3] = 1;   /* glColorMask(FALSE, FALSE, FALSE, TRUE) */
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("an alpha-only mask still counts as invisible",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);

    /* One live channel is enough to be visible. */
    cmds[0].args[0] = 1;   /* glColorMask(TRUE, FALSE, FALSE, TRUE) */
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("a single live color channel draws the outline",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
}

/* SINGLE_POLYGON scope: only the cursor's primitive within the matched
 * block gets the active highlight; the rest of the block is either bare
 * (show_vertex_outlines off) or a plain edge outline (on). */
static void test_single_polygon_highlight(void) {
    printf("--- edit_overlays single-polygon highlight ---\n");

    GLCmd cmds[10];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_QUADS, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0, 0, 0); cmds[1].src_cmd_idx = 7; /* v0 */
    mk_cmd(&cmds[2], CMD_VERTEX3F, 1, 0, 0);                          /* v1 */
    mk_cmd(&cmds[3], CMD_VERTEX3F, 2, 0, 0);                          /* v2 */
    mk_cmd(&cmds[4], CMD_VERTEX3F, 3, 0, 0);                          /* v3 */
    mk_cmd(&cmds[5], CMD_VERTEX3F, 4, 0, 0);                          /* v4 */
    mk_cmd(&cmds[6], CMD_VERTEX3F, 5, 0, 0);                          /* v5 */
    mk_cmd(&cmds[7], CMD_VERTEX3F, 6, 0, 0);                          /* v6 */
    mk_cmd(&cmds[8], CMD_VERTEX3F, 7, 0, 0);                          /* v7 */
    mk_cmd(&cmds[9], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 10;
    ctx.cursor.edit_line_idx = 7;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.show_vertex_outlines = 0;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    ctx.cursor_poly_valid = 1;
    ctx.cursor_poly_first = 4;  /* second quad: v4..v7 */
    ctx.cursor_poly_last = 7;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_INT("single-polygon highlights exactly one primitive",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
    ASSERT_INT("single-polygon draws v4 (selected quad)",
               trace_count_line(&log, "glVertex3f 4 0 0"), 1);
    ASSERT_INT("single-polygon draws v7 (selected quad)",
               trace_count_line(&log, "glVertex3f 7 0 0"), 1);
    ASSERT_INT("single-polygon skips v0 (other quad, outlines off)",
               trace_count_line(&log, "glVertex3f 0 0 0"), 0);
    ASSERT_INT("single-polygon skips v3 (other quad, outlines off)",
               trace_count_line(&log, "glVertex3f 3 0 0"), 0);

    /* With show_vertex_outlines also on, the rest of the block still draws
     * as a plain edge outline; the selected quad draws twice (once via the
     * active highlight, once via the plain outline pass over the block). */
    ctx.show_vertex_outlines = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("plain outline still draws the other quad's v0",
               trace_count_line(&log, "glVertex3f 0 0 0"), 1);
    ASSERT_INT("selected quad's v4 draws via both passes",
               trace_count_line(&log, "glVertex3f 4 0 0"), 2);
    ASSERT_INT("active highlight still isolated to one primitive",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
}

/* Every overlay pass replays the program's own glClipPlane / clip-cap enables
 * so it traces the geometry the frame actually shows, and drops them again
 * before returning. POLY_HIGHLIGHT_ON is the one opt-out: the cursor's
 * highlight draw is bracketed by a suspend/resume pair so it shows the shape
 * as authored, while the plain outlines around it stay cut.
 * POLY_HIGHLIGHT_CLIPPED_CULLED (highlight_clipped_culled) drops the
 * opt-out. */
static void test_highlight_clip_planes(void) {
    printf("--- edit_overlays clip-aware overlays ---\n");

    GLCmd cmds[7];
    mk_cmd(&cmds[0], CMD_CLIP_PLANE, (float)GL_CLIP_PLANE0, 1, 0);
    cmds[0].args[3] = 0;   /* equation: x >= 0.5 -> (1, 0, 0, -0.5) */
    cmds[0].args[4] = -0.5f;
    cmds[0].num_args = 5;
    mk_cmd(&cmds[1], CMD_ENABLE, (float)GL_CLIP_PLANE0, 0, 0);
    cmds[1].num_args = 1;
    mk_cmd(&cmds[2], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[3], CMD_VERTEX3F, 0, 0, 0); cmds[3].src_cmd_idx = 4;
    mk_cmd(&cmds[4], CMD_VERTEX3F, 1, 0, 0);
    mk_cmd(&cmds[5], CMD_VERTEX3F, 2, 0, 0);
    mk_cmd(&cmds[6], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 7;
    ctx.cursor.edit_line_idx = 4;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;

    /* POLY_HIGHLIGHT_ON: the plane is still replayed, but suspended across
     * the highlight draw, so the cursor's shape outlines as authored. */
    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("the plane equation is replayed once per outline pass",
               trace_count_line(&log, "glClipPlane 12288 1 0 0 -0.5"), 3);
    ASSERT_INT("On mode draws the highlight with the plane suspended",
               trace_clip_cap_state_at(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("On mode still highlights the cursor's block",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
    ASSERT_INT("no clip cap is left enabled when the passes return",
               trace_clip_cap_state_at(&log, NULL), 0);

    /* POLY_HIGHLIGHT_CLIPPED_CULLED: no suspend - the highlight is cut too. */
    ctx.highlight_clipped_culled = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("Clipped mode draws the highlight with the plane live",
               trace_clip_cap_state_at(&log, "glVertex3f 1 0 0"), 1);
    ASSERT_INT("Clipped mode still highlights the cursor's block",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH), 1);
    ASSERT_INT("no clip cap is left enabled when the passes return",
               trace_clip_cap_state_at(&log, NULL), 0);
    ASSERT_INT("clip commands are not mistaken for geometry",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);

    /* Plain outlines are clipped in BOTH modes: no cursor highlight, just
     * the outline pass over the same block. */
    ctx.highlight_current_poly = 0;
    ctx.show_vertex_outlines = 1;
    for (int clipped = 0; clipped <= 1; clipped++) {
        ctx.highlight_clipped_culled = clipped;
        trace_begin();
        edit_overlays_render_outlines(&ctx, 0, 0);
        trace_end(&log);
        ASSERT_INT("plain outlines are drawn with the plane live",
                   trace_clip_cap_state_at(&log, "glVertex3f 1 0 0"), 1);
    }

    /* Vertex points honor the planes too, in both modes. */
    ctx.show_vertex_points = 1;
    for (int clipped = 0; clipped <= 1; clipped++) {
        ctx.highlight_clipped_culled = clipped;
        trace_begin();
        edit_overlays_render_vertex_points(&ctx);
        trace_end(&log);
        ASSERT_INT("vertex points are drawn with the plane live",
                   trace_clip_cap_state_at(&log, "glVertex3f 1 0 0"), 1);
        ASSERT_INT("vertex points replay the plane equation",
                   trace_count_line(&log, "glClipPlane 12288 1 0 0 -0.5"), 1);
        ASSERT_INT("vertex points leave no clip cap enabled",
                   trace_clip_cap_state_at(&log, NULL), 0);
    }

    /* A plane the program disables again is mirrored as a disable, and the
     * pass-end reset has nothing left to drop. */
    ctx.show_vertex_points = 0;
    mk_cmd(&cmds[1], CMD_DISABLE, (float)GL_CLIP_PLANE0, 0, 0);
    cmds[1].num_args = 1;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("a disabled plane is mirrored, not enabled",
               trace_count_line(&log, "glEnable 12288"), 0);
    ASSERT_INT("outlines under a disabled plane draw unclipped",
               trace_clip_cap_state_at(&log, "glVertex3f 1 0 0"), 0);
}

/* SINGLE_POLYGON scope with GL_TRIANGLE_FAN: the shared center vertex
 * (ordinal 0) is highlighted alongside the selected [first, last] pair. */
static void test_single_polygon_highlight_fan_anchor(void) {
    printf("--- edit_overlays single-polygon highlight (fan anchor) ---\n");

    GLCmd cmds[6];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLE_FAN, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 9, 9, 9); cmds[1].src_cmd_idx = 3; /* center */
    mk_cmd(&cmds[2], CMD_VERTEX3F, 1, 0, 0);                          /* v1 */
    mk_cmd(&cmds[3], CMD_VERTEX3F, 2, 0, 0);                          /* v2 */
    mk_cmd(&cmds[4], CMD_VERTEX3F, 3, 0, 0);                          /* v3 */
    mk_cmd(&cmds[5], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 6;
    ctx.cursor.edit_line_idx = 3;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    ctx.cursor_poly_valid = 1;
    ctx.cursor_poly_first = 2;  /* triangle {center, v2, v3} */
    ctx.cursor_poly_last = 3;
    ctx.cursor_poly_fan_anchor = 1;

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    ASSERT_INT("fan anchor draws the shared center vertex",
               trace_count_line(&log, "glVertex3f 9 9 9"), 1);
    ASSERT_INT("fan triangle skips v1 (not in {center,2,3})",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("fan triangle draws v2", trace_count_line(&log, "glVertex3f 2 0 0"), 1);
    ASSERT_INT("fan triangle draws v3", trace_count_line(&log, "glVertex3f 3 0 0"), 1);
}

/* Bold vertex-outline style scales VERTEX_OUTLINE_EDGE_WIDTH by
 * VERTEX_OUTLINE_BOLD_SCALE (1.2 * 2.5 = 3.0), an exact tie with the
 * unscaled VERTEX_OUTLINE_ACTIVE_WIDTH (3.0f). SINGLE_POLYGON's highlight
 * fires, then falls through to the plain-outline pass (show_vertex_outlines
 * redraws the whole block, including the selected primitive, at edge
 * width/color) - so an unscaled highlight width was an exact tie with that
 * later redraw and got fully overwritten. The highlight width must scale
 * the same way so it stays strictly thicker than the bold edge redraw. */
static void test_single_polygon_highlight_bold_style(void) {
    printf("--- edit_overlays single-polygon highlight (bold style) ---\n");

    GLCmd cmds[10];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_QUADS, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0, 0, 0); cmds[1].src_cmd_idx = 7; /* v0 */
    mk_cmd(&cmds[2], CMD_VERTEX3F, 1, 0, 0);                          /* v1 */
    mk_cmd(&cmds[3], CMD_VERTEX3F, 2, 0, 0);                          /* v2 */
    mk_cmd(&cmds[4], CMD_VERTEX3F, 3, 0, 0);                          /* v3 */
    mk_cmd(&cmds[5], CMD_VERTEX3F, 4, 0, 0);                          /* v4 */
    mk_cmd(&cmds[6], CMD_VERTEX3F, 5, 0, 0);                          /* v5 */
    mk_cmd(&cmds[7], CMD_VERTEX3F, 6, 0, 0);                          /* v6 */
    mk_cmd(&cmds[8], CMD_VERTEX3F, 7, 0, 0);                          /* v7 */
    mk_cmd(&cmds[9], CMD_END, 0, 0, 0);

    OverlayWalkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program.cmds = cmds;
    ctx.program.cmd_count = 10;
    ctx.cursor.edit_line_idx = 7;
    ctx.cursor.cursor_block_begin = -1;
    ctx.cursor.cursor_block_end = -1;
    ctx.highlight_current_poly = 1;
    ctx.show_vertex_outlines = 1;  /* triggers the fall-through redraw */
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD;
    ctx.cursor_overlay_scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    ctx.cursor_poly_valid = 1;
    ctx.cursor_poly_first = 4;  /* second quad: v4..v7 */
    ctx.cursor_poly_last = 7;

    /* Sanity: bold edge width lands exactly on the unscaled active width -
     * the tie this test guards against. */
    ASSERT_TRUE("bold edge width ties the unscaled active width",
                fabsf(VERTEX_OUTLINE_EDGE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE -
                      VERTEX_OUTLINE_ACTIVE_WIDTH) < 0.0005f);

    TraceLog log;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);

    float bold_active = VERTEX_OUTLINE_ACTIVE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE;
    float bold_edge = VERTEX_OUTLINE_EDGE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE;
    ASSERT_INT("bold single-polygon highlight scales past the bold edge redraw",
               trace_count_line_width_near(&log, bold_active), 1);
    ASSERT_INT("bold edge redraw still present at its own (lower) width",
               trace_count_line_width_near(&log, bold_edge), 1);
    ASSERT_INT("highlight draws the selected quad's v4",
               trace_count_line(&log, "glVertex3f 4 0 0"), 2);
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
               trace_count_line_width_near(&log, VERTEX_OUTLINE_TESS_WIDTH), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("bold tess contour outline applies scale",
               trace_count_line_width_near(
                   &log, VERTEX_OUTLINE_TESS_WIDTH * VERTEX_OUTLINE_BOLD_SCALE), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_INVERTED;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("inverted tess contour flips outline color",
               trace_count_line(&log, "glColor4f 0.45 0.8 0.3 1"), 1);
    ASSERT_INT("inverted tess contour keeps default width",
               trace_count_line_width_near(&log, VERTEX_OUTLINE_TESS_WIDTH), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_DEFAULT;

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
    tctx.show_vertex_outlines = 1;
    tctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
    trace_begin();
    edit_overlays_render_outlines(&tctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("first contour vertex traced", trace_count_sym(&log, "glVertex3f") >= 2, 1);
    ASSERT_TRUE("multi-contour opens two line loops",
                trace_count_line_width_near(&log, VERTEX_OUTLINE_TESS_WIDTH) >= 2);
    ASSERT_INT("tess current-poly highlight ignores bold outline width",
               trace_count_line_width_near(
                   &log, VERTEX_OUTLINE_TESS_WIDTH * VERTEX_OUTLINE_BOLD_SCALE), 0);
    ASSERT_INT("tess current-poly highlight ignores inverted outline color",
               trace_count_line(&log, "glColor4f 0.45 0.8 0.3 1"), 0);
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

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("bold glut solid outline applies scale",
               trace_count_line_width_near(
                   &log, VERTEX_OUTLINE_EDGE_WIDTH * VERTEX_OUTLINE_BOLD_SCALE), 1);

    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_INVERTED;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("inverted glut solid outline flips black to white",
               trace_count_line(&log, "glColor4f 1 1 1 1"), 1);
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_DEFAULT;

    /* highlight_current_poly: a glut solid matching the cursor gets the
     * thick active outline width. */
    cmds[1].src_cmd_idx = 5;
    ctx.show_vertex_outlines = 1;
    ctx.vertex_outline_style = VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
    ctx.highlight_current_poly = 1;
    ctx.cursor.edit_line_idx = 5;
    trace_begin();
    edit_overlays_render_outlines(&ctx, 0, 0);
    trace_end(&log);
    ASSERT_INT("active glut solid still drawn",
               (int)gl_stub_counts[GL_STUB_glutSolidCube], 1);
    ASSERT_TRUE("active glut solid uses the thick width",
                trace_count_line_width_near(&log, VERTEX_OUTLINE_ACTIVE_WIDTH) >= 1);
    ASSERT_INT("active glut solid ignores inverted outline color",
               trace_count_line(&log, "glColor4f 1 1 1 1"), 0);
    ASSERT_TRUE("active glut solid keeps active color",
                trace_count_line(&log, "glColor4f 0 0.9 0.9 1") >= 1);
}

/* find_next_vertex_args_in_flat: scans forward for the next vertex, stopping
 * at block boundaries. Pure function - assert the recovered coords directly. */
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

    /* Raster position command -> raster anchor populated. */
    mk_cmd(&v, CMD_RASTER_POS3F, 2.0f, 3.0f, 4.0f);
    s = cursor_guide_snapshot_with_flat_args(&base, &v, 0);
    ASSERT_INT("raster pos marked complete", s.raster_pos_n_filled, 3);
    ASSERT_TRUE("raster pos args copied",
                s.raster_pos_args[0] == 2.0f &&
                s.raster_pos_args[1] == 3.0f &&
                s.raster_pos_args[2] == 4.0f);

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

    /* Apply translation */
    float m_tr[16];
    mat4_identity(m_tr);
    mat4_apply_translate(m_tr, 2.0f, -3.0f, 4.0f);
    ASSERT_TRUE("apply_translate tx", m_tr[12] == 2.0f);
    ASSERT_TRUE("apply_translate ty", m_tr[13] == -3.0f);
    ASSERT_TRUE("apply_translate tz", m_tr[14] == 4.0f);
    ASSERT_TRUE("apply_translate diagonal 1", m_tr[0] == 1.0f && m_tr[5] == 1.0f && m_tr[10] == 1.0f && m_tr[15] == 1.0f);

    /* Apply scale */
    float m_sc[16];
    mat4_identity(m_sc);
    mat4_apply_scale(m_sc, 2.0f, 3.0f, 4.0f);
    ASSERT_TRUE("apply_scale sx", m_sc[0] == 2.0f);
    ASSERT_TRUE("apply_scale sy", m_sc[5] == 3.0f);
    ASSERT_TRUE("apply_scale sz", m_sc[10] == 4.0f);
    ASSERT_TRUE("apply_scale components zero", m_sc[1] == 0.0f && m_sc[12] == 0.0f);
    ASSERT_TRUE("apply_scale bottom-right 1", m_sc[15] == 1.0f);

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

static void set_identity(float m[16]) {
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (i % 5) == 0 ? 1.0f : 0.0f;
}

static void test_guide_label_obstacle_record_and_projection(void) {
    printf("--- edit_overlays guide label obstacle projection ---\n");

    extern float g_gl_stub_modelview_matrix[16];
    float identity[16];
    float ox[GUIDE_LABEL_OBSTACLE_MAX];
    float oy[GUIDE_LABEL_OBSTACLE_MAX];
    float ow[GUIDE_LABEL_OBSTACLE_MAX];
    Render3dGuideLabelSpec label;
    VertexLabelCtx ctx;
    int projected;

    set_identity(identity);
    memcpy(g_gl_stub_modelview_matrix, identity, sizeof(identity));
    g_gl_stub_modelview_matrix[12] = 0.5f;

    memset(&label, 0, sizeof(label));
    label.runs[0].font = FONT_MONO;
    label.runs[0].text = "ab";
    label.runs[1].font = FONT_TINY;
    label.runs[1].text = "c";
    label.run_count = 2;

    guide_label_obstacles_reset();
    guide_label_obstacles_record(NULL, &label);
    ASSERT_INT("compound guide label records one obstacle",
               g_guide_label_obstacles.count, 1);
    ASSERT_FLOAT("compound obstacle width sums both font runs",
                 g_guide_label_obstacles.items[0].width, 24.0f);

    /* Change the live stub matrix after recording: projection must use the
     * saved +0.5 X modelview, not whatever matrix happens to be current now. */
    memcpy(g_gl_stub_modelview_matrix, identity, sizeof(identity));
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.proj, identity, sizeof(identity));
    ctx.vw = 1024;
    ctx.vh = 768;
    projected = project_guide_label_obstacles(
        &ctx, ox, oy, ow, GUIDE_LABEL_OBSTACLE_MAX);
    ASSERT_INT("recorded guide obstacle projects on the canonical pass",
               projected, 1);
    ASSERT_FLOAT("saved modelview positions projected obstacle X", ox[0], 768.0f);
    ASSERT_FLOAT("projected obstacle Y", oy[0], 384.0f);
    ASSERT_FLOAT("projected obstacle keeps measured width", ow[0], 24.0f);

    guide_label_obstacles_reset();
    projected = project_guide_label_obstacles(
        &ctx, ox, oy, ow, GUIDE_LABEL_OBSTACLE_MAX);
    ASSERT_INT("obstacle reset clears the prior subpass", projected, 0);

    label.pos[0] = 5.0f;
    guide_label_obstacles_record(NULL, &label);
    projected = project_guide_label_obstacles(
        &ctx, ox, oy, ow, GUIDE_LABEL_OBSTACLE_MAX);
    ASSERT_INT("off-screen guide label is not a layout obstacle", projected, 0);
    guide_label_obstacles_reset();
}

static void init_test_vertex_label_ctx(VertexLabelCtx *ctx, int scope) {
    float identity[16];
    memset(ctx, 0, sizeof(*ctx));
    set_identity(identity);
    memcpy(ctx->proj, identity, sizeof(identity));
    ctx->mode = OVERLAY_VERTEX_LABEL_INDEX;
    ctx->scope = scope;
    ctx->vw = 1024;
    ctx->vh = 768;
    ctx->count = 1;
    ctx->labels[0].num = 0;
    ctx->labels[0].anchor_x = 512.0f;
    ctx->labels[0].anchor_y = 384.0f;
    strcpy(ctx->labels[0].idx, " v0");
}

static void reset_test_label_sticky(void) {
    memset(g_label_sticky, 0, sizeof(g_label_sticky));
    g_label_sticky_frame = 1;
    g_label_sticky_scope = -1;
    g_label_sticky_mode = -1;
}

static void add_identity_guide_obstacle(float sx, float sy, float width) {
    GuideLabelObstacle *obstacle;
    float identity[16];

    ASSERT_TRUE("test obstacle capacity",
                g_guide_label_obstacles.count < GUIDE_LABEL_OBSTACLE_MAX);
    if (g_guide_label_obstacles.count >= GUIDE_LABEL_OBSTACLE_MAX)
        return;
    obstacle = &g_guide_label_obstacles.items[g_guide_label_obstacles.count++];
    set_identity(identity);
    memcpy(obstacle->modelview, identity, sizeof(identity));
    obstacle->pos[0] = sx / 512.0f - 1.0f;
    obstacle->pos[1] = sy / 384.0f - 1.0f;
    obstacle->pos[2] = 0.0f;
    obstacle->width = width;
}

static void test_vertex_label_guide_obstacle_layout(void) {
    printf("--- edit_overlays vertex labels avoid guide obstacles ---\n");

    float placed_x[1] = { 0.0f };
    float placed_y[1] = { 0.0f };
    float placed_w[1] = { 10.0f };
    float obstacle_x[1] = { 0.0f };
    float obstacle_y[1] = { 0.0f };
    float obstacle_w[1] = { 10.0f };
    VertexLabelCtx ctx;
    VertexLabelCtx at_vertex;
    int row;

    /* A 2 px vertex/vertex overlap is tolerated by the 4 px incumbent
     * shrink, but the identical guide obstacle remains a strict collision. */
    ASSERT_INT("incumbent leniency can relax vertex overlap",
               vertex_label_cand_clashes(
                   placed_x, placed_y, placed_w, 1,
                   obstacle_x, obstacle_y, obstacle_w, 0,
                   8.0f, 0.0f, 10.0f, -VERTEX_LABEL_KEEP_SHRINK), 0);
    ASSERT_INT("incumbent leniency never relaxes guide overlap",
               vertex_label_cand_clashes(
                   placed_x, placed_y, placed_w, 0,
                   obstacle_x, obstacle_y, obstacle_w, 1,
                   8.0f, 0.0f, 10.0f, -VERTEX_LABEL_KEEP_SHRINK), 1);

    reset_test_label_sticky();
    guide_label_obstacles_reset();
    add_identity_guide_obstacle(512.0f, 384.0f, 32.0f);
    init_test_vertex_label_ctx(&ctx, OVERLAY_SCOPE_LAST_INSTANCE);
    vertex_labels_layout_and_draw(&ctx);
    ASSERT_INT("vertex label remains drawable beside one guide obstacle",
               ctx.labels[0].drawn, 1);
    ASSERT_TRUE("vertex label moves away from the fixed guide label",
                ctx.labels[0].draw_y != ctx.labels[0].anchor_y);

    at_vertex = ctx;
    at_vertex.placement = OVERLAY_LABEL_PLACEMENT_AT_VERTEX;
    at_vertex.labels[0].draw_y = 0.0f;
    vertex_labels_layout_and_draw(&at_vertex);
    ASSERT_INT("At vertex still draws through guide obstacles",
               at_vertex.labels[0].drawn, 1);
    ASSERT_FLOAT("At vertex preserves the exact anchor",
                 at_vertex.labels[0].draw_y, at_vertex.labels[0].anchor_y);

    reset_test_label_sticky();
    guide_label_obstacles_reset();
    for (row = -VERTEX_LABEL_NUDGE_STEPS;
         row <= VERTEX_LABEL_NUDGE_STEPS; row++) {
        add_identity_guide_obstacle(
            512.0f,
            384.0f + (float)row *
                     (VERTEX_LABEL_LINE_H + VERTEX_LABEL_GAP),
            32.0f);
    }
    init_test_vertex_label_ctx(&ctx, OVERLAY_SCOPE_LAST_INSTANCE);
    vertex_labels_layout_and_draw(&ctx);
    ASSERT_INT("label drops when every bounded row is obstructed",
               ctx.labels[0].drawn, 0);
    guide_label_obstacles_reset();
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
    idx_ctx.block_instances = 1;   /* mid-block: not a copy boundary */
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

/* Overlay scope: last-instance mode labels only the last unrolled copy of a
 * looped primitive (the parametric-torus duplicate-label fix), while
 * all-instances mode labels every vertex with a globally-unique number. The
 * walk resets vertex_idx_in_block to 0 at each block, which is how the callback
 * detects block (loop-iteration) boundaries - and how the forward-only walk
 * knows to discard the copy it had collected so far. */
static void test_overlay_scope(void) {
    printf("--- edit_overlays overlay scope ---\n");

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

    /* Last-instance: only block 1's two vertices, numbered by in-block index. */
    VertexLabelCtx one;
    memset(&one, 0, sizeof(one));
    one.mode = OVERLAY_VERTEX_LABEL_INDEX;
    one.scope = OVERLAY_SCOPE_LAST_INSTANCE;
    memcpy(one.proj, id, sizeof(id));
    one.vw = 1024;
    one.vh = 768;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = vidx[i];
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &one);
    }
    ASSERT_INT("last-instance collects only one block", one.count, 2);
    ASSERT_TRUE("last-instance numbers by in-block index",
                strcmp(one.labels[0].idx, " v0") == 0 &&
                strcmp(one.labels[1].idx, " v1") == 0);
    ASSERT_TRUE("last-instance keeps the LAST block's vertices",
                one.labels[0].anchor_x ==
                    (verts[2][0] * 0.5f + 0.5f) * 1024.0f &&
                one.labels[1].anchor_x ==
                    (verts[3][0] * 0.5f + 0.5f) * 1024.0f);

    /* All-instances: every vertex, numbered globally and uniquely. */
    VertexLabelCtx all;
    memset(&all, 0, sizeof(all));
    all.mode = OVERLAY_VERTEX_LABEL_INDEX;
    all.scope = OVERLAY_SCOPE_ALL_INSTANCES;
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

    /* Whole-scene: same collection rule as all-instances at the callback
     * level - the difference is upstream, where the walk is not narrowed to
     * the cursor's block. Numbering must stay global, or vertices from
     * different blocks would collide on v0. */
    VertexLabelCtx scene;
    memset(&scene, 0, sizeof(scene));
    scene.mode = OVERLAY_VERTEX_LABEL_INDEX;
    scene.scope = OVERLAY_SCOPE_WHOLE_SCENE;
    memcpy(scene.proj, id, sizeof(id));
    scene.vw = 1024;
    scene.vh = 768;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = vidx[i];
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &scene);
    }
    ASSERT_INT("whole-scene collects every vertex", scene.count, 4);
    ASSERT_TRUE("whole-scene numbers globally (no duplicates)",
                strcmp(scene.labels[0].idx, " v0") == 0 &&
                strcmp(scene.labels[1].idx, " v1") == 0 &&
                strcmp(scene.labels[2].idx, " v2") == 0 &&
                strcmp(scene.labels[3].idx, " v3") == 0);

    /* At-vertex placement keeps the collection rule of whatever scope it runs
     * under (here all-instances: every vertex) but numbers in-block, because
     * a label sitting on its own vertex has nothing to disambiguate. That is
     * what the retired at-vertex *scope* did, now reachable under any scope. */
    VertexLabelCtx at_vert;
    memset(&at_vert, 0, sizeof(at_vert));
    at_vert.mode = OVERLAY_VERTEX_LABEL_INDEX;
    at_vert.scope = OVERLAY_SCOPE_ALL_INSTANCES;
    at_vert.placement = OVERLAY_LABEL_PLACEMENT_AT_VERTEX;
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

    /* The same scope decluttered numbers globally instead - the placement is
     * what decides, since only a floated label can be ambiguous. */
    ASSERT_TRUE("decluttered all-instances still numbers globally",
                strcmp(all.labels[2].idx, " v2") == 0);

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

/* Single-polygon label scope: narrows last-instance further to the
 * ordinal range of the cursor's primitive, but only inside a glBegin
 * block (primitive_mode != 0) - tess (GLU) polygons keep whole-block
 * labels regardless of the resolved range. */
static void test_single_polygon_label_scope(void) {
    printf("--- edit_overlays single-polygon label scope ---\n");

    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    extern float g_gl_stub_modelview_matrix[16];
    memcpy(g_gl_stub_modelview_matrix, id, sizeof(id));

    const float verts[8][3] = {
        { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.0f, 0.0f },
        { 0.2f, 0.0f, 0.0f }, { 0.3f, 0.0f, 0.0f },
        { -0.1f, 0.0f, 0.0f }, { -0.2f, 0.0f, 0.0f },
        { -0.3f, 0.0f, 0.0f }, { -0.4f, 0.0f, 0.0f },
    };

    ReplayVertexWalkState state;
    memset(&state, 0, sizeof(state));

    /* A single glBegin(GL_QUADS) block (nonzero primitive_mode): only
     * ordinals [4, 7] (the second quad) get labeled, numbered by
     * in-block index. */
    VertexLabelCtx quad;
    memset(&quad, 0, sizeof(quad));
    quad.mode = OVERLAY_VERTEX_LABEL_INDEX;
    quad.scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    quad.poly_valid = 1;
    quad.poly_first = 4;
    quad.poly_last = 7;
    memcpy(quad.proj, id, sizeof(id));
    quad.vw = 1024;
    quad.vh = 768;
    state.primitive_mode = GL_QUADS;
    for (int i = 0; i < 8; i++) {
        state.vertex_idx_in_block = i;
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &quad);
    }
    ASSERT_INT("single-polygon labels only the selected quad's 4 vertices",
               quad.count, 4);
    ASSERT_TRUE("single-polygon numbers by in-block index",
                strcmp(quad.labels[0].idx, " v4") == 0 &&
                strcmp(quad.labels[1].idx, " v5") == 0 &&
                strcmp(quad.labels[2].idx, " v6") == 0 &&
                strcmp(quad.labels[3].idx, " v7") == 0);

    /* A tess (GLU) polygon reports primitive_mode == 0: the per-primitive
     * filter is bypassed and every vertex in the selected block labels,
     * same as plain last-instance scope. */
    VertexLabelCtx tess;
    memset(&tess, 0, sizeof(tess));
    tess.mode = OVERLAY_VERTEX_LABEL_INDEX;
    tess.scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    tess.poly_valid = 1;
    tess.poly_first = 0;
    tess.poly_last = 0;  /* would filter to just ordinal 0 if applied */
    memcpy(tess.proj, id, sizeof(id));
    tess.vw = 1024;
    tess.vh = 768;
    state.primitive_mode = 0;
    for (int i = 0; i < 8; i++) {
        state.vertex_idx_in_block = i;
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &tess);
    }
    ASSERT_INT("tess (primitive_mode 0) bypasses the per-primitive filter",
               tess.count, 8);

    /* GL_TRIANGLE_FAN: the fan center (ordinal 0) labels alongside the
     * selected [first, last] pair. */
    VertexLabelCtx fan;
    memset(&fan, 0, sizeof(fan));
    fan.mode = OVERLAY_VERTEX_LABEL_INDEX;
    fan.scope = OVERLAY_SCOPE_SINGLE_POLYGON;
    fan.poly_valid = 1;
    fan.poly_first = 2;
    fan.poly_last = 3;
    fan.poly_fan_anchor = 1;
    memcpy(fan.proj, id, sizeof(id));
    fan.vw = 1024;
    fan.vh = 768;
    state.primitive_mode = GL_TRIANGLE_FAN;
    for (int i = 0; i < 4; i++) {
        state.vertex_idx_in_block = i;
        on_vertex_number_label(&state, verts[i][0], verts[i][1], verts[i][2], &fan);
    }
    ASSERT_INT("fan anchor labels center + selected pair (3 vertices)",
               fan.count, 3);
    ASSERT_TRUE("fan anchor label set is {v0, v2, v3}",
                strcmp(fan.labels[0].idx, " v0") == 0 &&
                strcmp(fan.labels[1].idx, " v2") == 0 &&
                strcmp(fan.labels[2].idx, " v3") == 0);
}

/* Occlusion cull: a vertex is dropped when the scene depth buffer holds nearer
 * geometry at its pixel. This is unconditional - it is not a scope, so the
 * assertion runs over every scope rather than one opted-in mode. Uses a tiny
 * 4x4 depth grid so the projected pixel lookup is easy to reason about
 * (identity proj over a 4x4 viewport maps object (x,y) ->
 * ((x*0.5+0.5)*4, (y*0.5+0.5)*4)). */
static void test_vertex_label_visible_occlusion(void) {
    printf("--- edit_overlays vertex label occlusion cull ---\n");

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

    for (int scope = 0; scope < OVERLAY_SCOPE_COUNT; scope++) {
        char msg[128];
        VertexLabelCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.mode = OVERLAY_VERTEX_LABEL_INDEX;
        ctx.scope = scope;
        memcpy(ctx.proj, id, sizeof(id));
        ctx.vw = 4;
        ctx.vh = 4;
        ctx.depthbuf = depth;

        /* Both vertices sit in one block, so no scope drops either on
         * block-selection grounds; only the depth cull can. */
        state.vertex_idx_in_block = 0;
        /* (0,0,0) -> pixel (2,2), window depth 0.5 vs scene 1.0 -> visible. */
        on_vertex_number_label(&state, 0.0f, 0.0f, 0.0f, &ctx);
        state.vertex_idx_in_block = 1;
        /* (-0.5,-0.5,0) -> pixel (1,1), depth 0.5 vs scene 0.0 -> occluded. */
        on_vertex_number_label(&state, -0.5f, -0.5f, 0.0f, &ctx);

        snprintf(msg, sizeof(msg),
                 "scope %d keeps the unoccluded vertex and drops the occluded one",
                 scope);
        ASSERT_INT(msg, ctx.count, 1);
    }
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

    /* Whole-scene scope: this covers the arrow geometry, so take the one
     * scope that filters nothing. Scope filtering itself is
     * test_normal_vectors_overlay_scope. */
    NormalVectorRenderCtx ctx = {
        .scale = 2.0f,
        .show_all = 1,
        .replay_display = REPLAY_NORMAL_DISPLAY_OFF,
        .anchor_idx = -1,
        .stop_flag = NULL,
        .scope = OVERLAY_SCOPE_WHOLE_SCENE,
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

/* Normal vectors honor the overlay scope. Two unrolled copies of one source
 * line (what a for loop flattens to), so the three interesting scopes give
 * three different answers over the same program. Vertices carry distinct
 * coordinates so the trace can tell the copies apart. */
static void test_normal_vectors_overlay_scope(void) {
    printf("--- edit_overlays normal vectors honor the overlay scope ---\n");

    enum { CURSOR_LINE = 7, OTHER_LINE = 9 };
    GLCmd cmds[9];
    /* Copy 0 of the cursor's block. */
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[1], CMD_VERTEX3F, 1.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[2], CMD_END, 0, 0, 0);
    /* Copy 1 of the same source line. */
    mk_cmd(&cmds[3], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[4], CMD_VERTEX3F, 2.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[5], CMD_END, 0, 0, 0);
    /* An unrelated block the cursor is not on. */
    mk_cmd(&cmds[6], CMD_BEGIN, (float)GL_TRIANGLES, 0, 0);
    mk_cmd(&cmds[7], CMD_VERTEX3F, 3.0f, 0.0f, 0.0f);
    mk_cmd(&cmds[8], CMD_END, 0, 0, 0);
    cmds[1].src_cmd_idx = CURSOR_LINE;
    cmds[4].src_cmd_idx = CURSOR_LINE;
    cmds[7].src_cmd_idx = OTHER_LINE;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 9;
    walk.show_normal_vectors = 1;
    walk.cursor.edit_line_idx = CURSOR_LINE;
    /* Match by source line, not by a flat block range. */
    walk.cursor.cursor_block_begin = -1;
    walk.cursor.cursor_block_end = -1;

    TraceLog log;

    walk.cursor_overlay_scope = OVERLAY_SCOPE_WHOLE_SCENE;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("whole scene: arrow on the first copy",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
    ASSERT_INT("whole scene: arrow on the second copy",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);
    ASSERT_INT("whole scene: arrow on the unrelated block",
               trace_count_line(&log, "glVertex3f 3 0 0"), 1);

    walk.cursor_overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("all instances: arrow on the first copy",
               trace_count_line(&log, "glVertex3f 1 0 0"), 1);
    ASSERT_INT("all instances: arrow on the second copy",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);
    ASSERT_INT("all instances: unrelated block is out of scope",
               trace_count_line(&log, "glVertex3f 3 0 0"), 0);

    walk.cursor_overlay_scope = OVERLAY_SCOPE_LAST_INSTANCE;
    trace_begin();
    edit_overlays_render_normal_vectors(&walk);
    trace_end(&log);
    ASSERT_INT("last instance: earlier copy is out of scope",
               trace_count_line(&log, "glVertex3f 1 0 0"), 0);
    ASSERT_INT("last instance: arrow on the last copy only",
               trace_count_line(&log, "glVertex3f 2 0 0"), 1);
    ASSERT_INT("last instance: unrelated block is out of scope",
               trace_count_line(&log, "glVertex3f 3 0 0"), 0);
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
    Render3dGuideLabelSink label_sink;
    memset(&label_sink, 0, sizeof(label_sink));
    label_sink.record = guide_label_obstacles_record;
    guide_label_obstacles_reset();
    trace_begin();
    edit_overlays_render_normal_vectors_with_sink(&walk, &label_sink);
    trace_end(&log);
    ASSERT_INT("replay normal direction label is anchored at the arrow tip",
               trace_count_line(&log, "glRasterPos3f 1 0.35 0"), 1);
    char label_text[128];
    trace_bitmap_text(&log, label_text, sizeof(label_text));
    ASSERT_TRUE("replay normal direction label uses %.2f precision",
                strstr(label_text, " n=(0.00, 1.00, 0.00)") != NULL);
    ASSERT_INT("replay normal direction records one guide obstacle",
               g_guide_label_obstacles.count, 1);
    ASSERT_FLOAT("replay normal obstacle uses the arrow-tip X",
                 g_guide_label_obstacles.items[0].pos[0], 1.0f);
    ASSERT_FLOAT("replay normal obstacle uses the arrow-tip Y",
                 g_guide_label_obstacles.items[0].pos[1], 0.35f);
    ASSERT_TRUE("replay normal obstacle measures both text runs",
                g_guide_label_obstacles.items[0].width > 0.0f);
    guide_label_obstacles_reset();

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
                                        OVERLAY_SCOPE_ALL_INSTANCES,
                                        OVERLAY_LABEL_PLACEMENT_DECLUTTERED,
                                        1);
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
                                        OVERLAY_SCOPE_ALL_INSTANCES,
                                        OVERLAY_LABEL_PLACEMENT_DECLUTTERED,
                                        1);
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

    /* post_overlays orchestrator: runs outlines + points + guides + normals
     * in one pass (labels moved to the post-resolve hook below). Build the
     * snapshot pack and verify it emits the flattened vertices through at
     * least one overlay path. */
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
    pack.depth_readback_supported = 1;
    pack.show_normal_vectors = 1;
    trace_begin();
    edit_overlays_post_overlays(&pack);
    trace_end(&log);
    ASSERT_TRUE("post_overlays emits the flattened vertices",
                trace_count_line(&log, "glVertex3f 0.25 0.5 0.75") >= 1);
    /* The per-pass orchestrator must NOT draw the bitmap-text labels any
     * more - inside the accumulation loop they'd ghost across sub-passes. */
    ASSERT_INT("post_overlays draws no vertex-number labels",
               trace_count_sym(&log, "glRasterPos2f"), 0);

    /* The once-per-frame post-resolve hook owns the label pass. */
    trace_begin();
    edit_overlays_post_resolve_overlays(&pack);
    trace_end(&log);
    ASSERT_INT("post_resolve_overlays draws vertex-number labels",
               trace_count_sym(&log, "glRasterPos2f") >= 1, 1);
    ASSERT_INT("supported label pass snapshots scene depth",
               trace_count_sym(&log, "glReadPixels"), 1);

    /* WebGL cannot read the default framebuffer's depth. Its gl4es shim may
     * silently leave the destination untouched, so unsupported contexts must
     * skip the read rather than cull against indeterminate heap contents. */
    pack.depth_readback_supported = 0;
    trace_begin();
    edit_overlays_post_resolve_overlays(&pack);
    trace_end(&log);
    ASSERT_INT("unsupported label pass skips depth readback",
               trace_count_sym(&log, "glReadPixels"), 0);
    ASSERT_INT("unsupported label pass keeps vertex-number labels",
               trace_count_sym(&log, "glRasterPos2f") >= 1, 1);

    /* NULL pack is a safe no-op. */
    edit_overlays_post_overlays(NULL);
    edit_overlays_post_resolve_overlays(NULL);
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
               trace_count_sym(&log, "glPointSize") >= 2, 1);
    ASSERT_INT("cursor guide uses the flat vertex coords",
               trace_count_line(&log, "glVertex3f 0.25 0.5 0.75"), 2);

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
               trace_count_sym(&log, "glPointSize") >= 2, 1);
    ASSERT_INT("append-row guide uses live input coords",
               trace_count_line(&log, "glVertex3f -0.25 -0.5 0"), 2);
}

/* A cursor line inside a loop expands to several flat vertices. The guide has
 * to read the same copy the highlight does: prefer_last_instance (set by the
 * last-instance overlay scopes) moves it from the first expansion to the last,
 * so it agrees with the loop values the variable panel is showing. */
static void test_cursor_guides_follow_last_instance(void) {
    printf("--- edit_overlays cursor guides prefer the last instance ---\n");

    /* Two unrolled copies of source line 1, at different positions. */
    GLCmd cmds[4];
    mk_cmd(&cmds[0], CMD_BEGIN, (float)GL_POINTS, 0, 0);
    cmds[0].src_cmd_idx = 0;
    mk_cmd(&cmds[1], CMD_VERTEX3F, 0.25f, 0.5f, 0.75f);
    cmds[1].src_cmd_idx = 1;
    mk_cmd(&cmds[2], CMD_VERTEX3F, -0.25f, -0.5f, -0.75f);
    cmds[2].src_cmd_idx = 1;
    mk_cmd(&cmds[3], CMD_END, 0, 0, 0);
    cmds[3].src_cmd_idx = 2;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 4;
    walk.cursor.edit_line_idx = 1;
    walk.cursor.cursor_block_begin = -1;
    walk.cursor.cursor_block_end = -1;

    const char *input = "glVertex3f(x, y, z)";
    Render3dGuideSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.show_guides = 1;
    snap.input = input;
    snap.input_len = (int)strlen(input);
    snap.edit_line_idx = 1;
    snap.flat_program = walk.program;
    snap.vertex_filled[0] = snap.vertex_filled[1] = snap.vertex_filled[2] = 1;
    snap.vertex_n_filled = 3;
    snap.alpha_scale = 1.0f;

    TraceLog log;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_TRUE("default guide reads the first expansion",
                trace_count_line(&log, "glVertex3f 0.25 0.5 0.75") > 0);
    ASSERT_INT("default guide ignores the later expansion",
               trace_count_line(&log, "glVertex3f -0.25 -0.5 -0.75"), 0);

    snap.prefer_last_instance = 1;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_TRUE("last-instance guide reads the final expansion",
                trace_count_line(&log, "glVertex3f -0.25 -0.5 -0.75") > 0);
    ASSERT_INT("last-instance guide ignores the first expansion",
               trace_count_line(&log, "glVertex3f 0.25 0.5 0.75"), 0);

    /* Replay anchors on its own step, so the flag must not divert the walk. */
    snap.replaying = 1;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);
    ASSERT_INT("replay draws no cursor geometry guide",
               trace_count_line(&log, "glVertex3f -0.25 -0.5 -0.75"), 0);
}

static void test_cursor_guides_render_for_raster_pos(void) {
    printf("--- edit_overlays cursor guides for glRasterPos3f ---\n");

    GLCmd cmds[1];
    mk_cmd(&cmds[0], CMD_RASTER_POS3F, 1.25f, 2.5f, 3.75f);
    cmds[0].src_cmd_idx = 0;

    OverlayWalkCtx walk;
    memset(&walk, 0, sizeof(walk));
    walk.program.cmds = cmds;
    walk.program.cmd_count = 1;
    walk.cursor.edit_line_idx = 0;
    walk.cursor.cursor_block_begin = -1;
    walk.cursor.cursor_block_end = -1;

    const char *input = "glRasterPos3f(0, 0, 0)";
    Render3dGuideSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.show_guides = 1;
    snap.input = input;
    snap.input_len = (int)strlen(input);
    snap.edit_line_idx = 0;
    snap.flat_program = walk.program;
    snap.raster_pos_n_filled = 3;
    snap.raster_pos_args[0] = 0.0f;
    snap.raster_pos_args[1] = 0.0f;
    snap.raster_pos_args[2] = 0.0f;
    snap.alpha_scale = 1.0f;

    TraceLog log;
    trace_begin();
    edit_overlays_render_cursor_guides(&snap, &walk);
    trace_end(&log);

    ASSERT_INT("raster guide uses flat raster-pos coords",
               trace_count_line(&log, "glVertex3f 1.25 2.5 3.75"), 2);
    ASSERT_INT("raster guide reuses point marker point-size calls",
               trace_count_sym(&log, "glPointSize") >= 2, 1);
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

static void test_guide_obstacle_store_per_subpass(void) {
    printf("--- edit_overlays guide obstacle store per subpass ---\n");

    GLCmd unused_cmd;
    OverlaySnapshotPack pack;
    const char *input = "glTranslatef(2,0,0)";

    memset(&unused_cmd, 0, sizeof(unused_cmd));
    memset(&pack, 0, sizeof(pack));
    pack.walk.program.cmds = &unused_cmd;
    pack.walk.program.cmd_count = 0;
    pack.walk.cursor.edit_line_idx = 0;
    pack.snapshot.show_guides = 1;
    pack.snapshot.input = input;
    pack.snapshot.input_len = (int)strlen(input);
    pack.snapshot.edit_line_idx = 0;
    pack.snapshot.source_cmd_count = 0;
    pack.snapshot.flat_program = pack.walk.program;
    pack.snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
    pack.snapshot.xform_args[0] = 2.0f;
    pack.snapshot.xform_filled[0] = 1;
    pack.snapshot.xform_filled[1] = 1;
    pack.snapshot.xform_filled[2] = 1;
    pack.snapshot.xform_n_filled = 3;
    pack.snapshot.alpha_scale = 1.0f;
    pack.vertex_label_mode = OVERLAY_VERTEX_LABEL_INDEX;
    pack.overlay_scope = OVERLAY_SCOPE_LAST_INSTANCE;

    guide_label_obstacles_reset();
    edit_overlays_post_overlays(&pack);
    ASSERT_INT("active subpass rebuilds the translate-label obstacle",
               g_guide_label_obstacles.count, 1);

    pack.snapshot.show_guides = 0;
    edit_overlays_post_overlays(&pack);
    ASSERT_INT("next subpass clears stale guide-label obstacles",
               g_guide_label_obstacles.count, 0);

    pack.snapshot.show_guides = 1;
    pack.vertex_label_placement = OVERLAY_LABEL_PLACEMENT_AT_VERTEX;
    edit_overlays_post_overlays(&pack);
    ASSERT_INT("At vertex bypass does not collect guide-label obstacles",
               g_guide_label_obstacles.count, 0);
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
    test_current_poly_highlight_respects_overlay_scope();
    test_highlight_clip_planes();
    test_overlays_honor_culling();
    test_overlays_honor_stencil_test();
    test_overlays_scope_push_pop_attrib();
    test_single_polygon_highlight_strip_winding();
    test_overlays_honor_color_mask();
    test_single_polygon_highlight();
    test_single_polygon_highlight_fan_anchor();
    test_single_polygon_highlight_bold_style();
    test_render_outlines_tess();
    test_render_outlines_glut();
    test_find_next_vertex_args();
    test_cursor_guide_snapshot_with_flat_args();
    test_mat4_math();
    test_sanitize_zero();
    test_guide_label_obstacle_record_and_projection();
    test_vertex_label_guide_obstacle_layout();
    test_on_vertex_number_label_callback();
    test_overlay_scope();
    test_single_polygon_label_scope();
    test_vertex_label_visible_occlusion();
    test_on_normal_vector_arrow_callback();
    test_render_normal_vectors_replay_only();
    test_normal_vectors_overlay_scope();
    test_render_replay_vertex_label();
    test_vertex_numbers_use_source_begin_block();
    test_render_via_repl_program();
    test_cursor_guides_render_for_unterminated_begin();
    test_cursor_guides_follow_last_instance();
    test_cursor_guides_render_for_raster_pos();
    test_cursor_guides_render_for_new_transform_line();
    test_guide_obstacle_store_per_subpass();

    return test_harness_report(&g_harness, "test_edit_overlays");
}
