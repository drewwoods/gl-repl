#ifdef GL_STUBS
#include "support/test_harness.h"
#include "config.h"
#include "ui/core/metrics.h"
#include "ui/core/text_layout.h"
#include "ui/core/text_panel.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#define STATUSBAR_H 22

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static UiTextPanelSnapshot make_snapshot(const UiTextPanelRow *rows,
                                         int row_count,
                                         int cp_x,
                                         int cp_y,
                                         int cp_w,
                                         int cp_h,
                                         int chrome_flags) {
    UiTextPanelSnapshot snap;

    memset(&snap, 0, sizeof(snap));
    snap.vp_w = 800;
    snap.vp_h = 600;
    snap.cp_x = cp_x;
    snap.cp_y = cp_y;
    snap.cp_w = cp_w;
    snap.cp_h = cp_h;
    snap.text_x = CODE_MARGIN_X + 5 * FONT_W;
    snap.wrap_at_comma = 1;
    snap.rows = rows;
    snap.row_count = row_count;
    snap.chrome_flags = chrome_flags;
    snap.statusbar_h = STATUSBAR_H;
    snap.input.input = "";
    snap.input.input_len = 0;
    snap.input.cursor = 0;
    snap.input.anchor = 0;
    snap.input.ghost = "";
    snap.input.hint = "";
    snap.search.query = "";
    return snap;
}

static int expected_visible_rows(int panel_h, int chrome_flags) {
    int statusbar_h = (chrome_flags & UI_TEXT_PANEL_CHROME_STATUSBAR)
                    ? STATUSBAR_H : 0;
    int available = panel_h - CODE_MARGIN_Y - 2 * LINE_H - 3 - statusbar_h;

    if (available < 0)
        return 1;
    return available / LINE_H + 1;
}

static int my_for_visual_row(const UiTextPanelSnapshot *snap, int visual_row) {
    int panel_top = snap->cp_y + snap->cp_h;
    int gl_y = panel_top - CODE_MARGIN_Y - 2 * LINE_H - visual_row * LINE_H;
    return snap->vp_h - gl_y;
}

static void test_visible_rows_respect_statusbar_flag(void) {
    UiTextPanelRow rows[3] = {
        {
            .text = "row 0",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .left_gutter_label = 1,
            .source_line_idx = 0,
            .search_row_idx = 0,
            .hit_eligible = 1,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
        {
            .text = "row 1",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .left_gutter_label = 2,
            .source_line_idx = 1,
            .search_row_idx = 1,
            .hit_eligible = 1,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
        {
            .text = "row 2",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .left_gutter_label = 3,
            .source_line_idx = 2,
            .search_row_idx = 2,
            .hit_eligible = 1,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
    };
    int cp_h = CODE_MARGIN_Y + 2 * LINE_H + 3 + STATUSBAR_H + LINE_H;
    UiTextPanelSnapshot snap_no_status = make_snapshot(rows, 3, 0, 0, 240, cp_h, 0);
    UiTextPanelSnapshot snap_with_status = make_snapshot(
        rows, 3, 0, 0, 240, cp_h, UI_TEXT_PANEL_CHROME_STATUSBAR);
    UiTextPanelOutput out = {0};
    UiHit hit;

    ASSERT_INT_EQ("visible rows without statusbar helper",
                  ui_text_panel_visible_lines_for_height(cp_h, 0, 0),
                  expected_visible_rows(cp_h, 0));
    ASSERT_INT_EQ("visible rows with statusbar helper",
                  ui_text_panel_visible_lines_for_height(
                      cp_h, 22, 0),
                  expected_visible_rows(cp_h,
                                        UI_TEXT_PANEL_CHROME_STATUSBAR));

    ui_text_panel_render(&snap_no_status, &out);
    ASSERT_INT_EQ("render visible rows without statusbar", out.visible_rows, 3);
    ASSERT_INT_EQ("render statusbar slot disabled", out.statusbar_slot.h, 0);

    hit = ui_text_panel_hit_test(&snap_no_status,
                                 snap_no_status.cp_x + snap_no_status.text_x,
                                 my_for_visual_row(&snap_no_status, 2));
    ASSERT_INT_EQ("third row clickable without statusbar", hit.kind,
                  UI_HIT_CODE_TEXT);
    ASSERT_INT_EQ("third row maps to line 2", hit.line_idx, 2);

    ui_text_panel_render(&snap_with_status, &out);
    ASSERT_INT_EQ("render visible rows with statusbar", out.visible_rows, 2);
    ASSERT_INT_EQ("render statusbar slot enabled", out.statusbar_slot.h,
                  STATUSBAR_H);

    hit = ui_text_panel_hit_test(&snap_with_status,
                                 snap_with_status.cp_x + snap_with_status.text_x,
                                 my_for_visual_row(&snap_with_status, 2));
    ASSERT_INT_EQ("third row clipped by statusbar", hit.kind, UI_HIT_NONE);
}

static void test_wrap_and_hit_are_panel_local(void) {
    static const char *k_text =
        "glVertex3f(123456, 234567, 345678, 456789, 567890, 678901)";
    UiTextPanelRow row = {
        .text = k_text,
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 7,
        .source_line_idx = 7,
        .search_row_idx = 7,
        .hit_eligible = 1,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 140, 0, 150, 180, 0);
    UiTextPanelOutput out = {0};
    snap.text_x = FONT_W;
    CodeLayout layout = code_layout_make(snap.cp_w, snap.text_x, FONT_W,
                                         snap.wrap_at_comma);
    int expected_rows = code_layout_row_count_for_text(k_text, &layout);
    UiHit hit;

    ui_text_panel_render(&snap, &out);
    ASSERT_INT_EQ("panel-local wrap count", out.total_rows, expected_rows);
    ASSERT_TRUE("test text actually wraps", expected_rows > 1);

    hit = ui_text_panel_hit_test(&snap,
                                 snap.cp_x + snap.text_x + 2 * FONT_W,
                                 my_for_visual_row(&snap, 0));
    ASSERT_INT_EQ("nonzero cp_x hit kind", hit.kind, UI_HIT_CODE_TEXT);
    ASSERT_INT_EQ("nonzero cp_x hit line", hit.line_idx, 7);
    ASSERT_INT_EQ("nonzero cp_x hit visual row", hit.visual_row, 0);
    ASSERT_INT_EQ("nonzero cp_x hit char index", hit.char_idx, 2);
}

static void test_virtual_row_keeps_hit_target_out_of_generic_hit(void) {
    UiTextPanelRow row = {
        .text = "virtual row",
        .kind = UI_TEXT_PANEL_ROW_VIRTUAL,
        .left_gutter_label = 7,
        .source_line_idx = -1,
        .hit_target_line_idx = 5,
        .hit_eligible = 1,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 220, 140, 0);
    UiHit hit;

    hit = ui_text_panel_hit_test(&snap,
                                 snap.cp_x + snap.text_x + 2 * FONT_W,
                                 my_for_visual_row(&snap, 0));
    ASSERT_INT_EQ("virtual row hit kind", hit.kind, UI_HIT_CODE_TEXT);
    ASSERT_INT_EQ("virtual row keeps line_idx unresolved in generic hit",
                  hit.line_idx, -1);
    ASSERT_INT_EQ("virtual row keeps cmd_idx", hit.cmd_idx, 0);
    ASSERT_INT_EQ("virtual row char index still computed", hit.char_idx, 2);
}

static void test_alpha_text_enables_blending(void) {
    UiTextPanelRow row = {
        .text = "alpha row",
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 1,
        .source_line_idx = 0,
        .search_row_idx = 0,
        .hit_eligible = 1,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 220, 140, 0);
    UiTextPanelOutput out = {0};
    unsigned long long opaque_enable;
    unsigned long long opaque_blend_func;

    gl_stub_counts_reset();
    ui_text_panel_render(&snap, &out);
    opaque_enable = gl_stub_counts[GL_STUB_glEnable];
    opaque_blend_func = gl_stub_counts[GL_STUB_glBlendFunc];

    row.color.has_alpha = 1;
    row.color.a = 0.35f;

    gl_stub_counts_reset();
    ui_text_panel_render(&snap, &out);

    ASSERT_TRUE("alpha text emits glyphs",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("alpha text adds a blend enable",
                gl_stub_counts[GL_STUB_glEnable] > opaque_enable);
    ASSERT_TRUE("alpha text adds a blend func",
                gl_stub_counts[GL_STUB_glBlendFunc] > opaque_blend_func);
}

/* #30: shadow segments no longer query glIsEnabled per-span — the caller
 * threads a blend_on bool instead. Verify a drop-shadow span still renders
 * correctly (enables blend, draws glyphs) without the per-span query. */
static void test_shadow_segment_blend_threaded(void) {
    UiTextPanelRow row = {
        .text = "shadow text here",
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 1,
        .source_line_idx = 0,
        .search_row_idx = 0,
        .hit_eligible = 1,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 220, 140, 0);
    UiTextPanelOutput out = {0};

    row.color_segments[0] = (UiTextPanelColorSegment){
        .char_start = 0,
        .char_count = 6,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
        .shadow = 1,
    };
    row.color_segments[1] = (UiTextPanelColorSegment){
        .char_start = 6,
        .char_count = 10,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
        .shadow = 0,
    };
    row.color_segment_count = 2;

    gl_stub_counts_reset();
    ui_text_panel_render(&snap, &out);

    ASSERT_INT_EQ("shadow span does not call glIsEnabled",
                  (int)gl_stub_counts[GL_STUB_glIsEnabled], 0);
    ASSERT_TRUE("shadow segment enables blend",
                gl_stub_counts[GL_STUB_glEnable] > 0);
    ASSERT_TRUE("shadow segment draws glyphs",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

/* #63 regression: text_panel_row_layout() is called per-row; pin that
 * it returns consistent results for the same row+snapshot so a future
 * caching refactor doesn't change behavior. We verify this indirectly by
 * checking wrap count and hit-test agree. */
static void test_row_layout_consistency(void) {
    static const char *k_long =
        "glVertex3f(111111, 222222, 333333, 444444, 555555, 666666)";
    UiTextPanelRow row = {
        .text = k_long,
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 1,
        .source_line_idx = 0,
        .search_row_idx = 0,
        .hit_eligible = 1,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 160, 200, 0);
    UiTextPanelOutput out1 = {0};
    UiTextPanelOutput out2 = {0};

    ui_text_panel_render(&snap, &out1);
    ui_text_panel_render(&snap, &out2);

    ASSERT_INT_EQ("repeated render: same total_rows",
                  out1.total_rows, out2.total_rows);
    ASSERT_INT_EQ("repeated render: same visible_rows",
                  out1.visible_rows, out2.visible_rows);
    ASSERT_TRUE("long text wraps", out1.total_rows > 1);

    UiHit hit = ui_text_panel_hit_test(&snap,
                                       snap.cp_x + snap.text_x + FONT_W,
                                       my_for_visual_row(&snap, 0));
    ASSERT_INT_EQ("hit-test agrees with render on wrapped row",
                  hit.kind, UI_HIT_CODE_TEXT);
}

/* #63 strengthening: the hoisted text_panel_row_layout in row_wrap_count
 * is now called once and used for both INPUT and regular branches. Pin
 * that an INPUT row and a TEXT row with the SAME text produce the same
 * wrap count — the hoist relies on the layout being row-kind-agnostic.
 * A regression that diverged the two branches' layouts (e.g. a future
 * refactor that re-derives layout per-branch with subtly different
 * parameters) would surface as a wrap-count mismatch here. */
static void test_row_layout_shared_across_kinds(void) {
    static const char *k_long_text =
        "glVertex3f(111111, 222222, 333333, 444444, 555555, 666666)";
    UiTextPanelRow text_row = {
        .text = k_long_text,
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 1,
        .source_line_idx = 0,
        .search_row_idx = 0,
        .hit_eligible = 1,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelRow input_row = {
        /* INPUT rows ignore .text — the renderer reads snap.input.input
         * instead. Set kind only and let the snapshot supply the text. */
        .text = "",
        .kind = UI_TEXT_PANEL_ROW_INPUT,
        .left_gutter_label = 1,
        .source_line_idx = -1,
        .search_row_idx = -1,
        .hit_eligible = 1,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };

    UiTextPanelSnapshot text_snap = make_snapshot(&text_row, 1, 0, 0, 160, 200, 0);
    UiTextPanelSnapshot input_snap = make_snapshot(&input_row, 1, 0, 0, 160, 200, 0);
    input_snap.input.input     = k_long_text;
    input_snap.input.input_len = (int)strlen(k_long_text);

    UiTextPanelOutput out_text  = {0};
    UiTextPanelOutput out_input = {0};
    ui_text_panel_render(&text_snap,  &out_text);
    ui_text_panel_render(&input_snap, &out_input);

    /* Both should wrap by the same amount because both use the hoisted
     * layout. A divergence would mean the row_layout call is no longer
     * shared between branches (the #63 regression). */
    ASSERT_INT_EQ("INPUT and TEXT rows wrap identically for same text",
                  out_input.total_rows, out_text.total_rows);
    ASSERT_TRUE("both wrap into multiple rows",
                out_text.total_rows > 1);
}

static void test_color_segments_enable_blending(void) {
    UiTextPanelRow row = {
        .text = "segment fade",
        .kind = UI_TEXT_PANEL_ROW_TEXT,
        .left_gutter_label = 1,
        .source_line_idx = 0,
        .search_row_idx = 0,
        .hit_eligible = 1,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 220, 140, 0);
    UiTextPanelOutput out = {0};
    unsigned long long base_enable;
    unsigned long long base_blend_func;

    gl_stub_counts_reset();
    ui_text_panel_render(&snap, &out);
    base_enable = gl_stub_counts[GL_STUB_glEnable];
    base_blend_func = gl_stub_counts[GL_STUB_glBlendFunc];

    row.color_segments[0] = (UiTextPanelColorSegment){
        .char_start = 0,
        .char_count = 7,
        .color = { 0.8f, 0.9f, 1.0f, 1.0f, 0 },
    };
    row.color_segments[1] = (UiTextPanelColorSegment){
        .char_start = 7,
        .char_count = 1,
        .color = { 0.8f, 0.9f, 1.0f, 0.35f, 1 },
    };
    row.color_segments[2] = (UiTextPanelColorSegment){
        .char_start = 8,
        .char_count = (int)strlen(row.text) - 8,
        .color = { 0.8f, 0.9f, 1.0f, 0.0f, 1 },
    };
    row.color_segment_count = 3;

    gl_stub_counts_reset();
    ui_text_panel_render(&snap, &out);

    ASSERT_TRUE("segmented text emits glyphs",
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("segmented text adds a blend enable",
                gl_stub_counts[GL_STUB_glEnable] > base_enable);
    ASSERT_TRUE("segmented text adds a blend func",
                gl_stub_counts[GL_STUB_glBlendFunc] > base_blend_func);
}

int main(void) {
    printf("--- ui_text_panel tests ---\n");

    test_visible_rows_respect_statusbar_flag();
    test_wrap_and_hit_are_panel_local();
    test_virtual_row_keeps_hit_target_out_of_generic_hit();
    test_alpha_text_enables_blending();
    test_shadow_segment_blend_threaded();
    test_row_layout_consistency();
    test_row_layout_shared_across_kinds();
    test_color_segments_enable_blending();

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_text_panel");
}

#else

int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}

#endif
