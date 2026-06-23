# Rename `src/scene` → `src/renderer`

## Context

The `src/scene` module is the reusable fixed-function-GL **renderer**: it owns
the *frame* (viewport, clear, projection, lighting baseline, grid/axes/backdrop
"studio" decorations, accumulation AA + motion blur, post-process), takes a
geometry callback (`SceneExecuteProgramFn`), and deliberately does **not** own a
camera type, decide *what* geometry exists, or read `ReplState` / `EditorState`
/ `UiState`. (See `src/scene/README.md` — it defines itself as "a scene
renderer … the part of a graphics program that owns the frame.")

`scene` is the weakest name in the tree for two concrete reasons:

1. **It names the one thing the module does not own.** In graphics, "scene" is
   the *contents* — the scene graph, the objects in the world. This module owns
   the framing *around* the contents; the geometry arrives through a callback.
   The module's own docs spend three paragraphs walking the name back.
2. **"scene" is already overloaded in this codebase** with the *user-scene*
   concept — saved program slots: `g_user_scenes[]`, `src/ui/app/scene_tabs.c`,
   `SceneSnapshot`, `src/repl/scene_snapshot.c`, `scene_name`, `scene_idx`,
   `repl_active_user_scene`, the F12 cycle, workspace `.c` files. Renaming the
   renderer frees "scene" to mean exactly one thing for the end user — a saved
   program.

Decision (from the user): rename the renderer module to **`renderer`**. This is
how the README already defines the module, it de-stutters the central
`scene_render_*` family, and it resolves the overload.

This is a **pure rename — no behavior change.** It touches ~100 files and
several hundred identifiers, so it lands on its own branch / PR and is reviewed
as a no-op diff.

## Naming scheme

The non-obvious constraint that drives the scheme: **the `render_*` snake-case
namespace is already taken.** A repo grep finds existing, *unrelated*
`render_*` symbols owned by the controller and the demos —
`render_repl_geometry`, `render_3d_scene_pass`, `render_outlines`,
`render_apply_camera`, `render_display_func`, `render_reshape_func`,
`render_cube`, `render_load_current_sample`, `render_hud`, `render_fade_batches`
— plus the app-layer `GlrRenderState` / `render_state` presentation config. A
blanket `scene_ → render_` would collide with and be confused for those.

So the function prefix is the **agent-noun `renderer_`**, which is collision-free.
The TypeCase `Render*` namespace *is* free (only `Rendering`/`Renders` in
comments and a stray `Renderer`), so types can use the shorter `Render*`.

| Kind | Old | New | Notes |
|---|---|---|---|
| Directory | `src/scene/`, `src/scene/guides/` | `src/renderer/`, `src/renderer/guides/` | `git mv` to preserve history |
| Include path | `"scene/render.h"`, `"scene/guides/..."`, `"scene/themes.h"` | `"renderer/..."` | callers use path-qualified includes off `-Isrc`; sibling bare includes (`"render_types.h"`) are unaffected unless the filename changes |
| Functions (snake) | `scene_*` | `renderer_*` | `render_*` is taken — see above |
| Types (TypeCase) | `Scene*` | `Render*` | TypeCase namespace is free; `SceneRenderConfig` → `RenderConfig` (drops the doubled token) |
| Macros / enum consts | `SCENE_*` | `RENDER_*` | macro namespace is free |
| Header guards | `SCENE_*_H` | `RENDERER_*_H` | |
| Demo binary + dir | `scene_demo`, `tools/scene_demo/` | `renderer_demo`, `tools/renderer_demo/` | the layer-independence proof; see decision D2 |
| Guard targets | `check-pure-scene-no-repl-state`, `check-scene-no-repl-state-mut`, `check-scene-no-upper-layers` | `check-pure-renderer-no-repl-state`, `check-renderer-no-repl-state-mut`, `check-renderer-no-upper-layers` | |
| Tests | `test_scene_render`, `test_scene_guides`, `test_scene_transition`, `test_scene_palette` | `test_renderer_*` | **NOT** `test_ui_scene_tabs` / `test_scene_file_menu` — those are user-scene tests |

### Names that need a deliberate (non-mechanical) choice

A blind `s/scene_/renderer_/` produces stutters and one awkward case. Pick
these by hand during Phase 2:

| Old | Mechanical (avoid) | Suggested |
|---|---|---|
| `scene_render_3d_scene` | `renderer_render_3d_scene` | `renderer_draw_scene` (verb "draw" avoids render-render stutter; the trailing "scene" here is the legitimate sense — the 3D world being drawn) |
| `scene_render_init_gl` | `renderer_render_init_gl` | `renderer_init_gl` |
| `scene_grid_render` | `renderer_grid_render` | `renderer_grid_render` is acceptable, or collapse to `renderer_draw_grid`; **keep the family consistent** with axes/lights/backdrop |
| `scene_axes_render` | — | match the grid choice (`renderer_axes_render` or `renderer_draw_axes`) |
| `scene_lights_render` / `_setup` / `_apply_theme` | — | `renderer_lights_*` |
| `scene_renderer_state_init` | `renderer_renderer_state_init` | `renderer_state_init` (and type `SceneRendererState` → `RendererState`) |

> Recommendation: keep the verb **at the end** as today (`renderer_grid_render`,
> `renderer_axes_render`, `renderer_lights_setup`) for a clean grep and minimal
> churn; reserve a custom name only for `scene_render_3d_scene` →
> `renderer_draw_scene` and the `_init`/`_state` cases above. Lock the final
> spelling in review before running the sweep.

## Scope: what moves vs. what stays

**The single biggest risk is the overload.** The rename is scoped to *renderer*
tokens — those **declared in `src/scene/**`** — and must leave the *user-scene*
tokens untouched. Do the sweep token-by-token from the appendix list, never a
blanket `s/scene/renderer/` over the tree.

**MOVES** (declared in `src/scene/*.h`, `src/scene/guides/*.h`): all
`Scene*` types (`SceneRenderConfig`, `SceneRendererState`, `SceneRgba`,
`SceneFrameRenderContext`, `SceneGuideSnapshot`, `SceneLight`,
`SceneExecuteContext`/`Fn`/`Purpose`, `SceneProjectionDesc`,
`SceneAxesTheme`/`BackdropMode`/`GridTheme`/`GridExtent`/`GridMajor`/
`GridBrightness`/`LightTheme`/`ViewMode`/`WireframeMode`/`AccumEffect`/
`PostFilterMode`/`XformGuideMode`/`FocusVertex`/`OverlayXn`/`ColorToken`/
`TransformGuidePlan`/`XnState`/`XnPhase`/`XnReveal`), all `scene_*` functions
(`scene_render_3d_scene`, `scene_render_init_gl`, `scene_get_active_projection`,
`scene_apply_projection`, `scene_grid_render`/`_reveal`/`_theme_*`,
`scene_axes_render`/`_reveal`, `scene_lights_*`, `scene_backdrop_*`,
`scene_overlay_xn_resolve`, `scene_draw_*`, `scene_geometry_guides_*`,
`scene_transform_guides_*`, `scene_postprocess_filter_*`, `scene_probe_eye_dist`,
`scene_renderer_state_init`, `scene_xn_*`, `scene_rgba`, `scene_clr`, the
`scene_x/y/w/h`/`scene_clr_a` config-rect/clear fields), all `SCENE_*` macros
(`SCENE_CLR_*`, `SCENE_BACKDROP_*`, `SCENE_VIEW_*`, `SCENE_WIREFRAME_*`,
`SCENE_XFORM_GUIDE_*`, `SCENE_XN_*`, `SCENE_POST_FILTER_*`, `SCENE_EXEC_*`,
`SCENE_ACCUM_EFFECT_*`, `SCENE_OCCLUDED_GHOST_*`, the `*_H` guards).

**STAYS** (user-scene concept, declared outside `src/scene/`):
`scene_name`, `scene_name_hint`, `scene_slot`, `scene_idx`, `scene_slug_used`,
`scene_filename_slug_for_slot`, `scene_cfg_clear`/`_reset_all`,
`SceneSnapshot`, `SceneSnapshotCameraMode`, all `scene_snapshot_*`
(`src/repl/scene_snapshot.c`), `scene_tabs*` (`src/ui/app/scene_tabs.c`),
`UserScene`, `g_user_scenes`, `restore_user_scene`, `repl_active_user_scene` /
`repl_*_scene` / `repl_save_workspace` / `glr_scene_load_example`, and the
tests `test_ui_scene_tabs` / `test_scene_file_menu` and the guard
`check-repl-scenes-cfg-clear-paired`.

**Theme constants in `scene/themes.h` that do NOT have the prefix stay as-is**
(`GRID_THEME_*`, `AXES_THEME_*`, `LIGHT_THEME_*`) — only `SCENE_BACKDROP_*`
there moves.

## Phases

Each phase ends green and is a reviewable unit. Land them as a sequence of
commits on one branch (or one squashed PR), since the tree only links after
Phase 3.

### Phase 0 — Branch + baseline

- `git switch -c rename-scene-to-renderer` (we are on `main`; never rename on
  `main`).
- Confirm a clean baseline: `make test`, `make check-state-ownership`,
  `make check-c99`, `make scene_demo && ./scene_demo` smoke. Record that the
  tree is green so the final diff can be asserted behavior-neutral.

### Phase 1 — Move files + fix include paths (no symbol renames yet)

- `git mv src/scene src/renderer` (carries `guides/` along).
- `git mv tools/scene_demo tools/renderer_demo` and the source file
  `scene_demo.c` → `renderer_demo.c` (decision D2 — defer if keeping the demo
  name).
- Update every `#include "scene/..."` → `#include "renderer/..."` across `src`,
  `tools`, `tests`. (Sibling bare includes inside the module — `"render.h"`,
  `"render_types.h"`, `"grid.h"` — are unchanged in this phase.)
- Update Makefile path variables only: `SCENE_SRCS`, `SCENE_HDRS`, `SCENE_OBJS`,
  `SCENE_DEMO_DEP_SRCS`, `SCENE_DEMO_BIN`, the per-test `_OBJS` paths
  (`$(OBJDIR)/src/scene/...`), and the `find src/scene` in
  `scripts/check-scene-no-upper-layers.sh`. (Leave the *names* of vars/targets
  for Phase 3 — this phase is path-only so the diff stays legible.)
- **Verify:** `make gl-repl`, `make scene_demo` (still old target name), `make
  test` all build/link. No symbol has changed yet, so this is purely a path
  move.

### Phase 2 — Rename the renderer's symbols

- Run the token sweep over `src` + `tools` + `tests`, **one token at a time**
  from the appendix, transforming `scene_ → renderer_`, `Scene → Render`,
  `SCENE_ → RENDER_`, `*_H` guards → `RENDERER_*_H`. Use `gsed -i` with
  word-boundaried patterns (`\bSceneRenderConfig\b`, etc.), never a bare
  `s/scene/renderer/g`.
- Apply the hand-picked names from "Names that need a deliberate choice"
  (`scene_render_3d_scene` → `renderer_draw_scene`, `scene_render_init_gl` →
  `renderer_init_gl`, `scene_renderer_state_init` → `renderer_state_init`,
  etc.).
- **Do not touch** the STAYS list. After the sweep, prove it:
  `grep -rnE '\b(SceneSnapshot|scene_snapshot|scene_tabs|scene_name|scene_slot|scene_idx|scene_cfg_)' src tests`
  should still find them, unchanged.
- **Verify:** `make gl-repl`, `make test`, and a clean
  `grep -rnE '\b(scene_[a-z]|Scene[A-Z]|SCENE_)' src/renderer` → **zero hits**
  (no stale renderer-prefix tokens left in the renamed dir).

### Phase 3 — Build system, guards, scripts, demo/test target names

- Rename Makefile vars for clarity (`SCENE_SRCS` → `RENDERER_SRCS`, etc.) and
  the demo/test targets: `scene_demo` → `renderer_demo` in `SCENE_DEMO_BIN`,
  `ROOT_BIN_LINKS`, the `.PHONY`/recipe; `test_scene_render|guides|transition|
  palette` → `test_renderer_*` (the per-test `_OBJS`/`_LDLIBS`/`_RUN` blocks,
  `TEST_BINS`, and the `CORE_TEST_BINS` filter-out list). Rename the test
  source files under `tests/` to match. **Leave** `test_ui_scene_tabs` /
  `test_scene_file_menu` alone.
- Rename guard targets and recipes: `check-pure-scene-no-repl-state` →
  `check-pure-renderer-no-repl-state`, `check-scene-no-repl-state-mut` →
  `check-renderer-no-repl-state-mut`, and the script
  `scripts/check-scene-no-upper-layers.sh` →
  `scripts/check-renderer-no-upper-layers.sh` (target
  `check-renderer-no-upper-layers`). Update the three aggregate lists in the
  Makefile (`check-state-ownership` and the two collected lists around lines
  329 / 1333). **Leave** `check-repl-scenes-cfg-clear-paired`.
- Update `scripts/check-module-prefixes.sh`: the documented mapping comment
  `src/scene -> scene_/Scene` becomes `src/renderer -> renderer_/Render`, and —
  importantly — invert the guard so the *stale* prefix it now denies in
  `src/renderer` is `scene_`/`Scene`/`SCENE_` (catches a future revert/partial
  rename). Mirror the same path/prefix edits in
  `scripts/check-scene-no-upper-layers.sh` (now `-renderer-`),
  `scripts/check-views-by-value-snapshot.sh`,
  `scripts/check-no-facade-include-in-views.sh`,
  `scripts/check-views-flat.sh`, `scripts/check-ui-core-no-upper-layers.sh`, and
  any `src/scene` path in `scripts/allowlists/facade-includes-in-views.txt` /
  `scripts/baselines/views-flat-violations.txt`.
- **Verify:** `make check-state-ownership` (exercises the renamed guards +
  `check-c99` + `check-module-prefixes`), `make test` (renamed test targets run),
  `make renderer_demo && ./renderer_demo` smoke.

### Phase 4 — Documentation

Rename and update — these are the *current-design* docs (historical
`plans/done/*.md` are **not** edited; they are the record of what was true then):

- `src/scene/README.md` → `src/renderer/README.md`: rewrite "scene
  renderer"/`src/scene` references; keep the conceptual "what a scene renderer
  is" framing but anchor it on the `renderer` module name.
- `CLAUDE.md`: the File-Layout table rows for every `src/scene/*` file, the
  `scene_*` prefix bullet under Conventions ("`scene_*` for 3D rendering" →
  `renderer_*`), the rendering-pipeline prose (`scene_render_3d_scene` →
  `renderer_draw_scene`, `SceneRenderConfig` → `RenderConfig`), the
  Accumulation-Motion-Blur section, the `scene_demo` mentions, and the
  `### C99` guard list if it names the scene guards.
- `MODULES.md`: the "layer 4 / scene renderer" ownership map and the `scene_*`
  prefix line.
- `ARCHITECTURE.md`: every `scene_render_3d_scene` / `SceneRenderConfig` /
  `src/scene` reference in the frame-pipeline narrative and the Runtime GL
  Capability / accumulation sections.
- `ADVANCED_USAGE.md`, `src/app/README.md`, `src/support/README.md`,
  `src/subsystems/README.md`: their `src/scene` path references.
- **Verify:** `grep -rnE 'src/scene\b|scene_render_3d_scene|SceneRenderConfig'`
  over tracked, non-`plans/done` files → zero hits.

### Phase 5 — Full verification matrix

- `make test` (debug = ASan+UBSan), `make gl-repl`, `make renderer_demo`,
  `make repl_demo`/`editor_demo` (cross-check unrelated demos still link),
  `make check-state-ownership`, `make check-c99`, `make test-stubs`,
  `make gl-repl USE_GL_STUBS=1`.
- Final stale-token sweeps:
  - `grep -rnE '\b(scene_[a-z]|Scene[A-Z]|SCENE_)' src/renderer` → 0.
  - `grep -rn '"scene/' src tools tests` → 0 (all includes repointed).
  - `grep -rnE '\b(SceneSnapshot|scene_snapshot|scene_tabs|scene_name|scene_cfg_)' src tests`
    → unchanged count vs. Phase 0 (user-scenes untouched).
- **gracemont cross-check** (real GCC, portability — per CLAUDE.md):
  ```bash
  ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git fetch origin rename-scene-to-renderer && git checkout FETCH_HEAD && \
    make check-c99 && make test-stubs'
  ```
- Optional: headless OSMesa smoke (`make gl-repl FREEGLUT_OSMESA=1`) and a
  `scripts/docs-assets.sh --list` dry-run to confirm no asset script hardcodes a
  `src/scene` path.

## Risks & gotchas

- **The overload trap (highest risk).** A blanket `scene → renderer` would
  corrupt the user-scene concept (`SceneSnapshot`, `scene_tabs`, `scene_name`,
  `scene_idx`, `scene_cfg_*`, `repl_*_scene`). The sweep is per-token from the
  appendix, and Phase 2/5 grep-assert the STAYS set is untouched.
- **`render_*` is taken** (controller/demo statics + `GlrRenderState`). Hence
  `renderer_*` for functions. Do not "simplify" to `render_*` later.
- **Standalone demo must still link.** `renderer_demo` (ex-`scene_demo`) is the
  layer-independence proof: it links `src/renderer/` + `src/support/cpuprof.c`
  with a non-REPL geometry callback. If the rename accidentally pulls an
  editor/controller dep into the module, this target stops linking — treat a
  `renderer_demo` link failure as a real boundary regression, not a Makefile
  typo.
- **Guard inversion.** `check-module-prefixes.sh` must be updated to deny the
  *old* prefix in the *new* dir, or a future partial revert sails through.
- **C99 / old-GCC conventions are unaffected** but re-verify: keep header guards
  renamed consistently, no `_Static_assert` introduced, TUs stay non-empty.
  `make check-c99` + gracemont catch regressions.
- **History.** Use `git mv` so blame survives; do the symbol sweep in a separate
  commit from the file move so reviewers can read each.
- **No behavior change.** The whole point is a no-op diff. If any test output
  changes, something other than a rename happened — stop and investigate.

## Alternatives considered (rejected)

The diagnostic throughout: *does the name describe what the module owns
(the framing apparatus), or what it excludes (the geometry/contents)?*

- **`stage` / `studio`** — collision-free prefix, and the README literally
  uses "studio decorations"; the metaphor (lighting rig + backdrop + floor,
  actors handed in) fits the grid/axes/backdrop/lights decorations well. The
  crowding finding (below) genuinely narrows the gap by removing `renderer`'s
  prefix wrinkle. Rejected because it's *metaphorical where `renderer` is
  literal*: `stage_apply_projection` / `stage_postprocess_filter` ask a reader
  to buy the metaphor, whereas `renderer_*` self-documents — and "stage"
  undersells the module's heaviest, central work (projection blend, the
  accumulation AA/blur loop, post-process), which is renderer work, not
  set-dressing. Closest runner-up.
- **`world`** — rejected outright; *worse than the status quo*. "World"
  (world space, game world, the set of objects) names precisely the contents
  the module refuses to own and stores none of. It re-commits the original
  `scene` mistake, and also collides with the world/modelview the module
  doesn't own either.
- **Rename the user-scene concept instead, keep `src/scene`** — fewer source
  files (~32 vs ~43) but rejected on three grounds: (1) it is **not
  behavior-neutral** — the user-scene concept owns the on-disk `@scene-name`
  save directive and workspace headers plus UI labels, so renaming it breaks
  saved files / needs back-compat aliasing and changes user-visible strings,
  whereas the renderer rename touches zero user-facing surface; (2) it is
  **semantically backwards** — a user's saved 3D program is the *more*
  legitimate owner of the word "scene", so this renames the correct use to
  rescue the incorrect one; (3) it does **not solve the stated problem** — the
  renderer would keep the name we diagnosed as wrong.
- **Also renaming the crowded `render_*` namespace** — out of scope, and not a
  defect to fix. The noisy `render_*` symbols are overwhelmingly *file-private
  statics* (`render_3d_scene_pass`, `render_outlines`, `render_cube`, the demo
  `render_*_func` callbacks), which this codebase's convention explicitly
  permits to be neutrally named; the module-prefix guard polices only exported
  symbols. The only header-level token of substance is `render_state_lines`
  (render-state export text), correctly anchored to the well-prefixed
  `GlrRenderState`. The `renderer_*` exported prefix already resolves the one
  real consequence (the module needs a collision-free exported prefix); a
  static-renaming sweep would balloon the blast radius and break the
  no-op-diff review claim. (In-module statics like `render_3d_scene_pass` ride
  along to `renderer_3d_scene_pass` in Phase 2 only because they live in moved
  files — zero extra reach.)

## Decisions for the user (resolve before/at review)

- **D1 — Type prefix.** Recommended `Render*` (TypeCase is free;
  `SceneRenderConfig` → `RenderConfig` drops the doubled token). Alternative:
  uniform `Renderer*` to match the `renderer_` function prefix exactly
  (`RendererConfig`, `RendererRgba`, `RendererState`) at the cost of verbosity
  and a `RendererRenderConfig`-style awkwardness on a couple of names. **Pick one
  before Phase 2.**
- **D2 — Rename the demo?** Recommended `scene_demo` → `renderer_demo` (+
  `tools/renderer_demo/`) for consistency. It is a developer-facing `make`
  target / binary name, not user-facing; the only cost is muscle memory. If we
  keep `scene_demo`, drop its rows from Phases 1/3.
- **D3 — `scene_render_3d_scene` target name.** Recommended `renderer_draw_scene`.
  Confirm vs. `renderer_render_scene` / `renderer_render_frame`.

## Appendix — full token move-list

Source: identifiers declared in `src/scene/*.h` + `src/scene/guides/*.h`. These
are the *only* tokens the sweep rewrites.

**Types (`Scene*` → `Render*` per D1):** `SceneRenderConfig`,
`SceneRendererState`, `SceneRgba`, `SceneFrameRenderContext`,
`SceneGuideSnapshot`, `SceneLight`, `SceneExecuteContext`,
`SceneExecuteProgramFn`, `SceneExecutePurpose`, `SceneFocusVertex`,
`SceneProjectionDesc`, `SceneAccumEffect`, `SceneAxesTheme`,
`SceneBackdropMode`, `SceneColorToken`, `SceneGridBrightness`,
`SceneGridExtent`, `SceneGridMajor`, `SceneGridTheme`, `SceneLightTheme`,
`SceneOverlayXn`, `ScenePostFilterMode`, `SceneTransformGuidePlan`,
`SceneViewMode`, `SceneWireframeMode`, `SceneXformGuideMode`, `SceneXnPhase`,
`SceneXnReveal`, `SceneXnState`.

**Functions (`scene_*` → `renderer_*`):** `scene_render_3d_scene`*,
`scene_render_init_gl`*, `scene_renderer_state_init`*,
`scene_get_active_projection`, `scene_apply_projection`, `scene_probe_eye_dist`,
`scene_grid_render`, `scene_grid_reveal`, `scene_grid_theme_uses_edge_fade`,
`scene_grid_theme_uses_fog`, `scene_axes_render`, `scene_axes_reveal`,
`scene_lights_render`, `scene_lights_setup`, `scene_lights_apply_theme`,
`scene_lights_init_global_ambient`, `scene_light_theme_names`,
`scene_backdrop_render`, `scene_backdrop_setup_lights`,
`scene_overlay_xn_resolve`, `scene_draw_bitmap_text`,
`scene_draw_normal_vector_arrow`, `scene_draw_vertex_label_text`,
`scene_geometry_guides_render_for_cursor`, `scene_transform_guides_prepare`,
`scene_transform_guides_render_if_due`, `scene_postprocess_filter_render`,
`scene_postprocess_filter_reset`, `scene_postprocess_filter_mode_name`,
`scene_xn_init`, `scene_xn_set`, `scene_xn_show`, `scene_xn_tick`,
`scene_xn_opacity`, `scene_rgba`, `scene_clr`.  (`*` = hand-picked target, see
D3 / "deliberate choice" table.)

**Config struct fields (in `SceneRenderConfig`):** `scene_x`, `scene_y`,
`scene_w`, `scene_h`, `scene_clr_a` (rect + clear-alpha) → `renderer_*`.

**Macros / enum constants (`SCENE_*` → `RENDER_*`):** `SCENE_CLR_*` (all
~40 color tokens), `SCENE_BACKDROP_*` (incl. the `_LIST`/`_NAME_ENTRY`/
`_ENUM_ENTRY`/`_COUNT` X-macro helpers), `SCENE_VIEW_*`, `SCENE_WIREFRAME_*`,
`SCENE_XFORM_GUIDE_*`, `SCENE_XN_*`, `SCENE_POST_FILTER_*`, `SCENE_EXEC_*`,
`SCENE_ACCUM_EFFECT_*` (incl. `SCENE_ACCUM_EFFECT_IS_BLUR`),
`SCENE_OCCLUDED_GHOST_*`, `SCENE_BREATH_FREQ`, `SCENE_GLACIAL_TINT_*`.

**Header guards:** `SCENE_RENDER_H`, `SCENE_RENDER_TYPES_H`, `SCENE_AXES_H`,
`SCENE_GRID_H`, `SCENE_LIGHTS_H`, `SCENE_BACKDROP_H`, `SCENE_OVERLAYS_H`,
`SCENE_TRANSITION_H`, `SCENE_THEMES_H`, `SCENE_PALETTE_H`,
`SCENE_OCCLUDED_GHOST_H`, `SCENE_OVERLAY_XN_H`, `SCENE_VIEW_MODE_H`,
`SCENE_POSTPROCESS_FILTER_H`, `SCENE_GEOMETRY_GUIDES_H`,
`SCENE_TRANSFORM_GUIDES_H`, `SCENE_GUIDES_SHARED_H`, `SCENE_XFORM_GUIDE_MODE_H`
→ `RENDERER_*_H`.

**Explicitly NOT in scope (user-scene concept):** `scene_name`,
`scene_name_hint`, `scene_slot`, `scene_idx`, `scene_slug_used`,
`scene_filename_slug_for_slot`, `scene_cfg_clear`, `scene_cfg_reset_all`,
`SceneSnapshot`, `SceneSnapshotCameraMode`, all `scene_snapshot_*`,
all `scene_tabs*`, `UserScene`, `g_user_scenes`, `restore_user_scene`,
`repl_*_scene`, `glr_scene_load_example`; tests `test_ui_scene_tabs`,
`test_scene_file_menu`; guard `check-repl-scenes-cfg-clear-paired`; theme
constants `GRID_THEME_*` / `AXES_THEME_*` / `LIGHT_THEME_*`.
