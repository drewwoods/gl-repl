# Plan: Eliminate Remaining `_mut()` Reads in `src/repl/`

As of **2026-06-08**, `scripts/check-repl-mut-reads.sh` still reports
`repl-mut-reads OK (_mut() calls=11/11)`, so the plan's core goal is still
current. The remaining non-owner call sites are still the same six-file
cluster this draft targeted.

What *did* drift is the accessor shape: the original draft assumed the read
accessors could be dropped into the old pointer-style macros unchanged. That
is no longer true. In the current tree:

- `repl_state_scenes()` returns `ReplSceneRuntimeState` **by value**
  (`src/repl/state_views.h`)
- `repl_state_render()` returns `ReplRenderState` **by value**
  (`src/repl/state_owners.h`)
- `repl_state_import_export()` returns `ReplImportExportView`
  (a read-only view with const pointers into live buffers)

So the work is still valid, but the read-side refactor has to use **view
fields / dedicated getters**, not `STATE->field` pointer macros.

## Objective

Reduce the `mut_call_count` in `scripts/baselines/repl-mut-reads.txt` from
**11** to **0** by replacing the remaining non-owner `_mut()` call sites with:

- const/read-only accessors for reads
- specific setters for narrow writes
- explicitly named writable accessors (for legitimate bulk writes)

## Current Verified Call Sites (2026-06-08)

- `src/repl/scenes.c`
  - `repl_state_scenes_mut()` macro use at line 21
  - `repl_state_import_export_mut()` macro use at line 24
  - `scene_cfg_mut()` at line 107 and use at line 267
  - `pre_example_cfg_mut()` at line 122 and use at line 196
- `src/repl/export.c`
  - `repl_state_import_export_mut()` macro use at line 19
- `src/repl/import.c`
  - `repl_state_import_export_mut()` macro use at line 50
- `src/repl/executor.c`
  - `repl_state_render_mut()` macro use at line 45
- `src/repl/flatten.c`
  - `repl_state_flat_program_mut()` at line 711
- `src/repl/example_loader.c`
  - direct `repl_state_scenes_mut()->active_example_idx = idx;` at line 473

## Key Files & Context

- **Owner files (allowed to use `_mut()`):**
  - `src/repl/state.c`
  - `src/repl/apply.c`
  - `src/repl/command_store.c`
  - `src/repl/eval.c`
- **Ratchet / baseline:**
  - `scripts/check-repl-mut-reads.sh`
  - `scripts/baselines/repl-mut-reads.txt`
- **Typed state APIs that matter here:**
  - `src/repl/state_views.h`
  - `src/repl/state_owners.h`

## Implementation Steps

### 1. Expand the State API with Explicit Read/Write Surfaces

Add the new APIs in `src/repl/state.c`, with declarations split by API shape:

- **read helpers** in `state_views.h` / `state_owners.h` as appropriate
- **writable accessors / setters** in `state_owners.h`

Required additions:

- `int repl_state_active_example_idx(void);`
- `const char *repl_state_workspace_dir(void);`
- `void repl_state_scenes_set_active_example_idx(int idx);`
- `ReplSceneRuntimeState *repl_state_scenes_writable(void);`
- `ReplImportExportState *repl_state_import_export_writable(void);`
- `ReplFlatProgramState *repl_state_flat_program_writable(void);`
- `void repl_state_render_set_clear_color(const float rgba[4]);`
- `void repl_state_render_set_light_enabled(int light_idx, int enabled);`
- `void repl_state_render_clear_light_enabled_mask(void);`

Notes:

- The **scene** read helpers are necessary because `repl_state_scenes()`
  returns a struct **by value** with an embedded `workspace_dir[]`; the old
  `g_workspace_dir` macro cannot safely be rewritten as
  `repl_state_scenes().workspace_dir`.
- The **import/export** read path can use `ReplImportExportView`, because that
  view already exposes stable const pointers into live buffers.

### 2. Refactor `src/repl/scenes.c` (6 hits)

- Replace the current `_mut()`-backed scene macros with a split read/write
  surface:
  - read side via `repl_state_active_example_idx()` /
    `repl_state_workspace_dir()`
  - write side via `repl_state_scenes_writable()`
- Replace the import/export macro cluster the same way:
  - read side via `ReplImportExportView`
  - write side via `repl_state_import_export_writable()`
- Rename local helpers to reflect legitimate mutability:
  - `scene_cfg_mut` -> `scene_cfg_writable`
  - `pre_example_cfg_mut` -> `pre_example_cfg_writable`
- Keep the `_writable` access path only at actual mutation sites.

### 3. Refactor `src/repl/executor.c` (1 hit)

- Remove `#define EXEC_RENDER (repl_state_render_mut())`
- Replace direct state writes with explicit helpers:
  - `repl_state_render_clear_light_enabled_mask()` at the start of each walk
  - `repl_state_render_set_light_enabled(slot, 1/0)` for
    `glEnable/glDisable(GL_LIGHTn)`
  - `repl_state_render_set_clear_color(rgba)` for `CMD_CLEAR_COLOR`
- Any remaining reads should use `repl_state_render()` by value.

### 4. Refactor `src/repl/export.c` and `src/repl/import.c` (2 hits)

- Replace the current `IMPORT_EXPORT_STATE` macro with two distinct surfaces:
  - a read-only view based on `repl_state_import_export()`
  - a write-only pointer based on `repl_state_import_export_writable()`
- Because `ReplImportExportView` already carries const pointers for:
  - `workspace_header_lines`
  - `render_state_lines`
  - `cam_lines`
  - `pending_scene_name`
  - `pending_workspace_dir`
  most current reads can stay macro-friendly after switching from `->` to
  view fields.
- Use the writable accessor only for the actual buffer writes / pending-name
  mutations.

### 5. Refactor `src/repl/flatten.c` (1 hit)

- Replace `repl_state_flat_program_mut()` with
  `repl_state_flat_program_writable()`
- Keep `repl_state_flat_program_set_count()` for the count write; that setter
  already exists and stays valid.

### 6. Refactor `src/repl/example_loader.c` (1 hit)

- Replace:
  - `repl_state_scenes_mut()->active_example_idx = idx;`
- With:
  - `repl_state_scenes_set_active_example_idx(idx);`

### 7. Update the Ratchet Baseline and Comments

- Set `mut_call_count: 0` in `scripts/baselines/repl-mut-reads.txt`
- Rewrite the comments in:
  - `scripts/baselines/repl-mut-reads.txt`
  - `scripts/check-repl-mut-reads.sh`

The current wording says the remaining non-owner `_mut()` sites are legitimate
writes. Once this plan lands, that will no longer be true; the comments should
instead document the new rule: **non-owner files use named writable accessors
or narrow setters, not raw `_mut()` calls**.

## Verification & Testing

Minimum gate:

- `scripts/check-repl-mut-reads.sh`
- `make check-c99`
- `make test-stubs`

Focused regressions worth running because they directly cover the touched
surfaces:

- `make test_repl_core_io USE_GL_STUBS=1`
- `make test_repl_core_examples USE_GL_STUBS=1`
- `make test_repl_core_extra USE_GL_STUBS=1`
- `make test_repl_tune USE_GL_STUBS=1`

Behavior to spot-check:

- Example loading still updates the active example correctly
- Workspace save/load still preserves the workspace directory and pending names
- Export/import still works after the import/export macro split
- Lighting indicator bookkeeping and clear-color updates still work
- Flatten rebuild still writes the flat-program buffers correctly
