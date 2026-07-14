/*
 * overlay_layout.h - Layout engine for the floating scene-overlay panels.
 *
 * Single placement policy for the free-floating panels that share the 3D
 * scene viewport: the variable slider panel and the CPU-profile and memory
 * panels. Panels stack bottom-up in one right-edge column above the status
 * bar and an optional reserved bottom band (the replay HUD), spilling into
 * a fresh column to the left when the stack runs out of vertical room — so
 * panels never overlap each other or the HUD by construction.
 *
 * Two layers:
 *   - ui_overlay_layout_solve() is pure: inputs in, target positions out.
 *   - A small eased-position state, advanced once per frame by the
 *     controller via ui_overlay_layout_tick(), makes every panel glide
 *     toward its target — the generalization of the old variable-panel-only
 *     replay_lift_px easing. ui_overlay_layout_panel_pos() returns the
 *     eased position and falls back to the pure target while the state has
 *     never been ticked (headless/test paths that query geometry without
 *     rendering frames).
 *
 * Anchor-bound popups (autocomplete, color picker, menu dropdowns, status
 * history) are deliberately NOT routed through this engine — they stay
 * glued to their anchors.
 */
#ifndef UI_APP_OVERLAY_LAYOUT_H
#define UI_APP_OVERLAY_LAYOUT_H

typedef enum {
    UI_OVERLAY_PANEL_VARIABLE = 0,  /* variable slider panel (stack bottom) */
    UI_OVERLAY_PANEL_FPS,           /* FPS plot panel (Compute profile level 1) */
    UI_OVERLAY_PANEL_PROFILE,       /* compute-profile section listing */
    UI_OVERLAY_PANEL_HISTOGRAM,     /* compute-profile section histograms */
    UI_OVERLAY_PANEL_MEMORY,        /* memory (RSS) panel */
    UI_OVERLAY_PANEL_COUNT
} UiOverlayPanelId;

/* One panel's placement request. Invisible panels still get a position
 * (where they would land) but take no slot in the stack. */
typedef struct {
    int visible;
    int w, h;
} UiOverlayPanelReq;

/* Solver inputs. Flat by-value data only (check-views-flat). */
typedef struct {
    int render3d_x, render3d_y, render3d_w, render3d_h;
    int bottom_inset;            /* status bar height along the scene bottom */
    int band_h;                  /* reserved bottom band above the inset
                                  * (replay HUD + clearance), 0 when absent */
    int prefer_top_on_overflow;  /* degenerate clamp: park against the scene
                                  * top (code panel at top) vs. the inset */
    UiOverlayPanelReq panels[UI_OVERLAY_PANEL_COUNT];
} UiOverlayLayoutIn;

/* Collect solver inputs from the live layout (scene rect, status bar,
 * code-panel side) plus the caller-supplied panel states. `profile_mode` /
 * `memory_mode` take UiProfilePanelMode / UiMemoryPanelMode values (passed
 * as int so this header stays include-free); `profile_collapsed_sections`
 * is the DETAILS-tree presentation bitmask. Panel sizes are resolved via
 * the panels' own size queries. `band_h` is the replay-HUD reservation —
 * the controller computes it per tick; other callers forward
 * ui_overlay_layout_last_band_h(). */
UiOverlayLayoutIn ui_overlay_layout_inputs(int var_visible, int var_count,
                                           int profile_mode,
                                           unsigned long long profile_collapsed_sections,
                                           int memory_mode,
                                           int band_h);

/* The band height most recently passed to ui_overlay_layout_tick(); 0
 * before the first tick. Lets view builders reuse the controller's replay
 * reservation without reading replay state themselves. */
int ui_overlay_layout_last_band_h(void);

/* Pure solve: fill each panel's target bottom-left position (GL window
 * coords, y up). No state reads or writes. */
void ui_overlay_layout_solve(const UiOverlayLayoutIn *in,
                             int out_x[UI_OVERLAY_PANEL_COUNT],
                             int out_y[UI_OVERLAY_PANEL_COUNT]);

/* Advance the eased positions one frame toward the solve targets for `in`.
 * Newly visible (or never-placed) panels snap straight to their target;
 * hidden panels forget their position so they re-snap on reappearing.
 * Called once per frame by the controller. */
void ui_overlay_layout_tick(const UiOverlayLayoutIn *in);

/* Resolved position for one panel: the eased position when ticked, else
 * the pure solve target for `in`. */
void ui_overlay_layout_panel_pos(const UiOverlayLayoutIn *in,
                                 UiOverlayPanelId id, int *x, int *y);

/* Drop all eased positions (next tick / query starts from targets). */
void ui_overlay_layout_reset(void);

#endif /* UI_APP_OVERLAY_LAYOUT_H */
