/*
 * repl_code_panel_statusbar.h - Table-driven code-panel status strip.
 *
 * k_statusbar_items[] is the single source of visual order, hit kind,
 * tooltip, and paint. statusbar_solve() places every item once; draw,
 * hit-test, and tooltip walk that result so a reorder is a table-row
 * move, not a seven-site edit.
 */
#ifndef UI_REPL_CODE_PANEL_STATUSBAR_H
#define UI_REPL_CODE_PANEL_STATUSBAR_H

#include "ui/app/snapshot.h"
#include "ui/core/text_panel.h"

void repl_code_panel_statusbar_draw(const UiRenderSnapshot *snap,
                                    const UiTextPanelRect *slot);
int  repl_code_panel_statusbar_hit_kind(const UiRenderSnapshot *snap,
                                        const UiTextPanelSnapshot *text_snap,
                                        int mx, int gl_y);

#endif /* UI_REPL_CODE_PANEL_STATUSBAR_H */
