# Config menu → flyout submenu sections (+ "All")

Status: **active** — direction chosen: **Option A** (generalize the
submenu engine). Implementing step-by-step; each step lands as its own
commit and updates this file's progress log below.

## Progress log

- [x] Step 1 — Section model in `glr_config` (incl. `row_kind`)
- [x] Step 2 — Generalize submenu plumbing (Scene-wired; Config stubbed)
- [x] Step 3 — `UiHit` contract rename
- [x] Step 4 — Config menu shape (+ flyout state/shortcut render, pulled from Step 8)
- [x] Step 5 — Parent-row click guard (option b; activate-layer)
- [x] Step 6 — Controller routing
- [x] Step 7 — Right-click backward-cycle in flyouts
- [x] Step 8 — Render verification (gates green; All-flyout scroll noted)
- [x] Step 9 — Tests + docs (full gate green; CLAUDE.md + MODULES.md updated)

## Goal

Replace the single flat Config dropdown with a short list of **section
rows** (RENDERING, TIME & REPLAY, OVERLAYS & SCENE, GEOMETRY,
INTERFACE, AUDIO) that each open a **flyout submenu** of their items —
the same interaction the Scene menu already uses for example tags. Add
an **All** section row whose flyout is today's full flat list (so the
current behavior remains reachable verbatim).

## What already exists (so this is mostly wiring)

- **Sections are already data.** `g_cfg_items[]`
  (`src/app/glr_actions.c:94`) already carries `### RENDERING`,
  `### TIME & REPLAY`, `### OVERLAYS & SCENE`, `### GEOMETRY`,
  `### INTERFACE`, `### AUDIO` as `section_header` rows with `---`
  separators. No data restructure is required — a runtime walk that
  treats `### X` rows as boundaries yields `(label, start, count)`.
- **"All" is the current flat list** — same content/order/look as
  today, just relocated into a flyout. Not quite *free*: because it
  spans the `### `/`---` chrome rows, the generic submenu layer needs a
  `row_kind` so chrome stays inert (Finding #4 — see Step 2).
- **Submenu-item activation is index-based and unchanged.**
  `glr_action_menu_item_activate(menu_id, item_idx)`
  (`src/app/glr_actions.c:455`, Config branch at `:551`) already
  dispatches a Config row purely by its **absolute `g_cfg_items[]`
  index** via `glr_config_item_at(item_idx)`. A submenu row only has to
  carry that absolute index — exactly like the Scene submenu carries
  the global example index — so the *leaf-item* activation path needs
  *zero* changes. **The parent (section) row path does need a guard —
  see the "Parent-row click guard" risk below.**
- **There is an exact precedent for the parent-row guard.** The
  `MENU_SCENE` branch of `glr_action_menu_item_activate`
  (`src/app/glr_actions.c:~520`) already does
  `if (item_idx >= 1 && item_idx <= tag_count) return 0;` — a tag
  *parent* row click is a deliberate no-op (the flyout opens on
  **hover**, not click). Config must mirror this for section rows.
- **The flyout pattern is proven** in `src/ui/menu_bar.c`:
  `scene_example_submenu_rect` / `scene_example_submenu_hit_test`,
  hover-open timing via `g_scene_open_tag` + `g_scene_submenu_open_time`
  + `update_scene_submenu_hover_at`, and `render_scene_example_submenu`.

## The actual decision: the submenu engine is scene-coupled

Every layer of the existing submenu code is hardwired to
scene/example concepts:

- `scene_example_submenu_rect/_hit_test` call `repl_example_count_for_tag`
  / `repl_example_index_for_tag` directly.
- Open-state is a single pair of globals (`g_scene_open_tag`,
  `g_scene_submenu_open_time`) gated on `g_open_menu == MENU_SCENE`.
- The hit result is a dedicated `UI_HIT_EXAMPLE_SUBMENU_ITEM`
  (`src/ui/hit.h:28`), routed by `route_example_submenu_item_hit`
  (`src/app/glr_ctrl.c:2933`).
- `menu_item_count/label/shortcut/state` for `MENU_CONFIG`
  (`src/ui/menu_bar.c:154+`) currently index the flat list.

So there are two viable directions.

### Option A — Generalize the submenu engine (recommended)

Make one menu-agnostic submenu mechanism driven by `(menu_id,
parent_row)` plus a tiny row-provider indirection:

- `submenu_count(menu, parent_row)`
- `submenu_label(menu, parent_row, ordinal)`
- `submenu_abs_index(menu, parent_row, ordinal)`
- **`submenu_row_kind(menu, parent_row, ordinal)` →
  `{HEADER, SEPARATOR, ITEM}`** — required because the **All** section
  spans the *full* `g_cfg_items[]` range, which contains `### …`
  header and `---` separator rows. Without a row-kind the generic
  hit-test would route a click on chrome as a real config hit. The
  existing flat path already solves this in
  `ui_menu_bar_dropdown_item_hit` (`src/ui/menu_bar.c:435` returns -1
  for `###`/`---`); the generic submenu hit-test must carry the same
  rule, and the renderer must draw HEADER/SEPARATOR rows as the same
  inert chrome they are today.

Scene plugs in its existing `repl_example_*_for_tag` (every row is an
`ITEM`). Config plugs in a "section → flat range" walker over
`g_cfg_items[]`: named sections expose only their `ITEM` rows (the
leading `### `/`---` excluded from the range, so those flyouts are
clean); the synthetic **All** section exposes the full range with
HEADER/SEPARATOR rows passed through as inert chrome (preserving
today's flat-list look verbatim).

Pros:
- One submenu code path (rect/hit/hover-open/render) instead of two.
  Consistent with the codebase's ownership discipline (CLAUDE.md
  prefixes; `arch-simplicity` ethos) — avoids a parallel copy.
- `MENU_CONFIG`'s `menu_item_count/label` collapse to the section list;
  per-item detail moves entirely behind the submenu provider.
- Future menus (Tutorials?) get submenus for free.

Cons:
- Diffuse change: touches the hover-open state machine (a single
  `g_open_*tag` pair can't represent two menus' open submenus — replace
  with `{menu_id, parent_row, open_time}`), the hit-test precedence in
  `ui_menu_bar_hit_test` (`:480`), the render path, and the `UiHit`
  contract (rename `UI_HIT_EXAMPLE_SUBMENU_ITEM` →
  `UI_HIT_SUBMENU_ITEM`, carry `menu_id` so the controller routes to
  scene-load *or* `glr_action_menu_item_activate`).
- Touches Scene-menu behavior — regression-test both menus.
- Config-specific gaps the Scene menu never had (all addressed in the
  Steps, not deferred): **(1)** Config parent rows must not route as
  flat-index activation — guarded via the `MENU_SCENE` precedent
  (Step 5); **(3)** right-click backward-cycle must move from the flat
  dropdown to the open flyout (Step 7); **(4)** the All flyout spans
  `### `/`---` chrome, so the generic provider needs a `row_kind` and
  the renderer/hit-test must keep chrome inert (Step 2 provider, Step 8
  render).

### Option B — Duplicate the submenu code for Config

Copy `scene_example_submenu_*` into `config_section_submenu_*` with its
own `g_config_open_section` / open-time globals and a
`UI_HIT_CONFIG_SUBMENU_ITEM` kind + route.

Pros:
- Smaller blast radius; Scene menu untouched; faster to land.

Cons (decisive):
- Two near-identical submenu engines to keep in sync (rect math,
  hover-open timing, fade, hit precedence) — classic pattern-soup in a
  tree that is otherwise careful about single-owner mechanics.
- The hover-open globals still need disambiguation once both menus can
  have an open submenu, so half of A's "diffuse" cost is paid anyway.

## Recommendation

**Option A.** The generalization is the *less* complex end state (one
submenu engine, Config detail hidden behind a provider), and B pays much
of A's state-machine cost without the consolidation benefit. Note the
review correction: the "All" section and the parent-row activation path
are *not* trivial — All drags in row-kind/chrome handling and section
rows need the no-op-on-click guard — but those costs are identical in A
and B, so they don't change the A-vs-B verdict; they're folded into the
Steps.

## Implementation sketch (Option A)

Order matters; each step builds + passes tests before the next.

1. **Section model (no UI yet).** ✅ **Done** (commit `Step 1`).
   Added to `src/app/glr_config.{c,h}` a pure helper enumerating **only
   the real sections present in the data** (no synthetic All — that is
   a menu-layer concern, see Step 4): `glr_config_section_count()`
   (count of `### ` headers), `glr_config_section_label(s)` ("### "
   marker stripped), `glr_config_section_range(s, *start, *count)`
   (item rows only; header and trailing `---` excluded). Ownership
   rule: this accessor is data-faithful; the synthetic **All** row is
   *never* counted here.
   **Deviation from sketch:** `glr_config_row_kind(idx) →
   {ITEM,HEADER,SEPARATOR}` (originally slated for Step 2) landed here
   too — it is pure config-domain row classification, so it belongs
   beside the section model, not in the menu layer. Step 2's provider
   now just *consumes* it.
   Tests: `test_glr_actions.c::test_config_sections` — section
   count/labels, range contiguity & item-only invariant, and that
   header+separator+item kinds partition the whole table
   (`build/release-gl-stubs/test_glr_actions` → 157/157).
2. **Generalize submenu plumbing in `src/ui/menu_bar.c`.** ✅ **Done**
   (commit `Step 2`). Replaced `g_scene_open_tag` /
   `g_scene_submenu_open_time` with generic
   `g_submenu_menu_id`/`g_submenu_parent_row`/`g_submenu_open_time`
   (+ `submenu_reset()`). Rewrote the scene-specific functions into a
   menu-agnostic engine — `submenu_rect`, `submenu_hit_test`,
   `submenu_hover_ordinal`, `update_submenu_hover_at`,
   `render_active_submenu` — driven by a small file-static provider
   keyed by `(menu_id, parent_row)`: `submenu_row_count/_label/
   _abs_index/_kind/_is_active` + `menu_row_has_submenu`. `MENU_SCENE`
   resolves the parent row back to its tag and reuses
   `repl_example_*_for_tag`; the render path now also handles
   `GLR_CFG_ROW_HEADER`/`SEPARATOR` rows as inert chrome (for the
   future Config "All" flyout). `_for_test` accessor kept (now maps
   `tag → parent_row → submenu_rect`).
   **Deviations from sketch (kept the step behaviour-neutral):**
   - The Config side of the provider is *stubbed* (`submenu_row_count`
     returns 0 for non-Scene) — the `glr_config_section_*` wiring lands
     in Step 4 with the menu-shape change, so this commit cannot regress
     Config.
   - The `UiHit` contract is **not** renamed yet (that is Step 3). A
     `submenu_fill_hit()` shim still emits the existing
     `UI_HIT_EXAMPLE_SUBMENU_ITEM` payload for Scene, so existing
     hit-test tests stay green verbatim.
   Verified: `make sample USE_GL_STUBS=1` clean; full suite
   `make test USE_GL_STUBS=1` → 43/43 binaries, 6024/6024 tests.
3. **`UiHit` contract.** ✅ **Done** (commit `Step 3`). Renamed
   `UI_HIT_EXAMPLE_SUBMENU_ITEM` → `UI_HIT_SUBMENU_ITEM`; payload is now
   `cmd_idx = menu_id`, `item_idx = absolute target index` (global
   example idx / `g_cfg_items[]` idx), `line_idx = ordinal`.
   `submenu_fill_hit()` collapsed to a single menu-agnostic fill (no
   more Scene special-case). Updated `src/ui/hit.h` + `src/ui/menu_bar.h`
   doc blocks and the `glr_ctrl.c` enum references (case label,
   dismiss-guard, comment). `route_example_submenu_item_hit` keeps
   working unchanged (it only reads `item_idx`); its rename + Config
   `cmd_idx` branch is Step 6. Test updated:
   `test_ui_menu_bar.c` now asserts `UI_HIT_SUBMENU_ITEM` and
   `cmd_idx == GLR_MENU_SCENE`. Full suite 6024/6024.
4. **Config menu shape.** ✅ **Done** (commit `Step 4`).
   `menu_item_count(MENU_CONFIG)` → `glr_config_section_count() + 1`;
   the **menu layer owns the +1 synthetic All row** via
   `config_all_parent_row()` (Step 1's accessor never counts it — no
   double-count). `menu_item_label` returns section labels + "All";
   every Config top-level row is a parent that hover-opens the generic
   submenu. Provider Config branch wired
   (`config_submenu_abs_index` + `submenu_row_count/_label/_abs_index/
   _kind/_is_active`): named section → its `ITEM` range; All → whole
   table 1:1 with `### `/`---` passed through as inert chrome.
   `ui_menu_bar_handle_config_right_press` made a safe no-op over the
   section list (full in-flyout right-press is Step 7).
   **Deviations from sketch (discovered while keeping each step green):**
   - **Pulled the flyout per-item state-label + shortcut *rendering*
     forward from Step 8.** The shape change deletes the only place
     the top-level dropdown rendered them, which would have orphaned
     `cfg_state_str` / `config_item_shortcut` / `cfg_max_state_chars`
     (dead-code warnings) and shipped an unusable Config menu mid-plan.
     `render_active_submenu` now draws the shortcut + state columns for
     Config `ITEM` rows and `submenu_rect` widens via
     `config_submenu_extra_w()`. Step 8 is correspondingly reduced to
     render *verification/polish* (chrome inertness already done in
     Step 2's `render_active_submenu`).
   - Removed now-dead `menu_max_shortcut_px`, `state_right`,
     `max_state` (they only fed the deleted top-level Config state
     column).
   - Tests: removed the flat-model `find_first_config_action_point`
     helper + the flat right-press assertions from
     `test_ui_menu_bar.c` (right-press coverage returns in Step 7).
     `test_ui_scene_tabs.c`'s "### header → CHROME" overlay-precedence
     regression updated to the new shape (a Config section row is
     `UI_HIT_MENU_ITEM`, consumed, scene-unchanged invariant intact).
   - **Step 4 ↔ 5 coupling found:** plan-Step-5 option (a) ("hit-test
     never emits `UI_HIT_MENU_ITEM` for parent rows") is **not viable**
     — `ui_menu_bar_dropdown_item_hit` is shared with the hover path,
     and returning −1 there would stop Config sections from
     hover-opening. So Step 5 collapses to **option (b) only**: the
     `glr_action_menu_item_activate(MENU_CONFIG, …)` guard. At Step 4
     the scene-tabs row-0 case is already a safe no-op (row 0 aliases
     a `### ` header so the existing `!section_header` check skips the
     cycle); Step 5 generalizes that to *every* section/All row.
   Verified: `make sample USE_GL_STUBS=1` warning-free; full suite
   `make test USE_GL_STUBS=1` → 6020/6020.
5. **Parent-row click guard (Finding #1).** ✅ **Done** (commit
   `Step 5`). Option (a) ruled out in Step 4 (hover shares
   `ui_menu_bar_dropdown_item_hit`); implemented **option (b)**: the
   `GLR_MENU_CONFIG` branch of `glr_action_menu_item_activate` is now
   an explicit parent-row no-op — it never treats `item_idx` as a
   `g_cfg_items[]` index, returns 0 (dropdown stays open), mirroring
   the `MENU_SCENE` tag-row guard. Leaf config items will be activated
   by the controller calling `glr_cfg_cycle_row()` on the absolute
   index via the submenu route (Step 6), never through this branch.
   Tests: new `test_glr_actions.c::test_config_parent_rows_inert`
   asserts every section parent + the "All" row returns 0 and mutates
   **no** config value; `test_repl_editor.c` retargeted to the
   `glr_cfg_cycle_row()` primitive (and asserts the parent-row path is
   inert); `test_glr_actions.c:278` comment corrected. Full suite
   6059/6059.
6. **Controller routing.** ✅ **Done** (commit `Step 6`). Renamed
   `route_example_submenu_item_hit` → `route_submenu_item_hit`;
   branches on `hit->cmd_idx`: `GLR_MENU_CONFIG` →
   `glr_cfg_cycle_row(item_idx, +1)` on the absolute `g_cfg_items[]`
   index and **keeps** the dropdown + flyout open (repeated toggles);
   default/`MENU_SCENE` → `glr_scene_load_example(item_idx)` then
   `ui_menu_bar_close()`. **Plan correction:** the sketch said route
   Config through `glr_action_menu_item_activate(GLR_MENU_CONFIG, …)`,
   but Step 5 made that branch the inert parent-row guard — so the
   leaf path deliberately calls `glr_cfg_cycle_row` directly instead
   (the activate path would now no-op). `UI_HIT_SUBMENU_ITEM` is
   already in the dropdown-dismiss keep-open allowlist (Step 3), so a
   Config flyout click doesn't trip the outside-click preamble.
   Dedicated Config-flyout hit/route tests (incl. the keep-open
   assertion) are added in Step 9 with the Scene-mirrored submenu
   tests; full suite stays 6059/6059.
7. **Right-click backward-cycle in flyouts (Finding #3).** ✅ **Done**
   (commit `Step 7`). `ui_menu_bar_handle_config_right_press` now
   calls the generic `submenu_hit_test(mx,my)` — which only resolves
   actionable `ITEM` rows (chrome skipped via `submenu_row_kind`;
   parent/All rows own no submenu) and carries the absolute
   `g_cfg_items[]` index in `item_idx` — then `glr_cfg_cycle_row(idx,
   -1)`. A right-press over the section list / chrome / closed menu is
   a clean no-op. Added a public generic test accessor
   `ui_menu_bar_submenu_rect_for_test(menu_id, parent_row, …)` (the
   Scene tag-indexed helper now delegates to it). New
   `test_ui_menu_bar.c::test_config_submenu_right_press`: flyout item
   cycles backward; section parent row & closed menu are no-ops. Full
   suite 6069/6069.
8. **Render (verification — core work pulled into Steps 2 & 4).**
   ✅ **Done** (commit `Step 8`, plan-only). `render_active_submenu`
   (unified fade, Step 2) + HEADER/SEPARATOR inert chrome (Finding #4)
   + Config flyout shortcut/state columns (Step 4) were code-reviewed
   for the **All** flyout path: HEADER rows draw the section caption
   ("### " stripped by `submenu_row_label`), SEPARATOR rows draw the
   divider rule, ITEM rows draw label + right-aligned shortcut + state
   (accent when on); the `>` parent affordance shows on every Config
   section/All row via `menu_row_has_submenu`. Verification gates:
   `make sample` (real GL) builds **clean** (no warnings),
   `make check-c99` OK, `make check-state-ownership` OK (no
   GL-purity/ownership regression), `make test USE_GL_STUBS=1`
   6069/6069.
   **Known limitation (documented, out of scope):** the **All** flyout
   spans the whole table (~40+ rows). The submenu engine — like the
   pre-existing Scene tag submenu — does not scroll, so on a short
   window the All flyout clamps to `y=0` and its earliest rows render
   above the viewport. Named sections (the primary feature) are short
   and unaffected; All is a secondary escape hatch. A scrolling
   submenu is a separate follow-up, not part of this plan (added to
   Open sub-questions).
9. **Tests + docs.** ✅ **Done** (commit `Step 9`).
   - Flat-Config test churn was absorbed in the steps that changed the
     behavior (4/5/7). Added positive coverage:
     `test_config_submenu_with_stubs` (flyout rect/hit/render +
     `All`-flyout HEADER stays inert chrome) mirroring
     `test_scene_submenu_with_stubs`.
   - **Gate fix (Step 7 latent issue, surfaced here):**
     `submenu_row_point` was inside the `#ifdef GL_STUBS` block but the
     non-stub `test_config_submenu_right_press` used it → real-GL
     `make test` failed to compile. Moved the pure-geometry helper
     outside the guard.
   - Full gate green: `make test` 5436/5436, `make test-stubs`
     6084/6084, `make sample` (real GL) clean, `make sample
     USE_GL_STUBS=1` clean, `make check-state-ownership` clean.
   - Docs: CLAUDE.md "Config Menu" rewritten (section flyouts, All,
     shared engine, click/right-press semantics, add-an-item note);
     MODULES.md `ui_menu_bar` row notes the shared submenu engine.

## Outcome

All nine steps landed as separate commits with the plan kept in lock-
step. Option A (generalize) delivered: one `(menu_id, parent_row)`
flyout engine now serves both the Scene example menu and the new
Config section/All menu, with the four review findings closed
(parent-row guard, single All ownership, right-press in flyout,
row-kind chrome). Net deviations from the original sketch — all
documented inline above — were forced by the "every step builds +
passes" constraint: render columns pulled Step 8→4, the Step 4↔5
coupling collapsed Step 5 to option (b), Step 6 routes Config leaves
directly via `glr_cfg_cycle_row` (not the now-inert activate branch),
and Step 8 became verification. One out-of-scope follow-up recorded:
a scrolling submenu for the tall `All` flyout.

### Post-merge render fixes (visual review)

A live-binary review caught two regressions in the Step-4 flyout
render; both fixed (commit `flyout render fix`):

1. **Item label colour.** Step 4's `submenu_row_is_active` tinted
   *enabled* Config rows' labels with the accent colour, unlike the
   original flat dropdown (label always primary; only the state value
   is coloured). Dropped the Config branch of `submenu_row_is_active`
   so Config labels stay `UI_TOK_TEXT_PRIMARY`; on/off is conveyed
   solely by the right-hand state-value colour, matching the original.
2. **Column alignment.** The shortcut/state were positioned per-row
   (state placed relative to that row's own shortcut), so rows without
   a shortcut pushed the state value into the shortcut column. Now the
   flyout computes fixed `cfg_sc_right` / `cfg_state_right` x's once
   (from `config_submenu_max_sc_px`), so both columns align across all
   rows like the original flat dropdown. Gate: 6084/6084 stubs,
   real-GL `make sample` clean.

A second live review (commit `header case + arrow column`) added:

3. **Section header casing (display-layer only).** The menu *heading*
   rows should read leading-uppercase ("Rendering", "Time & replay").
   Per user direction the `g_cfg_items[]` `### ` labels stay
   **UPPERCASE** (single source of truth, data-faithful) and
   `glr_config_section_label` still returns them verbatim
   (`test_config_sections` unchanged); the prettify is applied only in
   `menu_item_label(MENU_CONFIG, …)` for the top-level section parent
   rows. The All-flyout chrome captions deliberately stay UPPERCASE,
   matching the original flat dropdown.
4. **Parent label vs `>` collision.** The Config dropdown width didn't
   reserve space for the flyout-arrow glyph, so a long section name
   ("Overlays & scene") ran into / clipped the `>`. `menu_dropdown_rect`
   now adds a fixed `SUBMENU_ARROW_COL` (26 px) to the width when the
   menu has any submenu parent row (Scene benefits too). Gate:
   6084/6084 stubs, real-GL `make sample` clean.

## Open sub-questions for review

- **All placement:** ✅ resolved — **last** row (after all sections),
  via `config_all_parent_row() == glr_config_section_count()`.
- **Keyboard shortcuts:** ✅ resolved — the submenu ITEM rows render
  the shortcut hint + state label (Step 4); F-key/Ctrl dispatch is
  unchanged (still `glr_actions`, not menu geometry).
- **Scene-menu regression scope:** ✅ held — the Scene example submenu
  rides the same generalized engine and all Scene submenu tests stayed
  green every step (6069/6069 at Step 7).
- **Scrolling submenu (NEW, follow-up — out of scope):** the **All**
  flyout exceeds a short window's height and the engine doesn't
  scroll (a limitation inherited from the pre-existing Scene tag
  submenu). Worth a dedicated follow-up plan (clamp + scroll, or cap
  All to N and paginate). Not blocking: named sections are short; All
  is the secondary escape hatch.

## Folder note

`in-review` → (pick A or B) → `not-started` → `active` → `done`, or
deleted if dropped.
