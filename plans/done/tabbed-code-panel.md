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
   example tab if present; in the transient/neither-active state, `active_idx =
   -1`.
3. **Always show** the strip when the derived tab set is non-empty, even with
   a single tab. (Fresh start is **one user tab** — the home slot, *not* an
   example tab: `core.c:790-791` runs `repl_load_example(0)` then
   `repl_scenes_activate_home_slot()`, which saves slot 0 and sets
   `g_active_user_scene = 0`, `g_example_idx = -1` (`scenes.c:763-765`). An
   example tab appears only once an example is explicitly loaded via
   Scene-menu/F12 while a user scene exists.) If the derived count is zero
   (only possible in reset/test/transient states with no occupied slots), the
   band height is `0` and the strip is not rendered.
4. Click a tab = switch, **reusing the exact Scene-menu load paths**. Switching
   to a user-scene tab clears the active example (existing behavior) → the
   example tab disappears; this needs *zero* extra code. Selecting another
   example just relabels the single example tab.
5. Double-click a **user-scene** tab → switch to it, then trigger the
   **existing status-bar inline rename** (`editor_inline_rename_begin`). No
   in-tab text editing. Example tabs are not renamable. Scene menu stays as-is.
6. **Transient / "neither active" documents get no synthetic tab.** A live
   document can exist while both `active_user_scene == -1` and
   `active_example_idx == -1` — Scene → New (`glr_actions.c`: clears the
   active example, `editor_clear_all_cmds()`, activates no slot) and tutorial
   start (`repl_scenes_enter_transient_scene()`, `tutorial.c:286`). Decision:
   the strip stays a *pure view of scene state* — it lists the occupied
   user slots (+ the example tab iff `active_example_idx >= 0`) and sets
   `active_idx = -1` (no highlighted tab) in this state. **No transient/
   "Untitled" synthetic tab** (same rationale as the deferred dirty-dot: no
   per-document identity exists to name or route). If the home slot is still
   occupied (the common case after New) its tab simply shows unhighlighted;
   if zero slots are occupied the derived count is `0`, the band height is
   `0`, and nothing is rendered. Switching back via F12 / Scene-menu / clicking
   a listed tab re-establishes an active tab through the existing paths — zero
   extra routing.

## Design overview

A new `ui_*` view module mirrors `src/ui/menu_bar.{c,h}` exactly: takes all
tab/scene **content** from the frozen snapshot, derives panel **geometry**
through the shared `ui_layout_code_panel_rect()` helper (exactly as
`menu_bar.c` does — see §1/§5), returns a `UiHit`, mutates nothing, and never
touches REPL/editor state. The controller bakes the tab list into the
per-frame snapshot (precedent: `ReplImportExportView` flat by-value view). All
routing/mutation stays in `src/app/glr_ctrl.c`.

> **Line references are indicative, not authoritative.** This branch has
> already shifted `src/ui/text_panel.{c,h}` and `src/ui/repl_code_panel.c`
> (color-segment cap raise + the generic fake-bold primitive). Treat every
> `file:line` below as a search hint and re-derive the offset at
> implementation. What binds is the *structural* claim (site counts,
> expression forms, function names), not the numbers. The §Verification
> grep is therefore a **mandatory, blocking** step, not a courtesy audit.

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

Includes: `metrics.h`, `layout.h`, `gl_2d.h` — and **no `state.h`**, no
`repl/*`, no `editor/*`. Purity here is *precise, not absolute*: the module
takes tab/scene content (names, kinds, active idx, pointer) and the hit-test
y-flip from `snap`, and never reads or mutates REPL/editor state — that is what
the boundary guards actually enforce (`check-ui-returns-hits-only`,
`check-ui-panels-no-mutators`, controller-boundary). Panel *geometry* still
comes through the shared `ui_layout_code_panel_rect()` (declared in `layout.h`;
`layout.c` forward-declares the `ui_state_*` chrome getters itself, so callers
need no `state.h`). That helper reads live UiState chrome (viewport size,
`panel_frac`, layout mode) and is the **single source of truth** for panel
geometry used by *all* panel chrome including `menu_bar.c`; duplicating its
mode/`panel_frac` math into snapshot fields is explicitly rejected
(reuse-over-duplication; drift hazard). Consistency invariant:
`snap->viewport`/`snap->code_panel` are captured from the same live UiState at
frame start and UiState chrome does not mutate mid-frame, so the snapshot
y-flip and the live-derived rect agree within a frame — the same reason
`menu_bar.c` (layout-helper rect + viewport flip) is correct and passes the
guards. This module is strictly *purer* than `menu_bar.c` (snapshot flip, not
live `ui_state_viewport()`).

### 2. Snapshot view struct — zero persistent state

`ReplSceneRuntimeState` has no per-scene names, so the list is assembled in the
controller and frozen. Add to `src/ui/snapshot.h`:

```c
/* Both constants are hardcoded here, NOT derived from repl macros —
 * snapshot.h must stay free of repl/* includes (UI-layer purity). The
 * equivalence to the repl source-of-truth is enforced by _Static_assert
 * in glr_ctrl.c, which already includes repl/core.h. */
enum { UI_SCENE_TAB_NAME_MAX = 64 };   /* == USER_SCENE_NAME_MAX */
enum { UI_SCENE_TAB_CAP = 9 };         /* == MAX_USER_SCENES + 1 */
typedef enum { UI_SCENE_TAB_USER = 0, UI_SCENE_TAB_EXAMPLE } UiSceneTabKind;
typedef struct { char name[UI_SCENE_TAB_NAME_MAX]; UiSceneTabKind kind;
                 int slot; int active; } UiSceneTab;
typedef struct { UiSceneTab tabs[UI_SCENE_TAB_CAP]; int count; int active_idx; }
        UiSceneTabList;
```

Add `UiSceneTabList scene_tabs;` to `UiRenderSnapshot`. Guard **both**
constants in `glr_ctrl.c` (which already includes `repl/core.h`; keep both
asserts out of `snapshot.h`):
`_Static_assert(UI_SCENE_TAB_CAP >= MAX_USER_SCENES + 1, ...)` and
`_Static_assert(UI_SCENE_TAB_NAME_MAX == USER_SCENE_NAME_MAX, ...)`. (The
earlier draft wrote `UI_SCENE_TAB_NAME_MAX = USER_SCENE_NAME_MAX` directly in
`snapshot.h`, which would have pulled a repl macro into the UI layer —
exactly the dependency the hardcoded-cap treatment exists to avoid. Both
constants get the same hardcode-plus-assert treatment for consistency.)

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
- Adapter wires the **render/hit** path in `repl_code_panel_init_builder()`
  (`src/ui/repl_code_panel.c`) — sets `.top_chrome_h =
  ui_scene_tabs_band_h(snap)` on the `UiTextPanelSnapshot`.
- Tests that call the generic helper pass `0` (behavior unchanged).
  `text_panel.c` stays REPL-free (generic int, passes
  `check-ui-text-panel-pure`).

**Scroll / paging parity — mandatory consumer audit (this was the
under-scoped risk, not the 5 render sites).** The band must subtract from
*every* "rows that fit" computation, not just the renderer/hit-test, or
scroll-clamp and cursor-follow drift exactly one row from what is drawn
(bottom row never revealed; Page-Down overshoots by one). The public
wrapper `ui_repl_code_panel_visible_lines_for_height(int cp_h)` has **no
`snap`** and cannot see `scene_tabs.count`, so it cannot self-correct.
Resolution: give it the band height explicitly — either add an `int
top_chrome_h` parameter (break-the-build, mirrors the generic helper) or
add a `snap`-taking variant — and migrate callers. Enumerate, thread, and
unit-cover each "rows that fit" consumer (re-derive the line numbers):

- `ui_repl_code_panel_build_layout()` — has `snap`; derive the band there
  and feed `visible_lines`, `cursor_doc_line`, and follow-scroll from the
  band-adjusted count.
- the follow-scroll / row-count helpers around `repl_code_panel.c` ≈`:324`
  (`repl_code_panel_*_rows`, `*_follow_doc_line_from_layout`).
- `glr_ctrl.c`: scroll clamp, Page-Up/Down, and
  `glr_ctrl_apply_code_panel_follow_scroll()` — any `visible_lines` use.
- `editor_*` scroll / `scroll_follow_cursor` math, **iff** it reaches the
  wrapper (verify; if it derives independently it needs the band too).
- tests (`test_repl_code_panel_layout`, `test_repl_code_panel_document`)
  pass `0`.

The §Verification grep is the forcing function for this audit and is
**blocking**, not "pre-finish".

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
`ui_repl_code_panel_render()` (`src/ui/repl_code_panel.c`, the
`ui_menu_bar_render` block), drawing into the caller's open block — no own
`gl2d_begin/end`, same as the menu bar there.

**Open-dropdown z-order (must resolve — closed-state-only reasoning is
insufficient).** The Scene/Config dropdown expands *downward from the menu
bar*, straight through the new band. Today the dropdown is not drawn inside
`ui_menu_bar_render()`; it is the later controller overlay pass
`ui_menu_bar_render_example_dropdown()` after `ui_panels_render_code_panel()`.
Keep that later pass unchanged so the dropdown overpaints both the tab strip
and closed menu bar. Inside the code-panel `gl2d_begin/end` block, draw
`ui_scene_tabs_render(snap)` **before** `ui_menu_bar_render(snap)` so the
closed menu row remains the topmost chrome in its own band. If a future change
folds dropdown rendering back into `ui_menu_bar_render()`, this same order
still lets the dropdown draw after the strip. The matching hit-precedence
requirement is in §5.

Visibility gating mirrors `menu_bar.c`: early-return if `cp_w<=0 || cp_h<=0`
(HIDDEN) or `count<=0` (zero derived tabs means no band, consistent with
decision #3/#6). TOP/BOTTOM/LEFT need no special-casing — the strip is relative
to `panel_top`+menu bar, which `layout.c` already places per mode.

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

`ui_scene_tabs_hit_test`: y-flip from the snapshot —
`int ry = snap->viewport.window_h - my;` (snapshot, not live
`ui_state_viewport()` — see §1 for why this is consistent and needs no
`state.h`). Then **consume the entire band**, not just the tab rects:
- outside the band (`mx ∉ [cp_x, cp_x+cp_w)` or `ry ∉ [tab_by, tab_by+tab_bh)`)
  or `count==0` → `UI_HIT_NONE` (let other handlers run);
- on a tab rect (`mx ∈ [x[i], x[i]+w[i])`) → `UI_HIT_CODE_PANEL_TAB,
  item_idx=i`;
- **in-band but off-tab** (gaps, right of the last tab) →
  `UI_HIT_CODE_PANEL_CHROME` (inert; coordinates only, no line/row payload).

The CHROME branch is load-bearing, not cosmetic — verified fall-through if the
band returned `UI_HIT_NONE` off-tab: `ui_panels_hit_test` continues to
`ui_repl_code_panel_hit_test` (`panels.c:151-154`); `ui_text_panel_hit_test`
does *not* reject the point (the band is inside `[cp_y, cp_y+cp_h]`,
`text_panel.c:754-756`) and at the band's lower pixels `row_from_top`
integer-truncates to `0` → a spurious **row-0** `UI_HIT_CODE_TEXT`; for higher
band pixels the text panel returns NONE but then `repl_code_panel.c:1414-1430`
returns `UI_HIT_CODE_GUTTER`/`UI_HIT_CODE_TEXT` (the `gl_y < cp_y + STATUSBAR_H`
chrome guard is false near the panel top). Net without the CHROME branch:
blank-strip clicks move the cursor / start a selection. (The earlier "falls
through to `UI_HIT_SCENE`" note was wrong: `UI_HIT_SCENE` is only the final
fallback *after* the code-panel handler — `panels.c:157` — which the band
never reaches.)

Integrate in `ui_panels_hit_test()` **between the menu-bar block and the
variable-panel block** (re-derive lines), so the band is claimed before
`ui_repl_code_panel_hit_test` runs. Add `#include "scene_tabs.h"` to
`panels.c`.

**Dropdown hit-precedence (matches the §4 z-order fix).** An open
Scene/Config dropdown's rect overlaps the band. Verify the menu-bar hit
block consumes the *open dropdown's full list rect* (not just the bar) and
runs **before** the band block — so a click on dropdown rows that visually
sit over the strip routes to the menu, not a tab. If the menu-bar block only
hit-tests the closed bar, the band block must early-out when a dropdown is
open (same menu-open snapshot flag as the §4 render gate). State which is
true after inspecting `panels.c` + `menu_bar.c`; do not assume.

**CHROME router inertness (verify, don't assert).** §6 routes in-band
off-tab clicks as `UI_HIT_CODE_PANEL_CHROME`. That kind today only services
the statusbar slot; the band is a *new* coordinate region for it. Before
relying on it, confirm `glr_ctrl_router_handle_code_panel_hit()`'s
`UI_HIT_CODE_PANEL_CHROME` case is genuinely side-effect-free at band
coordinates — specifically that it cannot start a panel-divider drag or move
the cursor/selection there. If it is not inert, the band's off-tab branch
must return `UI_HIT_NONE` *and* the §5 consume-the-whole-band guarantee must
instead be enforced by an explicit early dismissal in the router.

### 6. Routing — `route_scene_tab_hit()` in `glr_ctrl.c`

Tab double-click statics next to `g_last_text_press_*` (`src/app/glr_ctrl.c`
≈`:2236`): `g_last_tab_press_ms`, `g_last_tab_press_idx`. Reuse
`DOUBLE_CLICK_MS` and `current_double_click_ms()` (test-clock seam
`glr_ctrl_router_set_double_click_clock_for_test` — also wipe the new statics
there for the rename-trigger test).

`route_scene_tab_hit(const UiHit *hit)` near `route_menu_item_hit`:
- Resolve display idx against the **live** tab shape, not just the stale
  snapshot payload: compute `user_tab_count = repl_user_scene_count()`. If
  `idx < 0` → stale consumed no-op. If `idx < user_tab_count`, map it by
  **reusing `glr_scene_menu_slot_for_dense_index(idx)`**
  (`src/app/glr_actions.c:287`) and treat `>=0` as a user tab. If
  `idx == user_tab_count && active_example_idx >= 0`, it is the single appended
  example tab. Any other idx is stale → consumed no-op. This avoids
  misclassifying arbitrary out-of-range stale clicks as the example tab just
  because the dense slot lookup returned `-1`.
- **Reuse, do not copy, the load sequences (shared-helper extraction).**
  Copying the Scene-menu's 3-statement load sequences inline into the router
  is the *same* drift hazard this plan rejects for layout math (§1) — the
  load-bearing subtlety (user path must **not** call
  `editor_reset_transients()`; example path must) would then live in two
  places. Instead, extract two helpers in the same change and convert the
  existing Scene-menu actions to call them, so menu and tab share one
  definition:
  - `glr_scene_load_user_slot(int slot)` — body exactly the current
    `glr_actions.c:500-503`: `editor_undo_clear(); if
    (repl_load_user_scene_idx(slot)) load_line_to_input(repl_state_edit_line());`.
    No `editor_reset_transients()` (load-bearing omission — keep it). **No
    active-slot no-op inside the helper**: Scene-menu behavior must stay
    unchanged when refactored through this function. The tab router performs
    its own active-tab no-op before calling the helper.
  - `glr_scene_load_example(int example_idx)` — body exactly the current
    `glr_actions.c` example branch: `editor_reset_transients();
    editor_undo_clear(); repl_load_example(example_idx);`. **Takes an
    explicit index, not `active_example_idx`** — the Scene menu loads the
    *clicked* example via `item_idx - 1`, so a `(void)` helper reading
    `active_example_idx` would reload the wrong example (or no-op from a
    user scene) when a *different* example is picked from the menu. The
    menu action passes `item_idx - 1`; `route_scene_tab_hit` passes
    `repl_state_scenes().active_example_idx` (the example tab can only
    re-select the already-active example). **No active-example no-op inside the
    helper** for the same menu-preservation reason; the tab router consumes an
    active example-tab click without reloading.
  Both helpers are **declared in `src/app/glr_actions.h`** (next to
  `glr_scene_menu_slot_for_dense_index`) — `glr_ctrl.c` calls them and an
  undeclared call is an error under C2x. Placement follows existing layering
  (these touch `repl_*` + `editor_*`; `glr_actions.c` already does, so
  co-locate there and have `route_scene_tab_hit` call across — same
  direction as the existing menu→action calls). Example tabs are never
  renamed.
- Double-click on a user-scene tab (after switching, or if already active):
  `editor_inline_rename_begin(slot)`. Hard-modal capture and the status-bar
  prompt are already wired (`glr_ctrl_keyboard` → `editor_input_rename_capture_key`,
  `editor_inline_rename_active()`), so no extra plumbing.
- Active-tab clicks are consumed no-ops in the tab router (after updating the
  tab double-click statics): active user tab → no reload but a second click can
  still open rename; active example tab → no reload and never rename. This
  preserves the tab UX while keeping the shared helpers exact copies of the
  existing Scene-menu load behavior.

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
  mandatory helper-signature change; **5** reserve sites (3 code-row + 2
  scrollbar)
- `src/ui/repl_code_panel.c` — include; set `.top_chrome_h` in
  `init_builder`; call `ui_scene_tabs_render`; **public
  `ui_repl_code_panel_visible_lines_for_height` signature change** + every
  "rows that fit" / follow-scroll consumer threaded (§3 scroll-parity audit)
- `src/ui/panels.c` — include + hit-test insert (+ dropdown precedence check)
- `src/app/glr_actions.c` — extract `glr_scene_load_user_slot(int)` /
  `glr_scene_load_example(int)` shared helpers; refactor the existing
  Scene-menu user/example actions to call them (no behavior change — menu
  still passes the clicked `item_idx - 1`)
- `src/app/glr_actions.h` — declare the two shared helpers (called from
  `glr_ctrl.c`; undeclared call is a C2x error)
- `src/app/glr_ctrl.c` — snapshot build helper, **two** `_Static_assert`s
  (cap + name-max), double-click statics, `route_scene_tab_hit` (calls the
  shared helpers), dispatch case; thread band into scroll/Page/follow math
- `MODULES.md`, `CLAUDE.md` — add `src/ui/scene_tabs.{c,h}` to the layered
  overview and the File-Layout table (repo keeps these exhaustive)
- `tests/test_ui_text_panel.c`, `tests/test_repl_code_panel_layout.c`,
  `tests/test_repl_code_panel_document.c` — pass `0` for the new helper arg
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
example cleared, its tab vanishes (locked, zero code). **Scene → New** →
active example cleared, `editor_clear_all_cmds()`, no slot activated →
`active_idx == -1`; occupied slots (home, typically) still listed but
unhighlighted; no synthetic tab (decision #6). **Tutorial start
(`repl_scenes_enter_transient_scene`)** → same `neither-active` shape:
existing user tabs unhighlighted, no transient tab. HIDDEN → early-return,
`top_chrome_h==0`; count==0 → same zero-band behavior. TOP/BOTTOM → relative
to panel_top, follows. Scrollbar thumb → clears the tab band (both scrollbar
reserve sites threaded). Long name → hard-truncated. Too-narrow panel →
off-panel tabs clipped. Click active tab → no-op (still records press time so a
2nd click double-clicks). Double-click example tab → consumed only, no rename.
Stale click → resolved against live `repl_*`, consumed no-op. Blank tab-strip
space (gaps between tabs, right of the last tab) → `UI_HIT_CODE_PANEL_CHROME`,
inert; never falls through to a code-text/gutter hit.

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
   → N user tabs in slot order; **neither-active state (decision #6:
   home slot used, `active_user_scene==-1`, `active_example_idx==-1`, as
   after Scene→New / tutorial transient) → home tab listed, `active_idx ==
   -1`, no example tab, no synthetic tab; zero occupied slots in that state
   → `count == 0`**.
2. **Geometry hit-mapping** (module-level `ui_scene_tabs_hit_test`) — center of
   each tab → correct `item_idx`; in-band gap / right-of-last-tab →
   `UI_HIT_CODE_PANEL_CHROME`; truly outside the band → `UI_HIT_NONE`.
2b. **Band fall-through guard** (integration-level `ui_panels_hit_test`; seed a
   tab list + panel rect) — a click on blank tab-strip space returns
   `UI_HIT_CODE_PANEL_CHROME`, **not** `UI_HIT_CODE_TEXT`/`UI_HIT_CODE_GUTTER`
   (regression for the verified fall-through via `ui_repl_code_panel_hit_test`,
   incl. the band's lower pixels where `row_from_top` truncates to 0); a click
   on a tab → `UI_HIT_CODE_PANEL_TAB`; a click below the band still reaches the
   code panel.
3. **band_h lockstep** — `==TAB_STRIP_H` (==`LINE_H`) with tabs, `==0` HIDDEN
   or `count==0`;
   assert `visible_lines(H,flags,TAB_STRIP_H)` is **exactly one** row fewer
   than `(H,flags,0)`, swept over several `H` — valid precisely because
   `TAB_STRIP_H==LINE_H` (see §3); the sweep guards a future
   `TAB_STRIP_H != LINE_H` regression. **Restrict the sweep to heights where
   the baseline `visible_lines(H,flags,0) > 1`.** `ui_text_panel_visible_
   lines_for_height()` floors `available` at 0 and returns `available/LINE_H
   + 1` (`text_panel.c:669-671`), so its minimum return is `1`; on very
   short panels both sides hit that floor and the delta is `0`, not `1` —
   not a regression, just the clamp. Skipping `baseline <= 1` heights keeps
   the assertion meaningful without weakening it where it matters.
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

Six review findings across two rounds, all re-verified against source and
folded into the sections above:

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
- **[P1 · round 2] Blank tab-strip space fell through to a code hit.**
  Verified: off-tab band points returning `UI_HIT_NONE` let `ui_panels_hit_test`
  → `ui_repl_code_panel_hit_test` misclassify them as
  `UI_HIT_CODE_TEXT`/`UI_HIT_CODE_GUTTER` (or a row-0 hit via the
  `row_from_top==0` truncation in `text_panel.c`; the `gl_y < cp_y +
  STATUSBAR_H` chrome guard is false near the panel top,
  `repl_code_panel.c:1414-1430`). Fix: the band hit-test now consumes the whole
  band — tab → `UI_HIT_CODE_PANEL_TAB`, in-band off-tab →
  `UI_HIT_CODE_PANEL_CHROME`, `UI_HIT_NONE` only outside. Added an
  integration-level `ui_panels_hit_test` regression test (§Verification 2b).
  Also corrected the inaccurate "falls through to `UI_HIT_SCENE`" note.
- **[P2 · round 2] "Strictly snapshot-only" contradicted the geometry plan.**
  `scene_tabs_rects()` calls `ui_layout_code_panel_rect()`, which reads live
  `ui_state_viewport()`/`ui_state_code_panel()` (`layout.c:12-13, 29-58`).
  Resolution = option (b): reworded §design / §1 / §5 to scope purity precisely
  (snapshot for content + y-flip; no `state.h`, no REPL/editor state; panel
  geometry via the shared layout helper exactly as `menu_bar.c`), documented
  the within-frame consistency invariant, and explicitly rejected option (a)
  (duplicating layout math into snapshot fields — forks geometry, drift
  hazard).

### Round 3

- **[P1] Vertical-reserve threading was scoped to rendering only.** The
  public `ui_repl_code_panel_visible_lines_for_height(int cp_h)` has no
  `snap`, so scroll-clamp / cursor-follow / Page math would drift one row
  from the rendered text. §3 now carries a *mandatory* scroll-parity
  consumer audit (named call sites + a signature change on the wrapper), and
  the §Verification grep is reclassified blocking, not pre-finish.
- **[P1] `snapshot.h` purity break.** Draft had `UI_SCENE_TAB_NAME_MAX =
  USER_SCENE_NAME_MAX` directly in `snapshot.h`, pulling a repl macro into
  the UI layer — the exact thing the hardcoded-cap+`_Static_assert` pattern
  exists to prevent. §2 now hardcodes `64` and adds the matching assert in
  `glr_ctrl.c`, symmetric with the cap.
- **[P1] Open-dropdown z-order/hit interaction unanalyzed.** Scene/Config
  dropdowns expand through the new band. §4 now keeps the dropdown in the
  later controller overlay pass (`ui_menu_bar_render_example_dropdown`) so it
  overpaints the strip, and draws the strip before `ui_menu_bar_render` inside
  the code-panel pass; §5 adds the matching dropdown hit-precedence
  requirement.
- **[P2] Scene-load sequences were duplicated.** §6 copied the menu's
  3-statement load paths inline — the same drift hazard the plan rejects for
  layout math. Replaced with `glr_scene_load_user_slot()` /
  `glr_scene_load_example(int)` shared helpers that the Scene-menu actions are
  refactored to call too. The helpers preserve menu reload semantics;
  active-tab no-ops live only in the tab router.
- **[P2] CHROME router inertness asserted, not verified.** §5 now requires
  confirming `UI_HIT_CODE_PANEL_CHROME` is side-effect-free at the new band
  coordinates (no divider drag / cursor move) rather than assuming it.
- **[P3] Line numbers stale on this branch + docs gap.** Added the
  indicative-line-numbers banner (segment-cap / fake-bold work already
  shifted `text_panel.c`); added `MODULES.md` / `CLAUDE.md` to Files;
  corrected the effort estimate to ~a day-plus.

### Round 4

All four findings verified against source and incorporated:

- **[P1] Example helper would break the Scene menu.** `glr_actions.c`
  loads the *clicked* example via `repl_load_example(item_idx - 1)`; a
  `glr_scene_load_active_example(void)` reading `active_example_idx` would
  reload the wrong example (or no-op from a user scene) on a fresh menu
  pick. §6 helper is now `glr_scene_load_example(int example_idx)` — menu
  passes `item_idx - 1`, tab passes `active_example_idx`.
- **[P2] Transient / neither-active documents were unspecified.** Tutorial
  start (`tutorial.c:286 repl_scenes_enter_transient_scene()`) and Scene →
  New leave `active_user_scene == -1 && active_example_idx == -1` with a
  live document. Added locked decision #6 (no synthetic tab; list occupied
  slots, `active_idx == -1`), edge-case entries, and a derivation test
  case.
- **[P2] Shared helpers need a header.** `glr_actions.h` exposes only the
  existing action API; `glr_ctrl.c` calling the new helpers is a C2x
  undeclared-call error. Added `glr_actions.h` to Files and the §6
  declaration requirement.
- **[P3] Lockstep sweep needs a min-height guard.**
  `ui_text_panel_visible_lines_for_height()` floors at `1`
  (`text_panel.c:669-671`), so on tiny panels the delta is `0`, not `1`.
  Test #3 now restricts the sweep to heights where `baseline > 1`.

### Round 5

Follow-up coherence pass corrections:

- **[P1] Helper no-ops conflicted with Scene-menu behavior.** The shared
  `glr_scene_load_*` helpers now preserve the current menu reload semantics;
  active-tab no-ops live only in `route_scene_tab_hit`.
- **[P2] Stale tab indices could become example hits.** §6 now resolves
  `item_idx` against the live `repl_user_scene_count()` shape and only treats
  `idx == user_tab_count` as the example tab.
- **[P2] Zero-tab visibility contradicted "always shown."** Decisions #3/#6
  now state that non-empty tab sets always show, including one tab; `count==0`
  means `band_h==0` and no strip.
- **[P3] Dropdown z-order rationale was stale.** §4 now names the actual
  later `ui_menu_bar_render_example_dropdown()` overlay pass instead of
  implying dropdown rows are drawn inside `ui_menu_bar_render()`.

## Scope / effort

Moderate — realistically **~one day-plus** for someone familiar with the
tree (the earlier "half a day" estimate predated the scroll-parity audit and
dropdown-z-order work). Code volume is small and mostly a structural copy of
`menu_bar.c`; the cost is *care*, concentrated in three things, not one:
(1) the scroll/paging consumer audit (§3 — the genuine high-blast-radius
item, broader than the 5 render sites), (2) the open-dropdown z-order +
hit-precedence interaction (§4/§5), and (3) the shared-load-helper
extraction + Scene-menu refactor (§6). *Architectural* risk is low (no new
persistent state, no new ownership crossings, all boundary guards checked);
*integration* risk is the residual, retired by the mandatory helper
parameter, the blocking §Verification grep, and the lockstep + fall-through
+ scroll-parity tests.
