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
#include "ui/core/text_panel.h"   /* UiTextPanelColorSegment (test accessor) */

struct UiCodePanelOutput;

/* -------------------------------------------------------------------------
 * Per-kind argument syntax coloring
 *
 * The code panel colors a command row by its class (CmdSyntaxCategory).
 * On top of that, argument *tokens* - literals, constants, and variables -
 * are drawn as dimmer/desaturated tiers of that same class hue (derived
 * from the class color only, never an independent hue), so the class stays
 * recognizable and a line never clashes with itself. Keyword / function-call
 * names / operators / punctuation produce no span and keep the class color.
 *
 * The classifier is a flat left-to-right lexer over the display text. It is
 * exposed so tests can assert classification independently of rendering.
 * ---------------------------------------------------------------------- */

#define SYNTAX_HIGHLIGHT_LIST(X) \
    X(OFF, "Off") \
    X(ON, "On") \
    X(ON_SHADOW, "On+Shadow")

#define SYNTAX_HIGHLIGHT_NAME_ENTRY(name, str) [SYNTAX_HIGHLIGHT_##name] = str,

typedef enum SyntaxHighlightMode {
#define SYNTAX_HIGHLIGHT_ENUM_ENTRY(name, str) SYNTAX_HIGHLIGHT_##name,
    SYNTAX_HIGHLIGHT_LIST(SYNTAX_HIGHLIGHT_ENUM_ENTRY)
#undef SYNTAX_HIGHLIGHT_ENUM_ENTRY
    SYNTAX_HIGHLIGHT_COUNT
} SyntaxHighlightMode;

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

/* Infer the syntax category of a generated, non-editable C row. Recognizes
 * REPL GL calls plus the extra C/GLUT scaffold vocabulary used by the expanded
 * code view. Exposed for focused classifier tests. */
CmdSyntaxCategory ui_repl_code_panel_generated_category(const char *text);

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
    int cmd_main_rows[MAX_EDITOR_COMMANDS];
    int replay_extra_rows[MAX_EDITOR_COMMANDS];
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

/* Scroll row a scrollbar drag should land on: my is the pointer's window y
 * (GLUT coordinates, origin top-left) and grab_dy the offset the press
 * recorded in UiHit.item_idx, so the thumb keeps the same grip through the
 * drag. Returns the clamped scroll row, or -1 when the panel currently shows
 * no scrollbar. */
int  ui_repl_code_panel_scrollbar_scroll_at(const UiRenderSnapshot *snap,
                                            int my, int grab_dy);

/* Resolve a visible source row to a point in its code-text hit area, in GLUT
 * mouse coordinates (origin top-left). Handles committed, live-input, insert,
 * and trailing blank rows through the same row model as render/hit-test. */
int  ui_repl_code_panel_source_line_point(const UiRenderSnapshot *snap,
                                          int source_line_idx,
                                          int *out_mx,
                                          int *out_my);

int  ui_repl_code_panel_input_row_y(const UiRenderSnapshot *snap,
                                    float *out_py);
int  ui_repl_code_panel_input_row_has_color_swatch(
         const UiRenderSnapshot *snap);

/* Test-only: clear the row builder cache shared by render and hit-test.
 * Lets tests start from a deterministic empty cache so a regression that
 * fails to refresh the cache after a snapshot change becomes observable. */
/* Resolve the code panel's left-margin gutter label for each of `count`
 * document line indices (parallel arrays; -1 where no row represents the
 * line). The label is not the document index - with code focus off the
 * derived-C chrome rows shift it - so any UI quoting a line number to the
 * user must resolve it here rather than printing source_line_idx + 1.
 * Batched: a cache miss rebuilds the whole row list. */
void ui_repl_code_panel_gutter_labels_for_lines(const UiRenderSnapshot *snap,
                                                const int *source_lines,
                                                int *out_labels,
                                                int count);

/* Format a PATH breadcrumb from the structured snapshot. Writes a
 * NUL-terminated string of at most out_sz-1 chars, eliding the middle of
 * a deep chain so the row stays inside MAX_VIRTUAL_LINE_TEXT. Pure: no
 * REPL parsing. Returns characters written, excluding NUL. */
int ui_repl_code_panel_format_replay_path(const ReplReplayPathSnapshot *path,
                                          char *out, int out_sz);

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

/* Test-only companion to ui_repl_code_panel_row_aux_label_for_test: report
 * the row's base text alpha and its aux-label alpha (both 1.0 when the row
 * opted out of translucency). Lets the auto-normal dim be asserted as a
 * relationship against an ordinary row rather than against the literal
 * tuning constants. Returns 1 when a row was found. */
int ui_repl_code_panel_row_alphas_for_test(int source_line_idx,
                                           float *out_text_alpha,
                                           float *out_aux_alpha);

/* Test-only: build rows without GL rendering and return the muted base color
 * plus syntax-span count for the first static row containing `needle`. */
int ui_repl_code_panel_generated_row_style_for_test(
        const UiRenderSnapshot *snap, const char *needle,
        float out_rgb[3], int *out_segment_count);

/* Test-only: build rows without GL rendering and copy the per-character color
 * segments of the committed (TEXT) or active edit (INPUT) row for
 * `source_line_idx` into `out` (up to `max_out`). Returns the row's total
 * segment count (may exceed `max_out`), or -1 if no such row exists. Lets the
 * glPushAttrib mask-token colouring be asserted against the row's drawn text. */
int ui_repl_code_panel_row_color_segments_for_test(
        const UiRenderSnapshot *snap, int source_line_idx,
        UiTextPanelColorSegment *out, int max_out);

#endif /* UI_REPL_CODE_PANEL_H */
