# Editor Demo + SRP Split For Code Panel UI

## Summary

Three steps: (1) split the code-panel UI into a generic text-panel renderer plus a REPL-specific adapter; (2) decouple `src/editor/input.c` and `src/editor/commit.c`'s REPL/chrome reach by **extending** the existing `EditorServices` seam (`src/editor/services.h`, today scoped to compile/apply via `commit.c`) and adding an `EditorChromeServices` seam alongside it, so the editor module set can link without the full REPL pipeline or `glr_*` chrome; (3) add `editor_demo` as the forcing function that proves the split. Like `scene_demo` (which keeps `src/scene/` honest about its REPL dependencies) and `repl_demo`, `editor_demo` is a second binary that fails to link if the split regresses, turning "the module is independent" from a claim into a checkable invariant. The goal is not to make the editor fully reusable in one step; it is to create a working standalone proof and improve `ui/panels.c` by separating text-editor rendering from REPL presentation.

## Phase 0 — Baseline And Invariants

- Record current behavior before refactor:
  - Build `make sample USE_GL_STUBS=1`, `make repl_demo USE_GL_STUBS=1`, `make scene_demo USE_GL_STUBS=1`.
  - Run `make test-stubs` and `make check-state-ownership`.
  - **Capture baseline snapshots** so logical regressions are catchable later:
    - `testdata/repl_examples_ui/*.golden.txt` is a **logical** fixture (one row per header/source line, no wrap geometry), not a pixel-accurate visual fixture — see the comment at `tests/test_repl_core_examples.c:1044-1048` that calls this out and points to "the visual code-panel dump tests" as the place wrapped-row rendering is checked. Re-running the example-fixture suite after each phase catches structural drift (row counts, source order, header/footer scaffolding) but **does not** catch glyph-level visual regressions (color shifts, alpha blending, kerning, wrap geometry).
    - Current `--dump-code` output is useful as a source/export baseline, but it is still logical text: `sample.c` calls `glr_debug_dump_editor()`, which calls `repl_dump_code_panel_text()`, and that path does **not** exercise the wrap iterator. Capture those dumps for structural/source diffs if useful, but do not treat them as visual coverage.
    - For a checkable wrapped-rendering proxy, **do not** revive
      `repl_dump_code_panel_visual_text()` in place. That helper is
      disabled specifically because it crosses the intended REPL boundary:
      it lives in `src/repl/export.c` (line ~3445) but reaches into
      `code_layout_*` and presentation state to do wrapped-row rendering.
      Reviving it inside `src/repl/` would re-introduce the same layering
      inversion the rest of this plan is trying to fix.
    - Instead, the visual baseline lands **after Phase 2** as a UI-side
      test helper: once `ui_text_panel_render` exists, add a small
      `ui_text_panel_render_to_buffer()` (or similar) in `src/ui/` that
      drives the renderer against a snapshot and emits the wrapped rows
      as text. Capture representative scenes (wrapped lines, tutorial
      mid-fade, replay annotations, color swatches) to
      `/tmp/editor-demo-baseline/` and byte-diff after Phases 2-3 land.
    - Phase 0 baseline is therefore **structural-only**: rely on the
      example-fixture suite and `--dump-code` for structural coverage;
      defer visual coverage to the post-Phase-2 UI-side helper. Manual
      smoke testing (Test Plan below) covers what text dumps cannot in
      the interim.
- Keep public app entrypoints stable:
  - `ui_panels_render_code_panel`
  - `ui_panels_hit_test`
  - `ui_panels_render_scene_status`
  - `ui_panels_handle_right_press`
- Code touched: none except later documentation updates.

## Phase 1 — Define Generic Text Panel Contract

- Add `src/ui/text_panel.h`.
- Define generic, REPL-free types:
  - `UiTextPanelColor { float r, g, b, a; int has_alpha; }`
  - `UiTextPanelRowKind`: `STATIC`, `TEXT`, `INPUT`, `PLACEHOLDER`, `VIRTUAL`
    - `STATIC`: workspace header, render-state, camera, `header_pre/post`, footer scaffolding — chrome the adapter never edits.
    - `TEXT`: a committed source row (one per document command in the REPL adapter).
    - `INPUT`: the active edit row — the renderer draws cursor, selection, autocomplete ghost/hint here.
    - `PLACEHOLDER`: scroll-position-only row (e.g., blank insert-mode preview).
    - `VIRTUAL`: adapter-supplied row not backed by a source line directly (replay annotations, evaluated-arg previews). Carries an optional `hit_target_line_idx` — replay virtual rows map back to their owning source line (current hit-test behavior in `editor_code_panel_document_target_for_doc_line` maps replay extra rows to their owning `cmd_idx`; replay follow-scroll separately uses `replay_src_line()`), while demo-only virtual rows may set it to `-1` for non-hit-testable.
  - `UiTextPanelRow` decoration slots — *distinct fields, not a single "gutter labels" bag*:
    - `left_gutter_label`: line number (always the leftmost numeric column).
    - `left_aux_label`: auxiliary left-gutter content drawn at `idx_x` — currently the vertex/tess index (`v3`, `vn`); see `src/ui/panels.c:540-545`. Optional per row.
    - `right_action`: right-edge interactive element. Color swatches go here (drawn at `cp_x + cp_w - CODE_MARGIN_X - sw - 2`; see `src/ui/panels.c:548-558`). Distinct from `left_aux_label` because the right action is interactive (hit-testable, opens picker) while the left aux is purely visual.
    - Plus: text pointer, file-line number, source line index, search row index, indent chars, colors, hit eligibility flags.
  - `UiTextPanelInput`: input text, length, cursor, anchor, ghost, hint, cursor-visible flag. `ghost` and `hint` are pre-resolved strings — the text panel treats them as opaque and does not interpret them. The full app populates both from REPL-side autocomplete (`editor_state_autocomplete()`); the editor demo does **not** need ghost/hint (no grammar to suggest from) and just sends empty strings, so the fields cost nothing in the demo path.
  - `UiTextPanelSearch`: active flag, query, query length, hit row/char. Preserve current **search-row** semantics: `hit_row` is already in the editor search row space, where insert-mode can add a live input row and shift source rows (`editor_search_row_for_cmd_index` does this today). This is not the same as replay/source-line routing; the adapter assigns each `UiTextPanelRow.search_row_idx` to the search row that should be compared with `hit_row`, or `-1` for rows outside the search model.
  - `UiTextPanelSnapshot`: viewport, panel rect, `const UiTextPanelRow *rows`, row count, scroll, visible chrome flags, input/search/completion state.
    - `rows` is caller-owned storage valid for the render/hit-test call. Do **not** put a generic fixed `UI_TEXT_PANEL_ROW_CAP` in `text_panel.h`; the generic renderer should not bake in REPL document limits or a too-small visible-window assumption.
    - **Wrapping math is a generic-panel responsibility, not an adapter one.** Today `src/ui/panels.c:417` walks the *full* logical document while maintaining an absolute visual-row cursor — that's the math being moved into the generic panel in Phase 2. If the adapter pre-clips the row set, every subsequent caller has to reason about an offset between "snapshot row index" and "absolute visual row" — which means the wrapping calculation gets duplicated on both sides. Avoid that.
    - **Contract: the adapter ships *logical* rows (one entry per source line / virtual / static / input row) covering the entire document; the generic panel does the wrap iteration and visible-row clipping itself, the same way the current `code_panel_draw_command_row` does.** The REPL adapter sizes a temporary row array from live counts (`document_count + editor_virtual_lines->count + chrome/input rows`) or from the known upper bound (`MAX_COMMANDS` from `config.h` plus `MAX_VIRTUAL_LINES` from `src/ui/editor.h` plus chrome/input rows). The editor demo can use a much smaller caller-owned array because its fake document is small.
    - If profiling later shows a real per-frame cost in walking the full document, add an explicit `visible_row_first_absolute_idx` field on the snapshot so the renderer can resume the wrap walk from a known offset — but defer that until the cost is measured. Today the wrap walker is O(document_count × char_count) and that's not been a bottleneck.
  - `UiTextPanelOutput`: cursor pixel, cursor-valid, total rows, visible rows, **text-area rect + statusbar-slot rect** so the REPL adapter can overlay its status strip without recomputing layout (statusbar is REPL chrome — see Phase 3).
- Add APIs:
  - `ui_text_panel_visible_lines_for_height(int panel_h)`
  - `ui_text_panel_render(const UiTextPanelSnapshot *, UiTextPanelOutput *)`
  - `ui_text_panel_hit_test(const UiTextPanelSnapshot *, int mx, int my)`
- Layout helpers are pure pixel-math, not editor state — **move `src/editor/code_layout.{c,h}` to `src/ui/text_layout.{c,h}`** as part of this phase so the text panel doesn't have to include from a higher-level module. Update every existing caller (`src/editor/code_panel_document.c`, `src/ui/panels.c`, `tests/test_repl_editor.c`, etc.) to the new include path.
- Also pull the layout-only half of `src/editor/code_panel_document.c` into `src/ui/text_layout.{c,h}`, but **only after removing the hidden app-state dependency**. Today these wrappers call `editor_code_panel_document_text_layout()`, which reads `glr_state_presentation().wrap_at_comma`; that read is not pure and must not move into `src/ui/text_layout`. Phase 1 should parameterize the helpers with an explicit `CodeLayout` / `wrap_at_comma` value, move only the wrapper logic that is then just thin `code_layout_*` forwarding (`wrap_iter_init`/`_next`, `_row_count_for_text`, `_segment_for_row`, `_cursor_row_for_text`, `_visible_lines_for_height`), and leave the production `glr_state_presentation()` lookup adapter-side until Phase 3 absorbs it. The remaining REPL-aware half (`build`, `apply_follow_scroll`, `active_indent_chars`, `target_for_doc_line`) stays in `src/editor/code_panel_document.c` for now and folds into Phase 3's REPL adapter — see Phase 3 below.
- Constraints:
  - `src/ui/text_panel.*` must not include `repl/*` or `src/editor/*`.
  - It must not mention `GLCmd`, `CmdType`, or `CMD_*`.
- Code touched: new `src/ui/text_panel.{c,h}`, renamed `src/ui/text_layout.{c,h}` (was `src/editor/code_layout.{c,h}`), `src/editor/code_panel_document.{c,h}` (pure-half migration; the REPL-aware half stays here until Phase 3 absorbs it), every existing caller of `code_layout_*` for the include rename, `Makefile` source/header lists.

## Phase 2 — Extract Generic Rendering

- Move generic code-panel rendering from `src/ui/panels.c` into `src/ui/text_panel.c`:
  - panel background/divider
  - line wrapping using `src/ui/text_layout.h` (renamed from `src/editor/code_layout.h` in Phase 1)
  - gutter line numbers
  - active input row
  - caret, input selection, autocomplete ghost/hint
  - search highlight drawing
  - scrollbar
- Before moving search highlighting, move the pure case-insensitive text-match helpers out of `src/editor/search.c` into a neutral UI/text helper (for example `src/ui/text_search.{c,h}` with `ui_text_find_next/prev`). `src/ui/text_panel.c` must not include `src/editor/search.h`; `editor/search.c` can keep thin wrappers or switch its internal calls to the neutral helper.
- Move the generic hit-mapping math (mouse → row / source line / visual row / input cursor char) in the **same** phase. It shares every layout call with rendering, so splitting it across phases would force the same `text_layout_*` walks to be reproduced twice. Phase 4 keeps only the overlay-priority routing, which is independent of layout math.
- Keep REPL-only features out:
  - command colors
  - header/footer scaffolding
  - vertex/tess labels
  - replay rows
  - tutorial fading
  - color swatches
  - REPL statusbar content
- Code touched: `src/ui/panels.c`, new `src/ui/text_panel.c`, new `src/ui/text_search.{c,h}` or equivalent neutral helper, `src/editor/search.c`, `tests/test_repl_editor.c` (private `code_panel_header_row_count` helper duplicates the header-row math; any rename/move of those primitives during this phase ripples here — keep the helper aligned in the same patch).

## Phase 3 — Add REPL Code Panel Adapter

- Add `src/ui/repl_code_panel.{c,h}`.
- **Absorb the REPL-aware half of `src/editor/code_panel_document.c`** into the new adapter — the document-layout functions that read REPL state, replay state, and `repl_export_*` getters belong on the REPL-adapter side, not in `src/editor/`. After this phase, `src/editor/code_panel_document.c` is gone (its pure half moved to `src/ui/text_layout` in Phase 1; its REPL-aware half is now part of `src/ui/repl_code_panel.c`). This is what makes `code_panel_document` not a Phase 5 / shim concern: it never reaches Phase 5 in the first place.
- Move REPL-aware code-panel behavior from `src/ui/panels.c` into this adapter:
  - build `UiTextPanelRow[]` from `UiRenderSnapshot`, editor buffer lines, `GLCmd[]`, and import/export header/footer lines.
  - compute command colors from `repl_cmd_type_category`.
  - populate row decoration slots:
    - `left_aux_label` ← vertex/tess index for the row (REPL-specific; uses `primitive_vnums_exact` etc. from the snapshot).
    - `right_action` ← color swatch sourced from `snap->editor_transformers`. `EditorTransformer` snapshots stay owned by `EditorState` (the controller already pushes them per frame); the adapter maps each transformer to its row's `right_action`.
  - insert replay `VIRTUAL` rows **after** their owning source row, with `hit_target_line_idx` set to that source line. TEXT row source-line indices stay sequential; clicks on replay virtual rows route to the owning source line, preserving the current `editor_code_panel_document_target_for_doc_line` replay-extra-row behavior.
  - tutorial fade is **per-character**, not per-row: `src/ui/panels.c:158` calls `tutorial_step_fade_alpha(line_idx, char_idx, line_len, now)` inside the character loop. The implementation has to vary alpha across a row, but the fade math is REPL-specific and the generic text panel must not call out per character.

    **Design** — keep the fade machinery in `ui_repl_code_panel.c` and use a small color-segments array on the row, written by the adapter and walked by the renderer:

    ```c
    /* In text_panel.h — generic, no fade-specific naming */
    #define UI_TEXT_PANEL_MAX_COLOR_SEGMENTS 4
    typedef struct {
        int                 char_start;   /* inclusive */
        int                 char_count;
        UiTextPanelColor    color;        /* applied via one glColor4f for the whole span */
    } UiTextPanelColorSegment;
    /* On UiTextPanelRow: */
    UiTextPanelColorSegment color_segments[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    int                     color_segment_count;   /* 0 = use row's solid text_color */
    ```

    - Why this avoids a function call per char: the fade is monotonic and produces a typewriter-reveal shape — at any instant, characters before the wipe front are fully visible (α = 1.0), the leading character is mid-ramp (α ∈ (0, 1)), and characters after are hidden (α = 0.0). That's at most **3 segments per fading row**, regardless of row length. The renderer issues one `glColor4f` per segment and batches `glutBitmapCharacter` calls inside — `O(segments)` color changes, not `O(chars)`.
    - Why this is implementation-specific: the segment field is generic data ("color may vary across char ranges"), but the *fade-aware* code that calls `tutorial_step_fade_alpha` and computes the wipe front lives entirely in `ui_repl_code_panel.c`. The generic text panel never imports `widgets/tutorial.h` and has no `UiTextPanelFadeFn` symbol in its API surface.
    - Default cost: rows with `color_segment_count == 0` (the 99% case) bypass the segment loop entirely and use the row's `text_color` — no overhead added for non-fading rows.
    - Adapter responsibility: for any row where `tutorial_line_is_fading(line_idx, now)` is true, the adapter calls a local helper (e.g. `static void fill_fade_segments(int line_idx, int line_len, float now, UiTextPanelRow *row);`) that resolves the wipe-front character and writes 1-3 segments into the row. The wipe-front position is computed from timing state currently private to `src/widgets/tutorial.c` (fade_start_t, fade_duration, per-char window), so the adapter cannot derive it cheaply from outside. Two acceptable options:
      1. **Expose a tutorial API** (preferred). Add a single helper to `src/widgets/tutorial.h`:
         ```c
         /* Returns the index of the first character whose fade alpha is < 1.0
          * at `now`. Returns -1 if the line is not fading or `now` is past the
          * fade window (all characters fully revealed). When >= 0, the caller
          * can split the row into [0, front-1] @ alpha 1.0, [front] @ partial
          * alpha (read via tutorial_step_fade_alpha), and [front+1, end] @
          * alpha 0.0. */
         int tutorial_step_fade_front(int line_idx, int line_len, float now);
         ```
         The tutorial module already has the timing math; surfacing the front position is one helper, O(1), and keeps the fade math owned by the widget.
      2. **Bounded binary search** over `tutorial_step_fade_alpha`. The function is monotonic in `char_idx` (later characters reveal later), so a binary search on `alpha < 1.0` finds the front in `ceil(log2(line_len))` calls — 7 calls for a 100-char line, 10 for a 1000-char line. The plan's "~3 calls" claim is wrong; it should say `O(log2 line_len)` if option 2 is chosen.

      Pick option 1 for v1: the API is narrower, the cost is one helper definition, and it doesn't lock the adapter to the current binary-search-friendly shape of the fade math.
  - render REPL-specific statusbar after `ui_text_panel_render()` returns, using the `UiTextPanelOutput.statusbar_slot_rect` so the adapter doesn't recompute layout.
- Keep existing visual behavior by having:
  - `ui_panels_render_code_panel()` call `ui_repl_code_panel_render()`
  - `ui_repl_code_panel_render()` build the generic snapshot and call `ui_text_panel_render()`
- Code touched: new `src/ui/repl_code_panel.{c,h}`, `src/ui/panels.c`, `src/ui/panels.h`, **delete `src/editor/code_panel_document.{c,h}`** (its pure half lives in `src/ui/text_layout` since Phase 1; its REPL-aware half is now part of `src/ui/repl_code_panel.c`), update `src/editor/state.h`'s include and any test references to the deleted header, `Makefile` source/header lists.

## Phase 4 — Overlay-Priority Hit Routing

- Generic mouse → row mapping already moved in Phase 2 (shares layout math with rendering). Phase 4 finishes the hit-test split by keeping overlay-priority routing in `ui_panels_hit_test` and the REPL-specific routing in `ui_repl_code_panel_hit_test`:
  - help overlay (modal, beats everything)
  - color picker (modal while open)
  - menu bar
  - variable panel
  - color swatch row action — drawn as `right_action`, routed through the adapter, not the generic panel
  - scene fallback when no panel hit lands
- Generic `ui_text_panel_hit_test` returns only text-panel hit kinds: text row, insert/input row, gutter, panel divider, none.
- When the generic hit lands on a `VIRTUAL` row with `hit_target_line_idx >= 0`, the adapter rewrites the hit to point at the owning source line before returning to the caller — matches the current `code_panel_document` routing for replay annotations so users can still click replay-evaluated rows to navigate to their source.
- Code touched: `src/ui/repl_code_panel.c`, `src/ui/panels.c`.

## Phase 5 — Editor REPL/Chrome Decoupling

This phase is the load-bearing prerequisite for Phase 6's editor demo. It is **not** about the demo — it's a focused refactor of `src/editor/input.c` and `src/editor/commit.c` that pulls their REPL-pipeline and app-controller reach behind registered service tables. The demo in Phase 6 is downstream proof that this phase worked.

The starting point is **not green-field**: `src/editor/services.h` already defines an `EditorServices` table that `src/editor/commit.c` uses for compile/apply (`context`, `compile`, `apply_repl_change`, `apply_predef_ops`, `apply_scratch_ops`). Phase 5 **extends** that seam rather than introducing a parallel `EditorReplServices`. The chrome reach gets its own complementary `EditorChromeServices` table.

### Motivation — measurement table (2026-05-13)

REPL-symbol surface of each `src/editor/*.c` file:

| File | REPL functions called | Notes |
|------|----------------------|-------|
| `state.c` | 1 (`repl_state_edit_line`) | One-line stub. Per-file stub growth — fine. |
| `clipboard.c` | ~10 (command_store, source_scope, status) | Moderate; mutators are no-ops in the demo. |
| `undo.c` | ~10 (command_store, func_alias, eval, promote_example) | Moderate; `repl_promote_example_if_needed` is the only REPL-semantics call. |
| `input.c` | **23** (parse + compile + command_store + status) | **This phase reduces this to ~5.** Also has chrome reach (`glr_*`, `ui_*`, `color_picker_*`) covered by `EditorChromeServices`. |
| `commit.c` | **33** (compile / apply / func_alias / eval / source_scope) | **This phase reduces this to ~5.** Already routes some calls through `EditorServices` today — extension target. |

`code_panel_document.c` is **not in this table** — it was split during Phases 1 and 3 (pure half → `src/ui/text_layout`; REPL-aware half → `src/ui/repl_code_panel.c`) and no longer exists in `src/editor/` by the time Phase 5 starts.

Sum today: **~77 REPL function calls across the five remaining files** — roughly 5× the Phase 6 shim tripwire (~12-15 functions). The three smaller files (sum: ~21) can be shimmed directly. The two largest carry the real coupling and have to come down before any demo work can land coherently.

### What this phase produces

- **Extended `EditorServices` table** (`src/editor/services.h`). The existing struct already covers compile/apply for `commit.c`; this phase adds the remaining REPL-pipeline surface `input.c` and `commit.c` currently reach by name:
  - command-store mutators (`repl_command_store_*`)
  - parser front-door (`repl_parser_parse_command_ctx`)
  - eval helpers used in commit (`repl_eval_parse_exprs`, `repl_eval_validate_expression_idents`, etc.)
  - func-alias bookkeeping (`repl_func_alias_*`)
  - source-scope queries (`repl_source_scope_*` — pure, but uniform dispatch keeps the shape clean)
  - dirty-state notifier (`repl_mark_normals_dirty`)
  - status sink (`repl_set_status` — likely the existing `repl_set_status_sink` extended)
  - The existing compile/apply members stay as-is. `editor_services_default()` keeps working unchanged for production code; the new fields populate via the same default.
- **`EditorChromeServices` table** (`src/editor/chrome_services.h`). Covers app-controller reach from `input.c`:
  - `glr_camera_controls_reset`
  - `glr_ctrl_router_reset_code_panel_drag`
  - `glr_ctrl_sync_ui_chrome`
  - `glr_state_presentation` / `glr_state_presentation_mut` getters/setters for `code_panel_layout`
  - `color_picker_*` reach from `input.c` (audit during implementation). These route through `EditorChromeServices` too — keeping color-picker dispatch on the same table avoids growing a third seam for a handful of calls. The color-picker module's own state continues to live in `src/widgets/color_picker_state.c`; `EditorChromeServices` just exposes the open/close/close-if-active entry points the input dispatcher needs.
  - Current direct `ui_*` reach from `input.c`: status mutations (`ui_state_status_mut` / eventual status setter), help visibility access, code-panel runtime state, viewport height/width, `ui_layout_code_panel_rect`, and `ui_menu_bar_close`. Because the chrome-surface guard below counts `ui_*(`, the plan chooses to route these through `EditorChromeServices`; if implementation intentionally allows any direct UI calls to remain, narrow the guard to the disallowed symbols instead of counting every `ui_*`.
- Controller registers production bindings at app init (`glr_ctrl_init_gl` or equivalent). `input.c` / `commit.c` switch their REPL/chrome calls to the registered tables.
- The pattern matches existing seams: `repl_set_status_sink`, `repl_install_input_reset_sink`, the existing `EditorServices` — same dispatch shape, same lifecycle.

### Target reductions

- `input.c`: 23 → ~5 REPL function calls + chrome/UI reach routed through `EditorChromeServices` (verified by the chrome-surface guard).
- `commit.c`: 33 → ~5 REPL function calls.
- The three smaller editor files stay where they are — they're already within budget.

### Validation

- Existing `make test`, `make test-stubs`, `make check-state-ownership` pass with no changes to test fixtures.
- Test fixtures `testdata/repl_examples_ui/*.golden.txt` byte-equal after the refactor.
- Two greppable guards:
  - `scripts/check-editor-repl-surface.sh` counts `repl_*(` calls in `src/editor/input.c` and `src/editor/commit.c`; fails if either exceeds a ratcheted threshold (start at 8).
  - `scripts/check-editor-chrome-surface.sh` counts `glr_*(`, `ui_*(`, and `color_picker_*(` calls in `src/editor/input.c`; fails if it exceeds a ratcheted threshold (start at 4). Without this, `EditorChromeServices` becomes a paper seam that gets bypassed in the next patch.
- Both service tables have a "not yet installed" assert path so an uninstalled binary fails loudly rather than null-deref.

### Code touched

- `src/editor/input.{c,h}` — switch direct REPL calls to the extended `EditorServices` table; switch chrome/UI reach to `EditorChromeServices`.
- `src/editor/commit.{c,h}` — extend the existing `EditorServices` consumption to cover the rest of the REPL-pipeline surface.
- `src/editor/services.{c,h}` — add new fields to the `EditorServices` struct; extend `editor_services_default()` to populate them.
- new `src/editor/chrome_services.{c,h}` — chrome service-table struct + registration entry point.
- `src/app/glr_ctrl.c` — register production bindings for `EditorChromeServices` at init; production `EditorServices` already comes through `editor_services_default()`.
- new `scripts/check-editor-repl-surface.sh` and `scripts/check-editor-chrome-surface.sh` — surface-count regression gates.
- `Makefile` — wire both check targets.

## Phase 6 — Add Editor Demo Host

This phase only lands after Phase 5. By the time Phase 6 starts, `input.c` / `commit.c` route their REPL/chrome reach through service tables, so the demo's job is to populate those tables with fake/no-op bindings — not to fight against direct calls.

- Add `tools/editor_demo/editor_demo.c`.
  - GLUT window setup and callback registration.
  - Links `src/editor/state.c` directly and uses the same global `EditorState` the full app uses — no parallel state instance.
  - Builds a `UiTextPanelSnapshot` directly from `EditorState` and fake document rows.
  - Applies `ReplInputDispatchEffects`: redraw, cursor, timer.
  - Calls `editor_handle_key`, `editor_handle_special`, mouse handlers, and wheel handler.
- Add `tools/editor_demo/repl_shim.c`. Provides a fake `EditorServices` instance (registered via the same install path the controller uses) with no-op/fake implementations:
  - Static fake document: `GLCmd cmds[MAX_COMMANDS]`, `count`, `edit_line`.
  - Fake parser: empty line -> `CMD_EMPTY`; non-empty text -> inert `CMD_COMMENT`; canonical text is stripped input without trailing `;`.
  - Fake command store/state functions expected by editor input.
  - No-op source-scope, tutorial, replay, variable, color-picker, export, and dirty-state functions.
  - Registering an `EditorCompletionProvider` is **optional**, not required for safety: `editor_completion_update`, `editor_completion_update_selected_preview`, and `editor_completion_clear` in `src/editor/completion.c` all early-return when `g_provider == NULL`. The demo skips registration entirely — there's no grammar to suggest from, so ghost/hint stay empty and the Tab key path no-ops cleanly.
  - Status messages forward to `ui_state_status_set`.
- Add `tools/editor_demo/app_chrome_shim.c`. Populates `EditorChromeServices` with no-op camera/router reset plus local code-panel layout state. Production bindings stay in the controller. Kept separate from `repl_shim.c` so the dependency ledger distinguishes text/REPL semantics from app chrome.
- Shim-size tripwire (regression gate, **not** entry budget): count **unique shim functions / exported symbols**, not raw call sites. `repl_shim.c` should stay under ~12-15 unique REPL symbols after the `input.c` / `commit.c` reductions; the chrome shim should stay under ~5-7 unique chrome/UI symbols. The three smaller editor files still have ~21 raw direct REPL call sites today, so this cap is only realistic if those calls dedupe to a small symbol set after Phase 5. If the actual unique-symbol count still exceeds the cap, do not hand-wave it — either raise the tripwire explicitly with the measured count or coarsen the service callbacks so the demo does not grow a parallel REPL implementation. Growth past the final measured cap means a regression in the seam contract.
- Add `make editor_demo`.
  - `USE_GL_STUBS=1` verifies compile/link only.
  - Real GL build opens the editor demo window.
- Code touched: `tools/editor_demo/*`, `Makefile`.

## Phase 7 — Guards And Documentation

- Add a guard target, e.g. `check-ui-text-panel-pure`.
  - Fail if `src/ui/text_panel.*` includes `repl/`.
  - Fail if it references `GLCmd`, `CmdType`, or `CMD_`.
- Wire `scripts/check-editor-repl-surface.sh` **and** `scripts/check-editor-chrome-surface.sh` (both added in Phase 5) into `make check` so the `input.c` / `commit.c` REPL-call surface and the `input.c` chrome-call surface each stay below threshold over time. Ratchet both thresholds down as further decoupling lands.
- Add `tests/test_ui_text_panel.c` (built with `USE_GL_STUBS=1`).
  - Drives `ui_text_panel_render` and `ui_text_panel_hit_test` with a fabricated snapshot (rows constructed inline, no REPL state).
  - Asserts: total/visible row counts, cursor pixel for a known input, hit-test row mapping for known coordinates, statusbar slot rect dimensions.
  - Locks the contract independently of the REPL pipeline so future refactors can't quietly drift.
- Add root-level `editor_demo` symlink alongside `sample` / `repl_demo` so `./editor_demo` runs the binary from the repo root (matches the existing convention).
- Update `MODULES.md` and `feature/editor-demo.md`:
  - `ui_text_panel` is generic text rendering/hit-test.
  - `ui_repl_code_panel` is the REPL adapter.
  - `tools/editor_demo/repl_shim.c` is a dependency ledger, not production architecture.
- Code touched: `Makefile`, `scripts/check-ui-text-panel-pure.sh`, `tests/test_ui_text_panel.c`, docs.

## Test Plan

- Build/check:
  - `make editor_demo USE_GL_STUBS=1`
  - `make sample USE_GL_STUBS=1`
  - `make repl_demo USE_GL_STUBS=1`
  - `make scene_demo USE_GL_STUBS=1`
  - `make test-stubs` (includes new `test_ui_text_panel`)
  - `make test_ui_text_panel USE_GL_STUBS=1` (focused unit run)
  - `make check-state-ownership`
  - `make check-ui-text-panel-pure`
  - `make check-editor-repl-surface` (input.c / commit.c REPL surface gate)
  - `make check-editor-chrome-surface` (input.c chrome surface gate)
- Manual full-app smoke:
  - code panel renders header/footer, command rows, colors, search, active input, replay annotations, tutorial fade, color swatches, statusbar, and hit-test routing.
- Manual editor-demo smoke:
  - type text, commit lines, navigate, edit existing lines, delete, search, select/copy/paste input text, scroll, resize panel.

## Assumptions

- `editor_demo` is a plain text editor proof, not a GL language editor.
- `src/ui/panels.h` remains the stable public surface for the full app.
- The fake REPL shim is intentionally demo-local and should not migrate into production code.
- After Phase 5 lands, the shim is a **dependency ledger against the extended `EditorServices` and the new `EditorChromeServices` tables, plus a small set of direct stubs** for the three smaller editor files (`state.c`, `clipboard.c`, `undo.c`) that keep calling REPL helpers by name. Its role is to populate the tables with no-op or fake function pointers and to stub the remaining direct calls. The Phase 6 tripwires (~12-15 unique REPL shim symbols / ~5-7 unique chrome/UI shim symbols) are regression gates on that ledger, not a promise that ~21 remaining raw call sites magically fit under a 15-function cap. Before landing Phase 6, measure the actual unique symbol set required by those three files; if it is larger than the cap, either extend Phase 5 to coarsen the service surface or record a deliberately higher tripwire with rationale.
- Further cleanup of `src/editor/input.c`, `src/editor/clipboard.c`, and `src/editor/undo.c` into generic document services is the long-term direction. Phase 5's seam extraction is the first concrete step; the demo in Phase 6 is the scaffolding that makes the remaining coupling visible and shrinkable.

## Landing Strategy

- **Phases 0-2** are the load-bearing UI split: text-panel module exists, generic rendering + hit-mapping live there, full app still works through the unchanged `ui_panels_*` surface. This is the first natural pause point — the cleanup is real even without the rest.
- **Phase 3** is mechanical once Phase 2 lands; **Phase 4** is hit-routing cleanup. Together these complete the SRP split for `ui/panels.c`.
- **Phase 5** is the editor-side decoupling: **extend** the existing `EditorServices` seam (already used by `commit.c` for compile/apply) to cover the rest of the REPL-pipeline surface and add a complementary `EditorChromeServices` seam for `input.c`'s `glr_*` / `ui_*` / `color_picker_*` reach. This is the second natural pause point — the project still has no second editor binary, but the editor module set is now much closer to linkable without the REPL pipeline. Useful on its own: the service tables also make the existing test harnesses easier to drive in isolation.
- **Phase 6** (demo) lands only after Phase 5's measured surface reductions land. By that point the shims become small dependency ledgers against the service tables, and the tripwires (~12-15 REPL fns / ~5-7 chrome fns) are meaningful regression gates rather than aspirational.
- **Phase 7** (guards + docs) lands incrementally as each preceding phase merges — don't batch the purity guard until the end.
