# Scene tab strip for the code panel

## Context

Today the only way to move between active scenes is the **Scene menu** dropdown
(load example / load user scene / rename). User scenes and the active example
already exist as first-class state, but switching between them is buried in a
menu. We want a VS Code-style **tab strip** at the top of the code panel — one
tab per active scene, showing its name — so switching is one click.

The behavioral model of user scenes and examples does **not** change. Tabs are a
*pure view* of existing state plus a second way to invoke the *existing*
scene-switch code paths. Intended outcome: the tab strip reflects exactly what
the Scene menu and F12 already do, with click-to-switch and double-click-to-rename.

## Locked decisions (from clarifying Q&A)

1. Tab strip sits **directly below the existing menu bar**, above the code rows.
2. Tab set is **derived each frame from existing state — no new persistent model**:
   one tab per occupied user-scene slot (slot order) + exactly one "example
   tab" iff `active_example_idx >= 0`. Active tab = active user slot, else the
   example tab.
3. **Always show** the strip, even with a single tab. (Fresh start is **one
   user tab** — the home slot, *not* an example tab: `core.c:790-791` runs
   `repl_load_example(0)` then `repl_scenes_activate_home_slot()`, which saves
   slot 0 and sets `g_active_user_scene = 0`, `g_example_idx = -1`
   (`scenes.c:763-765`). An example tab appears only once an example is
   explicitly loaded via Scene-menu/F12 while a user scene exists.)
4. Click a tab = switch, **reusing the exact Scene-menu load paths**. Switching
   to a user-scene tab clears the active example (existing behavior) → the
   example tab disappears; this needs *zero* extra code. Selecting another
   example just relabels the single example tab.
5. Double-click a **user-scene** tab → switch to it, then trigger the
   **existing status-bar inline rename** (`editor_inline_rename_begin`). No
   in-tab text editing. Example tabs are not renamable. Scene menu stays as-is.

## Design overview

A new `ui_*` view module mirrors `src/ui/menu_bar.{c,h}` exactly: reads only the
frozen snapshot, returns a `UiHit`, mutates nothing. The controller bakes the
tab list into the per-frame snapshot (precedent: `ReplImportExportView` flat
by-value view). All routing/mutation stays in `src/app/glr_ctrl.c`.

## Implementation

### 1. New module `src/ui/scene_tabs.{c,h}`

Public API (header contract paragraph styled like `menu_bar.h`):

```c
void  ui_scene_tabs_render(const UiRenderSnapshot *snap);
UiHit ui_scene_tabs_hit_test(const UiRenderSnapshot *snap, int mx, int my);
int   ui_scene_tabs_band_h(const UiRenderSnapshot *snap);  /* TAB_STRIP_H or 0 */
```

Private `scene_tabs_rects()` mirrors `menubar_rects()` (`src/ui/menu_bar.c:248`):
`ui_layout_code_panel_rect()` → `panel_top = cp_y + cp_h`; per-tab `x[]`/`w[]`.
Render, hit-test, and `band_h` all derive from this one helper +
`snap->scene_tabs.count`, so they stay consistent by construction.
Includes: `metrics.h`, `layout.h`, `gl_2d.h` — and **no `state.h`**: unlike
`menu_bar.c` (which reads live `ui_state_viewport()` for legacy reasons), this
module is strictly snapshot-only and takes the viewport from `snap` (see §5).
No `repl/*` or `editor/*`.

### 2. Snapshot view struct — zero persistent state

`ReplSceneRuntimeState` has no per-scene names, so the list is assembled in the
controller and frozen. Add to `src/ui/snapshot.h`:

```c
enum { UI_SCENE_TAB_NAME_MAX = USER_SCENE_NAME_MAX };   /* 64 */
enum { UI_SCENE_TAB_CAP = 9 };                          /* MAX_USER_SCENES + 1 */
typedef enum { UI_SCENE_TAB_USER = 0, UI_SCENE_TAB_EXAMPLE } UiSceneTabKind;
typedef struct { char name[UI_SCENE_TAB_NAME_MAX]; UiSceneTabKind kind;
                 int slot; int active; } UiSceneTab;
typedef struct { UiSceneTab tabs[UI_SCENE_TAB_CAP]; int count; int active_idx; }
        UiSceneTabList;
```

Add `UiSceneTabList scene_tabs;` to `UiRenderSnapshot`. Guard the cap with
`_Static_assert(UI_SCENE_TAB_CAP >= MAX_USER_SCENES + 1, ...)` in `glr_ctrl.c`
(which already includes `repl/core.h`; keep it out of `snapshot.h`).

Build helper in `glr_ctrl_build_ui_snapshot()` (`src/app/glr_ctrl.c`, just
after `snap->user_scene_active_idx = repl_active_user_scene();`):
- Iterate `slot` 0..`MAX_USER_SCENES`, skip `!repl_user_scene_slot_used(slot)`
  — reproduces the dense order of `glr_scene_menu_slot_for_dense_index()`
  (`src/app/glr_actions.c:287`) so tab order == Scene-menu order == F12 order.
  Name via `repl_user_scene_name(slot)`; `active = (slot == active_slot)`.
- If `repl_state_scenes().active_example_idx >= 0`, append one example tab,
  name via **`repl_example_name(idx)`** (`src/repl/core.h` — controller-layer
  spelling, matches `glr_actions.c`); `active = (active_slot < 0)`.
- `active_idx` = display index whose `active` is set, else -1.

Lives only in the per-frame snapshot (cleared by the existing `memset`).

### 3. Geometry threading — HIGHEST RISK

The top reserve is hardcoded in **five** spots in `src/ui/text_panel.c`, not
three — and they do **not** all use the same expression:
- `2 * LINE_H` form: render origin `line_y` (`:713`), hit-test origin
  `line_y_start` (`:759`), visible-lines `available` inside
  `ui_text_panel_visible_lines_for_height` (`:668`).
- `LINE_H` form, inside `text_panel_draw_scrollbar()`: `bar_h` (`:560`) and
  `thumb_y` (`:566`). These currently start flush under the menu bar; without
  threading, the scrollbar thumb overlaps the new tab band.

An off-by-one in any of these maps clicks to the wrong source rows or floats
the scrollbar over the tabs.

Fix — one carrier, compiler-enforced:
- Add generic `int top_chrome_h;` to `UiTextPanelSnapshot` (`src/ui/text_panel.h`).
  Subtract `snap->top_chrome_h` at the render origin, the hit-test origin,
  **and both scrollbar expressions** (`bar_h -= top_chrome_h`,
  `thumb_y -= top_chrome_h`).
- Make `ui_text_panel_visible_lines_for_height()` take `top_chrome_h` as a
  **required new parameter** (break-the-build forcing function) and subtract it.
- Adapter wires it in **one place**: `repl_code_panel_init_builder()`
  (`src/ui/repl_code_panel.c`) sets `.top_chrome_h = ui_scene_tabs_band_h(snap)`.
  Audit `ui_repl_code_panel_build_layout()` and the layout helper near
  `repl_code_panel.c:324` for any other "rows that fit" computation.
- Tests that call the helper pass `0` (behavior unchanged). `text_panel.c`
  stays REPL-free (generic int, passes `check-ui-text-panel-pure`).

Strip rect: `panel_top = cp_y + cp_h`; `menu_by = panel_top - CODE_MARGIN_Y -
LINE_H` (unchanged menu bar); `tab_bh = TAB_STRIP_H`; `tab_by = menu_by -
tab_bh`. **`TAB_STRIP_H` must equal `LINE_H` (18), not 20** — add
`#define TAB_STRIP_H LINE_H` to `src/ui/metrics.h` next to `STATUSBAR_H`.
Rationale: the row count is integer division by `LINE_H`
(`return available / LINE_H + 1`, `:671`). Only when the reserve is an exact
multiple of `LINE_H` does adding the band drop the visible-row count by
*exactly one* (`(a - L)/L + 1 == a/L`); a 20px band drops it by one *or two*
depending on panel height — breaking the lockstep invariant and the §3 test.
Equal-to-`LINE_H` also keeps the band visually uniform with the menu bar.
Per-tab x from `cp_x + CODE_MARGIN_X`, `FONT_SMALL_W = 8`. Overflow v1 policy:
equal-share shrink + **hard label truncation, no horizontal scroll** (max 9
tabs fit any normal panel; idiomatic — `menu_bar.c` hard-limits via
`max_chars`). `TAB_PAD_X`/`TAB_MIN_W`/`TAB_MAX_W` stay file-private in
`scene_tabs.c`.

### 4. Rendering

Insert `ui_scene_tabs_render(snap);` in the `gl2d_begin/end` block of
`ui_repl_code_panel_render()` (`src/ui/repl_code_panel.c:1342-1354`),
**immediately after** `ui_menu_bar_render(snap);` (it draws into the caller's
open block — no own `gl2d_begin/end`, same as the menu bar there).

Visibility gating mirrors `menu_bar.c`: early-return if `cp_w<=0 || cp_h<=0`
(HIDDEN) or `count<=0`. TOP/BOTTOM/LEFT need no special-casing — the strip is
relative to `panel_top`+menu bar, which `layout.c` already places per mode.

Draw order: (1) dark background band (`#1d1d1d`, matches menu strip); (2) per
tab L→R clipped at the right edge: inactive label `#d8d8d8`; active = 2px
accent-green top rule (`UI_ACCENT_GREEN_*` from `metrics.h`) + bright `#ffffff`
label; optional hover `#2a2a2a` computed from `snap->pointer` (window coords,
not y-flipped, like the menu bar); 1px left separator. **Dirty dot: deferred**
— no reachable per-scene modified flag exists (editor dirty state is global);
document the deferral in `scene_tabs.h`.

### 5. Hit-test

Add `UI_HIT_CODE_PANEL_TAB` to `src/ui/hit.h` (after `UI_HIT_CODE_PANEL_CHROME`)
plus a field-semantics comment entry: `item_idx` = tab display index;
`local_x/local_y` = sub-strip offset.

`ui_scene_tabs_hit_test`: y-flip from the **snapshot** —
`int ry = snap->viewport.window_h - my;` (NOT live `ui_state_viewport()`; the
module is snapshot-only per the design contract, which is also why it needs no
`state.h`). Reject if outside band / count==0; loop tabs
`mx ∈ [x[i], x[i]+w[i])` → `UI_HIT_CODE_PANEL_TAB, item_idx=i`.

Integrate in `ui_panels_hit_test()` (`src/ui/panels.c:108-160`) **between the
menu-bar block and the variable-panel block**. Essential — otherwise the strip
band falls through the y-shifted code-panel hit-test to `UI_HIT_SCENE`. Add
`#include "scene_tabs.h"` to `panels.c`.

### 6. Routing — `route_scene_tab_hit()` in `glr_ctrl.c`

Tab double-click statics next to `g_last_text_press_*` (`src/app/glr_ctrl.c`
≈`:2236`): `g_last_tab_press_ms`, `g_last_tab_press_idx`. Reuse
`DOUBLE_CLICK_MS` and `current_double_click_ms()` (test-clock seam
`glr_ctrl_router_set_double_click_clock_for_test` — also wipe the new statics
there for the rename-trigger test).

`route_scene_tab_hit(const UiHit *hit)` near `route_menu_item_hit`:
- Resolve display idx → slot by **reusing
  `glr_scene_menu_slot_for_dense_index(idx)`** (`src/app/glr_actions.c:287`):
  `>=0` → user tab; `-1` with `active_example_idx>=0` → example tab; else
  stale → consumed no-op.
- **User-scene tab — exact path from `glr_actions.c:500-503`:**
  `editor_undo_clear(); if (repl_load_user_scene_idx(slot))
  load_line_to_input(repl_state_edit_line());` — the live user-scene Scene-menu
  path does **not** call `editor_reset_transients()`; do not add it. No-op if
  `slot == repl_active_user_scene()`.
- **Example tab — exact path from `glr_actions.c:468-471`:**
  `editor_reset_transients(); editor_undo_clear();
  repl_load_example(active_example_idx);`. No-op if already active. Never
  rename an example.
- Double-click on a user-scene tab (after switching, or if already active):
  `editor_inline_rename_begin(slot)`. Hard-modal capture and the status-bar
  prompt are already wired (`glr_ctrl_keyboard` → `editor_input_rename_capture_key`,
  `editor_inline_rename_active()`), so no extra plumbing.

Add `case UI_HIT_CODE_PANEL_TAB:` to the switch in
`glr_ctrl_router_handle_code_panel_hit()` (after `UI_HIT_CODE_PANEL_CHROME`).
The existing dropdown-dismiss preamble correctly closes an open menu first.

## Files

**New:** `src/ui/scene_tabs.h`, `src/ui/scene_tabs.c`, `tests/test_ui_scene_tabs.c`.

**Modified:**
- `src/ui/snapshot.h` — `UiSceneTabList` view + `scene_tabs` field
- `src/ui/hit.h` — `UI_HIT_CODE_PANEL_TAB` + comment block
- `src/ui/metrics.h` — `TAB_STRIP_H`
- `src/ui/text_panel.h` / `src/ui/text_panel.c` — `top_chrome_h` carrier +
  helper signature; **5** reserve sites (3 code-row + 2 scrollbar) — highest risk
- `src/ui/repl_code_panel.c` — include, set `.top_chrome_h`, call
  `ui_scene_tabs_render`, layout-helper audit
- `src/ui/panels.c` — include + hit-test insert
- `src/app/glr_ctrl.c` — snapshot build helper, static_assert, double-click
  statics, `route_scene_tab_hit`, dispatch case
- `tests/test_ui_text_panel.c`, `tests/test_repl_code_panel_layout.c` — pass
  `0` for the new helper arg
- `scripts/allowlists/ui-renderers-signature.txt` — add `ui_scene_tabs_render`
- `Makefile` — add `src/ui/scene_tabs.c` to `SRCS` and `CORE_TEST_SRCS`,
  `src/ui/scene_tabs.h` to the header list, `test_ui_scene_tabs` to
  `TEST_BINS`. Objects derive via the existing `$(SRCS:.c=.o)` patsubst, so
  both real-GL and `USE_GL_STUBS=1` paths are covered with no per-file rule.

## Edge cases

Startup → **1 active user tab** (home slot `USER_SCENE_HOME_NAME`), no example
tab. Single-file import → 1 user tab (home slot, via `core.c:779`
`repl_scenes_activate_home_slot`). Workspace import → user tab(s) in slot
order. Example tab appears only after an explicit Scene-menu/F12 example load
while a user scene exists. F12 → list rebuilt each frame, follows
automatically. Auto-promote mid-edit (editing a loaded example) → example tab
replaced by the new user tab next frame. Switch user tab while example active →
example cleared, its tab vanishes (locked, zero code). HIDDEN → early-return,
`top_chrome_h==0`. TOP/BOTTOM → relative to panel_top, follows. Scrollbar
thumb → clears the tab band (both scrollbar reserve sites threaded). Long name
→ hard-truncated. Too-narrow panel → off-panel tabs clipped. Click active tab →
no-op (still records press time so a 2nd click double-clicks). Double-click
example tab → switches only, no rename. Stale click → resolved against live
`repl_*`, consumed no-op.

## Verification

Build: `make sample`, `make sample USE_GL_STUBS=1`, `make test`,
`make check-state-ownership`. Boundary guards expected to pass:
`check-ui-text-panel-pure` (generic int only), `check-ui-returns-hits-only`,
`check-ui-renderer-signatures` (after allowlist add),
`check-ui-panels-no-mutators`, `check-views-by-value-snapshot`/`-flat`.

New `tests/test_ui_scene_tabs.c` (mirror `tests/test_ui_menu_bar.c` harness).
Note: a bare `repl_*_reset()` leaves `active_user_scene=-1`,
`active_example_idx=-1`, no used slots → **0 tabs**; every derivation case must
seed state explicitly (occupy slots / set `active_example_idx`), not rely on a
default.
1. **Tab-list derivation** — startup shape (home slot 0 used,
   `active_example_idx=-1`) → exactly 1 *user* tab, active, no example tab;
   home slot + example loaded (`active_example_idx>=0`, `active_user_scene=-1`)
   → 2 tabs (user + example), example active, correct kinds/order; multi-scene
   → N user tabs in slot order.
2. **Geometry hit-mapping** — center of each tab → correct `item_idx`; outside
   band / right edge → `UI_HIT_NONE`.
3. **band_h lockstep** — `==TAB_STRIP_H` (==`LINE_H`) with tabs, `==0` HIDDEN;
   assert `visible_lines(H,flags,TAB_STRIP_H)` is **exactly one** row fewer
   than `(H,flags,0)`, swept over several `H` — valid precisely because
   `TAB_STRIP_H==LINE_H` (see §3); the sweep guards a future
   `TAB_STRIP_H != LINE_H` regression.
4. In `tests/test_glr_ctrl.c` (already links the router + clock seam):
   deterministic clock, two tab hits on the same non-active tab within
   `DOUBLE_CLICK_MS` → `editor_inline_rename_active()` + slot switched; single
   click → rename inactive; double-click example tab → rename inactive.

Pre-finish audit (the `2*LINE_H` pattern alone is insufficient — the scrollbar
reserves via `- LINE_H`): inspect **every** top-reserve expression in
`src/ui/text_panel.c` and `src/ui/repl_code_panel.c` for a `top_chrome_h` term:
`grep -rn "LINE_H\|CODE_MARGIN_Y\|ui_text_panel_visible_lines_for_height" src/ui/text_panel.c src/ui/repl_code_panel.c`.

Manual e2e: fresh → 1 user tab (`USER_SCENE_HOME_NAME`), no example tab; load
an example via Scene menu/F12 → 2 tabs (home + example, example active); click
a row-1 text line and confirm it lands on row 1, and the scrollbar thumb clears
the tab band (proves `top_chrome_h` threading across all 5 sites); click the
home tab → example tab gone; edit a loaded example → promotes, example tab
replaced by the new user tab; double-click a user tab → status-bar rename
prompt + rename; double-click example tab → no prompt; F12 → tabs track the
cycle; test narrow panel and TOP/BOTTOM/HIDDEN layout modes.

## Review corrections (incorporated)

Four review findings, all re-verified against source and folded into the
sections above:

- **[P1] Startup is a user tab, not an example tab.** `core.c:790-791` →
  `repl_scenes_activate_home_slot()` (`scenes.c:755-766`) sets
  `g_active_user_scene=0`, `g_example_idx=-1`. Corrected decision #3, edge
  cases, test #1, and the manual e2e; added the "bare reset → 0 tabs, must
  seed" caveat. The build logic in §2 is unchanged (it already derives the
  list correctly — only the prose describing the startup outcome was wrong).
- **[P1] Scrollbar is a 4th/5th reserve site.** `text_panel_draw_scrollbar()`
  `bar_h` (`:560`) and `thumb_y` (`:566`) reserve via `- LINE_H`. Threaded
  `top_chrome_h` there too; corrected the site count (3 → 5) and broadened the
  audit (the old `2*LINE_H` grep would have missed the `- LINE_H` form).
- **[P2] `TAB_STRIP_H` must equal `LINE_H`.** Integer-division row count
  (`:671`) drops by exactly one only when the reserve is a multiple of
  `LINE_H`. Changed `20` → `LINE_H`; the exact-one-fewer lockstep test stays
  valid and now sweeps `H`.
- **[P2] Hit-test uses the snapshot, not live state.**
  `snap->viewport.window_h - my`; the module no longer includes `state.h`
  (purer than the `menu_bar.c` legacy it was mirroring).

## Scope / effort

Moderate — ~half a day to a day for someone familiar with the tree. Code
volume is small and mostly a structural copy of `menu_bar.c`; the cost is
*care*, concentrated in the vertical-reserve threading (small diff, high
blast radius) and the tests. Risk is low: no new persistent state, no new
ownership crossings, every reused function verified to exist, all boundary
guards checked. The reserve lockstep is de-risked by the compiler-enforced
mandatory helper parameter + the lockstep assertion in test #3.
