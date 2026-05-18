# Plan: Code-panel "focus" toggle — hide boilerplate chrome

## Progress

Branch `feat/code-panel-focus-mode`. Commits ordered for per-commit
buildability (state field before its `config_value_ptr` reference).

- [x] Commit 1 — Step 2: backing state field + default (`make sample` green)
- [x] Commit 2 — Step 1: config key, descriptor, mapping, cycle-row branch (`make sample` green)
- [ ] Commit 3 — Step 3: UI-snapshot mirror + sync
- [ ] Commit 4 — Step 4: gate chrome in code panel (feature live)
- [ ] Commit 5 — Step 5: layout regression test

## Context

The code panel renders a full standalone-C view: workspace-header comments,
`#include` stanza, `void display() {` framing, render-state/camera/lights
setup, the user's commands (variable declarations, `func0..func9` defs, and
the top-level geometry that becomes `display()`'s body), then a footer with
the `init()`/`reshape()` boilerplate. The fixed chrome is useful for
orientation but visually crowds the actual code the user is editing.

The user wants a toggle that switches the panel between **all code** and a
**focused** view that shows only what they typed (variable declarations,
funcN definitions, and the display() body) by hiding the derived C
boilerplate stanzas. Editing must keep working in focused mode.

Key simplification from the design discussion: only the *static chrome
stanzas* are hidden. Those rows are not backed by editor-buffer lines and are
never cursor-navigable, so "still editable / skip hidden lines" needs no
cursor-skipping logic — nothing editable is hidden. The document command
loop, the input/placeholder cursor row, virtual replay rows, and all
scroll/cursor math are untouched.

This is an interface preference (like `Wrap at commas`, `Syntax highlight`,
`Code panel` layout), **not** scene/example state, so it is *not* added to
the `@cfg` scene subset (`cfg_key_in_scene_subset`) or example-reset path.
It *is*, like those siblings, emitted by `glr_export_cfg_fill_all()` and
therefore persisted in single-file / workspace save headers as
`// @cfg code_focus = N` and re-applied on load — that is the intended,
consistent behavior for a global UI pref (it just isn't per-scene).

## Approach

Add a 2-state presentation toggle `GLR_CONFIG_CODE_FOCUS` wired exactly like
`GLR_CONFIG_WRAP_AT_COMMA`: descriptor row, config-key enum, state field,
`config_value_ptr` case, default macro, and a per-frame mirror into the code
panel's UI-chrome snapshot. Then gate the chrome-emitting and
chrome-row-counting code in `src/ui/repl_code_panel.c` on that mirrored flag.

Because `repl_code_panel_header_row_count` / `repl_code_panel_footer_row_count`
iterate the *same* static arrays as the emit block in
`repl_code_panel_build_rows`, gating both with one helper keeps
`layout->header_rows` / `footer_rows` (which drive scroll, cursor-follow, and
`total_lines`) automatically consistent — no separate offset bookkeeping.

## Changes

### 1. Config key + descriptor

- `src/app/glr_config.h:59` — add `GLR_CONFIG_CODE_FOCUS,` to the
  `GlrConfigKey` enum (before `GLR_CONFIG_COUNT`; placing it next to
  `GLR_CONFIG_SYNTAX_HIGHLIGHT` keeps the INTERFACE group together).
- `src/app/glr_actions.c:132-133` — add a descriptor row in the
  `### INTERFACE` block, menu-only (no key binding — F2–F12 and the Ctrl
  shortcuts are taken; matches `Wrap at commas`/`Vertex outlines`):
  ```c
  { "Code focus",       0, 0,  GLR_CONFIG_CODE_FOCUS,          2, NULL,                 0 },
  ```
  `CFG_ITEM_COUNT` auto-recomputes via `sizeof`. Do **not** add a case to
  `cfg_key_in_scene_subset()` (`src/app/glr_actions.c:160`) — it stays a
  global interface pref, like `CODE_PANEL_LAYOUT`/`WRAP_AT_COMMA`.

### 2. Backing state + default

- `src/app/glr_state.h:42-44` — add `int code_focus;` to
  `GlrPresentationState` (next to `wrap_at_comma` / `code_panel_layout`).
- `src/app/glr_defaults.h:44` — add `#define CFG_DEFAULT_CODE_FOCUS 0`
  (off by default — full view stays the default).
- `src/app/glr_state.c:61-63` — add
  `.code_focus = CFG_DEFAULT_CODE_FOCUS,` to the presentation initializer.
- `src/app/glr_config.c:66` — add the mapping case:
  ```c
  case GLR_CONFIG_CODE_FOCUS:          return &glr_state_presentation_mut()->code_focus;
  ```
  (`glr_config_get/set/cycle` and the menu's OFF/ON label render then work
  automatically since `state_names` is NULL with `state_count == 2`.)
- `src/app/glr_actions.c:356-` (`glr_cfg_cycle_row`) — add an
  `else if (item->key == GLR_CONFIG_CODE_FOCUS)` branch in the per-key
  side-effect chain (after the existing `glr_ctrl_sync_ui_chrome()` call at
  line 357). It must call `editor_scroll_follow_cursor_set(1)` and
  `repl_set_status("Code focus: ON" / "...: OFF")`. **Load-bearing
  (review finding P1):** toggling collapses `header_rows` from ~20 to 0,
  but the follow-scroll block in `glr_ctrl.c:2016` only re-centers when
  `scroll_follow_cursor` is set — without this the active edit row scrolls
  off-screen and `scroll` is merely clamped to `max_scroll`. Add
  `#include "editor/state.h"` to `glr_actions.c` if the declaration of
  `editor_scroll_follow_cursor_set` (state.h:404) is not already
  transitively visible there.

### 3. Mirror into the code-panel UI snapshot

- `src/ui/state_types.h:26-35` — add `int code_focus;` to
  `ReplCodePanelRuntimeState` (mirror, same pattern/comment as
  `wrap_at_comma`).
- `src/app/glr_ctrl.c:1989-1996` (`glr_ctrl_sync_ui_chrome`) — add
  `cp->code_focus = p.code_focus;`.

The flag is then readable in the renderer as
`snap->code_panel.code_focus`.

### 4. Gate the chrome in the code panel — `src/ui/repl_code_panel.c`

Add one file-static helper, e.g.
`static int repl_code_panel_chrome_visible(const UiRenderSnapshot *snap)`
returning `!(snap && snap->code_panel.code_focus)`.

- **`repl_code_panel_header_row_count` (lines 146-180):** early-return `0`
  when chrome is hidden.
- **`repl_code_panel_footer_row_count` (lines 182-208):** early-return `0`
  when chrome is hidden.
- **`repl_code_panel_build_rows` (lines 1043-1228):** wrap the header emit
  block (lines **1056-1105**: workspace header, `g_header_pre`,
  `g_display_header`, scratch-decl line, render-state, cam, lights,
  `g_header_post`) and the footer emit block (lines **1193-1225**:
  `g_footer_pre_init`/reshape sentinel, init section, `g_footer_post_init`)
  in `if (repl_code_panel_chrome_visible(snap)) { ... }`.

Leave untouched: the document-command loop (1107-1182), the
input/placeholder cursor row (1184-1191), `repl_code_panel_newline_rows`
(the trailing editable line, line 261 — *not* chrome), virtual replay rows,
and `repl_code_panel_cursor_doc_line_from_layout` /
`..._follow_doc_line_from_layout` (they take `header_rows` as a parameter,
which is now 0 in focus mode — already correct, no edit needed).

No changes to hit-testing or commit: `UiTextPanelRow.source_line_idx` for
command rows is the real command index, unaffected by suppressed static
rows; cursor never targets chrome rows in either mode.

### 5. Regression test (review finding P2b)

This feature is mostly layout math, so add a GL-free regression to
`tests/test_repl_code_panel_document.c` (already builds layouts via its
`build_doc(snap, layout)` helper, which runs `glr_ctrl_build_ui_snapshot`
→ `glr_ctrl_sync_ui_chrome` → `ui_repl_code_panel_build_layout`):

- Feed a few commands, build the layout, capture baseline
  `header_rows`/`footer_rows` (> 0).
- Set focus on (`glr_config_set(GLR_CONFIG_CODE_FOCUS, 1)` so the
  presentation→mirror sync runs inside `build_doc`), rebuild, and assert
  `layout.header_rows == 0` **and** `layout.footer_rows == 0`.
- Assert command 0 maps from doc line `layout.header_rows` (== 0 in focus
  mode): `ui_repl_code_panel_target_for_doc_line(&snap, 0, &layout, ...)`
  resolves to command index 0 — same assertion shape already used at
  test lines 65-72.
- Toggle focus back off, rebuild, assert `header_rows`/`footer_rows`
  return to the baseline (round-trip, no drift).
- Follow-scroll recentering after toggle is controller-level math
  (`glr_ctrl.c:2016`); assert it via the existing
  `glr_ctrl_code_panel_apply_scroll_follow_for_test` seam
  (`glr_ctrl.c:2067`) — with focus toggled and follow requested, the
  resulting scroll keeps `layout.follow_doc_line` within
  `[scroll, scroll + visible_lines)`.

## Files to modify

| File | Change |
|------|--------|
| `src/app/glr_config.h` | add `GLR_CONFIG_CODE_FOCUS` enum |
| `src/app/glr_actions.c` | add `g_cfg_items[]` descriptor row (INTERFACE); add `GLR_CONFIG_CODE_FOCUS` branch in `glr_cfg_cycle_row` (scroll-follow + status); maybe `#include "editor/state.h"` |
| `src/app/glr_config.c` | add `config_value_ptr` case |
| `src/app/glr_state.h` | add `code_focus` to `GlrPresentationState` |
| `src/app/glr_defaults.h` | add `CFG_DEFAULT_CODE_FOCUS 0` |
| `src/app/glr_state.c` | init `.code_focus` |
| `src/ui/state_types.h` | add `code_focus` mirror field |
| `src/app/glr_ctrl.c` | mirror in `glr_ctrl_sync_ui_chrome` |
| `src/ui/repl_code_panel.c` | chrome-visible helper + gate header/footer count & emit |
| `tests/test_repl_code_panel_document.c` | focus-mode layout regression (header/footer==0, doc-line mapping, round-trip, follow-scroll) |

## Verification

1. `make sample && ./sample` — load any example (e.g. F12 to cycle to one
   with a `funcN` def and `float` decls).
2. Open the Config dropdown (or wherever the Config menu is); confirm a
   **Code focus — OFF** row appears under `### INTERFACE`. Toggle it ON.
   - Chrome (workspace header comment, `#include`, `void display() {`,
     render-state/camera/lights setup, `init()`/`reshape()` footer) is gone.
   - Variable declarations, `funcN` definitions, and the display() body
     remain, with correct syntax colors and vertex-index gutter.
   - Scrolling, Up/Down navigation, clicking a line, and committing edits
     (`;`) still land on the correct lines (no off-by-N drift — the row
     count and emit are gated by the same helper).
   - **Scroll-follow on toggle (P1):** with the cursor on a line that is
     mid/late in a long document, toggle focus on then off — the active
     edit row stays visible both times (the new `glr_cfg_cycle_row`
     branch requested follow). Status line shows `Code focus: ON/OFF`.
   - Toggle OFF; chrome returns; cursor position/scroll remain coherent.
3. Exercise replay (Ctrl+R / Replay toggle) with focus ON to confirm
   virtual replay-annotation rows and replay scroll-follow still align.
4. **Persistence round-trip (P2):** with focus ON, Ctrl+S, then inspect
   `output.c` — it should contain `// @cfg code_focus = 1` (emitted by
   `glr_export_cfg_fill_all`, like `wrap_at_comma`/`syntax_highlight`).
   Restart with `./sample output.c` and confirm focus is restored ON.
   Confirm it is **not** written into per-scene `@cfg` blocks on workspace
   save (not in the scene subset).
5. `make test` (includes the new `test_repl_code_panel_document` case)
   and `make check-c99`. Run `make test_repl_core_examples` and
   `make check-state-ownership` to confirm no config-count/ownership
   assertion regressed.
