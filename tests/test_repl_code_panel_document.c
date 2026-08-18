#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/core/gl_2d.h"
#include "ui/core/text_panel.h"
#include "editor/input.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui/app/state.h"
#include "repl/export.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay_state.h"
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/panels.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(name, cond) \
    TEST_ASSERT_TRUE(&g_harness, name, cond)

/* Pixel step for the exhaustive hit-test probe below. Kept under the
 * smallest target it has to land on, so no probe steps over one. */
#define HIT_PROBE_STEP 4

static int code_panel_text_x(void) {
    int linenum_w = 4 * FONT_W;
    int idx_col_w = 6 * FONT_W;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    return idx_x + idx_col_w;
}

static void reset_doc_fixture(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 260);
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    glr_ctrl_sync_ui_chrome();
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);
    repl_state_refresh_workspace_header_lines();
}

static void build_doc(UiRenderSnapshot *snap, UiReplCodePanelLayout *layout) {
    int cp_w, cp_h;
    ui_layout_code_panel_rect(NULL, NULL, &cp_w, &cp_h);
    glr_ctrl_build_ui_snapshot(snap);
    ui_repl_code_panel_build_layout(snap, layout, cp_w, code_panel_text_x(), cp_h);
}

/* Mirror glr_ctrl_push_color_transformers() (which only runs inside a
 * full display frame, not glr_ctrl_build_ui_snapshot) so build_doc's
 * snapshot carries a color transformer for `line`. Without this the
 * snapshot has no transformers, no inline swatch is ever produced, and
 * the swatch row-builder path is untested. */
static void seed_color_transformer(int line, float r, float g, float b) {
    UiTransformer t = {
        .line_idx   = line,
        .char_start = -1,
        .char_end   = -1,
        .kind       = TRANSFORMER_COLOR_PICKER,
        .state.color = { .r = r, .g = g, .b = b, .a = 1.0f,
                         .has_alpha = 0 },
    };
    editor_state_transformers_clear();
    editor_state_transformers_append(&t);
}

/* Winning (last-drawn) color at char position `pos` among a row's color
 * segments. `has` is 0 when no segment covers the position (the renderer
 * would fall back to the row base color there). Segments are drawn in array
 * order, so the last one covering `pos` is what the user actually sees. */
typedef struct { float r, g, b; int has; } SegColor;

static SegColor winning_seg_color(const UiTextPanelColorSegment *segs, int n,
                                  int pos) {
    SegColor c = { 0.0f, 0.0f, 0.0f, 0 };
    for (int i = 0; i < n; i++) {
        if (pos >= segs[i].char_start &&
            pos < segs[i].char_start + segs[i].char_count) {
            c.r = segs[i].color.r;
            c.g = segs[i].color.g;
            c.b = segs[i].color.b;
            c.has = 1;
        }
    }
    return c;
}

static int seg_color_eq(SegColor a, SegColor b) {
    return a.has && b.has &&
           fabsf(a.r - b.r) < 1e-4f &&
           fabsf(a.g - b.g) < 1e-4f &&
           fabsf(a.b - b.b) < 1e-4f;
}

/* Assert a GL_*_BIT mask token is coloured UNIFORMLY across its whole span
 * (its per-bit hue covers the leading "GL_" too, not starting a few chars in)
 * and hand back that colour. `text` is the row's drawn text (input buffer for
 * the active edit row; buffer line for a committed row). */
static SegColor check_attrib_token_uniform(TestHarness *h, const char *label,
                                           const UiTextPanelColorSegment *segs,
                                           int n, const char *text,
                                           const char *token) {
    const char *hit = text ? strstr(text, token) : NULL;
    int start = hit ? (int)(hit - text) : -1;
    int len = (int)strlen(token);
    SegColor c0, c1;
    char name[160];

    snprintf(name, sizeof name, "%s: token '%s' present", label, token);
    TEST_ASSERT_TRUE(h, name, start >= 0);
    if (start < 0)
        return (SegColor){ 0.0f, 0.0f, 0.0f, 0 };

    c0 = winning_seg_color(segs, n, start);
    c1 = winning_seg_color(segs, n, start + len - 1);
    snprintf(name, sizeof name,
             "%s: '%s' is one uniform colour (per-bit hue covers GL_ prefix)",
             label, token);
    TEST_ASSERT_TRUE(h, name, seg_color_eq(c0, c1));
    return c0;
}

/* Regression: active glPushAttrib input derives its token spans from the live
 * input buffer, rather than translating committed-row coordinates across the
 * stripped indentation. It colors every supported bit token currently typed;
 * the committed-row path remains gated by the parsed mask. */
static void test_attrib_bit_tokens_align_on_edit_line(TestHarness *h) {
    UiRenderSnapshot snap;
    UiTextPanelColorSegment segs[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    const char *buffer_line;
    SegColor enable_c, color_c;
    int n;

    reset_doc_fixture();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glPopAttrib();");
    editor_feed_line("glPopMatrix();");

    /* --- Cursor ON the push line (active edit row, indent stripped). --- */
    editor_navigate_to_line(1);
    glr_ctrl_display_frame();          /* computes editor_state_highlights */
    glr_ctrl_build_ui_snapshot(&snap); /* snap->editor_highlights = live list */

    /* The push line is indented inside the glPushMatrix block while the input
     * buffer has no indent. This distinction no longer needs an offset. */
    buffer_line = editor_buffer_view_line(editor_buffer_view(), 1);
    TEST_ASSERT_TRUE(h, "push line is indented in the buffer",
                     buffer_line && (buffer_line[0] == ' ' ||
                                     buffer_line[0] == '\t'));
    TEST_ASSERT_TRUE(h, "edit-line input buffer has indent stripped",
                     snap.editor_input.input &&
                     snap.editor_input.input[0] == 'g');

    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 1, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    TEST_ASSERT_TRUE(h, "edit-line push row exists", n >= 0);

    enable_c = check_attrib_token_uniform(h, "edit line", segs, n,
                                          snap.editor_input.input,
                                          "GL_ENABLE_BIT");
    color_c = check_attrib_token_uniform(h, "edit line", segs, n,
                                         snap.editor_input.input,
                                         "GL_COLOR_BUFFER_BIT");
    TEST_ASSERT_TRUE(h,
                     "edit line: the two mask tokens carry distinct per-bit hues",
                     !seg_color_eq(enable_c, color_c));
    TEST_ASSERT_INT(h, "edit row has only its two per-bit color segments", n, 2);

    /* --- Cursor on glPopAttrib: the push line is now a committed row whose
     * display text still carries the indent. Its parsed-mask gate excludes
     * comment text and any bits not in the committed command. --- */
    editor_navigate_to_line(2);
    glr_ctrl_display_frame();
    glr_ctrl_build_ui_snapshot(&snap);

    buffer_line = editor_buffer_view_line(editor_buffer_view(), 1);
    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 1, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    TEST_ASSERT_TRUE(h, "committed push row exists", n >= 0);

    enable_c = check_attrib_token_uniform(h, "committed row", segs, n,
                                          buffer_line, "GL_ENABLE_BIT");
    color_c = check_attrib_token_uniform(h, "committed row", segs, n,
                                         buffer_line, "GL_COLOR_BUFFER_BIT");
    TEST_ASSERT_TRUE(h,
                     "committed row: the two mask tokens carry distinct per-bit hues",
                     !seg_color_eq(enable_c, color_c));

    /* Do not commit this edit: GL_FOG_BIT was not in the parsed source mask.
     * It should still be colored because the active row reads the live text
     * it draws, proving there is no stale committed-range coupling. */
    editor_navigate_to_line(1);
    glr_ctrl_display_frame();
    editor_input_set_text("glPushAttrib(GL_ENABLE_BIT | GL_FOG_BIT);");
    glr_ctrl_build_ui_snapshot(&snap);
    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 1, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    enable_c = check_attrib_token_uniform(h, "edited input", segs, n,
                                          snap.editor_input.input,
                                          "GL_ENABLE_BIT");
    color_c = check_attrib_token_uniform(h, "edited input", segs, n,
                                         snap.editor_input.input,
                                         "GL_FOG_BIT");
    TEST_ASSERT_TRUE(h,
                     "edited input: textual tokens carry distinct per-bit hues",
                     !seg_color_eq(enable_c, color_c));
    TEST_ASSERT_INT(h, "edited input has only its two per-bit color segments", n, 2);

    /* The narrow renderer path must not reintroduce syntax segments on every
     * active edit row: a matrix command keeps the legacy flat input wash. */
    editor_navigate_to_line(0);
    glr_ctrl_display_frame();
    glr_ctrl_build_ui_snapshot(&snap);
    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 0, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    TEST_ASSERT_INT(h, "non-push edit row has no color segments", n, 0);

    /* A newly inserted line has no committed command to gate on. It still
     * gets live per-bit feedback when its typed text is glPushAttrib(...). */
    editor_insert_mode_set(1);
    editor_input_set_text("glPushAttrib(GL_POINT_BIT | GL_FOG_BIT);");
    glr_ctrl_build_ui_snapshot(&snap);
    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, -1, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    enable_c = check_attrib_token_uniform(h, "new input", segs, n,
                                          snap.editor_input.input,
                                          "GL_POINT_BIT");
    color_c = check_attrib_token_uniform(h, "new input", segs, n,
                                         snap.editor_input.input,
                                         "GL_FOG_BIT");
    TEST_ASSERT_TRUE(h,
                     "new input: textual tokens carry distinct per-bit hues",
                     !seg_color_eq(enable_c, color_c));
    TEST_ASSERT_INT(h, "new input has only its two per-bit color segments", n, 2);
}

static void test_expanded_replay_color_comments_match_values(TestHarness *h) {
    UiRenderSnapshot snap;
    UiTextPanelColorSegment segs[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    const char *gl_text = "glColor3f(t, 0, 0); // glColor3f(1.2, 0.25, -0.1);";
    const char *glu_text = "gluColor(t, 0, 0, 1); // gluColor(0.1, 0.6, 0.9, 0.4);";
    const char *gl_annotation;
    const char *glu_annotation;
    SegColor gl_color;
    SegColor glu_color;
    int n;

    reset_doc_fixture();
    editor_feed_line("glColor3f(t, 0, 0);");
    editor_feed_line("gluColor(t, 0, 0, 1);");
    editor_state_line_overrides_clear();
    editor_state_line_overrides_append(0, gl_text);
    editor_state_line_overrides_append(1, glu_text);
    replay_state_mut()->active = 1;
    replay_state_mut()->expand_args = REPLAY_EXPAND_EXPANDED;
    glr_ctrl_build_ui_snapshot(&snap);

    gl_annotation = strstr(gl_text, "// glColor3f");
    glu_annotation = strstr(glu_text, "// gluColor");
    ASSERT_TRUE("glColor replay annotation present", gl_annotation != NULL);
    ASSERT_TRUE("gluColor replay annotation present", glu_annotation != NULL);

    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 0, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    gl_color = winning_seg_color(segs, n, (int)(gl_annotation - gl_text));
    ASSERT_TRUE("glColor annotation uses evaluated red",
                gl_color.has && fabsf(gl_color.r - 1.0f) < 1e-4f);
    ASSERT_TRUE("glColor annotation uses evaluated green",
                gl_color.has && fabsf(gl_color.g - 0.25f) < 1e-4f);
    ASSERT_TRUE("glColor annotation clamps evaluated blue",
                gl_color.has && fabsf(gl_color.b) < 1e-4f);

    n = ui_repl_code_panel_row_color_segments_for_test(
            &snap, 1, segs, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    glu_color = winning_seg_color(segs, n, (int)(glu_annotation - glu_text));
    ASSERT_TRUE("gluColor annotation uses evaluated RGB",
                glu_color.has && fabsf(glu_color.r - 0.1f) < 1e-4f &&
                fabsf(glu_color.g - 0.6f) < 1e-4f &&
                fabsf(glu_color.b - 0.9f) < 1e-4f);
}


/* Effect states the Config row cycles through, derived from the enum
 * rather than pinned to a literal so a new mode can't silently make the
 * wrap assertions below vacuous. */
#define ACCUM_PASS_STATES 9
#define VERTEX_LABEL_STATES (OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE + 1)
#define OVERLAY_SCOPE_STATES (OVERLAY_SCOPE_SINGLE_POLYGON + 1)

/* Scan the statusbar row for the first x owning `kind`, or -1. */
static int statusbar_hit_x(const UiRenderSnapshot *snap, int status_my,
                           int kind) {
    int cp_x, cp_w;
    ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    for (int mx = cp_x; mx < cp_x + cp_w; mx++)
        if (ui_repl_code_panel_hit_test(snap, mx, status_my).kind == kind)
            return mx;
    return -1;
}

/* Statusbar interactive readouts: AA status, Vertex labels, Overlay scope.
 * They sit AFTER the cursor cost readout in the left cluster, and each is
 * a click target - left advances the state, right steps it back.
 * Both directions go through the real dispatch
 * (glr_ctrl_router_handle_code_panel_hit / glr_ctrl_mouse), not the action
 * helper directly, so missing switch arms fail here. */
static void test_statusbar_readouts(TestHarness *h) {
    UiRenderSnapshot snap;
    UiReplCodePanelLayout layout;
    int cp_y, status_my;
    int aa_x_plain, aa_x_with_cost, aa_y;
    int vlabel_x_plain, vlabel_x_with_cost;
    int scope_x_plain, scope_x_with_cost;
    int poly_x_plain, poly_x_with_cost;
    int val_before;

    printf("Testing statusbar readouts position + clicks (AA, Vertex Labels, Overlay Scope, Poly Highlight)...\n");

    reset_doc_fixture();
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP;
    ui_state_viewport_set_size(1600, 300);   /* wide -> whole cluster fits */
    glr_ctrl_sync_ui_chrome();
    editor_feed_line("glClearColor(0, 0, 0, 1);");   /* line 0: plain */
    editor_feed_line("for(i, 0, 8) {");               /* line 1: scope */
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");

    glr_state_render_mut()->use_accum = 1;
    glr_config_set(GLR_CONFIG_ACCUM_EFFECT, RENDER3D_ACCUM_EFFECT_AA);
    glr_config_set(GLR_CONFIG_ACCUM_PASSES, 4); /* 8 passes */
    glr_config_set(GLR_CONFIG_VERTEX_LABELS, OVERLAY_VERTEX_LABEL_INDEX);
    glr_config_set(GLR_CONFIG_OVERLAY_SCOPE, OVERLAY_SCOPE_LAST_INSTANCE);
    glr_config_set(GLR_CONFIG_POLY_HIGHLIGHT, POLY_HIGHLIGHT_ON);

    ui_layout_code_panel_rect(NULL, &cp_y, NULL, NULL);
    status_my = ui_state_viewport().window_h - (cp_y + STATUSBAR_H / 2);
    aa_y = cp_y + STATUSBAR_H / 2;

    /* Cursor on a plain top-level line (cost 1): no cost readout. */
    editor_navigate_to_line(0);
    build_doc(&snap, &layout);
    TEST_ASSERT_TRUE(h, "cursor on a plain line shows no cost readout",
                     snap.cursor_cost_label[0] == '\0');
    aa_x_plain = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_AA_STATUS);
    vlabel_x_plain = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_VERTEX_LABELS_STATUS);
    scope_x_plain = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_OVERLAY_SCOPE_STATUS);
    poly_x_plain = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_POLY_HIGHLIGHT_STATUS);
    TEST_ASSERT_TRUE(h, "AA readout is hit-testable", aa_x_plain >= 0);
    TEST_ASSERT_TRUE(h, "Vertex Labels readout is hit-testable", vlabel_x_plain >= 0);
    TEST_ASSERT_TRUE(h, "Overlay Scope readout is hit-testable", scope_x_plain >= 0);
    TEST_ASSERT_TRUE(h, "Poly Highlight readout is hit-testable", poly_x_plain >= 0);
    TEST_ASSERT_TRUE(h, "readout ordering: AA < VLabels < Scope < Poly",
                     aa_x_plain < vlabel_x_plain &&
                     vlabel_x_plain < scope_x_plain &&
                     scope_x_plain < poly_x_plain);

    /* Cursor on the for-loop head: the "scope cmds N" readout appears.
     * State readouts are centered in the panel, so they do NOT shift when
     * cursor cost appears/disappears. */
    editor_navigate_to_line(1);
    build_doc(&snap, &layout);
    TEST_ASSERT_TRUE(h, "cursor on the loop head shows a cost readout",
                     snap.cursor_cost_label[0] != '\0');
    aa_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_AA_STATUS);
    vlabel_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_VERTEX_LABELS_STATUS);
    scope_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_OVERLAY_SCOPE_STATUS);
    poly_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_POLY_HIGHLIGHT_STATUS);
    TEST_ASSERT_TRUE(h, "cost readout does not shift centered AA readout", aa_x_with_cost == aa_x_plain);
    TEST_ASSERT_TRUE(h, "cost readout does not shift centered VLabels readout", vlabel_x_with_cost == vlabel_x_plain);
    TEST_ASSERT_TRUE(h, "cost readout does not shift centered Scope readout", scope_x_with_cost == scope_x_plain);
    TEST_ASSERT_TRUE(h, "cost readout does not shift centered Poly Highlight readout", poly_x_with_cost == poly_x_plain);

    /* Left click AA -> advances accum passes, through the dispatch switch. */
    val_before = glr_config_get(GLR_CONFIG_ACCUM_PASSES);
    {
        UiHit ah = ui_hit_none();
        ah.kind = UI_HIT_CODE_AA_STATUS;
        glr_ctrl_router_handle_code_panel_hit(ah, aa_x_with_cost, aa_y);
    }
    TEST_ASSERT_TRUE(h, "left click advances accum passes",
                     glr_config_get(GLR_CONFIG_ACCUM_PASSES) ==
                         (val_before + 1) % ACCUM_PASS_STATES);

    /* Right press AA -> decrements accum passes, through the real mouse callback. */
    build_doc(&snap, &layout);
    aa_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_AA_STATUS);
    val_before = glr_config_get(GLR_CONFIG_ACCUM_PASSES);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, aa_x_with_cost, status_my);
    TEST_ASSERT_TRUE(h, "right click decrements accum passes",
                     glr_config_get(GLR_CONFIG_ACCUM_PASSES) ==
                         (val_before - 1 + ACCUM_PASS_STATES) % ACCUM_PASS_STATES);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, aa_x_with_cost, status_my);

    /* Left click Vertex Labels -> next mode, through dispatch switch. */
    build_doc(&snap, &layout);
    vlabel_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_VERTEX_LABELS_STATUS);
    val_before = glr_config_get(GLR_CONFIG_VERTEX_LABELS);
    {
        UiHit vh = ui_hit_none();
        vh.kind = UI_HIT_CODE_VERTEX_LABELS_STATUS;
        glr_ctrl_router_handle_code_panel_hit(vh, vlabel_x_with_cost, aa_y);
    }
    TEST_ASSERT_TRUE(h, "left click advances vertex labels",
                     glr_config_get(GLR_CONFIG_VERTEX_LABELS) ==
                         (val_before + 1) % VERTEX_LABEL_STATES);

    /* Right press Vertex Labels -> previous mode, re-probing hit x. */
    build_doc(&snap, &layout);
    vlabel_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_VERTEX_LABELS_STATUS);
    val_before = glr_config_get(GLR_CONFIG_VERTEX_LABELS);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, vlabel_x_with_cost, status_my);
    TEST_ASSERT_TRUE(h, "right click decrements vertex labels",
                     glr_config_get(GLR_CONFIG_VERTEX_LABELS) ==
                         (val_before - 1 + VERTEX_LABEL_STATES) % VERTEX_LABEL_STATES);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, vlabel_x_with_cost, status_my);

    /* Left click Overlay Scope -> next mode, through dispatch switch. */
    build_doc(&snap, &layout);
    scope_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_OVERLAY_SCOPE_STATUS);
    val_before = glr_config_get(GLR_CONFIG_OVERLAY_SCOPE);
    {
        UiHit sh_hit = ui_hit_none();
        sh_hit.kind = UI_HIT_CODE_OVERLAY_SCOPE_STATUS;
        glr_ctrl_router_handle_code_panel_hit(sh_hit, scope_x_with_cost, aa_y);
    }
    TEST_ASSERT_TRUE(h, "left click advances overlay scope",
                     glr_config_get(GLR_CONFIG_OVERLAY_SCOPE) ==
                         (val_before + 1) % OVERLAY_SCOPE_STATES);

    /* Right press Overlay Scope -> previous mode, re-probing hit x. */
    build_doc(&snap, &layout);
    scope_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_OVERLAY_SCOPE_STATUS);
    val_before = glr_config_get(GLR_CONFIG_OVERLAY_SCOPE);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, scope_x_with_cost, status_my);
    TEST_ASSERT_TRUE(h, "right click decrements overlay scope",
                     glr_config_get(GLR_CONFIG_OVERLAY_SCOPE) ==
                         (val_before - 1 + OVERLAY_SCOPE_STATES) % OVERLAY_SCOPE_STATES);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, scope_x_with_cost, status_my);

    /* Left click Polygon Highlight -> next mode, through dispatch switch. */
    build_doc(&snap, &layout);
    poly_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_POLY_HIGHLIGHT_STATUS);
    val_before = glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT);
    {
        UiHit ph_hit = ui_hit_none();
        ph_hit.kind = UI_HIT_CODE_POLY_HIGHLIGHT_STATUS;
        glr_ctrl_router_handle_code_panel_hit(ph_hit, poly_x_with_cost, aa_y);
    }
    TEST_ASSERT_TRUE(h, "left click advances polygon highlight",
                     glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT) ==
                         (val_before + 1) % POLY_HIGHLIGHT_COUNT);

    /* Right press Polygon Highlight -> previous mode, re-probing hit x. */
    build_doc(&snap, &layout);
    poly_x_with_cost = statusbar_hit_x(&snap, status_my, UI_HIT_CODE_POLY_HIGHLIGHT_STATUS);
    val_before = glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, poly_x_with_cost, status_my);
    TEST_ASSERT_TRUE(h, "right click decrements polygon highlight",
                     glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT) ==
                         (val_before - 1 + POLY_HIGHLIGHT_COUNT) % POLY_HIGHLIGHT_COUNT);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, poly_x_with_cost, status_my);

    /* Constrained panel width (800x300 viewport in left layout -> 360px panel):
     * center state configs are culled before right editor buttons. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_viewport_set_size(800, 300);
    glr_ctrl_sync_ui_chrome();
    ui_layout_code_panel_rect(NULL, &cp_y, NULL, NULL);
    status_my = ui_state_viewport().window_h - (cp_y + STATUSBAR_H / 2);
    build_doc(&snap, &layout);
    TEST_ASSERT_TRUE(h, "center AA readout culled on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_AA_STATUS) == -1);
    TEST_ASSERT_TRUE(h, "center VLabels readout culled on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_VERTEX_LABELS_STATUS) == -1);
    TEST_ASSERT_TRUE(h, "center Scope readout culled on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_OVERLAY_SCOPE_STATUS) == -1);
    TEST_ASSERT_TRUE(h, "center Poly Highlight readout culled on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_POLY_HIGHLIGHT_STATUS) == -1);
    TEST_ASSERT_TRUE(h, "right help button remains visible on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_HELP_TOGGLE) >= 0);
    TEST_ASSERT_TRUE(h, "right focus button remains visible on narrow 360px panel",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_FOCUS_TOGGLE) >= 0);

    /* On plain line (no cost): editor chips extend further left (trash, copy, etc.). */
    editor_navigate_to_line(0);
    build_doc(&snap, &layout);
    TEST_ASSERT_TRUE(h, "right trash button visible on plain line at 360px width",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_CLEAR_ALL) >= 0);
    TEST_ASSERT_TRUE(h, "right copy button visible on plain line at 360px width",
                     statusbar_hit_x(&snap, status_my, UI_HIT_CODE_COPY) >= 0);

    glr_config_set(GLR_CONFIG_ACCUM_EFFECT, CFG_DEFAULT_ACCUM_EFFECT);
    glr_config_set(GLR_CONFIG_ACCUM_PASSES, CFG_DEFAULT_ACCUM_PASSES);
    glr_config_set(GLR_CONFIG_VERTEX_LABELS, CFG_DEFAULT_VERTEX_LABELS);
    glr_config_set(GLR_CONFIG_OVERLAY_SCOPE, CFG_DEFAULT_OVERLAY_SCOPE);
    glr_config_set(GLR_CONFIG_POLY_HIGHLIGHT, CFG_DEFAULT_HIGHLIGHT_POLY);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_viewport_set_size(1600, 300);
    glr_ctrl_sync_ui_chrome();
}

int main(void) {
    UiRenderSnapshot snap;
    UiReplCodePanelLayout layout;
    int target = -99;
    int on_insert = -99;
    int row_offset = -99;

    test_expanded_replay_color_comments_match_values(&g_harness);

    reset_doc_fixture();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    build_doc(&snap, &layout);

    ASSERT_TRUE("document counts commands",
                layout.total_lines >= layout.header_rows + layout.footer_rows + 4);
    ASSERT_TRUE("visible lines nonzero", layout.visible_lines > 0);
    ASSERT_TRUE("first command has one row", layout.cmd_main_rows[0] == 1);

    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("target lookup succeeds",
                    ui_repl_code_panel_target_for_doc_line(
                        &snap, doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("target lookup command index", target == 1);
        ASSERT_TRUE("target lookup is source row", on_insert == 0);
        ASSERT_TRUE("target lookup row offset", row_offset == 0);
    }

    /* Open a virtual insert row below the first command. Cursor > 0 keeps
     * the "open an empty insert row for typing" behavior (cursor == 0 now
     * inserts a real blank line above instead). */
    editor_navigate_to_line(0);
    editor_cursor_pos_set(1);
    editor_handle_key('\r', 0, 0);
    build_doc(&snap, &layout);
    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("insert row lookup succeeds",
                    ui_repl_code_panel_target_for_doc_line(
                        &snap, doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("insert row reports virtual line", target == -1);
        ASSERT_TRUE("insert row flag", on_insert == 1);
    }

    reset_doc_fixture();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_navigate_to_line(2);
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(1);
    build_doc(&snap, &layout);
    glr_ctrl_apply_code_panel_follow_scroll(&layout);
    ASSERT_TRUE("follow line visible after apply",
                layout.follow_doc_line >= editor_scroll() &&
                layout.follow_doc_line < editor_scroll() + layout.visible_lines);

    /* "Code focus" hides the derived C boilerplate stanzas: header and
     * footer row counts collapse to 0, document rows stay, and the
     * row-count/emit gating stays consistent so doc-line mapping holds
     * (review findings P1/P2b). */
    {
        int base_header, base_footer;

        reset_doc_fixture();
        editor_feed_line("float r;");
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("glEnd();");

        glr_state_presentation_mut()->code_focus = 0;
        build_doc(&snap, &layout);
        base_header = layout.header_rows;
        base_footer = layout.footer_rows;
        ASSERT_TRUE("baseline has header chrome", base_header > 0);
        ASSERT_TRUE("baseline has footer chrome", base_footer > 0);

        {
            float state_rgb[3];
            float transform_rgb[3];
            int state_segments = 0;
            int transform_segments = 0;
            ASSERT_TRUE("generated state row style available",
                        ui_repl_code_panel_generated_row_style_for_test(
                            &snap, "glEnable(GL_MULTISAMPLE)", state_rgb,
                            &state_segments));
            ASSERT_TRUE("generated transform row style available",
                        ui_repl_code_panel_generated_row_style_for_test(
                            &snap, "glTranslatef(", transform_rgb,
                            &transform_segments));
            ASSERT_TRUE("generated rows retain syntax spans",
                        state_segments > 0 && transform_segments > 0);
            ASSERT_TRUE("generated categories retain distinct hues",
                        state_rgb[0] != transform_rgb[0] ||
                        state_rgb[1] != transform_rgb[1] ||
                        state_rgb[2] != transform_rgb[2]);
            ASSERT_TRUE("generated base colors stay visually subordinate",
                        state_rgb[0] < 0.75f && state_rgb[1] < 0.75f &&
                        state_rgb[2] < 0.75f &&
                        transform_rgb[0] < 0.75f && transform_rgb[1] < 0.75f &&
                        transform_rgb[2] < 0.75f);
        }

        (void)base_header;

        glr_state_presentation_mut()->code_focus = 1;
        build_doc(&snap, &layout);
        ASSERT_TRUE("focus hides header chrome", layout.header_rows == 0);
        ASSERT_TRUE("focus hides footer chrome", layout.footer_rows == 0);
        ASSERT_TRUE("focus keeps document rows",
                    layout.total_lines >= 4 && layout.cmd_main_rows[0] == 1);
        {
            /* Command 1 starts right after command 0, at doc line
             * header_rows(==0) + cmd_main_rows[0]. */
            int doc_line = layout.header_rows + layout.cmd_main_rows[0];
            ASSERT_TRUE("focus doc-line mapping succeeds",
                        ui_repl_code_panel_target_for_doc_line(
                            &snap, doc_line, &layout, &target, &on_insert,
                            &row_offset));
            ASSERT_TRUE("focus maps to command 1", target == 1);
            ASSERT_TRUE("focus maps to source row", on_insert == 0);
        }

        glr_state_presentation_mut()->code_focus = 0;
        build_doc(&snap, &layout);
        ASSERT_TRUE("toggling focus off restores header rows",
                    layout.header_rows == base_header);
        ASSERT_TRUE("toggling focus off restores footer rows",
                    layout.footer_rows == base_footer);
    }

    /* P1: glr_ctrl_toggle_code_focus() (shared by Ctrl+Shift+F and the
     * status-bar keycap click) flips code_focus, syncs chrome, and
     * requests follow-scroll. After the toggle the header rows collapse
     * from many to 0; assert the real toggle path keeps the active edit
     * row on screen, and that it actually flips the flag. */
    {
        reset_doc_fixture();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("glEnd();");
        editor_navigate_to_line(2);

        glr_state_presentation_mut()->code_focus = 0;
        editor_scroll_set(0);
        build_doc(&snap, &layout);

        glr_ctrl_toggle_code_focus();
        ASSERT_TRUE("toggle flips code_focus on",
                    glr_state_presentation().code_focus == 1);
        build_doc(&snap, &layout);
        glr_ctrl_apply_code_panel_follow_scroll(&layout);
        ASSERT_TRUE("follow keeps cursor visible after focus toggle",
                    layout.follow_doc_line >= editor_scroll() &&
                    layout.follow_doc_line < editor_scroll() + layout.visible_lines);

        glr_ctrl_toggle_code_focus();
        ASSERT_TRUE("toggle flips code_focus off",
                    glr_state_presentation().code_focus == 0);
    }

    /* Compact focus/help keycaps both fit at the default left layout
     * now that their persistent text labels live in hover tooltips.
     * This is the space-saving behavior that motivated the tooltip. */
    {
        int found_focus = 0;
        int found_help = 0;

        reset_doc_fixture();              /* 800x260, LEFT, 0.45 */
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        build_doc(&snap, &layout);

        for (int my = 259; my >= 0; my--) {
            for (int mx = 799; mx >= 0; mx--) {
                UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                if (hk.kind == UI_HIT_CODE_FOCUS_TOGGLE) found_focus = 1;
                else if (hk.kind == UI_HIT_HELP_TOGGLE)  found_help = 1;
            }
        }
        ASSERT_TRUE("compact focus keycap fits at narrow default width",
                    found_focus == 1);
        ASSERT_TRUE("help keycap still fits at narrow default width",
                    found_help == 1);
    }

    /* On a wide panel both keycaps render; a click on each routes
     * THROUGH glr_ctrl_router_handle_code_panel_hit (the real dispatch
     * switch, not the toggle helper directly) and flips the matching
     * session state. */
    {
        int fx = -1, fy = -1, hx = -1, hy = -1;
        int help_was;

        reset_doc_fixture();
        ui_state_viewport_set_size(1600, 300);   /* wide -> cluster fits */
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        build_doc(&snap, &layout);

        for (int my = 299; my >= 0 && (fx < 0 || hx < 0); my--) {
            for (int mx = 1599; mx >= 0; mx--) {
                UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                if (hk.kind == UI_HIT_CODE_FOCUS_TOGGLE && fx < 0) {
                    fx = mx; fy = my;
                } else if (hk.kind == UI_HIT_HELP_TOGGLE && hx < 0) {
                    hx = mx; hy = my;
                }
            }
        }
        ASSERT_TRUE("focus keycap hit-testable on wide panel", fx >= 0);
        ASSERT_TRUE("help keycap hit-testable on wide panel", hx >= 0);

        glr_state_presentation_mut()->code_focus = 0;
        glr_ctrl_sync_ui_chrome();
        {
            UiHit fh = ui_hit_none();
            fh.kind = UI_HIT_CODE_FOCUS_TOGGLE;
            glr_ctrl_router_handle_code_panel_hit(fh, fx, fy);
        }
        ASSERT_TRUE("focus click routes through dispatch -> toggles on",
                    glr_state_presentation().code_focus == 1);
        glr_ctrl_toggle_code_focus();    /* restore */

        help_was = ui_state_help().visible;
        {
            UiHit hh = ui_hit_none();
            hh.kind = UI_HIT_HELP_TOGGLE;
            glr_ctrl_router_handle_code_panel_hit(hh, hx, hy);
        }
        ASSERT_TRUE("help click routes through dispatch -> toggles overlay",
                    ui_state_help().visible == !help_was);
        glr_ctrl_toggle_help();          /* restore */
        ASSERT_TRUE("help overlay restored",
                    ui_state_help().visible == help_was);

        /* While help is open it is modal, but the "F1 help" keycap
         * stays clickable so a second click dismisses it: panel
         * hit-test passes the keycap pixel through as
         * UI_HIT_HELP_TOGGLE while every other pixel stays
         * UI_HIT_HELP_PANEL. */
        ui_state_help_mut()->visible = 1;
        build_doc(&snap, &layout);
        ASSERT_TRUE("help keycap stays clickable through modal",
                    ui_panels_hit_test(&snap, hx, hy, 0).kind
                        == UI_HIT_HELP_TOGGLE);
        ASSERT_TRUE("rest of screen stays modal under help",
                    ui_panels_hit_test(&snap, 20, hy, 0).kind
                        == UI_HIT_HELP_PANEL);
        ui_state_help_mut()->visible = 0;
    }

    /* Regression: the inline color swatch must stay visible AND
     * hittable whether or not the cursor sits on the glColor line, in
     * BOTH code-focus modes. Before the add_input_row fix the
     * edit-in-place input row never set right_action, so moving the
     * cursor onto a color line made the swatch (and its click target)
     * silently vanish; a draw-order-only fix looked plausible but did
     * nothing. code_focus is pinned explicitly so a CFG_DEFAULT_CODE_FOCUS
     * flip can't mask this. Scroll is set past the (focus-aware)
     * header so the color row is on screen in full mode too. */
    for (int cf = 0; cf <= 1; cf++) {
        const char *mode = cf ? " [focus]" : " [full]";

        for (int cursor_on = 0; cursor_on <= 1; cursor_on++) {
            const char *cur = cursor_on ? "/cursor-on" : "/cursor-off";
            char lbl[96];
            int cp_x, cp_w, win_h, found = 0, line = -1;

            reset_doc_fixture();
            glr_state_presentation_mut()->code_focus = cf;
            glr_ctrl_sync_ui_chrome();
            editor_feed_line("glBegin(GL_POINTS);");   /* cmd 0 */
            editor_feed_line("glColor3f(1, 0, 0);");   /* cmd 1 (color) */
            editor_feed_line("glEnd();");              /* cmd 2 */
            seed_color_transformer(1, 1.0f, 0.0f, 0.0f);

            /* cursor_on: edit-in-place input row vs plain command row. */
            editor_navigate_to_line(cursor_on ? 1 : 0);

            /* Build once for the focus-aware header_rows, scroll past
             * the chrome (0 in focus), rebuild so the color row is at
             * the top of the visible area in both modes. */
            build_doc(&snap, &layout);
            editor_scroll_set(layout.header_rows);
            build_doc(&snap, &layout);

            ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
            win_h = ui_state_viewport().window_h;
            /* Probe on a HIT_PROBE_STEP grid, not every pixel: the swatch
             * band is UI_TEXT_PANEL_RIGHT_ACTION_W (12) wide and a code row
             * is LINE_H (18) tall, so a step of 4 lands inside both. */
            for (int my = 0; my < win_h && !found; my += HIT_PROBE_STEP)
                for (int mx = cp_x + cp_w - 1;
                     mx >= cp_x + cp_w - CODE_MARGIN_X - 2 * FONT_W && !found;
                     mx -= HIT_PROBE_STEP) {
                    UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                    if (hk.kind == UI_HIT_INLINE_COLOR_SWATCH) {
                        found = 1;
                        line  = hk.line_idx;
                    }
                }
            snprintf(lbl, sizeof lbl, "swatch hittable%s%s", cur, mode);
            ASSERT_TRUE(lbl, found);
            snprintf(lbl, sizeof lbl,
                     "swatch targets the glColor line%s%s", cur, mode);
            ASSERT_TRUE(lbl, line == 1);
        }
    }

    test_attrib_bit_tokens_align_on_edit_line(&g_harness);
    test_statusbar_readouts(&g_harness);

    return test_harness_report(&g_harness, "repl_code_panel_document");
}
