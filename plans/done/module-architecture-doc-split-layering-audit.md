# Module Architecture Doc Split - Layering Audit

Status: in-review
Date: 2026-06-05

## Intent

Split the current dense root `ARCHITECTURE.md` into module-local
architecture docs, for example:

- `src/repl/ARCHITECTURE.md`
- `src/editor/ARCHITECTURE.md`
- `src/ui/ARCHITECTURE.md`
- `src/scene/ARCHITECTURE.md`
- `src/app/ARCHITECTURE.md`
- `src/subsystems/ARCHITECTURE.md`

The root `ARCHITECTURE.md` should merge with `MODULES.md` into a higher-level
map: ownership roles, frame/data pipeline, and cross-module contracts. The
module-local docs should explain architecture through that module's lens.

This is a good direction, but the doc tiers need to be simplified before the
split grows more pages. Today the tree already has overlapping architecture
material in `ARCHITECTURE.md`, `MODULES.md`, and `src/*/README.md`. The split
should dedupe those layers, not add `src/*/ARCHITECTURE.md` as a fourth live
copy of the same contracts.

Proposed doc shape:

- Root `ARCHITECTURE.md` + `MODULES.md` collapse into one high-level map:
  ownership roles, frame/data pipeline, module index, cross-cutting workflows,
  and links to canonical per-module contracts.
- Each module directory has one living deep doc. Either
  `src/<module>/ARCHITECTURE.md` replaces the current README detail and the
  README becomes a short pointer, or the README remains the module doc and no
  second file is added there. Do not keep two deep docs per module.
- Cross-cutting recipes stay outside any one module doc. Workflows like
  "Adding a New Command" or "Adding a Tutorial Step Kind" span parser/spec,
  executor, help, export, UI/replay annotations, stubs, and tests; they belong
  in the root doc or a `docs/recipes/` area, with module docs linking to them.

The caveat: a module-local doc split should not pretend every dependency is
already one-way. The current tree has no project-local file-level include
cycles, but it does have module-level cycles caused by shared snapshot types,
feature UI, replay annotations, and a few live-state reads.

## Sweep Method

I parsed production project-local quoted includes across:

- `src/**/*.c`, `src/**/*.h`
- `include/*.h` except the vendored `include/miniaudio.h`
- root `gl_repl.c`, `gl_repl.h`, `config.h`, `keymap.h`, `prof_sections.h`,
  `source_document.h`

I excluded tests, tools, vendored sources, and angle includes. This matches the
project's include-style rule: project-local headers use quotes.

I also ran the most relevant existing guards:

```bash
make check-layer-coupling check-scene-no-upper-layers \
  check-ui-core-no-upper-layers check-repl-no-direct-editor \
  check-editor-no-app check-repl-no-app \
  check-replay-ui-isolation check-color-picker-ui-isolation
```

They all passed.

High-level result:

- No file-level include SCCs were found.
- `src/scene` and `src/ui/core` are already well protected by hard include
  guards.
- `src/repl` and `src/editor` have zero app includes under the existing
  ratchets.
- The real cleanup is module-level coupling, not C preprocessor cycles.

Demo constraints to preserve:

- The `tools/*_demo` binaries are layer-independence proofs. They should stay
  mostly `src/app`-free; each demo can have a tiny local shell, but it should
  not import the app composition layer to paper over a module seam.
- `tools/editor_demo` is stricter: it should use `src/editor/state.c`,
  `src/editor/edit_ops.c`, and `src/ui/core` primitives only. It should not
  pull from `src/ui/app`, because `ui/app` is the full-app code-panel/menu/
  snapshot layer, not the reusable editor view proof.

## Findings

### 1. App/UI is the largest real cycle

Evidence:

- `src/app/*` includes UI headers as expected. The controller renders and
  hit-tests through `src/ui`.
- `src/ui/app/snapshot.h` includes `app/glr_state.h` and `app/glr_config.h`.
- `src/ui/app/menu_bar.c` includes `app/glr_actions.h` and
  `app/glr_config.h`, and calls config/catalog APIs while rendering menu rows.

Impact:

The docs currently want `src/app` to be the composition root and mediator, but
`src/ui/app` still reaches back into app-owned config/action vocabulary. That
means a future `src/ui/ARCHITECTURE.md` must either document `ui/app` as
app-coupled feature UI, or the code should be refactored so UI receives a
fully resolved menu/config snapshot from the controller.

Difficulty: medium-high.

Likely cleanup path:

1. Introduce a UI-owned menu/config view model in the frame snapshot:
   labels, row kinds, active values, submenu structure, and stable item ids.
2. Have `glr_ctrl` or `glr_actions` build that view model from
   `glr_config_*`, `repl_example_*`, and `repl_tutorial_*`.
3. Make `ui_menu_bar` render and hit-test the view model only. It should return
   `UiHit` ids; app code should execute the selected action.
4. Replace `snapshot.h`'s direct `GlrRenderState` / `GLR_CONFIG_COUNT`
   dependency with UI-owned snapshot fields or a fixed UI cap asserted by the
   controller.

### 2. Editor/UI is conceptually one-way, but includes are two-way

Evidence:

- `src/editor/state.h` includes `ui/app/editor.h` for live overlay-list types.
- `src/ui/app/snapshot.h` includes `editor/state.h` and
  `editor/help_session.h`.
- `src/ui/app/repl_code_panel.c` includes `editor/state.h`.
- `src/editor/input.c` includes UI layout/state headers for hit geometry.

Impact:

The conceptual model "editor uses UI as its view" is still right, but the
current include direction is not a strict DAG. The UI snapshot carries many
editor-owned types directly, so a `src/ui/ARCHITECTURE.md` cannot honestly say
all UI is editor-independent unless it narrows that claim to `src/ui/core`.

Difficulty: medium-high if fully removed; medium if only clarified in docs.

Likely cleanup path:

1. Keep `src/ui/core` as the hard-pure reusable layer.
2. Move UI-facing editor snapshot structs into `src/ui/app/editor.h` or a
   nearby UI-owned header.
3. Have the controller translate `EditorState` slices into those UI-owned
   view structs.
4. Make UI renderers consume `Ui*` views only, not `Editor*` live types.

This preserves the intended direction: editor owns text behavior; UI owns view
shapes; app/controller copies between them once per frame.

### 3. REPL/replay is cyclic through replay annotations

Evidence:

- Replay depends on REPL, which is expected:
  `src/subsystems/replay/replay*.c` includes `repl/core.h`,
  `repl/pipeline.h`, `repl/state_owners.h`, `repl/eval.h`, and
  `repl/flatten.h`.
- REPL also depends on replay:
  `src/repl/replay_annotations.c` includes `subsystems/replay/replay.h` and
  `subsystems/replay/replay_state.h`.

Impact:

Replay annotations are a replay presentation feature, not core language
pipeline. Keeping them in `src/repl` makes the REPL module doc less clean:
the REPL doc has to explain a peer subsystem's runtime state.

Difficulty: low-medium.

Likely cleanup path:

- Move `replay_annotations.{c,h}` into `src/subsystems/replay/`, or into an
  app/controller-adjacent module if the ownership is "presentation bridge."
- Keep the current neutral output shape (`ReplReplayAnnotationOutput`, or a
  renamed `ReplayAnnotationOutput`) so the controller still publishes virtual
  lines to the editor.
- Update includes, Makefile object lists, and tests.

This is mostly a file move, not an API redesign: no `src/repl` public header
pulls replay, and the full-app consumer is the controller. Moving it would make
the dependency one-way: replay depends on the REPL program model; REPL no
longer depends on replay runtime state.

### 4. REPL/scene has a type-vocabulary cycle, not a live-state cycle

Evidence:

- `src/repl/state_views.h` includes `scene/render_types.h` for `SceneLight`.
- `src/scene/guides/guides_shared.h` includes `repl/command.h` and
  `repl/flatten.h`.
- `src/scene/guides/transform_utils.h` includes `repl/command.h` and applies
  GL transforms from `GLCmd`.

Existing guards already prevent the more serious failure mode:

- `check-scene-no-upper-layers` passes.
- `check-pure-scene-no-repl-state` passes.
- `check-layer-coupling` passes for scene/UI mutual includes.

Impact:

This is not scene reaching into live REPL state. It is shared vocabulary for
render contracts and REPL-aware guide overlays. Still, it complicates a strict
"scene is pure renderer" story.

Difficulty: medium, but lower urgency than app/UI and editor/UI.

Likely cleanup path:

- Move the `SceneLight` contract used by both executor and scene into a smaller
  neutral header, or split `ReplRenderState` so REPL does not include the full
  scene render contract.
- Consider moving REPL-aware guide snapshot/walk helpers out of `src/scene`
  into `src/subsystems/edit_overlays` or another feature-bridge module. Scene
  would then render pre-resolved primitives/configs, while REPL command walking
  lives outside scene.
- Treat `transform_utils.h` as REPL-aware GL walker support, not generic scene
  renderer infrastructure.

### 5. `edit_overlays` is the easiest peer-cycle cleanup

Evidence:

- App includes `subsystems/edit_overlays/edit_overlays.h`, which is expected.
- `src/subsystems/edit_overlays/edit_overlays.c` includes
  `app/glr_config.h` privately for `GLR_VERTEX_LABEL_*`.
- The same file includes `editor/state.h` and calls `editor_state_edit_line()`.
- It also reads live REPL flat-program state while building vertex-walk
  contexts.

Impact:

This creates both `app <-> subsystems` and `editor <-> subsystems` cycles at
module level. It is also inconsistent with the otherwise good public header,
which already tries to avoid pulling app config into the transitive API.

Difficulty: low.

Likely cleanup path:

- Pass label-display policy as booleans or a local overlay enum instead of
  comparing app config enum values inside the subsystem.
- Pass the cursor and `FlatProgramView` into the overlay render functions from
  the controller. `OverlayWalkCtx` already points in this direction.
- Keep all live editor/REPL reads in `glr_ctrl` snapshot construction.

### 6. `ui/app` and `ui/subsystems` are internally cyclic

Evidence:

- `src/ui/app/panels.c` includes `ui/subsystems/color_picker.h` and
  `ui/subsystems/variable_panel.h`.
- `src/ui/subsystems/replay_hud.c` includes `ui/app/layout.h` and
  `ui/app/snapshot.h`.

Impact:

This is mostly an internal organization issue inside `src/ui`. It matters only
if the doc split goes deeper than one `src/ui/ARCHITECTURE.md`.

Difficulty: low.

Options:

- Keep it and document `ui/subsystems` as feature-UI inside the broader UI
  module.
- Or move feature renderers under `src/ui/app/` and reserve
  `src/ui/subsystems/` for views that take narrow feature-owned view structs
  rather than the full app snapshot.

### 7. Root/app is not a meaningful architecture cycle

Evidence:

- `gl_repl.c` includes app headers.
- app files include root `config.h` and `source_document.h`.

Impact:

This is root bootstrap and neutral root contracts, not module behavior
coupling. It should not block the doc split.

Difficulty: low and mostly mechanical if desired:

- Move `gl_repl.c` into `src/app` when the shell rename lands.
- Move neutral root headers into `include/` if the root cleanup becomes a goal.

## Documentation Recommendation

The doc split can start before all cycles are removed, but the docs should use
three hard rules:

1. **Owner contract:** what this module owns and what it may mutate.
2. **Cross-module contracts:** explicit dependencies it consumes or produces.
3. **One canonical home per contract:** document a contract once on the owner /
   producer side and link from consumers. Do not restate the source-document
   port, `UiHit`, `SceneRenderConfig`, `ReplCompiledChange`, editor overlay
   snapshots, or export bridges in every module that touches them.

For example:

- `src/repl/ARCHITECTURE.md` should describe the language pipeline,
  `source_document`, `ReplHostEffects`, export bridges, and what peer features
  consume from REPL. It should not become the replay runtime doc.
- `src/editor/ARCHITECTURE.md` should describe text ownership and commits, plus
  the UI snapshot/hit-test contract it consumes. It should also call out
  `editor_demo` as the guard that the generic editor model uses `src/ui/core`
  but not `src/ui/app` or `src/app`.
- `src/ui/ARCHITECTURE.md` should separate `ui/core` purity from `ui/app`
  app-coupled feature UI, with `editor_demo` as the concrete proof that
  `ui/core` remains independently usable.
- `src/app/ARCHITECTURE.md` should be the composition-root/router doc and own
  the cross-module frame pipeline. It should document that demos generally
  replace this layer with local shells rather than linking app code.
- `src/scene/ARCHITECTURE.md` should emphasize `SceneRenderConfig`, geometry
  callbacks, and the current REPL-aware guide vocabulary as a documented
  exception or cleanup candidate.
- `src/subsystems/ARCHITECTURE.md` should document each peer's state, input
  routing, UI view, and program writeback path.

Root-level material to keep:

- The module index and ownership DAG.
- The frame pipeline from GLUT callback through controller, scene, UI, and
  restore paths.
- Cross-cutting recipes that intentionally span multiple modules, such as
  adding a command, adding a tutorial step kind, adding export scaffolding, or
  adding a new UI interaction.
- A contract index that links to the producer-side canonical section for each
  cross-module contract.

## Suggested Cleanup Order

If the goal is strict one-way module docs, do the two cheap cleanups before
the doc split:

1. Clean `edit_overlays` live reads and app enum dependency.
2. Move `replay_annotations` out of `src/repl`.

Then split the docs while documenting the remaining heavier edges as current
contracts / deferred cleanup:

3. Decide whether `ui/app` is allowed to be app-coupled. If not, introduce a
   controller-built menu/config view model.
4. Replace UI's direct editor type imports with UI-owned snapshot/view types.
5. Optionally split the REPL/scene shared vocabulary so guide overlays stop
   living in `src/scene` as REPL-aware code.

Minimum viable documentation-only path:

- Split the docs now, but explicitly label the app/UI, editor/UI, REPL/replay,
  and REPL/scene edges as current cross-module contracts.
- Add cleanup bullets to the affected module docs so future work does not
  mistake current coupling for the target architecture.
- In every module-local doc that mentions a demo, state the demo dependency
  contract alongside the behavior it exercises. In particular, pin
  `editor_demo` to `src/ui/core` and keep `tools/*_demo` app-shell-free by
  default.

Guard idea once the split exists:

- Add a small check that every intended module doc exists and that the root map
  links to it. Keep it structural only: it should prevent orphan docs, not try
  to lint prose.
