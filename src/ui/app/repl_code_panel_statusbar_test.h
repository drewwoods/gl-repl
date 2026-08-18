/*
 * Test-only accessors for the code-panel status strip. Production
 * render/hit-test includes repl_code_panel_statusbar.h only.
 */
#ifndef UI_REPL_CODE_PANEL_STATUSBAR_TEST_H
#define UI_REPL_CODE_PANEL_STATUSBAR_TEST_H

#include "ui/app/snapshot.h"

/* 1 if the overlay-family hover band would draw for the snapshot's
 * current pointer, writing the union span of the host group. */
int repl_code_panel_statusbar_group_band_for_test(
    const UiRenderSnapshot *snap, int sx, int sy, int sw, int sh,
    int *out_x0, int *out_x1);

/* Table cull_rank for the item whose hit is `hit`, or -1. */
int repl_code_panel_statusbar_cull_rank_for_test(int hit);

#endif /* UI_REPL_CODE_PANEL_STATUSBAR_TEST_H */
