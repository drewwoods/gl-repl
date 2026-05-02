# Plan: Complete the Editor-Owns-Text Ownership Split

## Context

`feature/editor-owns-text.md` (Steps 2–6) finished the *data-shape* part
of the redesign:

- `GLCmd.source[]` is gone; per-line text lives in `ReplEditorBuffer`.
- The parser returns `ReplParsedLine { GLCmd cmd; char text[] }`.
- Command-store APIs are text-aware (`_with_line[s]`).
- Undo / user scenes / clipboard each carry parallel `lines[][]` text
  sidecars.
- Controller pushes `EditorTransformerList` / `EditorHighlightList` /
  `EditorVirtualLineList` per frame; UI render reads from them.

What it did *not* do is move ownership. The buffer is named
"editor", but:

1. It physically lives on `ReplRuntimeState` alongside `document`,
   `flat_program`, `variables`, `replay`, `scenes`, etc.
2. A REPL module (`repl_command_store.c`) writes to it. The store knows
   about both cmds and text and updates them in lockstep.
3. REPL modules (`repl_replay_annotations.c`, `repl_export.c`,
   `repl_parser.c` reparse paths) reach into the buffer to read text.

In the user-facing model the spirit asks for:

```
Editor owns the text buffer + cursor + undo ring + commit boundaries.
REPL is a validator / compiler:
    repl_parse_validate(line, ctx) -> ParsedLine | error
The editor calls in to validate; on success it stores the parser's
canonical text in its own buffer and asks the cmd store to record the
parsed cmd. The store does not touch text.
```

This plan completes that ownership move. It is a sibling to
`feature/push-architecture-refinement.md` (which already plans the
`sample → imrepl` rename and the `repl_core.c` dissolution) and to the
`editor-namespace` discussion that produced this doc.

## Current Inversions (the punch list)

### A. State-struct mixing

`ReplRuntimeState` holds both REPL slices and editor / UI slices:

| REPL slice (stays) | Editor / UI slice (moves) |
|---|---|
| `document` | `editor_input` |
| `flat_program` | `editor_buffer` |
| `variables` | `editor_transformers` |
| `replay` | `editor_highlights` |
| `scenes` | `editor_virtual_lines` |
| `import_export` | `code_panel` |
| `presentation` (presentation toggles affect rendering, but they're owned by the menu/config UI today) | `selection` |
| `render` | `clipboard` |
| | `autocomplete` |
| | `variable_drag` |
| | `variable_panel` |
| | `help` |
| | `status` |
| | `search` |
| | `profile_panel` |
| | `viewport` |
| | `pointer` |
| | `camera` (scene-side, but tied to editor mouse drags via `repl_camera_controls`) |

Target shape:

```c
typedef struct {
    /* The REPL's own state — program, replay, exports, examples. */
    ReplDocumentState           document;
    ReplFlatProgramState        flat_program;
    ReplVariableState           variables;
    ReplReplayRuntimeState      replay;
    ReplSceneRuntimeState       scenes;
    ReplImportExportState       import_export;
    ReplRenderState             render;        /* GL pipeline knobs */
    ReplPresentationState       presentation;  /* moves to editor? see open question below */
} ReplState;

typedef struct {
    /* Live typing buffer + canonical per-line text. */
    ReplEditorInputState        input;
    ReplEditorBuffer            buffer;

    /* Per-frame snapshots the controller pushes for the renderer. */
    EditorTransformerList       transformers;
    EditorHighlightList         highlights;
    EditorVirtualLineList       virtual_lines;

    /* Editor-flavored UI state. */
    ReplCodePanelRuntimeState   code_panel;
    ReplSelectionState          selection;
    ReplClipboardState          clipboard;
    ReplAutocompleteState       autocomplete;
    ReplSearchState             search;
    ReplVariableDragState       variable_drag;
    ReplVariablePanelState      variable_panel;
    ReplHelpState               help;
    ReplProfilePanelState       profile_panel;
    ReplStatusState             status;

    /* Window-level inputs the editor consumes. */
    ReplViewportState           viewport;
    ReplPointerState            pointer;
    ReplCameraState             camera;        /* drives scene viewing; lives here because it's mouse-driven */
} EditorState;
```

`imrepl_ctrl` owns both as static singletons. Each render path consumes
the relevant state via existing snapshot mechanisms.

### B. Store writes editor text

`repl_command_store.c` owns `editor_buffer.lines[]` mutations today.
After the split:

- `repl_command_store_*` only mutates the cmd array.
- A new `editor_buffer_*` API mutates `EditorState.buffer.lines[]`.
- Commit / paste / load / undo paths call both in the right order. Most
  call sites already pass `(GLCmd, line)` together — the change is
  splitting that one call into two adjacent calls.

### C. REPL reads editor text directly

Three readers reach into the editor buffer:

1. `repl_replay_annotations.c::replay_visible_text(cmd_idx)` — used to
   build subst / eval annotations.
2. `repl_export.c` — composes save-file content.
3. `repl_parser.c` — when re-parsing a stored line during replay or
   reformat.

After the split each takes an editor-buffer view through its API:

```c
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int        line_count;
} EditorBufferView;

void repl_replay_annotations_prepare(EditorBufferView buf);
int  repl_export_save(const char *path, EditorBufferView buf, ...);
```

The controller passes the view from `EditorState.buffer`. REPL stays
ignorant of where the text lives.

### D. `ReplPresentationState` is fuzzy

Presentation toggles (`wireframe`, `grid_theme`, `axes_theme`, vertex
labels, normal vectors, …) drive scene rendering, but they're set
exclusively from the editor's menu bar and config shortcuts. Two options:

- Keep on `ReplState` (current home, scene reads it directly).
- Move to `EditorState` (config UI owns it; scene reads via snapshot).

Recommendation: leave on `ReplState` for now — scene already consumes
it through the snapshot, and the presentation toggles persist with the
program (workspace `@cfg` lines), not with the editor session. Revisit
if a "headless REPL" build target lands.

## Phases

Each phase is a contained commit that builds clean and keeps the test
suite green. Order matters because Phase 1 unblocks Phase 2's call-site
edits.

### Phase 1 — Carve `EditorState` out of `ReplRuntimeState` (~1 day)

Mechanical struct surgery. No behavior change.

1. Define `EditorState` in a new `editor_state.h` with the slices listed
   above.
2. Add a static `EditorState g_editor_state` in `repl_state.c` (or a new
   `editor_state.c`); leave `ReplRuntimeState` minus the editor slices.
3. Update every accessor:
   - `repl_state_editor_buffer*` → `editor_state_buffer*`
   - `repl_state_editor_input*` → `editor_state_input*`
   - `repl_state_clipboard*` → `editor_state_clipboard*`
   - `repl_state_search*` → `editor_state_search*`
   - …and so on for autocomplete / selection / variable_drag / status
     / etc.
4. Add `editor_state_capture()` / `editor_state_restore()` mirroring the
   REPL-side helpers; wire them into `repl_state_reset_all()`.
5. Update `UiRenderSnapshot` builder to read from both `ReplState` and
   `EditorState`.

Header guard checks (`check-views-no-owners`,
`check-ui-no-repl-state-read`) get parallel rules for `editor_state_*`.

### Phase 2 — Stop the store writing text (~1 day)

1. Delete the text-aware overload set on `repl_command_store_*`. The
   `_with_line[s]` variants disappear; the old `_without_line[s]`
   variants become the only API.
2. Add an `editor_buffer_*` mutation API:
   ```c
   void editor_buffer_insert_line(int pos, const char *text);
   void editor_buffer_insert_lines(int pos, const char *const *lines, int count);
   void editor_buffer_replace_line(int pos, const char *text);
   void editor_buffer_delete_range(int start, int count);
   void editor_buffer_load(const char *const *lines, int count);
   ```
3. Walk the call sites currently using `_with_line[s]`. Each one becomes
   two calls in order (text first or cmd first, depending on the
   semantic).
4. Snapshot persistence (undo, scenes, clipboard) splits the same way:
   each carrier already has a `cmds[]` plus a `lines[][]` sidecar; the
   sidecar moves to the editor side, save/restore wires both.

### Phase 3 — REPL reads take a buffer view (~1 day)

1. Define `EditorBufferView`; expose a controller-level builder:
   ```c
   EditorBufferView editor_state_buffer_view(void);
   ```
2. Convert `repl_replay_annotations_prepare()` /
   `repl_replay_code_panel_get_command_display_text()` /
   `repl_export_save()` to take a view.
3. Update `imrepl_ctrl_display_frame` and the export entry point to
   pass `editor_state_buffer_view()`.
4. Drop direct `editor_state_buffer_line(idx)` calls from REPL modules;
   they now read through the supplied view.

After Phase 3, the only REPL module that can reach `EditorState`
directly is `repl_state.c` (for capture / restore symmetry), and even
that goes away if capture / restore split too.

### Phase 4 — Split `repl_commit.c` (~1 day, optional)

1. Move structural validation into `repl_commit_validate.c` (REPL):
   - `try_commit_float_decl_validate`
   - `try_commit_for_loop_validate`
   - `try_commit_func_def_validate`
   - …each returns `ReplParsedLine` + an error code, no mutations.
2. The current orchestration helpers (`try_commit_var_statements`,
   `try_commit_block_structs`, `try_commit_any`) move to
   `editor_commit.c`. They call the validators, then perform the
   `editor_buffer_*` + `repl_command_store_*` writes.
3. `repl_editor.c` calls `editor_commit_*` instead of `try_commit_*`.

### Phase 5 — Rename to match ownership (~mostly mechanical)

Optional but completes the spirit. Rename:

- `repl_undo` → `editor_undo`
- `repl_clipboard` → `editor_clipboard`
- `repl_search` → `editor_search`
- `repl_inline_rename` → `editor_inline_rename`
- `repl_var_drag` → `editor_var_drag`
- `repl_autocomplete` → `editor_autocomplete`
- `repl_layout` → `editor_layout`
- `repl_code_panel_layout` → `editor_code_panel_layout`
- `repl_code_panel_document` → `editor_code_panel_document`
- `repl_actions` → `editor_actions`
- `repl_editor.c` splits into `editor_input.c` (router) + already-moved commit
- `repl_camera_controls` → `scene_camera_controls`

Each rename is a one-commit move with a stub header (or `#include`
redirect) during the transition. Update Makefile, ARCHITECTURE.md,
MODULES.md, callgraph_file_groups.json in the same commits.

## Verification

```bash
make test && make test-stubs

# After each phase, every test should still pass.

# After Phase 1, repl_state_*() callers in editor-state surfaces must
# fail to compile (forcing the rename).
grep -E 'repl_state_(editor_|clipboard|selection|autocomplete|search|status|variable_panel|variable_drag|help|code_panel|profile_panel|viewport|pointer)' \
    *.c | wc -l   # should be 0 outside imrepl_ctrl.c

# After Phase 2, no _with_line[s] APIs remain:
grep -rn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# should be 0

# After Phase 3, REPL modules don't include editor_state.h except
# through their snapshot-view parameters:
grep -l '#include "editor_state.h"' \
    repl_*.c | grep -v repl_state.c
# should be empty
```

## Trade-offs

**Pros**

- The names match what the modules actually own.
- A future "headless REPL" / "embedded REPL in another editor" /
  "scripted REPL test harness" becomes feasible — `EditorState` can be
  swapped or omitted entirely.
- `ReplState` shrinks dramatically; capture / restore become cheaper.
- `repl_command_store.c` becomes a small, focused command-array
  mutation surface again.

**Cons**

- Touches every translation unit that includes `repl_state.h`. Big diff
  but mostly mechanical.
- Test fixtures need updating (currently many tests poke `ReplRuntimeState`
  via the existing accessors; they'll point at `g_editor_state` instead).
- Phase 5 renames invalidate every external doc / branch that referenced
  the old file names.

## Out of Scope

- Phase C of `push-architecture-ui.md` (UI input handlers returning
  `UiAction` lists rather than calling actions inline). That's a
  parallel question; nothing in this plan blocks or unblocks it.
- The `ReplCameraState` placement question. It currently lives on
  `ReplRuntimeState` but is mouse-driven; ownership is genuinely
  ambiguous. Resolve when we audit `repl_camera_controls.c` →
  `scene_camera_controls.c`.
- Color scheme + syntax keyword extraction (deferred sub-task of
  `editor-owns-text.md` Step 6).
