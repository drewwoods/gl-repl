#define _DEFAULT_SOURCE  /* mkdtemp() */
#define STATUSBAR_H 22
#include "ui/core/gl_2d.h"
#include "editor/clipboard.h"
#include "ui/app/repl_code_panel.h"
#include "ui/core/text_layout.h"
#include "editor/commit.h"
#include "editor/help_session.h"
#include "editor/inline_rename.h"
#include "editor/input.h"
#include "editor/search.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "app/glr_actions.h"
#include "app/glr_camera.h"
#include "app/glr_config.h"
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h" /* CFG_DEFAULT_* */
#include "app/glr_state.h"
#include "keys.h"
#include "support/cpuprof.h"
#include <math.h>
#include "repl/example_loader.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/examples.h"     /* repl_example_count */
#include "repl/scenes.h"       /* repl_promote_example_if_needed */
#include "repl/util.h"         /* repl_format_fits / repl_copy_string_fits */
#include "repl/export.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "source_document.h"   /* source_document_view */
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "ui/app/layout.h" /* CODE_PANEL_LAYOUT_* */
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/app/panels.h"
#include "ui/app/state.h"
#include "ui/support/cpuprof.h" /* PROFILE_PANEL_* modes (legacy-slug test) */
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"

#define g_status     (ui_state_status_mut()->text)
#define g_scroll     (editor_state_scroll_mut()->scroll)
#define g_panel_frac (ui_state_code_panel_mut()->panel_frac)
#define g_ac_count   (editor_state_autocomplete_mut()->match_count)
#define g_ac_sel     (editor_state_autocomplete_mut()->selected_idx)
#define g_ac_ghost   (editor_state_autocomplete_mut()->ghost)
#define g_ac_hint    (editor_state_autocomplete_mut()->hint)
#define g_search_active    (editor_state_search_mut()->active)
#define g_search_query     (editor_state_search_mut()->query)
#define g_search_query_len (editor_state_search_mut()->query_len)
#define g_search_cursor_pos (editor_state_search_mut()->cursor_pos)
#define g_show_help          (ui_state_help_mut()->visible)
#define g_show_profile_panel (ui_state_profile_panel_mut()->mode)
#define g_scroll_follow_cursor (editor_state_scroll_mut()->scroll_follow_cursor)
#define refresh_workspace_header_lines repl_state_refresh_workspace_header_lines
#define parse_workspace_header_line    repl_state_parse_workspace_header_line

#include "support/repl_test_support.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
static int g_mock_modifiers = 0;

/* Live-state variable-panel rect (mirrors the pre-narrowing NULL-snapshot
 * path) so these tests can query the panel rect by declared-var count. */
static void vp_rect(int count, int *px, int *py, int *pw, int *ph) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    ui_variable_panel_rect(&v, px, py, pw, ph);
}

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

#define g_workspace_header_lines (repl_state_import_export().workspace_header_lines)
#define g_workspace_header_line_count (repl_state_import_export().workspace_header_line_count)
#define g_render_state_lines (repl_state_import_export().render_state_lines)
#define g_cam_lines (repl_state_import_export().cam_lines)

#define replay_active        (replay_state_mut()->active)
#define replay_state         (replay_state_mut()->state)
#define replay_pc            (replay_state_mut()->pc)
#define replay_mode          (replay_state_mut()->mode)
#define replay_src_line      (replay_state_mut()->src_line_idx)
#define replay_expand_args   (replay_state_mut()->expand_args)

#define ASSERT_DECL_OK(label, cond, err) do { \
    (void)(err); \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    ASSERT_DECL_OK("declare_predef_var x", repl_eval_declare_predef_var("x", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var y", repl_eval_declare_predef_var("y", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var z", repl_eval_declare_predef_var("z", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var i", repl_eval_declare_predef_var("i", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var j", repl_eval_declare_predef_var("j", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var k", repl_eval_declare_predef_var("k", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var n", repl_eval_declare_predef_var("n", err, sizeof(err)), err);
}

/* Frozen excerpt from the Teapot carousel example at the time the
 * clipboard-paste regression was found. Keep this local to the test so
 * later showcase/example edits do not accidentally delete the coverage:
 * the copied pair must include a loop-local `i` assignment followed by
 * a valid `glColor3f` that can otherwise paste alone. */
static const char *const k_carousel_copy_paste_fixture[] = {
    "static float spinRate = 0.06;",
    "static float bulbs = 28;",
    "static float glow;",
    "",
    "glEnable(GL_DEPTH_TEST);",
    "glPushMatrix();",
    "  glRotatef(t * spinRate * 360, 0, 1, 0);",
    "",
    "  // Deck: triangle-fan disc, rim swept by the bulb chase phase",
    "  // (i*TAU*4/bulbs ~= the bulbs' i*0.9 step, but closes the loop exactly)",
    "  glDisable(GL_LIGHTING);",
    "  glBegin(GL_TRIANGLE_FAN);",
    "    glColor3f(0.2, 0.12, 0.06);",
    "    glVertex3f(0, 0, 0);",
    "    for(i, 0, bulbs + 1) {",
    "      glow = 0.6 + 0.4 * sin(6*t + i*TAU*4/bulbs);",
    "      glColor3f(0.45, 0.07 + 0.31*glow, 0.16*glow);",
    "      glVertex3f(2.6*cos(i*TAU/bulbs), 0, 2.6*sin(i*TAU/bulbs));",
    "    }",
    "  glEnd();",
    "glPopMatrix();",
};

static void load_carousel_copy_paste_fixture(void) {
    ASSERT_TRUE("load frozen carousel paste fixture",
                feed_program_lines(
                    k_carousel_copy_paste_fixture,
                    (int)(sizeof(k_carousel_copy_paste_fixture) /
                          sizeof(k_carousel_copy_paste_fixture[0]))));
}

static int apply_code_panel_follow_for_test(int *out_follow_doc_line,
                                            int *out_visible_lines) {
    UiRenderSnapshot snap;

    glr_ctrl_build_ui_snapshot(&snap);
    return glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &snap, out_follow_doc_line, out_visible_lines);
}

static UiHit code_panel_hit_test_current_snapshot(int mx, int my,
                                                  int variable_count) {
    UiRenderSnapshot snap;

    glr_ctrl_build_ui_snapshot(&snap);
    return ui_panels_hit_test(&snap, mx, my, variable_count);
}

static int current_active_indent_chars(void) {
    UiRenderSnapshot snap;

    glr_ctrl_build_ui_snapshot(&snap);
    return snap.active_indent_chars;
}

#include "subsystems/variable_panel/variable_panel_state.h"
#include "subsystems/variable_panel/variable_panel_drag.h"

#define VAR_PANEL_PAD_INTERNAL   6
#define VAR_TITLE_H_INTERNAL    20

static void set_editor_input(const char *text) {
    EditorInputState *inp = editor_state_input_mut();
    strncpy(inp->input, text, MAX_INPUT_LEN - 1);
    inp->input[MAX_INPUT_LEN - 1] = '\0';
    inp->input_len = (int)strlen(inp->input);
    editor_cursor_pos_set(inp->input_len);
}

static int mock_get_modifiers(void) {
    return g_mock_modifiers;
}

/* Test wrapper: matches what glr_ctrl_apply_input_effects does in
 * production for the editor → controller effects the editor cannot
 * actualize itself. Currently the only such effect is the
 * hidden-code-panel restore signal (audit #8 hoisted the actual
 * write into src/app/glr_ctrl.c). Tests that exercise editor input
 * paths that may set the flag drain it through this helper so the
 * post-call state mirrors production. */
static void apply_editor_effects(EditorInputDispatchEffects fx) {
    if (fx.restore_hidden_code_panel)
        glr_ctrl_restore_hidden_code_panel();
}

static void assert_status_contains(const char *label, const char *needle) {
    ASSERT_TRUE(label, strstr(g_status, needle) != NULL);
}

static int cfg_row_for_key(GlrConfigKey key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].key == key)
            return i;
    }
    return -1;
}

static int test_code_panel_row_count_for_text(const char *text, int first_x,
                                              int panel_w) {
    CodeLayout layout =
        code_layout_make(panel_w, first_x, FONT_W, glr_state_presentation().wrap_at_comma);
    return code_layout_row_count_for_text(text, &layout);
}

static int code_panel_header_row_count(void) {
    int panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int rows = 0;

    /* Code focus hides every derived C-boilerplate stanza, so the
     * production header row count collapses to 0 (see
     * repl_code_panel_chrome_visible). Mirror that here, or the
     * mouse-y math is off by the full-chrome height in focus mode. */
    if (glr_state_presentation().code_focus)
        return 0;

    ui_layout_code_panel_rect(NULL, NULL, &panel_w, NULL);
    refresh_workspace_header_lines();
    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += test_code_panel_row_count_for_text(g_workspace_header_lines[i],
                                                   text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += test_code_panel_row_count_for_text(g_header_pre[i], text_x, panel_w);
    for (int i = 0; g_display_header[i]; i++)
        rows += test_code_panel_row_count_for_text(g_display_header[i], text_x, panel_w);
    rows += test_code_panel_row_count_for_text(REPL_CODE_PANEL_SCRATCH_DECL_LINE,
                                               text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += test_code_panel_row_count_for_text(g_render_state_lines[i],
                                                   text_x, panel_w);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        rows += test_code_panel_row_count_for_text(g_cam_lines[i], text_x, panel_w);
    {
        char line[MAX_LINE_LEN];
        int n = repl_export_lights_display_line_count();
        for (int pos_idx = 0; pos_idx < n; pos_idx++) {
            repl_export_lights_display_line(pos_idx, line, sizeof(line));
            rows += test_code_panel_row_count_for_text(line, text_x, panel_w);
        }
    }
    for (int i = 0; g_header_post[i]; i++)
        rows += test_code_panel_row_count_for_text(g_header_post[i], text_x, panel_w);
    return rows;
}

static int code_panel_mouse_y_for_cmd(int cmd_idx) {
    int cp_y, cp_h, panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = code_panel_header_row_count();

    ui_layout_code_panel_rect(NULL, &cp_y, &panel_w, &cp_h);
    for (int i = 0; i < cmd_idx && i < repl_state_document_count(); i++) {
        const char *line_text = editor_buffer_line(i);
        doc_line += test_code_panel_row_count_for_text(line_text ? line_text : "",
                                                       text_x, panel_w);
    }

    int vis = doc_line - g_scroll;
    int line_y_start = cp_y + cp_h - CODE_MARGIN_Y - 2 * LINE_H;
    int gl_y = line_y_start - vis * LINE_H + 1;
    return ui_state_viewport().window_h - gl_y;
}

static void assert_float_decl_rejected_atomic(const char *label,
                                              const char *src,
                                              const char *rejected_name,
                                              const char *status_part) {
    char detail[160];
    int old_num_cmds = repl_state_document_count();
    int old_num_predef_vars = g_num_predef_vars;

    set_editor_input(src);
    editor_state_edit_line_set(repl_state_document_count());
    editor_insert_mode_set(0);

    int result = editor_try_commit_float_decl();

    snprintf(detail, sizeof(detail), "%s: handler consumed input", label);
    ASSERT_INT(detail, result, 1);
    snprintf(detail, sizeof(detail), "%s: cmd count unchanged", label);
    ASSERT_INT(detail, repl_state_document_count(), old_num_cmds);
    snprintf(detail, sizeof(detail), "%s: var count unchanged", label);
    ASSERT_INT(detail, g_num_predef_vars, old_num_predef_vars);
    snprintf(detail, sizeof(detail), "%s: rejected name not registered", label);
    ASSERT_TRUE(detail, repl_eval_find_predef_var_idx(rejected_name) < 0);
    snprintf(detail, sizeof(detail), "%s: status", label);
    assert_status_contains(detail, status_part);
    ASSERT_TRUE("atomic failure preserves anchor",
                old_num_predef_vars <= 1 || repl_eval_find_predef_var_idx("anchor") >= 0);
}

int main() {
    repl_eval_init_predef_vars();
    editor_input_set_modifier_provider_for_test(mock_get_modifiers);
    /* Tests skip glr_ctrl_init_gl (no GLUT context); register the
     * comment-toggle prefix and the code-panel layout provider
     * explicitly so the matching production paths (Ctrl+/ toggle,
     * editor_input_code_panel_layout reads for hit-tests and the
     * hidden-panel auto-restore detection) work the same way they
     * do under glr_ctrl_init_gl. */
    editor_set_line_comment_prefix("// ");
    editor_input_set_code_panel_layout_provider(glr_ctrl_code_panel_layout_provider);
    printf("--- repl_editor tests ---\n");

    /* Commit chain ordering: float declarations must run before assignment
     * parsing, otherwise `float name` is misclassified as an assignment. */
    {
        glr_ctrl_reset_all();
        set_editor_input("float chain_order;");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = editor_try_commit_any();

        ASSERT_INT("commit chain float decl consumed", result, 1);
        ASSERT_INT("commit chain float decl inserted one cmd", repl_state_document_count(), 1);
        ASSERT_INT("commit chain float decl cmd type",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("commit chain float decl registered var",
                    repl_eval_find_predef_var_idx("chain_order") >= 0);
    }

    /* commit_current_input ordering invariant #1 (under Enter):
     * editor_try_commit_block_structs runs BEFORE the var-statement
     * chain so `}` on its own line closes the active block instead
     * of falling through to misread as a stray-`}` to var-statement.
     * Pin this by verifying block_structs accepts `}` when an open
     * for-loop exists. */
    {
        glr_ctrl_reset_all();
        set_editor_input("for(i, 0, 3) {");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        ASSERT_INT("for-loop opens block", editor_try_commit_block_structs(), 1);

        set_editor_input("}");
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(1);
        int closed = editor_try_commit_block_structs();
        ASSERT_INT("} on its own line accepted by block_structs", closed, 1);
        /* The block now has FOR_BEGIN + FOR_END. */
        ASSERT_TRUE("for-loop closed",
                    repl_state_document_count() >= 2 &&
                    repl_state_document_cmds_mut()[repl_state_document_count() - 1].type
                        == CMD_FOR_END);
    }

    /* commit_current_input ordering invariant #2 (within var_statements):
     * `editor_try_commit_var_statements_then_insert` is the overwrite-
     * Enter variant — it runs float_decl before assign_variable and
     * additionally flips into insert mode + clears input on success
     * (post-effects the canonical `_any` chain does not provide). */
    {
        glr_ctrl_reset_all();
        /* Pre-seed an existing line so overwrite mode has something to
         * sit on. */
        editor_feed_line("float seed;");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        set_editor_input("float pinned;");
        int consumed = editor_try_commit_var_statements_then_insert();
        ASSERT_INT("then_insert variant consumed", consumed, 1);
        ASSERT_TRUE("then_insert registered the new decl",
                    repl_eval_find_predef_var_idx("pinned") >= 0);
        ASSERT_INT("then_insert flipped to insert mode", editor_insert_mode(), 1);
        ASSERT_INT("then_insert cleared input buffer",
                   editor_state_input().input_len, 0);
        assert_status_contains("then_insert preserved decl status",
                       "declared pinned");

        editor_insert_mode_set(0);
        editor_state_edit_line_set(repl_state_document_count());
        set_editor_input("pinned = 3;");
        consumed = editor_try_commit_var_statements_then_insert();
        ASSERT_INT("then_insert assign consumed", consumed, 1);
        ASSERT_INT("then_insert assign stays insert mode", editor_insert_mode(), 1);
        ASSERT_INT("then_insert assign cleared input buffer",
               editor_state_input().input_len, 0);
        assert_status_contains("then_insert preserved assign status",
                   "pinned = 3");
    }

    /* commit_current_input lenient-block regression (main commit
     * d704d33 "editor: allow Enter to commit block structs from any
     * cursor position"). In overwrite mode at a non-last, non-block-
     * head line, Enter used to try editor_try_commit_block_structs()
     * only when the cursor already sat on a block-head command. On any
     * other line (assignment, GL call, ...) an if(...) {, for(...) {,
     * or funcN() { fell through to the GL parser, which rejected the
     * trailing `{` with "unexpected text after ')'". The fix moves the
     * block_structs attempt unconditionally ahead of the var-statement
     * chain and the GL parser, matching the ;-key path
     * (editor_try_commit_any). These cases drive the real Enter route
     * (editor_handle_key('\r', ...)) from a glVertex line — a
     * non-block-head, non-last cursor — and assert each block head
     * commits instead of erroring out. */
    {
        struct { const char *input; CmdType want; const char *label; } cases[] = {
            { "if(t < 1) {",   CMD_IF_BEGIN,  "if-block" },
            { "for(i, 0, 3) {", CMD_FOR_BEGIN, "for-loop" },
            { "func0() {",     CMD_FUNC_DEF,  "func-def" },
        };
        for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
            char detail[160];
            glr_ctrl_reset_all();
            declare_test_vars();
            /* Two plain GL lines so the cursor parks on a non-last,
             * non-block-head line. */
            editor_feed_line("glVertex3f(0,0,0)");
            editor_feed_line("glVertex3f(1,1,1)");
            editor_navigate_to_line(0);
            editor_insert_mode_set(0);
            /* Enter accepts an active autocomplete before committing;
             * clear it so we reach the commit path. */
            g_ac_count = 0;
            g_ac_sel = 0;
            g_ac_ghost[0] = '\0';
            g_ac_hint[0] = '\0';

            snprintf(detail, sizeof(detail), "%s: cursor is non-block-head", cases[c].label);
            ASSERT_INT(detail, repl_line_is_block_head(editor_state_edit_line()), 0);
            snprintf(detail, sizeof(detail), "%s: overwrite mode", cases[c].label);
            ASSERT_INT(detail, editor_insert_mode(), 0);
            snprintf(detail, sizeof(detail), "%s: cursor not on last line", cases[c].label);
            ASSERT_INT(detail, editor_state_edit_line() < repl_state_document_count(), 1);

            int before = repl_state_document_count();
            set_editor_input(cases[c].input);
            editor_handle_key('\r', 0, 0);

            snprintf(detail, sizeof(detail), "%s: Enter committed (cmd count grew)", cases[c].label);
            ASSERT_TRUE(detail, repl_state_document_count() > before);

            int found = 0;
            for (int i = 0; i < repl_state_document_count(); i++)
                if (repl_state_document_cmds_mut()[i].type == cases[c].want)
                    found = 1;
            snprintf(detail, sizeof(detail), "%s: block head command present", cases[c].label);
            ASSERT_TRUE(detail, found);

            snprintf(detail, sizeof(detail), "%s: no GL parse error", cases[c].label);
            ASSERT_TRUE(detail, strstr(g_status, "unexpected text") == NULL);
        }
    }

    /* 0. Code/scene panel geometry supports left, top, bottom, and hidden layouts */
    {
        int x, y, w, h;

        ui_state_viewport_set_size(1000, 800);
        g_panel_frac = 0.25f;

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        ui_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("left code x", x, 0);
        ASSERT_INT("left code y", y, 0);
        ASSERT_INT("left code w", w, 250);
        ASSERT_INT("left code h", h, 800);
        ui_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("left scene x", x, 250);
        ASSERT_INT("left scene y", y, 0);
        ASSERT_INT("left scene w", w, 750);
        ASSERT_INT("left scene h", h, 800);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP; glr_ctrl_sync_ui_chrome();
        ui_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("top code x", x, 0);
        ASSERT_INT("top code y", y, 600);
        ASSERT_INT("top code w", w, 1000);
        ASSERT_INT("top code h", h, 200);
        ui_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("top scene x", x, 0);
        ASSERT_INT("top scene y", y, 0);
        ASSERT_INT("top scene w", w, 1000);
        ASSERT_INT("top scene h", h, 600);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM; glr_ctrl_sync_ui_chrome();
        ui_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom code x", x, 0);
        ASSERT_INT("bottom code y", y, 0);
        ASSERT_INT("bottom code w", w, 1000);
        ASSERT_INT("bottom code h", h, 200);
        ui_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom scene x", x, 0);
        ASSERT_INT("bottom scene y", y, 200);
        ASSERT_INT("bottom scene w", w, 1000);
        ASSERT_INT("bottom scene h", h, 600);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        ui_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden code x", x, 0);
        ASSERT_INT("hidden code y", y, 0);
        ASSERT_INT("hidden code w", w, 0);
        ASSERT_INT("hidden code h", h, 0);
        ui_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden scene x", x, 0);
        ASSERT_INT("hidden scene y", y, 0);
        ASSERT_INT("hidden scene w", w, 1000);
        ASSERT_INT("hidden scene h", h, 800);

        ui_state_viewport_set_size(1200, 800);
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0b. Code panel config cycles Left -> Top -> Bottom -> Hidden and imports legacy top layout */
    {
        int row = cfg_row_for_key(GLR_CONFIG_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("code panel cfg row exists", row >= 0);
        if (row >= 0) {
            glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
            glr_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to top",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);
            glr_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to bottom",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
            g_ac_count = 2;
            g_ac_sel = 1;
            strcpy(g_ac_ghost, "glVertex3f");
            strcpy(g_ac_hint, "vertex");
            glr_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to hidden",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
            ASSERT_INT("hide clears autocomplete count", g_ac_count, 0);
            ASSERT_INT("hide clears autocomplete selection", g_ac_sel, 0);
            ASSERT_STR("hide clears autocomplete ghost", g_ac_ghost, "");
            ASSERT_STR("hide clears autocomplete hint", g_ac_hint, "");
            glr_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg wraps to left",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        }

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        ASSERT_INT("parse code_panel cfg",
                   parse_workspace_header_line("// @cfg code_panel = 2"), 1);
        repl_export_apply_pending_cfg();
        ASSERT_INT("parse code_panel bottom",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);

        ASSERT_INT("parse code_panel hidden cfg",
                   parse_workspace_header_line("// @cfg code_panel = 3"), 1);
        repl_export_apply_pending_cfg();
        ASSERT_INT("parse code_panel hidden",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);

        {
            int found_hidden_export = 0;
            refresh_workspace_header_lines();
            for (int i = 0; i < g_workspace_header_line_count; i++) {
                if (strcmp(g_workspace_header_lines[i],
                           "/* @cfg code_panel = 3 */") == 0)
                    found_hidden_export = 1;
            }
            ASSERT_TRUE("workspace header exports hidden code panel",
                        found_hidden_export);
        }

        ASSERT_INT("parse legacy top_code_panel cfg",
                   parse_workspace_header_line("// @cfg top_code_panel = 1"), 1);
        repl_export_apply_pending_cfg();
        ASSERT_INT("legacy top_code_panel maps to top",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);

        ASSERT_INT("parse legacy top_code_panel off cfg",
                   parse_workspace_header_line("// @cfg top_code_panel = 0"), 1);
        repl_export_apply_pending_cfg();
        ASSERT_INT("legacy top_code_panel off maps to left",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);

        /* "CPU profile" -> "Compute profile" rename: files saved before it
         * carry the old cpu_profile slug and must still drive the panel. */
        ASSERT_INT("parse legacy cpu_profile cfg",
                   parse_workspace_header_line("// @cfg cpu_profile = 2"), 1);
        repl_export_apply_pending_cfg();
        ASSERT_INT("legacy cpu_profile maps to compute_profile",
                   ui_state_profile_panel().mode, PROFILE_PANEL_DETAILS);
        ui_state_profile_panel_mut()->mode = PROFILE_PANEL_OFF;

        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0b2. Config/action module owns shortcut and menu row dispatch. */
    {
        glr_ctrl_reset_all();

        /* F2 cycles Grid theme; wireframe moved to the
         * plain Ctrl+G ascii shortcut. */
        int grid_before = glr_config_get(GLR_CONFIG_GRID_THEME);
        ASSERT_INT("config special shortcut consumed",
                   glr_cfg_handle_special_shortcut(GLUT_KEY_F2), 1);
        ASSERT_TRUE("config special shortcut (F2) cycles Grid theme",
                    glr_config_get(GLR_CONFIG_GRID_THEME) != grid_before);

        glr_state_presentation_mut()->wireframe = 0;
        ASSERT_INT("wireframe ascii shortcut consumed",
                   glr_cfg_handle_ascii_shortcut(KEY_CTRL_G), 1);
        ASSERT_INT("Ctrl+G toggles wireframe",
                   glr_state_presentation().wireframe, 1);

        glr_state_presentation_mut()->grid_major_idx = 0;
        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_SHIFT;
        ASSERT_INT("config ascii shortcut consumed",
                   glr_cfg_handle_ascii_shortcut(KEY_CTRL_G), 1);
        ASSERT_INT("config ascii shortcut cycles grid major",
                   glr_state_presentation().grid_major_idx, 1);
        g_mock_modifiers = saved_mods;

        int row = cfg_row_for_key(GLR_CONFIG_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("config menu action row exists", row >= 0);
        if (row >= 0) {
            /* Config items are no longer activated through
             * glr_action_menu_item_activate(GLR_MENU_CONFIG, …) — that
             * path is now a parent-row no-op (plan Step 5). The Config
             * flyout route invokes glr_cfg_cycle_row() on the absolute
             * g_cfg_items[] index; exercise that primitive directly. */
            glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
            ASSERT_INT("config parent-row activate is an inert no-op",
                       glr_action_menu_item_activate(GLR_MENU_CONFIG, row), 0);
            ASSERT_INT("parent-row activate does not cycle the item",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
            glr_cfg_cycle_row(row, 1);
            ASSERT_INT("config flyout primitive cycles code panel",
                       glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);
        }
    }

    /* 0c. Hidden code panel returns to the editor on ordinary input */
    {
        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        apply_editor_effects(editor_handle_key('v', 0, 0));
        ASSERT_INT("typing restores hidden code panel",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        ASSERT_STR("typing after restore still reaches input", editor_state_input().input, "v");

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        apply_editor_effects(editor_handle_key('`', 0, 0));
        ASSERT_INT("config shortcut restores hidden code panel",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        apply_editor_effects(editor_handle_key('`', 0, 0));

        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0c2. Keyboard mode routing keeps rename ahead of config, replay, and search. */
    {
        glr_ctrl_reset_all();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename route setup slot", slot >= 0);
        ASSERT_INT("rename route begin", editor_inline_rename_begin(slot), 1);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        editor_handle_key('`', 0, 0);
        ASSERT_INT("rename swallows config hidden restore",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename remains active after config key",
                   editor_inline_rename_active(), 1);

        editor_handle_key(KEY_CTRL_R, 0, 0);
        ASSERT_INT("rename swallows replay toggle", replay_active, 0);

        editor_handle_key(KEY_CTRL_F, 0, 0);
        ASSERT_INT("rename swallows search open", g_search_active, 0);

        editor_inline_rename_cancel();
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0c3. Search mode captures printable editing keys before text editing. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        set_editor_input("");

        editor_handle_key(KEY_CTRL_F, 0, 0);
        ASSERT_INT("search route opens", g_search_active, 1);

        editor_handle_key('v', 0, 0);
        ASSERT_STR("search route receives printable key", g_search_query, "v");
        ASSERT_TRUE("search route does not type into editor input",
                    strcmp(editor_state_input().input, "v") != 0);
        ASSERT_INT("search route does not commit", repl_state_document_count(), 1);

        editor_search_clear_all();
    }

    /* 0c4. Semicolon keyboard route still enters the commit handler chain. */
    {
        glr_ctrl_reset_all();
        set_editor_input("float keyboard_route");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("semicolon route committed one command", repl_state_document_count(), 1);
        ASSERT_INT("semicolon route used float declaration handler",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("semicolon route registered declared variable",
                    repl_eval_find_predef_var_idx("keyboard_route") >= 0);
    }

    /* 0c5. Special-key routing keeps rename ahead of replay/search/navigation. */
    {
        glr_ctrl_reset_all();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename special route setup slot", slot >= 0);
        ASSERT_INT("rename special route begin", editor_inline_rename_begin(slot), 1);

        set_editor_input("abc");
        editor_cursor_pos_set(2);
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        g_show_help = 0;
        replay_active = 1;
        replay_state = REPLAY_PAUSED;
        replay_pc = 0;
        g_search_active = 1;
        snprintf(g_search_query, MAX_INPUT_LEN, "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("rename special swallows hidden restore",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename special keeps editor cursor", editor_cursor_pos(), 2);
        ASSERT_INT("rename special keeps search cursor", g_search_cursor_pos, 2);
        ASSERT_INT("rename special keeps replay pc", replay_pc, 0);

        editor_handle_special(GLUT_KEY_F1, 0, 0);
        ASSERT_INT("rename special swallows help toggle", g_show_help, 0);
        ASSERT_INT("rename still active after special keys",
                   editor_inline_rename_active(), 1);

        editor_inline_rename_cancel();
        editor_search_clear_all();
        replay_active = 0;
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0c6. Search mode captures special arrows before editor/help navigation. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_navigate_to_line(0);
        set_editor_input("abc");
        editor_cursor_pos_set(2);
        g_show_help = 1;
        editor_help_session_set_tab(1);
        g_search_active = 1;
        snprintf(g_search_query, MAX_INPUT_LEN, "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        editor_handle_special(GLUT_KEY_LEFT, 0, 0);

        ASSERT_INT("search special moves search cursor", g_search_cursor_pos, 1);
        ASSERT_INT("search special leaves editor cursor", editor_cursor_pos(), 2);
        ASSERT_INT("search special leaves help tab", editor_help_session_tab_idx(), 1);

        editor_search_clear_all();
        g_show_help = 0;
    }

    /* 0c6b. A bare modifier special key (Ctrl/Shift/Alt/Cmd) is not edit
     * input: it must not re-arm scroll-follow-cursor (which would snap the
     * code panel back to the cursor and undo a wheel scroll), while a real
     * special key still does. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_navigate_to_line(0);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_CTRL_L, 0, 0);
        ASSERT_INT("bare Ctrl_L leaves scroll-follow off", g_scroll_follow_cursor, 0);
        editor_handle_special(GLUT_KEY_SHIFT_R, 0, 0);
        ASSERT_INT("bare Shift_R leaves scroll-follow off", g_scroll_follow_cursor, 0);
        editor_handle_special(GLUT_KEY_SUPER_L, 0, 0);
        ASSERT_INT("bare Cmd_L leaves scroll-follow off", g_scroll_follow_cursor, 0);

        /* A real navigation special key re-arms follow as before. */
        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("arrow key re-arms scroll-follow", g_scroll_follow_cursor, 1);
    }

    /* 0c7. F12 special route still cycles built-in examples. */
    {
        glr_ctrl_reset_all();
        if (repl_example_count() > 1) {
            repl_load_example(0);
            glr_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            ASSERT_INT("f12 special route advances example",
                    repl_state_scenes().active_example_idx, 1);
        }
    }

    /* 0d. Cramped variable-panel fallback still preserves scene status strip */
    {
        int x, y, w, h;
        ui_state_viewport_set_size(320, 80);
        g_panel_frac = 0.25f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        replay_active = 0;

        vp_rect(g_num_predef_vars, &x, &y, &w, &h);
        ASSERT_INT("cramped var panel clears status strip",
                   y, STATUSBAR_H + 4);

        ui_state_viewport_set_size(1200, 800);
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();
    }

    /* 0e. Camera control module owns scene drag and momentum behavior. */
    {
        glr_ctrl_reset_all();
        glr_camera_set_orbit(0.0f, 0.0f);
        glr_camera_set_distance(10.0f);
        glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 10, 10, 0);
        glr_camera_drag_motion(20, 14);
        ASSERT_TRUE("camera left drag yaws",
                    fabsf(glr_camera().ry - 5.0f) < 1e-6f);
        ASSERT_TRUE("camera left drag pitches",
                    fabsf(glr_camera().rx - 2.0f) < 1e-6f);
        glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_UP, 20, 14, 0);
        glr_camera_tick();
        ASSERT_TRUE("camera release keeps yaw momentum",
                    fabsf(glr_camera().ry - 7.5f) < 1e-6f);

        glr_camera_controls_reset();
        glr_camera_set_orbit(0.0f, 0.0f);
        glr_camera_set_pan(0.0f, 0.0f, 0.0f);
        glr_camera_set_distance(10.0f);
        glr_camera_mouse_event(GLUT_RIGHT_BUTTON, GLUT_DOWN, 0, 0,
                                GLUT_ACTIVE_SHIFT);
        glr_camera_drag_motion(0, 20);
        ASSERT_TRUE("camera shift-right drag pans y",
                    fabsf(glr_camera().ty - 1.0f) < 1e-6f);
        ASSERT_TRUE("camera y pan lights motion glow",
                    fabsf(glr_camera().motion_glow - 1.0f) < 1e-6f);

        glr_camera_controls_reset();
        glr_camera_set_control_mode(GLR_CAMERA_CONTROL_2D);
        glr_camera_set_orbit(0.0f, 0.0f);
        glr_camera_set_pan(0.0f, 0.0f, 0.0f);
        glr_camera_set_distance(10.0f);
        glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 0, 0, 0);
        glr_camera_drag_motion(20, 10);
        ASSERT_TRUE("2d left drag does not yaw",
                    fabsf(glr_camera().ry) < 1e-6f);
        ASSERT_TRUE("2d left drag pans x",
                    fabsf(glr_camera().tx - (-1.0f)) < 1e-6f);
        ASSERT_TRUE("2d left drag pans y",
                    fabsf(glr_camera().ty - 0.5f) < 1e-6f);

        glr_camera_controls_reset();
        glr_camera_set_distance(0.6f);
        glr_camera_mouse_event(GLUT_MIDDLE_BUTTON, GLUT_DOWN, 0, 0, 0);
        glr_camera_drag_motion(0, -100);
        ASSERT_TRUE("camera middle drag clamps near zoom",
                    fabsf(glr_camera().dist - 0.5f) < 1e-6f);
    }

    /* 1. Undo when nothing to undo. glr_ctrl_reset_all() calls
     * editor_undo_note_wholesale_replacement() which clears the rings
     * and bumps the generation, so we can rely on a clean undo state
     * here regardless of previous pushes. */
    {
        glr_ctrl_reset_all();
        editor_undo_pop_snapshot();
        /* Should survive without crashing; state unchanged */
        ASSERT_INT("undo-nothing: num_cmds still 0", repl_state_document_count(), 0);
    }

    /* 2. Redo when nothing to redo - same clean-undo invariant as #1. */
    {
        glr_ctrl_reset_all();
        editor_undo_do_redo();
        ASSERT_INT("redo-nothing: num_cmds still 0", repl_state_document_count(), 0);
    }

    /* 3. Undo/Redo basic */
    {
        float scratch = 0.0f;
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("A[0] = 3;");
        ASSERT_INT("num_cmds 1", repl_state_document_count(), 1);
        ASSERT_TRUE("scratch set before undo snapshot",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 3.0f) < 1e-6f);

        editor_undo_push_snapshot();
        editor_feed_line("A[0] = 7;");
        editor_feed_line("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", repl_state_document_count(), 3);
        ASSERT_TRUE("scratch changed after redo mutation",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 7.0f) < 1e-6f);

        editor_undo_pop_snapshot();
        ASSERT_INT("num_cmds after undo", repl_state_document_count(), 1);
        ASSERT_STR("cmd 0 source", editor_buffer_line(0), "  A[0] = 3;");
        ASSERT_TRUE("undo restores scratch value",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 3.0f) < 1e-6f);

        editor_undo_do_redo();
        ASSERT_INT("num_cmds after redo", repl_state_document_count(), 3);
        ASSERT_STR("cmd 2 source", editor_buffer_line(2), "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("redo restores scratch value",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 7.0f) < 1e-6f);
    }

    /* 3b. Undo ring is cleared by glr_ctrl_reset_all / scene switch /
     * example load. Cross-scene undo would otherwise restore a previous
     * scene's pre-mutation state into the current scene's live commands
     * — silently corrupting the active scene.
     *
     * Reproducer for the bug (without the clear):
     *   1. Populate scene A with 2 cmds; push an undo snapshot (saves
     *      A's current state).
     *   2. Reset (simulates scene switch / example load — the live REPL
     *      state goes empty but the undo ring is preserved by the
     *      buggy code).
     *   3. Populate scene B with 1 cmd; verify it stuck.
     *   4. Press Ctrl+Z. The pop pulls scene A's snapshot off the ring
     *      and restores 2 cmds into the live state — but the user
     *      thinks they're on scene B. Live state of "B" now shows
     *      what A had at push time.
     *
     * The fix clears the ring on reset / scene-load / example-load so
     * the pop after step 4 finds the ring empty and is a no-op.
     */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(11,11,11)");
        editor_feed_line("glVertex3f(22,22,22)");
        ASSERT_INT("cross-scene undo: scene A populated",
                   repl_state_document_count(), 2);
        editor_undo_push_snapshot();

        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_INT("cross-scene undo: reset clears live cmds",
                   repl_state_document_count(), 0);

        editor_feed_line("glVertex3f(99,99,99)");
        ASSERT_INT("cross-scene undo: scene B populated",
                   repl_state_document_count(), 1);

        editor_undo_pop_snapshot();
        /* With the fix, the reset cleared the undo ring, so this pop
         * is a no-op and live state stays at scene B (1 cmd). Without
         * the fix, the pop pulls scene A's snapshot off the ring and
         * overwrites scene B's content with A's 2 cmds. */
        ASSERT_INT("cross-scene undo: pop must NOT bleed scene A into scene B",
                   repl_state_document_count(), 1);
    }

    /* 3c. Generation counter: wholesale replacement bumps generation,
     * and pop/redo refuse to restore cross-generation snapshots even
     * if ring counts were somehow non-zero.
     *
     * Flow: edit scene A → push undo → wholesale replace (bumps gen
     * + clears doc) → populate scene B → Ctrl+Z must not restore A. */
    {
        unsigned int gen0;
        glr_ctrl_reset_all(); declare_test_vars();
        gen0 = editor_undo_generation();

        editor_feed_line("glVertex3f(1,1,1)");
        editor_undo_push_snapshot();
        editor_feed_line("glVertex3f(2,2,2)");
        ASSERT_INT("gen: pre-replacement cmd count", repl_state_document_count(), 2);

        /* Simulate scene switch: clear doc + bump generation. */
        glr_ctrl_reset_all(); declare_test_vars();
        ASSERT_TRUE("gen: generation bumped",
                    editor_undo_generation() > gen0);

        editor_feed_line("glVertex3f(99,99,99)");
        ASSERT_INT("gen: post-replacement cmd count", repl_state_document_count(), 1);

        editor_undo_pop_snapshot();
        ASSERT_INT("gen: pop after wholesale replacement is no-op",
                   repl_state_document_count(), 1);

        editor_undo_do_redo();
        ASSERT_INT("gen: redo after wholesale replacement is no-op",
                   repl_state_document_count(), 1);
    }

    /* 3d. Generation counter: undo within the same generation works
     * normally, and redo pushed by pop carries the current generation. */
    {
        glr_ctrl_reset_all(); declare_test_vars();

        editor_feed_line("glVertex3f(10,10,10)");
        editor_undo_push_snapshot();
        editor_feed_line("glVertex3f(20,20,20)");
        ASSERT_INT("gen same-gen: 2 cmds", repl_state_document_count(), 2);

        editor_undo_pop_snapshot();
        ASSERT_INT("gen same-gen: undo restores 1 cmd",
                   repl_state_document_count(), 1);

        editor_undo_do_redo();
        ASSERT_INT("gen same-gen: redo restores 2 cmds",
                   repl_state_document_count(), 2);
    }

    /* 3e. Generation counter: multiple wholesale replacements each
     * bump the generation, isolating snapshots from all prior worlds.
     *
     * World A → wholesale → World B → wholesale → World C.
     * Pop in C must not reach B's snapshot. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_undo_push_snapshot();
        editor_feed_line("glVertex3f(2,2,2)");

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(3,3,3)");
        editor_undo_push_snapshot();
        editor_feed_line("glVertex3f(4,4,4)");

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(5,5,5)");
        ASSERT_INT("gen multi: world C has 1 cmd",
                   repl_state_document_count(), 1);

        editor_undo_pop_snapshot();
        ASSERT_INT("gen multi: pop in world C is no-op (world B snapshot stale)",
                   repl_state_document_count(), 1);
    }

    /* 4. Deleting commands - basic */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", repl_state_document_count(), 3);

        editor_delete_cmd_range(1, 1, "test");
        ASSERT_INT("num_cmds after delete", repl_state_document_count(), 2);
        ASSERT_STR("line 0 still there", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("line 1 is now old line 2", editor_buffer_line(1), "  glVertex3f(2, 2, 2);");
    }

    /* 5. editor_delete_cmd_range - count=0 (no-op) */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_delete_cmd_range(0, 0, "noop");
        ASSERT_INT("delete count=0: no change", repl_state_document_count(), 1);
    }

    /* 6. editor_delete_cmd_range - start=repl_state_document_count() (out of bounds, no-op) */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_delete_cmd_range(1, 1, "oob");  /* start == repl_state_document_count() (1) */
        ASSERT_INT("delete oob start: no change", repl_state_document_count(), 1);
    }

    /* 7. editor_delete_cmd_range - count clamped when over-count */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        /* ask to delete 5 but only 1 remains at start=1 */
        editor_delete_cmd_range(1, 5, "clamp");
        ASSERT_INT("delete clamp: leaves 1 cmd", repl_state_document_count(), 1);
        ASSERT_STR("delete clamp: cmd 0 remains", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
    }

    /* 8. editor_delete_cmd_range - edit_line clamped when it would exceed repl_state_document_count() */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_state_edit_line_set(2);  /* beyond last cmd */
        editor_delete_cmd_range(1, 1, "elclamp");
        /* editor_state_edit_line() should be clamped to repl_state_document_count() (1) */
        ASSERT_INT("delete edit_line clamp: edit_line<=num_cmds", editor_state_edit_line() <= repl_state_document_count(), 1);
    }

    /* 8a. editor_delete_cmd_range - interior multi-line block */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_feed_line("glVertex3f(3,3,3)");
        editor_feed_line("glVertex3f(4,4,4)");
        editor_state_selection_set(1, 3);

        editor_delete_cmd_range(1, 3, "Deleted");

        ASSERT_INT("delete block: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("delete block: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("delete block: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
        ASSERT_INT("delete block: edit_line at deletion start", editor_state_edit_line(), 1);
        ASSERT_STR("delete block: input reloaded", editor_state_input().input, "glVertex3f(4, 4, 4)");
        ASSERT_TRUE("delete block: selection cleared", !editor_clipboard_sel_active());
        assert_status_contains("delete block: status line count", "Deleted 3 lines");
    }

    /* 8b. Backspace deletes a reversed selection */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_feed_line("glVertex3f(3,3,3)");
        editor_feed_line("glVertex3f(4,4,4)");
        editor_state_selection_set(4, 2);
        editor_state_edit_line_set(4);

        editor_handle_key(8, 0, 0);

        ASSERT_INT("backspace reversed selection: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("backspace reversed selection: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("backspace reversed selection: second survives", editor_buffer_line(1), "  glVertex3f(1, 1, 1);");
        ASSERT_INT("backspace reversed selection: edit_line", editor_state_edit_line(), 2);
        ASSERT_TRUE("backspace reversed selection: selection cleared", !editor_clipboard_sel_active());
    }

    /* 8b2. Ctrl+D deletes a selected range through its own key path */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_feed_line("glVertex3f(3,3,3)");
        editor_feed_line("glVertex3f(4,4,4)");
        editor_state_selection_set(3, 1);
        editor_state_edit_line_set(3);

        editor_handle_key(4, 0, 0);

        ASSERT_INT("ctrl-d selection: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("ctrl-d selection: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("ctrl-d selection: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
        ASSERT_INT("ctrl-d selection: edit_line", editor_state_edit_line(), 1);
        ASSERT_TRUE("ctrl-d selection: selection cleared", !editor_clipboard_sel_active());
        assert_status_contains("ctrl-d selection: status", "Deleted 3 lines");
    }

    /* 8c. Undo/redo after a multi-line delete */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_feed_line("glVertex3f(3,3,3)");
        editor_feed_line("glVertex3f(4,4,4)");

        editor_delete_cmd_range(1, 3, "Deleted");
        ASSERT_INT("delete undo setup: leaves 2 cmds", repl_state_document_count(), 2);

        editor_undo_pop_snapshot();
        ASSERT_INT("delete undo: restores 5 cmds", repl_state_document_count(), 5);
        ASSERT_STR("delete undo: middle restored", editor_buffer_line(2), "  glVertex3f(2, 2, 2);");

        editor_undo_do_redo();
        ASSERT_INT("delete redo: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("delete redo: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
    }

    /* 8c2. Delete range of 17+ lines (greater than MAX_COMMIT_CMDS) */
    {
        glr_ctrl_reset_all();
        for (int i = 0; i < 18; i++) {
            editor_feed_line("glVertex3f(0,0,0)");
        }
        ASSERT_INT("large delete setup: 18 lines fed", repl_state_document_count(), 18);
        ASSERT_INT("large delete setup: editor line_count is 18", editor_buffer_view().line_count, 18);

        /* Delete all 18 lines */
        editor_delete_cmd_range(0, 18, "Deleted");

        ASSERT_INT("large delete: leaves 0 cmds in repl", repl_state_document_count(), 0);
        ASSERT_INT("large delete: leaves 0 lines in editor buffer", editor_buffer_view().line_count, 0);
    }

    /* 8c3. Direct test for editor_undo_snapshot_restore cross-generation no-op */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        ASSERT_INT("snapshot restore cross-gen: document count before save",
                   repl_state_document_count(), 1);

        EditorUndoSnapshot snap;
        editor_undo_snapshot_save(&snap);

        /* Make a wholesale replacement which increments g_undo_generation */
        editor_undo_note_wholesale_replacement();

        /* Clear the live document commands so we can verify if restore actually recreates them */
        editor_delete_cmd_range(0, 1, "test");

        /* Now the document is empty after manual delete */
        ASSERT_INT("snapshot restore cross-gen: document count after wholesale replacement",
                   repl_state_document_count(), 0);

        /* Attempt to restore the snapshot with the old generation */
        editor_undo_snapshot_restore(&snap);

        /* It should be a no-op, i.e., the document remains empty! */
        ASSERT_INT("snapshot restore cross-gen: restore of old generation is a no-op",
                   repl_state_document_count(), 0);
    }

    /* 8d. Copy selected block and paste inside a later block */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glVertex3f(0,0,0);");
        editor_feed_line("glVertex3f(1,1,1);");
        editor_feed_line("glEnd();");
        editor_feed_line("if(x > 0) {");
        editor_feed_line("glColor3f(1,0,0);");
        editor_feed_line("}");
        editor_state_selection_set(1, 2);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy block: clipboard count", editor_state_clipboard_count(), 2);
        ASSERT_STR("copy block: first copied", editor_state_clipboard_mut()->lines[0], "    glVertex3f(0, 0, 0);");
        ASSERT_STR("copy block: second copied", editor_state_clipboard_mut()->lines[1], "    glVertex3f(1, 1, 1);");
        ASSERT_TRUE("copy block: selection cleared", !editor_clipboard_sel_active());

        editor_state_edit_line_set(5);
        editor_insert_mode_set(0);
        editor_handle_key(22, 0, 0);
        ASSERT_INT("paste block: cmd count", repl_state_document_count(), 9);
        ASSERT_STR("paste block: if preserved", editor_buffer_line(4), "  if(x > 0) {");
        ASSERT_STR("paste block: first inserted", editor_buffer_line(5), "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste block: second inserted", editor_buffer_line(6), "    glVertex3f(1, 1, 1);");
        ASSERT_STR("paste block: original body shifted", editor_buffer_line(7), "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste block: close brace preserved", editor_buffer_line(8), "  }");
        ASSERT_INT("paste block: edit_line after pasted block", editor_state_edit_line(), 7);
        ASSERT_STR("paste block: input reloaded after paste", editor_state_input().input, "glColor3f(1, 0, 0)");
        assert_status_contains("paste block: status", "Pasted 2 lines");

        editor_undo_pop_snapshot();
        ASSERT_INT("paste undo: restores original count", repl_state_document_count(), 7);
        ASSERT_STR("paste undo: original body restored", editor_buffer_line(5), "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste undo: close brace restored", editor_buffer_line(6), "  }");

        editor_undo_do_redo();
        ASSERT_INT("paste redo: restores pasted count", repl_state_document_count(), 9);
        ASSERT_STR("paste redo: first pasted line", editor_buffer_line(5), "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste redo: original body shifted again", editor_buffer_line(7), "    glColor3f(1, 0, 0);");
    }

    /* 8d.1. Regression: line paste of the carousel glow assignment
     * plus following color line must insert both. The first line uses
     * the active for-loop variable `i`; a paste that reports two lines
     * but silently drops handled per-line failures used to leave only
     * the glColor3f line behind. */
    {
        int glow_line = -1;
        int before_count;

        glr_ctrl_reset_all();
        load_carousel_copy_paste_fixture();

        for (int i = 0; i + 1 < repl_state_document_count(); i++) {
            const char *line = editor_buffer_line(i);
            if (line && strstr(line, "glow = 0.6 + 0.4 * sin(6*t + i*TAU*4/bulbs)") != NULL) {
                glow_line = i;
                break;
            }
        }
        ASSERT_TRUE("carousel glow line found", glow_line >= 0);
        ASSERT_TRUE("carousel color line follows glow",
                    strstr(editor_buffer_line(glow_line + 1),
                           "glColor3f(0.45, 0.07 + 0.31*glow, 0.16*glow)") != NULL);

        editor_state_selection_set(glow_line, glow_line + 1);
        editor_state_edit_line_set(glow_line + 1);
        editor_insert_mode_set(0);
        editor_handle_key(3, 0, 0);
        ASSERT_INT("carousel copy count", editor_state_clipboard_count(), 2);
        ASSERT_TRUE("carousel copy first is glow assignment",
                    strstr(editor_state_clipboard_mut()->lines[0],
                           "glow = 0.6 + 0.4 * sin") != NULL);
        ASSERT_TRUE("carousel copy second is color",
                    strstr(editor_state_clipboard_mut()->lines[1],
                           "glColor3f(0.45") != NULL);

        before_count = repl_state_document_count();
        editor_handle_key(22, 0, 0);

        ASSERT_INT("carousel paste inserts two lines",
                   repl_state_document_count(), before_count + 2);
        ASSERT_TRUE("carousel paste first inserted line is glow assignment",
                    strstr(editor_buffer_line(glow_line + 1),
                           "glow = 0.6 + 0.4 * sin(6*t + i*TAU*4/bulbs)") != NULL);
        ASSERT_TRUE("carousel paste second inserted line is color",
                    strstr(editor_buffer_line(glow_line + 2),
                           "glColor3f(0.45, 0.07 + 0.31*glow, 0.16*glow)") != NULL);
        ASSERT_TRUE("carousel original color shifted after paste",
                    strstr(editor_buffer_line(glow_line + 3),
                           "glColor3f(0.45, 0.07 + 0.31*glow, 0.16*glow)") != NULL);
        assert_status_contains("carousel paste status", "Pasted 2 lines");
    }

    /* 8d.2. Pasting the same two-line snippet outside its for-loop
     * should be all-or-nothing. The color line is valid there because
     * `glow` is declared, but the assignment line is not because `i`
     * is a loop-local. Do not silently paste only the valid tail. */
    {
        int before_count;

        glr_ctrl_reset_all();
        editor_feed_line("static float bulbs = 28;");
        editor_feed_line("static float glow;");
        editor_feed_line("for(i, 0, bulbs + 1) {");
        editor_feed_line("glow = 0.6 + 0.4 * sin(6*t + i*TAU*4/bulbs);");
        editor_feed_line("glColor3f(0.45, 0.07 + 0.31*glow, 0.16*glow);");
        editor_feed_line("}");

        editor_state_selection_set(3, 4);
        editor_state_edit_line_set(4);
        editor_insert_mode_set(0);
        editor_handle_key(3, 0, 0);
        ASSERT_INT("out-of-scope copy count", editor_state_clipboard_count(), 2);

        before_count = repl_state_document_count();
        editor_state_edit_line_set(2);
        editor_insert_mode_set(1);
        editor_handle_key(22, 0, 0);

        ASSERT_INT("out-of-scope paste is atomic",
                   repl_state_document_count(), before_count);
        ASSERT_TRUE("out-of-scope paste leaves for header in place",
                    strstr(editor_buffer_line(2), "for(i, 0, bulbs + 1)") != NULL);
        ASSERT_TRUE("out-of-scope paste did not insert color tail",
                    strstr(editor_buffer_line(2), "glColor3f(0.45") == NULL);
        assert_status_contains("out-of-scope paste status", "Paste failed at line 1 of 2");
        ASSERT_INT("out-of-scope paste status is error",
                   ui_state_status().kind, UI_STATUS_ERROR);
    }

    /* 8e. Cut selected block, then undo/redo */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_feed_line("glVertex3f(3,3,3)");
        editor_state_selection_set(1, 2);

        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut block: clipboard count", editor_state_clipboard_count(), 2);
        ASSERT_STR("cut block: first copied", editor_state_clipboard_mut()->lines[0], "  glVertex3f(1, 1, 1);");
        ASSERT_STR("cut block: second copied", editor_state_clipboard_mut()->lines[1], "  glVertex3f(2, 2, 2);");
        ASSERT_INT("cut block: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("cut block: first survivor", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("cut block: second survivor", editor_buffer_line(1), "  glVertex3f(3, 3, 3);");
        ASSERT_TRUE("cut block: selection cleared", !editor_clipboard_sel_active());

        editor_undo_pop_snapshot();
        ASSERT_INT("cut undo: restores 4 cmds", repl_state_document_count(), 4);
        ASSERT_STR("cut undo: first cut line restored", editor_buffer_line(1), "  glVertex3f(1, 1, 1);");

        editor_undo_do_redo();
        ASSERT_INT("cut redo: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("cut redo: second survivor", editor_buffer_line(1), "  glVertex3f(3, 3, 3);");
    }

    /* 8f. Copy/cut from a for-begin line captures the whole block */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 2) {");
        editor_feed_line("glVertex3f(i,0,0);");
        editor_feed_line("}");
        editor_state_edit_line_set(0);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy for block: clipboard count", editor_state_clipboard_count(), 3);
        ASSERT_TRUE("copy for block: first line is for-header",
                    strstr(editor_state_clipboard_mut()->lines[0], "for(") != NULL);
        ASSERT_TRUE("copy for block: last line closes block",
                    strstr(editor_state_clipboard_mut()->lines[2], "}") != NULL);
        ASSERT_INT("copy for block: source unchanged", repl_state_document_count(), 3);

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 2) {");
        editor_feed_line("glVertex3f(i,0,0);");
        editor_feed_line("}");
        editor_state_edit_line_set(0);

        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut for block: clipboard count", editor_state_clipboard_count(), 3);
        ASSERT_TRUE("cut for block: first line is for-header",
                    strstr(editor_state_clipboard_mut()->lines[0], "for(") != NULL);
        ASSERT_TRUE("cut for block: last line closes block",
                    strstr(editor_state_clipboard_mut()->lines[2], "}") != NULL);
        ASSERT_INT("cut for block: buffer empty", repl_state_document_count(), 0);
        ASSERT_INT("cut for block: edit line at start", editor_state_edit_line(), 0);
    }

    /* 8g. Paste with empty clipboard */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_state_clipboard_count_set(0);

        editor_handle_key(22, 0, 0);

        ASSERT_INT("paste empty: no mutation", repl_state_document_count(), 1);
        ASSERT_STR("paste empty: source unchanged", editor_buffer_line(0), "  glVertex3f(1, 1, 1);");
        assert_status_contains("paste empty: status", "Clipboard empty");
    }

    /* 8h. Paste refuses to overflow the command buffer */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(0));
        editor_state_clipboard_count_set(1);
        repl_state_document_count_set(MAX_EDITOR_COMMANDS);

        editor_handle_key(22, 0, 0);

        ASSERT_INT("paste full: no mutation", repl_state_document_count(), MAX_EDITOR_COMMANDS);
        ASSERT_INT("paste full: clipboard preserved", editor_state_clipboard_count(), 1);
        assert_status_contains("paste full: status", "Command buffer full");
    }

    /* 8i. Copy refuses to capture a range that contains a float
     * declaration. (Paste-time decl guard is gone in Phase B —
     * copy-time guard prevents decls from entering the clipboard
     * in the first place; pasting a decl that somehow ended up in
     * the clipboard now goes through the standard commit chain
     * and surfaces a parser-level diagnostic if it conflicts.) */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_edit_line_set(0);

        editor_handle_key(3, 0, 0);

        ASSERT_INT("copy decl line: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("copy decl line: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("copy decl line: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("copy decl line: status", "Cannot copy float declarations");
    }

    /* 8j. Copy/cut in insert mode clear selection without touching clipboard */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_selection_set(0, 1);
        editor_state_edit_line_set(1);
        editor_insert_mode_set(1);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("copy insert mode: clipboard unchanged", editor_state_clipboard_count(), 1);
        ASSERT_STR("copy insert mode: clipboard source unchanged", editor_state_clipboard_mut()->lines[0], "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("copy insert mode: selection cleared", !editor_clipboard_sel_active());

        editor_state_selection_set(0, 1);
        editor_insert_mode_set(1);
        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("cut insert mode: clipboard unchanged", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut insert mode: clipboard source unchanged", editor_state_clipboard_mut()->lines[0], "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("cut insert mode: selection cleared", !editor_clipboard_sel_active());
    }

    /* 8k. Backspace in insert mode edits input instead of selected source lines */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_state_selection_set(0, 1);
        editor_state_edit_line_set(1);
        editor_insert_mode_set(1);
        set_editor_input("abcd");
        editor_cursor_pos_set(3);

        editor_handle_key(8, 0, 0);

        ASSERT_INT("backspace insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_STR("backspace insert mode: input edited", editor_state_input().input, "abd");
        ASSERT_INT("backspace insert mode: cursor moved", editor_cursor_pos(), 2);
        ASSERT_INT("backspace insert mode: still inserting", editor_insert_mode(), 1);
    }

    /* 8k. Committing incomplete commands reports incomplete, not unknown */
    {
        glr_ctrl_reset_all();
        set_editor_input("glColor3f(1, 1");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("commit incomplete glColor3f: no cmd added", repl_state_document_count(), 0);
        assert_status_contains("commit incomplete glColor3f: status", "Incomplete command");
        assert_status_contains("commit incomplete glColor3f: missing paren", "missing ')'");
        ASSERT_TRUE("commit incomplete glColor3f: not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_ctrl_reset_all();
        set_editor_input("glTotallyUnknown(1, 2, 3)");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("commit unknown command: no cmd added", repl_state_document_count(), 0);
        assert_status_contains("commit unknown command: status", "Unknown cmd");
    }

    /* 9. editor_load_line_to_input - CMD_GOTO_LABEL path */
    {
        glr_ctrl_reset_all();
        /* Feed a label command to create a CMD_GOTO_LABEL entry */
        editor_feed_line(":myloop");
        ASSERT_INT("label cmd created", repl_state_document_count(), 1);
        ASSERT_INT("label cmd type", repl_state_document_cmds_mut()[0].type, CMD_GOTO_LABEL);

        /* Now load it back into input */
        editor_load_line_to_input(0);
        /* Input should start with ':' and contain the label name */
        ASSERT_TRUE("label load: starts with colon", editor_state_input().input[0] == ':');
        ASSERT_TRUE("label load: contains name", strstr(editor_state_input().input, "myloop") != NULL);
    }

    /* 10. editor_navigate_to_line - clamp target < 0 */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(-5);
        ASSERT_INT("editor_navigate_to_line neg: edit_line=0", editor_state_edit_line(), 0);
    }

    /* 11. editor_navigate_to_line - clamp target > repl_state_document_count() */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(999);
        ASSERT_INT("editor_navigate_to_line over: edit_line=repl_state_document_count()", editor_state_edit_line(), repl_state_document_count());
    }

    /* 12. editor_try_commit_assign_variable - append to end (existing tests cover this) */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "n = 10.5");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_assign_variable();
        ASSERT_INT("editor_try_commit_assign_variable returns 1", r, 1);
        ASSERT_INT("num_cmds 1 after assign", repl_state_document_count(), 1);
        ASSERT_STR("assigned cmd source", editor_buffer_line(0), "  n = 10.5;");
    }

    /* 12b. Enter on an empty insert row creates a persistent blank line. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1,0,0);");
        editor_feed_line("glEnd();");

        editor_navigate_to_line(1);
        editor_cursor_pos_set(1);
        editor_handle_key('\r', 0, 0);

        ASSERT_INT("blank line setup: insert mode entered", editor_insert_mode(), 1);
        ASSERT_INT("blank line setup: edit line moved below current", editor_state_edit_line(), 2);

        editor_handle_key('\r', 0, 0);

        ASSERT_INT("blank line inserted: doc count grows", repl_state_document_count(), 4);
        ASSERT_INT("blank line inserted: cmd type", repl_state_document_cmds_mut()[2].type, CMD_EMPTY);
        ASSERT_STR("blank line inserted: source text empty", editor_buffer_line(2), "");
        ASSERT_INT("blank line inserted: edit line advances", editor_state_edit_line(), 3);
        ASSERT_INT("blank line inserted: stays in insert mode", editor_insert_mode(), 1);

        editor_navigate_to_line(2);

        ASSERT_INT("blank line edit uses scope indent",
                   current_active_indent_chars(),
                   repl_source_scope_cmd_indent_chars(2));
    }

    /* 12c. Enter at column 0 of a committed line inserts a real,
     * persistent blank line ABOVE and keeps the cursor on the original
     * content (it follows the text down one row) — "newline before the
     * cursor". No transient insert row that vanishes on a same-index
     * click. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glColor3f(1,0,0);");   /* index 0 */
        editor_feed_line("glEnd();");            /* index 1 (the 'World') */

        editor_navigate_to_line(1);
        editor_cursor_pos_set(0);
        editor_handle_key('\r', 0, 0);

        ASSERT_INT("enter-col0: blank inserted immediately", repl_state_document_count(), 3);
        ASSERT_INT("enter-col0: blank above is empty", repl_state_document_cmds_mut()[1].type, CMD_EMPTY);
        ASSERT_STR("enter-col0: blank source text empty", editor_buffer_line(1), "");
        ASSERT_INT("enter-col0: content shifts down", repl_state_document_cmds_mut()[2].type, CMD_END);
        ASSERT_INT("enter-col0: cursor follows content down", editor_state_edit_line(), 2);
        ASSERT_INT("enter-col0: not insert mode", editor_insert_mode(), 0);
        ASSERT_INT("enter-col0: cursor at column 0", editor_cursor_pos(), 0);

        /* Persisted: navigating to the same index does not drop it. */
        editor_navigate_to_line(2);
        ASSERT_INT("enter-col0: blank persists after navigation", repl_state_document_count(), 3);
    }

    /* 12d. An empty insert row (cursor > 0 path) persists as a real blank
     * line whether the next navigation is a different index (down-arrow)
     * or the SAME document index (a click on the line directly below,
     * which hit-tests to the insert row's own index). Previously the
     * same-index case silently discarded the row. */
    {
        /* Same-index navigation (click) commits the blank. */
        glr_ctrl_reset_all();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1,0,0);");
        editor_feed_line("glEnd();");
        editor_navigate_to_line(1);
        editor_cursor_pos_set(2);                /* cursor > 0 → insert row below */
        editor_handle_key('\r', 0, 0);
        ASSERT_INT("insert-row setup: insert mode", editor_insert_mode(), 1);
        ASSERT_INT("insert-row setup: no real line yet", repl_state_document_count(), 3);
        int insert_idx = editor_state_edit_line();
        editor_navigate_to_line(insert_idx);     /* "click" the same index */
        ASSERT_INT("insert-row click(same idx): blank persists", repl_state_document_count(), 4);
        ASSERT_INT("insert-row click(same idx): blank is empty",
                   repl_state_document_cmds_mut()[insert_idx].type, CMD_EMPTY);

        /* Down-arrow (different index) is consistent: also persists. */
        glr_ctrl_reset_all();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1,0,0);");
        editor_feed_line("glEnd();");
        editor_navigate_to_line(1);
        editor_cursor_pos_set(2);
        editor_handle_key('\r', 0, 0);
        int insert_idx2 = editor_state_edit_line();
        editor_navigate_to_line(insert_idx2 + 1); /* down-arrow */
        ASSERT_INT("insert-row down-arrow: blank persists", repl_state_document_count(), 4);
        ASSERT_INT("insert-row down-arrow: blank is empty",
                   repl_state_document_cmds_mut()[insert_idx2].type, CMD_EMPTY);
    }

    /* 12e. The editor statusbar surfaces a count of structurally
     * unbalanced bracket commands via snap.unbalanced_count (an unmatched
     * glPushMatrix + an unmatched glBegin -> 2; balanced -> 0). */
    {
        UiRenderSnapshot snap;

        glr_ctrl_reset_all();
        editor_feed_line("glPushMatrix();");
        editor_feed_line("glTranslatef(1, 0, 0);");
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_feed_line("glVertex3f(0, 0, 0);");
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar counts 2 unbalanced", snap.unbalanced_count, 2);

        editor_feed_line("glEnd();");
        editor_feed_line("glPopMatrix();");
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar count clears when balanced", snap.unbalanced_count, 0);
    }

    /* 13. editor_try_commit_assign_variable - inserting mode (inserts before cursor) */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        /* Put cursor at line 1, inserting mode */
        editor_state_edit_line_set(1);
        editor_insert_mode_set(1);
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "x = 3.0");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_assign_variable();
        ASSERT_INT("insert assign returns 1", r, 1);
        ASSERT_INT("insert assign: 3 cmds", repl_state_document_count(), 3);
        /* The assignment should appear at index 1 */
        ASSERT_INT("insert assign: cmd 1 is VAR_ASSIGN", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
    }

    /* 14. editor_try_commit_assign_variable - overwrite existing cmd */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("n = 1.0");
        editor_feed_line("glVertex3f(1,1,1)");
        /* Navigate to the assignment line and overwrite */
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "n = 7.0");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_assign_variable();
        ASSERT_INT("overwrite assign returns 1", r, 1);
        ASSERT_STR("overwrite assign: new source", editor_buffer_line(0), "  n = 7.0;");
        ASSERT_INT("overwrite assign: edit_line advanced", editor_state_edit_line(), 1);
    }

    /* 15. Committing for loops - basic open brace form */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 5) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_for_loop();
        ASSERT_INT("editor_try_commit_for_loop returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after for", repl_state_document_count(), 2);
        ASSERT_STR("for loop source", editor_buffer_line(0), "  for(i, 0, 5) {");
        ASSERT_STR("for end source", editor_buffer_line(1), "  }");
    }

    /* 16. editor_try_commit_for_loop - with explicit step */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 10, 2) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_for_loop();
        ASSERT_INT("for with step returns 1", r, 1);
        ASSERT_INT("for with step: 2 cmds", repl_state_document_count(), 2);
        /* Source should contain step value */
        ASSERT_TRUE("for with step: source has 2 step",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "2") != NULL);
    }

    /* 17. editor_try_commit_for_loop - update existing for-begin header */
    {
        glr_ctrl_reset_all();
        /* Create an existing for-loop */
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 5) {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_for_loop();
        ASSERT_INT("setup: 2 cmds", repl_state_document_count(), 2);

        /* Navigate back to line 0 and update the for header */
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 10) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_for_loop();
        ASSERT_INT("update for-begin returns 1", r, 1);
        /* Should update in place, not add more commands */
        ASSERT_INT("update for-begin: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("update for-begin: source updated",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "10") != NULL);
    }

    /* 18. editor_try_commit_for_loop - inline form (for with body on same line) */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) glVertex3f(i,0,0);");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_for_loop();
        ASSERT_INT("for inline returns 1", r, 1);
        /* Should create 3 cmds: for-begin, body, for-end */
        ASSERT_INT("for inline: 3 cmds", repl_state_document_count(), 3);
        ASSERT_INT("for inline: cmd 0 is FOR_BEGIN", repl_state_document_cmds_mut()[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("for inline: cmd 2 is FOR_END", repl_state_document_cmds_mut()[2].type, CMD_FOR_END);
    }

    /* 19. Committing func defs - basic */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func0(x, y) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_func_def();
        ASSERT_INT("editor_try_commit_func_def returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after func", repl_state_document_count(), 2);
        ASSERT_STR("func def source", editor_buffer_line(0), "  func0(x, y) {");
        ASSERT_STR("func end source", editor_buffer_line(1), "  }");
    }

    /* 20. editor_try_commit_func_def - update existing func-def header */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func1(a) {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_func_def();
        ASSERT_INT("func update setup: 2 cmds", repl_state_document_count(), 2);

        /* Navigate back and update the func-def header */
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func1(a, b) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_func_def();
        ASSERT_INT("update func-def returns 1", r, 1);
        ASSERT_INT("update func-def: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("update func-def: source has b param",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "b") != NULL);
    }

    /* 21. Committing if blocks - basic */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_if_block();
        ASSERT_INT("editor_try_commit_if_block returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after if", repl_state_document_count(), 2);
        ASSERT_STR("if block source", editor_buffer_line(0), "  if(x > 0) {");
        ASSERT_STR("if end source", editor_buffer_line(1), "  }");
    }

    /* 22. editor_try_commit_if_block - update existing if-begin */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_if_block();

        /* Navigate back and update the if condition */
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x < 0) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_if_block();
        ASSERT_INT("update if-begin returns 1", r, 1);
        ASSERT_INT("update if-begin: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("update if-begin: source updated", editor_buffer_line(0), "  if(x < 0) {");
    }

    /* 23. Committing close brace - for-loop */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("for(i, 0, 1) {");
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_state_edit_line_set(1);

        int r = editor_try_commit_close_brace();
        ASSERT_INT("editor_try_commit_close_brace returns 1", r, 1);
        ASSERT_INT("num_cmds after brace", repl_state_document_count(), 2);
        ASSERT_STR("close brace source", editor_buffer_line(1), "  }");
    }

    /* 26. Committing close brace - while in inserting mode closes existing end */
    {
        glr_ctrl_reset_all();
        /* Set up: for-loop with begin+end, enter inserting mode inside */
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_for_loop();
        /* editor_state_edit_line()=1, editor_insert_mode()=1 - we're inside the loop */
        ASSERT_INT("insert-close setup: in inserting", editor_insert_mode(), 1);
        ASSERT_INT("insert-close setup: edit_line=1", editor_state_edit_line(), 1);

        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_close_brace();
        ASSERT_INT("insert-close brace returns 1", r, 1);
        ASSERT_INT("insert-close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 27. Committing close brace - func-def */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func2() {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_func_def();
        /* Now close the func with '}' */
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_close_brace();
        ASSERT_INT("func close brace returns 1", r, 1);
        ASSERT_INT("func close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 28. Committing close brace - if-block */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }
        editor_try_commit_if_block();
        /* Inserting inside if-block, close it */
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_close_brace();
        ASSERT_INT("if close brace returns 1", r, 1);
        ASSERT_INT("if close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 29. editor_try_commit_for_loop - empty body emits error */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) ;");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_for_loop();
        ASSERT_INT("for empty body returns 1 (error)", r, 1);
        /* No commands committed on bad body */
        ASSERT_INT("for empty body: no cmds added", repl_state_document_count(), 0);
    }

    /* 30. editor_try_commit_func_def - func with no params */
    {
        glr_ctrl_reset_all();
        {
            EditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func3() {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = editor_try_commit_func_def();
        ASSERT_INT("func no-params returns 1", r, 1);
        ASSERT_INT("func no-params: 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("func no-params: source correct",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "func3") != NULL);
    }

    /* 24. prof_frame_tick - increments staleness counters */
    {
        /* Call begin/end to prime a section, then tick several frames */
        prof_begin(PROF_RENDER3D);
        prof_end(PROF_RENDER3D);
        /* Frame tick - sections that didn't run this frame become stale */
        prof_frame_tick();
        prof_frame_tick();
        /* Should not crash; calling multiple times is fine */
        ASSERT_TRUE("prof_frame_tick survives", 1);
    }

    /* ---- Float declaration parsing (regression tests) ---- */

    /* 26. float decl without trailing semicolon (interactive ';' key path) */
    {
        glr_ctrl_reset_all();

        /* Simulate the interactive ';' key handler: editor_state_input().input has no ';' */
        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float tmp", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("float_decl no-semi: accepted", result, 1);
        ASSERT_INT("float_decl no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl no-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("float_decl no-semi: payload.decl.count", repl_state_document_cmds_mut()[0].payload.decl.count, 1);
        ASSERT_STR("float_decl no-semi: var name", repl_state_document_cmds_mut()[0].payload.decl.names[0], "tmp");
        ASSERT_TRUE("float_decl no-semi: predef registered",
                     repl_eval_find_predef_var_idx("tmp") >= 0);
    }

    /* 27. float decl WITH trailing semicolon (editor_feed_line path) */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float abc;");
        ASSERT_INT("float_decl with-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl with-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl with-semi: var name", repl_state_document_cmds_mut()[0].payload.decl.names[0], "abc");
        ASSERT_TRUE("float_decl with-semi: predef registered",
                     repl_eval_find_predef_var_idx("abc") >= 0);
    }

    /* 28. float decl with initializer, no trailing semicolon */
    {
        glr_ctrl_reset_all();

        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float tmp = 0", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("float_decl init no-semi: accepted", result, 1);
        ASSERT_INT("float_decl init no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl init no-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init no-semi: var name", repl_state_document_cmds_mut()[0].payload.decl.names[0], "tmp");
        int idx = repl_eval_find_predef_var_idx("tmp");
        ASSERT_TRUE("float_decl init no-semi: predef registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init no-semi: value is 0",
                         g_predef_vars[idx].value == 0.0f);
    }

    /* 29. float decl with initializer expression */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float radius = 2.5;");
        ASSERT_INT("float_decl init expr: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl init expr: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init expr: var name", repl_state_document_cmds_mut()[0].payload.decl.names[0], "radius");
        int idx = repl_eval_find_predef_var_idx("radius");
        ASSERT_TRUE("float_decl init expr: registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init expr: value is 2.5",
                         g_predef_vars[idx].value == 2.5f);
    }

    /* 30. multi-name float decl without semicolon */
    {
        glr_ctrl_reset_all();

        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, b, c", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("float_decl multi no-semi: accepted", result, 1);
        ASSERT_INT("float_decl multi no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl multi no-semi: payload.decl.count",
                   repl_state_document_cmds_mut()[0].payload.decl.count, 3);
        ASSERT_TRUE("float_decl multi no-semi: a registered",
                     repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: b registered",
                     repl_eval_find_predef_var_idx("b") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: c registered",
                     repl_eval_find_predef_var_idx("c") >= 0);
    }

    /* 31. multi-name float decl with initializers */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float x = 1, y = 2;");
        ASSERT_INT("float_decl multi init: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl multi init: payload.decl.count",
                   repl_state_document_cmds_mut()[0].payload.decl.count, 2);
        int xi = repl_eval_find_predef_var_idx("x");
        int yi = repl_eval_find_predef_var_idx("y");
        ASSERT_TRUE("float_decl multi init: x registered", xi >= 0);
        ASSERT_TRUE("float_decl multi init: y registered", yi >= 0);
        if (xi >= 0)
            ASSERT_TRUE("float_decl multi init: x value is 1",
                         g_predef_vars[xi].value == 1.0f);
        if (yi >= 0)
            ASSERT_TRUE("float_decl multi init: y value is 2",
                         g_predef_vars[yi].value == 2.0f);
    }

    /* 32. float decl inserted above existing non-decl commands.
     * Regression: previously `float tmp = 40;` committed after
     * `n = tmp;` could land below its reference, producing
     *     n = tmp;
     *     float tmp = 40;
     * which is semantically wrong. New behavior pushes decls to
     * the top of non-decl code. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        editor_feed_line("glBegin(GL_POINTS);");
        ASSERT_INT("decl-top baseline: 3 cmds", repl_state_document_count(), 3);

        editor_feed_line("float tmp = 40;");
        ASSERT_INT("decl-top: 4 cmds after float tmp", repl_state_document_count(), 4);
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[0] is float n", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: repl_state_document_cmds_mut()[0] name", repl_state_document_cmds_mut()[0].payload.decl.names[0], "n");
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[1] is float tmp", repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: repl_state_document_cmds_mut()[1] name", repl_state_document_cmds_mut()[1].payload.decl.names[0], "tmp");
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[2] is assign", repl_state_document_cmds_mut()[2].type, CMD_VAR_ASSIGN);
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[3] is glBegin", repl_state_document_cmds_mut()[3].type, CMD_BEGIN);
    }

    /* 33. float decl from edit position in middle of code still
     * lands at the top (not at the cursor). */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glBegin(GL_LINES);");
        editor_feed_line("glEnd();");
        ASSERT_INT("decl-top mid: 2 cmds", repl_state_document_count(), 2);

        /* Simulate being on line 1 in overwrite mode when committing
         * a float decl through the ';' key (no trailing ';'). */
        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float radius = 3", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(1);
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("decl-top mid: accepted", result, 1);
        ASSERT_INT("decl-top mid: 3 cmds", repl_state_document_count(), 3);
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[0] is decl",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top mid: repl_state_document_cmds_mut()[0] name",
                   repl_state_document_cmds_mut()[0].payload.decl.names[0], "radius");
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[1] preserved glBegin",
                   repl_state_document_cmds_mut()[1].type, CMD_BEGIN);
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[2] preserved glEnd",
                   repl_state_document_cmds_mut()[2].type, CMD_END);
    }

    /* 34. Overwriting a multi-name decl that shares a name with the new
     * decl must not fail with "already declared". Regression: previously
     * `declare_predef_var` ran before `undeclare_predef_var`, so shared
     * names (still registered from the old decl) collided. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float a, b;");
        ASSERT_INT("overwrite shared: baseline cmd count", repl_state_document_count(), 1);
        ASSERT_TRUE("overwrite shared: a registered",
                    repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: b registered",
                    repl_eval_find_predef_var_idx("b") >= 0);

        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, c", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("overwrite shared: accepted", result, 1);
        ASSERT_INT("overwrite shared: still 1 cmd", repl_state_document_count(), 1);
        ASSERT_INT("overwrite shared: payload.decl.count", repl_state_document_cmds_mut()[0].payload.decl.count, 2);
        ASSERT_STR("overwrite shared: slot 0 is a", repl_state_document_cmds_mut()[0].payload.decl.names[0], "a");
        ASSERT_STR("overwrite shared: slot 1 is c", repl_state_document_cmds_mut()[0].payload.decl.names[1], "c");
        ASSERT_TRUE("overwrite shared: a still registered",
                    repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: c registered",
                    repl_eval_find_predef_var_idx("c") >= 0);
        ASSERT_TRUE("overwrite shared: b unregistered",
                    repl_eval_find_predef_var_idx("b") < 0);
    }

    /* 35. Overwriting `float n, b;` → `float n;` with `n` referenced
     * elsewhere must succeed (only `b` is being dropped, and `b` is not
     * referenced). Regression: previously the check iterated ALL old
     * names and errored on `n is in use` even though `n` is being kept. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n, b;");
        editor_feed_line("n = 5;");
        ASSERT_INT("drop b: baseline cmd count", repl_state_document_count(), 2);

        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float n", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("drop b: accepted", result, 1);
        ASSERT_INT("drop b: payload.decl.count now 1", repl_state_document_cmds_mut()[0].payload.decl.count, 1);
        ASSERT_STR("drop b: slot 0 is n", repl_state_document_cmds_mut()[0].payload.decl.names[0], "n");
        ASSERT_TRUE("drop b: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_TRUE("drop b: b unregistered",
                    repl_eval_find_predef_var_idx("b") < 0);
    }

    /* 36. Attempting to drop a name that IS referenced elsewhere must
     * fail, and the error must name the dropped variable (not a
     * different name in the same decl). */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n, b;");
        editor_feed_line("b = 7;");
        ASSERT_INT("drop referenced b: baseline", repl_state_document_count(), 2);

        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float n", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("drop referenced b: handler consumed input", result, 1);
        /* Overwrite must have been rejected: decl still has both names */
        ASSERT_INT("drop referenced b: decl unchanged",
                   repl_state_document_cmds_mut()[0].payload.decl.count, 2);
        ASSERT_STR("drop referenced b: b preserved",
                   repl_state_document_cmds_mut()[0].payload.decl.names[1], "b");
        ASSERT_TRUE("drop referenced b: status mentions 'b'",
                    strstr(g_status, "'b'") != NULL);
        ASSERT_TRUE("drop referenced b: status does not mention 'n'",
                    strstr(g_status, "'n'") == NULL);
    }

    /* 37. Deleting a declaration line is rejected when the var is still
     * referenced outside the deleted range. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        ASSERT_TRUE("delete referenced decl setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);

        editor_delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete referenced decl: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("delete referenced decl: first still decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("delete referenced decl: second still assign", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("delete referenced decl: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        assert_status_contains("delete referenced decl: status", "'n'");
        assert_status_contains("delete referenced decl: status reason", "still referenced");
    }

    /* 37a. Ctrl+X rejects cutting declaration lines */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_edit_line_set(0);

        editor_handle_key(24, 0, 0);

        ASSERT_INT("cut referenced decl: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("cut referenced decl: first still decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("cut referenced decl: second still assign", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("cut referenced decl: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut referenced decl: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut referenced decl: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("cut referenced decl: status", "Cannot remove float declarations");
    }

    /* 37b. Ctrl+X rejects blocks that include a declaration and all uses */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        ASSERT_TRUE("cut decl block setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_selection_set(0, 1);

        editor_handle_key(24, 0, 0);

        ASSERT_INT("cut decl block: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_TRUE("cut decl block: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut decl block: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut decl block: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("cut decl block: status", "Cannot remove float declarations");
    }

    /* 38. Deleting a declaration together with all its uses succeeds:
     * no remaining references means the variable can be cleanly removed. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        editor_feed_line("n = 5;");
        ASSERT_TRUE("delete decl block setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);

        editor_delete_cmd_range(0, 2, "Deleted");

        ASSERT_INT("delete decl block: cmd count is zero", repl_state_document_count(), 0);
        ASSERT_TRUE("delete decl block: n unregistered",
                    repl_eval_find_predef_var_idx("n") < 0);
    }

    /* 38a. Deleting an unreferenced decl line on its own succeeds. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float n;");
        ASSERT_TRUE("delete unref decl setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);

        editor_delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete unref decl: cmd count is zero", repl_state_document_count(), 0);
        ASSERT_TRUE("delete unref decl: n unregistered",
                    repl_eval_find_predef_var_idx("n") < 0);
    }

    /* 39. Per-declaration name limit rejects atomically */
    {
        glr_ctrl_reset_all();
        set_editor_input("float v0, v1, v2, v3, v4, v5, v6, v7, v8");
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();

        ASSERT_INT("decl per-line overflow: handler consumed input", result, 1);
        ASSERT_INT("decl per-line overflow: no cmd added", repl_state_document_count(), 0);
        ASSERT_INT("decl per-line overflow: only t registered", g_num_predef_vars, 1);
        for (int i = 0; i <= MAX_NAMES_PER_DECL; i++) {
            char name[REPL_PREDEF_NAME_MAX];
            char label[128];
            snprintf(name, sizeof(name), "v%d", i);
            snprintf(label, sizeof(label), "decl per-line overflow: %s not registered", name);
            ASSERT_TRUE(label, repl_eval_find_predef_var_idx(name) < 0);
        }
        assert_status_contains("decl per-line overflow: status count", "too many names per declaration");
        assert_status_contains("decl per-line overflow: status max", "max 8");
    }

    /* 40. Total variable table limit rejects atomically */
    {
        glr_ctrl_reset_all();
        /* Fill all user-declarable slots (MAX_PREDEF_VARS - 1; slot 0 is
         * reserved for t which is re-declared on every reset). Feed in
         * batches of 8 — the per-line name limit. */
        int user_slots = MAX_PREDEF_VARS - 1;
        int filled_cmds = 0;
        char last_var[16] = "";
        for (int base = 0; base < user_slots; base += 8) {
            int count = user_slots - base;
            if (count > 8) count = 8;
            char decl[256] = "float ";
            int pos = 6;
            for (int j = 0; j < count; j++) {
                if (j > 0) { decl[pos++] = ','; decl[pos++] = ' '; }
                int n = snprintf(decl + pos, sizeof(decl) - pos, "v%d", base + j);
                pos += n;
                if (j == count - 1)
                    snprintf(last_var, sizeof(last_var), "v%d", base + j);
            }
            snprintf(decl + pos, sizeof(decl) - pos, ";");
            editor_feed_line(decl);
            filled_cmds++;
        }
        ASSERT_INT("decl table full setup: all decl cmds", repl_state_document_count(), filled_cmds);
        ASSERT_INT("decl table full setup: all slots used", g_num_predef_vars, MAX_PREDEF_VARS);

        set_editor_input("float overflow");
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);
        int result = editor_try_commit_float_decl();

        ASSERT_INT("decl table full: handler consumed input", result, 1);
        ASSERT_INT("decl table full: cmd count unchanged", repl_state_document_count(), filled_cmds);
        ASSERT_INT("decl table full: var count unchanged", g_num_predef_vars, MAX_PREDEF_VARS);
        ASSERT_TRUE("decl table full: overflow not registered",
                    repl_eval_find_predef_var_idx("overflow") < 0);
        ASSERT_TRUE("decl table full: first existing var remains",
                    repl_eval_find_predef_var_idx("v0") >= 0);
        ASSERT_TRUE("decl table full: last existing var remains",
                    repl_eval_find_predef_var_idx(last_var) >= 0);
        char max_msg[32];
        snprintf(max_msg, sizeof(max_msg), "max %d", MAX_PREDEF_VARS);
        assert_status_contains("decl table full: status full", "variable table full");
        assert_status_contains("decl table full: status max", max_msg);
    }

    /* 41. Declaration validation failures are atomic */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float anchor;");
        ASSERT_INT("decl atomic setup: one decl", repl_state_document_count(), 1);
        ASSERT_TRUE("decl atomic setup: anchor registered",
                    repl_eval_find_predef_var_idx("anchor") >= 0);

        assert_float_decl_rejected_atomic("decl duplicate", "float dup, dup",
                                          "dup", "duplicate name 'dup'");
        assert_float_decl_rejected_atomic("decl reserved", "float sin",
                                          "sin", "'sin' is reserved");
        assert_float_decl_rejected_atomic("decl invalid", "float 1bad",
                                          "1bad", "expected identifier");
        assert_float_decl_rejected_atomic("decl overlong", "float abcdefghijklmnop",
                                          "abcdefghijklmnop", "max 15");

        ASSERT_INT("decl atomic: only anchor cmd remains", repl_state_document_count(), 1);
        ASSERT_INT("decl atomic: only t plus anchor registered", g_num_predef_vars, 2);
        ASSERT_TRUE("decl atomic: anchor still registered",
                    repl_eval_find_predef_var_idx("anchor") >= 0);
    }

    /* 42. Expanding a multi-name decl (float a,b,c → float a,b,c,d) must
     * preserve the live values of kept variables. Regression: the old
     * overwrite path undeclared ALL names then re-declared them with
     * value 0.0f, silently zeroing a, b, and c. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float a, b, c;");
        editor_feed_line("a = 1;");
        editor_feed_line("b = 2;");
        editor_feed_line("c = 3;");
        ASSERT_INT("expand decl: baseline cmd count", repl_state_document_count(), 4);

        int ai = repl_eval_find_predef_var_idx("a");
        int bi = repl_eval_find_predef_var_idx("b");
        int ci = repl_eval_find_predef_var_idx("c");
        ASSERT_TRUE("expand decl: a registered before", ai >= 0);
        ASSERT_TRUE("expand decl: b registered before", bi >= 0);
        ASSERT_TRUE("expand decl: c registered before", ci >= 0);
        if (ai >= 0) ASSERT_TRUE("expand decl: a value is 1 before",
                                  g_predef_vars[ai].value == 1.0f);
        if (bi >= 0) ASSERT_TRUE("expand decl: b value is 2 before",
                                  g_predef_vars[bi].value == 2.0f);
        if (ci >= 0) ASSERT_TRUE("expand decl: c value is 3 before",
                                  g_predef_vars[ci].value == 3.0f);

        /* Overwrite decl to add d */
        extern int editor_try_commit_float_decl(void);
        {
            EditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, b, c, d", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = editor_try_commit_float_decl();
        ASSERT_INT("expand decl: accepted", result, 1);
        ASSERT_INT("expand decl: still 4 cmds", repl_state_document_count(), 4);
        ASSERT_INT("expand decl: payload.decl.count is 4", repl_state_document_cmds_mut()[0].payload.decl.count, 4);

        ai = repl_eval_find_predef_var_idx("a");
        bi = repl_eval_find_predef_var_idx("b");
        ci = repl_eval_find_predef_var_idx("c");
        int di = repl_eval_find_predef_var_idx("d");
        ASSERT_TRUE("expand decl: a still registered", ai >= 0);
        ASSERT_TRUE("expand decl: b still registered", bi >= 0);
        ASSERT_TRUE("expand decl: c still registered", ci >= 0);
        ASSERT_TRUE("expand decl: d newly registered", di >= 0);

        if (ai >= 0)
            ASSERT_TRUE("expand decl: a value preserved (1)",
                        g_predef_vars[ai].value == 1.0f);
        if (bi >= 0)
            ASSERT_TRUE("expand decl: b value preserved (2)",
                        g_predef_vars[bi].value == 2.0f);
        if (ci >= 0)
            ASSERT_TRUE("expand decl: c value preserved (3)",
                        g_predef_vars[ci].value == 3.0f);
        if (di >= 0)
            ASSERT_TRUE("expand decl: d value is 0",
                        g_predef_vars[di].value == 0.0f);

        /* CMD_VAR_ASSIGN slot refs must still point to the right variables */
        ASSERT_INT("expand decl: assign a slot correct", repl_state_document_cmds_mut()[1].var_idx, ai);
        ASSERT_INT("expand decl: assign b slot correct", repl_state_document_cmds_mut()[2].var_idx, bi);
        ASSERT_INT("expand decl: assign c slot correct", repl_state_document_cmds_mut()[3].var_idx, ci);
    }

    /* editor_try_commit_assign_variable - overlong formatted source is rejected with
     * "Command too long" and does not mutate repl_state_document_cmds_mut() / repl_state_document_count(). */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        int old_num_cmds = repl_state_document_count();
        g_status[0] = '\0';

        /* Build rhs padded out with parenthesized zeros so the final
         * "  n = <rhs>;" comfortably exceeds MAX_LINE_LEN (256). */
        char big_rhs[MAX_INPUT_LEN];
        int off = snprintf(big_rhs, sizeof(big_rhs), "0");
        while (off < 400 && off + 4 < (int)sizeof(big_rhs)) {
            off += snprintf(big_rhs + off, sizeof(big_rhs) - off, "+(0)");
        }

        char input[MAX_INPUT_LEN];
        ASSERT_TRUE("overlong assign: test input fits",
                    repl_format_fits(input, sizeof(input), "n = %s", big_rhs));
        set_editor_input(input);
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int r = editor_try_commit_assign_variable();
        ASSERT_INT("overlong assign: handler consumed input", r, 1);
        ASSERT_INT("overlong assign: num_cmds unchanged",
                   repl_state_document_count(), old_num_cmds);
        assert_status_contains("overlong assign: status", "Command too long");
    }

    /* Ctrl+R is config-owned; the remaining replay runtime keys stay on
     * repl_replay.c rather than the editor. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glVertex3f(1, 0, 0);");
        repl_flatten_commands(editor_state_edit_line());

        ASSERT_INT("config ctrl-r consumed",
                   glr_cfg_handle_ascii_shortcut(KEY_CTRL_R), 1);
        ASSERT_INT("config ctrl-r starts replay",
                   replay_active, 1);
        ASSERT_INT("replay space consumed",
                   replay_handle_key(' '), 1);
        ASSERT_INT("replay space pauses",
                   replay_state, REPLAY_PAUSED);
        ASSERT_INT("replay space resumes",
                   replay_handle_key(' '), 1);
        ASSERT_INT("replay resumed playing",
                   replay_state, REPLAY_PLAYING);

        replay_state = REPLAY_PAUSED;
        ASSERT_INT("replay right consumed",
                   replay_handle_special(GLUT_KEY_RIGHT), 1);
        ASSERT_INT("replay right advances one step",
                   replay_pc, 1);
        ASSERT_INT("replay left consumed",
                   replay_handle_special(GLUT_KEY_LEFT), 1);
        ASSERT_INT("replay left steps back",
                   replay_pc, 0);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        replay_state = REPLAY_PLAYING;
        glr_ctrl_router_handle_replay_key(' ');
        ASSERT_INT("replay space keeps hidden code panel",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay space still pauses through router",
                   replay_state, REPLAY_PAUSED);

        replay_state = REPLAY_PAUSED;
        replay_pc = 0;
        glr_ctrl_router_handle_replay_special(GLUT_KEY_RIGHT);
        ASSERT_INT("replay right keeps hidden code panel",
                   glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay right still advances through router",
                   replay_pc, 1);
        glr_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; glr_ctrl_sync_ui_chrome();

        /* Behavior contract: unrecognized keys that stop replay are consumed to prevent cascading downstream side-effects */
        ASSERT_INT("replay unknown key consumed on cancellation",
                   replay_handle_key('x'), 1);
        ASSERT_INT("replay unknown key stops replay",
                   replay_active, 0);
    }

    /* Replay scroll-follow should keep the replayed source command visible
     * without moving the user's editor cursor/input. */
    {
        int follow_doc_line = -1;
        int visible_lines = -1;

        glr_ctrl_reset_all(); declare_test_vars();
        for (int i = 0; i < 30; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            editor_feed_line(line);
        }
        repl_flatten_commands(editor_state_edit_line());


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
        editor_navigate_to_line(0);

        replay_active = 1;
        replay_state = REPLAY_PAUSED;
        replay_pc = repl_state_flat_program_count();
        replay_src_line = 25;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        (void)apply_code_panel_follow_for_test(&follow_doc_line,
                               &visible_lines);
        ASSERT_TRUE("replay follow helper computes target",
                    follow_doc_line >= 0);
        ASSERT_INT("code panel visible rows match rendered rows",
                   visible_lines, 9);

        g_scroll = follow_doc_line - 9;
        g_scroll_follow_cursor = 1;

        ASSERT_TRUE("replay follow helper reports visible",
                    apply_code_panel_follow_for_test(&follow_doc_line,
                                                     &visible_lines));
        ASSERT_INT("replay follow scrolls row above status bar",
                   g_scroll, follow_doc_line - visible_lines + 1);
        ASSERT_TRUE("replay follow line after scroll",
                    follow_doc_line >= g_scroll &&
                    follow_doc_line < g_scroll + visible_lines);
        ASSERT_INT("replay follow leaves edit line alone", editor_state_edit_line(), 0);
        ASSERT_STR("replay follow leaves input alone", editor_state_input().input, "glVertex3f(0, 0, 0)");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    /* Replay source focus should follow GLU tessellation vertices, not the
     * structural gluBegin/gluEnd commands that wrap them. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("gluBegin(GLU_POLYGON);");
        editor_feed_line("gluBegin(GLU_CONTOUR);");
        editor_feed_line("gluVertex(0, 0, 0);");
        editor_feed_line("gluVertex(1, 0, 0);");
        editor_feed_line("gluVertex(0, 1, 0);");
        editor_feed_line("gluEnd();");
        editor_feed_line("gluEnd();");
        repl_flatten_commands(editor_state_edit_line());

        replay_mode = REPLAY_MODE_VERTEX;
        replay_start();
        replay_advance(repl_state_flat_program_view());
        ASSERT_INT("replay vertex mode focuses first gluVertex",
                   replay_src_line, 2);
        replay_advance(repl_state_flat_program_view());
        ASSERT_INT("replay vertex mode focuses next gluVertex",
                   replay_src_line, 3);
        replay_stop();

        replay_mode = REPLAY_MODE_POLYGON;
        replay_start();
        replay_advance(repl_state_flat_program_view());
        ASSERT_INT("replay polygon mode focuses tess vertex",
                   replay_src_line, 4);
        replay_stop();
        replay_mode = REPLAY_MODE_VERTEX;
    }

    /* Replay follow rows must match whether variable expansion rows are
     * actually rendered. Collapsed replay should follow the command row. */
    {
        int collapsed_follow = -1;
        int expanded_follow = -1;
        int visible_lines = -1;

        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glVertex3f(x, y, z);");
        editor_feed_line("glVertex3f(2, 0, 0);");
        repl_flatten_commands(editor_state_edit_line());


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
        editor_navigate_to_line(0);

        replay_start();
        replay_state = REPLAY_PAUSED;
        replay_pc = repl_state_flat_program_count();
        replay_src_line = 1;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        replay_expand_args = 0;
        repl_test_publish_replay_annotations();
        (void)apply_code_panel_follow_for_test(&collapsed_follow,
                               &visible_lines);
        ASSERT_TRUE("collapsed replay follow resolves command row",
                    collapsed_follow >= 0);

        replay_expand_args = 1;
        repl_test_publish_replay_annotations();
        (void)apply_code_panel_follow_for_test(&expanded_follow,
                               &visible_lines);
        ASSERT_INT("expanded replay follows final annotation row",
                   expanded_follow, collapsed_follow + 2);

        replay_expand_args = 0;
        expanded_follow = -1;
        repl_test_publish_replay_annotations();
        (void)apply_code_panel_follow_for_test(&expanded_follow,
                               &visible_lines);
        ASSERT_INT("collapsed replay removes annotation rows from follow",
                   expanded_follow, collapsed_follow);

        replay_expand_args = 1;
        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    /* Regression: pressing Enter on the last existing command must enter
     * insert mode at editor_state_edit_line() == repl_state_document_count().  Before the fix the
     * cursor was invisible because the renderer guarded the active-input
     * row with !editor_insert_mode(), so the slot drew a dimmed placeholder instead
     * of the cursor. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");
        editor_navigate_to_line(2);
        ASSERT_INT("enter-at-last: setup edit_line", editor_state_edit_line(), 2);
        ASSERT_INT("enter-at-last: setup not inserting", editor_insert_mode(), 0);

        editor_handle_key('\r', 0, 0);

        ASSERT_INT("enter-at-last: now inserting", editor_insert_mode(), 1);
        ASSERT_INT("enter-at-last: edit_line == num_cmds", editor_state_edit_line(), repl_state_document_count());
    }

    /* Regression: scroll follow for insert-at-end must track the cursor
     * identically to overwrite-at-end.  Both were already computing the
     * same cursor_doc_line, but total_lines differed for non-empty input
     * before the code_panel_newline_rows fix. */
    {
        int follow_insert = -1, follow_overwrite = -1;
        int visible_lines = -1;

        glr_ctrl_reset_all();
        for (int i = 0; i < 20; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            editor_feed_line(line);
        }


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
        replay_active = 0;

        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(1);
        {
            EditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        apply_code_panel_follow_for_test(&follow_insert, &visible_lines);
        ASSERT_TRUE("insert-at-end cursor in visible region",
                    follow_insert >= g_scroll &&
                    follow_insert < g_scroll + visible_lines);

        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        apply_code_panel_follow_for_test(&follow_overwrite, &visible_lines);

        ASSERT_INT("insert-at-end follow matches overwrite-at-end follow",
                   follow_insert, follow_overwrite);
    }

    /* Regression: special_func (arrow keys) must set g_scroll_follow_cursor
     * so the viewport tracks cursor movement on each keypress.  Before the
     * fix only keyboard_func set the flag, so Up/Down left the view stale. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_navigate_to_line(0);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_DOWN, 0, 0);
        ASSERT_INT("down key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        /* Page Up/Down scroll the view manually and must NOT trigger cursor
         * follow - that would snap the view back to the cursor position,
         * defeating the purpose of the scroll. */
        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_PAGE_UP, 0, 0);
        ASSERT_INT("page up cancels scroll_follow_cursor", g_scroll_follow_cursor, 0);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_PAGE_DOWN, 0, 0);
        ASSERT_INT("page down cancels scroll_follow_cursor", g_scroll_follow_cursor, 0);
    }

    /* Regression: pressing Up when the cursor is below the visible area
     * must scroll the viewport to reveal it. */
    {
        int follow_doc_line = -1;
        int visible_lines = -1;

        glr_ctrl_reset_all();
        for (int i = 0; i < 20; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            editor_feed_line(line);
        }


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
        replay_active = 0;

        editor_navigate_to_line(repl_state_document_count() - 1);
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up nav: scroll_follow_cursor set", g_scroll_follow_cursor, 1);

        ASSERT_TRUE("up nav: cursor visible after follow",
                    apply_code_panel_follow_for_test(&follow_doc_line,
                                                     &visible_lines));
    }

    /* Auto-commit modified lines before keyboard navigation. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(0);
        set_editor_input("glVertex3f(9, 0, 0)");

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit valid edit: cursor moved", editor_state_edit_line(), 1);
        ASSERT_INT("nav auto-commit valid edit: first cmd type",
                   repl_state_document_cmds_mut()[0].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit valid edit: x arg",
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 9.0f) < 1e-6f);
        ASSERT_STR("nav auto-commit valid edit: input loaded next", editor_state_input().input, "glVertex3f(1, 1, 1)");
    }

    /* Invalid auto-commit reverts the edited line and still navigates. */
    {
        char old_source[MAX_LINE_LEN];
        int old_num_cmds;

        glr_ctrl_reset_all();
        editor_feed_line("glColor3f(1,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(0);
        snprintf(old_source, sizeof(old_source), "%s",
                 editor_buffer_line(0) ? editor_buffer_line(0) : "");
        old_num_cmds = repl_state_document_count();
        set_editor_input("glColor3f(1, 0");

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit invalid edit: cmd count unchanged",
                   repl_state_document_count(), old_num_cmds);
        ASSERT_STR("nav auto-commit invalid edit: source reverted",
                   editor_buffer_line(0), old_source);
        ASSERT_INT("nav auto-commit invalid edit: cursor moved", editor_state_edit_line(), 1);
        ASSERT_STR("nav auto-commit invalid edit: input loaded next", editor_state_input().input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid edit: status",
                               "Incomplete command");
    }

    /* Auto-commit new end-of-buffer input before moving away. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(repl_state_document_count());
        set_editor_input("glVertex3f(3, 3, 3)");

        editor_handle_special(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit append: cmd count", repl_state_document_count(), 3);
        ASSERT_INT("nav auto-commit append: cursor moved up", editor_state_edit_line(), 1);
        ASSERT_INT("nav auto-commit append: new cmd type",
                   repl_state_document_cmds_mut()[2].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit append: x arg",
                    fabsf(repl_state_document_cmds_mut()[2].args[0] - 3.0f) < 1e-6f);
    }

    /* Invalid new end-of-buffer input is discarded before moving away. */
    {
        int old_num_cmds;

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        old_num_cmds = repl_state_document_count();
        editor_navigate_to_line(repl_state_document_count());
        set_editor_input("glVertex3f(");

        editor_handle_special(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit invalid append: cmd count unchanged",
                   repl_state_document_count(), old_num_cmds);
        ASSERT_INT("nav auto-commit invalid append: cursor moved up",
                   editor_state_edit_line(), 1);
        ASSERT_STR("nav auto-commit invalid append: input loaded target", editor_state_input().input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid append: status",
                               "Incomplete command");
        ASSERT_STR("nav auto-commit invalid append: newline buffer discarded",
                   editor_pending_newline_buffer_mut(), "");

        editor_navigate_to_line(repl_state_document_count());
        ASSERT_STR("nav auto-commit invalid append: return to end is empty", editor_state_input().input, "");
    }

    /* Code-panel clicks use the same auto-commit path. Exercised under
     * BOTH code-focus modes: the click-to-line geometry differs (focus
     * hides the C-boilerplate chrome), so neither mode may be assumed.
     * Regression guard for b472592, which flipped CFG_DEFAULT_CODE_FOCUS
     * and silently broke this case because it relied on the old
     * default. code_focus is set explicitly so the default can move
     * again without re-breaking it. */
    for (int cf = 0; cf <= 1; cf++) {
        const char *mode = cf ? " [focus]" : " [full]";
        char lbl[96];

        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_focus = cf; glr_ctrl_sync_ui_chrome();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_feed_line("glVertex3f(2,2,2)");

        ui_state_viewport_set_size(800, 600);
        g_panel_frac = 0.5f;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
        g_scroll = code_panel_header_row_count();
        editor_navigate_to_line(0);
        set_editor_input("glVertex3f(8, 0, 0)");

        {
            int mx = CODE_MARGIN_X + 1;
            int my = code_panel_mouse_y_for_cmd(2);
            UiHit hit = code_panel_hit_test_current_snapshot(mx, my,
                                                             g_num_predef_vars);
            glr_ctrl_router_handle_code_panel_hit(hit, mx, my);
        }

        snprintf(lbl, sizeof lbl,
                 "mouse auto-commit valid edit: cursor moved%s", mode);
        ASSERT_INT(lbl, editor_state_edit_line(), 2);
        snprintf(lbl, sizeof lbl,
                 "mouse auto-commit valid edit: x arg%s", mode);
        ASSERT_TRUE(lbl,
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 8.0f) < 1e-6f);
        snprintf(lbl, sizeof lbl,
                 "mouse auto-commit valid edit: input loaded clicked%s", mode);
        ASSERT_STR(lbl, editor_state_input().input, "glVertex3f(2, 2, 2)");
    }

    /* Autocomplete Up/Down keeps selection behavior and does not navigate. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        editor_navigate_to_line(0);
        set_editor_input("glVertex3f(7, 0, 0)");
        g_ac_count = 2;
        g_ac_sel = 0;

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("autocomplete down changes selection", g_ac_sel, 1);
        ASSERT_INT("autocomplete down does not navigate", editor_state_edit_line(), 0);
        ASSERT_TRUE("autocomplete down does not commit edit",
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 0.0f) < 1e-6f);
    }

    /* editor_clear_all_cmds - clears scene including float declarations. */
    {
        int base_num_predef_vars;
        int i;
        int found_tmp_before_clear = 0;
        int found_tmp_after_clear = 0;

        glr_ctrl_reset_all();
        base_num_predef_vars = g_num_predef_vars;
        editor_feed_line("float tmp;");
        editor_feed_line("glVertex3f(1, 0, 0)");
        ASSERT_INT("clear_all: setup two cmds", repl_state_document_count(), 2);
        ASSERT_INT("clear_all: first is var decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("clear_all: decl registers one predef var",
                   g_num_predef_vars, base_num_predef_vars + 1);
        for (i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "tmp") == 0) {
                found_tmp_before_clear = 1;
                break;
            }
        }
        ASSERT_TRUE("clear_all: tmp is registered before clear", found_tmp_before_clear);

        editor_clear_all_cmds();
        ASSERT_INT("clear_all: baseline cmd count", repl_state_document_count(), 5);
        /* Editor owns the text buffer (Phase 4 of
         * docs/plans/done/edit-line-ownership.md). A clear-all has to drop
         * the editor's source text in lockstep with the command store
         * — otherwise the user sees the old lines in the code panel
         * while every commit acts on an empty cmd-store. */
        ASSERT_INT("clear_all: source_document has baseline",
                   source_document_view().line_count, 5);
        ASSERT_STR("clear_all: color material enable",
                   editor_buffer_line(0), "  glEnable(GL_COLOR_MATERIAL);");
        ASSERT_STR("clear_all: color material mode",
                   editor_buffer_line(1),
                   "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
        ASSERT_STR("clear_all: two-sided lighting",
                   editor_buffer_line(2),
                   "  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);");
        ASSERT_STR("clear_all: material specular",
                   editor_buffer_line(3),
                   "  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (GLfloat[]){0.4, 0.4, 0.4, 1});");
        ASSERT_STR("clear_all: material shininess",
                   editor_buffer_line(4),
                   "  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 30);");
        ASSERT_INT("clear_all: edit_line follows baseline", editor_state_edit_line(), 5);
        ASSERT_INT("clear_all: inserting is 0", editor_insert_mode(), 0);
        ASSERT_INT("clear_all: input is empty", editor_state_input().input[0], 0);
        ASSERT_INT("clear_all: predef var count restored",
                   g_num_predef_vars, base_num_predef_vars);
        for (i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "tmp") == 0) {
                found_tmp_after_clear = 1;
                break;
            }
        }
        ASSERT_TRUE("clear_all: tmp is no longer registered", !found_tmp_after_clear);

        editor_undo_pop_snapshot();
        ASSERT_INT("clear_all: undo restores prior document",
                   repl_state_document_count(), 2);
        ASSERT_INT("clear_all: undo restores prior predefs",
                   g_num_predef_vars, base_num_predef_vars + 1);
    }

    /* editor_reset_for_new_scene - clears scene for transient load without toast. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float tmp;");
        editor_feed_line("glVertex3f(1, 0, 0)");
        ASSERT_INT("reset_scene: setup two cmds", repl_state_document_count(), 2);

        editor_reset_for_new_scene();

        ASSERT_INT("reset_scene: num_cmds is 0", repl_state_document_count(), 0);
        ASSERT_INT("reset_scene: source_document line count is 0",
                   source_document_view().line_count, 0);
        ASSERT_INT("reset_scene: edit_line is 0", editor_state_edit_line(), 0);
        ASSERT_INT("reset_scene: inserting is 0", editor_insert_mode(), 0);
        ASSERT_INT("reset_scene: input is empty", editor_state_input().input[0], 0);
    }

    /* Regression: Ctrl+/ on a commented-out variable assignment should
     * uncomment it into a CMD_VAR_ASSIGN.  Before the fix the GL-command
     * parser would reject the stripped "x = 1;" and the user would see
     * "Cannot uncomment: not a valid command". */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("float x;");
        editor_feed_line("x = 1;");
        ASSERT_INT("uncomment setup: two cmds", repl_state_document_count(), 2);
        ASSERT_INT("uncomment setup: line 1 is assign",
                   repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);

        /* Comment the assignment with Ctrl+/. */
        editor_state_edit_line_set(1);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment assignment: type became comment",
                   repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
        ASSERT_TRUE("comment assignment: source keeps x = 1",
                    strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "x = 1") != NULL);
        assert_status_contains("comment assignment: status", "Commented");

        /* Uncomment with Ctrl+/ again.  Fallback path must rebuild the
         * CMD_VAR_ASSIGN in place, not reject with "not a valid command". */
        editor_state_edit_line_set(1);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment assignment: type back to assign",
                   repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("uncomment assignment: no error status",
                    strstr(g_status, "Cannot uncomment") == NULL);
        assert_status_contains("uncomment assignment: status", "Uncommented");
        ASSERT_TRUE("uncomment assignment: value restored",
                    repl_state_document_cmds_mut()[1].var_idx == repl_eval_find_predef_var_idx("x"));

        g_mock_modifiers = saved_mods;
    }

    /* Regression: uncommenting a line that referenced a variable must
     * preserve the variable name in the source text. Before the fix the
     * uncomment fallback wrote pl.text (the parser's numeric canonical
     * form) back into the editor buffer, so `// glVertex3f(t, 0, 0)`
     * round-tripped to `glVertex3f(0.0000, 0, 0)`. */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(t, 0, 0);");
        ASSERT_INT("uncomment vars: setup vertex committed",
                   repl_state_document_cmds_mut()[0].type, CMD_VERTEX3F);
        ASSERT_TRUE("uncomment vars: setup line keeps t",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "glVertex3f(t") != NULL);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment vars: row is comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_TRUE("comment vars: source keeps t",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "glVertex3f(t") != NULL);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment vars: type back to vertex",
                   repl_state_document_cmds_mut()[0].type, CMD_VERTEX3F);
        ASSERT_TRUE("uncomment vars: no error status",
                    strstr(g_status, "Cannot uncomment") == NULL);
        ASSERT_TRUE("uncomment vars: source still references t (not literal)",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "glVertex3f(t") != NULL);
        ASSERT_TRUE("uncomment vars: has_vars still set",
                    repl_state_document_cmds_mut()[0].has_vars == 1);

        g_mock_modifiers = saved_mods;
    }

    /* Regression: glPointParameterfv canonicalizes to a
     * `(GLfloat[]){...}` argument. Ctrl+/ must be able to comment the
     * canonical line and then parse that same canonical text when
     * uncommenting it. */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 0.2, 0, 0.15);");
        ASSERT_INT("uncomment point-param setup: committed",
                   repl_state_document_cmds_mut()[0].type, CMD_POINT_PARAMETER_FV);
        ASSERT_TRUE("uncomment point-param setup: canonical compound literal",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "(GLfloat[]){0.2, 0, 0.15}") != NULL);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment point-param: row is comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_TRUE("comment point-param: source keeps command",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "glPointParameterfv") != NULL);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment point-param: type restored",
                   repl_state_document_cmds_mut()[0].type, CMD_POINT_PARAMETER_FV);
        ASSERT_TRUE("uncomment point-param: no error status",
                    strstr(g_status, "Cannot uncomment") == NULL);
        ASSERT_TRUE("uncomment point-param: source still canonical",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "(GLfloat[]){0.2, 0, 0.15}") != NULL);

        g_mock_modifiers = saved_mods;
    }

    /* Regression (feedback P1): commenting out an unreferenced
     * float declaration must drop the variable from the predef
     * table, not just turn the row into CMD_COMMENT. Otherwise the
     * runtime keeps a name the source no longer declares. */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("float comment_x;");
        ASSERT_TRUE("comment-decl setup: x declared",
                    repl_eval_find_predef_var_idx("comment_x") >= 0);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);

        ASSERT_INT("comment unref decl: row is CMD_COMMENT",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_TRUE("comment unref decl: predef var removed",
                    repl_eval_find_predef_var_idx("comment_x") < 0);

        g_mock_modifiers = saved_mods;
    }

    /* Regression (feedback P1): commenting a referenced declaration
     * must refuse with a "still referenced" diagnostic, mirroring
     * delete-range semantics. The row stays a decl; predef-var stays
     * declared. */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("float refed_y;");
        editor_feed_line("refed_y = 5;");

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);

        ASSERT_INT("comment refed decl: row stays CMD_VAR_DECLARE",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("comment refed decl: predef var still declared",
                    repl_eval_find_predef_var_idx("refed_y") >= 0);
        assert_status_contains("comment refed decl: error wording",
                               "still referenced");

        g_mock_modifiers = saved_mods;
    }

    /* Regression (feedback P2): the delete-range reference scan must
     * skip CMD_COMMENT lines. A comment like `// p axis` is not a
     * real use of `p`; deleting `float p;` should succeed. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float p;");
        editor_feed_line("// p axis");
        ASSERT_TRUE("delete-with-comment-mention setup: p declared",
                    repl_eval_find_predef_var_idx("p") >= 0);

        editor_delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete decl with comment-mention: row gone",
                   repl_state_document_count(), 1);
        ASSERT_TRUE("delete decl with comment-mention: predef removed",
                    repl_eval_find_predef_var_idx("p") < 0);
    }

    /* Regression (feedback P2 part 2): the reference scan must also
     * stop at inline `//`. The previous fix only handled full-line
     * CMD_COMMENT rows; a regular cmd line like `glVertex3f(0,0,0)
     * // p axis` would still falsely flag `p` as referenced because
     * repl_eval_source_uses_ident walked the full text including
     * the trailing comment. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("float p;");
        editor_feed_line("glVertex3f(0,0,0); // p axis");
        ASSERT_TRUE("delete-with-inline-comment setup: p declared",
                    repl_eval_find_predef_var_idx("p") >= 0);
        ASSERT_INT("delete-with-inline-comment setup: vertex line is GL cmd",
                   repl_state_document_cmds_mut()[1].type, CMD_VERTEX3F);

        editor_delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete decl with inline-comment-mention: row gone",
                   repl_state_document_count(), 1);
        ASSERT_TRUE("delete decl with inline-comment-mention: predef removed",
                    repl_eval_find_predef_var_idx("p") < 0);
    }

    /* Regression (feedback P1, block-batch path): repl_compile_
     * toggle_comment's block-batch branch uses the same shared
     * `compile_collect_undeclare_for_range` helper as the single-
     * line path, so the unreferenced/referenced cases already
     * tested above cover the helper. A targeted block-batch test
     * with a CMD_VAR_DECLARE inside a for-block would have to
     * bypass parser auto-relocation (decls always move to the top
     * of non-decl code at commit time), and that doc layout cannot
     * arise through normal user input. The single-line tests are
     * the practical regression for P1; documenting the block path
     * inherits correctness from helper-level coverage. */

    /* Regression: uncomment still errors on genuinely unparseable lines.
     * Comment a valid line, mangle the source so neither the GL parser
     * nor the assignment fallback can rebuild it, then Ctrl+/ again. */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("mangled setup: commented", repl_state_document_cmds_mut()[0].type, CMD_COMMENT);

        /* Replace the comment body with nonsense so the re-parse fails. */
        editor_buffer_set_line(0, "// !@#$not a command$@#!");
        g_status[0] = '\0';
        editor_state_edit_line_set(0);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment genuinely bad: stays a comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        assert_status_contains("uncomment genuinely bad: error status",
                               "Cannot uncomment");

        g_mock_modifiers = saved_mods;
    }

    /* Ctrl++ / Ctrl+- step the Config "Accum passes" cycle
     * (1/2/4/8/12/16, clamped, no wrap), gated on use_accum and the effect
     * being active. The passes cycle never turns the effect off — it only
     * changes the sample count. Cycle index: 0=1 1=2 2=4 3=8 4=12 5=16. */
    {
        int saved_mods = g_mock_modifiers;
        GlrRenderState *rs = glr_state_render_mut();

        glr_ctrl_reset_all();
        rs->use_accum = 1;
        rs->accum_effect = RENDER3D_ACCUM_EFFECT_AA;
        glr_config_set(GLR_CONFIG_ACCUM_PASSES, 1);   /* index 1 == 2 passes */

        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Increment steps the cycle up and tracks accum_passes. */
        glr_ctrl_router_handle_accum_samples_key('+');
        ASSERT_INT("accum +: cycle 2->4", glr_config_get(GLR_CONFIG_ACCUM_PASSES), 2);
        ASSERT_INT("accum +: passes = 4", rs->accum_passes, 4);

        glr_ctrl_router_handle_accum_samples_key('='); /* '=' == '+' w/o shift */
        ASSERT_INT("accum =: cycle 4->8", glr_config_get(GLR_CONFIG_ACCUM_PASSES), 3);
        ASSERT_INT("accum =: passes = 8", rs->accum_passes, 8);

        /* Decrement steps back down. */
        glr_ctrl_router_handle_accum_samples_key('-');
        ASSERT_INT("accum -: cycle 8->4", glr_config_get(GLR_CONFIG_ACCUM_PASSES), 2);
        ASSERT_INT("accum -: passes = 4", rs->accum_passes, 4);

        /* Top clamp: 16 + Ctrl++ stays 16 (no wrap). */
        glr_config_set(GLR_CONFIG_ACCUM_PASSES, 5);
        glr_ctrl_router_handle_accum_samples_key('+');
        ASSERT_INT("accum +: capped at 16", glr_config_get(GLR_CONFIG_ACCUM_PASSES), 5);
        ASSERT_INT("accum +: passes = 16", rs->accum_passes, 16);

        /* Bottom clamp: index 0 == 1 pass, Ctrl+- stays there (no wrap). */
        glr_config_set(GLR_CONFIG_ACCUM_PASSES, 0);
        glr_ctrl_router_handle_accum_samples_key('-');
        ASSERT_INT("accum -: floored at 1", glr_config_get(GLR_CONFIG_ACCUM_PASSES), 0);
        ASSERT_INT("accum -: passes = 1", rs->accum_passes, 1);

        /* With the effect Off the fine-adjust is inert (consumed, no change). */
        rs->accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
        glr_config_set(GLR_CONFIG_ACCUM_PASSES, 2); /* 4 */
        glr_ctrl_router_handle_accum_samples_key('+');
        ASSERT_INT("accum +: inert while effect Off",
                   glr_config_get(GLR_CONFIG_ACCUM_PASSES), 2);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Ctrl+L (Clear All) */
    {
        int saved_mods = g_mock_modifiers;
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        ASSERT_INT("Ctrl+L setup: 1 cmd", repl_state_document_count(), 1);

        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(12, 0, 0); /* Ctrl+L is 12 */
        ASSERT_INT("Ctrl+L: restored baseline", repl_state_document_count(), 5);

        g_mock_modifiers = saved_mods;
    }

    /* Statusbar trash chip: same Clear All action as Ctrl+L. */
    {
        UiHit hit = ui_hit_none();

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");
        ASSERT_INT("trash clear setup: 1 cmd", repl_state_document_count(), 1);

        hit.kind = UI_HIT_CODE_CLEAR_ALL;
        ASSERT_INT("trash clear: consumed",
                   glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
        ASSERT_INT("trash clear: restored baseline", repl_state_document_count(), 5);
        ASSERT_STR("trash clear: status", g_status, "All commands cleared");
    }

    /* Statusbar edit chips: same editor actions as keyboard shortcuts. */
    {
        UiHit hit = ui_hit_none();
        UiRenderSnapshot snap;

        glr_ctrl_reset_all();
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar edit setup: undo disabled", snap.can_undo, 0);
        ASSERT_INT("statusbar edit setup: redo disabled", snap.can_redo, 0);

        editor_feed_line("glVertex3f(0,0,0)");
        editor_feed_line("glVertex3f(1,1,1)");
        ASSERT_INT("statusbar edit setup: 2 cmds", repl_state_document_count(), 2);

        editor_state_selection_set(0, 1);
        hit.kind = UI_HIT_CODE_COPY;
        ASSERT_INT("statusbar copy: consumed",
                   glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
        ASSERT_INT("statusbar copy: clipboard count",
                   editor_state_clipboard_count(), 2);
        ASSERT_STR("statusbar copy: first copied",
                   editor_state_clipboard_mut()->lines[0],
                   "  glVertex3f(0, 0, 0);");

        editor_state_selection_set(0, 0);
        hit.kind = UI_HIT_CODE_CUT;
        ASSERT_INT("statusbar cut: consumed",
                   glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
        ASSERT_INT("statusbar cut: document count", repl_state_document_count(), 1);
        ASSERT_INT("statusbar cut: clipboard count",
                   editor_state_clipboard_count(), 1);
        ASSERT_STR("statusbar cut: survivor",
                   editor_buffer_line(0), "  glVertex3f(1, 1, 1);");
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar cut: undo enabled", snap.can_undo, 1);
        ASSERT_INT("statusbar cut: redo disabled", snap.can_redo, 0);

        hit.kind = UI_HIT_CODE_UNDO;
        ASSERT_INT("statusbar undo: consumed",
                   glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
        ASSERT_INT("statusbar undo: restores count", repl_state_document_count(), 2);
        ASSERT_STR("statusbar undo: first restored",
                   editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar undo: undo disabled", snap.can_undo, 0);
        ASSERT_INT("statusbar undo: redo enabled", snap.can_redo, 1);

        hit.kind = UI_HIT_CODE_REDO;
        ASSERT_INT("statusbar redo: consumed",
                   glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
        ASSERT_INT("statusbar redo: reapplies cut", repl_state_document_count(), 1);
        ASSERT_STR("statusbar redo: survivor",
                   editor_buffer_line(0), "  glVertex3f(1, 1, 1);");
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("statusbar redo: undo enabled", snap.can_undo, 1);
        ASSERT_INT("statusbar redo: redo disabled", snap.can_redo, 0);
    }

    /* Extra coverage: F12 cycling with user scenes */
    {
        glr_ctrl_reset_all();
        int example_count = repl_example_count();
        if (example_count > 0) {
            /* Load example 0 */
            repl_load_example(0);

            /* Promote to user scene */
            int slot = repl_promote_example_if_needed();
            ASSERT_TRUE("F12 test: promoted example to slot", slot >= 0);

            /* Cycle from user scene 0 -> should go to example 0 */
            glr_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            ASSERT_INT("F12: user scene 0 -> example 0", repl_state_scenes().active_example_idx, 0);
            ASSERT_INT("F12: active user scene now -1", repl_active_user_scene(), -1);

            /* Cycle through all examples to reach user scenes again */
            for (int i = 0; i < example_count; i++) {
                 glr_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            }
            /* After all examples, it should hit the first used user scene slot */
            ASSERT_INT("F12: cycled back to user scene 0", repl_active_user_scene(), 0);
        }
    }

    /* Extra coverage: panel resizing via mouse motion */
    {
        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        ui_state_code_panel_mut()->resizing_panel = 1;

        editor_handle_motion(300, 500);
        ASSERT_TRUE("panel resize left: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.3f) < 1e-6f);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP; glr_ctrl_sync_ui_chrome();
        editor_handle_motion(500, 400);
        ASSERT_TRUE("panel resize top: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.4f) < 1e-6f);

        ui_state_code_panel_mut()->resizing_panel = 0;
    }

    /* Extra coverage: editor_point_on_code_panel_divider */
    {
        EditorInputDispatchEffects fx;
        int cp_x, cp_y, cp_w, cp_h;
        int win_h;

        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        win_h = ui_state_viewport().window_h;
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
        ui_state_code_panel_mut()->panel_frac = 0.3f;
        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);

        fx = editor_handle_passive_motion(cp_x + cp_w - 4, 500);
        ASSERT_TRUE("divider hover stays inherit inside code panel",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_INHERIT);
        fx = editor_handle_passive_motion(cp_x + cp_w + 4, 500);
        ASSERT_TRUE("divider hover stays inherit inside scene",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_INHERIT);
        fx = editor_handle_passive_motion(cp_x + cp_w + 3, 500);
        ASSERT_TRUE("divider hover cursor appears on divider",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_LEFT_RIGHT);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP; glr_ctrl_sync_ui_chrome();
        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        fx = editor_handle_passive_motion(500, win_h - (cp_y + 4));
        ASSERT_TRUE("top divider hover stays inherit inside code panel",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_INHERIT);
        fx = editor_handle_passive_motion(500, win_h - (cp_y - 4));
        ASSERT_TRUE("top divider hover stays inherit inside scene",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_INHERIT);
        fx = editor_handle_passive_motion(500, win_h - (cp_y + 3));
        ASSERT_TRUE("top divider hover cursor appears on divider",
                    fx.set_cursor && fx.cursor == GLUT_CURSOR_UP_DOWN);
    }

    /* Extra coverage: Audio track navigation (Ctrl+Left/Right) */
    {
        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Just trigger the routes to ensure they are covered.
         * Actual track change depends on audio assets. */
        glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_LEFT);
        glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_RIGHT);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Help overlay scrolling. Use a small viewport so
     * the active tab overflows — scroll is now clamped to the real
     * content bounds, so a tab that fully fits would (correctly) pin
     * at 0 and never increment. */
    {
        ui_state_viewport_set_size(800, 360);
        ui_state_help_mut()->visible = 1;
        editor_help_session_set_scroll(0);
        editor_help_session_set_tab(0);

        glr_ctrl_router_handle_help_scroll_special(GLUT_KEY_DOWN);
        ASSERT_INT("help scroll down", editor_help_session_scroll(), 1);

        glr_ctrl_router_handle_help_scroll_special(GLUT_KEY_UP);
        ASSERT_INT("help scroll up", editor_help_session_scroll(), 0);

        glr_ctrl_router_handle_help_tab_special(GLUT_KEY_RIGHT);
        ASSERT_INT("help tab right", editor_help_session_tab_idx(), 1);

        glr_ctrl_router_handle_help_tab_special(GLUT_KEY_LEFT);
        ASSERT_INT("help tab left", editor_help_session_tab_idx(), 0);

        glr_ctrl_router_handle_help_scroll_special(GLUT_KEY_PAGE_DOWN);
        ASSERT_INT("help page down", editor_help_session_scroll(), 5);

        glr_ctrl_router_handle_help_scroll_special(GLUT_KEY_PAGE_UP);
        ASSERT_INT("help page up", editor_help_session_scroll(), 0);

        ui_state_help_mut()->visible = 0;
        ui_state_viewport_set_size(1000, 1000);
    }

    /* Extra coverage: Escape key routes */
    {
        /* 1. Help visible */
        ui_state_help_mut()->visible = 1;
        glr_ctrl_router_handle_escape_key(27);
        ASSERT_TRUE("Esc: help closed", !ui_state_help().visible);

        /* 2. Autocomplete active */
        editor_state_autocomplete_mut()->match_count = 1;
        editor_handle_key(27, 0, 0);
        ASSERT_INT("Esc: autocomplete cleared", editor_state_autocomplete()->match_count, 0);

        /* 3. Insert mode */
        editor_insert_mode_set(1);
        editor_handle_key(27, 0, 0);
        ASSERT_TRUE("Esc: insert mode exited", !editor_insert_mode());

        /* 4. Input clear */
        set_editor_input("some text");
        editor_handle_key(27, 0, 0);
        ASSERT_INT("Esc: input cleared", editor_state_input().input_len, 0);
    }

    /* Extra coverage: timer_func logic */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0,0,0);");
        editor_feed_line("glVertex3f(1,1,1);");
        repl_flatten_commands(editor_state_edit_line());

        /* 1. Status TTL decrement */
        ui_state_status_mut()->ttl = 10;
        glr_ctrl_tick();
        ASSERT_INT("timer: status ttl decremented", ui_state_status().ttl, 9);

        /* 2. Cursor blink */
        editor_state_cursor_blink_mut()->blink_tick = 29;
        editor_state_cursor_blink_mut()->cursor_visible = 1;
        glr_ctrl_tick();
        ASSERT_INT("timer: blink tick reset", editor_state_cursor_blink().blink_tick, 0);
        ASSERT_TRUE("timer: cursor visibility toggled", !editor_state_cursor_blink().cursor_visible);

        /* 3. Replay advance */
        replay_state_mut()->active = 1;
        replay_state_mut()->state = REPLAY_PLAYING;
        replay_state_mut()->speed = 1.0f;
        replay_state_mut()->accum = 0.99f;
        /* This should trigger at least one replay_advance */
        glr_ctrl_tick();
        ASSERT_TRUE("timer: replay accum advanced", replay_state_view().accum < 0.5f);

        replay_state_mut()->active = 0;
    }

    /* Extra coverage: variable dragging via mouse */
    {
        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        variable_panel_state_mut()->visible = 1;
        editor_feed_line("float testvar = 5.0;");

        /* Variable panel is usually at bottom-right of scene.
         * Scene is to the right of code panel (0.3 frac).
         * Code panel = 0..300. Scene = 300..1000.
         * Variable panel rect is roughly [800..1000, 8..60] in GL coords.
         * GL y=8 is window y=992.
         */

        /* Attempt to hit the variable row. */
        /* ui_variable_panel_rect will place it at the right edge of the scene. */
        int px, py, pw, ph;
        vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);

        /* Click in the middle of the first row. */
        int click_x = px + pw / 2;
        int click_y = 1000 - (py + ph - VAR_PANEL_PAD_INTERNAL - VAR_TITLE_H_INTERNAL / 2);

        glr_ctrl_router_handle_variable_panel_drag_begin(GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y);
        ASSERT_TRUE("mouse: variable drag active", variable_panel_drag_active());

        glr_ctrl_router_handle_variable_panel_motion(click_x + 100, click_y);
        /* drag motion should have changed the variable value. */

        glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP);
        ASSERT_TRUE("mouse: variable drag inactive after release", !variable_panel_drag_active());
    }

    /* Extra coverage: Undo/Redo keys */
    {
        int saved_mods = g_mock_modifiers;
        glr_ctrl_reset_all();
        editor_undo_push_snapshot();
        editor_feed_line("glVertex3f(1,1,1)");
        ASSERT_INT("undo setup: 1 cmd", repl_state_document_count(), 1);

        /* Undo */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(26, 0, 0); /* Ctrl+Z */
        ASSERT_INT("Ctrl+Z: undo works", repl_state_document_count(), 0);

        /* Redo via Ctrl+Shift+Z */
        g_mock_modifiers = GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT;
        editor_handle_key(26, 0, 0);
        ASSERT_INT("Ctrl+Shift+Z: redo works", repl_state_document_count(), 1);

        /* Undo again */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(26, 0, 0);
        ASSERT_INT("undo again", repl_state_document_count(), 0);

        /* Redo via Ctrl+Y */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(25, 0, 0); /* Ctrl+Y */
        ASSERT_INT("Ctrl+Y: redo works", repl_state_document_count(), 1);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Mouse wheel */
#ifndef USE_GLUT
    {
        const int layouts[] = {
            CODE_PANEL_LAYOUT_LEFT,
            CODE_PANEL_LAYOUT_TOP,
            CODE_PANEL_LAYOUT_BOTTOM
        };
        const char *const layout_names[] = {
            "left",
            "top",
            "bottom"
        };

        for (int layout_idx = 0; layout_idx < 3; layout_idx++) {
            int cp_x, cp_y, cp_w, cp_h;
            int render3d_x, render3d_y, render3d_w, render3d_h;
            int code_x, code_y;
            int layout = layouts[layout_idx];
            char label[128];

            glr_ctrl_reset_all();
            ui_state_viewport_set_size(1000, 1000);
            editor_scroll_set(0);
            glr_state_presentation_mut()->code_panel_layout = layout; glr_ctrl_sync_ui_chrome();

            ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
            ui_layout_scene_rect(&render3d_x, &render3d_y, &render3d_w, &render3d_h);

            code_x = cp_x + cp_w / 2;
            code_y = ui_state_viewport().window_h - (cp_y + cp_h / 2);
            render3d_x += render3d_w / 2;
            render3d_y = ui_state_viewport().window_h - (render3d_y + render3d_h / 2);

            editor_handle_mousewheel(0, 1, code_x, code_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s code panel scrolled down",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), -1);

            editor_handle_mousewheel(0, -1, code_x, code_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s code panel scrolled up",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), 0);

            editor_handle_mousewheel(0, 1, render3d_x, render3d_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s scene wheel leaves code scroll unchanged",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), 0);
        }

        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        editor_scroll_set(0);
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();

        editor_handle_mousewheel(0, 1, 500, 500);
        ASSERT_INT("mousewheel: hidden layout leaves code scroll unchanged",
                   editor_scroll(), 0);
    }
#endif

    /* Extra coverage: handle_buffer_command_key_route remaining keys */
    {
        int saved_mods = g_mock_modifiers;
        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(1,1,1)");

        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Ctrl+\ (Reformat) */
        editor_handle_key(28, 0, 0);
        assert_status_contains("Ctrl+\\ reformat", "Reformatted");

        /* Ctrl+Shift+D (Dump) */
        /* We can't easily check stdout, but we trigger the branch. */
        g_mock_modifiers = GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT;
        glr_ctrl_router_handle_debug_dump_key(4);
        assert_status_contains("Ctrl+Shift+D dump", "Dumped");
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Ctrl+S (Save) */
        {
            /* Ctrl+S is now File > Save Scene. With an active *example*
             * (no named user scene, no workspace bound) it falls back
             * to the default ./output.c save — keep that deterministic
             * so the assertion below is stable. */
            repl_load_example(0);
            /* Keep the default output path inside a throwaway directory so
             * the test never touches repo-root output.c. */
            char cwd[1024];
            char temp_dir[] = "/tmp/test_repl_editor_output.XXXXXX";
            char *made_dir = mkdtemp(temp_dir);
            int have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;

            ASSERT_TRUE("mkdtemp default output dir", made_dir != NULL);
            ASSERT_TRUE("getcwd before Ctrl+S save", have_cwd);
            if (made_dir && have_cwd) {
                int cd_ok = chdir(made_dir);
                ASSERT_INT("chdir default output dir", cd_ok, 0);
                if (cd_ok == 0) {
                    glr_ctrl_router_handle_save_key(19);
                    assert_status_contains("Ctrl+S save", "Saved");
                    ASSERT_INT("default output saved in temp dir",
                               access("output.c", F_OK), 0);
                    unlink("output.c");
                    ASSERT_INT("restore cwd after Ctrl+S save",
                               chdir(cwd), 0);
                }
                rmdir(made_dir);
            }
        }

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: glr_ctrl_restore_hidden_code_panel from keys */
    {
        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();

        /* Pressing a printable key should restore it. */
        apply_editor_effects(editor_handle_key('a', 0, 0));
        ASSERT_INT("key restores hidden panel",
                   glr_state_presentation().code_panel_layout,
                   CODE_PANEL_LAYOUT_LEFT);

        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
        /* Pressing a special key should restore it. */
        apply_editor_effects(editor_handle_special(GLUT_KEY_UP, 0, 0));
        ASSERT_INT("special key restores hidden panel",
                   glr_state_presentation().code_panel_layout,
                   CODE_PANEL_LAYOUT_LEFT);
    }

    /* Extra coverage: Commenting Func/If blocks */
    {
        glr_ctrl_reset_all();
        editor_feed_line("if(1) {");
        editor_feed_line("  glVertex3f(0,0,0);");
        editor_feed_line("}");

        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment if-begin", repl_state_document_cmds_mut()[0].type, CMD_COMMENT);

        editor_state_edit_line_set(2);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment if-end", repl_state_document_cmds_mut()[2].type, CMD_COMMENT);

        g_mock_modifiers = saved_mods;
    }

    /* Block-batch toggle: Ctrl+/ on a FOR_BEGIN comments out the whole
     * for-loop in one stroke. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("for(i, 0, 3) {");
        editor_feed_line("  glVertex2f(i, 0);");
        editor_feed_line("}");

        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        editor_handle_key('/', 0, 0);

        ASSERT_INT("for-block batch: line 0 is comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_INT("for-block batch: line 1 is comment",
                   repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
        ASSERT_INT("for-block batch: line 2 is comment",
                   repl_state_document_cmds_mut()[2].type, CMD_COMMENT);
        ASSERT_TRUE("for-block batch: line 0 text has // prefix",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                           "// for") != NULL);
        ASSERT_TRUE("for-block batch: body line text has // prefix",
                    strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "",
                           "// ") != NULL);
        assert_status_contains("for-block batch: status counts lines",
                               "3 lines");

        g_mock_modifiers = saved_mods;
    }

    /* Block-batch toggle from the END side: Ctrl+/ on FOR_END walks
     * back to the matching FOR_BEGIN and comments the whole block. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("for(i, 0, 3) {");
        editor_feed_line("  glVertex2f(i, 0);");
        editor_feed_line("}");

        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        editor_state_edit_line_set(2);  /* on FOR_END */
        editor_insert_mode_set(0);
        editor_handle_key('/', 0, 0);

        ASSERT_INT("for-end back-scan: line 0 is comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_INT("for-end back-scan: line 1 is comment",
                   repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
        ASSERT_INT("for-end back-scan: line 2 is comment",
                   repl_state_document_cmds_mut()[2].type, CMD_COMMENT);

        g_mock_modifiers = saved_mods;
    }

    /* Block-batch toggle on FUNC_DEF: same shape as FOR but with
     * different head/end cmd kinds. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("func0() {");
        editor_feed_line("  glVertex3f(0,0,0);");
        editor_feed_line("}");

        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        editor_handle_key('/', 0, 0);

        ASSERT_INT("func-block batch: line 0 is comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        ASSERT_INT("func-block batch: line 1 is comment",
                   repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
        ASSERT_INT("func-block batch: line 2 is comment",
                   repl_state_document_cmds_mut()[2].type, CMD_COMMENT);

        g_mock_modifiers = saved_mods;
    }

    /* Phase 3 status framing: REPL produces the diagnostic, editor
     * wraps it with "Toggle failed: ". */
    {
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);  /* commenting out */
        ASSERT_INT("toggle framing setup: commented",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);

        editor_buffer_set_line(0, "// !@#$not a command$@#!");
        g_status[0] = '\0';
        editor_state_edit_line_set(0);
        editor_handle_key('/', 0, 0);  /* uncomment fails */
        assert_status_contains("toggle framing: editor wraps as 'Toggle failed:'",
                               "Toggle failed");
        assert_status_contains("toggle framing: REPL diagnostic preserved",
                               "Cannot uncomment");

        g_mock_modifiers = saved_mods;
    }

    /* Unset prefix: Ctrl+/ becomes a no-op. */
    {
        const char *saved_prefix = editor_line_comment_prefix();
        int saved_mods = g_mock_modifiers;

        glr_ctrl_reset_all();
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_set_line_comment_prefix(NULL);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);

        ASSERT_INT("unset prefix: line stays a regular cmd",
                   repl_state_document_cmds_mut()[0].type, CMD_VERTEX3F);
        ASSERT_INT("unset prefix: status untouched", g_status[0], '\0');

        editor_set_line_comment_prefix(saved_prefix);
        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: BOTTOM layout resizing */
    {
        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM; glr_ctrl_sync_ui_chrome();
        ui_state_code_panel_mut()->panel_frac = 0.3f;

        /* Divider should be at gl_y = 300 (y = 700). */
        editor_handle_passive_motion(500, 695);

        ui_state_code_panel_mut()->resizing_panel = 1;
        editor_handle_motion(500, 600);
        ASSERT_TRUE("panel resize bottom: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.4f) < 1e-6f);
        ui_state_code_panel_mut()->resizing_panel = 0;
    }

    /* Extra coverage: Right-click variable drag */
    {
        glr_ctrl_reset_all();
        ui_state_viewport_set_size(1000, 1000);
        variable_panel_state_mut()->visible = 1;
        editor_feed_line("float testvar = 5.0;");

        int px, py, pw, ph;
        vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
        int click_x = px + pw / 2;
        int click_y = 1000 - (py + ph - VAR_PANEL_PAD_INTERNAL - VAR_TITLE_H_INTERNAL / 2);

        glr_ctrl_router_handle_variable_panel_drag_begin(GLUT_RIGHT_BUTTON, GLUT_DOWN, click_x, click_y);
        ASSERT_TRUE("mouse: right-click variable drag active", variable_panel_drag_active());
        glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP);
    }

    /* Bugfix: modifying glcommands inside a function inside a glbegin scope breaks the indentation */
    {
        glr_ctrl_reset_all();
        editor_feed_line("func0 {");
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_feed_line("glVertex3f(0, sin(t), 0);");
        editor_feed_line("glEnd();");
        editor_feed_line("}");

        /* Now edit line 2: glVertex3f(0, sin(t), 0); */
        editor_state_edit_line_set(2);
        editor_insert_mode_set(0);
        editor_load_line_to_input(2);

        /* Modify it: change sin(t) to cos(t) and commit via Enter key */
        editor_input_set_text("glVertex3f(0, cos(t), 0)");
        editor_handle_key('\r', 0, 0);

        /* Verify that the indentation is NOT broken (should be 6 spaces) */
        const char *line_text = editor_buffer_line(2);
        ASSERT_TRUE("indentedCos", line_text != NULL);
        ASSERT_STR("indentedCosText", line_text, "      glVertex3f(0, cos(t), 0);");
    }

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
