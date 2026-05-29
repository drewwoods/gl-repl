# Isolation Demos for color_picker / variable_panel / memprof

## Context

The REPL's subsystems (`src/subsystems/*`) and support/UI helpers
(`src/support/*`, `src/ui/support/*`, `src/ui/subsystems/*`,
`src/ui/core/*`) are *meant* to be reusable, feature-scoped modules that
don't drag in the controller/REPL/editor layers. We want three small
standalone GLUT demos to **prove that claim** — each should link only
`{src/subsystems, src/support, src/ui/support, src/ui/subsystems,
src/ui/core}` and **not** `src/ui/app`, `src/app`, `src/repl`, or
`src/editor`. The demos double as a forcing function: wherever a demo
can't be built without a forbidden dependency, that's a real coupling to
sever.

This mirrors the three demos that already exist as isolation proofs:
`scene_demo` (scene/ has no REPL dep), `repl_demo` (REPL pipeline has no
editor/UI dep — reached **zero stub bodies**), and `editor_demo` (editor
data-model has no REPL-controller dep).

**Exploration found a clear isolation gradient:**

| Subsystem | Data/logic layer | UI/render layer |
|---|---|---|
| **memprof** | `src/support/memprof.c` — **already pure** (stdio/time only) | `src/ui/support/memprof.c` takes `UiRenderSnapshot*` (a god-object pulling in editor/repl/app) but reads only 6 scalars; also calls `ui_layout_*` (ui/app) |
| **variable_panel** | `variable_panel_state.c` pure; `variable_panel_drag.c` reaches `repl/eval.h` for value read at drag-begin | renderer takes `UiRenderSnapshot*`, calls `ui_state_viewport()`/`ui_layout_*` (ui/app); hit-kind `UI_HIT_VARIABLE_SLIDER` lives in `ui/app/hit.h` |
| **color_picker** | `color_picker_state.c` reads the REPL document, synthesizes+parses a `glColor` line, and writes back via `editor_commit_apply_external_change` | renderer is nearly pure; only the **test-only** `ui_color_picker_render_swatch` uses `UiTransformer` (`ui/app/editor.h`) |

**Decisions locked with the user:** reuse the existing in-place bridge
idiom (no new `src/sync/` directory); do **full zero-stub decoupling**
enforced by new guards; **phase** the work memprof → variable_panel →
color_picker.

## The "sync" mechanism (reuse, don't invent)

The project already has the decoupling primitive the user called a "sync
system": **controller-installed function-pointer bridges + narrow
per-frame view structs**. Precedents to copy verbatim:

- `ReplHostEffects` (`src/repl/core.h`) — host effects via installed
  callbacks; unset hook = silent no-op.
- `ReplExportCameraBridge` / `ProjectionBridge` / `ConfigBridge`
  (`src/repl/export.h`) — `repl_export_install_*()`.
- `SceneExecuteProgramFn` + `SceneRenderConfig` (`src/scene/render_types.h`)
  — controller projects state into a narrow read-only config the renderer
  consumes.
- `EditorCompletionProvider` (`src/editor/completion.h`).

The pattern for each subsystem: (1) a **narrow view struct** the renderer
reads (the 2D analog of `SceneRenderConfig`, replacing `UiRenderSnapshot`);
(2) an **installed bridge** for the writeback/value path. The full app
installs real bridges (in `src/app/glr_ctrl.c` / `glr_actions.c`); the demo
leaves them unset or installs an in-memory stub. "Zero stubs" means **zero
stub function bodies** (like `tools/repl_demo/stubs.c`, which is 47 lines of
comments) — unset bridges are silent no-ops.

## Shared groundwork (do once, before phase 1)

1. **Subsystem-owned hit kinds (no shared-file churn).** `ui/core/hit.h`'s
   `int kind` field is the *designed-in* extension seam (`ui/core/hit.h:52`
   documents "int kind to allow enum extension"); do **not** fold the ~14
   app-level feature kinds into core (that would make core a registry of
   every feature — the opposite of what the demos prove). Instead each
   subsystem UI header owns only the kind it emits, as a fixed offset off
   `UI_HIT_CORE_COUNT` in a reserved high range that cannot collide with
   `ui/app/hit.h`'s contiguous app range (`UI_HIT_CORE_COUNT+0..+12`):
   ```c
   // ui/subsystems/variable_panel.h
   enum { UI_HIT_VARIABLE_SLIDER = UI_HIT_CORE_COUNT + 64 };
   // ui/subsystems/color_picker.h
   enum { UI_HIT_COLOR_SWATCH = UI_HIT_CORE_COUNT + 65 };
   ```
   Delete those two enumerators from `ui/app/hit.h` (`UiHit.kind` is `int`,
   so mixing ranges is fine; the controller/panels already include the
   subsystem headers for routing). `ui/core/hit.h` is left untouched.
2. **Hard constraint on every new view struct:** all pointer fields must be
   `const` — `scripts/check-views-flat.sh` scans every `ui_*.h`
   `typedef struct` named `Ui*View`/`*State`/`*Output` and fails on a
   non-`const` pointer. New view type names must end in `View` to satisfy
   `scripts/check-ui-renderer-signatures.sh`. New view headers must **not**
   include `repl/state*.h` (`scripts/check-no-facade-include-in-views.sh`).

## Phase 1 — memprof demo (proof of pattern)

**Decouple shipped source:**
- In `src/ui/support/memprof.h`: drop `#include "ui/app/snapshot.h"`; define
  `UiMemoryPanelView { int window_w, window_h; UiMemoryPanelMode mode;
  int panel_x, panel_y; }` and change `ui_memory_panel_render` to take
  `const UiMemoryPanelView *`.
- In `src/ui/support/memprof.c`: drop `ui/app/layout.h`,
  `ui/subsystems/variable_panel.h`, `ui/app/snapshot.h`. Keep only
  `support/memprof.h`, `ui/core/*`, `config.h`. The panel reads sample data
  from `memprof_*()` and draws at the pre-resolved `panel_x/panel_y`.
- **Move all stacking geometry to the controller.** Both `memprof.c:145`
  *and* `src/ui/support/cpuprof.c:63` compute their panel anchor via
  `ui_variable_panel_rect_for_count(snap, ...)` + `ui_layout_scene_rect()` +
  the `PROFILE_PANEL_W` side-by-side shift + scene clamp. Lift this into
  `src/app/glr_ctrl.c` (caller at `glr_ctrl.c:1363`) so it bakes a resolved
  `(panel_x, panel_y)` into the view. **Migrate cpuprof's anchor the same
  way in this phase** — otherwise narrowing `ui_variable_panel_rect_for_count`
  in phase 2 forces an unplanned cpuprof edit (ordering hazard).

**Demo `tools/memprof_demo/memprof_demo.c`:** GLUT window, a slowly
spinning `glutSolidTeapot` as filler, memory panel overlay. Keys: `a`
malloc+touch a ~4 MB block (append to a list), `f` free the newest, `c`
clear, `q` quit — RSS graph visibly climbs/drops. Each frame:
`memprof_frame_tick()`, fill `UiMemoryPanelView` (full-window anchor,
`mode = MEMORY_PANEL_ON`), `ui_memory_panel_render(&view)`.

**Makefile:** `MEMPROF_DEMO_DEP_SRCS = src/support/memprof.c
src/ui/support/memprof.c src/ui/core/theme.c
tests/gl-stubs/gl_stub_counts.c` + target/phony/`ROOT_BIN_LINKS`/clean,
modeled on the `scene_demo` block (Makefile ~782).

## Phase 2 — variable_panel demo

**Decouple shipped source:**
- **Value-source bridge** in `variable_panel_state.h`:
  `VariablePanelValueSource { int (*count)(void); int (*read_row)(int row,
  char *name_out, int name_cap, float *value_out); }` +
  `variable_panel_install_value_source(...)`. `variable_panel_drag.c`
  replaces its two `repl_eval_predef_view()` reads (drag-begin L44, motion
  fallback L101) with the bridge and **drops `#include "repl/eval.h"`**.
  Writeback already returns a `VariablePanelValueChange` to the caller, so
  the write side is already decoupled.
- **Narrow view** `UiVariablePanelView` (in `ui/subsystems/variable_panel.h`):
  `{ int visible; float replay_lift_px; int window_w, window_h;
  int scene_x, scene_y, scene_w, scene_h; int code_panel_layout_mode;
  const UiVariable *vars; int var_count; int drag_active_var, drag_log_mode; }`.
  Change `ui_variable_panel_render` / `_rect_for_count` / `_hit_test` /
  `_hit_for_count` to take it. Replace `ui_state_viewport()` /
  `ui_layout_scene_rect()` / `ui_layout_code_panel_layout_mode()` with view
  fields (controller bakes them). Move `UiVariable` / `UiVariableList` out of
  `ui/app/snapshot.h` into this header (keep `value` as `const float *`).
  Drop the now-unused `subsystems/replay/replay_state.h` include. Define
  `UI_HIT_VARIABLE_SLIDER` locally in this header (off `UI_HIT_CORE_COUNT`,
  per Shared groundwork) and drop the `ui/app/hit.h` + `ui/app/snapshot.h`
  includes.
- Update callers: `src/app/glr_ctrl.c:1343` (`ui_variable_panel_render`),
  `glr_ctrl.c:2383` and `src/ui/app/panels.c:274` (hit-tests), and
  `cpuprof.c` (already anchor-migrated in phase 1). Give `_rect_for_count` a
  NULL-view fallback or update the ~8 `(NULL, ...)` test call sites.

**Demo `tools/variable_panel_demo/variable_panel_demo.c`:** a `glutSolidTorus`
whose params/transform are driven by 3–4 named floats in a local
`{char name[16]; float value;}` array; demo installs the value-source over
it. Mouse press → `ui_variable_panel_hit_test(&view, ...)` → if
`UI_HIT_VARIABLE_SLIDER`, `variable_panel_handle_drag_begin`; motion →
`_handle_drag_motion` → apply the returned change to the local array;
release → `_handle_drag_reset`. Keys: `v` toggle panel, `q` quit.

**Makefile:** `VARIABLE_PANEL_DEMO_DEP_SRCS =
src/subsystems/variable_panel/variable_panel_state.c
src/subsystems/variable_panel/variable_panel_drag.c
src/ui/subsystems/variable_panel.c src/ui/core/theme.c
tests/gl-stubs/gl_stub_counts.c`.

## Phase 3 — color_picker demo (hardest)

**Decouple shipped source:**
- **Document bridge** in `color_picker_state.h`. The peer doesn't actually
  branch on command *type* — it branches on two derived properties (does the
  command have an alpha channel? and value-max clamping). So no parallel
  `CmdType`-shaped enum is needed: `read_color` returns those two derived
  properties plus the rgba; `write_color` does the string synthesis + parse
  + commit **inside the bridge impl** (host side, which has `cmd_idx` and
  looks up the command shape itself):
  ```c
  typedef struct {
      /* 0 => not editable; else fills rgba + has_alpha + value_max. */
      int (*read_color)(int cmd_idx, float *r, float *g, float *b, float *a,
                        int *has_alpha, float *value_max);
      /* synthesize + parse + commit the new color; capture_undo marks the
       * session's first writeback. Returns 1 if state mutated. */
      int (*write_color)(int cmd_idx, float r, float g, float b, float a,
                         int capture_undo);
  } ColorPickerDocumentBridge;
  void color_picker_install_document_bridge(const ColorPickerDocumentBridge *b);
  ```
  This lets the peer drop `editor/commit.h`, `repl/parser.h`,
  `repl/compile.h`, `repl/core.h`, `repl/command.h`, `repl/state_views.h`
  and the `MAX_LINE_LEN`/`CMD_*` uses. `repl_set_status` folds into the
  host's `write_color`. `CP_CLEAR_MAX_V`/`value_max` clamping stays
  peer-local (`config.h` is universally includable). The full-app bridge
  impl (essentially today's `color_picker_write_cmd` body) lives in a new
  `src/app/glr_color_picker_bridge.c` installed by the controller; the demo
  installs an in-memory impl.
- **Pass viewport/anchor as params** (not just `_start`): `color_picker_start`,
  `color_picker_handle_press`, and `color_picker_handle_motion` all read
  `ui_state_viewport()` / `ui_layout_code_panel_rect()` today. Add
  `viewport_w/h` (+ code-panel x/w to `_start`) params and remove the
  `ui/app` calls. Updates ripple to ~13 test sites + `glr_ctrl.c:2792`.
- **Move `ui_color_picker_render_swatch` out of the subsystem.** It is the
  *only* `UiTransformer`/`ui/app/editor.h` user in
  `src/ui/subsystems/color_picker.c`, and it is **test-only** (sole caller
  `tests/test_ui.c:293`; the live inline-swatch path is the independent
  `TRANSFORMER_COLOR_PICKER` branch in `repl_code_panel.c:134`). Relocate it
  to `src/ui/app` (e.g. into `numeric_swatch.c`/a small `ui/app` file) and
  point the test at the new home. **The load-bearing change is the header:**
  `src/ui/subsystems/color_picker.h` includes `ui/app/editor.h` (line 15,
  for the `UiTransformer` in `render_swatch`'s signature) and `ui/app/hit.h`
  (line 16) — both must be removed when `render_swatch` leaves; replace the
  latter with the subsystem-owned `UI_HIT_COLOR_SWATCH` + `ui/core/hit.h`.
  The remaining `color_picker.c` (`ui_color_picker_render` + `_hit_test`)
  then needs only `ColorPickerView` + `ui/core`.

**Demo `tools/color_picker_demo/color_picker_demo.c`:** a row of glut solid
shapes (cube/sphere/torus/cone/teapot) in fixed screen columns, each with
its own RGBA in a local array; install the document bridge over it. Click a
column → `color_picker_start(idx, click_y, w, h, panel_x, panel_w)`; drag
sliders → `write_color` recolors that shape live; click outside → close.
Keys: `q` quit.

**Makefile:** `COLOR_PICKER_DEMO_DEP_SRCS =
src/subsystems/color_picker/color_picker_state.c
src/ui/subsystems/color_picker.c src/ui/core/theme.c
tests/gl-stubs/gl_stub_counts.c`.

## Guards (new + edits)

**New (one per demo, wired into `check-state-ownership` + `test-full`):**
`check-<name>-demo-isolation.sh` — assert the demo's `*_DEMO_DEP_SRCS`
contains only allowed dirs (no `src/app`, `src/repl`, `src/editor`,
`src/ui/app`) and, for built binaries, a `nm` negative test (no `repl_`/
`editor_`/`glr_` symbols), mirroring `scene_demo`'s documented
`nm | grep` check and `check-repl-demo-no-editor.sh`.

**Existing guards to verify/adjust (per Plan-agent audit):**
`check-views-flat.sh` (new views — all pointer fields `const`),
`check-ui-renderer-signatures.sh` (view type names end in `View`),
`check-no-facade-include-in-views.sh` (no `repl/state*.h` in new view
headers), `check-color-picker-ui-isolation.sh` (its `render_swatch` branch
becomes dead — clean up), `check-duplicate-api-decls.sh` (each installer
declared in exactly one header; the subsystem-owned hit-kind `enum`s are not
function decls, so they're not tracked). Deleting two enumerators from
`ui/app/hit.h` shifts the auto-increment values of the app-only kinds below
them — verify no code persists hit `kind` values (none does; they're routed
live). `check-c99.sh` auto-globs `tools/**/*.c`, so the new demo drivers are
picked up with no Makefile edit.

## Ordering hazards (call-outs)

1. Phase 1 must migrate **both** memprof and cpuprof anchors to the
   controller, or phase 2's `ui_variable_panel_rect_for_count` signature
   change breaks cpuprof.
2. Phase 2: ~8 tests call `ui_variable_panel_rect_for_count(NULL, ...)` —
   keep a NULL-view fallback or update them.
3. Phase 3: `color_picker_start`/handlers signature change touches ~13 test
   sites + `glr_ctrl.c:2792` in lockstep.

## Critical files

- Decouple: `src/ui/support/memprof.{c,h}`, `src/ui/support/cpuprof.c`,
  `src/subsystems/variable_panel/variable_panel_drag.c`,
  `src/ui/subsystems/variable_panel.{c,h}`,
  `src/subsystems/color_picker/color_picker_state.{c,h}`,
  `src/ui/subsystems/color_picker.{c,h}`, `src/ui/app/hit.h` (remove
  `UI_HIT_VARIABLE_SLIDER` + `UI_HIT_COLOR_SWATCH`),
  `src/ui/app/snapshot.h` (remove `UiVariable*`), `src/app/glr_ctrl.c`
  (view-fill call sites), new `src/app/glr_color_picker_bridge.c`.
  (`src/ui/core/hit.h` is intentionally left unchanged.)
- New demos: `tools/{memprof_demo,variable_panel_demo,color_picker_demo}/*.c`.
- Build/guards: `Makefile`, `scripts/check-*-demo-isolation.sh`.

## Verification

Per phase (debug ASan/UBSan is the test default):
- `make <name>_demo && ./<name>_demo` — interact (memprof: `a`/`f` move the
  RSS graph; variable_panel: drag sliders reshape the torus; color_picker:
  click a shape, drag sliders, it recolors).
- `make <name>_demo USE_GL_STUBS=1` — headless link-check of the isolated
  link set.
- `make check-c99` and the new `make check-<name>-demo-isolation`.
- Regression: `make test_memprof` (phase 1), `make test_ui`
  (phases 2/3 — view signature + swatch move), full `make test`.
- Final: `make test-full` (builds all demos under stubs + runs the gate),
  and the gracemont real-gcc cross-check
  (`make check-c99 && make test-stubs`).
