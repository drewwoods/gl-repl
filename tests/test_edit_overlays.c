/*
 * tests/test_edit_overlays.c - Unit tests for edit overlays subsystem.
 */
#include "editor/state.h"
#include "repl/state_views.h"
#include "repl/state_owners.h"
#include "repl/core.h"
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
    glr_ctrl_reset_all();

    editor_state_edit_line_set(42);
    repl_state_flat_program_set_current_block(10, 20, 0);

    ReplayVertexWalkContext walk = edit_overlays_build_vertex_walk_context(1);
    ASSERT_INT("selected block only matches input", walk.selected_block_only, 1);
    ASSERT_INT("edit_line_idx matches editor state", walk.cursor.edit_line_idx, 42);
    ASSERT_INT("block begin matches program state", walk.cursor.cursor_block_begin, 10);
    ASSERT_INT("block end matches program state", walk.cursor.cursor_block_end, 20);
    ASSERT_INT("func_scope_mask starts at 0", walk.cursor.cursor_func_scope_mask, 0u);
}

int main(void) {
    printf("--- edit_overlays tests ---\n");
    test_outline_begin_mode_has_overlay();
    test_outline_cmd_matches_cursor();
    test_outline_block_matches_cursor();
    test_build_vertex_walk_context();

    return test_harness_report(&g_harness, "test_edit_overlays");
}
