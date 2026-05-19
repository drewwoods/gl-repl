# Code-Quality Refactor Followups — Findings & Plan

## Provenance

Captured 2026-05-19 from the "final pass" audits behind PR #41 — four
parallel read-only passes (smells / inconsistencies / dense uncommented
code) over `src/app`+root, `src/repl`, `src/editor`, and
`src/scene`+`src/ui`+`src/widgets`, calibrated to the #36–#40 cleanup
bar.

The **clear-wins** from those audits have already landed and are NOT in
scope here:

- **#41** — comments on dense code, dead/no-op-scaffold removal, stale
  API-doc rewrites, trivial mechanical consistency. No behavior change.
- **#42** — line-stipple pattern unified on
  `SCENE_OCCLUDED_GHOST_STIPPLE` (an approved visual change).
- **#43** — the tutorial paste/undo ordering bug (guard now runs before
  the undo push); regression-tested.

This document records the **`[refactor — report only]`** residue: items
that need a behavior-preserving *structural* change (or a deliberate
behavior decision) and were intentionally deferred out of the
conservative cleanup. As with `plans/partial/src-repl-simplicity-review.md`,
**no redesign is proposed** — every approach below uses an idiom already
present in the tree, and each item is localized.

Two audit suggestions were investigated and **rejected**; do not
re-chase them:

- Routing the `0xAAAA` stipple sites through a single constant was the
  *right* call but the constant is `0x0F0F` (a different dash); resolved
  correctly in #42 as an explicit visual change, not a no-op.
- "Extract one `color_picker.c` checkerboard helper" — the two blocks
  are **not** textually identical (top-anchored-descending vs
  bottom-anchored-ascending Y origin), so it is a parameterized refactor
  with pixel-placement risk, not a factor-out. Captured below as item 14
  with that caveat rather than as a quick win.

Cross-reference: `compile.c`'s verb-boundary split is already tracked
(DEFERRED) in `plans/partial/src-repl-simplicity-review.md` §1 and is not
duplicated here.

## Stance

- Behavior-preserving unless explicitly flagged (items 1 and 2 are
  deliberate behavior decisions).
- Each followup names its in-tree idiom and its test surface.
- Priority tiers: **P1** correctness-adjacent / latent, **P2**
  high-drift duplication (maintenance value), **P3** lower-value
  dedup/consistency, **P4** large parallel-implementation hazards
  (flag-only, schedule when a triggering feature pays for it).

---

## P1 — Correctness-adjacent / latent

### 1. `editor_buffer_replace_line` extends over stale gap lines — Not started

**Problem.** `src/editor/state.c` (~`:145`) `editor_buffer_replace_line`
clamps `pos` only to `[0, MAX_COMMANDS)` then does
`if (buf->line_count <= pos) buf->line_count = pos + 1;` — replacing at
`pos > line_count` silently extends `line_count` over never-written,
zeroed gap lines `[old_count, pos-1]`. The sibling
`editor_buffer_insert_lines` (~`:121`) clamps `pos` to `[0, old_count]`
and cannot create a gap. Currently dead (every caller replaces at
`pos < document_count`) but it is an unguarded latent corruption and an
inconsistency with the insert path.

**Approach.** Reject (`return 0`) or clamp `pos` to `line_count`, mirroring
`editor_buffer_insert_lines`. Add a focused `test_repl_command_store` /
editor-state assertion for `pos > line_count`.

**Risk & scope.** Very low — only changes behavior on input no caller
produces today; aligns two siblings.

### 2. Ctrl+Q "save and quit" writes a hardcoded `/tmp` path — Not started

**Problem.** `src/app/glr_ctrl.c` (~`:2299`)
`glr_ctrl_router_handle_quit_key` hardcodes
`repl_export_save_output("/tmp/temp-output.c", …)` (the literal
duplicated on two lines) while Ctrl+S
(`glr_ctrl_router_handle_save_key`) does the real
`repl_save_active_scene(&layout)`. A "save and quit" that writes to a
throwaway `/tmp` file rather than the user's scene/workspace is a
data-loss surprise; `keys.h` documents Ctrl+Q as "save and quit".

**Approach.** **Decision required** (behavior): either route Ctrl+Q
through the same `repl_save_active_scene` path as Ctrl+S, or — if the
`/tmp` scratch behavior is intentional for crash-recovery — keep it but
hoist the literal to one named constant and document why. Recommend
matching Ctrl+S.

**Risk & scope.** Behavior change by design; small. Needs a product call
before implementation.

### 3. Render-time state mutation in the search overlay — Not started

**Problem.** `src/ui/menu_bar.c` (~`:1086`)
`ui_menu_bar_render_search_overlay()` mutates a function-`static int
prev_active` and writes `g_search_open_time` during a **render** call
(rising-edge detection of `srch.active`). This both breaks render purity
(the `ui_` contract) and **duplicates** `ui_menu_bar_note_search_opened()`
(~`:804`), which the controller already calls from `glr_ctrl.c` for the
same fade-clock purpose.

**Approach.** Delete the render-side `prev_active`/`g_search_open_time`
write; rely solely on the controller hook. First confirm the controller
calls `ui_menu_bar_note_search_opened()` on every path that can set
`srch.active` (Ctrl+F and any menu/search entry point).

**Risk & scope.** Low; removes code. Verify the fade still triggers on
all search-open paths (no automated coverage today — add one if cheap).

---

## P2 — High-drift duplication (highest maintenance value)

### 4. Triplicated "parse-and-place" commit tail in `input.c` — Not started

**Problem.** `src/editor/input.c` re-implements the parse→place tail
near-verbatim in 3+ sites (`handle_semicolon_commit_key_route`,
`editor_feed_line`, the insert/overwrite/append arms of
`commit_current_input`). Each independently recomputes the identical
`insert_idx` ternary (`editor_insert_mode() ? repl_state_edit_line()
: (repl_state_edit_line() < repl_state_document_count() ?
repl_state_edit_line() : repl_state_document_count())`), calls
`collect_visible_vars`, branches `num_vis_vars > 0` to pick
`repl_parse_and_normalize_strict` vs `repl_parser_parse_command_ctx`,
then branches insert/replace/append into the `repl_command_store_*` +
`editor_buffer_*` primitives. Any change to parse-tail semantics must be
applied in 3+ places — the **highest drift risk in the editor dir**.

**Approach.** Extract `editor_resolve_insert_idx()` and
`editor_place_parsed_command(insert_idx, &cmd, cmd_text)`; have all
sites call them. The shared primitives already exist (the header at
`input.c:~1505` acknowledges they share primitives — this lifts the
*orchestration* too).

**Risk & scope.** Medium — load-bearing commit orchestration. Full
`test_repl_editor` + `test_repl_core_commit` + `test_tutorial_runner`
gate. Highest value of the P2 set.

### 5. Four identical `editor_try_commit_*` bodies — Not started

**Problem.** `src/editor/commit.c` (~`:1256-1350`)
`editor_try_commit_for_loop` / `_func_def` / `_if_block` /
`_close_brace` are byte-identical except the single `editor_compile_*`
call: same ctx/plan/err setup, same `(OK && NO_CHANGE &&
!commit_message_valid) → 0`, same `r != OK → set_status; return 1`,
same `!apply_plan → "Command buffer full!"`, same
`mark_normals_dirty; return 1`.

**Approach.** A helper taking the compile-fn pointer, or a small
`{fn}`-table the four wrappers index. Matches the descriptor-table idiom
the codebase already favors.

**Risk & scope.** Low-medium; behavior-bearing glue but mechanical.
`test_repl_core_commit` covers the four forms.

### 6. 6×-duplicated insert-position clamp in `compile.c` — Not started

**Problem.** `src/repl/compile.c` repeats
`ctx->insert_mode ? ctx->edit_line : (ctx->edit_line <
ctx->document_count ? ctx->edit_line : ctx->document_count)` at ~6 sites
(`:741, :847, :1492, :1544, :1754, :1798`).

**Approach.** `static int compile_insert_pos(const ReplCompileContext
*ctx)`; replace all six.

**Risk & scope.** Low — every occurrence is identical, pure mechanical.
`test_repl_compile`.

### 7. Hand-rolled arg splitters in `export.c` vs the mandated scanner — Not started

**Problem.** `src/repl/export.c` has ≥6 hand-coded paren-depth
comma/arg scanners (`write_for_begin_as_c` ~`:1097`, plus ~`:1188,
:1223, :1356, :1395, :1531`, and `split_top_level_args` ~`:1583`).
CLAUDE.md mandates `repl_scan_next_arg_delim()` (eval.h) for splitting a
call's comma-separated args; it is used in exactly one place
project-wide (`parser.c`). The export splitters reimplement the same
paren-aware logic.

**Approach.** Migrate to `repl_scan_next_arg_delim` / a single
`split_top_level_args`. Some sites differ in string-literal handling —
audit each before swapping.

**Risk & scope.** Medium; behavior-sensitive (export text format).
`test_repl_export_all_commands` (the comprehensive 30-command
export/import round-trip) is the safety net.

### 8. Open-coded indent math vs the `format.c` / `source_scope.c` helpers — Not started

**Problem.** `src/repl/parser.c` (`:404, :441, :933`) open-codes
`spaces = 2 + 2*td + 2*kd; memset(ind,' ',spaces)` for the glBegin-style
indent and `src/repl/core.c` (`:514-528`) open-codes the close-brace
"indent − 2" dedent, despite `format.c` (`repl_format_indent`,
`repl_format_end_indent`, `repl_format_tess_end_indent`) and
`source_scope.c` (`repl_source_scope_cmd_indent`) already centralizing
the formula. The bare `2`/`+2`/`-2` are duplicated across 4 files.

**Approach.** Route the begin-style and close-brace indents through a
shared `source_scope`/`format` helper analogous to the existing
`_cmd_indent`.

**Risk & scope.** Low-medium; behavior-equivalent. `test_repl_core_format`
+ `test_repl_core_parse` gate.

---

## P3 — Lower-value dedup / consistency

### 9. 11 hand-rolled clear-input blocks in `input.c` — Not started

`{ inp->input[0]='\0'; inp->input_len=0; } editor_cursor_pos_set(0)`
appears ~11× (input.c `:260, :574, :621, :685, :744, :773, :801, :974,
:1337, :1361, :1573`). `editor_input_clear()` (state.c) already does
exactly this. Consolidate; preserve the separate `pending_newline` clear
where it is paired. Low risk, ~30 lines removed.

### 10. Duplicated trailing-`;`/whitespace strip primitive — Not started

`input.c` (`:322, :387, :517`, + label variants `:308`) hand-rolls
"strip trailing `;` and whitespace, return new length" 3–4×. A
`static int strip_trailing_semicolon_ws(...)` centralizes the
end-of-string/`;` terminator contract documented in CLAUDE.md. Call
sites differ in buffer ownership — report-only granularity.

### 11. Triplicated cursor-doc-line branches in `repl_code_panel.c` — Not started

`repl_code_panel_cursor_doc_line_from_layout` (`:297-343`) has three
branches that differ **only** in the prefix loop bound; the trailing
`repl_code_panel_cursor_row(...)` add is identical (the in-code comment
already says so). Collapse to: compute bound → one summation → one
cursor-row add. Load-bearing layout math — behavior must be byte-exact;
`test_glr_ctrl` / panel layout tests gate.

### 12. Duplicated SV/hue/alpha drag math in `color_picker_state.c` — Not started

`color_picker_handle_press` (`:286-320`) and `_handle_motion`
(`:340-354`) duplicate the ratio-compute + clamp + `CP_HUE_MAX` wrap +
`value_max` cap for all three drag arms; press just adds the region
hit-test on top. Factor `cp_apply_drag_at(mx, gl_y, &r)`. Input path,
behavior-sensitive — manual picker check needed (no GL test).

### 13. Split `CP_GAP`/`CP_PREV_H` definition — Not started

`color_picker_state.c` `#define`s `CP_GAP`/`CP_PREV_H`; `ui/color_picker.c`
re-declares them as a local `enum`. `CP_SV_SZ`/`CP_HUE_W`/`CP_ALPHA_W`
already flow correctly through `ColorPickerView.rects`; surface the
two popup-frame constants the same way (on `ColorPickerView` or a shared
header) to kill the magic-twin risk.

### 14. `color_picker.c` checkerboard blocks — Not started (caveated)

The alpha-bar and preview-swatch checkerboard fills (`:130-134`,
`:159-163`) look duplicated but the Y origin differs structurally
(`py - iy - th` top-anchored descending vs `swy - CP_PREV_H + iy`
bottom-anchored ascending). A shared `cp_draw_checkerboard(...)` must
parameterize the Y mapping — a real refactor with pixel-placement risk,
**not** the textual factor-out the audit first suggested. Low value;
only attempt alongside item 12.

### 15. Three `### `/`---` chrome detect+draw copies in `menu_bar.c` — Not started

Header/separator chrome is rendered twice (`ui_menu_bar_render_example_dropdown`
string-matches `"### "`/`"---"`; `render_active_submenu` enum-matches
`GLR_CFG_ROW_HEADER`/`_SEPARATOR`) plus a third predicate copy in
`ui_menu_bar_dropdown_item_hit`. Same divider primitive + `+6/-6`
insets duplicated. Extract one `GlrConfigRowKind`-keyed
"draw chrome row" helper. `test_ui_menu_bar` gate.

### 16. `is_detail_section()` parallel list in `profile_panel.c` — Not started

`:136-167` is a ~30-entry `||` chain that must stay 1:1 with the
indented (`"  "`/`"    "`) entries in `section_label()`. Derive "detail"
from the label indent, or add a `detail` flag to a section-metadata
table. Lowest severity (profiling overlay only).

### 17. Duplicated key-lookup loop in `glr_config.c` — Not started

`glr_config_state_count()` / `glr_config_state_name()` (and
`glr_config_cycle`) open-code the same "scan `g_cfg_items[]` for
`.key == key` + null/section guard". Extract
`static const GlrConfigItem *find_item_by_key(GlrConfigKey)`.
Low risk; `test_ui_menu_bar` / config tests.

### 18. Duplicated scanning helpers in `src/repl` — Not started

`find_matching_square` (eval.c) vs `replay_find_matching_square`
(replay_annotations.c); three whitespace-skip copies (`skip_ws_ptr`,
`skip_leading_ws`, `example_cam_skip_ws`); `expr_has_visible_vars`
(replay_annotations.c) vs `input_has_expr_vars` (export.c). Consolidate
into the eval/util layer (`eval.h` already hosts
`repl_scan_next_arg_delim`). (The false-identity `skip_numeric_literal`
was already resolved by rename in #41.)

### 19. `GLR_FILE_ITEM_LOAD_SCENE` permanent stub — Not started

`glr_actions.h:~37` / `glr_actions.c:~540` wire a clickable File-menu
row that can only ever print `"Runtime load unsupported - relaunch …"`.
**Decision required**: implement runtime scene load, or remove the
enum + menu row until it is real (don't let it linger).

---

## P4 — Large parallel-implementation hazards (flag-only)

### 20. Replay simulator re-implements the executor control-flow walk — Not started

`src/repl/replay_annotations.c` `replay_copy_runtime_state_before_flat_cmd`
(`:544-655`) re-implements the executor's CMD_GOTO label search
(`executor.c:635-647`) and IF_BEGIN→IF_END depth scan
(`executor.c:673-678`). Two engines that must stay in lockstep (the
in-code "Mirror executor.c" comments acknowledge it). A shared flat-walk
control-flow stepper is the real fix — significant, not mechanical.
Schedule only when a control-flow feature makes it pay for itself; until
then the lockstep is documented and tested (`test_replay_walk`).

### 21. `eval_fmt_for_type` duplicates `command_spec` format data — Not started

`replay_annotations.c:~1025` `eval_fmt_for_type` is a private
`CmdType → (fmt, nargs)` switch duplicating the authoritative
`k_std_command_specs[]` / `g_command_type_specs[]` in `command_spec.c`
(two sources of truth that can silently diverge). Add a `command_spec`
display-fmt query and delete the table. `command_spec.c` is the
codebase's canonical extension point (per the simplicity review), so
this strengthens the spine; needs a new query API + `test_replay_walk`
drift check.

---

## Recommended sequencing

Batch into small, independently-reviewable PRs (the established rhythm):

1. **PR A — P1 safe pair:** items 1 + 3 (latent corruption guard +
   render-purity leak removal). Low risk, high signal.
2. **Decisions:** items 2 and 19 need a product call before code —
   surface, don't pre-implement.
3. **PR B — mechanical dedup:** items 6 + 9 + 17 (insert-pos clamp,
   clear-input, key-lookup). Pure, low-risk, big readability win.
4. **PR C — commit-glue dedup:** item 5, then item 4 (in that order —
   5 de-risks the helper shape 4 reuses). The highest-value structural
   pair; full editor/commit/tutorial gate.
5. **PR D — indent + arg-splitter unification:** items 8 then 7
   (format/source_scope first, then the export scanners against the
   round-trip test).
6. **PR E — UI/widget dedup:** items 11, 12(+14), 13, 15, 16.
7. **Deferred:** items 10, 18 (low value), 20, 21 (P4 — schedule
   against a triggering feature).

Each PR: `make check-state-ownership` + `make test` green, behavior
unchanged (except items 1/2/19 which carry an explicit, called-out
behavior delta). Move this doc `plans/in-review → plans/active` when the
first PR opens; check items off as they land.
