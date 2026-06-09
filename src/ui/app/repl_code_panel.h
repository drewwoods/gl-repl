/*
 * repl_code_panel.h - REPL-specific adapter over the generic text panel.
 *
 * Owns the REPL-aware code-panel row model, layout helpers, render bridge,
 * and hit-test bridge. The generic renderer/hit-tester live in
 * src/ui/text_panel.{c,h}; this module translates REPL/editor state into that
 * generic surface.
 */
#ifndef UI_REPL_CODE_PANEL_H
#define UI_REPL_CODE_PANEL_H

#include "config.h"
#include "repl/command_spec.h"
#include "ui/app/hit.h"
#include "ui/app/snapshot.h"

struct UiCodePanelOutput;

/* -------------------------------------------------------------------------
 * Per-kind argument syntax coloring
 *
 * The code panel colors a command row by its class (CmdSyntaxCategory).
 * On top of that, argument *tokens* — literals, constants, and variables —
 * are drawn as dimmer/desaturated tiers of that same class hue (derived
 * from the class color only, never an independent hue), so the class stays
 * recognizable and a line never clashes with itself. Keyword / function-call
 * names / operators / punctuation produce no span and keep the class color.
 *
 * The classifier is a flat left-to-right lexer over the display text. It is
 * exposed so tests can assert classification independently of rendering.
 * ---------------------------------------------------------------------- */

typedef enum {
    REPL_SYNTAX_LITERAL = 0,   /* numeric literal or quoted string run */
    REPL_SYNTAX_CONSTANT,      /* PI, TAU, or a GL_* enum constant */
    REPL_SYNTAX_VARIABLE,      /* declared / predef / scratch / local var */
    REPL_SYNTAX_KIND_COUNT
} UiSyntaxKind;

typedef struct {
    int            start;  /* char offset into the line text */
    int            len;    /* span length in chars */
    UiSyntaxKind kind;
} UiSyntaxSpan;

/* Classify argument tokens in a single display line. Writes up to
 * max_spans ordered, non-overlapping spans and returns the count written.
 * Keyword / function-call names / operators / punctuation produce no span.
 * Pure: no global state, safe to call from tests. */
int ui_repl_code_panel_classify_syntax(const UiRenderSnapshot *snap,
                                       const char *text,
                                       UiSyntaxSpan *out, int max_spans);

/* Final RGB (0..1) for a token kind: a brightness/saturation tier of the
 * command's class color (same hue, dimmer). Exposed for the contrast
 * regression test. */
void ui_repl_code_panel_syntax_kind_rgb(UiSyntaxKind kind,
                                         CmdSyntaxCategory category,
                                         float out_rgb[3]);

typedef struct {
    int panel_w;
    int text_x;
    int cp_h;
    int visible_lines;
    int header_rows;
    int footer_rows;
    int total_lines;
    int cursor_doc_line;
    int follow_doc_line;
    int cmd_main_rows[MAX_COMMANDS];
    int replay_extra_rows[MAX_COMMANDS];
} UiReplCodePanelLayout;

int ui_repl_code_panel_compute_text_x(const UiRenderSnapshot *snap);

int        ui_repl_code_panel_visible_lines_for_height(int cp_h,
                                                       int top_chrome_h);

void ui_repl_code_panel_build_layout(const UiRenderSnapshot *snap,
                                     UiReplCodePanelLayout *layout,
                                     int panel_w, int text_x, int cp_h);
int  ui_repl_code_panel_target_for_doc_line(const UiRenderSnapshot *snap,
                                            int doc_line,
                                            const UiReplCodePanelLayout *layout,
                                            int *out_target,
                                            int *out_on_insert_line,
                                            int *out_row_offset);

void ui_repl_code_panel_render_with_chrome(const UiRenderSnapshot *snap,
                               struct UiCodePanelOutput *out);
UiHit ui_repl_code_panel_hit_test(const UiRenderSnapshot *snap,
                                  int mx, int my);

int  ui_repl_code_panel_input_row_y(const UiRenderSnapshot *snap,
                                    float *out_py);
int  ui_repl_code_panel_input_row_has_color_swatch(
         const UiRenderSnapshot *snap);

/* Test-only: clear the row builder cache shared by render and hit-test.
 * Lets tests start from a deterministic empty cache so a regression that
 * fails to refresh the cache after a snapshot change becomes observable. */
void ui_repl_code_panel_invalidate_row_cache_for_test(void);

/* Test-only: after a render call populates the row buffer, return the
 * left-marker color of the row representing `source_line_idx`. Returns 1
 * on hit (writing `out_active` / `out_rgba`), 0 if no matching row. Used
 * by the marker-priority cascade regression test. */
int ui_repl_code_panel_row_marker_for_test(int source_line_idx,
                                           int *out_active,
                                           float out_rgba[4]);

/* Test-only: after a render or hit-test populates the row builder, return the
 * left auxiliary label for the row representing `source_line_idx`. Returns 1
 * when a row was found, including rows whose aux label is empty. */
int ui_repl_code_panel_row_aux_label_for_test(int source_line_idx,
                                              char out_label[8]);

#endif /* UI_REPL_CODE_PANEL_H */
