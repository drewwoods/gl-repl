# Gold-Standard State Ownership

> **Working directory:** `/Users/drew/src/code/opengl/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/`
> **Branch base:** `imrepl/repl-cleanup-push-refinement`

## Context

The current `repl_state.h` / `repl_state_views.h` / `repl_state_owners.h` split is still a pointer facade over globals living in `repl_state.c`. The outer pointer may be `const`, but the inner fields are still `int *`, `float *`, `char *`, etc., so code like `*cp->cursor_px = cursor_x` in `ui_panels.c` still compiles. That is the structural bug: the type system does not distinguish read-only observation from writable state.

This document keeps the fix aligned with the architecture we actually want:

- `repl_state` remains the canonical home of runtime state.
- `ReplRuntimeState` becomes the real aggregate that owns the mutable bytes.
- Public state structs hold values, not writable aliases into globals.
- Read paths return pure values (or read-only view structs). Pointer-returning *read* getters are gone.
- Write paths go through one slice-level `_mut()` accessor, not a fan of per-field named setters. Named setters are kept only where a non-trivial invariant must be enforced (clamping, ttl, cache invalidation).
- Renderers consume frozen per-frame views.
- Full runtime capture becomes straightforward for continuation, undo assembly, and tutorial-style replay.

The goal is not to explode storage across dozens of owner modules, and not to explode the API surface with a named setter per leaf field. The goal is to make `repl_state` a real state container with a small, predictable read/write surface.

The intended outcome:

- Writing through a *read* result fails to compile because reads return values or `const`-fielded view structs, not writable aliases.
- `ReplRuntimeState` is the single source of truth for live runtime state.
- Small state slices such as `selected`, `visible`, `scroll`, `tab_idx`, and similar leaf values live as ordinary fields inside nested state structs, not as re-exported pointer fields.
- Each slice has one read entrypoint (by-value getter) and one write entrypoint (`_mut()` returning a pointer into `g_repl_state`). Domain helpers (`repl_state_status_set`, `repl_state_camera_reset_default`, ...) survive when they encapsulate non-trivial behavior, but trivial leaf setters do not get added.
- UI renderers are pure functions of snapshots plus optional output structs.
- Runtime capture and restore are first-class operations instead of a future refactor.

## Stage-1 Reference Commit

Commit `b2f649128b9dcaccb86118c6a827f9b812cfc33d` is the reference pattern for
Stage 1. It established the three moves that later work must stay aligned
with:

- live storage moved into one real `static ReplRuntimeState g_repl_state;`
- the old accessor surface kept working through a transitional
  `ReplRuntimeFacade` in `repl_state_views.h`
- `repl_state_capture()` / `repl_state_restore()` landed immediately, with a
  focused round-trip regression in `test_repl_state.c`

The first and third items are the non-negotiable Stage-1 invariants. The
second item is only a migration aid.

- Keeping a slice behind `ReplRuntimeFacade` is acceptable during Stage 1.
- Bypassing the facade for a slice and returning `g_repl_state.<slice>`
  directly is also acceptable.
- What is not acceptable is touching runtime-storage shape without also
  keeping `repl_state_capture()` / `repl_state_restore()` current and tested.

The exact `repl_state_capture()` signature is not the point during migration.
Whether it is `ReplRuntimeState repl_state_capture(void)` or
`void repl_state_capture(ReplRuntimeState *snapshot)`, the contract is the
same: every slice that has become real owned runtime state must round-trip
through capture and restore.

## Observed Drift After The Pilot

The recent Stage-1-labelled commits show why this needs to be explicit:

- `4adeca0` converted `ReplHelpState` to direct-value fields and returned
  `g_repl_state.help` directly. That is valid work, but it is already
  consuming the later slice-conversion plan, not just Stage-1 storage setup.
- `fb2ac9b` continued the same pattern for additional `state_views` and
  variable-panel-facing slices.
- `6c35edf` moved search and related UI-facing leaf state along the same path.
- `b58f624` retargeted more camera/autocomplete/view callers to the new shape.
- `a955b86` did the same for selection, clipboard, scenes, and import/export.

None of those commits are inherently wrong. The problem is that the commit
labels and instructions no longer made it obvious whether the work was:

- pure Stage-1 ownership consolidation
- deliberate later-stage slice graduation
- or a mixed commit doing both

Future work must say which of those it is.

## Tenets

These are the rules the end state must satisfy.

1. **`repl_state` remains the owner.** The live runtime state stays centralized in `repl_state.c` as one `static ReplRuntimeState g_repl_state;`. We are not deleting `repl_state`; we are making it the actual owner.
2. **Runtime fields are values, not aliases.** `ReplRuntimeState` and its nested public state structs store ints, floats, enums, arrays, and owned buffers directly. No `int *cursor_px`, no `float *panel_frac`, no `char *status`. Embedded arrays are fine; writable pointer fields are not.
3. **Read APIs return values.** Getters return scalars by value or snapshot structs by value. If a snapshot needs to expose a large collection, it does so via read-only span-style fields such as `(const T *, int count)` or a by-value view struct containing those read-only fields. **No `read` getter returns a pointer to state, mutable or otherwise.** That is the structural invariant — readers cannot incidentally write back.
4. **Writes go through `_mut()` accessors.** Each slice exposes one writable accessor: `ReplHelpState *repl_state_help_mut(void)`. Callers mutate fields through that pointer. **Named per-field setters (`..._set_visible`, `..._set_scroll`) are not added.** They explode the API surface for no architectural benefit; the structural rule is enforced by the read/write split, not by counting setters. Domain helpers such as `repl_state_status_set`, `repl_state_camera_reset_default`, or `repl_state_camera_set_orbit` survive when they encode an invariant the caller should not have to know (ttl, scale clamping, derived-state invalidation), but routine leaf writes go through `_mut()`.
5. **Renderers consume snapshots; render is pure.** `imrepl_ctrl.c` builds `Ui*View` and `Scene*View` inputs from `ReplRuntimeState` once per frame and passes them to renderers. Render functions do not read live runtime state directly and do not call `_mut()`.
6. **Render-time discoveries flow through outputs.** If rendering computes state another consumer needs, it goes into a `Ui*Output` struct and is actualized by the controller back into `repl_state` after the render call. Renderers never write directly into runtime state.
7. **Runtime capture is first-class.** `ReplRuntimeState` can be copied, restored, or serialized intentionally. That supports continuation, deterministic tutorial replay, undo assembly, and future tooling without re-deriving state from scattered globals.
8. **Headers reflect the rule.** The public state API centers on `repl_state.h` with by-value getters, by-value snapshot structs, and `_mut()` accessors. If `repl_state_views.h` / `repl_state_owners.h` survive during migration, they are temporary adapters, not the final design.
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
/* read paths: by-value */
ReplHelpState      repl_state_help(void);
ReplCodePanelState repl_state_code_panel(void);

/* write paths: one _mut() per slice */
ReplHelpState      *repl_state_help_mut(void);
ReplCodePanelState *repl_state_code_panel_mut(void);

/* invariant-bearing helpers stay; trivial leaf setters do not exist */
void repl_state_status_set(const char *message);
void repl_state_camera_reset_default(void);

/* full-state lifecycle */
void repl_state_capture(ReplRuntimeState *snapshot);
void repl_state_restore(const ReplRuntimeState *snapshot);
```

A leaf write looks like:

```c
repl_state_help_mut()->visible = 0;
repl_state_help_mut()->scroll  = 0;
```

Not:

```c
repl_state_help_set_visible(0);
repl_state_help_set_scroll(0);
```

Both keep state inside `g_repl_state`, but the second form multiplies the
public API surface by the number of leaf fields, for no extra architectural
guarantee.

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

if (panels_out.cursor_pixel_valid) {
    ReplCodePanelState *cp = repl_state_code_panel_mut();
    cp->cursor_px = panels_out.cursor_px;
    cp->cursor_py = panels_out.cursor_py;
}
```

The controller is the only place that calls `_mut()` to actualize render
output back into runtime state. Renderers never call `_mut()` directly.

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

### check-state-read-getters-return-values (NEW)

The base `repl_state_<slice>(void)` *read* getter must return by value (or a
by-value view struct), not a pointer. Pointer-returning *read* getters are the
structural bug we are removing — they let any caller incidentally write back.

Examples of good read signatures:

```c
ReplHelpState     repl_state_help(void);
ReplDocumentView  repl_state_document(void);
```

Bad read signatures (the check fails on these):

```c
const ReplHelpState *repl_state_help(void);
const ReplHelpState *repl_state_help_ptr(void);
int                 *repl_state_help_visible_ptr(void);
```

`repl_state_<slice>_mut(void)` is **not** a read getter and is **not**
flagged. The naming convention is the contract: anything ending in `_mut`
returns a pointer-into-runtime-state and is the legitimate write path.

This check is a ratchet: count the surviving pointer-returning read getters
in `repl_state.h` / `repl_state_views.h` / `repl_state_owners.h` and fail if
the count goes up. The ratchet retires once the count hits zero.

### check-ui-renderer-takes-view (NEW)

Keep the renderer-signature rule:

```c
void ui_<panel>_render(const Ui<Panel>View *in);
void ui_<panel>_render(const Ui<Panel>View *in, Ui<Panel>Output *out);
```

Anything else means the renderer is still reaching around the snapshot boundary.

### check-renderer-no-direct-mutators (NEW)

Migrated renderers may not call:

- `repl_state_*_mut()` (writes back into runtime state)
- `repl_action_*` (input-side action helpers)
- per-domain mutating helpers such as `repl_state_status_set` /
  `repl_state_camera_set_*` / `repl_state_workspace_set_dir`

They may only read `in` and write to their `out` parameter. `_mut()` is
fine in the controller and in input-handling modules; it is not fine in
`ui_*` renderer code paths.

### check-output-actualization (NEW)

If a renderer fills a `Ui*Output` field, `imrepl_ctrl.c` must actualize it back into runtime state or consume it immediately. This prevents render-discovered state from being silently dropped.

### check-mut-accessor-count — REMOVED

Earlier drafts of this plan ratcheted `_mut()` usage to zero on the
assumption that named per-field setters would replace it. That assumption
is gone. `_mut()` is the canonical write path; there is nothing to ratchet.

What is still ratcheted is **pointer-returning *read* getters** (see
`check-state-read-getters-return-values`). That is the actual structural
defect: a writable result that pretends to be a read.

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
	check-state-read-getters-return-values \
	check-ui-renderer-takes-view \
	check-renderer-no-direct-mutators \
	check-output-actualization \
	check-runtime-capture-roundtrip

test: ... check-state-ownership
```

## Stages

Each stage is reviewable on its own. The plan is intentionally incremental and does not require moving all storage out of `repl_state`.

### Stage Bookkeeping

When a migration commit touches a slice, state which mode that slice is in
after the commit:

- `facade-backed`: storage is in `g_repl_state`, but public readers still go
  through `ReplRuntimeFacade`
- `direct-runtime`: storage is in `g_repl_state`, and `repl_state_*` returns
  `&g_repl_state.<slice>` directly
- `value-getter`: readers use by-value getters or read-only view structs plus
  named mutators

The facade is optional transitional scaffolding, not a goal. If a commit moves
a slice from `facade-backed` to `direct-runtime` or `value-getter`, call that
out explicitly instead of hiding it under a generic "stage 1" label.

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

Reference pilot:

- `b2f649128b9dcaccb86118c6a827f9b812cfc33d`

Scope:

- fold the scattered globals into `static ReplRuntimeState g_repl_state;`
- replace pointer-based leaf storage with nested value structs
- keep existing behavior modules intact
- add `repl_state_capture()` and `repl_state_restore()` entrypoints, even if some call sites still use legacy accessors for a short time

This is the decisive shift: state stays centralized, but it becomes real structured data rather than aliases.

Stage-1 struct inventory:

The Stage-1 storage conversion target is every field inside `ReplRuntimeState`
in `repl_state.h`. Be explicit about these by exact field and type:

- `document`: `ReplDocumentRuntimeState`
- `flat_program`: `ReplFlatProgramRuntimeState`
- `variables`: `ReplVariableRuntimeState`
- `editor_input`: `ReplEditorInputRuntimeState`
- `selection`: `ReplSelectionRuntimeState`
- `clipboard`: `ReplClipboardRuntimeState`
- `code_panel`: `ReplCodePanelRuntimeStorage`
- `help`: `ReplHelpRuntimeState`
- `variable_panel`: `ReplVariablePanelRuntimeState`
- `variable_drag`: `ReplVariableDragRuntimeState`
- `profile_panel`: `ReplProfilePanelRuntimeState`
- `status`: `ReplStatusRuntimeState`
- `search`: `ReplSearchRuntimeState`
- `autocomplete`: `ReplAutocompleteRuntimeState`
- `camera`: `ReplCameraRuntimeState`
- `pointer`: `ReplPointerRuntimeState`
- `viewport`: `ReplViewportRuntimeState`
- `presentation`: `ReplPresentationRuntimeState`
- `render`: `ReplRenderRuntimeState`
- `replay`: `ReplReplayRuntimeStateStore`
- `scenes`: `ReplSceneRuntimeStateStore`
- `import_export`: `ReplImportExportRuntimeState`

Important clarification:

- Some of those runtime types are currently separate struct definitions.
- Some are temporary aliases to `Repl*State` types from `repl_state_views.h`.
- They are still Stage-1-covered runtime storage either way. The alias does not
  exempt the slice from Stage 1; it only means the runtime/public shape is
  currently identical for that slice.

Not Stage 1 by default:

- `ReplRenderDerivedState` in the facade layer, because it is not a field in
  `ReplRuntimeState`
- `UserScene` / `g_user_scenes[]` / `g_active_user_scene` in `repl_scenes.c`
- undo/redo ring state in `repl_undo.c`
- controller/editor/menu/audio/camera-control sidecar globals that are not
  stored in `ReplRuntimeState`

If a commit touches one of those non-`ReplRuntimeState` sidecars, call that out
separately. Do not describe it as pure Stage-1 runtime-storage conversion.

Required invariants for every Stage-1 follow-up commit:

- every touched mutable field still lives in `g_repl_state`
- `repl_state_capture()` / `repl_state_restore()` keep round-tripping every
  touched slice
- `test_repl_state.c` grows whenever a new slice starts relying on real owned
  runtime storage in a way that capture/restore must preserve
- if `ReplRuntimeFacade` still exists for a slice, it is an adapter only and
  never an alternate owner
- if a commit also converts a slice's public shape, readers, or mutator style,
  the commit message and review notes say that it is intentionally consuming
  later-stage work as well

Exit criteria:

- `g_repl_state` is the only owner for the Stage-1-covered slices
- `repl_state_capture()` / `repl_state_restore()` are kept current for those
  slices
- the capture/restore test exercises representative fields from each newly
  covered slice
- any surviving facade fields are compatibility adapters only

### Stage 2 — Pilot value getters on small slices

Convert one low-risk slice end to end as the load-bearing pattern. Follow-up
slices (viewport, status, selection, profile_panel, variable_panel) replicate
that pattern with no new design work.

The shape after this stage:

- the slice's runtime storage stays inside `g_repl_state` (Stage-1 invariant)
- `repl_state_<slice>(void)` returns the slice **by value**
- `repl_state_<slice>_mut(void)` keeps returning `Repl<Slice>State *` for writes
- **no per-field setters are added** — leaf writes go through `_mut()->field`
- existing domain helpers that encode invariants (e.g. `repl_state_status_set`)
  stay; they are not what this stage is removing
- the public `Repl<Slice>State` struct contains only value fields (no `int *`,
  `char *`, etc.), so a by-value getter is meaningful

#### Pilot procedure

1. Pick a slice whose `Repl<Slice>State` is already a pure value struct in
   `repl_state_views.h` (no pointer fields). Stage 1 has already done that
   work for `help`, `viewport`, `status`, `selection`, `variable_panel`,
   `variable_drag`, `profile_panel`, `pointer`, `camera`, `search`,
   `autocomplete`, `clipboard`, `scenes`. These are the candidate set.
2. In all three of `repl_state.h`, `repl_state_views.h`, and
   `repl_state_owners.h`, change the read getter declaration:

   ```c
   /* before */
   const Repl<Slice>State *repl_state_<slice>(void);
   /* after */
   Repl<Slice>State        repl_state_<slice>(void);
   ```

   `repl_state_<slice>_mut(void)` stays exactly as it is.
3. In `repl_state.c`, change the implementation to return the value:

   ```c
   Repl<Slice>State repl_state_<slice>(void) {
       return g_repl_state.<slice>;
   }
   ```
4. Sweep call sites. Two mechanical edits cover everything:
   - `const Repl<Slice>State *foo = repl_state_<slice>();` and uses of
     `foo->field` become `Repl<Slice>State foo = repl_state_<slice>();`
     and `foo.field`.
   - `repl_state_<slice>()->field` becomes `repl_state_<slice>().field`.
   `_mut()` callers do not change.
5. Build (`make sample`, `make sample USE_GL_STUBS=1`).
6. Run the focused tests for the slice plus `make test`.
7. Commit with a `state:` or `feat:` prefix and a Stage-2 label in the
   message body so future agents can find this commit by `git log
   --grep="Stage 2"` and reuse it as a template.

#### Stage 2 reference commit

The first slice converted under this procedure is the **help** slice.

- Commit: `1ea317e7bf36b7b747621b36b7803fc2e9d27845`
- Title: `gold-standard: stage 2 pilot — help slice by-value getter`
- Slice: `help` (`ReplHelpState { int visible; int tab_idx; int scroll; }`)
- Files touched (eight): `repl_state.h`, `repl_state_views.h`,
  `repl_state_owners.h`, `repl_state.c`, `repl_editor.c`,
  `ui_help_overlay.c`, `test_repl_state.c`, `test_repl_editor.c`
- Call-site changes: 13 read-side rewrites (mostly `->` → `.`); zero
  changes to `_mut()` callers; zero new named setters.

Treat this commit as the canonical "what does Stage-2 work look like in
this repo" example for every later slice. The slice was deliberately
chosen as the smallest possible: pure-value struct, three int fields,
twelve read sites, two write sites — small enough to sweep mechanically,
big enough to exercise `repl_state.h` / `repl_state_views.h` /
`repl_state_owners.h` together with both production and test code.

Notes for next slice converters:

- `repl_state_views.h` and `repl_state_owners.h` both declare the
  read getter (the legacy facade + owners split is still in place).
  Change *both* declarations and the one in `repl_state.h`. Forgetting
  one produces a redeclaration conflict the compiler catches loudly.
- The `_mut()` impl in `repl_state.c` already returns
  `&g_repl_state.<slice>`. The matching read getter just needs to drop
  the `&`. No facade plumbing was needed for this slice; that may or
  may not be true for slices whose public struct still has pointer
  fields (those are not yet Stage-2-eligible — they belong to Stage 3
  or later).
- The Stage-1 capture/restore round-trip in `test_repl_state.c` already
  covered the slice; the read-style change required only swapping `->`
  for `.` on the assertions. No new test was needed.
- `test_ui` has a pre-existing Makefile-include bug and was not run as
  part of this pilot. Don't get distracted by it; it is unrelated to
  this work.

Exit criteria for the pilot (all met by `1ea317e`):

- the pilot slice's `repl_state_<slice>(void)` returns by value ✓
- `_mut()` survives unchanged ✓
- no new named per-field setters were added ✓
- `repl_state_capture()` / `repl_state_restore()` round-trip still passes ✓
- both `make sample` and `make sample USE_GL_STUBS=1` are green ✓
- `test_repl_state` (101/101) and `test_repl_editor` (708/708) pass ✓
- the commit message is explicitly Stage-2-labelled ✓

### Stage 3 — Convert UI-facing leaf state

Apply the same pattern to the state that drives UI behavior directly:

- search
- autocomplete
- pointer state
- variable panel visibility
- code panel flags that are read frequently but are still small values

The goal here is to kill the easy pointer-through-view cases without a large structural rewrite.

If a commit in this stage bypasses the facade directly, that is fine, but it
should say so explicitly and should not be described as pure Stage-1 plumbing.

### Stage 4 — Fix the cursor-pixel write with outputs

This is the canonical render-discovered state case.

Changes:

- `ui_panels_render` becomes `void ui_panels_render(const UiPanelsView *in, UiPanelsOutput *out)`
- the renderer fills `out->cursor_px`, `out->cursor_py`, and `out->cursor_pixel_valid`
- `imrepl_ctrl.c` actualizes the result by writing through `repl_state_code_panel_mut()` once per frame
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

- ensure every slice's read path is by-value and every write path goes
  through `_mut()` (or an invariant-bearing domain helper)
- audit the surviving domain helpers and remove any that were leaf-setter
  duplicates of `_mut()->field` writes
- update `ARCHITECTURE.md`, `MODULES.md`, and `CLAUDE.md`
- document snapshot/capture semantics explicitly
- keep any future refactor that moves behavior into new modules separate
  from this state-shape work

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
- whenever a commit moves a new slice into direct owned runtime storage, extend
  the round-trip test in the same commit
- renderer fixture tests using `Ui*View` inputs and `Ui*Output` assertions
- controller ordering tests for cases where one panel's render output feeds another panel's input in the same frame

Final verification:

- `repl_state.c` still exists and owns a single `ReplRuntimeState`
- `ReplRuntimeState` contains no writable pointer fields
- every public `repl_state_<slice>(void)` *read* getter returns by value
  (or a read-only view struct); none returns a pointer
- `_mut()` accessors are the canonical write path and are NOT being
  ratcheted to zero
- `make check-state-ownership` is green
- the cursor-pixel case is routed through `UiPanelsOutput` and controller actualization
- every UI renderer matches one of the two canonical signatures
- `ui_*` files contain no `repl_state_*_mut()` or `repl_state_*_set_*` calls
- `repl_state_capture()` / `repl_state_restore()` round-trip is covered by tests

## Risks and Notes

- **Large snapshots can be expensive if copied blindly.** Use by-value getters for small and medium slices. For very large collections, return a by-value view struct with read-only span fields.
- **Do not reintroduce aliases through convenience helpers.** The temptation will be to add `*_ptr()` or `*_mut()` back once migration pressure appears. That defeats the point.
- **Centralized ownership does not excuse render-time writes.** Keeping state in `repl_state` is compatible with pure rendering only if outputs remain the sole render-to-state path.
- **Capture semantics must be explicit.** Some state is durable and belongs in a snapshot. Some state may be frame-local or derived. The design is better precisely because this boundary becomes visible and testable.
- **Do not let stage labels drift.** If a commit converts public slice APIs or bypasses the facade for a slice, say which later stage that spends. "Stage 1" should not become a bucket for every incremental state cleanup.
- **Avoid API explosion.** Not every leaf value needs its own getter or setter. Prefer one slice-level by-value getter and one slice-level `_mut()` over five tiny getters and five tiny setters. Named setters survive only when they encode an invariant (clamping, ttl, dependent-state invalidation), not when they exist just to wrap `_mut()->field = value`.
- **Do not bundle unrelated behavior refactors into this work.** This plan is about making state ownership and state reads defensible, not about moving every helper to a new file.
- **Tutorial replay and continuation are real design constraints.** The document should keep calling this out so the end state does not optimize only for immediate compile-time cleanup while making future state capture harder.
