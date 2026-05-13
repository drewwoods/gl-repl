# Editor Demo + SRP Split For Code Panel UI

## Summary

Split the code-panel UI into a generic text-panel renderer plus a REPL-specific adapter, then add `editor_demo` backed by a demo-local fake REPL shim. The goal is not to make the editor fully reusable in one step; it is to create a working standalone proof and improve `ui/panels.c` by separating text-editor rendering from REPL presentation.

## Phase 0 — Baseline And Invariants

- Record current behavior before refactor:
  - Build `make sample USE_GL_STUBS=1`, `make repl_demo USE_GL_STUBS=1`, `make scene_demo USE_GL_STUBS=1`.
  - Run `make test-stubs` and `make check-state-ownership`.
  - **Capture baseline snapshots** so visual regressions are catchable later: regenerate `testdata/repl_examples_ui/*.golden.txt` from the current binary and save the SHA before any refactor; copy a few `--dump-code` outputs of representative scenes to `/tmp/editor-demo-baseline/` for byte-level diffs after Phase 2 and Phase 3 land. The example-fixture suite is the existing golden harness — re-running it after each phase is the primary regression check.
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
  - `UiTextPanelSearch`: active flag, query, query length, hit row/char. The adapter resolves the REPL-side source-line index to the snapshot's row index before populating `hit_row` (same shape as replay-row routing — REPL line indices are translated to text-panel row indices in the adapter).
  - `UiTextPanelSnapshot`: viewport, panel rect, rows, row count, scroll, visible chrome flags, input/search/completion state.
    - `rows` is a fixed inline array of `UI_TEXT_PANEL_ROW_CAP` (= 512) `UiTextPanelRow` entries. The adapter clips to a "visible window + small scroll buffer" before building the snapshot, so it never has to ship the entire 4096-command document; 512 covers any realistic panel height (~24-50 visible rows) with ~10× headroom for wrapping plus scroll overscan.
  - `UiTextPanelOutput`: cursor pixel, cursor-valid, total rows, visible rows, **text-area rect + statusbar-slot rect** so the REPL adapter can overlay its status strip without recomputing layout (statusbar is REPL chrome — see Phase 3).
- Add APIs:
  - `ui_text_panel_visible_lines_for_height(int panel_h)`
  - `ui_text_panel_render(const UiTextPanelSnapshot *, UiTextPanelOutput *)`
  - `ui_text_panel_hit_test(const UiTextPanelSnapshot *, int mx, int my)`
- Layout helpers are pure pixel-math, not editor state — **move `src/editor/code_layout.{c,h}` to `src/ui/text_layout.{c,h}`** as part of this phase so the text panel doesn't have to include from a higher-level module. Update every existing caller (`src/editor/code_panel_document.c`, `src/ui/panels.c`, `tests/test_repl_editor.c`, etc.) to the new include path.
- Constraints:
  - `src/ui/text_panel.*` must not include `repl/*` or `src/editor/*`.
  - It must not mention `GLCmd`, `CmdType`, or `CMD_*`.
- Code touched: new `src/ui/text_panel.{c,h}`, renamed `src/ui/text_layout.{c,h}` (was `src/editor/code_layout.{c,h}`), every existing caller of `code_layout_*` for the include rename, `Makefile` source/header lists.

## Phase 2 — Extract Generic Rendering

- Move generic code-panel rendering from `src/ui/panels.c` into `src/ui/text_panel.c`:
  - panel background/divider
  - line wrapping using `src/ui/text_layout.h` (renamed from `src/editor/code_layout.h` in Phase 1)
  - gutter line numbers
  - active input row
  - caret, input selection, autocomplete ghost/hint
  - search highlight drawing
  - scrollbar
- Move the generic hit-mapping math (mouse → row / source line / visual row / input cursor char) in the **same** phase. It shares every layout call with rendering, so splitting it across phases would force the same `text_layout_*` walks to be reproduced twice. Phase 4 keeps only the overlay-priority routing, which is independent of layout math.
- Keep REPL-only features out:
  - command colors
  - header/footer scaffolding
  - vertex/tess labels
  - replay rows
  - tutorial fading
  - color swatches
  - REPL statusbar content
- Code touched: `src/ui/panels.c`, new `src/ui/text_panel.c`, `tests/test_repl_editor.c` (private `code_panel_header_row_count` helper duplicates the header-row math; any rename/move of those primitives during this phase ripples here — keep the helper aligned in the same patch).

## Phase 3 — Add REPL Code Panel Adapter

- Add `src/ui/repl_code_panel.{c,h}`.
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
    - Adapter responsibility: for any row where `tutorial_line_is_fading(line_idx, now)` is true, call a local helper (e.g. `static void fill_fade_segments(int line_idx, int line_len, float now, UiTextPanelRow *row);`) that calls `tutorial_step_fade_alpha` at most ~3 times to find the wipe-front character and writes 1-3 segments into the row. Cap at `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS` (4); fall back to a single dimmed segment if the cap is ever exceeded (it won't be in practice).
  - render REPL-specific statusbar after `ui_text_panel_render()` returns, using the `UiTextPanelOutput.statusbar_slot_rect` so the adapter doesn't recompute layout.
- Keep existing visual behavior by having:
  - `ui_panels_render_code_panel()` call `ui_repl_code_panel_render()`
  - `ui_repl_code_panel_render()` build the generic snapshot and call `ui_text_panel_render()`
- Code touched: new `src/ui/repl_code_panel.{c,h}`, `src/ui/panels.c`, `src/ui/panels.h`, `Makefile`.

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

## Phase 5 — Add Editor Demo Host

- Add `tools/editor_demo/editor_demo.c`.
  - GLUT window setup and callback registration.
  - Links `src/editor/state.c` directly and uses the same global `EditorState` the full app uses — no parallel state instance.
  - REPL-symbol surface of each `src/editor/*.c` file (measured 2026-05-13):

    | File | REPL functions called | Shim cost |
    |------|----------------------|-----------|
    | `state.c` | 1 (`repl_state_edit_line`) | Trivial — one-line stub returning a local int. Just shim it. |
    | `code_panel_document.c` | ~8 (state, source_scope, export) | Small. Most are already pure queries (export getters are no-side-effect). Shim them. |
    | `clipboard.c` | ~10 (command_store, source_scope, status) | Moderate. The command_store mutators are where work happens; mostly no-ops in the demo. |
    | `undo.c` | ~10 (command_store, func_alias, eval, promote_example) | Moderate. `repl_promote_example_if_needed` is the only REPL-semantics call; no-op in the demo. |
    | `input.c` | 23 (parse + compile + command_store + status) | Larger. This is where the "is the editor really decoupled" question lives. |
    | `commit.c` | 33 (compile / apply / func_alias / eval / source_scope) | Largest. Genuine REPL pipeline use — the right place to push real decoupling work into a follow-up. |
  - Policy: **extend the shim within reason.** The shim is already an artifact of present coupling; growing it by a stub or two per file is fine. The tripwire numbers in this phase (~12-15 REPL functions / ~5-7 chrome functions) are the enforcement mechanism — when *either* gets uncomfortably close to its cap, that's the cue to pause and decouple at source rather than to keep stubbing.
  - In particular: `state.c`'s one function (`repl_state_edit_line`) is cheap to shim and produces no churn anywhere else. The hard-line "must not" framing earlier in this plan was too strict for that case.
  - Builds a `UiTextPanelSnapshot` directly from `EditorState` and fake document rows.
  - Applies `ReplInputDispatchEffects`: redraw, cursor, timer.
  - Calls `editor_handle_key`, `editor_handle_special`, mouse handlers, and wheel handler.
- Add `tools/editor_demo/repl_shim.c`.
  - Static fake document: `GLCmd cmds[MAX_COMMANDS]`, `count`, `edit_line`.
  - Fake parser: empty line -> `CMD_EMPTY`; non-empty text -> inert `CMD_COMMENT`; canonical text is stripped input without trailing `;`.
  - Fake command store/state functions expected by editor input.
  - No-op source-scope, tutorial, replay, variable, color-picker, export, and dirty-state functions.
  - Registering an `EditorCompletionProvider` is **optional**, not required for safety: `editor_completion_update`, `editor_completion_update_selected_preview`, and `editor_completion_clear` in `src/editor/completion.c` all early-return when `g_provider == NULL`. The demo skips registration entirely — there's no grammar to suggest from, so ghost/hint stay empty and the Tab key path no-ops cleanly.
  - Status messages forward to `ui_state_status_set`.
- Add `tools/editor_demo/app_chrome_shim.c` or, preferably, extract an `EditorChromeServices` seam before linking the demo:
  - Today `src/editor/input.c` still reaches app/controller chrome directly: `glr_camera_controls_reset`, `glr_ctrl_router_reset_code_panel_drag`, `glr_ctrl_sync_ui_chrome`, and `glr_state_presentation*`.
  - The demo must account for those hooks explicitly instead of accidentally pulling in `src/app/glr_ctrl.c`. A short-term shim can provide no-op camera/router reset plus local code-panel layout state; the cleaner implementation is a small editor-input service table with production bindings in the controller and demo bindings in `tools/editor_demo`.
  - Keep this separate from `repl_shim.c` so the dependency ledger distinguishes text/REPL semantics from app chrome.
- Shim-size tripwire: if `repl_shim.c` grows past ~12-15 functions, or the app/chrome shim grows past ~5-7 functions, treat that as a real coupling problem and pause to evaluate `editor/*` decoupling before continuing. The shims are meant to be small dependency ledgers, not sprawling parallel implementations.
- Add `make editor_demo`.
  - `USE_GL_STUBS=1` verifies compile/link only.
  - Real GL build opens the editor demo window.
- Code touched: `tools/editor_demo/*`, `src/editor/input.{c,h}` if an `EditorChromeServices` seam is extracted, `Makefile`.

## Phase 6 — Guards And Documentation

- Add a guard target, e.g. `check-ui-text-panel-pure`.
  - Fail if `src/ui/text_panel.*` includes `repl/`.
  - Fail if it references `GLCmd`, `CmdType`, or `CMD_`.
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
- Manual full-app smoke:
  - code panel renders header/footer, command rows, colors, search, active input, replay annotations, tutorial fade, color swatches, statusbar, and hit-test routing.
- Manual editor-demo smoke:
  - type text, commit lines, navigate, edit existing lines, delete, search, select/copy/paste input text, scroll, resize panel.

## Assumptions

- `editor_demo` is a plain text editor proof, not a GL language editor.
- `src/ui/panels.h` remains the stable public surface for the full app.
- The fake REPL shim is intentionally demo-local and should not migrate into production code.
- The shim is a **measurement device**, not a permanent architectural choice: it acknowledges current `src/editor/*` ↔ REPL coupling so the demo can link today. Extending it stub-by-stub is fine and expected — the tripwire (~12-15 REPL fns / ~5-7 chrome fns) is the budget. The long-term direction is splitting `src/editor/*` so the shim shrinks toward zero, but a small shim growth to make a file linkable is not a cardinal sin; it's the whole point of having a shim.
- Further cleanup of `src/editor/input.c`, `src/editor/clipboard.c`, and `src/editor/undo.c` into generic document services is the real goal of this initiative; this demo is the scaffolding that makes the coupling visible and shrinkable.

## Landing Strategy

- Phases 0-2 are the load-bearing refactor: text-panel module exists, generic rendering + hit-mapping live there, full app still works through the unchanged `ui_panels_*` surface. This is the natural pause point — the cleanup is real even without the demo.
- Phase 3 is mechanical once Phase 2 lands; Phase 4 is cleanup; Phase 5 is the proof-of-concept demo. If Phase 5's shim trips the size tripwire, pause and reassess whether decoupling `src/editor/*` from REPL types is a prerequisite rather than a follow-up.
- Phase 6 (guards + docs) lands incrementally as each preceding phase merges — don't batch the purity guard until the end.
