# Module Naming Convention Cleanup

## Status - DONE (2026-05-23 audit)

All four phases shipped on `main`; landed commits (run `git log --oneline
<sha>` to inspect):

- Phase 1 - `2325ce2` ("naming(phase 1): rename stale cross-module type prefixes")
- Phase 2 - `5fbca20` ("naming(phase 2): prefix orphan functions; collapse feed_line shim")
- Phase 3 - `87b1da4` ("naming(phase 3): Scene-prefix the scene theme enums")
- Phase 4 - `4c92081` ("naming(phase 4): add check-module-prefixes guard + document convention")

Verified at audit time: `scripts/check-module-prefixes.sh` exists and
`make check-module-prefixes` prints `module-prefixes OK`. The renamed
`SceneGridTheme` / `SceneAxesTheme` enums are live in `src/scene/`.

(Original commit shas the doc tracked - `2bd12af`/`e03dbc5`/`13a3cc6` -
are the worktree-local commits; the on-main equivalents are listed
above.)

## Implementation Progress

Implemented on branch `module-naming-cleanup` (off `main`), one commit
per phase, full gate (`make sample`/`test`/`check-state-ownership`/
`check-c99`/`test-stubs`) green after each.

- **Phase 1 - DONE** (`2bd12af`). All cross-module type/enum/macro
  renames. Also fixed guard scripts that hardcoded old type names
  (`check-color-picker-ui-isolation`, `check-repl-no-direct-editor`,
  `check-no-test-default-output`) + baseline-comment prose + docs.
- **Phase 2 - DONE** (`e03dbc5`). Orphan functions prefixed;
  `feed_line`/`navigate_to_line` collapsed into single
  `int editor_feed_line` / `void editor_navigate_to_line` with
  canonical decls moved to `editor/input.h` and removed from
  `repl/core.h`. 16 test TUs gained `#include "editor/input.h"`
  (no `src/repl/` code ever called it - all refs were comments).
  Guard scripts/Makefile prose updated.
- **Phase 3 - DONE** (`13a3cc6`). Scene theme enums `Scene`-prefixed.
- **Phase 4 - DONE**. Added `scripts/check-module-prefixes.sh`
  (removed-name denylist) wired into the `check-state-ownership`
  aggregate + `.PHONY` + leaf target; documented the convention and
  Sanctioned Exceptions in MODULES.md. The guard caught one residual
  stale comment (`REPL_MENU_BAR_PIN_*` in `src/ui/menu_bar.h`), which
  was fixed - evidence the gate has teeth.

Net: no behavior change; verified `make test` 5468/5468 at every
phase. AGENTS.md is a symlink to CLAUDE.md (edited once via CLAUDE.md).

## Context

Successive refactors moved code between modules without renaming its symbols.
Many public functions/types now carry a prefix that no longer matches the
directory that owns them - overwhelmingly stale `Repl*` / `REPL_` on symbols
that now live in `src/editor/`, `src/ui/`, `src/app/`, and `src/widgets/`.
The collisions are actively confusing because `Repl*` is *also* the correct,
live prefix for `src/repl/`, so a reader can't tell a genuine REPL type from a
leftover. Goal: every exported symbol's prefix follows the directory that owns
it, with a small set of *documented* intentional exceptions so the convention
is enforceable and reviewable.

## Canonical Rule (prefix follows owning directory)

| Directory | Function prefix | Type prefix |
|-----------|-----------------|-------------|
| `src/repl/` | `repl_` | `Repl` |
| `src/editor/` | `editor_` | `Editor` |
| `src/app/` | `glr_` | `Glr` / `GLR_` |
| `src/scene/` | `scene_` | `Scene` |
| `src/ui/` | `ui_` | `Ui` / `UI_` |
| `src/widgets/replay*` | `replay_` | `Replay` |
| `src/widgets/color_picker*` | `color_picker_` | `ColorPicker` |
| `src/widgets/variable_panel*` | `variable_panel_` | `VariablePanel` |
| `src/widgets/tutorial*` | `tutorial_` | `Tutorial` |
| root neutral utils | `audio_` `prof_` `fmt_` `source_document_` | `Fmt` `Source` |

The survey confirmed `src/repl/` and the widget modules (color_picker,
tutorial, variable_panel) are already clean. Work is concentrated in
editor / ui / app / scene-themes / replay.

## Sanctioned Exceptions (leave as-is; record in MODULES.md)

These are *intentional* and must not be re-flagged by future audits:

- **Legacy GL/eval domain types** (cross-domain, deliberately un-prefixed):
  `GLCmd`, `CmdType`, `ExprVar`, `ExprCtx`, `TessVertex`,
  `FlatCmdLocalVars`, `FlatProgramView`, `CmdSyntaxCategory`, and the
  `cmd_type_name` thin alias - all in `src/repl/`.
- **Root neutral helpers**: `cmd_format.h` `Fmt*`/`fmt_*`,
  `include/gl_2d.h` `gl2d_*`, `transform_utils.h`
  `apply_tracked_transform` / `unwind_transform_stack`.
- **Intentional feature prefix**: `replay_ui_hud_render` (the documented
  `replay_ui_*` feature-UI prefix in `src/ui/replay_hud.c`).
- **`src/ui/text_layout.h`** `CodeLayout` / `CodeWrapIter` / `code_layout_*`:
  a pure utility shared by UI, export dumps, and tests. **Borderline** -
  left out of this sweep to honor the "leave neutral helpers" decision,
  but explicitly noted here as the one symbol set a future pass could
  reasonably revisit (would become `UiCodeLayout` / `ui_text_layout_*`).
- **Borrowed cross-module API types** (a header *referencing* a type
  another module owns is correct C API design, not a naming defect -
  the guard must not flag these): `ReplCompileContext` /
  `ReplCompiledChange` in `src/editor/services.h`; the `Repl*`
  snapshot fields in `src/ui/snapshot.h`; the export / replay-
  annotation bridge types in `src/app/glr_ctrl.h`; and
  `UiVariablePanelState` surfaced by `src/widgets/variable_panel_state.h`.
  The rule: a symbol is in scope only if a module's *own* header
  **declares/typedefs** it with a foreign prefix - not if the header
  merely *uses* a type owned elsewhere.

## Phase 1 - Cross-module collisions (the confusing ones)

Highest value: kills the `Repl*`-means-two-things ambiguity. One commit.

**`src/editor/` (`Repl` → `Editor`)**
- `state.h`: `ReplEditorBuffer`→`EditorBuffer`, `ReplEditorInputState`→
  `EditorInputState`, `ReplEditorInputView`→`EditorInputView`,
  `ReplSelectionState`→`EditorSelectionState`, `ReplClipboardState`→
  `EditorClipboardState`, `ReplSearchState`→`EditorSearchState`,
  `ReplAutocompleteState`→`EditorAutocompleteState`,
  `ReplVariableDragState`→`EditorVariableDragState`
- `input.h`: `ReplModifierProvider`→`EditorModifierProvider`,
  `ReplInputDispatchEffects`→`EditorInputDispatchEffects`
- `undo.h`: `ReplUndoSnapshot`→`EditorUndoSnapshot`,
  `ReplUndoRingState`→`EditorUndoRingState`

**`src/ui/` (`Repl`/`Editor`/bare/`REPL_` → `Ui`/`UI_`)** - *prefix by owning file*
- `state_types.h`: `ReplCodePanelRuntimeState`→`UiCodePanelRuntimeState`,
  `ReplHelpState`→`UiHelpState`, `ReplProfilePanelState`→
  `UiProfilePanelState`, `ReplStatusState`→`UiStatusState`,
  `ReplPointerState`→`UiPointerState`, `ReplViewportState`→
  `UiViewportState`
- `ReplVariablePanelState` (defined in `src/ui/state_types.h:46`) →
  `UiVariablePanelState`, **but** it is *not* a UI-private type: it is
  exposed in the public API of `src/widgets/variable_panel_state.h`
  (`variable_panel_view()` / `variable_panel_view_mut()`, and a
  `view` field of the widget's own `VariablePanelState`). The earlier
  "widget modules already clean" statement is therefore wrong for this
  one type. Decision: it stays a `Ui*` type (UI owns the
  definition; the widget borrows it by value), and is recorded under
  Sanctioned Exceptions as an **intentional cross-module value type**
  - the widget API legitimately surfaces a `Ui*` value, exactly as
  `editor/services.h` legitimately surfaces `Repl*` (see guard scope
  below). Renaming the widget's own `VariablePanelState` is out of
  scope.
- `repl_code_panel.h`: `ReplSyntaxKind`→`UiSyntaxKind`,
  `ReplSyntaxSpan`→`UiSyntaxSpan` (file name and the
  `ui_repl_code_panel_*` fns stay - "ui's repl-code-panel adapter" is
  deliberate)
- `editor.h`: `TransformerKind`→`UiTransformerKind`, `EditorTransformer`→
  `UiTransformer`, `EditorTransformerList`→`UiTransformerList`,
  `HighlightKind`→`UiHighlightKind`, `EditorHighlight`→`UiHighlight`,
  `EditorHighlightList`→`UiHighlightList`, `VirtualLineStyle`→
  `UiVirtualLineStyle`, `EditorVirtualLine`→`UiVirtualLine`,
  `EditorVirtualLineList`→`UiVirtualLineList`, `EditorLineOverride`→
  `UiLineOverride`, `EditorLineOverrideList`→`UiLineOverrideList`
- `menu_bar.h`: `REPL_MENU_BAR_PIN_*`→`UI_MENU_BAR_PIN_*`
- `layout.h`: `CodePanelLayout`→`UiCodePanelLayout`
- `profile_panel.h`: `ProfilePanelMode`→`UiProfilePanelMode`

**`src/app/` (`Repl`/`REPL_` → `Glr`/`GLR_`)**
- `glr_actions.h`: `ReplMenuId`→`GlrMenuId`, `REPL_FILE_ITEM_*`→
  `GLR_FILE_ITEM_*`, `REPL_SCENE_FIXED_COUNT`→`GLR_SCENE_FIXED_COUNT`
  (verify no clash with existing `GLR_SCENE_*`/`GLR_MENU_*` enumerators)
- `glr_camera.h`: `ReplCameraState`→`GlrCameraState` (~9 files; the
  header comment already anticipates this rename)

**`src/widgets/replay.h` (`Repl` → `Replay`)**
- `ReplVertexWalkState`→`ReplayVertexWalkState`, `ReplVertexWalkContext`→
  `ReplayVertexWalkContext`, `ReplVertexWalkCallbacks`→
  `ReplayVertexWalkCallbacks`, `ReplTessPreviewCallbacks`→
  `ReplayTessPreviewCallbacks`

## Phase 2 - Orphan / un-prefixed functions

Separate commit (function renames, distinct review concern).

- `src/repl/executor.h`: `apply_state_cmd`→`repl_apply_state_cmd`
- `src/app/glr_completion.h`: `accept_autocomplete`→
  `glr_completion_accept_autocomplete`
- `src/editor/search.h`: `search_clear_all`→`editor_search_clear_all`,
  `handle_search_key`→`editor_search_handle_key`,
  `handle_search_special`→`editor_search_handle_special`
- `src/editor/commit.h` `try_commit_*` family (11 symbols:
  `try_commit_float_decl`, `try_assign_variable`, `try_commit_for_loop`,
  `try_commit_func_def`, `try_commit_if_block`, `try_commit_close_brace`,
  `try_commit_var_statements`, `try_commit_block_structs`,
  `try_commit_any`, `try_commit_var_statements_then_insert`) →
  `editor_*` prefix. **CLAUDE.md references these names extensively** -
  update the "Command Lifecycle" / "Commit Dispatch Sites" sections in
  the same commit.
- `src/editor/input.h`: `delete_cmd_range`→`editor_delete_cmd_range`,
  `load_line_to_input`→`editor_load_line_to_input`.
- **`feed_line` / `navigate_to_line` - resolved (not a blind rename).**
  Verified layout: `int feed_line(const char *)` is the real impl
  (`input.c:1491`, declared in `input.h:149`); `void
  editor_feed_line(const char *)` is a *separate* thin wrapper
  (`input.c:1564`) that calls `feed_line()` and **discards its `int`
  result**, declared in `repl/core.h:209`. `navigate_to_line` /
  `editor_navigate_to_line` follow the same wrapper pattern
  (`core.h:179`). A mechanical `feed_line`→`editor_feed_line` rename
  would collide with the existing wrapper and silently drop the
  success signal. **Final signature is decided up front:** collapse
  the pair into a single `int editor_feed_line(const char *)` (and
  `void editor_navigate_to_line(int)` - it has no return value to
  preserve). Steps: (1) delete the void `editor_feed_line` wrapper;
  (2) rename the `int feed_line` impl to `editor_feed_line`, keeping
  the `int` return; (3) existing call sites of the old void wrapper
  already ignore the return, so they remain valid as statement-form
  `int` calls - no caller edits needed beyond the symbol rename;
  (4) move the canonical declarations out of `repl/core.h` into an
  editor header (`editor/input.h`) so `src/repl/` no longer declares
  editor symbols. This is the one place naming overlaps a layering
  fix; treat it as its own sub-commit within Phase 2.

## Phase 3 - Intra-module enum tidy (scene)

Smallest, lowest-risk; can ride with Phase 2 or stand alone.

- `src/scene/themes.h`: `GridTheme`→`SceneGridTheme`,
  `AxesTheme`→`SceneAxesTheme`, `GridMajorIdx`→`SceneGridMajorIdx`,
  `GridExtentIdx`→`SceneGridExtentIdx` (~5 files each; check
  `src/app/glr_state.h` accessors and `g_cfg_items[]` cycle wiring).

## Phase 4 (optional) - Lightweight enforcement guard

Add `check-module-prefixes` to the `check-state-ownership` aggregate
(Makefile around line 892), in the project's existing guard idiom.

**The guard must match owned declarations, not foreign references.**
A blanket `grep -rn 'Repl' src/editor` is wrong - it false-positives on
legitimate borrowed-API types (`ReplCompileContext` /
`ReplCompiledChange` in `editor/services.h`, `Repl*` snapshot fields
in `ui/snapshot.h`, export/annotation bridge types in
`app/glr_ctrl.h`). The rule matches only **stale owned declarations**:
grep each module's headers for *declaration* forms of a foreign prefix
- `typedef struct <Foreign>… }`, `typedef enum … } <Foreign>`,
`} <Foreign>;`, and `<foreign_>…(` prototypes whose definition lives
in that module's `.c`. Concretely, since the renamed set is finite and
known, the simplest correct guard is an explicit denylist of the exact
stale names this plan eliminates (`ReplEditorBuffer`,
`ReplVariablePanelState`-as-a-typedef-in-editor, `ReplMenuId`,
`ReplVertexWalkState`, …) asserting they no longer appear as a
`typedef`/`} X;` in their new home - not a prefix sweep. Keep it a
pure `grep`/`! grep` shell rule, no new tooling. Skip entirely if the
user wants the cleanup landed without new gates.

## Mechanics

- Per symbol: `grep -rl '<Sym>' --include='*.c' --include='*.h' .` to
  scope, then `gsed -i 's/\b<Old>\b/<New>/g'` on that file set (GNU sed
  is `gsed` on this macOS box). Word boundaries are required - e.g.
  `feed_line` must not catch `editor_feed_line`.
- **Also sweep `scripts/` and `scripts/baselines/`** (discovered in
  Phase 1, not in the original scope): several ownership guards
  hardcode type names in their signature regexes
  (`check-color-picker-ui-isolation.sh` matched
  `EditorTransformer`, `check-repl-no-direct-editor.sh` matched
  `ReplEditorBuffer`, `check-no-test-default-output.sh` matched
  `REPL_FILE_ITEM`), and the `editor-ownership-budget.txt` /
  `ui-returns-hits-only.txt` baselines reference the names in comment
  prose. The Makefile may also reference symbols. Add these to the
  per-phase scope or `make check-state-ownership` fails after the
  code rename even though the build/tests pass.
- `AGENTS.md` is a **symlink to `CLAUDE.md`** - editing `CLAUDE.md`
  covers both; do not edit `AGENTS.md` separately.
- Rename the `_s` tag and the typedef together where structs use
  `typedef struct X_s { ... } X;`.
- Update doc/prose references in the same commit: `CLAUDE.md`,
  `MODULES.md`, `AGENTS.md`, `ARCHITECTURE.md`, and any `done/*.md` /
  `plans/**/*.md` that name a renamed symbol. Drive this off
  `grep -rl '<Old>' --include='*.md' .` per renamed symbol rather than
  a fixed file list, so no live doc is missed.
- After **each phase**: `make clean && make sample && make test &&
  make check-state-ownership && make check-c99`. Also build the stub
  path for the touched headers: `make test-stubs`.
- Commit per phase with the survey-derived symbol list in the message
  so the diff is auditable.

## Critical Files

- Editor: `src/editor/state.h`, `input.h`, `undo.h`, `search.h`,
  `commit.h` (+ their `.c` and all callers)
- UI: `src/ui/state_types.h`, `repl_code_panel.h`, `editor.h`,
  `menu_bar.h`, `layout.h`, `profile_panel.h`
- App: `src/app/glr_actions.h`, `glr_camera.h`, `glr_completion.h`,
  `glr_state.h`
- Widgets: `src/widgets/replay.h`
- Scene: `src/scene/themes.h`, `src/app/glr_state.h`
- Repl: `src/repl/executor.h`, `src/repl/core.h` (editor-decl relocation)
- Docs: `CLAUDE.md`, `MODULES.md`, `AGENTS.md`, `ARCHITECTURE.md`,
  `done/*.md`, `plans/**/*.md`

## Verification

1. `make clean && make sample` - compiles with no
   implicit-declaration / unknown-symbol errors (C99 hard errors catch
   any missed call site).
2. `make test` - all suites green; pay attention to
   `test_repl_core_*`, replay-walk, and any test that referenced a
   renamed type.
3. `make check-state-ownership && make check-c99` - ownership guards and
   the C99 ratchet still pass.
4. `make test-stubs` - stub build path still compiles.
5. `./sample` smoke: open Config flyouts (exercises `GlrMenuId` /
   `g_cfg_items`), toggle grid/axes themes (scene-theme enums), open the
   color picker and variable panel, run a replay (replay-walk types),
   and trigger autocomplete (the renamed completion fn).
6. Per-symbol absence check (**not** a blanket `grep 'Repl'`, which
   correctly still matches borrowed-API types): for each stale name in
   the Phase 1-3 tables, assert it no longer appears as a
   `typedef`/`} <Name>;`/prototype in its old home -
   e.g. `! grep -RnE 'typedef .*\b(ReplEditorBuffer|ReplMenuId|…)\b|}
   (ReplEditorBuffer|…);' src/editor src/ui src/app src/widgets`.
   Borrowed references (`ReplCompileContext` in `editor/services.h`,
   `Repl*` in `ui/snapshot.h`, bridges in `app/glr_ctrl.h`,
   `UiVariablePanelState` in `widgets/variable_panel_state.h`) are
   expected to remain and must pass.
