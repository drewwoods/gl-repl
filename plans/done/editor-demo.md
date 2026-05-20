# Editor Demo + SRP Split For Code Panel UI

> **Current direction:** see ["Updated direction
> (2026-05-20)"](#updated-direction-2026-05-20--generic-text-editor-demo--shared-edit_ops)
> and ["Phase 8"](#phase-8--demo-refit-generic-text-editor--shared-edit_ops-in-progress)
> below. They supersede the Summary, Phase 5, Phase 6 (link-set
> portion), Phase 7b, Assumptions, and Landing Strategy sections,
> which are retained as historical context only.

## Summary (historical — superseded by Phase 8)

Three steps: (1) split the code-panel UI into a generic text-panel renderer plus a REPL-specific adapter; (2) decouple `src/editor/input.c` and `src/editor/commit.c` from the REPL pipeline by **extending** the existing `EditorServices` seam (`src/editor/services.h`, today scoped to compile/apply via `commit.c`) so the editor module set can link without the full REPL. UI chrome (menu bar, help overlay, color picker, tutorial) is treated as **editor-inherent**: any standalone editor binary needs equivalents of those modules, so the editor depends on `src/ui/` and `src/widgets/` directly rather than abstracting them behind a second service table. The residual upward `glr_*` reach (~3-4 sites: camera reset, code-panel presentation, panel-drag router) is small enough to stub as direct symbols rather than abstract; (3) add `editor_demo` as the forcing function that proves the split. Like `scene_demo` (which keeps `src/scene/` honest about its REPL dependencies) and `repl_demo`, `editor_demo` is a second binary that fails to link if the split regresses, turning "the module is independent" from a claim into a checkable invariant. The near-term implementation can stop after Phases 0-4 (plus the relevant Phase 7 docs/guards): that still completes the SRP cleanup for `ui/panels.c`. Phases 5 and 6 are deferred draft work and need a fresh review before implementation.

Phases 0-4 and 7a have landed and are still authoritative for the
UI split. Phases 5, 6, 7b describe the EditorServices/large-shim
approach that the "Updated direction" pivot replaced; treat their
service-table designs and shim-shrinkage targets as historical.

## Current state (2026-05-20) — Phase 8 v1 landed

Phase 8 v1 is implemented and pushed on branch
`editor-repl-decoupling`. The generic editor demo binary builds,
renders text via `ui_text_panel`, dispatches its own keys via a
generic dispatcher, and ships with a working File menu — all
without linking any REPL-flavored editor controller files.

### Landed

| Commit | Phase | What |
|--------|-------|------|
| `456c4aa` | (rescope) | Plan moved to `plans/active/`. `EditorChromeServices` dropped: chrome is editor-inherent, residual `glr_*` reach stubbed as direct symbols. |
| `c1247bf` | Phase 5 entry | Surfaces re-measured against current tree: input.c=22, commit.c=30 unique `repl_*` symbols. input.c `glr_*` reach=6 (vs draft "~3-4", below the 8-symbol reopen threshold). |
| `a5b6388` | Phase 5 step | `parse_command_ctx` routed through `EditorServices` — 5 call sites migrated as the worked example of the migration pattern. Kept as a worked example for the REPL editor's own testability; not load-bearing for the demo. |
| `b6250bc` | **Phase 6** | `tools/editor_demo/editor_demo.c` skeleton + `tools/editor_demo/repl_shim.c` (~85 direct stubs at the time). |
| `01c60d3` | **Phase 7b** | `scripts/check-editor-repl-surface.sh` ratchet + baseline (input.c=22, commit.c=30). Wired into `make check` and `make check-state-ownership`. |
| `d5a0407` | **Phase 7b** | `MODULES.md` documents the `editor_demo` binary alongside `repl_demo` and `scene_demo`. |
| `4ef3761` | (review) | Plan update: recorded current state, named next steps, paused for review. |
| `ff0b073` | (pivot) | Plan refit: generic text editor demo + shared `edit_ops` library; Phase 8 scope locked. |
| `1a88444` | (review fixes) | Marked historical sections superseded; narrowed v1 scope (no undo, no search, no status bar); made `check-edit-ops-pure` required. |
| `28fccb7` | **Phase 8.3** | New `src/editor/edit_ops.{c,h}` library with 5 primitives (buffer insert/delete, selection consume, type-char, backspace). `editor_input_consume_selection` moved into `edit_ops` as `edit_op_consume_input_selection`. Two `src/editor/input.c` handlers (printable, text-delete) migrated. `scripts/check-edit-ops-pure.sh` wired into `make check` + `check-state-ownership`. |
| `d114512` | **Phase 8.5** | `editor_demo` display callback builds a `UiTextPanelSnapshot` from `EditorState` and calls `ui_text_panel_render`. Adds `text_panel`/`text_layout`/`text_search` to demo link set. |
| `0c83781` | **Phase 8.6** | `tools/editor_demo/menu.{c,h}` — minimal File menu (Load / Save / Quit). Load/Save are unimplemented; Quit calls exit. |
| `5c3020d` | **Phase 8.4** | `tools/editor_demo/input.{c,h}` — generic input dispatcher. Demo's GLUT keyboard callbacks route through `demo_input_handle_key` / `demo_input_handle_special` instead of `editor_handle_key`. v1 covers printable ASCII, backspace, Escape, and cursor-within-line nav. |
| `61407e4` | **Phase 8.7+8.8** | `EDITOR_DEMO_DEP_SRCS` reduced to **6 source files**: `edit_ops.c`, `state.c`, `text_layout.c`, `text_panel.c`, `text_search.c`, `prof.c` (+ `gl_stub_counts.c`). All REPL-flavored controller files dropped. `repl_shim.c` shrunk from ~85 stubs to **1**: `repl_state_edit_line` (the acknowledged `state.c` leak). |

Full regression: 6292/6292 tests across 45 binaries clean; full
`make check-state-ownership` clean.

### Phase 8 outcome vs. plan

- Demo link set: target was "well under 20" unique shim symbols
  (8.8). Actual: **1**. The one remaining stub is exactly the
  acknowledged `state.c` edit-line leak called out in the
  "Editor files that aren't yet generic" inventory.
- `tools/editor_demo/repl_shim.c` is now an honest one-symbol
  ledger. Any second entry is a layering-regression signal.
- `check-edit-ops-pure` is wired and required, locking the
  edit_ops boundary against silent regression.
- `MODULES.md` and `CLAUDE.md` updated (Phase 8.10): `input.c`
  relabelled as the REPL editor input dispatcher; new `edit_ops`
  entry added; `editor_demo` description updated to describe the
  current link set + the v1 generic-editor behavior.

### What's still open (not v1 scope)

These are intentionally deferred from Phase 8 — each is a separate
follow-up phase:

- **Edit-line ownership cleanup. DONE 2026-05-21** — see
  `plans/done/edit-line-ownership.md`. Option A landed in 7 phases
  (storage flip + accessor migration + scenes/load.c via a
  controller-installed host-effects sink + shim deletion). The
  options below are retained as historical context for the
  decision.

  - **Option A — Full ownership migration.** Move edit-line state
    out of `ReplState` into `EditorState`. Concretely:

    1. Add storage and accessors on the editor side:
       `editor_state_edit_line()` / `_set()` / `_clamp()` plus
       backing storage on `EditorDocumentState` (or an extension
       of `EditorInputState`).
    2. Migrate every reader. The 176 sites split roughly by
       owner: 8 editor controller files (`input.c`, `commit.c`,
       `clipboard.c`, `undo.c`, `reformat.c`, `search.c`,
       `state.c`, `inline_file_prompt.c`), 3 app-shell files
       (`glr_ctrl.c`, `glr_actions.c`, `glr_debug.c`), 5 REPL
       pipeline files (`compile.c`, `flatten.c`, `parser.c`,
       `scenes.c`, `state.c`), 1 widget (`replay.c`), and ~10
       test files plus the demo.
    3. Decide what `repl_state_edit_line` / `_set` / `_clamp` in
       `src/repl/state.c` become: deleted outright, or kept as
       thin forwarders to the editor accessors.
    4. Drop the shim stub; delete `tools/editor_demo/repl_shim.c`
       (or keep it as a zero-stub ledger, the way
       `tools/repl_demo/stubs.c` is).
    5. Update MODULES.md / CLAUDE.md to reflect that edit-line is
       editor-owned.

    Cost: multi-day refactor. Touches all 33 files.

    Open architectural question: the REPL pipeline currently
    reads edit-line. After migration, those reads become a
    backward dependency (REPL → editor). Either accept that
    framing ("the REPL pipeline takes the editor's cursor
    position as input") or push the reads up into the caller —
    the controller / commit code that knows both states — and
    pass edit-line through as a parameter. Pick one before
    starting; that choice drives the call-site rewrites.

    This is the architecturally pure answer; it also has the
    largest blast radius and needs the design question pinned
    down before execution.

  - **Option B — Targeted view-field cleanup.** Smaller and
    keeps edit-line where it is today. Concretely:

    1. Delete `edit_line_idx` from `EditorInputView` (the
       by-value snapshot returned by `editor_state_input()`).
    2. `src/editor/state.c`'s view builder stops calling
       `repl_state_edit_line()` — the field is gone, the call
       isn't needed.
    3. Anyone reading `input.edit_line_idx` migrates to calling
       `repl_state_edit_line()` directly (it was always the
       canonical source; the view field was a convenience copy).
       Expected: ~5 call sites.
    4. Drop the shim stub; delete `tools/editor_demo/repl_shim.c`.

    Cost: 1-2 commits. No reorganization of edit-line storage —
    it still lives in `ReplState`. The win is honest: the demo's
    `state.c` link no longer requires a REPL stub, so the demo
    is shim-free.

    Honesty cost: this does **not** fix the underlying layering
    (the REPL editor's controllers still read edit-line from
    REPL state). It just removes the surface that the demo
    happens to trip over. If "no shim file" is the goal, B
    delivers it. If "edit-line is owned by the editor" is the
    goal, B leaves that work undone.

  Recommendation: pick B unless and until REPL→editor coupling
  becomes a real concern (right now it isn't — the REPL pipeline
  legitimately needs to know which line it's about to parse). A
  is the right move if the project later decides EditorState
  should own all editing cursors; B is the right move if "the
  demo is shim-free" is the immediate target.
- **Cross-line navigation.** Arrow up/down between lines, Enter to
  insert a new line. Needs new `edit_ops` primitives (or local
  demo logic) and decision on how the demo's edit-line cursor
  advances.
- **Undo/redo for the demo.** Generic text-only undo ring inside
  `EditorState`, separate from `src/editor/undo.c`'s REPL-flavored
  predef-var / scratch-array snapshotter.
- **Find/search for the demo.** Generic text find primitive in
  `edit_ops` (separate from `src/editor/search.c`'s REPL-state
  coupling).
- **Word jumps / Shift+arrow selection / Ctrl+A/C/X/V / scroll
  wheel.** Each is a small dispatcher extension once the
  corresponding primitive lands in `edit_ops`.
- **File menu Load/Save handlers.** Currently unimplemented per
  locked v1 scope; can be wired against `editor_buffer_load_lines`
  / a path-prompt overlay.

### What the demo currently is — and isn't

The real-GL `editor_demo` build opens a GLUT window driven by the
demo's own generic dispatcher (`tools/editor_demo/input.c`) and
File menu (`tools/editor_demo/menu.c`). The display callback
builds a `UiTextPanelSnapshot` from `EditorState` (one TEXT row per
buffer line, an INPUT row at `edit_line`) and renders through
`ui_text_panel_render`. Typing inserts characters via
`edit_op_type_char`, backspace via `edit_op_backspace`, arrow keys
move the cursor within the input row, and the File menu opens to
Load / Save / Quit (Load and Save are unimplemented v1 handlers
that log; Quit calls exit). No `editor_handle_key` /
`editor_handle_special` from the REPL editor is involved.

The `repl_shim.c` ledger is **one symbol**:
- `repl_state_edit_line` — the acknowledged `state.c`
  `EditorInputView` leak. A follow-up phase moves edit-line
  ownership into `EditorState` and the stub vanishes.

The prior shim's ~85 symbols (REPL state / command_store / compile /
apply / eval / func_alias / source_scope / line predicates, plus
tutorial / color_picker / ui_* / glr_*) are gone — those stubs
existed to satisfy the REPL-flavored editor controller files
(`input.c`, `commit.c`, `clipboard.c`, `undo.c`, `reformat.c`,
`search.c`, `completion.c`, and the inline overlays), which Phase
8.7 dropped from the demo's link set entirely.

## Updated direction (2026-05-20) — generic text editor demo + shared edit_ops

Review surfaced a sharper architectural cleavage than the original
plan named. Restating the goal in the new framing:

- **`src/editor/input.c` is the *REPL editor input dispatcher*, not
  a generic editor controller.** Most of its key handlers are
  REPL-specific (`;` commit, Tab GL-autocomplete, Ctrl+R reformat,
  tutorial guards, comment toggle) and most of its dispatch reaches
  into REPL machinery (command store, compile, source scope).
- **`src/editor/{commit,clipboard,undo,reformat}.c` are similarly
  REPL-flavored controllers.** They orchestrate REPL parse →
  compile → apply, validate `CMD_VAR_DECLARE` ranges in clipboard
  ops, snapshot REPL predef vars + scratch arrays in undo, etc.
- **`src/editor/state.c`** is *mostly* generic — text buffer +
  cursor + selection storage — with one acknowledged REPL leak
  (`EditorInputView` reads `repl_state_edit_line`) that the demo
  handles via a one-line shim until a follow-up phase moves
  edit-line ownership into `EditorState`.
- **`src/editor/{search,undo,commit,clipboard,reformat,
  completion,inline_file_prompt,inline_rename,help_session}.c`**
  are REPL-flavored controllers. They reach into REPL state,
  REPL command store, or UI state directly; the demo does not
  link them in v1.

The cleaner factoring the review converged on:

| Module | Role |
|--------|------|
| `src/editor/state.c` (existing) | Data model: text buffer, cursor, selection storage. Mostly generic — has one acknowledged REPL leak (`EditorInputView` reads `repl_state_edit_line` for its `edit_line_idx` field). The demo stubs that one symbol; a follow-up phase moves edit-line ownership into `EditorState`. |
| `src/editor/edit_ops.{c,h}` (**new**) | Generic text-editing primitives extracted from `input.c`: cursor moves, character insert/delete, selection extend/clear, text-only clipboard. Strictly REPL-free, locked by `check-edit-ops-pure` (see 8.9). |
| `src/editor/input.c` (existing, relabelled) | **REPL editor input dispatcher** — REPL key bindings + REPL-specific orchestration on top of `edit_ops` primitives. Stays where it is; demo does *not* link it. |
| `tools/editor_demo/input.c` (**new**) | **Generic editor input dispatcher** — plain-text key bindings on top of the same `edit_ops` primitives. No REPL. |

`src/editor/{search,undo,commit,clipboard,reformat,completion,
inline_file_prompt,inline_rename,help_session}.c` are intentionally
**not** in this table. They are REPL-flavored controllers (see
"Editor files that aren't yet generic" below for the coupling
audit) and the demo does not link them.

This is the right answer to the question the demo was forcing.
"Chrome stays direct" was right for *truly editor-inherent* chrome
(code panel, file save/load menu, search overlay) — those modules
genuinely don't need REPL. It was wrong for the REPL-feature widgets
(tutorial, color picker for GL color literals, variable panel for
predef-vars, replay): those *are* REPL-specific, not editor-inherent,
and the demo correctly should not link them. The previous shim
stubbed their APIs because `src/editor/input.c` calls them directly;
once the demo stops linking input.c, those stubs go away with it.

### Generic editor demo — locked scope (v1, intentionally narrow)

The scope leans hard toward *less, not more*. Every feature
included below has a clear extraction path through `edit_ops`
primitives and no REPL/UI side-channels. Anything that requires
non-trivial cleanup in shared editor files (`state.c`, `search.c`,
`undo.c`) is **deferred** rather than dragged into Phase 8 — those
files have known REPL/UI leaks (see "Editor files that aren't yet
generic" below) that need their own follow-up phases before the
demo can call them generic.

What `editor_demo` v1 is:

- A plain text editor in a GLUT window.
- Renders text via `src/ui/text_panel.c` (the generic renderer from
  Phase 2). Built from a `UiTextPanelSnapshot` constructed in the
  demo's display callback from `EditorState`.
- Generic input dispatch in `tools/editor_demo/input.c` covering
  the primitives that have a clean extraction path:
  printable ASCII insertion, backspace/delete, bare Left/Right and
  Home/End within the input row, Escape to quit. Word jumps,
  Shift+arrow selection, double-click word select, drag selection,
  Ctrl+A/C/X/V, and scroll wheel are listed in "What's still open"
  below — they need additional `edit_ops` primitives or cross-line
  navigation support and are explicitly *not* in v1.
- A **menu bar with a File menu**. Load/Save items render and are
  hit-testable but their *handlers can be unimplemented* initially
  (status messages or no-ops). The point is to exercise the
  ability to *create* menu items in a generic editor — the
  semantics come later.

What `editor_demo` v1 is NOT (deferred or out of scope):

- **No undo/redo.** `src/editor/undo.c` snapshots REPL predef vars
  and scratch arrays alongside text and depends on `repl_command_store_*`;
  it is fundamentally REPL-flavored. A generic undo ring (text-only
  snapshots stored inside `EditorState`) is a *separate* follow-up
  phase, not part of Phase 8. The demo dispatcher does not bind
  Ctrl+Z / Ctrl+Y; typed changes are committed.
- **No find / search overlay.** `src/editor/search.c` reaches into
  REPL state views (`repl_state_edit_line`) and UI help state
  (`ui_state_help_mut` at search.c line 408). Not generic in its
  current form. The demo dispatcher does not bind Ctrl+F.
- **No status bar.** The current statusbar is entwined with REPL
  status sinks / `repl_set_status` / scene status banners; not
  worth refitting for the demo. Drop the chrome flag.
- **No tutorial.** REPL-feature; the demo doesn't link
  `src/widgets/tutorial.c` and doesn't need its API stubs.
- **No color picker.** Picker exists to edit `glColor3f` / `glColor4f`
  literals — REPL-feature.
- **No variable panel / slider.** Predef-var sliders — REPL-feature.
- **No replay HUD / annotations.** REPL-feature.
- **No GL grammar autocomplete.** No grammar to suggest from. The
  completion provider seam stays unregistered (the existing
  early-return path handles this cleanly).
- **No `;` commit semantics.** A text-editor "commit" just inserts
  a newline / advances to the next row. The REPL's parse → compile
  → apply pipeline never runs.

### Editor files that aren't yet generic (require follow-up phases)

The previous "Updated direction" claim that `state.c` and `search.c`
are genuinely generic was too strong. The actual coupling, as of
2026-05-20:

- **`src/editor/state.c`** — `EditorInputView` builder forward-declares
  and calls `repl_state_edit_line()` to populate the view's
  `edit_line_idx` field. The editor doesn't own its own edit-line
  cursor today; it reads it from REPL state. The demo can either
  (a) provide a one-line stub for `repl_state_edit_line` (acknowledged
  in `repl_shim.c`), or (b) a follow-up phase moves edit-line
  ownership into `EditorState` so the editor owns its own cursor.
  v1 takes path (a); cleanup is deferred.
- **`src/editor/search.c`** — Includes REPL state views,
  calls `repl_state_edit_line` throughout, and mutates
  `ui_state_help_mut` directly. Not generic. The demo does not
  link it; Ctrl+F is dropped from v1 scope.
- **`src/editor/undo.c`** — Snapshots REPL predef vars and scratch
  arrays alongside text. Not generic; demo does not link it.
- **`src/editor/{commit,clipboard,reformat,completion,
  inline_file_prompt,inline_rename,help_session}.c`** — All
  REPL-flavored controllers. Demo does not link.

Future phases (not Phase 8) can clean up state.c's edit-line leak
and decompose undo.c into a generic text-only ring + a REPL-aware
predef-vars/scratch snapshotter. Doing that work *inside* Phase 8
would bloat the phase beyond its forcing-function value.

### Why this is cleaner

1. **Honest layering.** `edit_ops` is the named, testable boundary
   for "generic text editing." `input.c` is the named boundary for
   "REPL editing." Each module has one job.
2. **The forcing function works the right way.** Anything the
   generic demo needs *must* be in `edit_ops`. If a primitive is
   missing, the demo can't compile without it — extraction
   pressure points are obvious.
3. **The shim shrinks dramatically.** Demo stops linking
   `src/editor/{input,commit,clipboard,undo,reformat,completion}.c`
   — the files that call most of the REPL surface. The remaining
   shim is whatever `state.c` / `search.c` / new `edit_ops.c`
   still call into REPL, which should be near-zero.
4. **Code reuse is real, not notional.** Both the REPL app and the
   demo call the same `edit_ops` primitives, so the library has
   two consumers exercising it.
5. **Better testability.** Unit-testing `edit_ops` primitives
   without going through the full REPL commit chain becomes
   straightforward.

Trade-off: there are now two input dispatchers to maintain
(`src/editor/input.c` for REPL key bindings, `tools/editor_demo/input.c`
for generic). Each is shorter than today's input.c because the
primitive logic moved to `edit_ops`. Net more lines of code overall
but each module is simpler. The right answer at the architectural
level.

### Effect on prior plan content

- **Phase 5 (EditorServices migrations)** is largely superseded.
  The migration of `parse_command_ctx` (commit `a5b6388`) still
  stands as a worked example for the REPL editor's own
  testability, but Phase 5's primary goal — shrinking the demo's
  shim by routing through services — is now met by *not linking
  the controller files at all*. Remaining Phase 5 migrations are
  optional REPL-editor quality improvements, not load-bearing
  for the demo.
- **Phase 6 (editor demo)** stands but the link set changes
  substantially under Phase 8: drop `src/editor/{input,commit,
  clipboard,undo,reformat,completion}.c`; add
  `src/editor/edit_ops.c`, `tools/editor_demo/input.c`, plus the
  generic-render UI modules (`text_panel.c`, `text_layout.c`,
  `text_search.c`, menu helpers as needed).
- **Phase 7b ratchet** stays useful for the REPL editor (input.c /
  commit.c shouldn't *grow* their REPL coupling), but the demo no
  longer drives the ratchet down by extraction — it drives the
  shim down by changing the link set.

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

## Phase 5 — Editor REPL Decoupling (historical — superseded by Phase 8)

> **Superseded.** This phase's central claim — that the demo's
> independence requires routing `input.c` / `commit.c` REPL calls
> through `EditorServices` — was reframed by the "Updated
> direction" decision. The demo achieves independence by *not
> linking* those controller files at all. The
> `parse_command_ctx` migration in commit `a5b6388` still stands
> as a useful worked example for the REPL editor's own
> testability, but the broader migration program below is no
> longer load-bearing for `editor_demo`. Retained here as
> historical context.

This phase is the load-bearing prerequisite for Phase 6's editor demo. It is **not** about the demo — it's a focused refactor of `src/editor/input.c` and `src/editor/commit.c` that pulls their REPL-pipeline reach behind the registered `EditorServices` table. The demo in Phase 6 is downstream proof that this phase worked.

**Deferred status:** Phases 5 and 6 are intentionally not required for the
SRP split of `ui/panels.c`. Treat the rest of this phase as a draft design
and dependency ledger, not an implementation-ready spec. Before starting
Phase 5, do a fresh source review against the then-current tree and update
the service table, shim counts, and guards below.

The starting point is **not green-field**: `src/editor/services.h` already defines an `EditorServices` table that `src/editor/commit.c` uses for compile/apply (`context`, `compile`, `apply_repl_change`, `apply_predef_ops`, `apply_scratch_ops`). Phase 5 **extends** that seam rather than introducing a parallel `EditorReplServices`.

**Scope decision (chrome stays direct).** An earlier draft of this phase
added a parallel `EditorChromeServices` table to abstract the editor's
reach into menu bar, help overlay, color picker, tutorial, and the
app-shell `glr_*` controller. That table is **not** built. Rationale:
the menu / help-overlay / picker / tutorial modules are *editor-inherent*
— any standalone editor binary needs equivalents — so the editor module
set legitimately depends on `src/ui/` and `src/widgets/` modules
directly. That's downward layering, not a violation. What's left after
removing the editor-inherent surface is a handful (~3-4) of upward
`glr_*` calls in `input.c` (camera reset, code-panel presentation
get/set, panel-drag router); abstracting four trivial calls behind a
nine-field service table is more abstraction than the problem deserves.
The demo handles them with direct symbol stubs, the same way
`repl_shim.c` already plans to stub the smaller editor files' direct
REPL reach.

### Motivation — measurement table (verified 2026-05-20)

REPL-symbol surface of each `src/editor/*.c` file (re-counted at Phase 5
entry):

| File | REPL functions called | Notes |
|------|----------------------|-------|
| `state.c` | 1 (`repl_state_edit_line`) | One-line stub. Per-file stub growth — fine. |
| `clipboard.c` | ~10 (command_store, source_scope, status) | Moderate; mutators are no-ops in the demo. |
| `undo.c` | ~10 (command_store, func_alias, eval, promote_example) | Moderate; `repl_promote_example_if_needed` is the only REPL-semantics call. |
| `input.c` | **23** (parse + compile + command_store + status) | **This phase reduces this to ~5.** Also has chrome reach (`ui_*`, `color_picker_*`, `tutorial_*`) — kept direct as editor-inherent. Residual upward `glr_*` reach (**6 sites verified 2026-05-20**, vs draft "~3-4") stays direct; the demo stubs it. Six is below the 8-symbol reopen threshold. |
| `commit.c` | **33** (compile / apply / func_alias / eval / source_scope) | **This phase reduces this to ~5.** Already routes some calls through `EditorServices` today — extension target. No chrome reach at all. |

`code_panel_document.c` is **not in this table** — it was split during Phases 1 and 3 (pure half → `src/ui/text_layout`; REPL-aware half → `src/ui/repl_code_panel.c`) and no longer exists in `src/editor/` by the time Phase 5 starts.

**Verified 2026-05-20 — `glr_*` reach in `input.c` (6 unique symbols):**
`glr_camera_controls_reset`, `glr_completion_accept_autocomplete`,
`glr_ctrl_router_reset_code_panel_drag`, `glr_ctrl_sync_ui_chrome`,
`glr_state_presentation`, `glr_state_presentation_mut`. All stay direct;
six stubs in `repl_shim.c` is comfortably under the EditorChromeServices
reopen threshold.

**Service-surface refinement note.** The draft 13-field extension
covers the structured surface (parser/store/func_alias/eval/source_scope),
but the actual REPL reach also includes ~10 pure-read helpers
(`repl_state_edit_line`, `repl_state_document_count`,
`repl_line_is_block_head`/`_label`, `repl_format_fits`,
`repl_copy_string_fits`, `repl_compiled_change_init`,
`repl_apply_can_apply_compiled_change`,
`repl_func_alias_first_free_slot`/`_lookup_slot`/`_name_is_valid`/`_get`,
`repl_func_signature`, `repl_source_scope_in_begin_block_at`,
`repl_eval_input_has_predef_vars`). Per the rule already documented in
`src/editor/services.h` ("pure reads stay direct; only mutating
operations and context construction live in the table"), these stay
direct and become direct stubs in `repl_shim.c`. Additional mutating
reach to fold into the service table during implementation:
`repl_command_store_clear`, `repl_state_edit_line_set`,
`repl_compile_delete_range`/`_empty_line`/`_toggle_comment` (likely
coarsened into a single `compile_command` callback),
`repl_compiled_change_rollback_alias`, `repl_eval_init_predef_vars`,
`repl_parse_and_normalize_strict`. Estimate: ~17-20 final service
fields, ~47-50 total unique symbols in `repl_shim.c`. Slightly above
the draft 45-symbol cap; revisit the cap during step 5 implementation.

Sum today: **~77 REPL function calls across the five remaining files**. The
old "~12-15" shim target was aspirational and is no longer the operative
budget; the current draft ledger later in this phase estimates a smaller
surface (~40 unique symbols once chrome services are dropped) and still
needs a fresh review before Phase 5 or 6 implementation. The three smaller
files can be shimmed directly, but the two largest carry the real coupling
and have to come down before any demo work can land coherently.

### Required Review Before Implementation

Run this review immediately before implementing Phase 5 or Phase 6:

- Rebuild the REPL-symbol inventory for `src/editor/input.c` and
  `src/editor/commit.c`. The draft `EditorServices` table below is known
  incomplete today: it does not yet cover the document-state accessors
  (`repl_state_edit_line`, `repl_state_edit_line_set`,
  `repl_state_document_count`, `repl_state_document_cmds_mut`) that dominate
  both files. Decide explicitly whether those become service callbacks,
  move behind existing compile/apply helpers, or remain direct shim symbols.
- Audit the remaining nontrivial REPL calls in `input.c` / `commit.c` that
  are not represented in the draft table: delete-range compile,
  command-store clear, predef-var reset, comment-toggle compile,
  strict parse/normalize, apply preflight, compiled-change rollback, and
  any legacy `try_commit_*` compile helpers still called directly. Each one
  needs an owned path: service callback, relocated compile/apply helper, or
  counted direct shim.
- Re-audit the upward `glr_*` reach in `input.c`. Current sites are
  expected to be camera reset, code-panel presentation get/set, and
  the panel-drag router — roughly 3-4 unique symbols. Confirm the count
  is small enough to justify direct stubs in the demo; if it has grown
  past ~8 symbols since this plan was written, reopen the
  `EditorChromeServices` decision rather than stubbing each one.
- Confirm the editor's `ui_*` / `color_picker_*` / `tutorial_*` reach
  is still legitimately downward (editor depending on `src/ui/` and
  `src/widgets/`). If any of those modules grew an upward dependency
  on `glr_*` or REPL state that the editor now transitively pulls in,
  treat the editor's call as a layering bug to fix at the callee,
  not as another shim entry.
- Inventory non-`repl_*` direct calls in `input.c`, `commit.c`,
  `clipboard.c`, `undo.c`, and `state.c` (tutorial, color picker,
  menu bar, help overlay, ui status sink, etc.). These stay direct.
  The demo links the actual modules where they no-op cleanly in the
  demo's default state; where a module transitively pulls in REPL
  (likely candidates: tutorial, replay annotations), the demo
  provides a minimal alternate compilation unit instead. Decide per
  module during implementation.
- Recompute the Phase 6 caps from measured unique symbols after the service
  split lands. The current `~36 REPL-side + ~4 glr_* stubs` estimate is a
  draft, and is likely low until the missing document-state and
  any newly-uncovered direct surfaces above are accounted for.

### What this phase produces

The following tables are a starting shape for the deferred work, not a
complete checklist. The implementation review above must update them before
code lands.

- **Extended `EditorServices` table** (`src/editor/services.h`). The existing
  struct already covers compile/apply for `commit.c`; this phase adds the
  remaining REPL-pipeline surface `input.c` and `commit.c` currently reach
  by name. Concrete extension:

  ```c
  typedef struct EditorServices_s {
      /* --- existing fields (compile/apply) --- */
      ReplCompileContext (*context)(void *user);
      ReplCompileResult  (*compile)(...);
      int                (*apply_repl_change)(const ReplCompiledChange *, void *user);
      void               (*apply_predef_ops)(const ReplCompiledChange *, void *user);
      void               (*apply_scratch_ops)(const ReplCompiledChange *, void *user);

      /* --- new in Phase 5: input.c + commit.c REPL surface --- */
      /* Status sink. Already has a sink pattern (repl_set_status_sink);
       * folding it into EditorServices removes the parallel install path. */
      void               (*set_status)(const char *msg, void *user);

      /* Dirty-state notifier — both routes call this after a successful
       * commit. */
      void               (*mark_normals_dirty)(void *user);

      /* Parser front-door used by both routes' open-coded parser path. */
      int                (*parse_command_ctx)(const char *input,
                                              ReplParsedLine *out,
                                              const ReplParseContext *ctx,
                                              void *user);

      /* Command-store handle + mutators. Today the editor builds a
       * `ReplCommandStore` via `repl_command_store_live()` then calls
       * `_insert_one` / `_replace_one`. Coarsen to two callbacks so the
       * shim doesn't have to model the store struct: */
      int                (*command_store_insert_one)(int pos,
                                                     const GLCmd *cmd,
                                                     unsigned flags,
                                                     void *user);
      int                (*command_store_replace_one)(int pos,
                                                      const GLCmd *cmd,
                                                      void *user);

      /* Func-alias bookkeeping used by commit.c's func-def handler. Five
       * production symbols collapse to two callbacks: */
      int                (*func_alias_lookup_or_alloc)(const char *ident,
                                                       int *out_slot,
                                                       char *err, int err_sz,
                                                       void *user);
      const char *       (*func_alias_get)(int slot, void *user);

      /* Eval helpers commit.c calls during expression normalization. Pure
       * today, but routing keeps the shape uniform and gives the demo a
       * place to refuse expression evaluation cleanly. */
      int                (*eval_parse_exprs)(const char *text,
                                             float *args, int max_args,
                                             const ExprVar *vars, int nvars,
                                             int *out_n, void *user);
      int                (*eval_validate_expression_idents)(const char *expr,
                                                            const ExprVar *vars, int nvars,
                                                            char *err, int err_sz,
                                                            void *user);

      /* Source-scope queries used by commit.c's indent / block-end logic.
       * Four production symbols, collapsed under the same uniformity
       * rationale: */
      int                (*source_scope_block_depth_at)(int pos, void *user);
      int                (*source_scope_find_block_end)(int pos, void *user);
      void               (*source_scope_cmd_indent)(int pos, char *out, int out_sz, void *user);
      CmdType            (*source_scope_nearest_open_block_at)(int pos, void *user);

      void *user;
  } EditorServices;
  ```

  That's the existing 5 fields plus **~12 new fields** for input.c +
  commit.c's reachable REPL surface. `editor_services_default()` populates
  every field with the corresponding `repl_*` symbol; production code is
  unchanged. The shim populates each field with a no-op or fake.

  Two of the new fields (`command_store_*` and `func_alias_lookup_or_alloc`)
  are deliberate coarsenings of the raw surface: the editor currently makes
  ~5 `repl_command_store_*` calls and ~5 `repl_func_alias_*` calls, but the
  flows behind those calls reduce to two and one callback shapes
  respectively. Coarsening here keeps the table from blowing past 20 fields
  and gives the shim a much smaller surface to populate.
- **No `EditorChromeServices` table.** The earlier draft added a parallel
  nine-field seam over menu bar, help overlay, color picker, and the
  app-shell `glr_*` controller. That table is **not** built; see the
  "Scope decision" paragraph at the top of this phase. UI chrome
  (`ui_menu_bar_*`, `ui_help_overlay_*`, `color_picker_*`, `tutorial_*`)
  is editor-inherent and stays as direct downward calls into `src/ui/`
  and `src/widgets/`. The residual upward reach (camera controls reset,
  code-panel presentation get/set, panel-drag router — roughly 3-4 sites
  in `input.c`) stays as direct `glr_*` calls; the demo provides
  direct-symbol stubs for them in `repl_shim.c` alongside the smaller
  files' REPL stubs.
- Controller registers production `EditorServices` bindings at app init via the existing `editor_services_default()` path. `input.c` / `commit.c` switch their REPL calls to the registered table. Chrome calls stay direct.
- The pattern matches existing seams: `repl_set_status_sink`, `repl_install_input_reset_sink`, the existing `EditorServices` — same dispatch shape, same lifecycle.

### Direct stub surface (three smaller editor files)

Phase 5 does not touch `state.c`, `clipboard.c`, or `undo.c` — they're
already inside the per-file stub-growth budget. They continue to call
`repl_*` symbols by name. The Phase 6 shim provides these as direct
symbol definitions in `repl_shim.c`, separate from the service-table
fields:

| Symbol | Caller | Shim semantics |
|--------|--------|----------------|
| `repl_state_edit_line` | state.c, clipboard.c, undo.c | return shim-local edit_line |
| `repl_state_edit_line_set` | clipboard.c | write shim-local edit_line |
| `repl_state_document_count` | clipboard.c, undo.c | return shim cmd count |
| `repl_state_document_cmds_mut` | undo.c | return shim cmd array |
| `repl_command_store_live` | clipboard.c, undo.c | return a `ReplCommandStore` over shim cmds |
| `repl_command_store_can_insert` | clipboard.c | true unless capacity |
| `repl_command_store_normalize_range` | clipboard.c | range-clamp helper, pure |
| `repl_command_store_load` | undo.c | bulk replace shim cmds |
| `repl_copy_string_fits` | clipboard.c, undo.c | direct call to live impl (pure) |
| `repl_range_contains_var_decl` | clipboard.c | return 0 (no decls in demo) |
| `repl_source_scope_block_extent` | clipboard.c | return single-row extent |
| `repl_mark_normals_dirty` | clipboard.c, undo.c | no-op |
| `repl_set_status` | clipboard.c, undo.c | forward to `ui_state_status_set` |
| `repl_eval_copy_scratch_arrays` | undo.c | memcpy local arrays |
| `repl_eval_restore_scratch_arrays` | undo.c | memcpy local arrays |
| `repl_func_alias_clear_all` | undo.c | no-op |
| `repl_func_alias_get` | undo.c | return "" |
| `repl_func_alias_set` | undo.c | no-op, return 1 |
| `repl_promote_example_if_needed` | undo.c | no-op |

That's **19 unique direct-stub symbols** for the three smaller files.

### Draft Total Shim Surface

Draft totals before the required implementation review:

| Surface | Count | Notes |
|---------|-------|-------|
| `EditorServices` fields (incl. 5 existing) | ~17 | input.c + commit.c REPL pipeline |
| Direct REPL stubs in `repl_shim.c` | ~19 | state.c + clipboard.c + undo.c |
| Direct `glr_*` stubs in `repl_shim.c` | ~3-4 | input.c upward reach |
| **Total unique shim symbols** | **~40** | function pointers + direct symbols |

UI chrome modules (`src/ui/menu_bar`, `src/ui/tabbed_overlay`,
`src/widgets/color_picker_state`, `src/widgets/tutorial`) link directly
and are not counted here.

This is meaningfully smaller than the earlier draft that included a
parallel `EditorChromeServices` table (~45 symbols), but the count is
still incomplete until the required review accounts for the missing
document-state surface and confirms the `glr_*` site count. The tripwire
in Phase 6 must reflect the final measured reality.

### Target reductions

- `input.c`: 23 → ~5 REPL function calls (draft target; revalidate in the required review). Chrome reach (`ui_*`, `color_picker_*`, `tutorial_*`) remains direct as editor-inherent; the residual upward `glr_*` reach (~3-4 sites) also stays direct and is stubbed in the demo shim.
- `commit.c`: 33 → ~5 REPL function calls (draft target; revalidate in the required review).
- The three smaller editor files stay where they are — they're already within budget.

### Validation

- Existing `make test`, `make test-stubs`, `make check-state-ownership` pass with no changes to test fixtures.
- Test fixtures `testdata/repl_examples_ui/*.golden.txt` byte-equal after the refactor.
- Greppable guard:
  - `scripts/check-editor-repl-surface.sh` counts `repl_*(` calls in `src/editor/input.c` and `src/editor/commit.c`; fails if either exceeds a ratcheted threshold (start at 8).
  - Optional companion: a narrow `scripts/check-editor-glr-surface.sh` counting only `glr_*(` call sites in `src/editor/input.c`, ratcheted from the measured floor (likely 3-4). Lighter than the dropped `EditorChromeServices` guard since it just enforces "don't grow the upward reach," not "abstract it behind a table." Skip if the count is already at floor and the team trusts the `repl_shim.c` cap to catch regressions.
- `EditorServices` has a "not yet installed" assert path so an uninstalled binary fails loudly rather than null-deref.

### Code touched

- `src/editor/input.{c,h}` — switch direct REPL calls to the extended `EditorServices` table. Chrome / UI reach (`ui_*`, `color_picker_*`, `tutorial_*`) stays direct (editor-inherent). The residual `glr_*` calls stay direct too; the demo provides stubs.
- `src/editor/commit.{c,h}` — extend the existing `EditorServices` consumption to cover the rest of the REPL-pipeline surface.
- `src/editor/services.{c,h}` — add new fields to the `EditorServices` struct; extend `editor_services_default()` to populate them.
- `src/app/glr_ctrl.c` — production `EditorServices` already comes through `editor_services_default()`; only new field bindings need wiring.
- new `scripts/check-editor-repl-surface.sh` — REPL surface-count regression gate. Optionally `scripts/check-editor-glr-surface.sh` for the narrow upward-reach ratchet.
- `Makefile` — wire the check target(s).

## Phase 6 — Add Editor Demo Host (partial — link set superseded by Phase 8)

> **Partially superseded.** The `tools/editor_demo/` skeleton and
> Makefile target (commit `b6250bc`) still stand. The link-set
> and shim-content portions described below assumed the
> Phase 5 EditorServices migration; Phase 8 replaces that with
> a generic-dispatcher pattern (drop the REPL-flavored controller
> files entirely; add `tools/editor_demo/input.c` + `edit_ops`).
> Treat the "fake EditorServices" framing here as historical.

This phase only lands after Phase 5. By the time Phase 6 starts, `input.c` / `commit.c` route their REPL reach through the extended `EditorServices` table, so the demo's job is to populate that table with fake/no-op bindings — not to fight against direct REPL calls. UI chrome (menu bar, help overlay, color picker, tutorial) is editor-inherent: the demo links those modules directly. The handful of upward `glr_*` calls become direct symbol stubs in `repl_shim.c`.

Because Phase 5 is deferred, Phase 6 is deferred too. Before implementing
the demo, rerun the Phase 5 review gate, update the shim ledger with the
actual service fields and direct symbols, and only then set the
`repl_shim.c` tripwire.

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
  - No-op source-scope, replay, variable, and export functions in the shim. Tutorial, color picker, help overlay, and menu modules link directly — they no-op naturally when their state is inactive in the demo. Where a module transitively pulls in REPL (likely tutorial or replay annotations), substitute a minimal alternate compilation unit; the choice is per-module and is part of the Phase 5 review.
  - Direct `glr_*` stubs in the shim: roughly `glr_camera_controls_reset`, `glr_state_presentation_*` (get/set), and `glr_ctrl_router_reset_code_panel_drag` (3-4 functions). These are simple state-touch operations on app-shell state the demo doesn't have; stubs return defaults or no-op.
  - Registering an `EditorCompletionProvider` is **optional**, not required for safety: `editor_completion_update`, `editor_completion_update_selected_preview`, and `editor_completion_clear` in `src/editor/completion.c` all early-return when `g_provider == NULL`. The demo skips registration entirely — there's no grammar to suggest from, so ghost/hint stay empty and the Tab key path no-ops cleanly.
  - Status messages forward to `ui_state_status_set`.
- Shim-size tripwire (regression gate, **not** entry budget): count
  **unique shim functions / exported symbols**, not raw call sites. The
  measured surface after Phase 5 lands (see "Draft Total Shim Surface" table in
  Phase 5) is roughly **~40 unique symbols in `repl_shim.c`**: ~17
  `EditorServices` field bindings + ~19 direct REPL stubs + ~3-4 direct
  `glr_*` stubs.

  Set the tripwire at the measured cap + small headroom (e.g. cap
  `repl_shim.c` at 45) so future seam regression — a new direct-name
  call leaking back in — actually trips the gate. The earlier "~12-15
  REPL / ~5-7 chrome" figures were aspirational and don't match the
  enumerated surface; using them as gates would either be permanently
  red or force fictitious work to make them green. If a later
  decoupling pass (the long-term direction in Assumptions below)
  shrinks state.c / clipboard.c / undo.c's REPL reach, ratchet the cap
  down then.
- Add `make editor_demo`.
  - `USE_GL_STUBS=1` verifies compile/link only.
  - Real GL build opens the editor demo window.
- Code touched: `tools/editor_demo/*`, `Makefile`.

## Phase 7 — Guards And Documentation

Phase 7 splits into two halves by dependency:

- **Phase 7a — lock in the UI split (Phases 1-4).** Lands regardless of whether Phase 5/6 ever land. This is the load-bearing tail of the work already merged.
- **Phase 7b — gates and polish for the demo (Phases 5-6).** Only meaningful after Phases 5 and 6 land; deferred when those phases are deferred.

### Phase 7a — Lock in the UI split (done)

1. Add `scripts/check-ui-text-panel-pure.sh` and a `check-ui-text-panel-pure` Makefile target. Wire into `make check`.
   - Fail if `src/ui/text_panel.*` includes `repl/`, `editor/`, `src/repl/`, or `src/editor/`.
   - Fail if it references `GLCmd`, `CmdType`, or `CMD_`.
   - This is the single most important item in this phase — without it the Phase 1-4 invariants can quietly regress in the next refactor.
2. `tests/test_ui_text_panel.c` — **already landed** during Phase 3 findings-fix and extended in Phase 4 (virtual-row routing regression). 25 tests under `USE_GL_STUBS=1`. No further work required.
3. Update `MODULES.md`:
   - Replace the `editor_code_panel_document` row (around line 325) with entries for `ui_text_panel` (generic text rendering/hit-test) and `ui_repl_code_panel` (REPL adapter over the generic panel).
   - Fix the rename table near line 883 (`repl_code_panel_document → editor_code_panel_document` references a file that no longer exists).
   - Update any diagram nodes referencing the deleted `src/editor/code_panel_document.c`.
4. Tighten `src/ui/text_panel.h` field docs (from Phase 3 review nits):
   - Document `background_active` and `left_marker_active` next to their `_color` siblings (currently the `_active` gate fields aren't in the field-doc block, only the `_color` ones).
   - Add a "when adapters set this" sentence to `UiTextPanelRightAction.emphasized` — the current inline comment explains *what* is drawn but not *when* an adapter would set it.
- Code touched: `Makefile`, `scripts/check-ui-text-panel-pure.sh`, `MODULES.md`, `src/ui/text_panel.h`, optionally `feature/editor-demo.md` (move this section to `done/` once 7a lands).

### Phase 7b — Demo / decoupling polish (partial — see Phase 8)

> **Partial.** The `check-editor-repl-surface` ratchet (commit
> `01c60d3`) still stands and continues to guard the REPL
> editor's own `input.c` / `commit.c` surface. The
> `MODULES.md` editor_demo entry (commit `d5a0407`) describes
> the old link set and needs the Phase 8 update. The demo
> shim-shrinkage targets and `check-editor-glr-surface`
> hypothetical are superseded.


These items are gated on Phase 5 (surface guards) and Phase 6 (editor demo binary). They land alongside or immediately after those phases:

1. Wire `scripts/check-editor-repl-surface.sh` (added in Phase 5) into `make check` so the `input.c` / `commit.c` REPL-call surface stays below threshold over time. Ratchet the threshold down as further decoupling lands. If a narrow `scripts/check-editor-glr-surface.sh` was added alongside in Phase 5, wire it too. **Depends on Phase 5.**
2. Add root-level `editor_demo` symlink alongside `sample` / `repl_demo` so `./editor_demo` runs the binary from the repo root. **Depends on Phase 6.**
3. Extend the `MODULES.md` update from 7a to also note that `tools/editor_demo/repl_shim.c` is a dependency ledger against `EditorServices` (plus a handful of direct `glr_*` stubs for the residual upward reach and direct REPL stubs for the smaller editor files), not production architecture. **Depends on Phase 6.**
- Code touched (when unblocked): `Makefile`, `MODULES.md`, docs.

## Phase 8 — Demo refit: generic text editor + shared `edit_ops` (in progress)

Implements the "Updated direction" decision: extract generic
text-editing primitives into a shared library and rebuild the demo
as a real generic text editor, dropping the REPL-flavored controller
files from the demo's link set.

This phase is incremental — each step is independently committable
and leaves the tree green.

### 8.1 — Lock scope and update MODULES.md (this commit)

- Plan update (this section) records the direction.
- Subsequent MODULES.md edits land alongside the implementation steps.

### 8.2 — Inventory primitives to extract

Walk every key handler in `src/editor/input.c` and identify the
generic primitive operations underneath. Scope is limited to
primitives whose REPL-free implementation is obvious; search /
undo / commit primitives are *out of scope* for Phase 8 (see
"What `editor_demo` v1 is NOT" above).

In-scope primitive groups:

- Cursor: move-left, move-right, move-up, move-down,
  word-left, word-right, line-home, line-end.
- Edit: insert-char, delete-left, delete-right, delete-word-left,
  delete-word-right.
- Selection: extend-selection-by-cursor-move, select-word-at-cursor,
  select-line, clear-selection.
- Clipboard (text-only): copy-selected-text, cut-selected-text,
  paste-text-at-cursor.

Record the list in this file or a sidecar `plans/active/edit_ops-inventory.md`.
Each primitive must be:
- Pure with respect to REPL state (no `repl_*` calls).
- A reusable unit that both `src/editor/input.c` and the demo's
  dispatcher call.
- Testable in isolation.

### 8.3 — Extract `src/editor/edit_ops.{c,h}` incrementally

For each primitive identified in 8.2:

- Add the function to `edit_ops.{c,h}`.
- Migrate the corresponding inline logic in `src/editor/input.c` to
  call the new primitive (in-place — same behavior, no demo
  dependency yet).
- Add a focused unit test in `tests/test_edit_ops.c` (or similar)
  that exercises the primitive against `EditorState` directly.
- Verify `make test-stubs` and `make check-state-ownership` stay
  green.

Order: extract the primitives the generic dispatcher will need
*first* so 8.4 can land soonest. Cursor / character-edit / selection
come before clipboard / search.

### 8.4 — Build `tools/editor_demo/input.c` (generic dispatcher)

- New file. Hooks `glutKeyboardFunc` / `glutSpecialFunc` /
  `glutMouseFunc` / `glutMotionFunc` / `glutMouseWheelFunc` /
  `glutPassiveMotionFunc`.
- For each key: call the corresponding `edit_ops` primitive against
  `EditorState`. No `repl_*` calls; no `tutorial_*`; no
  `color_picker_*`.
- Cursor blink timer if needed.

### 8.5 — Wire the UI code panel into the demo's display callback

- Build a `UiTextPanelSnapshot` in `demo_display_func` from
  `EditorState` (`editor_state_buffer_view()`, current cursor,
  selection, scroll, search hit).
- Call `ui_text_panel_render`.
- Statusbar chrome flag stays *off* (`UI_TEXT_PANEL_CHROME_STATUSBAR`
  not set) — explicitly out of scope.
- Link `src/ui/{text_panel,text_layout,text_search}.c` into the
  demo's source list. These are REPL-free; should not introduce
  new shim stubs.

### 8.6 — Add the demo's File menu

Two acceptable implementations:

1. **Reuse `src/ui/menu_bar.c`** if it's REPL-free (audit needed).
   Build the menu model in the demo and pass it through.
2. **Demo-local minimal menu** in `tools/editor_demo/menu.c`:
   click-to-open, render a vertical list of items, hit-test,
   call-the-callback-on-click. Bounded ~100 lines.

Menu items:
- File → Load (handler: status message "load not implemented yet"
  or a no-op; can be wired to `editor_buffer_load_lines` later
  against a hardcoded path).
- File → Save (handler: same — print path or no-op).
- File → Quit (calls `exit(0)`).

The scope explicitly allows these handlers to be unimplemented at
first; the proof is that menu items can be *created* and
hit-tested in a generic editor without REPL.

### 8.7 — Drop the demo's link of REPL-flavored controller files

Update `EDITOR_DEMO_DEP_SRCS` in the Makefile:

**Remove:**
- `src/editor/clipboard.c`, `commit.c`, `completion.c`,
  `help_session.c`, `inline_file_prompt.c`, `inline_rename.c`,
  `input.c`, `reformat.c`, `search.c`, `undo.c`.

**Keep:**
- `src/editor/state.c` (with one stub for `repl_state_edit_line`
  in the shim — see "Editor files that aren't yet generic" above).
- `src/editor/edit_ops.c` (new).

**Add:**
- `tools/editor_demo/input.c` (new).
- `tools/editor_demo/menu.c` (new, if option 2 in 8.6).
- `src/ui/text_panel.c`, `text_layout.c`, `text_search.c`.
- `src/ui/menu_bar.c` (if option 1 in 8.6).

### 8.8 — Shrink `tools/editor_demo/repl_shim.c`

Remove every stub that was only needed because `src/editor/input.c`
or its siblings called the symbol. Expected casualties:

- All `tutorial_*` stubs (10) — `src/widgets/tutorial.c` no longer
  referenced.
- `color_picker_close` stub (1) — picker no longer called.
- Most `repl_command_store_*` stubs (5) — only state.c's pure-read
  callers may remain.
- Most `repl_compile_*` stubs (6) — compile path no longer
  referenced from the demo's link set.
- All `repl_apply_*` stubs (4) — apply path gone.
- All `repl_eval_parse_*` / `repl_eval_validate_*` /
  `repl_func_alias_*` / `repl_source_scope_*` / editor-internal
  helper stubs — controller-side callers gone.
- All `ui_state_*` / `ui_layout_*` / `ui_menu_bar_close` /
  `glr_completion_accept_autocomplete` /
  `glr_state_presentation*` / `glr_ctrl_*` / `glr_camera_*`
  stubs that were only there because `input.c` / `commit.c`
  called them.

Likely-remaining shim surface (best-guess pre-implementation):
- `repl_state_edit_line` — the one acknowledged leak from `state.c`'s
  `EditorInputView` builder. A future phase moves edit-line
  ownership into `EditorState` and the stub goes away.
- A handful of `ui_*` symbols if the menu / code-panel rendering
  paths touch global UI state. Could be near zero.

Re-measure post-8.7 and update the shim accordingly. Target: well
under 20 unique symbols. If the post-8.7 count is still large,
that's a finding worth recording — likely more REPL leaks hiding
in `state.c` than just edit-line.

### 8.9 — Update the surface ratchet and add `check-edit-ops-pure`

`scripts/baselines/editor-repl-surface.txt` continues to track
`src/editor/input.c` / `commit.c` for the REPL editor's own
ratchet. It does *not* drop those baselines (the REPL editor still
calls REPL by design); the demo's leverage point is now the
*Makefile link set*, not the in-file ratchet.

**Add `scripts/check-edit-ops-pure.sh`** (modeled on
`check-ui-text-panel-pure.sh`) that fails if
`src/editor/edit_ops.{c,h}` includes any `repl/` header or
references any `repl_*` symbol. Wire it into `make check` and into
the `check-state-ownership` block. This guard is **required**, not
optional — without it the whole edit_ops boundary can regress
silently in the next refactor, defeating the forcing function.

### 8.10 — Update MODULES.md

- Add a row for `src/editor/edit_ops` — "generic text-editing
  primitives (cursor moves, char edit, selection, text clipboard,
  search input). Library shared by `src/editor/input.c` (REPL
  dispatcher) and `tools/editor_demo/input.c` (generic dispatcher)."
- Relabel `src/editor/input.c` row to "REPL editor input
  dispatcher" (or similar) — explicit about its REPL flavor.
- Update the `editor_demo` standalone-demo entry to describe the
  new link set and the generic-dispatcher pattern.

### Verification for Phase 8

- `make sample USE_GL_STUBS=1` — REPL editor builds clean.
- `make editor_demo USE_GL_STUBS=1` — generic editor builds clean
  with the shrunk shim.
- `make editor_demo` — opens a window with rendered text and a
  working File menu (handlers may no-op).
- `make test-stubs` — full regression green (existing tests pass;
  `test_editor_input_selection` already exercises
  `edit_op_consume_input_selection` against `EditorState`. A focused
  `test_edit_ops` for the `buffer_insert_char_at_cursor` /
  `buffer_delete_left_of_cursor` / `type_char` / `backspace`
  primitives is deferred — file as a follow-up if a regression
  motivates it).
- `make check-state-ownership` — clean, including the required
  `check-edit-ops-pure` guard.
- `tools/editor_demo/repl_shim.c` — substantially smaller; the
  shim file itself acts as the new ledger of "what generic-editor
  code still needs to call into REPL," which should be near-zero.

## Test Plan

- Build/check for the near-term UI split (Phases 0-4 plus applicable Phase 7 guards):
  - `make sample USE_GL_STUBS=1`
  - `make repl_demo USE_GL_STUBS=1`
  - `make scene_demo USE_GL_STUBS=1`
  - `make test-stubs` (includes new `test_ui_text_panel`)
  - `make test_ui_text_panel USE_GL_STUBS=1` (focused unit run)
  - `make check-state-ownership`
  - `make check-ui-text-panel-pure`
- Manual full-app smoke:
  - code panel renders header/footer, command rows, colors, search, active input, replay annotations, tutorial fade, color swatches, statusbar, and hit-test routing.
- Additional checks when deferred Phases 5-6 land:
  - Rerun the Phase 5 "Required Review Before Implementation" checklist and update this plan with measured surfaces.
  - `make editor_demo USE_GL_STUBS=1`
  - `make check-editor-repl-surface` (input.c / commit.c REPL surface gate)
  - Optionally `make check-editor-glr-surface` if the narrow upward-reach ratchet was added in Phase 5.
  - Manual editor-demo smoke:
    - type text, commit lines, navigate, edit existing lines, delete, search, select/copy/paste input text, scroll, resize panel.

## Assumptions (historical — superseded by Phase 8)

> **Superseded.** The shim-as-EditorServices-ledger framing
> below predates the Phase 8 pivot. The current shim is a
> ledger of *what's left after we stop linking REPL-flavored
> editor controller files*, which the Phase 8 section
> documents directly. Retained for context.


- `editor_demo` is a plain text editor proof, not a GL language editor.
- `src/ui/panels.h` remains the stable public surface for the full app.
- The fake REPL shim is intentionally demo-local and should not migrate into production code.
- After Phase 5 lands, the shim is a **dependency ledger against the
  extended `EditorServices` table, plus a small set of direct stubs** —
  REPL stubs for the three smaller editor files (`state.c`, `clipboard.c`,
  `undo.c`) that keep calling REPL helpers by name, and a handful of
  `glr_*` stubs for the residual upward reach in `input.c`. UI chrome
  (menu bar, help overlay, color picker, tutorial) is treated as
  editor-inherent — the demo links those modules directly rather than
  abstracting them behind a second service table. The current draft
  surface (Phase 5 "Draft Total Shim Surface" table) is ~40 unique
  symbols, but the required review is expected to revise that as missing
  document-state surfaces are accounted for. The Phase 6 tripwire should
  be set at the final measured cap + small headroom — a regression gate,
  not a license for unbounded growth.
- Further cleanup of `src/editor/input.c`, `src/editor/clipboard.c`, and `src/editor/undo.c` into generic document services is the long-term direction. Phase 5's seam extraction is the first concrete step; the demo in Phase 6 is the scaffolding that makes the remaining coupling visible and shrinkable.

## Landing Strategy (historical — superseded by Phase 8 sub-steps)

> **Superseded.** Phase 8 sub-steps 8.1-8.10 are the current
> landing plan. The strategy below predates the pivot and
> describes a sequential Phase 5 → 6 → 7b march that no longer
> matches reality. Retained for context.


- **Phases 0-2** are the load-bearing UI split: text-panel module exists, generic rendering + hit-mapping live there, full app still works through the unchanged `ui_panels_*` surface. This is the first natural pause point — the cleanup is real even without the rest.
- **Phase 3** is mechanical once Phase 2 lands; **Phase 4** is hit-routing cleanup. Together these complete the SRP split for `ui/panels.c`.
- **Phase 5** is deferred editor-side decoupling, **scoped to the REPL pipeline only**. UI chrome (menu, help, picker, tutorial) is editor-inherent — the editor depends on `src/ui/` and `src/widgets/` modules directly and that's not a layering violation. The residual upward `glr_*` reach (~3-4 sites) is handled as direct stubs in the demo rather than a second service table, since the cost of abstracting a handful of trivial calls outweighs the maintenance burden of a parallel seam. Before implementation, run the required review in that phase and update the service table, shim ledger, and guard against the current source tree. Useful on its own once it lands: the service table should make the existing test harnesses easier to drive in isolation, but it is not required for the `ui/panels.c` SRP split.
- **Phase 6** (demo) is deferred and lands only after Phase 5's measured surface reductions land. Its shim and tripwire must be based on the final reviewed ledger, not the current draft counts.
- **Phase 7** splits along the same boundary: **Phase 7a** (text-panel purity guard, MODULES.md docs, header doc cleanups) lands now as the tail of the UI split — it locks in what Phases 1-4 just achieved. **Phase 7b** (editor REPL-surface guard, `editor_demo` symlink, shim documentation) is gated on the deferred Phase 5 and Phase 6 and lands with them.
