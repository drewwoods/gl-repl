# Gold-Standard State Ownership

> **Working directory:** `/Users/drew/src/code/opengl/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/`
> **Branch base:** `imrepl/repl-cleanup-push-refinement`

## Context

The current `repl_state.h` / `repl_state_views.h` / `repl_state_owners.h` split is still a pointer facade over globals living in `repl_state.c`. The outer pointer may be `const`, but the inner fields are still `int *`, `float *`, `char *`, etc., so code like `*cp->cursor_px = cursor_x` in `ui_panels.c` still compiles. That is the structural bug: the type system does not distinguish read-only observation from writable state.

This document keeps the fix aligned with the architecture we actually want:

- `repl_state` remains the canonical home of runtime state.
- `ReplRuntimeState` becomes the real aggregate that owns the mutable bytes.
- Public state structs hold values, not writable aliases into globals.
- State getters return pure values or by-value snapshots trivially.
- Renderers consume frozen per-frame views.
- Full runtime capture becomes straightforward for continuation, undo assembly, and tutorial-style replay.

The goal is not to explode storage across dozens of owner modules. The goal is to make `repl_state` a real state container rather than a typed bag of pointers.

The intended outcome:

- Writing through a view fails to compile because views no longer expose writable scalar pointers.
- `ReplRuntimeState` is the single source of truth for live runtime state.
- Small state slices such as `selected`, `visible`, `scroll`, `tab_idx`, and similar leaf values live as ordinary fields inside nested state structs, not as re-exported pointer fields.
- UI renderers are pure functions of snapshots plus optional output structs.
- Runtime capture and restore are first-class operations instead of a future refactor.

## Tenets

These are the rules the end state must satisfy.

1. **`repl_state` remains the owner.** The live runtime state stays centralized in `repl_state.c` as one `static ReplRuntimeState g_repl_state;`. We are not deleting `repl_state`; we are making it the actual owner.
2. **Runtime fields are values, not aliases.** `ReplRuntimeState` and its nested public state structs store ints, floats, enums, arrays, and owned buffers directly. No `int *cursor_px`, no `float *panel_frac`, no `char *status`. Embedded arrays are fine; writable pointer fields are not.
3. **Read APIs return values.** Getters return scalars by value or snapshot structs by value. If a snapshot needs to expose a large collection, it does so via read-only span-style fields such as `(const T *, int count)` or a by-value view struct containing those read-only fields. No getter returns a mutable pointer to state.
4. **Writes go through named mutators.** Mutation happens through explicit functions such as `repl_state_help_set_visible`, `repl_state_code_panel_set_scroll`, `repl_state_search_set_query`, or higher-level `repl_action_*` helpers. `_mut()` accessors are transitional only and are removed by the end.
5. **Renderers consume snapshots; render is pure.** `imrepl_ctrl.c` builds `Ui*View` and `Scene*View` inputs from `ReplRuntimeState` once per frame and passes them to renderers. Render functions do not read live runtime state directly and do not call mutators.
6. **Render-time discoveries flow through outputs.** If rendering computes state another consumer needs, it goes into a `Ui*Output` struct and is actualized by the controller back into `repl_state` after the render call. Renderers never write directly into runtime state.
7. **Runtime capture is first-class.** `ReplRuntimeState` can be copied, restored, or serialized intentionally. That supports continuation, deterministic tutorial replay, undo assembly, and future tooling without re-deriving state from scattered globals.
8. **Headers reflect the rule.** The public state API centers on `repl_state.h` with value-returning getters, snapshot structs, and named mutators. If `repl_state_views.h` / `repl_state_owners.h` survive during migration, they are temporary adapters, not the final design.
9. **Lifecycle is explicit.** `repl_state_init()`, `repl_state_reset()`, `repl_state_capture()`, and `repl_state_restore()` are explicit entrypoints. No lazy first-read initialization.

## Target Shape

The target is a single aggregate with nested value structs.

```c
typedef struct ReplHelpState {
    int visible;
    int tab_idx;
    int scroll;
} ReplHelpState;

typedef struct ReplCodePanelState {
    float panel_frac;
    int resizing_panel;
    int scroll;
    int scroll_follow_cursor;
    int cursor_visible;
    int blink_tick;
    int cursor_px;
    int cursor_py;
} ReplCodePanelState;

typedef struct ReplRuntimeState {
    ReplHelpState help;
    ReplCodePanelState code_panel;
    ReplSearchState search;
    ReplAutocompleteState autocomplete;
    ReplPresentationState presentation;
    ReplDocumentState document;
    ReplReplayState replay;
    /* ...other nested state slices... */
} ReplRuntimeState;
```

Public access then looks like this:

```c
ReplHelpState repl_state_help(void);
ReplCodePanelState repl_state_code_panel(void);
int repl_state_help_visible(void);
void repl_state_help_set_visible(int visible);
void repl_state_code_panel_set_cursor_pixel(int x, int y);

ReplRuntimeState repl_state_capture(void);
void repl_state_restore(const ReplRuntimeState *snapshot);
```

For large collections, the runtime still owns the storage inline, but readers consume a read-only view:

```c
typedef struct ReplDocumentView {
    const GLCmd *cmds;
    int count;
    int edit_line;
    int normals_dirty;
} ReplDocumentView;

ReplDocumentView repl_state_document(void);
```

That distinction matters:

- runtime ownership stays centralized and value-based inside `ReplRuntimeState`
- read paths stay cheap for large arrays
- no public API exposes writable scalar pointers
- leaf values like `selected` become ordinary struct fields, not special cases

## Render-Time Mutations: Intent Outputs, Controller Actualization

This is still the right mechanism for state that is discovered during render.

### The problem

Some state is produced by drawing. Cursor pixel position is the canonical example:

- `ui_panels.c` is the only place that knows where the cursor glyph landed because the answer depends on wrapping, font metrics, scroll, and panel geometry.
- `ui_autocomplete_panel.c` needs that screen-space position to place the popup.
- The controller cannot precompute it without duplicating layout.
- We do not want renderers writing directly back into `ReplRuntimeState`.

Today this is solved by writing through a pointer field exposed by `repl_state`. That is exactly what this plan removes.

### The pattern

Renderers take a read-only input view and, when needed, a small output struct:

```c
void ui_panels_render(const UiPanelsView *in, UiPanelsOutput *out);
```

- `in` is built from `ReplRuntimeState` by the controller.
- `out` is zero-initialized by the controller and filled by the renderer.

Example:

```c
typedef struct UiPanelsOutput {
    int cursor_px;
    int cursor_py;
    int cursor_pixel_valid;
} UiPanelsOutput;
```

Inside the renderer:

```c
out->cursor_px = cursor_x;
out->cursor_py = io_line_y;
out->cursor_pixel_valid = 1;
```

And in the controller:

```c
UiPanelsView panels_view;
UiPanelsOutput panels_out = {0};

imrepl_ctrl_build_panels_view(&panels_view);
ui_panels_render(&panels_view, &panels_out);

if (panels_out.cursor_pixel_valid)
    repl_state_code_panel_set_cursor_pixel(
        panels_out.cursor_px,
        panels_out.cursor_py);
```

`ui_autocomplete_panel.c` then reads cursor position from its own view, built from the now-actualized state snapshot.

### What belongs in `out`

`out` is for render-discovered state another consumer needs.

- cursor pixel position
- render-time hit-test results that a later step consumes
- clamped scroll requests or similar layout findings

What does not belong in `out`:

- input-triggered state changes
- anything the controller can derive directly from the view without rendering
- arbitrary cross-layer side effects unrelated to rendering

### Why this still matters with centralized state

Keeping `ReplRuntimeState` centralized does not weaken this rule. It makes it cleaner:

- `repl_state` owns the bytes
- the controller owns the frame pipeline
- renderers stay pure
- actualization back into runtime state happens in one place

That separation is what prevents `const` views from degenerating into writable state again.

## Checks Defined Up Front

These checks are wired into `make test` before the migration work begins. They enforce the value-based `ReplRuntimeState` direction rather than a per-domain extraction plan.

### check-no-write-through-view (NEW)

Keep the existing structural guard against `*cp->cursor_px = ...` style writes.

```makefile
check-no-write-through-view:
	@echo "Checking for writes through view pointers..."
	@bad=$$(grep -nE '^\s*\*[a-z_][a-zA-Z0-9_]*->[a-zA-Z0-9_]+\s*(=[^=]|\+\+|--|\+=|-=|\*=|/=)' \
		$(SCENE_SRCS) $(UI_SRCS) | grep -vE '//.*$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: writes through view-pointer fields:"; \
		echo "$$bad"; exit 1; \
	fi
```

Initial allowlist contains the known cursor-pixel write. It goes empty once the render-output pattern lands.

### check-runtime-state-value-fields (NEW)

`ReplRuntimeState` is the core contract. Its fields should be owned values, not aliases.

The check scans the `typedef struct ReplRuntimeState` block in `repl_state.h` and fails on pointer fields. Embedded arrays are allowed. Function pointers are irrelevant here and should not exist in runtime state.

Intent:

- fail on `int *cursor_px;`
- fail on `char *status;`
- allow `char status[256];`
- allow `GLCmd cmds[MAX_CMDS];`

This is the direct encoding of the desired architecture.

### check-public-state-no-writable-pointers (NEW)

Parse public `Repl*State`, `Repl*View`, `Ui*View`, and `Scene*View` struct definitions and fail if they contain writable scalar pointers.

Allowed:

- plain value fields
- embedded arrays
- read-only span fields such as `const GLCmd *cmds; int count;`

Forbidden:

- `int *selected`
- `float *panel_frac`
- `char *query`

This replaces the old assumption that state safety only comes from scattering ownership across modules.

### check-state-getters-return-values (NEW)

State getters should return scalars or snapshot structs by value. Mutable-pointer returns are forbidden.

Examples of good signatures:

```c
int repl_state_help_visible(void);
ReplHelpState repl_state_help(void);
ReplDocumentView repl_state_document(void);
```

Bad signatures:

```c
ReplHelpState *repl_state_help_mut(void);
const ReplHelpState *repl_state_help_ptr(void);
int *repl_state_help_visible_ptr(void);
```

The check starts as a ratchet against `_mut()` and pointer-returning accessors and goes to zero by the end.

### check-ui-renderer-takes-view (NEW)

Keep the renderer-signature rule:

```c
void ui_<panel>_render(const Ui<Panel>View *in);
void ui_<panel>_render(const Ui<Panel>View *in, Ui<Panel>Output *out);
```

Anything else means the renderer is still reaching around the snapshot boundary.

### check-renderer-no-direct-mutators (NEW)

Migrated renderers may not call:

- `repl_state_*_set_*`
- `repl_action_*`
- legacy `repl_state_*_mut()` accessors

They may only read `in` and write to their `out` parameter.

### check-output-actualization (NEW)

If a renderer fills a `Ui*Output` field, `imrepl_ctrl.c` must actualize it back into runtime state or consume it immediately. This prevents render-discovered state from being silently dropped.

### check-mut-accessor-count (NEW, ratchet)

Count legacy `_mut()` accessor usage and fail if the count increases. Ratchet it down to zero.

This keeps migration pressure on the real problem without requiring a large-bang rewrite.

### check-runtime-capture-roundtrip (NEW)

Add a focused test target that:

1. initializes runtime state
2. mutates a representative set of fields
3. captures `ReplRuntimeState`
4. resets state
5. restores from the snapshot
6. verifies value equality

This is the proof that centralized state capture is not just an aspiration.

### Existing checks that stay

These stay useful and should be tightened against the new architecture rather than removed:

- `check-gl-boundaries`
- `check-layer-coupling`
- `check-controller-boundaries`
- `check-scene-no-repl-state-mut`
- `check-pure-scene-no-repl-state`
- `check-state-boundaries`
- `check-ui-no-repl-state-mut`
- `check-public-api-usage`

### Wiring

The umbrella target becomes:

```makefile
check-state-ownership: check-no-write-through-view \
	check-runtime-state-value-fields \
	check-public-state-no-writable-pointers \
	check-state-getters-return-values \
	check-ui-renderer-takes-view \
	check-renderer-no-direct-mutators \
	check-output-actualization \
	check-mut-accessor-count \
	check-runtime-capture-roundtrip

test: ... check-state-ownership
```

## Stages

Each stage is reviewable on its own. The plan is intentionally incremental and does not require moving all storage out of `repl_state`.

### Stage 0 — Land the checks

Add the new checks and baselines while preserving behavior.

Files:

- `Makefile`
- `scripts/check-runtime-state-value-fields.sh`
- `scripts/check-public-state-no-writable-pointers.sh`
- `scripts/check-state-getters-return-values.sh`
- `scripts/check-ui-renderer-signatures.sh`
- `scripts/check-renderer-purity.sh`
- `scripts/check-output-actualization.sh`
- `scripts/baselines/mut-count.txt`

Exit criteria:

- `make test` green
- new checks reflect the current state honestly
- no architectural behavior change yet

### Stage 1 — Make `ReplRuntimeState` real

Turn `ReplRuntimeState` into the actual runtime container in `repl_state.c`.

Scope:

- fold the scattered globals into `static ReplRuntimeState g_repl_state;`
- replace pointer-based leaf storage with nested value structs
- keep existing behavior modules intact
- add `repl_state_capture()` and `repl_state_restore()` entrypoints, even if some call sites still use legacy accessors for a short time

This is the decisive shift: state stays centralized, but it becomes real structured data rather than aliases.

### Stage 2 — Pilot value getters on small slices

Convert a few low-risk slices end to end:

- help
- viewport
- status
- selection

For each slice:

- add a nested value struct to `ReplRuntimeState`
- add by-value getters such as `repl_state_help()`
- add named setters
- stop exposing pointer fields for that slice

Exit criteria:

- no `_mut()` or pointer-returning read path for the pilot slices
- tests for those slices stop poking global state

### Stage 3 — Convert UI-facing leaf state

Apply the same pattern to the state that drives UI behavior directly:

- search
- autocomplete
- pointer state
- variable panel visibility
- code panel flags that are read frequently but are still small values

The goal here is to kill the easy pointer-through-view cases without a large structural rewrite.

### Stage 4 — Fix the cursor-pixel write with outputs

This is the canonical render-discovered state case.

Changes:

- `ui_panels_render` becomes `void ui_panels_render(const UiPanelsView *in, UiPanelsOutput *out)`
- the renderer fills `out->cursor_px`, `out->cursor_py`, and `out->cursor_pixel_valid`
- `imrepl_ctrl.c` actualizes the result via `repl_state_code_panel_set_cursor_pixel(...)`
- `ui_autocomplete_panel` consumes cursor position from its own input view instead of live writable state

Exit criteria:

- the known write-through-view bug is impossible by type
- `check-no-write-through-view` allowlist goes empty

### Stage 5 — Convert medium state slices

Move the remaining pointer-style read APIs for:

- input/editor state
- presentation toggles
- camera state
- workspace state
- clipboard state
- replay UI state

The storage remains inside `ReplRuntimeState`; the work is about API shape, not scattering ownership.

### Stage 6 — Make capture/restore real consumers

Now that `ReplRuntimeState` is authoritative, use that fact.

Scope:

- teach `repl_undo.c` to assemble snapshots from `repl_state_capture()` or from clearly defined state slices derived from it
- add a focused runtime capture/restore test
- document continuation and tutorial replay as supported consumers of the same primitive

This is the stage where the design starts paying for itself directly.

### Stage 7 — Complete UI snapshot purity

Once getters are value-based, finish the render boundary.

For each `ui_*.c` renderer:

- the controller builds a `Ui*View` from `ReplRuntimeState`
- the renderer takes only `const Ui*View *in` or `const Ui*View *in, Ui*Output *out`
- the renderer stops calling `repl_state_*` directly
- any render-time discoveries move through `out`

By the end of this stage, render code is pure even though runtime ownership stays centralized.

### Stage 8 — Collapse transitional state headers

If `repl_state_views.h` and `repl_state_owners.h` still exist, reduce them to compatibility shims or delete them.

Target shape:

- `repl_state.h` is the public state API
- optional `repl_state_internal.h` exists only if tests or helpers need internal declarations
- no public pointer-facade layer remains

### Stage 9 — Final cleanup and documentation

- remove remaining `_mut()` accessors
- update `ARCHITECTURE.md`, `MODULES.md`, and `CLAUDE.md`
- document snapshot/capture semantics explicitly
- keep any future refactor that moves behavior into new modules separate from this state-shape work

## Critical Files

The plan is centered on existing files, not a large family of new owner modules.

Primary files:

- `repl_state.h` — public state structs, by-value getters, mutators, capture/restore API
- `repl_state.c` — owns `g_repl_state`, lifecycle, and snapshot assembly
- `imrepl_ctrl.c` — builds `Ui*View` / `Scene*View` inputs, actualizes `Ui*Output`
- `ui_panels.c` / `ui_panels.h` — first dual-signature renderer/output case
- `ui_autocomplete_panel.c` / `ui_autocomplete_panel.h` — consumes cursor position from a view instead of writable state
- `repl_undo.c` — runtime capture consumer
- `Makefile` and `scripts/` — enforcement and ratchets

Files that may shrink or disappear after migration:

- `repl_state_views.h`
- `repl_state_owners.h`

Existing behavior modules remain relevant, but they stop pretending to own storage:

- `repl_search.c`
- `repl_autocomplete.c`
- `repl_clipboard.c`
- `repl_camera_controls.c`
- `repl_replay.c`
- `repl_editor.c`

They can keep behavior and helper logic while mutating or reading through `repl_state` APIs.

## Verification

Per stage:

1. `make clean && make sample`
2. `make sample USE_GL_STUBS=1`
3. `make test`
4. `make check-state-ownership`
5. `./sample` smoke test for help, search, edit/save/reload, replay, and camera interactions

Focused verification to add during the migration:

- runtime capture/restore round-trip test
- renderer fixture tests using `Ui*View` inputs and `Ui*Output` assertions
- controller ordering tests for cases where one panel's render output feeds another panel's input in the same frame

Final verification:

- `repl_state.c` still exists and owns a single `ReplRuntimeState`
- `ReplRuntimeState` contains no writable pointer fields
- public state getters return values or read-only views, not mutable pointers
- `grep -r 'repl_state_.*_mut' .` returns no production matches
- `make check-state-ownership` is green
- the cursor-pixel case is routed through `UiPanelsOutput` and controller actualization
- every UI renderer matches one of the two canonical signatures
- `repl_state_capture()` / `repl_state_restore()` round-trip is covered by tests

## Risks and Notes

- **Large snapshots can be expensive if copied blindly.** Use by-value getters for small and medium slices. For very large collections, return a by-value view struct with read-only span fields.
- **Do not reintroduce aliases through convenience helpers.** The temptation will be to add `*_ptr()` or `*_mut()` back once migration pressure appears. That defeats the point.
- **Centralized ownership does not excuse render-time writes.** Keeping state in `repl_state` is compatible with pure rendering only if outputs remain the sole render-to-state path.
- **Capture semantics must be explicit.** Some state is durable and belongs in a snapshot. Some state may be frame-local or derived. The design is better precisely because this boundary becomes visible and testable.
- **Avoid API explosion.** Not every leaf value needs its own getter if a slice getter is clearer. Prefer `ReplHelpState repl_state_help(void)` over five tiny getters when that keeps call sites simpler.
- **Do not bundle unrelated behavior refactors into this work.** This plan is about making state ownership and state reads defensible, not about moving every helper to a new file.
- **Tutorial replay and continuation are real design constraints.** The document should keep calling this out so the end state does not optimize only for immediate compile-time cleanup while making future state capture harder.