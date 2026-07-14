#ifdef GL_STUBS
#include "support/test_harness.h"
#include "config.h"
#include "ui/core/metrics.h"
#include "ui/core/text_layout.h"
#include "ui/core/text_panel.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <math.h>
#include <string.h>

#define STATUSBAR_H 22

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define TRACE_PATH "build/test_ui_text_panel_trace.txt"

static void render_glyph_counts(UiTextPanelSnapshot *snap,
                                int *out_hyphens,
                                int *out_rules) {
    UiTextPanelOutput out = {0};
    FILE *trace;
    char line[128];
    int hyphens = 0;
    int rules = 0;

    gl_stub_counts_reset();
    gl_stub_trace_open(TRACE_PATH);
    ui_text_panel_render(snap, &out);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (trace) {
        while (fgets(line, sizeof(line), trace)) {
            int ch;
            if (sscanf(line, "glutBitmapCharacter %d", &ch) != 1 &&
                sscanf(line, "glutBitmapStringByte %d", &ch) != 1)
                continue;
            if (ch == '-')
                hyphens++;
            else if (ch == 0x12)
                rules++;
        }
        fclose(trace);
    }

    *out_hyphens = hyphens;
    *out_rules = rules;
}

static unsigned long long rendered_text_call_count(void) {
    return gl_stub_counts[GL_STUB_glutBitmapCharacter] +
           gl_stub_counts[GL_STUB_glutBitmapString];
}

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

static void test_comment_rule_ligature_is_comment_line_only(void) {
    UiTextPanelRow rows[5] = {
        {
            .text = "// --- Heading ---",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
            .color_segments = {
                { .char_start = 0, .char_count = 4,
                  .color = { 0.8f, 0.8f, 0.8f, 1.0f, 0 } },
                { .char_start = 4, .char_count = 12,
                  .color = { 0.7f, 0.7f, 0.7f, 1.0f, 0 } },
            },
            .color_segment_count = 2,
        },
        {
            .text = " \t// --",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
        {
            .text = "// x - y",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
        {
            .text = "code--value; // ---",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
        {
            .text = "\"// --\"",
            .kind = UI_TEXT_PANEL_ROW_TEXT,
            .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
        },
    };
    UiTextPanelSnapshot snap = make_snapshot(rows, 5, 0, 0, 800, 600, 0);
    int hyphens;
    int rules;

    printf("--- dash-rule comment lines ---\n");
    snap.comment_rule_ligature = 1;
    render_glyph_counts(&snap, &hyphens, &rules);
    ASSERT_INT_EQ("comment dash runs use rule glyph", rules, 6);
    ASSERT_INT_EQ("non-comment and lone dashes stay hyphens", hyphens, 10);
    ASSERT_TRUE("freeglut batches text with bitmap strings",
                gl_stub_counts[GL_STUB_glutBitmapString] > 0);
    ASSERT_INT_EQ("freeglut text path avoids character submissions",
                  (int)gl_stub_counts[GL_STUB_glutBitmapCharacter], 0);

    snap.comment_rule_ligature = 0;
    render_glyph_counts(&snap, &hyphens, &rules);
    ASSERT_INT_EQ("disabled ligature emits no rule glyphs", rules, 0);
    ASSERT_INT_EQ("disabled ligature preserves every hyphen", hyphens, 16);
}

static void test_comment_rule_ligature_applies_to_live_comment_input(void) {
    UiTextPanelRow row = {
        .text = "",
        .kind = UI_TEXT_PANEL_ROW_INPUT,
        .color = { 1.0f, 1.0f, 1.0f, 1.0f, 0 },
    };
    UiTextPanelSnapshot snap = make_snapshot(&row, 1, 0, 0, 800, 600, 0);
    int hyphens;
    int rules;

    snap.comment_rule_ligature = 1;
    snap.input.input = "  // ---";
    snap.input.input_len = (int)strlen(snap.input.input);
    snap.input.cursor = snap.input.input_len;
    snap.input.anchor = snap.input.cursor;
    render_glyph_counts(&snap, &hyphens, &rules);
    ASSERT_INT_EQ("live comment input uses rule glyph", rules, 3);
    ASSERT_INT_EQ("live comment rule has no hyphen glyph", hyphens, 0);
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
                rendered_text_call_count() > 0);
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
                rendered_text_call_count() > 0);
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
                rendered_text_call_count() > 0);
    ASSERT_TRUE("segmented text adds a blend enable",
                gl_stub_counts[GL_STUB_glEnable] > base_enable);
    ASSERT_TRUE("segmented text adds a blend func",
                gl_stub_counts[GL_STUB_glBlendFunc] > base_blend_func);
}

/* ui_text_panel_match_paren: the char "in front of" the caret (s[cursor])
 * drives the pair. Covers both directions, nesting, the no-paren case,
 * unbalanced input, and the caret-at-end boundary. */
static void test_match_paren_pairs(void) {
    const char *s = "glVertex3f(cos(t), 0, 0)";
    int self = -1, match = -1;
    int self2 = -1, match2 = -1;

    printf("--- match paren ---\n");

    /* Caret in front of the outer '(' at index 10. */
    ASSERT_INT_EQ("outer open is a paren", 1,
                  ui_text_panel_match_paren(s, (int)strlen(s), 10,
                                            &self, &match));
    ASSERT_INT_EQ("outer open self idx", self, 10);
    ASSERT_INT_EQ("outer open matches final ')'", match, 23);

    /* Caret in front of the final ')' at index 23 scans backward. */
    ASSERT_INT_EQ("outer close is a paren", 1,
                  ui_text_panel_match_paren(s, (int)strlen(s), 23,
                                            &self2, &match2));
    ASSERT_INT_EQ("outer close self idx", self2, 23);
    ASSERT_INT_EQ("outer close matches outer '('", match2, 10);

    /* Nested: the inner '(' at index 14 pairs with the ')' at 16. */
    ASSERT_INT_EQ("inner open is a paren", 1,
                  ui_text_panel_match_paren(s, (int)strlen(s), 14,
                                            &self, &match));
    ASSERT_INT_EQ("inner open matches inner ')'", match, 16);

    /* Non-paren char in front of caret: no pair. */
    ASSERT_INT_EQ("letter is not a paren", 0,
                  ui_text_panel_match_paren(s, (int)strlen(s), 0,
                                            &self, &match));

    /* Caret at end of string (cursor == len): nothing in front. */
    ASSERT_INT_EQ("caret at end has no char in front", 0,
                  ui_text_panel_match_paren(s, (int)strlen(s),
                                            (int)strlen(s), &self, &match));

    /* Unbalanced: a lone '(' has no partner. */
    ASSERT_INT_EQ("unbalanced open has no match", 0,
                  ui_text_panel_match_paren("foo(", 4, 3, &self, &match));
    ASSERT_INT_EQ("unbalanced close has no match", 0,
                  ui_text_panel_match_paren(")", 1, 0, &self, &match));

    /* Curly braces match independently of parens. In "if(t) { f(1) }"
     * the '{' at 6 pairs with the '}' at 13, ignoring the nested "(1)". */
    {
        const char *b = "if(t) { f(1) }";
        int len = (int)strlen(b);
        int bs = -1, bm = -1;

        ASSERT_INT_EQ("open brace is a bracket", 1,
                      ui_text_panel_match_paren(b, len, 6, &bs, &bm));
        ASSERT_INT_EQ("open brace self idx", bs, 6);
        ASSERT_INT_EQ("open brace matches close brace", bm, 13);

        bs = bm = -1;
        ASSERT_INT_EQ("close brace is a bracket", 1,
                      ui_text_panel_match_paren(b, len, 13, &bs, &bm));
        ASSERT_INT_EQ("close brace self idx", bs, 13);
        ASSERT_INT_EQ("close brace matches open brace", bm, 6);

        /* Caret on the '(' at 9 still pairs with ')' at 11, not a brace. */
        bs = bm = -1;
        ASSERT_INT_EQ("nested paren still matches its paren", 1,
                      ui_text_panel_match_paren(b, len, 9, &bs, &bm));
        ASSERT_INT_EQ("nested paren matches close paren", bm, 11);
    }

    /* Nested braces pick the balanced partner. In "{ a { b } c }" the
     * outer '{' at 0 matches '}' at 12, the inner '{' at 4 matches '}' at 8. */
    {
        const char *n = "{ a { b } c }";
        int len = (int)strlen(n);
        int bs = -1, bm = -1;

        ASSERT_INT_EQ("outer brace matches outer close", 1,
                      ui_text_panel_match_paren(n, len, 0, &bs, &bm));
        ASSERT_INT_EQ("outer brace close idx", bm, 12);

        bs = bm = -1;
        ASSERT_INT_EQ("inner brace matches inner close", 1,
                      ui_text_panel_match_paren(n, len, 4, &bs, &bm));
        ASSERT_INT_EQ("inner brace close idx", bm, 8);
    }

    /* Unbalanced brace has no partner. */
    ASSERT_INT_EQ("unbalanced open brace has no match", 0,
                  ui_text_panel_match_paren("foo{", 4, 3, &self, &match));
    ASSERT_INT_EQ("unbalanced close brace has no match", 0,
                  ui_text_panel_match_paren("}", 1, 0, &self, &match));
}

/* ui_text_panel_match_bracket_multiline: the caret's '{' / '}' on the
 * active input row pairs with its partner brace, possibly on another row
 * (the if / for case). Chrome / non-code rows are skipped; parens are not
 * matched across rows. self_row is the INPUT row, whose text comes from
 * snap.input.input. */
static void test_match_bracket_multiline(void) {
    int mr = -1, mc = -1;

    printf("--- match bracket multiline ---\n");

    /* Caret on the open brace of a for-header; close brace two rows down. */
    {
        UiTextPanelRow rows[3] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
            { .text = "glVertex3f(i, 0, 0)", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "}", .kind = UI_TEXT_PANEL_ROW_TEXT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 3, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "for(i, 0, 5) {";   /* '{' at index 13 */
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 13;
        snap.input.anchor = 13;

        mr = mc = -1;
        ASSERT_INT_EQ("open brace finds a partner", 1,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 13,
                                                            &mr, &mc));
        ASSERT_INT_EQ("open brace match row", mr, 2);
        ASSERT_INT_EQ("open brace match char", mc, 0);
    }

    /* Caret on the close brace; open brace two rows up. */
    {
        UiTextPanelRow rows[3] = {
            { .text = "for(i, 0, 5) {", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "glVertex3f(i, 0, 0)", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 3, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "}";
        snap.input.input_len = 1;
        snap.input.cursor = 0;
        snap.input.anchor = 0;

        mr = mc = -1;
        ASSERT_INT_EQ("close brace finds a partner", 1,
                      ui_text_panel_match_bracket_multiline(&snap, 2, 0,
                                                            &mr, &mc));
        ASSERT_INT_EQ("close brace match row", mr, 0);
        ASSERT_INT_EQ("close brace match char", mc, 13);
    }

    /* Nesting: the outer open brace skips the inner pair and lands on the
     * outer close. */
    {
        UiTextPanelRow rows[5] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
            { .text = "for(j, 0, 5) {", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "glVertex3f(i, j, 0)", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "}", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "}", .kind = UI_TEXT_PANEL_ROW_TEXT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 5, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "for(i, 0, 5) {";
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 13;
        snap.input.anchor = 13;

        mr = mc = -1;
        ASSERT_INT_EQ("outer open matches outer close", 1,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 13,
                                                            &mr, &mc));
        ASSERT_INT_EQ("outer open match row (skips inner)", mr, 4);
        ASSERT_INT_EQ("outer open match char", mc, 0);
    }

    /* Chrome / non-code rows are skipped: a STATIC row's stray '}' must not
     * steal the match from the real close brace below it. */
    {
        UiTextPanelRow rows[4] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
            { .text = "// chrome }", .kind = UI_TEXT_PANEL_ROW_STATIC },
            { .text = "glColor3f(1, 0, 0)", .kind = UI_TEXT_PANEL_ROW_TEXT },
            { .text = "}", .kind = UI_TEXT_PANEL_ROW_TEXT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 4, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "if(t > 0) {";    /* '{' at index 10 */
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 10;
        snap.input.anchor = 10;

        mr = mc = -1;
        ASSERT_INT_EQ("brace match skips chrome row", 1,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 10,
                                                            &mr, &mc));
        ASSERT_INT_EQ("brace match lands on real close row", mr, 3);
        ASSERT_INT_EQ("brace match char", mc, 0);
    }

    /* Same-row inline body resolves through the multiline path too. */
    {
        UiTextPanelRow rows[1] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 1, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "x { y } z";       /* '{' at 2, '}' at 6 */
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 2;
        snap.input.anchor = 2;

        mr = mc = -1;
        ASSERT_INT_EQ("same-row brace pairs on its own row", 1,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 2,
                                                            &mr, &mc));
        ASSERT_INT_EQ("same-row match row", mr, 0);
        ASSERT_INT_EQ("same-row match char", mc, 6);
    }

    /* Unbalanced (open brace, no close anywhere) → no match. */
    {
        UiTextPanelRow rows[2] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
            { .text = "glVertex3f(i, 0, 0)", .kind = UI_TEXT_PANEL_ROW_TEXT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 2, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "for(i, 0, 5) {";
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 13;
        snap.input.anchor = 13;

        ASSERT_INT_EQ("unbalanced open brace has no match", 0,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 13,
                                                            &mr, &mc));
    }

    /* A paren is not matched across rows (single-row only). */
    {
        UiTextPanelRow rows[1] = {
            { .text = "", .kind = UI_TEXT_PANEL_ROW_INPUT },
        };
        UiTextPanelSnapshot snap = make_snapshot(rows, 1, 0, 0, 800, 600, 0);
        snap.paren_match = 1;
        snap.input.input = "foo(bar)";        /* '(' at 3 */
        snap.input.input_len = (int)strlen(snap.input.input);
        snap.input.cursor = 3;
        snap.input.anchor = 3;

        ASSERT_INT_EQ("multiline matcher ignores parens", 0,
                      ui_text_panel_match_bracket_multiline(&snap, 0, 3,
                                                            &mr, &mc));
    }
}

/* ui_text_panel_enclosing_parens: the innermost pair surrounding the
 * caret, used to highlight the in-scope text. Indices for the literal s
 * (single-spaced): outer '(' at 0, B "(hello)" 2..8, C '(' at 10,
 * D "( world )" 20..28, C ')' at 30, outer ')' at 32. */
static void test_enclosing_parens_scope(void) {
    const char *s = "( (hello) ( there x ( world ) ) )";
    int len = (int)strlen(s);
    int open = -1, close = -1;

    printf("--- enclosing parens ---\n");

    /* Caret in front of 'x' (index 18) sits inside C; the innermost
     * enclosing pair is C [10, 30] — so the band spans that range while
     * "( (hello) " and the trailing " )" stay outside it. */
    ASSERT_INT_EQ("caret in C has an enclosing pair", 1,
                  ui_text_panel_enclosing_parens(s, len, 18, &open, &close));
    ASSERT_INT_EQ("C open index", open, 10);
    ASSERT_INT_EQ("C close index", close, 30);

    /* Caret inside the nested D ( world ) resolves to the innermost D,
     * not the outer C. */
    open = close = -1;
    ASSERT_INT_EQ("caret in D has an enclosing pair", 1,
                  ui_text_panel_enclosing_parens(s, len, 22, &open, &close));
    ASSERT_INT_EQ("D open index", open, 20);
    ASSERT_INT_EQ("D close index", close, 28);

    /* Caret at the very start is outside every pair. */
    ASSERT_INT_EQ("caret at start has no enclosing pair", 0,
                  ui_text_panel_enclosing_parens(s, len, 0, &open, &close));

    /* Unbalanced enclosing '(' has no close → no scope. */
    ASSERT_INT_EQ("unclosed enclosing pair yields none", 0,
                  ui_text_panel_enclosing_parens("foo(bar", 7, 5,
                                                 &open, &close));
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
    test_comment_rule_ligature_is_comment_line_only();
    test_comment_rule_ligature_applies_to_live_comment_input();
    test_match_paren_pairs();
    test_match_bracket_multiline();
    test_enclosing_parens_scope();

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_text_panel");
}

#else

int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}

#endif
