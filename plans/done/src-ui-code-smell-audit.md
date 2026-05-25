# `src/ui/` — Code-Smell Audit

> Audit produced 2026-05-23. Findings come from four parallel reviews
> of `src/ui/` (menu-bar + scene-tabs; code-panel adapter + panels;
> generic `core/` primitives; floating panels + `UiState`) plus
> targeted spot-verification of the most actionable claims. File:line
> references are exact at the time of writing — check `git log` on the
> cited files before acting if this doc has aged.
>
> Scope: every file under `src/ui/` (`app/` + `core/`). Tests under
> `tests/` were read where they document a contract, but not audited.
>
> The single most important contract for this directory:
> **renderers consume snapshots and never mutate; hit-tests compute a
> neutral `UiHit` and return**. The dominant theme in the findings
> below is *that boundary is leaky*.

## How to read this

Severity grouping mirrors the `src-repl` audit:

- **🔴 Actual bugs (verified)** — correctness or memory-safety issues
  with a concrete failure mode. Pick these up first.
- **🟡 Drift / boundary hazards** — snapshot-purity violations,
  core/app boundary leaks, hand-duplicated geometry. Working today; a
  one-side edit will silently diverge.
- **🟢 Dead code / dead fields** — code with no callers, unreachable
  branches, unused parameters. Pure surface reduction.
- **🔵 Structural concerns** — long functions, misnamed entry points,
  magic numbers. Bigger refactors; higher cost.

Each finding cites file + line, names the smell, says why it matters,
and suggests a one-line fix.

## Closeout (2026-05-24)

The audit has been closed out across five commits and a small final
gap-fix pass. Status per finding (numbers reference the headings
below):

**🔴 Bugs (all done in `fd70b4e`):** #1, #2, #3, #4, #5, #6.

**🟡 Drift / boundary hazards:**

- *Done:* #7, #8, #9, #10, #11, #12, #13, #14, #15, #16, #17, #19, #20,
  #21, #22, #23, #24, #26, #27, #28, #29 (across `a8c911a`, `7f5f782`,
  `4a33445`). #31 done in the closeout commit (locale-safe
  `ascii_tolower` in `text_search.c`).
- *Done in backlog pass (2026-05-24 second closeout — see below):*
  #18 (drag-state slice on snapshot, **Tier B**, commit `783d7e3`),
  #25 (replay HUD takes `const UiRenderSnapshot *`, **Tier B**,
  commit `783d7e3`).
- *Deferred:* #30 (`glIsEnabled(GL_BLEND)` save/restore in the
  fake-bold per-segment loop — sorted into **Tier C** by the
  2026-05-24 backlog pass; needs a row-level boolean threaded down).

**🟢 Dead code / dead fields:**

- *Done:* #32, #33, #34, #35, #36, #38, #39, #40, #41, #42, #43 (across
  `cf6157c` and `4a33445`).
- *Kept on purpose (Tier D):* #37 —
  `editor_search_find_{next,prev}_in_text` pass-through wrappers
  preserve the `editor_*` ↔ `ui_text_*` prefix boundary; deleting
  them would force editor-namespaced callers (search.c itself,
  `test_repl_core_search_extra.c`) to reach across.
- *Claim incorrect (Tier D):* #44 — the conditional `resolved_line`
  override for `UI_TEXT_PANEL_ROW_VIRTUAL` is load-bearing.
  `test_ui_text_panel.c:190` ("virtual row keeps line_idx unresolved
  in generic hit") fails without it because the adapter relies on the
  generic hit-tester returning `-1` so it can rewrite the line. The
  override now carries a comment explaining why; finding stands but
  the fix doesn't.

**🔵 Structural concerns:**

- *Done:* #57 (defensive `!snap` guards collapsed in `cf6157c`), #61
  (`MENU_TEXT_INSET_X` substitution). Closeout-commit fixes: #56
  (`STATIC_ASSERT` linking `TUTORIAL_FADE_SETTLE_CHARS` to the
  color-segment budget), #59 (empty-message guard in
  `ui_state_status_set_kind`), #60 (promoted
  `ui_layout_code_panel_layout_mode` to `layout.h`, deleted the
  variable-panel duplicate), #62 (re-indented stray two-space block in
  `text_panel.c`).
- *Deferred (Tier C):* #45, #46, #47, #48, #49, #50, #51, #52, #53,
  #54, #55, #58, #63. The long-function / row-cache / hot-path-static-
  buffer refactors. Each is a real follow-up but cost-per-finding is
  high relative to the rest of the list. None are bugs.

Verification (original closeout): `make check-c99`,
`make check-state-ownership`, `make test-stubs` clean (45/45 binaries,
6776/6776 tests). Backlog-pass closeout (2026-05-24): re-verified
with the same gates (45/45 binaries, 6815 tests after #25's HUD
signature change dropped 4 assertions on the deleted
`UiReplayHudState.scene_x/y/w/h` fields).

### Backlog pass (2026-05-24)

A follow-up review on 2026-05-24 reopened the partial/deferred items
and triaged them by **the same Tier system used in the sibling
`src-repl-code-smell-audit.md`** (reproduced here for stand-alone
reading):

- **Tier A — small, real fix, near-zero risk.** 5–30 line changes,
  no architectural exposure.
- **Tier B — moderate effort, clear value.** 50–200 line changes,
  one or two files of churn, a real test impact.
- **Tier C — defer (high cost, low payoff).** Real wins but touch
  a wide surface; revisit when the surrounding code is being
  reworked anyway.
- **Tier D — keep deferred.** Findings the audit landed wrong, or
  where the original intent (a kept-on-purpose trampoline, a
  test-pinned bug) makes "fixing" them worse than living with them.

Sorted into action tiers, the still-open items landed as:

- **Tier A:** **#29** confirmed already done before the audit — the
  table and active-theme global live in `theme.c` (with `extern`
  declarations in the header) since the original closeout; only the
  stale "Header-only (mirrors gl_2d.h)" claim in the file comment
  was fixed (`f5d0c50`).
- **Tier B (done — commit `783d7e3`):**
  - **#18** — Add `UiVariableDragView` slice to `UiRenderSnapshot`;
    `ui_variable_panel_render` reads `snap->variable_drag.*` instead
    of calling `variable_panel_drag_active_var()` / `_log_mode()`
    live. Closes the gap left by the original closeout.
  - **#25** — Reshape `replay_ui_hud_render` to take
    `const UiRenderSnapshot *`. Deletes the 14-field
    `UiReplayHudState` struct (the controller's 1832-1851 block
    collapses to `replay_ui_hud_render(&ui_snap)`); HUD reads
    `snap->replay.*`, `snap->code_panel.layout_mode`,
    `snap->viewport.*`, and calls `ui_layout_scene_rect()` itself.
    The "HUD shows pre-prepare replay" contract is preserved by
    briefly overriding `ui_snap.replay` with the pre-prepare
    `frame_replay` capture across the HUD call — other `snap->replay`
    readers only touch fields `replay_prepare_frame` doesn't write
    (`src_line_idx`, `active`).
- **Tier C (deferred at the time of this backlog pass):** #30, plus
  the structural cluster #45–#55 + #58, #63. *Most of these were
  closed in the 2026-05-25 follow-up pass — see the next section
  for the running status.*
- **Tier D (still kept):** #37, #44.

After this pass the only open items in the audit are Tier C (real
wins, high cost) and Tier D (kept on purpose). No bug-level
findings remain.

## Progress update — 2026-05-25

Re-verified each Tier C finding against the current source. Most of
the structural cluster has been quietly closed in the natural course
of subsequent menu-bar / code-panel work. Updated per-heading markers
to match.

**Closed since the backlog pass** (11 of 14, including #55 + #63
which were the remaining "Still open" pair until the same-day
follow-up, plus #50 finished off in a third same-day micro-pass):

- **#30** — `glIsEnabled(GL_BLEND)` is gone from `text_panel.c`; the
  fake-bold per-segment query/restore has been removed entirely.
- **#45** — `ui_menu_bar_render` went 140 → 40 lines, factored into
  `paint_menu_labels` + `paint_pin_buttons` exactly as the audit
  recommended.
- **#48** — `ui_repl_code_panel_render` was renamed to
  `ui_repl_code_panel_render_with_chrome` (the audit's recommended
  fix word-for-word).
- **#49** — Hit-test now reuses a cached builder keyed on the
  snapshot pointer (`g_builder_cache`) instead of rebuilding rows
  from scratch.
- **#51** — Both `menu_dropdown_rect` and `submenu_rect` cache their
  row-measurements; the per-call `strlen(label) + strlen(shortcut)`
  scan is gone.
- **#52** — `menu_item_label` no longer returns a `static char buf[48]`;
  every branch returns a string literal or a name from another module.
- **#53** — No more hand-rolled ASCII title-case math in
  `menu_bar.c`; the menu rows are static literal strings.
- **#54** — `repl_code_panel_newline_rows` renamed to
  `repl_code_panel_trailing_row_count` (the audit's recommended name).
- **#55** — `ui_panels_render_scene_status` 118 → 95 lines and
  `draw_modal_strip` 44 → 26 via extracted `status_strip_begin` /
  `_paint_bar` / `_end` shared by both call sites.
- **#63** — Per-frame `g_wrap_cache` in `text_panel.c` populated by
  `ui_text_panel_render`; `ui_text_panel_hit_test` and
  `ui_text_panel_input_row_y` now read through
  `text_panel_row_wrap_count_cached(snap, row_idx)`, removing the
  per-row `code_layout_row_count_for_text` walk that the renderer
  already did.
- **#50** — `MENU_LABEL_PAD_X` + `SUBMENU_ARROW_COL` were named in
  earlier work; the `+ 4` row-pad / `+ 8` extra-height are all
  expressed via `DROPDOWN_PAD_Y`; and the audit's last open piece —
  the triplicated `(top + h - DROPDOWN_PAD_Y - ry) / LINE_H`
  ordinal-from-y formula — is now `dropdown_row_for_gl_y(top, h, ry)`,
  used by `ui_menu_bar_dropdown_item_hit`, `submenu_hit_test`, and
  `submenu_hover_ordinal`.

**Partially closed** (3 of 14) — the function shape improved but
the underlying smell wasn't fully resolved:

- **#46** — `render_active_submenu` went 106 → 82 lines, but the
  Config-special-case columns (`cfg_max_sc`, `cfg_state_right`) are
  still hardcoded inline. The audit's full fix (a `FlyoutColumns`
  struct + per-provider draw-row-decoration hook) hasn't landed.
- **#47** — `ui_menu_bar_hit_test` went 87 → 61 lines and the
  submenu pass was extracted into `submenu_hit_test`. The "two
  duplicate chrome blocks" finding is mostly resolved (one is now in
  the submenu helper) but the inline `inert_chrome_hit` branch is
  still there.
- **#58** — `(int)strlen(text)` repetition went from 5+ sites
  (`fade`, `virtual-row build`, `statusbar layout`) down to 1
  remaining site (`repl_code_panel.c:815`). The `UiTextPanelRow`
  text-len cache the audit suggested wasn't added; the other call
  sites were just deleted along the way.

**Still open** (0 of 14):

(All Tier C now closed; the two stragglers below were picked up in
the same 2026-05-25 pass.)

- **#55** — *closed.* Extracted `status_strip_begin` /
  `status_strip_paint_bar` / `status_strip_end` in `panels.c`;
  `draw_modal_strip` and the status banner block both use them.
  `ui_panels_render_scene_status` went 118 → 95 lines;
  `draw_modal_strip` went 44 → 26 lines.
- **#63** — *closed.* Added a per-frame `g_wrap_cache` to
  `text_panel.c` keyed on snapshot + cp_w + wrap_at_comma + text_x.
  `ui_text_panel_render` populates the cache as a side effect of its
  row walk; `ui_text_panel_hit_test` and `ui_text_panel_input_row_y`
  read through `text_panel_row_wrap_count_cached(snap, row_idx)`.
  The 5 inline `text_panel_row_layout(snap, row)` calls remain
  (they're tiny — 1-line layout builds; the hot path was the
  `code_layout_row_count_for_text` re-walk, which is now cached).

**Tier D — still warranted** (no change):

- **#37** — Search pass-through wrappers still kept for the
  `editor_*` ↔ `ui_text_*` namespace boundary.
- **#44** — The conditional `resolved_line = source_line_idx` override
  for `UI_TEXT_PANEL_ROW_VIRTUAL` is still load-bearing
  (`text_panel.c:835-846`), test `test_ui_text_panel.c:190` still
  pins the contract.

Bottom line: 13 of the audit's 63 findings are now closed beyond the
original closeout (2 in the 2026-05-24 backlog pass — #18, #25 —
plus 11 here, including the 2 stragglers #55 and #63 and the
finally-finished #50); 3 partially closed (#46, #47, #58); only
**2 Tier D items remain** (#37, #44) — both kept on purpose. **No
Tier C items remain.** The audit is materially complete; the only
remaining work is the explicit Tier D carve-outs and the 3 partials
where the structural shape improved but the audit's full fix
didn't fully land.

## 🔴 Actual bugs (verified)

### 1. `tabbed_overlay.c` reads past NUL on short lines (done in `fd70b4e`)

**Where:** `src/ui/core/tabbed_overlay.c:261`

**Smell:**
```c
} else if (text[i][2] == ' ' && text[i][3] == ' ') {
```
Path is reached when `text[i][0] == ' '`. If `text[i]` is `" "`
(length 1) or `"  "` (length 2), the `[2]` / `[3]` reads run past
the NUL.

**Why it matters:** Undefined behavior in the help-overlay renderer
for any 1- or 2-character whitespace-led line. The earlier guard
filters `'\0'` at position [0] but not "single space" or "two spaces".

**Fix:** Replace with explicit length check, e.g.
`text[i][1] == ' ' && text[i][2] == ' ' && text[i][3] != '\0' && text[i][3] == ' '`,
or call `strlen(text[i]) >= 4` first.

### 2. `variable_panel.c` calls math/string functions without their headers (done in `fd70b4e`)

**Where:** `src/ui/app/variable_panel.c:65, 76-77, 124, 160, 249, 287`
(plus `src/ui/app/autocomplete_panel.c:37`)

**Smell:** Calls `fabsf`, `asinhf`, `lroundf`, `strlen`, `snprintf`
with no `<math.h>` / `<string.h>` / `<stdio.h>` include. Works only
via transitive includes; `-std=c99` warns on implicit declarations
and treats them as hard errors under the project's `make check-c99`
ratchet.

**Why it matters:** One removed transitive include breaks the C99
build everywhere this pattern hides — and the project explicitly
targets old-gcc.

**Fix:** Add the explicit includes.

### 3. Renderer mutates state during render (`menu_bar`) (done in `fd70b4e`)

**Where:** `src/ui/app/menu_bar.c:1525-1527` inside
`ui_menu_bar_render_example_dropdown`

**Smell:** The renderer calls `ui_menu_bar_update_pointer_hover()`,
which writes `g_menu_item_hover`, `g_submenu_menu_id`,
`g_submenu_parent_row`, `g_submenu_open_time`. The README says
explicitly: "A UI renderer may draw … it does not read live program
or editor state, and it does not mutate anything."

**Why it matters:** The controller's input-time
`ui_menu_bar_update_pointer_hover` call (`glr_ctrl.c:3818`) already
covers hover refresh. The render-time duplicate is the path that
couples render to mutation.

**Fix:** Delete lines 1525-1527; trust the input-time hover refresh.

### 4. Hit-test handler mutates subsystem state (done in `fd70b4e`)

**Where:** `src/ui/app/menu_bar.c:986-1002` (forwarded one-line from
`src/ui/app/panels.c:247-249`)

**Smell:** `ui_menu_bar_handle_config_right_press` calls
`glr_cfg_cycle_row(h.item_idx, -1)` directly. UI input handlers are
supposed to *classify* into a `UiHit`; the controller routes; the
owning subsystem implements behavior. Today the UI layer encodes the
policy "right-press means cycle backward".

**Why it matters:** Boundary violation per the audit contract; the
UI module shouldn't know `glr_cfg_cycle_row` exists.

**Fix:** Surface as a `UI_HIT_SUBMENU_ITEM` flavor (or include a
direction hint); let `glr_ctrl` call `glr_cfg_cycle_row`.

### 5. Replay / selection / feeding-normal / color overlays silently disappear on the edit row (done in `fd70b4e`)

**Where:** `src/ui/app/repl_code_panel.c:1065`
(`repl_code_panel_apply_command_overlays` called only from
`add_command_row`; not from `add_input_row` at L1234)

**Smell:** When the user is editing in-place on the line being
replayed (`is_edit==1`), `add_input_row` runs instead of
`add_command_row`. The green replay background/marker, blue
feeding-normal marker, line selection band, etc. don't apply.

**Why it matters:** Visible inconsistency — the same source line
draws different decorations depending on whether you're editing it
or not.

**Fix:** Call
`apply_command_overlays(builder, source_line_idx, row)` from
`add_input_row` when `source_line_idx >= 0` (edit-in-place); keep
insert-row (-1) unaffected.

### 6. Marker-color priority is decided by silent assignment order (done in `fd70b4e`)

**Where:** `src/ui/app/repl_code_panel.c:649-676`

**Smell:** Replay sets marker green; `highlight_normal_idx`
overwrites blue; `highlight_color_idx` overwrites yellow;
`apply_tutorial_insertion_marker` overwrites pink. The comment
documents "tutorial-insertion wins over feeding-normal/color" but
the assignment order *also* makes feeding-normal beat replay —
which is **not** commented and may be unintended.

**Why it matters:** Drift-prone — the implicit ordering can flip
under a small refactor with no test signal.

**Fix:** Pick the marker with an explicit priority enum
(`TUTORIAL_INSERTION > FEEDING_COLOR > FEEDING_NORMAL > REPLAY`)
computed in one place, assigned once.

## 🟡 Drift / boundary hazards

### 7. `core/layout.c` includes `ui/app/state_types.h` (core→app boundary leak) (done in original closeout)

**Where:** `src/ui/core/layout.c:5` (`#include "ui/app/state_types.h"`)
+ forward-declares `ui_state_viewport()` / `ui_state_code_panel()`

**Smell:** The README and `MODULES.md` say "`core/` never includes
from `app/`." `layout.c` does, and reaches into live `UiState` to
back its "pure geometry" entry points. The `check-ui-text-panel-pure`
guard only covers `text_panel.{c,h}`; nothing enforces purity for
`layout.c`.

**Why it matters:** Most load-bearing boundary violation in the
directory. Tools reusing `core/` (notably `editor_demo`) silently
inherit a coupling to the REPL's `UiState`.

**Fix:** Move `ui_layout_*` to `src/ui/app/layout.c` (it's an app
concern), or refactor to take
`(panel_frac, layout_mode, win_w, win_h)` as args and let the
caller plumb them. Extend the purity guard to cover `layout.c`.

### 8. `core/hit.h` enumerates 16 app-specific `UiHitKind` values (done in original closeout)

**Where:** `src/ui/core/hit.h` —
`UI_HIT_REPLAY_BUTTON`, `UI_HIT_HELP_TOGGLE`, `UI_HIT_HELP_PANEL`,
`UI_HIT_COLOR_SWATCH`, `UI_HIT_INLINE_COLOR_SWATCH`,
`UI_HIT_VARIABLE_SLIDER`, `UI_HIT_CODE_PANEL_TAB`,
`UI_HIT_MENU_BUTTON`, `UI_HIT_MENU_ITEM`, `UI_HIT_SUBMENU_ITEM`,
`UI_HIT_PIN_BUTTON`, `UI_HIT_CODE_FOCUS_TOGGLE`, `UI_HIT_SCENE`,
`UI_HIT_CODE_PANEL_CHROME`, etc.

**Smell:** `text_panel.c` itself emits only 5 of the 19 kinds
(NONE, PANEL_DIVIDER, CODE_GUTTER, CODE_INSERT_LINE, CODE_TEXT).
Header docs explicitly name `glr_ctrl_toggle_help` /
`glr_ctrl_toggle_code_focus` (file:67, 70) and `g_cfg_items[]`
(file:99) — those names belong in app/.

**Why it matters:** `editor_demo` and any future tool reusing
`core/` carries 14 unused enum values with REPL-app baggage in the
docs.

**Fix:** Keep only generic kinds in `core/hit.h`; move feature
kinds to `src/ui/app/hit.h` extending `UiHitKind` via a numeric
carve-out, or use an opaque int.

### 9. `core/metrics.h` defines `STATUSBAR_H` / `TAB_STRIP_H` (feature heights leaking into "shared" header) (done in original closeout)

**Where:** `src/ui/core/metrics.h`

**Smell:** `STATUSBAR_H = 22` is the height of one specific
chrome element of the REPL app; `TAB_STRIP_H` is used solely by
`src/ui/app/scene_tabs.c`. Both are REPL-specific design values
sitting in the "shared constants" header.

**Why it matters:** A new tool reusing `text_panel` inherits a 22px
banner reserve it has no banner for.

**Fix:** Pass `statusbar_reserve_h` numerically through the snapshot
(symmetric with the existing `top_chrome_h`); remove `STATUSBAR_H`
and `TAB_STRIP_H` from `core/metrics.h`.

### 10. `core/text_panel.h` doc comments name editor-private symbols (done in original closeout)

**Where:** `src/ui/core/text_panel.h:138-141, 206`

**Smell:** `search_row_idx` is documented as "Row index in the
editor search row space (see `editor_search_row_for_cmd_index`)".
The whole point of `core/text_panel.h` is to be REPL/editor-free —
even comment-level coupling forces co-edits.

**Fix:** Re-document as "opaque adapter-defined IDs compared for
equality only"; drop the editor symbol reference.

### 11. Code-panel renderer reaches past the snapshot for live per-line text (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:263-265, 578-584`
(`repl_code_panel_command_main_rows`, `_display_text`)

**Smell:** Both call `editor_state_line_override_for(line_idx)` and
`editor_buffer_line(line_idx)` directly (live `g_editor_state` reads
via `src/editor/state.c:84, 728`). Per-line text is the most basic
input — it should arrive as a `UiBufferView` field on the snapshot.

**Why it matters:** A mid-render mutation can yield rows that
disagree with the snapshot's `document_cmds[]`.

**Fix:** Thread an `editor_buffer_view` + `editor_line_overrides`
slice through `UiRenderSnapshot`; drop the live reads.

### 12. Virtual-line list read inconsistently — once from snapshot, once from live state (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:281` (precompute, live read)
vs. L1083 (add rows, snapshot read)

**Smell:** Same data, two access paths. Layout's `replay_extra_rows[]`
can silently disagree with what `add_virtual_rows` actually emits.

**Fix:** Count virtual lines from `builder->snap->editor_virtual_lines`
in `precompute_layout_rows`.

### 13. Syntax classifier advertised as pure but reads live predef-var table (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:909`
(`ui_repl_code_panel_classify_syntax`)

**Smell:** Calls `repl_eval_find_predef_var_idx(name)`, which scans
live `g_predef_vars`. The header at L49 says: "Pure: no global
state, safe to call from tests."

**Why it matters:** Tests can get cross-contamination through the
predef table.

**Fix:** Take a variable-name set as parameter; declare the existing
function impure in the header.

### 14. Tutorial fade pulled live per fading row (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:694, 725, 750, 1066`

**Smell:** Calls `tutorial_step_fade_front/_settle/_alpha`,
`tutorial_line_is_fading` — each does its own `tutorial_state_view()`.
`apply_fade_segments` calls `_settle` inside a per-char loop — up
to ~6 separate state reads per fading line per frame. No snapshot
field for tutorial fade exists.

**Fix:** Add a `TutorialFadeView` to the snapshot capturing
`{active, line_idx, line_len, fade_start_t, fade_duration}`;
derive front/alpha/settle from snapshot fields.

### 15. Header/footer row-count and emit paths call live REPL/export state (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:186, 191, 201-205, 232-237`
(row-count) and L1174-1199, L1289-1320 (build)

**Smell:** `repl_export_lights_display_line()`,
`repl_export_init_section_line_count()`,
`repl_export_init_section_line()` read `repl_state_render()` and
live `g_export_cfg_bridge`. Two-pass counting + building both pull
the same live data; a mid-frame config change can desync them.

**Fix:** Pre-materialize the lights and init-section lines into the
snapshot (mirror `import_export.render_state_lines` / `cam_lines`).

### 16. Menu-bar renderer reads ~51 live values vs ~17 snapshot reads (done in original closeout)

**Where:** `src/ui/app/menu_bar.c:1247, 1077, 1085, 1232, 1520, 1571,
1577, 1578` and submenu_rect / menu_dropdown_rect

**Smell:** Render-time calls to `glr_config_get(item->key)`,
`tutorial_active()`, `tutorial_state_view().tutorial_idx`,
`repl_user_scene_count()`, `glr_scene_menu_slot_for_dense_index()`,
`repl_example_visible_tag_count()`. Compare with `scene_tabs.c`,
which is exclusively snapshot-driven and stays clean.

**Why it matters:** `menu_bar.c` is 1618 lines mainly *because* the
abstraction is leaky, not because the menu surface is inherently
complex.

**Fix:** Push `cfg_states[]`, `active_tutorial_idx`, resolved
per-row "scene slot" and "is active" booleans into the snapshot.

### 17. `UiState` carries cursor blink (CLAUDE.md violation) (done in original closeout)

**Where:** `src/ui/app/state_types.h:29-30` and `state.c:26-27`

**Smell:** `UiCodePanelRuntimeState` (field of `UiState`) carries
`cursor_visible` and `blink_tick`. CLAUDE.md explicitly states:
"`UiState` owns chrome ONLY … **NOT cursor blink**." Advance lives
in `glr_ctrl_tick` (`src/app/glr_ctrl.c:3917-3923`) via
`ui_state_code_panel_mut()`.

**Why it matters:** Load-bearing example of the boundary slipping.

**Fix:** Move both fields to an editor-session slice next to
`scroll` (which the same comment says is on `EditorState`).

### 18. Variable panel reads peer live state instead of snapshot (done — Tier B, commit `783d7e3`)

**Where:** `src/ui/app/variable_panel.c:113, 195, 273-274, 307-308`

**Smell:** `if (replay_active())` calls peer file-static; same value
already on `snap->replay.active`. `variable_panel_visible()` /
`variable_panel_drag_active_var()` / `_drag_log_mode()` directly
read peer file-statics; `visible` is already on
`snap->variable_panel.visible`.

**Fix:** Pass through `snap->replay.active` and snapshot's
visibility; surface drag state via a new `UiVariableDragView` slice
on the snapshot.

### 19. "Shared flyout engine" has 21 menu-id branches and three classifiers (done in original closeout)

**Where:** `src/ui/app/menu_bar.c:553-645, 765-770, 625-645, 803`

**Smell:** README claims one generic `(menu_id, parent_row)` engine
shared by Scene examples, Tutorials, and Config. In practice:
- `submenu_row_count/_label/_abs_index/_kind` each carry an explicit
  3-way switch (`MENU_SCENE` / `MENU_TUTORIALS` / `MENU_CONFIG`).
- Only Scene + Tutorials share `CatalogFlyoutOps`; Config has a
  parallel `config_submenu_abs_index` path with different semantics.
- 21 `if (menu_id == MENU_*)` branches across the file.
- Two chrome classifiers co-exist: `menu_chrome_kind(label)` (decodes
  `"### "` / `"---"` string prefix) and `submenu_row_kind(menu_id,
  parent_row, ordinal)` (vtable-driven).

**Why it matters:** The "shared engine" framing makes growth look
cheap; in practice every new menu has to be added to ~4 switches.

**Fix:** Extend `CatalogFlyoutOps` (or define a richer
`FlyoutProvider`) and register one for Config that calls
`glr_config_section_range` / `glr_config_row_kind` internally.
Collapse the 3-way switches to ops-table dispatch.

### 20. Color picker bypasses theme tokens (done in original closeout)

**Where:** `src/ui/app/color_picker.c:45, 47, 174`

**Smell:** `glColor4f(0.08f, 0.08f, 0.12f, 0.94f)` for popup BG,
`glColor4f(0.30f, 0.30f, 0.50f, 0.80f)` for popup border,
`glColor3f(0.4f, 0.4f, 0.5f)` for swatch outline. Sibling panels
(`replay_hud.c:61, 63`, `autocomplete_panel.c:51, 55`,
`profile_panel.c:223, 227`) all use `ui_clr_a(UI_TOK_SUNKEN, …)` /
`ui_clr_a(UI_TOK_BORDER, …)`.

**Why it matters:** A theme switch won't restyle the picker frame
consistently with its neighbors.

**Fix:** Replace with `ui_clr_a(UI_TOK_SUNKEN, 0.94f)` /
`ui_clr_a(UI_TOK_BORDER, 0.80f)` / `ui_clr(UI_TOK_BORDER)`.

### 21. Cross-panel layout coupling: `variable_panel` includes `replay_hud.h` (done in original closeout)

**Where:** `src/ui/app/variable_panel.c:20, 88, 101`

**Smell:** Variable panel includes `ui/app/replay_hud.h` solely for
`REPLAY_HUD_BOTTOM_Y` (a geometry constant). The variable panel
"lifts itself" above the replay HUD using a hardcoded sister's
footprint.

**Why it matters:** A HUD geometry change requires touching the
variable panel; the replay subsystem's UI module shouldn't be in
the dependency graph of generic variable-panel chrome.

**Fix:** Compute the lift target in the controller (which already
knows both rects) and pass on the snapshot as
`snap->variable_panel.replay_lift_px`. Remove the
`#include "ui/app/replay_hud.h"`.

### 22. Render-order coupling via file-static lift (done in original closeout)

**Where:** `src/ui/app/variable_panel.c:96-97, 160`,
`src/ui/app/profile_panel.c:62-65`

**Smell:** `g_var_panel_replay_lift_px` is set inside
`ui_variable_panel_render` and read by
`ui_variable_panel_rect_for_count` (called by
`profile_panel_rect_for_height`). Today the controller renders
variable panel before profile panel so this is safe; a reshuffle
silently desyncs the profile panel's anchor.

**Fix:** Compute the lift in the snapshot phase
(controller-driven); store on `UiRenderSnapshot`.

### 23. Three duplicate copies of the `panel_y` clamp around statusbar (done in original closeout)

**Where:** `src/ui/app/variable_panel.c:161-170`,
`profile_panel.c:42-49`, `replay_hud.c:33-48`

**Smell:** Identical pattern `min_y = sc_y + STATUSBAR_H + 4;
max_y = sc_y + sc_h - panel_h - 4; if (max_y >= min_y) clamp else
fallback`. Two of three already drift on the "fall back to top of
scene if `CODE_PANEL_LAYOUT_TOP`" detail.

**Fix:** Add `ui_clamp_panel_y(scene_rect, panel_h, requested_y,
layout_mode)` in `ui/core/layout.{c,h}`.

### 24. Every floating panel hand-rolls its own bg + border frame (done in original closeout)

**Where:**
`src/ui/app/{variable_panel,profile_panel,replay_hud,autocomplete_panel,color_picker}.c`
— 9 occurrences of `glRectf` + `glBegin(GL_LINE_LOOP)`.

**Fix:** Add `gl2d_panel_frame(x, y, w, h, bg_tok, border_tok,
bg_alpha, border_alpha)` to `ui/core/gl_2d.{c,h}`; migrate.

### 25. Replay HUD ignores `snap->replay` (done — Tier B, commit `783d7e3`)

**Where:** `src/ui/app/replay_hud.h:35-50`,
`src/app/glr_ctrl.c:1699-1714`

**Smell:** The controller builds a separate `UiReplayHudState`
struct mostly re-reading fields already on `snap->replay` /
`snap->scene_config`. Three callee shapes co-exist across the
panels: `(const UiRenderSnapshot *)` (variable_panel / profile /
autocomplete), `(const ColorPickerView *, int, int)`
(color_picker), `(const UiReplayHudState *)` (replay_hud).

**Fix:** Have `replay_ui_hud_render` take
`const UiRenderSnapshot *` (or take the replay-state view
explicitly); collapse to one shape.

### 26. Menu-bar top math duplicated in `scene_tabs.c` (done in original closeout)

**Where:** `src/ui/app/menu_bar.c:419-420`,
`src/ui/app/scene_tabs.c:62-63`

**Smell:** Both compute `int by = panel_top - CODE_MARGIN_Y - LINE_H;`.

**Fix:** Add `ui_layout_menu_bar_rect(...)` to
`src/ui/core/layout.h`; call from both.

### 27. Geometry duplicated: `idx_x` / `linenum_w` formula across `init_builder` and `glr_ctrl.c` (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:495-515`,
`src/app/glr_ctrl.c:2190-2200`

**Smell:** Both compute `linenum_w = 4 * FONT_W; idx_col_w =
show_vertex_indices ? 6 * FONT_W : 0; idx_x = CODE_MARGIN_X +
linenum_w + FONT_W; text_x = idx_x + idx_col_w;` from scratch.

**Fix:** Extract
`ui_repl_code_panel_compute_text_x(const UiRenderSnapshot *)` into
`repl_code_panel.h`; call from both sites.

### 28. Color-swatch hit math hand-duplicated from text_panel draw math (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:1676-1678` vs.
`src/ui/core/text_panel.c:246`

**Smell:** Same `swatch_x = … - CODE_MARGIN_X -
UI_TEXT_PANEL_RIGHT_ACTION_W - 2;` formula at two sites.

**Fix:** Extract `ui_text_panel_right_action_rect(snap, line_y,
&sx, &sy, &sw)` in `text_panel.{c,h}`.

### 29. `theme.h` declares a static color table in a header included by 10 TUs (already done before audit; comment-only update in `f5d0c50`)

**Where:** `src/ui/core/theme.h` (174 lines, `static int
g_ui_theme;` + `static const UiRgba g_ui_theme_table[][]`)

**Smell:** Every TU gets its own copy of the ~456-byte table.
`ui_theme_select` only mutates the current TU's copy; the in-file
comment already acknowledges "a real runtime switcher must
relocate this to one .c TU."

**Fix:** Move the table + the static into a new `ui_theme.c`;
promote getters/setters to extern; keep `theme.h` for declarations
only.

### 30. `text_panel.c` query-and-restore of `GL_BLEND` in tight per-segment loop (done — Tier C closed 2026-05-25)

**Where:** `src/ui/core/text_panel.c:113`

**Smell:** `glIsEnabled(GL_BLEND)` save/restore inside the
fake-bold pass — known to stall some drivers. Called per character
segment, per frame. The row-level path already enables/disables
BLEND once.

**Fix:** Pass the row-level "blend is on" bool down; remove the
per-segment query.

### 31. `tolower` is locale-dependent in `text_layout.c` (done in original closeout)

**Where:** `src/ui/core/text_layout.c:24`

**Smell:** `tolower(tc)` respects the current C locale. Under a
Turkish locale the `'I'/'i'` mapping is wrong. The project is
ASCII-only (per CLAUDE.md).

**Fix:** Use a local
`static inline int ascii_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }`,
or document the ASCII assumption at the file top.

## 🟢 Dead code / dead fields

### 32. `ui_repl_code_panel_render` builds a `UiReplCodePanelLayout` it never uses (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:1603, 1615-1617`

**Smell:** `layout` is a stack local, populated by
`ui_repl_code_panel_build_layout`, then never referenced before
function return. `build_layout` is non-trivial (header/footer wrap
math + per-command wrap rows). Pure dead work per frame.

**Fix:** Delete the call. (Legitimate use is in `glr_ctrl.c:2206`,
separate.)

### 33. `text_panel_draw_indent` is a visual no-op (done in original closeout)

**Where:** `src/ui/core/text_panel.c:351-364`

**Smell:** Draws spaces with `gl2d_draw_string` — GLUT bitmap-font
spaces paint no pixels. Called twice (L428, L526). `glColor3fv` is
set but the only effect is advancing raster position, which is
discarded.

**Fix:** Delete the function and both call sites, or implement
dotted indent guides if that was the intent.

### 34. Stale public API: three `int`-returning hit-test entry points plus the `UiHit` one (done in original closeout)

**Where:** `src/ui/app/menu_bar.h:72, 77, 82` /
`menu_bar.c:449, 463, 792`

**Smell:** `ui_menu_bar_menu_hit`, `_pin_hit`, `_dropdown_item_hit`
return raw `int` (-1 or index). Header docstrings say "Called by
ui_panels.c on left-click" but `grep` shows `panels.c` no longer
calls any of them — only the rich `UiHit` form is used externally;
the int variants only serve tests + internals.

**Fix:** Demote to `static`; update tests to drive through
`ui_menu_bar_hit_test`; remove the stale "Called by ui_panels.c"
comments.

### 35. `ui_menu_bar_example_dropdown_is_open()` is a single-line alias used only by tests (done in original closeout)

**Where:** `src/ui/app/menu_bar.c:81`, `menu_bar.h:140`

**Smell:** Returns the same value as
`ui_menu_bar_menu_dropdown_is_open()`. Two `grep` hits, both in
tests.

**Fix:** Delete; have tests call the canonical name.

### 36. Two near-duplicate test-helper rect getters (done in original closeout)

**Where:** `src/ui/app/menu_bar.h:87-96` / `menu_bar.c:747-759`

**Smell:** `ui_menu_bar_scene_example_submenu_rect_for_test` and
`ui_menu_bar_tutorial_submenu_rect_for_test` are 4-line wrappers
around `ui_menu_bar_submenu_rect_for_test` that just convert
`tag_idx → parent_row`.

**Fix:** Drop the tag-indexed wrappers; expose
`scene_parent_row_for_tag` / `tutorial_parent_row_for_tag` and let
tests compose.

### 37. `editor_search_find_next_in_text` / `_prev_` are pass-through trampolines (Tier D — kept on purpose)

**Where:** `src/editor/search.c:57-65`

**Smell:** Both wrappers `return ui_text_find_{next,prev}_in_text(...)`
with identical signature.

**Fix:** Delete the wrappers; call `ui_text_*` directly.

### 38. Five exported `ui_state_*` functions have zero callers (done in original closeout)

**Where:** `src/ui/app/state.c:76-85, 95-97, 142-144, 154-156`

**Smell:** `ui_state_status_clear`, `_status_tick`, `_help_reset`,
`_code_panel_reset`, `_pointer_set_button` declared in `state.h`
and defined; never called in `src/` or `tests/`. `glr_ctrl_tick`
inlines the equivalent of `status_tick` via direct `_mut()`.

**Fix:** Delete the unused exports (or wire `glr_ctrl_tick` through
`ui_state_status_tick` so it gets coverage).

### 39. `is_clear` field on `UiTransformer.color` is write-only (done in original closeout)

**Where:** `src/ui/app/editor.h:31`

**Smell:** Written once by `glr_ctrl.c:1006`
(`is_clear = (cmd->type == CMD_CLEAR_COLOR)`) and zeroed by tests;
no reader anywhere.

**Fix:** Delete the field.

### 40. `var_panel_replay_lift_tick` cache guard is dead (done in original closeout)

**Where:** `src/ui/app/variable_panel.c:116-120`

**Smell:** The dedupe compares against a monotonic `anim_time` — so
the guard never fires during normal play; it triggers only if a
frame redraws twice with identical clock. Two file-statics
(`g_var_panel_lift_update_time`, `g_var_panel_lift_update_target`)
exist to power a no-op check.

**Fix:** Delete the guard + the two statics; the unconditional
easing step at L122-125 is cheap and idempotent.

### 41. `repl_code_panel_static_line_color` does an unconditional `strcmp` for every chrome line (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:133-140, 159, 165, 197,
232, 1299, 1305, 1319`

**Smell:** Every static header / footer / display row passes
through `strcmp(text, REPL_CODE_PANEL_SCRATCH_DECL_LINE)`. The
scratch line is only present in one emit site
(`build_rows:1170-1173`). ~50 strcmps per frame for zero hits.

**Fix:** Pass the explicit color at the scratch-line emit site;
use plain `repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB)`
everywhere else.

### 42. `ui_menu_bar_render_search_overlay` has three unused parameters (done in original closeout)

**Where:** `src/ui/app/menu_bar.c:1277-1283`

**Smell:** `(snap, cp_x, panel_w, panel_top)` immediately discards
`cp_x`, `panel_w`, `panel_top` and re-derives the geometry from
`menubar_rects`. Caller at `repl_code_panel.c:1636` still passes
them.

**Fix:** Drop the three int parameters; update tests.

### 43. `UiVariablePanelState` typedef lives in the wrong header (done in original closeout)

**Where:** `src/ui/app/state_types.h:44-46`

**Smell:** `UiVariablePanelState { int visible; }` is declared in
the UI-chrome value-types header, but ownership is on
`subsystems/variable_panel/variable_panel_state.c`.

**Fix:** Move the typedef into
`subsystems/variable_panel/variable_panel_state.h`; have
`ui/app/snapshot.h` include from there.

### 44. `text_panel.c:818-819` dead override of `resolved_line` for virtual rows (Tier D — claim incorrect; load-bearing)

**Where:** `src/ui/core/text_panel.c:818-819`

**Smell:**
```c
int resolved_line = row->source_line_idx >= 0 ? row->source_line_idx : row->hit_target_line_idx;
if (row->kind == UI_TEXT_PANEL_ROW_VIRTUAL) resolved_line = row->source_line_idx;
```
For virtual rows `source_line_idx == -1`; the first assignment
falls through to `hit_target_line_idx`, the second reverts it to
-1. Header L132-137 says: "the generic hit-tester leaves
line_idx unresolved for these rows" — the conditional contradicts
the docs.

**Fix:** Drop the conditional override; let `resolved_line` fall
through to `hit_target_line_idx`.

## 🔵 Structural concerns

### 45. `ui_menu_bar_render` is 140 lines mixing 6 jobs (done — Tier C closed 2026-05-25; now 40 lines via `paint_menu_labels` + `paint_pin_buttons`)

**Where:** `src/ui/app/menu_bar.c:1375-1514`

**Smell:** One function derives layout (`menubar_rects`), paints
surface, computes hover via `ui_menu_bar_menu_hit` /
`ui_menu_bar_pin_hit` (live-state hit-test from render), draws
menu labels, paints pin-mask rect, draws each pin (with
Search-vs-Replay-vs-default branching), and the bottom hairline.

**Fix:** Break into `paint_strip_bg`, `paint_menu_labels(hover)`,
`paint_pin_buttons(hover, replay)`; precompute hover from snap.

### 46. `render_active_submenu` is 106 lines combining layout / paint / hit / kind / fade / Config-special-case columns (partial — 106→82 lines as of 2026-05-25; Config column hoisting remains Tier C)

**Where:** `src/ui/app/menu_bar.c:1153-1258`

**Smell:** Computes Config's per-row column x's (L1176-1180)
regardless of menu; iterates rows with three different paint
branches; the Config-only block L1230-1254 is the only reason the
function isn't menu-agnostic.

**Fix:** Hoist per-flyout column-x into a `FlyoutColumns` struct;
let each provider expose an optional "draw row right-decoration"
hook.

### 47. `ui_menu_bar_hit_test` is 87 lines with duplicate chrome branches (partial — 87→61 lines as of 2026-05-25; one chrome branch now in `submenu_hit_test`, the other is still inline)

**Where:** `src/ui/app/menu_bar.c:856-942`

**Smell:** Two `UI_HIT_CODE_PANEL_CHROME` blocks (L877-885,
L905-912) repeat the same overlay-precedence logic.

**Fix:** Factor "inside dropdown rect but not on item" into one
helper returning a chrome-or-item discriminator.

### 48. `ui_repl_code_panel_render` is misnamed — it renders chrome too (done — Tier C closed 2026-05-25; renamed to `ui_repl_code_panel_render_with_chrome`)

**Where:** `src/ui/app/repl_code_panel.c:1599-1645`

**Smell:** Also renders scene tabs, menu bar, search overlay,
statusbar, color picker — the full chrome layer. Callers expecting
"render the code panel" get the whole shop, color picker drawn
last.

**Fix:** Rename to `ui_repl_code_panel_render_with_chrome`, or
split into `ui_repl_code_panel_render_chrome(...)` and have
`panels.c` orchestrate.

### 49. Hit-test rebuilds the entire row set already built by render (done — Tier C closed 2026-05-25; `g_builder_cache` reuses the per-frame builder keyed on snapshot pointer)

**Where:** `src/ui/app/repl_code_panel.c:1690-1702`

**Smell:** `ui_repl_code_panel_hit_test` calls
`repl_code_panel_init_builder` + `_build_rows` from scratch on the
same global `g_repl_code_panel_rows[]` buffer used by render.

**Why it matters:** Wasted work; the build path reads live state
(findings #11-#15) so render and hit-test can disagree if anything
ticked between them.

**Fix:** Cache the last-rendered builder/text_snap keyed on snapshot
pointer + row layout dims; expose a `rebuild_rows()` call the
controller makes once per frame.

### 50. Magic spacing constants scattered across menu render code (done — Tier C closed 2026-05-25; `MENU_LABEL_PAD_X` + `SUBMENU_ARROW_COL` named, the `+ 4` row-pad / `+ 8` extra-height all expressed via `DROPDOWN_PAD_Y`, and the triplicated ordinal-from-y formula extracted into `dropdown_row_for_gl_y`)

**Where:** `src/ui/app/menu_bar.c:428, 438, 502, 507, 509, 511,
724, 786-787, 1176, 1180, 1414, 1452, 1491, 1598, 1605` and
ordinal-from-y formula at L800, L841, L1103

**Smell:** Only `MENU_TEXT_INSET_X = 14` and `SUBMENU_ARROW_COL =
26` are named. The `+ 4` "row offset pad" appears in three distinct
ordinal-from-y formulas; the `+ 8` "extra height" for both dropdown
and submenu.

**Fix:** Add `MENU_LABEL_PAD_X`, `DROPDOWN_HEIGHT_PAD`,
`DROPDOWN_ROW_TOP_OFFSET`, `DROPDOWN_INNER_BORDER` to `metrics.h`;
extract `row_for_y(top, h, gl_y)` shared helper.

### 51. `menu_dropdown_rect` / `submenu_rect` re-measure all rows every call (done — Tier C closed 2026-05-25; both functions cache through `g_dropdown_cache` / `g_submenu_cache` with menu+window keys)

**Where:** `src/ui/app/menu_bar.c:484-498, 705-715`

**Smell:** Walks every row's `strlen(label) + strlen(shortcut)`
each call. Runs several times per frame from render, hover, every
hit-test, every test helper.

**Fix:** Cache per-frame keyed on open menu + open submenu;
invalidate on open / close / hover-change.

### 52. `menu_item_label` returns a `static char buf[48]` overwritten on each call (done — Tier C closed 2026-05-25; every branch now returns a string literal or a name owned by another module)

**Where:** `src/ui/app/menu_bar.c:312-324`
(same pattern in `config_item_shortcut`, L341-361)

**Smell:** Two successive calls share the buffer. Works today
because callers strlen / draw immediately. `menu_dropdown_rect`
(L488-498) reads `lbl` inside a `for (i = 0; i < n; i++)` loop —
would break the moment a future change saves the pointer. Also a
"renderer mutates file-static state" snapshot violation.

**Fix:** Pass `out_buf, out_sz` (mirrors `cfg_state_str`); or
title-case at config-section-table init.

### 53. Hand-rolled ASCII title-case in render-time hot path (done — Tier C closed 2026-05-25; folded into #52's static-literal rewrite)

**Where:** `src/ui/app/menu_bar.c:313-323`

**Smell:** Uppercases first char + lowercases the rest by ±32.
Locale-naive, in a render hot path, through the `static char
buf[48]` (#52). The `RENDERING → Rendering` policy is embedded in
the UI layer when it could live next to
`glr_config_section_label`.

**Fix:** Pre-compute display labels once in `glr_config.c`
(or expose `glr_config_section_display_label`); UI just renders.

### 54. `repl_code_panel_newline_rows` is misnamed (done — Tier C closed 2026-05-25; renamed to `repl_code_panel_trailing_row_count`)

**Where:** `src/ui/app/repl_code_panel.c:294-304`

**Smell:** Returns "input rows when edit_line == document_count,
otherwise 1 placeholder row." That's the trailing tail row count,
not a "newline."

**Fix:** Rename to `_trailing_tail_rows` or `_trailing_row_count`;
or split into `_trailing_input_rows` / `_trailing_placeholder_rows`
+ thin selector.

### 55. `ui_panels_render_scene_status` has two ~45-line near-identical bar blocks (done — Tier C closed 2026-05-25; extracted `status_strip_begin` / `_paint_bar` / `_end` shared by `draw_modal_strip` and the status banner)

**Where:** `src/ui/app/panels.c:49-92` (rename modal) and L98-151
(file prompt). The amber/red status banner at L157-244 shares the
same shell.

**Smell:** Viewport-begin, blend setup, two-color rect+rule,
font-cell text clamp, `gl2d_end` teardown — duplicated. Only the
message format and msg buffer size differ.

**Fix:** Extract
`draw_scene_status_strip(snap, bg, rule, fg, msg, max_msg_chars)`.

### 56. `STATIC_ASSERT` missing on tutorial-fade segment count (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:720-737`

**Smell:** The loop emits one-char segments per
`TUTORIAL_FADE_SETTLE_CHARS` column + settled-prefix + head + tail
≤ 9 segments. `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS = 16` so the `break`
is unreachable. But increasing the constant past 13 would silently
corrupt the fade.

**Fix:** `STATIC_ASSERT(TUTORIAL_FADE_SETTLE_CHARS + 3 <=
UI_TEXT_PANEL_MAX_COLOR_SEGMENTS, …)`.

### 57. Defensive `if (!snap)` after `init_builder` already validated it (done in original closeout)

**Where:** `src/ui/app/repl_code_panel.c:1144, 646, 1080, 612, 600`

**Smell:** `init_builder` returns 0 if `snap == NULL`; these
downstream functions only run on a successfully-initialized
builder. The duplicate guards exist on every helper.

**Fix:** Drop the redundant guards; keep only `init_builder`'s.

### 58. `(int)strlen(text)` repeated for the same row text within a frame (partial — 5+ sites → 1 site as of 2026-05-25; the `UiTextPanelRow.text_len` cache wasn't added but the other call sites were deleted)

**Where:** `src/ui/app/repl_code_panel.c:690, 1118-1119, 1543, 1549,
1555`

**Smell:** Row text lengths computed independently in fade,
virtual-row build, statusbar layout. `UiTextPanelRow` could cache
`text_len` once.

**Fix:** Precompute `text_len` inside `repl_code_panel_push_row`
(or the segment-emit helpers).

### 59. `ui_state_status_set` writes empty strings; only `repl_set_status` filters them (done in original closeout)

**Where:** `src/ui/app/state.c:58-66` vs. `src/repl/core.c:91-94`

**Smell:** `repl_set_status` guards `msg && msg[0]`;
`ui_state_status_set_kind` only guards `!message` and stamps full
TTL for an empty string. A direct `ui_state_status_set("")`
produces a visible-but-empty banner.

**Fix:** Pull the empty-message guard into
`ui_state_status_set_kind`.

### 60. Duplicate static `code_panel_layout_mode` helper (acknowledged in comment) (done in original closeout)

**Where:** `src/ui/app/variable_panel.c:36-44` vs.
`src/ui/core/layout.c:15-21`

**Smell:** The variable panel's own comment says: "Local copy of
the layout-mode clamp, also implemented as
`ui_layout_code_panel_layout_mode()` in ui/layout.c; promoting to
a shared header is a separate cleanup."

**Fix:** Promote `ui_layout_code_panel_layout_mode` to
`ui/core/layout.h`; delete `rvp_code_panel_layout_mode`.

### 61. `tabbed_overlay.c:223` uses raw literal `14` where `MENU_TEXT_INSET_X` exists (done in original closeout)

**Where:** `src/ui/core/tabbed_overlay.c:223`

**Smell:** `int tx = hx + 14;` — magic number paired with the same
named constant used at L160 of the same file.

**Fix:** `int tx = hx + MENU_TEXT_INSET_X;`.

### 62. `text_panel.c:832-835` over-indented block (done in original closeout)

**Where:** `src/ui/core/text_panel.c:832-835`

**Smell:** Stray two-space indent vs. the sibling `if (in_gutter)`
block above (12 spaces vs. 15).

**Fix:** Re-indent. Cosmetic, but a hint that the file isn't being
auto-formatted under the purity guard.

### 63. `text_panel_row_layout` called 5× across `_input_row`, `_regular_row`, `_row_wrap_count` (done — Tier C closed 2026-05-25; per-snapshot `g_wrap_cache` populated by render, consumed by hit-test + `ui_text_panel_input_row_y`)

**Where:** `src/ui/core/text_panel.c` (lines 638 vs. 645 inside
`text_panel_row_wrap_count` declare it twice in the same function
under different branches)

**Smell:** Wrap math walks the full row text twice per frame for
every row above a click (hit-test re-iterates rows render already
walked).

**Fix:** Hoist `text_panel_row_layout` to the top of
`text_panel_row_wrap_count`; cache wrap counts on
`UiTextPanelOutput` so hit-test can reuse them.

## Sequencing

### One-afternoon pass

- [x] **#1** — `tabbed_overlay.c` NUL read past short lines. Single
   conditional, surgical fix.
- [x] **#2** — Add missing `<math.h>` / `<string.h>` / `<stdio.h>`
   includes to `variable_panel.c` + `autocomplete_panel.c`. Two-line
   change per file.
- [x] **#3** — Delete the render-time hover-mutation in
   `menu_bar_render_example_dropdown` (lines 1525-1527).
- [x] **#5** + **#6** — Wire `apply_command_overlays` into
   `add_input_row` and replace the marker-color cascade with an
   explicit priority enum.
- [x] **#32** + **#33** + **#42** + **#34** + **#35** + **#36** —
   Delete dead work: the unused `UiReplCodePanelLayout` build, the
   `text_panel_draw_indent` no-op, the three unused
   `search_overlay` params, the three stale `int`-returning hit-test
   entry points, the test-only alias, the duplicate test rect
   helpers. All mechanical.
- [x] **#38** + **#39** + **#40** — Delete the five orphan
   `ui_state_*` exports, `UiTransformer.color.is_clear`, the dead
   var-panel lift cache.

### One-week pass

The dominant work is **closing the snapshot boundary**:

- [x] **#11** + **#12** + **#13** + **#14** + **#15** + **#16**
   — Push `editor_buffer_view`, `editor_line_overrides`,
  `tutorial_fade`, `init_section_lines`, resolved cfg states, replay
  state into `UiRenderSnapshot`. (`#18` partial — drag-state slice
  not yet on snapshot; `#25` deferred — replay HUD keeps own state
  struct.)

Then **the core/app boundary**:

- [x] **#7** + **#8** + **#9** + **#10** — Move `layout.c` out of
  `core/`, split `hit.h` into `core` + `app` halves, push
  `STATUSBAR_H` / `TAB_STRIP_H` out of `metrics.h`, refresh
  `text_panel.h` docs.

Then **the flyout-engine debt**:

- [x] **#19** — Extend `CatalogFlyoutOps` to cover Config; collapse
  the 21 menu-id branches via `FlyoutProvider` polymorphism.

Then **the panel-frame chrome layer**:

- [x] **#23** + **#24** + **#26** + **#27** + **#28** — Extract
  `gl2d_panel_frame`, `ui_clamp_panel_y`,
  `ui_layout_menu_bar_rect`, `ui_text_panel_right_action_rect`,
  `ui_repl_code_panel_compute_text_x`. Each is a tiny helper that
  removes a duplicated copy.

### Closeout pass (2026-05-24)

- [x] **#31** — `tolower` → `ascii_tolower` in `text_search.c`
  (locale-independent ASCII fold).
- [x] **#56** — `STATIC_ASSERT` linking `TUTORIAL_FADE_SETTLE_CHARS`
  to `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS` so future bumps don't
  silently corrupt the fade.
- [x] **#59** — Empty-message guard in `ui_state_status_set_kind`
  (drop empty banner instead of stamping full TTL).
- [x] **#60** — Promote `ui_layout_code_panel_layout_mode` to
  `layout.h`; delete the `rvp_code_panel_layout_mode` duplicate.
- [x] **#62** — Re-indent the stray two-space `if` block in
  `text_panel.c::ui_text_panel_hit_test`.

### Out of scope

- The `scene_tabs.c` file is clean — the audit's comparand for what
  a snapshot-pure UI module looks like. Don't touch it except where
  it shares a helper with `menu_bar.c` (e.g. #26).
- The two-piece code-panel split (`core/text_panel.c` +
  `app/repl_code_panel.c`) is load-bearing. The findings about
  `repl_code_panel.c` are about leaks past the snapshot, not about
  the split itself.
- The `replay_ui_*` prefix carve-out for feature-UI is a sanctioned
  exception per `MODULES.md`; the findings are about HUD-specific
  smells, not the prefix.
- `gl_2d.h` is genuinely tiny and header-only by design — don't
  conflate it with `theme.h` (#29), which actually does have a real
  TU-duplication problem.

## Method note

This audit was produced by four parallel review agents:

- `menu_bar.{c,h}` (61KB, 1618 lines — the second-biggest file in
  the directory) + `scene_tabs.{c,h}` (used as the clean comparand)
- `repl_code_panel.{c,h}` (70KB, 1743 lines — the biggest file in
  the directory) + `panels.{c,h}`
- The full `core/` subtree (`text_panel`, `text_layout`,
  `text_search`, `tabbed_overlay`, `layout`, `hit.h`, `gl_2d.h`,
  `metrics.h`, `theme.h`)
- Floating panels (`variable_panel`, `color_picker`,
  `autocomplete_panel`, `profile_panel`, `replay_hud`) + the
  `UiState` owner (`state.{c,h}`, `state_types.h`, `snapshot.h`,
  `editor.h`)

Each agent was asked for ~15-20 highest-signal findings only, not a
comprehensive sweep. The most actionable claims (real-bug findings
above) were verified against the source. The 🟡 / 🟢 / 🔵 findings
are reported as the agents framed them; spot-check before acting on
the more mechanical ones.
