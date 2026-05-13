# Editor Demo + SRP Split For Code Panel UI

## Summary

Split the code-panel UI into a generic text-panel renderer plus a REPL-specific adapter, then add `editor_demo` backed by a demo-local fake REPL shim. Like `scene_demo` (which keeps `src/scene/` honest about its REPL dependencies) and `repl_demo`, `editor_demo` is a forcing function for module independence — a second binary that fails to link if the split regresses, turning "the module is independent" from a claim into a checkable invariant. The goal is not to make the editor fully reusable in one step; it is to create a working standalone proof and improve `ui/panels.c` by separating text-editor rendering from REPL presentation.

## Phase 0 — Baseline And Invariants

- Record current behavior before refactor:
  - Build `make sample USE_GL_STUBS=1`, `make repl_demo USE_GL_STUBS=1`, `make scene_demo USE_GL_STUBS=1`.
  - Run `make test-stubs` and `make check-state-ownership`.
  - **Capture baseline snapshots** so logical regressions are catchable later:
    - `testdata/repl_examples_ui/*.golden.txt` is a **logical** fixture (one row per header/source line, no wrap geometry), not a pixel-accurate visual fixture — see the comment at `tests/test_repl_core_examples.c:1044-1048` that calls this out and points to "the visual code-panel dump tests" as the place wrapped-row rendering is checked. Re-running the example-fixture suite after each phase catches structural drift (row counts, source order, header/footer scaffolding) but **does not** catch glyph-level visual regressions (color shifts, alpha blending, kerning, wrap geometry).
    - Current `--dump-code` output is useful as a source/export baseline, but it is still logical text: `sample.c` calls `glr_debug_dump_editor()`, which calls `repl_dump_code_panel_text()`, and that path does **not** exercise the wrap iterator. Capture those dumps for structural/source diffs if useful, but do not treat them as visual coverage.
    - For a checkable wrapped-rendering proxy, expose
      `repl_dump_code_panel_visual_text()` (already defined in
      `src/repl/export.c:3445`, just not wired through `--dump-code`) via
      a CLI flag such as `--dump-code-visual`, or add a UI-side test helper
      that emits wrapped rows from `ui_text_panel_render` inputs. Capture
      representative scenes (wrapped lines, tutorial mid-fade, replay
      annotations, color swatches) to `/tmp/editor-demo-baseline/` and
      byte-diff after Phase 2 and Phase 3 land. Manual smoke testing (Test
      Plan below) covers what text dumps cannot.
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
    - Cap at `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS` (4); fall back to a single dimmed segment if the cap is ever exceeded (it won't be in practice — fade is at most 3 segments).
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

**Prerequisite: the measured REPL-symbol surface of `src/editor/*.c` is
already past the shim tripwire.** The table below shows `input.c` and
`commit.c` together calling **~56 REPL functions** (23 + 33). The tripwire
near the end of this phase is ~12-15 REPL shim functions. Phase 5 as
written trips its own guard immediately, because the demo host has to call
`editor_handle_key` / `editor_handle_special` — which pulls in `input.c`,
which transitively pulls in `commit.c`. There is no way to link the demo
without shimming the full ~56-function surface today.

Two ways to resolve this:

1. **Decouple `input.c` and `commit.c` from REPL types first** (recommended).
   The relevant REPL surface in those files is parse/compile/apply, status
   set, command_store mutation, and func-alias/eval bookkeeping. The
   compile/apply path is where the "is the editor really decoupled"
   question lives. Land a focused refactor that pulls compile/apply onto
   an `EditorReplServices` table (or equivalent dispatch seam) registered
   by the controller, so `input.c`/`commit.c` call the table, not the
   REPL functions by name. The shim then only needs to populate the
   table with no-op or fake implementations — a handful of function
   pointers, well inside the tripwire. This is the same shape as the
   existing `repl_set_status_sink` / `repl_install_input_reset_sink` /
   `EditorServices` patterns the project already uses.

   The chrome reach in `input.c` (`glr_camera_controls_reset`,
   `glr_ctrl_router_*`, `glr_ctrl_sync_ui_chrome`, `glr_state_presentation*`)
   needs its own seam — call it `EditorChromeServices` — extracted in the
   same refactor so the demo doesn't accidentally pull in
   `src/app/glr_ctrl.c`. The two seams are orthogonal: `EditorReplServices`
   covers REPL-pipeline surface, `EditorChromeServices` covers
   app-controller chrome. Both must land before Phase 5.
2. **Raise the tripwire and accept the shim**. Treat 56-function shim as
   the price of the demo today and use the demo as the forcing function
   to motivate (1). The downside: the shim is no longer a small
   dependency ledger; it's a substantial parallel implementation, and
   the tripwire stops being a useful enforcement signal.

Pick (1). Phase 5 lands only after the `input.c`/`commit.c` decoupling
refactor reduces their REPL-function surface to something close to the
existing tripwire. The measurement table below stays as the entry-time
audit — when each file's count drops below ~5, Phase 5 is ready to go.

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
    | `input.c` | 23 (parse + compile + command_store + status) | **Past the tripwire today.** Needs the prerequisite refactor above. |
    | `commit.c` | 33 (compile / apply / func_alias / eval / source_scope) | **Past the tripwire today.** Needs the prerequisite refactor above. |

    Sum: **~85 REPL function calls across all six files** without the
    prerequisite refactor — roughly 6× the tripwire. The plan that
    "extends the shim within reason" is only coherent for the four
    smaller files (sum: ~29). The two largest files have to come down
    first.
  - Policy after the prerequisite refactor lands: **the shim is a small
    dependency ledger, not a parallel implementation.** Once
    `input.c`/`commit.c` route their REPL surface through
    `EditorReplServices`, the shim only populates the table with no-op or
    fake implementations — a handful of function pointers per file. Per-file
    stub growth of one or two functions to make a TU linkable is fine; the
    tripwires (~12-15 REPL functions / ~5-7 chrome functions) are quality
    gates against regression. If a future change pushes the shim past either
    cap, that's the cue to pause and decouple further at source rather than
    keep stubbing.
  - `state.c`'s one function (`repl_state_edit_line`) is cheap to shim and
    produces no churn anywhere else — exactly the per-file stub-growth case
    the policy above accepts.
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
- Add `tools/editor_demo/app_chrome_shim.c` that populates the
  `EditorChromeServices` table extracted in the prerequisite refactor:
  - Today `src/editor/input.c` reaches app/controller chrome directly:
    `glr_camera_controls_reset`, `glr_ctrl_router_reset_code_panel_drag`,
    `glr_ctrl_sync_ui_chrome`, and `glr_state_presentation*`. The
    prerequisite refactor routes those through `EditorChromeServices`.
  - The demo binds the table to no-op camera/router reset plus local
    code-panel layout state. Production bindings stay in the controller.
  - Keep this separate from `repl_shim.c` so the dependency ledger
    distinguishes text/REPL semantics (`EditorReplServices`) from app chrome
    (`EditorChromeServices`).
- Shim-size tripwire (regression gate, **not** entry budget): once the
  prerequisite refactor lands and Phase 5 is in flight, `repl_shim.c` should
  stay under ~12-15 functions and the chrome shim under ~5-7 functions.
  Growth past those caps means a regression in the seam contract — pause and
  re-evaluate before continuing. The shims are small dependency ledgers
  against the two service tables, not sprawling parallel implementations.
- Add `make editor_demo`.
  - `USE_GL_STUBS=1` verifies compile/link only.
  - Real GL build opens the editor demo window.
- Code touched: `tools/editor_demo/*`, `Makefile`. (`src/editor/input.{c,h}`
  and `src/editor/commit.{c,h}` change in the prerequisite refactor, not in
  Phase 5 itself.)

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
- The shim is a **dependency ledger against `EditorReplServices` /
  `EditorChromeServices`**, not a parallel implementation. After the
  prerequisite refactor lands, the shim's role is to populate those tables
  with no-op or fake function pointers. The tripwires (~12-15 REPL fns /
  ~5-7 chrome fns) are regression gates on that ledger, not a license to
  grow it case-by-case from today's ~56-function entry-point state — that
  state is what the prerequisite refactor is for.
- Further cleanup of `src/editor/input.c`, `src/editor/clipboard.c`, and
  `src/editor/undo.c` into generic document services is the long-term
  direction. The seam extraction in the prerequisite refactor is the first
  step; this demo is the scaffolding that makes the remaining coupling
  visible and shrinkable.

## Landing Strategy

- Phases 0-2 are the load-bearing refactor: text-panel module exists, generic rendering + hit-mapping live there, full app still works through the unchanged `ui_panels_*` surface. This is the natural pause point — the cleanup is real even without the demo.
- Phase 3 is mechanical once Phase 2 lands; Phase 4 is cleanup.
- **Phase 5 is gated on a separate `input.c`/`commit.c` decoupling pass.** The 2026-05-13 measurement table (56 REPL functions across those two files) already trips the shim tripwire by ~4×. Treat the decoupling refactor as a prerequisite, not a follow-up — without it, Phase 5 ships a 56-stub parallel implementation that defeats the purpose of having a tripwire.
- Phase 6 (guards + docs) lands incrementally as each preceding phase merges — don't batch the purity guard until the end.
