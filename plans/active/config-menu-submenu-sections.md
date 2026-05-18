# Config menu → flyout submenu sections (+ "All")

Status: **active** — direction chosen: **Option A** (generalize the
submenu engine). Implementing step-by-step; each step lands as its own
commit and updates this file's progress log below.

## Progress log

- [ ] Step 1 — Section model in `glr_config`
- [ ] Step 2 — Generalize submenu plumbing
- [ ] Step 3 — `UiHit` contract rename
- [ ] Step 4 — Config menu shape
- [ ] Step 5 — Parent-row click guard
- [ ] Step 6 — Controller routing
- [ ] Step 7 — Right-click backward-cycle in flyouts
- [ ] Step 8 — Render generalization
- [ ] Step 9 — Tests + docs

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

1. **Section model (no UI yet).** Add to `src/app/glr_config.{c,h}` a
   pure helper enumerating **only the real sections present in the
   data** (no synthetic All — that is a menu-layer concern, see Step 4):
   `glr_config_section_count()` (count of `### ` headers),
   `glr_config_section_label(s)`,
   `glr_config_section_range(s, *start, *count)` (walks `### ` / `---`
   rows; `count` covers the section's `ITEM` rows only). Ownership rule:
   this accessor is data-faithful; the synthetic **All** row is
   *never* counted here. Unit test in a core test (no GL).
2. **Generalize submenu plumbing in `src/ui/menu_bar.c`.** Replace
   `g_scene_open_tag`/`g_scene_submenu_open_time` with a generic
   `{int open_menu_id; int open_parent_row; float open_time;}`.
   Rewrite `scene_example_submenu_rect/_hit_test/render/_hover` to take
   `(menu_id, parent_row)` and resolve rows through a small static
   dispatch:
   - `MENU_SCENE` → existing `repl_example_*_for_tag`
   - `MENU_CONFIG` → `glr_config_section_*`
   Keep the `_for_test` rect accessor; add a Config-parametrized twin.
3. **`UiHit` contract.** Rename `UI_HIT_EXAMPLE_SUBMENU_ITEM` →
   `UI_HIT_SUBMENU_ITEM`; carry `cmd_idx = menu_id`, `item_idx =
   absolute target index` (example idx or `g_cfg_items[]` idx),
   `line_idx = ordinal`. Update `src/ui/hit.h` doc block.
4. **Config menu shape.** In `src/ui/menu_bar.c`,
   `menu_item_count(MENU_CONFIG)` → `glr_config_section_count() + 1`,
   where the **menu layer owns the +1 synthetic All row** (the pure
   accessor in Step 1 never counts it — single ownership, no
   double-count). `menu_item_label` returns the section labels plus
   "All"; every Config top-level row is now a **parent row** that
   hover-opens the generic submenu (named-section parent → that
   section's `ITEM` range; All parent → full range incl. inert chrome).
   Update `menu_item_shortcut/state` for Config (shortcuts/state now
   render in the submenu rows, not the parent list).
5. **Parent-row click guard (Finding #1).** Section/All rows must be
   inert on click — the flyout opens on **hover**. Mirror the
   `MENU_SCENE` precedent: add a Config branch guard in
   `glr_action_menu_item_activate` so a parent-row `item_idx`
   `return 0;` (no-op) instead of being treated as an absolute
   `g_cfg_items[]` index. Concretely, the `UiHit` for a Config parent
   row must NOT be a plain `UI_HIT_MENU_ITEM` carrying a flat index:
   either (a) `ui_menu_bar_hit_test` resolves Config parent rows to the
   hover-open path and never emits `UI_HIT_MENU_ITEM` for them
   (preferred — symmetric with how Scene tag rows already work via
   `scene_example_submenu_hit_test` precedence at
   `src/ui/menu_bar.c:481`), or (b) the activate-guard rejects them.
   Do (a) *and* keep (b) as the defensive backstop. Test: clicking
   each Config section row toggles nothing.
6. **Controller routing.** Rename `route_example_submenu_item_hit` →
   `route_submenu_item_hit`; branch on `hit->cmd_idx` (menu_id):
   `MENU_SCENE` → `glr_scene_load_example(item_idx)` (unchanged);
   `MENU_CONFIG` → `glr_action_menu_item_activate(GLR_MENU_CONFIG,
   item_idx)` (the existing index-based leaf path — verify a Config
   submenu click keeps the dropdown open for repeated toggles, matching
   today's flat-list behavior; scene submenu closes on pick).
7. **Right-click backward-cycle in flyouts (Finding #3).**
   `ui_menu_bar_handle_config_right_press` (`src/ui/menu_bar.c:581`)
   today calls `ui_menu_bar_dropdown_item_hit` (flat dropdown) →
   `glr_cfg_cycle_row(item, -1)`. Once items live in flyouts, the
   right-press must hit-test the **open submenu** instead: resolve the
   submenu row under the cursor to its absolute `g_cfg_items[]` index
   via the generic submenu hit-test, skip HEADER/SEPARATOR rows, then
   `glr_cfg_cycle_row(abs_idx, -1)`. A right-press on a parent/All row
   is a no-op (no item to cycle). Add explicit test coverage:
   right-click a flyout item cycles it backward; right-click a section
   row does nothing.
8. **Render.** Generalize `render_scene_example_submenu` to
   `render_active_submenu(snap)`; reuse fade
   (`g_*_submenu_open_time` → unified open-time). Parent rows that own a
   submenu get the same affordance the Scene tag rows have. The All
   flyout renders HEADER/SEPARATOR rows as the same inert chrome the
   flat dropdown draws today (row-kind from the provider, Finding #4).
9. **Tests + docs.**
   - Existing menu tests asserting flat Config row counts/labels will
     change — update them; add submenu rect/hit tests for Config
     mirroring the Scene ones.
   - `make test`, `make test-stubs`, `make sample USE_GL_STUBS=1`,
     `make sample`, `make check-state-ownership` (UI-purity / boundary
     guards — submenu render stays snapshot-pure).
   - Update CLAUDE.md (Config Menu section) + MODULES.md if the menu
     ownership notes change.

## Open sub-questions for review

- **All placement:** first row (fast "give me everything") vs. last row
  (sections first, escape hatch below). Recommend **last**, matching the
  examples menu's recent ALL-category placement.
- **Keyboard shortcuts:** F-keys/Ctrl-keys on Config items
  (`g_cfg_items[].key_code`) are unaffected (they dispatch through
  `glr_actions`, not the menu geometry). Decide whether the submenu row
  still shows the shortcut hint (recommend yes — it already has the
  data).
- **Scene-menu regression scope:** Option A edits shared code; the
  Scene example submenu must be behavior-identical after. Gate on the
  existing scene submenu tests staying green.

## Folder note

`in-review` → (pick A or B) → `not-started` → `active` → `done`, or
deleted if dropped.
