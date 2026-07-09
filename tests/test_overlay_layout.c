/*
 * test_overlay_layout.c - floating scene-overlay panel layout engine.
 *
 * Exercises the pure solve pass (stacking order, replay band lift, spill
 * column, no-overlap invariant, degenerate clamps) plus the eased-position
 * state (snap-in, glide, hidden-panel re-snap, unticked fallback) from
 * src/ui/app/overlay_layout.c. The inputs collector is covered through the
 * live-view tests in test_ui / test_glr_ctrl; here inputs are hand-built so
 * the solver is pinned independently of panel size formulas.
 */
#include "ui/app/overlay_layout.h"
#include "support/test_harness.h"
#include <stdio.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond)   TEST_ASSERT_TRUE(&g_h, label, cond)
#define AI(label, got, exp) TEST_ASSERT_INT(&g_h, label, got, exp)

/* Mirror the engine's stack constants (pinned by these tests). */
enum {
    BASE_Y       = 8,
    RIGHT_MARGIN = 8,
    STACK_GAP    = 8,
    COL_GAP      = 8,
    EDGE_PAD     = 4,
};

/* A 700x600 scene at x=300 (code panel on the left) over a 22px status
 * bar — the shape the app actually produces. */
static UiOverlayLayoutIn base_inputs(void) {
    UiOverlayLayoutIn in;
    in.render3d_x = 300; in.render3d_y = 0;
    in.render3d_w = 700; in.render3d_h = 600;
    in.bottom_inset = 22;
    in.band_h = 0;
    in.prefer_top_on_overflow = 0;
    in.panels[UI_OVERLAY_PANEL_VARIABLE] =
        (UiOverlayPanelReq){ 1, 250, 92 };   /* ~3 var rows */
    in.panels[UI_OVERLAY_PANEL_FPS] =
        (UiOverlayPanelReq){ 0, 300, 142 };  /* hidden in the base shape */
    in.panels[UI_OVERLAY_PANEL_PROFILE] =
        (UiOverlayPanelReq){ 1, 384, 200 };
    in.panels[UI_OVERLAY_PANEL_HISTOGRAM] =
        (UiOverlayPanelReq){ 0, 384, 230 };  /* hidden in the base shape */
    in.panels[UI_OVERLAY_PANEL_MEMORY] =
        (UiOverlayPanelReq){ 1, 220, 150 };
    return in;
}

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

int main(void) {
    /* --- Solo variable panel: the historical bottom-right anchor. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_PROFILE].visible = 0;
        in.panels[UI_OVERLAY_PANEL_MEMORY].visible = 0;
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("solo var x = scene right - margin - w",
           x[UI_OVERLAY_PANEL_VARIABLE], 300 + 700 - RIGHT_MARGIN - 250);
        AI("solo var y = inset + base",
           y[UI_OVERLAY_PANEL_VARIABLE], 22 + BASE_Y);
        /* Hidden panels still get a landing position at the next slot. */
        AI("hidden profile parks above var",
           y[UI_OVERLAY_PANEL_PROFILE],
           y[UI_OVERLAY_PANEL_VARIABLE] + 92 + STACK_GAP);
    }

    /* --- Var hidden: profile takes the bottom slot (old behavior). --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_VARIABLE].visible = 0;
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("prof takes bottom slot when var hidden",
           y[UI_OVERLAY_PANEL_PROFILE], 22 + BASE_Y);
        AI("prof right-aligned",
           x[UI_OVERLAY_PANEL_PROFILE], 300 + 700 - RIGHT_MARGIN - 384);
        AI("mem stacks above prof",
           y[UI_OVERLAY_PANEL_MEMORY], 22 + BASE_Y + 200 + STACK_GAP);
    }

    /* --- All three visible: bottom-up stack, no overlap. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("stack: prof above var",
           y[UI_OVERLAY_PANEL_PROFILE], (22 + BASE_Y) + 92 + STACK_GAP);
        AI("stack: mem above prof",
           y[UI_OVERLAY_PANEL_MEMORY],
           (22 + BASE_Y) + 92 + STACK_GAP + 200 + STACK_GAP);
        for (int a = 0; a < UI_OVERLAY_PANEL_COUNT; a++)
            for (int b = a + 1; b < UI_OVERLAY_PANEL_COUNT; b++) {
                /* Hidden panels share the next-slot landing position by
                 * design; only visible pairs must be disjoint. */
                if (!in.panels[a].visible || !in.panels[b].visible)
                    continue;
                char label[64];
                snprintf(label, sizeof(label),
                         "stack: no overlap %d vs %d", a, b);
                AT(label, !rects_overlap(
                       x[a], y[a], in.panels[a].w, in.panels[a].h,
                       x[b], y[b], in.panels[b].w, in.panels[b].h));
            }
    }

    /* --- FPS plot panel joins the stack between variable and profile. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_FPS].visible = 1;
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("fps: above var",
           y[UI_OVERLAY_PANEL_FPS], (22 + BASE_Y) + 92 + STACK_GAP);
        AI("fps: prof above fps",
           y[UI_OVERLAY_PANEL_PROFILE],
           (22 + BASE_Y) + 92 + STACK_GAP + 142 + STACK_GAP);
        for (int a = 0; a < UI_OVERLAY_PANEL_COUNT; a++)
            for (int b = a + 1; b < UI_OVERLAY_PANEL_COUNT; b++) {
                if (!in.panels[a].visible || !in.panels[b].visible)
                    continue;
                char label[64];
                snprintf(label, sizeof(label),
                         "fps: no overlap %d vs %d", a, b);
                AT(label, !rects_overlap(
                       x[a], y[a], in.panels[a].w, in.panels[a].h,
                       x[b], y[b], in.panels[b].w, in.panels[b].h));
            }
    }

    /* --- Every compute-profile panel on at once: the histogram joins the
     * stack above the listing and, in a scene this short, spills into a
     * fresh column rather than overlapping anything. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_FPS].visible = 1;
        in.panels[UI_OVERLAY_PANEL_HISTOGRAM].visible = 1;
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AT("hist: spills left of the listing it would not fit above",
           x[UI_OVERLAY_PANEL_HISTOGRAM] < x[UI_OVERLAY_PANEL_PROFILE]);
        AT("hist: spilled column starts at the stack base",
           y[UI_OVERLAY_PANEL_HISTOGRAM] == 22 + BASE_Y);
        for (int a = 0; a < UI_OVERLAY_PANEL_COUNT; a++)
            for (int b = a + 1; b < UI_OVERLAY_PANEL_COUNT; b++) {
                if (!in.panels[a].visible || !in.panels[b].visible)
                    continue;
                char label[64];
                snprintf(label, sizeof(label),
                         "hist: no overlap %d vs %d", a, b);
                AT(label, !rects_overlap(
                       x[a], y[a], in.panels[a].w, in.panels[a].h,
                       x[b], y[b], in.panels[b].w, in.panels[b].h));
            }
    }

    /* --- Replay band lifts the whole stack. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.band_h = 84;   /* HUD bottom (74) + 10px clearance */
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("band lifts var", y[UI_OVERLAY_PANEL_VARIABLE], 22 + 84);
        AI("band lifts prof",
           y[UI_OVERLAY_PANEL_PROFILE], 22 + 84 + 92 + STACK_GAP);
    }

    /* --- Out of vertical room: third panel spills to a left column. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_VARIABLE].h = 300;
        in.panels[UI_OVERLAY_PANEL_PROFILE].h = 200;
        in.panels[UI_OVERLAY_PANEL_MEMORY].h = 150;
        /* var(300) + gap + prof(200) fits in 600; + mem(150) does not. */
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("spill: mem restarts at column base",
           y[UI_OVERLAY_PANEL_MEMORY], 22 + BASE_Y);
        /* New column sits left of the widest panel placed so far (prof). */
        AI("spill: mem column left of prof",
           x[UI_OVERLAY_PANEL_MEMORY],
           x[UI_OVERLAY_PANEL_PROFILE] - COL_GAP - 220);
        for (int a = 0; a < UI_OVERLAY_PANEL_COUNT; a++)
            for (int b = a + 1; b < UI_OVERLAY_PANEL_COUNT; b++) {
                if (!in.panels[a].visible || !in.panels[b].visible)
                    continue;
                char label[64];
                snprintf(label, sizeof(label),
                         "spill: no overlap %d vs %d", a, b);
                AT(label, !rects_overlap(
                       x[a], y[a], in.panels[a].w, in.panels[a].h,
                       x[b], y[b], in.panels[b].w, in.panels[b].h));
            }
    }

    /* --- Degenerate: one panel taller than the scene clamps, the next
     *     spills instead of overlapping. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.panels[UI_OVERLAY_PANEL_VARIABLE].h = 700;  /* > render3d_h */
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        AI("degenerate: oversized panel parks at the inset",
           y[UI_OVERLAY_PANEL_VARIABLE], 22 + EDGE_PAD);
        AI("degenerate: next panel spills to a new column",
           y[UI_OVERLAY_PANEL_PROFILE], 22 + BASE_Y);
        AT("degenerate: spilled column is to the left",
           x[UI_OVERLAY_PANEL_PROFILE] < x[UI_OVERLAY_PANEL_VARIABLE]);

        in.prefer_top_on_overflow = 1;
        ui_overlay_layout_solve(&in, x, y);
        AI("degenerate: prefer-top parks against the scene top",
           y[UI_OVERLAY_PANEL_VARIABLE], 0 + 600 - 700 - EDGE_PAD);
    }

    /* --- Top-docked code panel: the scene rect is the short band below it
     *     (render3d_y = 0, render3d_h = window_h - code_panel_h). The stack must
     *     stay inside that band — spilling sideways rather than climbing
     *     into the code panel above. --- */
    {
        UiOverlayLayoutIn in = base_inputs();
        in.render3d_x = 0; in.render3d_y = 0;
        in.render3d_w = 1000; in.render3d_h = 280;   /* short band below the panel */
        in.prefer_top_on_overflow = 1;          /* code panel at top */
        int x[UI_OVERLAY_PANEL_COUNT], y[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, x, y);
        /* var(92) fits column 1; prof(200) and mem(150) each spill. */
        for (int id = 0; id < UI_OVERLAY_PANEL_COUNT; id++) {
            char label[64];
            snprintf(label, sizeof(label),
                     "top-dock: panel %d below the code panel", id);
            AT(label, y[id] + in.panels[id].h <= in.render3d_y + in.render3d_h);
            snprintf(label, sizeof(label),
                     "top-dock: panel %d inside scene x", id);
            AT(label, x[id] >= in.render3d_x &&
                      x[id] + in.panels[id].w <= in.render3d_x + in.render3d_w);
        }
        AI("top-dock: prof spills to its own column",
           y[UI_OVERLAY_PANEL_PROFILE], 22 + BASE_Y);
        AT("top-dock: prof column left of var",
           x[UI_OVERLAY_PANEL_PROFILE] < x[UI_OVERLAY_PANEL_VARIABLE]);
        AT("top-dock: mem column left of prof",
           x[UI_OVERLAY_PANEL_MEMORY] + in.panels[UI_OVERLAY_PANEL_MEMORY].w
               <= x[UI_OVERLAY_PANEL_PROFILE]);
    }

    /* --- Eased state: unticked queries return pure targets; ticks snap
     *     then glide; hiding re-snaps. --- */
    {
        ui_overlay_layout_reset();
        UiOverlayLayoutIn in = base_inputs();
        int tx[UI_OVERLAY_PANEL_COUNT], ty[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&in, tx, ty);

        int px, py;
        ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_VARIABLE, &px, &py);
        AI("unticked pos.x = target", px, tx[UI_OVERLAY_PANEL_VARIABLE]);
        AI("unticked pos.y = target", py, ty[UI_OVERLAY_PANEL_VARIABLE]);
        AI("unticked band is 0", ui_overlay_layout_last_band_h(), 0);

        /* First tick snaps to target (no glide from a stale origin). */
        ui_overlay_layout_tick(&in);
        ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_VARIABLE, &px, &py);
        AI("first tick snaps y", py, ty[UI_OVERLAY_PANEL_VARIABLE]);

        /* Raise the band: one tick moves part-way (glide), many ticks
         * converge on the lifted target. */
        UiOverlayLayoutIn lifted = in;
        lifted.band_h = 84;
        int ly[UI_OVERLAY_PANEL_COUNT], lx[UI_OVERLAY_PANEL_COUNT];
        ui_overlay_layout_solve(&lifted, lx, ly);
        ui_overlay_layout_tick(&lifted);
        ui_overlay_layout_panel_pos(&lifted, UI_OVERLAY_PANEL_VARIABLE,
                                    &px, &py);
        AT("glide: one tick moves up",
           py > ty[UI_OVERLAY_PANEL_VARIABLE]);
        AT("glide: one tick is not there yet",
           py < ly[UI_OVERLAY_PANEL_VARIABLE]);
        AI("tick records band", ui_overlay_layout_last_band_h(), 84);
        for (int i = 0; i < 200; i++) ui_overlay_layout_tick(&lifted);
        ui_overlay_layout_panel_pos(&lifted, UI_OVERLAY_PANEL_VARIABLE,
                                    &px, &py);
        AI("glide converges on lifted target",
           py, ly[UI_OVERLAY_PANEL_VARIABLE]);

        /* Hide then re-show: the panel forgets its eased spot and snaps. */
        UiOverlayLayoutIn hidden = in;
        hidden.panels[UI_OVERLAY_PANEL_VARIABLE].visible = 0;
        ui_overlay_layout_tick(&hidden);
        ui_overlay_layout_tick(&in);
        ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_VARIABLE, &px, &py);
        AI("re-show snaps straight to target",
           py, ty[UI_OVERLAY_PANEL_VARIABLE]);

        ui_overlay_layout_reset();
        AI("reset clears the band", ui_overlay_layout_last_band_h(), 0);
    }

    return test_harness_report(&g_h, "overlay_layout");
}
