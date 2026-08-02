# Plan: Neutral Source Document Port

## Status

Done. Phases 0-7 landed on `feature/source-document-port`. The hard-zero
`check-repl-no-direct-editor`, `check-repl-demo-no-editor`, and
`check-source-document-port-owners` guards live in
`scripts/check-repl-no-direct-editor.sh`,
`scripts/check-repl-demo-no-editor.sh`, and
`scripts/check-source-document-port-owners.sh`, wired into
`check-state-ownership`. `tools/repl_demo/source_document.c` is the
standalone demo backend; `glr_source_document.c` is the full-app
adapter forwarding to EditorState.

## Goal

Remove direct `editor/*` dependencies from `repl_*.c` and `repl_*.h` while
preserving the current source-command model:

```text
source text + GLCmd source array -> flat commands -> GL execution
```

The REPL pipeline may need source text, and it may need to request source-text
updates when it imports, loads examples, inserts autonormals, or applies a
compiled change. It should not know that the full application stores that text
inside `EditorState`.

The connection point is host/app code:

```text
repl_*.c                 -> neutral source_document_* port
glr_source_document.c    -> full app implementation, forwards to EditorState
tools/repl_demo/...      -> small standalone implementation, no editor link
tests/support/...        -> fixture implementation where tests need isolation
```

This is deliberately a link-time port surface, not a broad callback interface
threaded through every call. REPL code declares/calls neutral host symbols.
The full app and the demo provide implementations. Hosts that do not exercise a
feature do not need to link that feature's implementation; missing required
symbols should fail the link instead of being hidden by empty stubs.

## Load-Bearing Contract

1. `repl_*.c` / `repl_*.h` do not include `editor/state.h`,
   `editor/input.h`, or any other `src/editor/*` header.
2. `repl_*.c` / `repl_*.h` do not call `editor_*`,
   `editor_buffer_*`, or `editor_cursor_*` symbols directly.
3. The neutral source-document headers do not include REPL or editor headers.
4. Full-app source-document implementations live in app/controller code
   (`glr_*`), where it is legal to include both editor and REPL headers.
5. `tools/repl_demo` provides a real source-document implementation backed by
   a tiny static line store. It must not link `src/editor/state.c`.

This keeps both directions clean: REPL does not bleed into editor, and editor
does not expose REPL-shaped APIs. The controller/app layer composes them.

## Non-Goals

- Do not move source text onto `GLCmd`.
- Do not move the source command array out of `ReplState`.
- Do not make every function take an interface struct. Use explicit parameters
  where they already make sense, and neutral host ports for live-document
  access.
- Do not make the editor understand REPL grammar.
- Do not keep empty demo stubs as compatibility shims. The demo should provide
  small working implementations for the source-document contract it uses.

## Current Coupling Inventory

### Header-level coupling

These public or internal REPL headers currently include `editor/state.h`
mainly to get `EditorBufferView`:

- `repl_core.h`
- `repl_compile.h`
- `repl_export.h`
- `repl_executor.h`
- `repl_flatten.h`
- `repl_replay_annotations.h`
- `repl_core_internal.h`

That means a caller that only wants flatten/export/compile types still sees
editor state types.

### Read-only source-text use

These are the easiest to migrate. They need a read-only view over source text:

- `repl_compile.c`: leading whitespace, declaration/use checks, block
  validators.
- `repl_flatten.c`: reparses source text for loop variables, function bodies,
  and expression-bearing commands.
- `repl_executor.c`: resolves flat/source text for status and display text.
- `repl_export.c`: serializes live source lines.
- `repl_replay_annotations.c`: builds display/annotation text from source
  lines.
- `repl_scenes.c`: copies source text into scene slots and workspace exports.
- `repl_core.c`: reformat/save helpers.

### Source-text mutation use

These need a write/apply port or should be moved to an app-side orchestration
function:

- `repl_load.c`: applies `ReplCompiledChange` text and inserts imported lines.
- `repl_autonormal.c`: inserts/replaces generated `glNormal3f` lines.
- `repl_export.c`: import path inserts declaration marker lines.
- `repl_scenes.c`: loads scene-slot text into the live source document.
- `repl_example_loader.c`: clears source text and resets editor input while
  loading examples.
- `repl_state.c`: clears editor text as part of program reset.
- `repl_core.c`: reformat path writes normalized source text.
- `repl_replay_annotations.c`: writes editor virtual lines. This is not source
  document text and should become a separate app/editor presentation port, not
  part of the source-document port.

### Likely dead or cosmetic coupling

- `repl_command_store.c` includes `editor/state.h` but, in the current branch,
  no longer uses editor symbols. Remove this include early as a cheap cleanup.
- `repl_apply.c` and several headers mention `editor_buffer_*` in comments only.
  Update comments as APIs move.

## Target Port Shape

Create a neutral header, for example `source_document.h`.

It should define read-only text views and text-change shapes without mentioning
REPL or editor:

```c
#ifndef SOURCE_DOCUMENT_H
#define SOURCE_DOCUMENT_H

#include "config.h"  /* MAX_COMMANDS, MAX_LINE_LEN, MAX_COMMIT_CMDS */

typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int          line_count;
} SourceTextView;

static inline const char *source_text_line(SourceTextView view, int idx) {
    if (idx < 0 || idx >= view.line_count || !view.lines)
        return "";
    return view.lines[idx];
}

typedef enum {
    SOURCE_TEXT_NO_CHANGE = 0,
    SOURCE_TEXT_INSERT_ONE,
    SOURCE_TEXT_REPLACE_ONE,
    SOURCE_TEXT_INSERT_MANY,
    SOURCE_TEXT_DELETE_RANGE,
    SOURCE_TEXT_LOAD_ALL,
} SourceTextChangeKind;

typedef struct {
    SourceTextChangeKind kind;
    int pos;
    int count;

    /* Optional pre-insert delete, matching the current compiled-change
     * combined shape. delete_pos = -1 / delete_count = 0 means none. */
    int delete_pos;
    int delete_count;

    char text[MAX_COMMIT_CMDS][MAX_LINE_LEN];
} SourceTextChange;

SourceTextView source_document_view(void);
int  source_document_apply_change(const SourceTextChange *change);
int  source_document_insert_line(int pos, const char *line);
int  source_document_replace_line(int pos, const char *line);
int  source_document_load_lines(const char *const *lines, int count);
void source_document_clear(void);

#endif
```

Important details:

- Move `MAX_COMMIT_CMDS` from `repl_compile.h` to `config.h` before adding
  `SourceTextChange`. It is a project-wide source-change bound once text
  changes are neutral, and `config.h` already owns shared capacities such as
  `MAX_COMMANDS` and `MAX_LINE_LEN`. Do not include `repl_compile.h` from
  `source_document.h`.
- Do not put an `EditorState` adapter in editor headers. The full app adapter
  belongs in a `glr_*` file that includes both `source_document.h` and
  `editor/state.h`.

## Host Implementations

### Full app

Add `glr_source_document.c` / `.h` or keep the header private if only the
port symbols are exported.

Implementation:

- `source_document_view()` calls `editor_buffer_view()` and maps it to
  `SourceTextView`.
- `source_document_apply_change()` translates `SourceTextChange` into existing
  editor buffer operations.
- `source_document_insert_line`, `replace_line`, `load_lines`, and `clear`
  forward to `editor_buffer_*`.

This file is an app composition file. It may include `editor/state.h`. REPL
pipeline TUs may not.

### Standalone demo

Add `tools/repl_demo/source_document.c` backed by:

```c
static char g_demo_lines[MAX_COMMANDS][MAX_LINE_LEN];
static int  g_demo_line_count;
```

It implements the same `source_document_*` symbols with real insert, replace,
delete, load, and clear behavior. `tools/repl_demo/repl_demo.c` should use the
neutral API to seed its sample programs. After this lands,
`REPL_DEMO_DEP_SRCS` should drop `src/editor/state.c`.

### Tests

Tests can choose one of two shapes:

- Link the full app `glr_source_document.c` if the test is already app/editor
  integrated.
- Link a fixture implementation under `tests/support/source_document_fixture.c`
  for isolated REPL tests.

Avoid empty test stubs for source text. A broken text implementation should
break tests visibly.

## Phases

### Phase 0 - Add guards and clean obvious dead includes

Before making behavioral changes, add a ratchet guard:

```bash
check-repl-no-direct-editor
```

Initial mode can be informational or baseline-driven. It should report:

- `#include "editor/..."`
- `#include "src/editor/..."`
- `editor_buffer_`
- `editor_state_`
- `editor_cursor_`
- `EditorBufferView`
- `ReplEditorBuffer`

Then remove the apparently unused `editor/state.h` include from
`repl_command_store.c` and update stale comments that mention
`editor_buffer_apply_compiled_change` as the only text apply path.

Acceptance:

- Guard reports the current inventory.
- No behavior changes.

### Phase 1 - Introduce neutral read-only source text

Add `source_document.h` with `SourceTextView` and `source_text_line()`.

Replace `EditorBufferView` in REPL-facing signatures:

- `ReplCompileContext.text`
- `ReplFlattenOptions.text`
- `ReplExecutionOptions.text`
- `repl_export_save_output(...)`
- `repl_dump_code_panel_text(...)`
- `repl_dump_code_panel_visual_text(...)`
- `repl_replay_annotations_prepare(...)`
- `repl_replay_code_panel_get_command_display_text(...)`

Update implementation reads:

- `editor_buffer_view_line(view, idx)` -> `source_text_line(view, idx)`
- `editor_buffer_view()` inside live REPL wrappers -> `source_document_view()`

At this phase, the full app can provide only `source_document_view()`.
Mutation functions can remain unimplemented until Phase 2 if not linked by the
changed objects, but the header should already describe the final contract.

Acceptance:

- No `EditorBufferView` in `repl_*.h`.
- No `editor_buffer_view_line` in `repl_*.c`.
- Existing tests pass.

### Phase 2 - Move source-text mutation behind the port

Add `SourceTextChange` and the mutation port functions.

Add a REPL-side translator from `ReplCompiledChange` to `SourceTextChange`.
The translator may live in `repl_compile.c` or a small helper, but the neutral
header must not include `repl_compile.h`.

Migrate write sites:

- `repl_load.c`: `editor_buffer_apply_compiled_change` becomes
  `source_document_apply_change(repl_compiled_change_text(change))`.
- `repl_load.c`: `editor_buffer_insert_line` becomes
  `source_document_insert_line`.
- `repl_autonormal.c`: insert/replace through `source_document_*`.
- `repl_export.c`: import declaration insertion through
  `source_document_insert_line`.
- `repl_scenes.c`: save paths use `source_document_view`; load paths use
  `source_document_load_lines`.
- `repl_core.c`: reformat writes through `source_document_replace_line`;
  save paths use `source_document_view`.

`repl_state_reset_program()` should stop clearing source text directly. Program
reset is REPL-state reset. Full-world reset belongs to `glr_app_reset_all()`,
which should call both `repl_state_reset_program()` and
`source_document_clear()`. The demo should do the same in its local reset
helper.

Acceptance:

- No `editor_buffer_insert`, `editor_buffer_replace`,
  `editor_buffer_apply`, `editor_buffer_load`, or `editor_buffer_clear` calls
  in `repl_*.c`.
- Reset behavior unchanged in the full app and tests.
- `make test-stubs` passes.

### Phase 3 - Move editor-input cleanup out of REPL loaders

`repl_example_loader.c` currently clears editor input state and cursor state
after loading examples. That is not source document text, and it should not be
part of `source_document_*`.

Replace it with a result/effect returned by the loader, or an app-side wrapper:

```c
typedef struct {
    int clear_input;
    int set_edit_line_to_end;
    int reset_insert_mode;
} ReplExampleLoadEffects;
```

Preferred shape:

- `repl_example_loader.c` loads commands/source text and returns effects.
- `glr_actions.c` / `glr_ctrl.c` applies editor input cleanup.
- `tools/repl_demo` ignores editor-only effects.

Acceptance:

- No `editor_state_input_mut` or `editor_cursor_pos_set` in `repl_*.c`.
- Example load behavior unchanged.

### Phase 4 - Return replay annotation output

`repl_replay_annotations.c` currently writes `editor_state_virtual_lines_*`.
That is not source text. Treat it as a separate presentation output and default
to returning data, not to adding another host port.

Preferred shape:

```c
typedef struct {
    int after_line_idx;
    char text[MAX_LINE_LEN];
} ReplayAnnotationLine;

typedef struct {
    ReplayAnnotationLine lines[MAX_COMMANDS];
    int count;
} ReplayAnnotationOutput;
```

`repl_replay_annotations.c` fills `ReplayAnnotationOutput`. The caller in the
render/controller path publishes those lines into the editor/UI virtual-line
storage. This keeps replay annotation generation pure from the editor's point
of view and avoids another globally implemented host surface.

Fallback shape:

- Add a neutral `code_annotations_*` host port only if carrying
  `ReplayAnnotationOutput` through the render path creates broad unrelated
  churn. Treat that as a documented exception, not the default.

Acceptance:

- No `editor_state_virtual_lines_*` in `repl_*.c`.
- The default implementation uses `ReplayAnnotationOutput`, unless the phase's
  commit explains why the fallback port was necessary.
- Replay annotations still appear in the full app.

### Phase 5 - Finish REPL internal-header cleanup

After read/write/editor-input migrations, split `repl_core_internal.h` so it
does not reintroduce editor coupling by convenience:

Already handled by decouple-7g:

- `src/editor/input.h` owns the modifier-provider typedef directly and no
  longer needs `repl_core_internal.h` for that test seam.
- `glr_completion.h` owns `glr_completion_register_provider` and
  `accept_autocomplete`, so completion app glue no longer needs to be declared
  from a REPL internal header.

Remaining work for this source-document plan:

- `repl_parse_internal.h`: parse/extract/normalize helpers.
- `repl_debug_dump.h`: code-panel dump declarations using `SourceTextView`.
- `repl_scenes.h`: scene promotion/capture/reset declarations.
- `repl_util.h`: `repl_format_fits`, `repl_copy_string_fits`.

`repl_core_internal.h` can either disappear or shrink to parse-only REPL
internals. It must not include editor headers.

Acceptance:

- `src/editor/input.h` and `glr_completion.h` stay independent of
  `repl_core_internal.h`.
- `repl_core_internal.h`, if it remains, includes no editor headers and
  exposes no app/controller/editor input hooks.
- The remaining declarations in `repl_core_internal.h` are either moved to the
  focused headers above or are demonstrably parse-only REPL internals.

### Phase 6 - Make `repl_demo` prove the split

Update `REPL_DEMO_DEP_SRCS`:

- Remove `src/editor/state.c`.
- Add `tools/repl_demo/source_document.c`.
- Keep only the REPL pipeline TUs plus neutral support (`cmd_format.c`,
  `prof.c`, GL stub counts, etc.).

Update `tools/repl_demo/repl_demo.c`:

- Seed source text through `source_document_*`, not `editor_buffer_*`.
- Pass `source_document_view()` to flatten/export/executor paths.
- Keep sample behavior unchanged.

Acceptance:

```bash
make -B repl_demo USE_GL_STUBS=1
./repl_demo --execute
nm repl_demo | rg 'editor_|EditorState|editor_buffer|editor_cursor'
# expected: no matches
```

### Phase 7 - Turn guards hard

Once the demo and full app are green:

- `check-repl-no-direct-editor`: hard fail on editor includes/types/symbols in
  `repl_*.c` and `repl_*.h`.
- `check-repl-demo-no-editor`: fail if `REPL_DEMO_DEP_SRCS` contains
  `src/editor/` or if `nm repl_demo` exposes `editor_*` symbols.
- `check-source-document-port-owners`: implementation symbols
  `source_document_*` may be defined only in approved host files
  (`glr_source_document.c`, `tools/repl_demo/source_document.c`,
  `tests/support/...`).

Keep `check-repl-demo-stubs-shrinking` at zero. The source-document demo file
is not a stub file; it is the demo's real source-text backend.

## Expected End State

```text
REPL pipeline:
  repl_*.c / repl_*.h
  - owns command grammar, compile/apply, flatten, execute, export/import
  - depends on SourceTextView / SourceTextChange
  - does not include editor headers

Editor:
  src/editor/*.c / *.h
  - owns text buffer, cursor/input, completion UI state, undo, selection
  - does not expose REPL-shaped adapters

Full app/controller:
  glr_source_document.c
  - composes editor storage with REPL pipeline through source_document_* ports

Standalone demo:
  tools/repl_demo/source_document.c
  - proves REPL pipeline runs with a non-editor source-text backend
```

At that point `feature/decouple-repl-from-gl-repl-alt.md` remains complete,
and this plan tightens the next layer: source-level ownership, not just
link-level app/UI/controller decoupling.
