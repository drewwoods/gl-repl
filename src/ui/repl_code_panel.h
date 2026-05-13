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
#include "ui/hit.h"
#include "ui/snapshot.h"
#include "ui/text_layout.h"

struct UiCodePanelOutput;

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

CodeLayout ui_repl_code_panel_text_layout(int panel_w, int first_x);
int        ui_repl_code_panel_active_indent_chars(void);
int        ui_repl_code_panel_visible_lines_for_height(int cp_h);

void ui_repl_code_panel_build_layout(UiReplCodePanelLayout *layout,
                                     int panel_w, int text_x, int cp_h);
void ui_repl_code_panel_apply_follow_scroll(const UiReplCodePanelLayout *layout);
int  ui_repl_code_panel_target_for_doc_line(int doc_line,
                                            const UiReplCodePanelLayout *layout,
                                            int *out_target,
                                            int *out_on_insert_line,
                                            int *out_row_offset);

void ui_repl_code_panel_render(const UiRenderSnapshot *snap,
                               struct UiCodePanelOutput *out);
UiHit ui_repl_code_panel_hit_test(int mx, int my);

int  ui_repl_code_panel_apply_scroll_follow_for_test(int show_vertex_indices,
                                                     int *out_follow_doc_line,
                                                     int *out_visible_lines);

#endif /* UI_REPL_CODE_PANEL_H */