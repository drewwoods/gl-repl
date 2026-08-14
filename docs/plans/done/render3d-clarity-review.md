# `src/render3d` Clarity, Consistency & Extensibility Review

## Status - IMPLEMENTED / DONE (2026-08-14)

A read-only review of `src/render3d` (11 `.c` + 21 `.h`, ~10,800 lines)
against the questions in the request: module boundaries, naming, API
consistency, data ownership, render-flow readability, extensibility of
the next overlay / guide / backdrop / theme / projection / post-effect /
pass, implementation-pattern consistency, readability, duplication, header
hygiene, and OpenGL state conventions.

The implementation and follow-up fixes are now landed. Every finding below
retains its original file:line citation and rationale so the completed work
can be re-checked against the design decisions that motivated it.

This is not a re-run of `plans/done/src-scene-code-smell-audit.md` (2026-05).
That pass closed the real bugs (mutable globals, god-functions, projection
split, camera leaving the renderer). What follows is the residue after those
fixes, plus drift that has accumulated since the `scene` → `render3d` rename.

## Verdict first

**The subsystem is in good shape.** The layering contract holds, the frame
pipeline is named and ordered, and most of what looks like a problem is a
documented, defended decision. This review deliberately did *not* manufacture
findings for the following, all of which were examined and found sound:

- **Config-in / callback-for-geometry.** `render3d_draw_scene()` consumes an
  explicit `Render3dRenderConfig`. User geometry, replay fades, edit overlays,
  and buffer visualization enter through hooks. Neutral helpers
  (`grid` / `axes` / `backdrop` / `lights`) never read REPL, editor, or UI
  state. `make render3d-demo` is a real, load-bearing proof; the guards
  `check-render3d-no-upper-layers` and `check-render3d-no-repl-state-mut`
  keep it that way.
- **Caller-owned camera and caller-owned clear.** The renderer never owns a
  camera type and never clears the color/depth buffers on the program's
  behalf. That split is written on `render.h:35-42` and
  `render.c:491-498` and is the reason `render3d_demo` can inline six matrix
  calls with no `glr_camera.h` dependency.
- **Two background colors.** `baseline_clear_color` vs `presentation_rgba`
  (`render_types.h:210-225`) is the right distinction; collapsing them would
  reintroduce the scene-switch clear bug the architecture already closed.
- **Named pass split** in `render.c` (`setup` / `fill` / `helpers` /
  `overlays`) plus the stencil-policy comment at `render.c:851-864`. Host
  chrome and geometry-reporting overlays are different categories and the
  split already matches the two policies. Do not invent a pass-plugin table.
- **The hook surface on `Render3dRenderConfig`.** `post_fill_fn`,
  `post_overlays_fn`, `post_resolve_overlays_fn`, the three buffer-inspection
  slots, and `setup_subframe_fn` are how replay, edit overlays, and
  `buffer_viz` stay out of the renderer. Adding another *caller-supplied*
  pass is another hook, not a new framework.
- **`Render3dState` as caller-owned instance state.** Ortho-ref + active
  projection live on a struct the embedding owns. That is why hot-reload
  works and why two viewports would not share a file-static projection.
- **The transition machine** (`render3d_transition.c` / `.h`). Pure clock,
  reveal plugin, no GL. Grid and axes bind different curves. Leave it.
- **`overlay_xn.h`.** One pure resolver, two style macros. Grid and axes
  already share it.
- **`palette.h` three-bucket policy** and **`occluded_ghost.h` as values,
  not a mechanism.** Tokens for one-off draw colors; theme tables stay
  local; ghost stipple/alpha are constants each helper applies itself.
  Do not wrap the two-pass ghost loop.
- **`render3d_hash.h` staying independent of `repl_randf`.** The comment
  at `render3d_hash.h:19-29` is the contract; do not unify them.
- **Post-process scratch textures as file statics.** `glr_compositor.c:12-16`
  documents the mutual-exclusion with the 3D-viewport pass. Moving the
  cache onto `Render3dState` would break that reuse or force the compositor
  to borrow renderer state it should not know about.
- **`grid.c` / `backdrop.c` / `transform_guides.c` size.** 2,477 / 1,338 /
  1,306 lines. Each file is one coherent job (every grid theme, every
  backdrop, every transform-guide shape). A verb-boundary split would
  force a private header, Makefile/guard churn, and a header web for
  `GridDrawContext` / `CityBoxCorners`. Same deferral as `compile.c` in
  `plans/partial/src-repl-simplicity-review.md`. Split only when a feature
  forces a new file.

What follows is the residue: places where the design has drifted from its
own stated intent, or where the next feature will cost more than it should.

---

## How the pipeline actually runs

`render3d_draw_scene()` (`render.c:879-1010`):

1. Validate config, set viewport, establish baseline clear color.
2. Refresh the ortho scale reference (optional `GL_FEEDBACK` probe).
3. Compute the canonical zero-jitter `Render3dProjectionDesc` into
   caller-owned `Render3dState`.
4. Either one pass, or N accumulation samples (AA jitter *or* blur
   subframe hook, never both).
5. Per pass (`render3d_scene_pass`, `render.c:830-877`):
   - `render3d_pass_setup` — projection (jittered), lights + backdrop
     env lights, lighting disabled as the user-geometry baseline,
     quality/wireframe flags. Outer `glPushAttrib(GL_ALL_ATTRIB_BITS)`.
   - `render3d_pass_fill` — user geometry (or hidden-line / winding
     substitute), then `post_fill_fn` (replay fades).
   - `buffer_read_fn` — geometry only; helpers have not written depth.
   - `GL_STENCIL_TEST` suspended around **host chrome**.
   - `render3d_pass_helpers` — backdrop → grid → axes → orbit target →
     light indicators.
   - `buffer_pass_overlay_fn` — sparse composites under outlines.
   - `render3d_pass_overlays` — `post_overlays_fn`, then the matching
     `glPopAttrib`.
6. Once, on the resolved image: re-apply the zero-jitter projection,
   `post_resolve_overlays_fn` (bitmap labels),
   `buffer_resolve_overlay_fn`, then the scene-rect post filter.

The caller populates `GL_MODELVIEW` *before* the call. Guides are **not**
in this list: the controller builds a `Render3dGuideSnapshot` and the
`edit_overlays` peer invokes `render3d_geometry_guides_*` /
`render3d_transform_guides_*` from `post_overlays_fn`.

---

## Extensibility as the code exists today

| Adding… | What you touch | What you should not invent |
|---|---|---|
| **Host-chrome overlay** (orbit-gizmo class) | New helper + one call in `render3d_pass_helpers`. Stencil-off is already around that whole pass. | A decorator registry. |
| **Geometry-reporting overlay** (outlines, vertex points) | Install `post_overlays_fn` or `post_resolve_overlays_fn`. Bitmap text that would ghost under AA goes on the resolve hook. | Anything inside `render.c`. |
| **Guide** | Grow `Render3dGuideSnapshot`, fill it in `glr_ctrl_build_guide_snapshot`, draw from `geometry_guides.c` or a sibling. Pre-evaluated cursor args (vertex / normal / clip / xform) are the pattern to copy. | Teaching a new guide to walk `GLCmd[]` if the controller can pre-resolve the numbers. |
| **Backdrop** | One `RENDER3D_BACKDROP_LIST` row, a `draw_*`, a `render3d_backdrop_render` case, and optionally a `BackdropEnvLight` table + `setup_lights` case. | A backdrop plugin vtable. The switch is the extension point. |
| **Grid / axes / light theme** | X-macro in `themes.h` (cfg symbols follow). Then either a `GridThemeSpec` / `AxesThemeSpec` row **or** a custom function + dispatch case, plus the membership predicates. This is the expensive one — Finding 3. | A theme-object framework. |
| **Projection / view mode** | `projection_mix` is a 1-D blend between perspective and a top-down ortho that matches the same FOV. `Render3dViewMode` (2D/3D) and `Render3dProjectionMode` (PERSPECTIVE/ORTHO) are the discrete faces of that same axis. A third mode (isometric, side ortho) does not fit the mix. | Do not add a fourth enum. Document the limit; invent a new mix only when a real third mode is requested. |
| **Post-process effect** | `Render3dPostFilterMode` + a `postprocess_filter_render_*` arm. Warp geometry already lives in `postprocess_surface` (RIPPLE is implemented and unwired). App cfg currently forces a *second* enum — Finding 4. | Shaders / FBOs. The module is fixed-function on purpose. |
| **Render pass** | A named `render3d_pass_*` and one call in `render3d_scene_pass`, or a hook if the body belongs to a caller. Hidden-line and winding are the in-renderer precedents. | A pass graph. |

---

## Findings

### 1. Pipeline comments and `ARCHITECTURE.md` describe a renderer the code is no longer

**Priority: high**

**Where.**
`docs/ARCHITECTURE.md:212-233` (frame mermaid), `:575-608` (config
contents and render3d responsibilities); `src/render3d/backdrop.h:14-16`;
`src/render3d/render.h:6-9` (one-pass / clear ownership summary);
`src/render3d/render.c:524` (`g_cam_motion_glow`);
`src/render3d/occluded_ghost.h:12` (`g_guide_alpha_mul`);
`src/render3d/README.md:11-15`, `:124-130`, and its file map (generic
camera/clear ownership stated as this renderer's contract, one call claimed
per accumulation sample, and missing `projection_mode.h`, `view_mode.h`,
`overlay_xn.h`, `render3d_hash.h`).

**What is unclear.** The live code and the written pipeline disagree in
ways that will send the next edit to the wrong place:

- The mermaid still has the renderer applying the camera, then drawing
  overlays, then light indicators. The code applies no camera
  (`render.h:35-42`); lights are last of **helpers**, before
  `post_overlays_fn` (`render.c:783-807`).
- The config section still says `Render3dRenderConfig` carries
  `FlatProgramView`, overlay toggles, replay/HUD layout, cursor-block
  metadata, and a `Render3dGuideSnapshot`. None of those fields exist
  on the struct (`render_types.h:139-348`). Guides are a separate
  snapshot; replay is `post_fill_fn`.
- Responsibilities still list "camera transform" as a render3d job.
- `render.h` says one call renders "one pass" and that render3d owns clear.
  One call owns the entire internal accumulation-sample loop, while
  color/depth clearing belongs to the caller / user program (render3d owns
  only its accumulation scratch clear and the baseline clear-color state).
- The module README says the controller calls `render3d_draw_scene()` once
  per accumulation sample. The controller calls it once per frame; the
  renderer loops over the samples internally. Its introductory definition
  and file map also attribute camera positioning and an unqualified "clear"
  to this renderer despite the caller-owned contract described later.
- `backdrop.h` says the backdrop runs "before grid, user geometry, and
  overlays." It runs *after* user geometry, first of the helper pass,
  so antialiased edges blend against the final background
  (`render.c:780-782`).
- Two leftover global names (`g_cam_motion_glow`, `g_guide_alpha_mul`)
  describe state that was lifted onto the config / a parameter in the
  2026-05 audit.

**Why it matters.** The 2026-05 audit's whole point was to make the
renderer a renderer. The code landed; the narrative did not fully
follow. A new overlay, pass, or "why isn't the backdrop behind the
teapot?" debug session will be steered by the mermaid and the header
comment, not by `render_3d_scene_pass`.

**Concrete improvement.** Rewrite the mermaid and the "Render3d Render
Config" / "Render3d Layer" paragraphs to match the list under **How the
pipeline actually runs** above. Fix the stale `render.h`, `backdrop.h`,
global-name, and style-token comments. Correct the README's ownership,
call-frequency, and file-map text, including the missing files. Do not
change code.

---

### 2. `Render3dFrameRenderContext` was built to carry derived pose; helpers still recompute it

**Priority: high**

**Where.**
`render_types.h:350-356` (the wrapper is `config` and nothing else);
`render.c:484-487` (`render3d_prepare_frame_context` is a struct copy);
`lights.c:227-256` (`render3d_lights_camera_world_pos` /
`render3d_lights_eye_dir_to_world`, documented as matching
`glr_camera_load_modelview`); `grid.c:883-886`
(`grid_camera_world_y`, Y-only, same trig);
`docs/ARCHITECTURE.md:590-592` (already names "camera world height" as
the example derived field).

**What is unclear.** The type's own comment says helper renderers should
consume derived state from the frame context "instead of recomputing
from globals" and that new derived fields can land here without changing
every helper's parameter list. After the audit, the globals are gone —
but the derived fields never arrived. Every helper that needs camera
height or world position re-derives it from `cam_rx` / `cam_ry` /
`cam_dist` / `cam_t*`, independently, and the lights path has to stay
bit-identical to a function in `src/app/glr_camera.c` that render3d
must not include.

**Why it matters.** A camera-model change (a roll term, a different
orbit convention, a second viewport with a different pose) has to be
replicated in at least two render3d files plus `glr_camera.c`. Ocean /
Frozen already branch on "is the eye below Y=0"; that number is
exactly the example ARCHITECTURE already gave this struct. The
abstraction exists; it is empty.

**Concrete improvement.** Compute a small pose bundle once in
`render3d_prepare_frame_context`: world-space eye position plus an
eye-to-world 3x3 orientation basis (or an equivalent set of precomputed
camera basis vectors). Put it on `Render3dFrameRenderContext`. The value
`grid_camera_world_y` needs is simply the Y component of that eye position;
the basis lets `lights.c` transform eye-space offsets and aim directions
without re-deriving the inverse rotation. Point `lights.c` and `grid.c` at
those fields. Keep the math in render3d (from the config's `cam_*` fields),
not a `glr_camera.h` include, and extend the config and derived bundle
together if the camera model later gains another term such as roll. This is
derived pose data, not a new caller-owned camera type.

---

### 3. A new grid theme has to be taught to four lists, or it silently fades wrong

**Priority: high**

**Where.**
`themes.h:16-40` (`GRID_THEME_LIST`); `grid.c:75-81` (`g_grid_reveal`);
`grid.c:842-872` (`g_grid_theme_specs` + `grid_theme_spec`);
`grid.c:1700-1702` (`render3d_grid_theme_uses_fog` — currently OCEAN
only); `grid.c:2201-2214` (`render3d_grid_theme_uses_edge_fade` —
spec-table membership plus a two-name carve-in);
`grid.c:2351-2417` (`grid_dispatch_theme` — custom cases vs `default`
spec path); `grid.c:2461-2463` (NV-fog membership hard-coded as
OCEAN || RADAR, while OCEAN's apply is `#if 0`'d just above).

Axes are in better shape: `g_axes_theme_specs` plus a switch whose
custom cases are named (`axes.c:65-70`, `:753-765`). Backdrops are a
single X-macro + two switches (draw / env lights). Lights are one
table keyed by `Render3dLightTheme`.

**What is limiting.** Line-grid themes go through `GridThemeSpec`.
Environment themes (Ocean, Frozen, Soil, Radar, Sketch, Neon,
Graph Planes, Adaptive Planes, Star Chart) each get a function and a
`case`. That two-path split is fine — a table cannot express an
underwater volume. The problem is the *membership* of "how does this
theme fade / fog / reveal" living in three other hand-edited sets.
Omitting a new line theme from `uses_edge_fade` (or adding an
environment theme that accidentally gets a spec row) changes the
hide animation and the FAR-extent fog, with no compile failure.

`uses_edge_fade` already *almost* reads the spec table, then special-
cases XZ Ruler and Star Chart because those are custom-path grids that
still emit through `draw_grid_line_pair`. That is the smell: the
predicate is reconstructing a fact the spec / the renderer already
knows.

**Why it matters.** Grid themes are the most frequently added visual
in this module. The next one will be added by copying a neighbour.
Copying Ember (table) vs copying Ocean (custom) vs copying Star Chart
(custom + edge-fade carve-in) are three different checklists. The
ARCHITECTURE page at `:637-724` already has to explain this. That is
the tax.

**Concrete improvement.** Put the fade/fog/reveal facts on
`GridThemeSpec` (or a parallel `GRID_THEME_COUNT`-sized trait table
that custom themes also fill). Drive `uses_fog`, `uses_edge_fade`,
NV-fog opt-in, and `g_grid_reveal` off that table. Keep the draw
dispatch as a switch — that is the right shape for "this theme is a
whole other renderer." Adding a line theme becomes: X-macro + one
spec row. Adding an environment theme becomes: X-macro + function +
dispatch case + one trait row. No new framework; one table the
predicates already want to be.

While there, delete or finish the `#if 0` Ocean NV-fog block
(`grid.c:2358-2368`). Today OCEAN is in `set_nv_fog` so the mode is
saved/restored, then never written.

---

### 4. App Post-FX vocabulary lives in a render3d header that `render_types.h` includes

**Priority: medium**

**Where.**
`postprocess_filter.h:19-54` (`Render3dPostFilterMode` *and*
`GlrPostFxScope` / `GlrPostFxEffect`); `render_types.h:13`
(`#include "postprocess_filter.h"` solely for the mode enum).

**What is inconsistent.** The renderer needs one mode enum to dispatch
`render3d_postprocess_filter_render`. The application needs a two-row
cfg control (scope × effect) whose symbols are `GLR_*`. Both currently
live in the renderer header. Because `render_types.h` includes that
header, every render3d consumer — including `render3d_demo` and any
test that builds a `Render3dRenderConfig` — sees `GlrPostFxScope`.

Adding an effect is therefore two enums, one X-macro mapping, the
filter switch, and the cfg row. The RIPPLE surface
(`postprocess_surface.h:34`) is already written and unused; wiring it
still has to touch the app-typed half of this header.

**Why it matters.** This is the same class of leak as `GLR_ORTHO_REF_*`
in `render.h:70-78`: app prefix in the renderer, left over from the
rename, now in the include graph of the central config type.

**Concrete improvement.** Move `Render3dPostFilterMode` into a tiny
header (or into `themes.h`, which is already the shared vocabulary
file) and include *that* from `render_types.h`. Keep
`render3d_postprocess_filter_render` / `_reset` / `_mode_name` and the
2D bracket in `postprocess_filter.h`. Move `GlrPostFxScope` /
`GlrPostFxEffect` next to the other cfg enums (or into
`src/app/glr_config.h`). One mapping table at the controller, which is
where scope becomes "3D rect vs whole window" anyway
(`glr_compositor.c`).

`render3d_post_2d_begin` / `_end` staying in the filter header is
fine: `buffer_viz` already includes it as a GL bracket, and
render3d must not depend on `ui/gl_2d.h`. Do not move the bracket
into `render_types.h`.

---

### 5. The public config still carries fields no renderer reads

**Priority: medium**

**Where.**
`render_types.h:280` (`user_lighting_enabled`); `:327-334`
(`grid_theme` / `axes_theme` as `int`, plus `grid_xn_phase` /
`axes_xn_phase` marked RESERVED); `:338-340` (`focus`, "no renderer
reads it yet"); `guides_shared.h:53` (the same
`user_lighting_enabled` copied onto the guide snapshot);
`glr_ctrl.c:571`, `:1729` (controller still fills both).

**What is unclear.** After the 2026-05 audit removed the
"re-enable lighting before `glPopAttrib`" restores, no render3d `.c`
reads `user_lighting_enabled`. The guide snapshot copies it and also
never reads it. `focus` and the two `*_xn_phase` fields are
explicitly reserved so tests can assert the controller forwarded
them. `grid_theme` / `axes_theme` are still `int` while
`backdrop_mode`, `wireframe`, and `post_filter_mode` are typed
enums — leftover from audit #18's partial sweep.

**Why it matters.** A new reader of `Render3dRenderConfig` has to
treat every field as load-bearing. Reserved-for-tests fields are
honestly commented; the lighting flag is not. Anyone adding a helper
that "should restore lighting" will reach for it and reintroduce the
restore-before-pop the audit deleted. Typed vs `int` theme fields
make the next theme addition guess which style to copy.

**Concrete improvement.** Drop `user_lighting_enabled` from
`Render3dRenderConfig` and `Render3dGuideSnapshot`. Leave it on the
flat program, where flatten still computes it for whoever actually
needs the fact. Keep the reserved `focus` / `*_xn_phase` fields
only if the forwarding tests are still worth the struct noise;
otherwise assert the transition machine state directly in
`test_glr_ctrl.c` and delete them. Type `grid_theme` / `axes_theme`
as `Render3dGridTheme` / `Render3dAxesTheme`.

---

### 6. Naming still mixes three rename-eras, so a new enum has no single convention

**Priority: medium** (convention only — do not mass-rename)

**Where.** Side by side in the public headers:

| Symbol | Prefix | File |
|---|---|---|
| `Render3dGridTheme` / `GRID_THEME_*` | type yes, enumerators no | `themes.h` |
| `Render3dBackdropMode` / `RENDER3D_BACKDROP_*` | both | `themes.h` |
| `Render3dWireframeMode` / `WIREFRAME_*` | type yes, enumerators no | `render_types.h:84-94` |
| `Render3dProjectionMode` / `PROJ_*` | type yes, enumerators no | `projection_mode.h` |
| `Render3dViewMode` / `RENDER3D_VIEW_*` | both | `view_mode.h` |
| `Render3dAccumEffect` / `RENDER3D_ACCUM_EFFECT_*` | both | `render_types.h:104-109` |
| `GLR_ORTHO_REF_*` | app | `render.h:75-78` |
| `MAX_LIGHTS` | unprefixed | `render_types.h:39` |
| `g_scene_palette` | old module name | `palette.h:118` |
| `render_3d_scene_pass` | leftover `scene` verb | `render.c:830` |
| `GRID_AXES_XN_*` | unprefixed | `overlay_xn.h:20-21` |

`view_mode.h` is never included by any render3d `.c`. The renderer
reads `projection_mix`; callers map `Render3dViewMode` onto that mix.
`Render3dProjectionMode` is the discrete snap of the same axis for
export. Three names, one idea — fine once written down, confusing
when adding a fourth.

**Why it matters.** Not a runtime bug. The next enum (another
wireframe mode, another projection snap, another overlay style) will
copy a neighbour, and the three neighbours disagree. The
`scene` → `render3d` rename landed the *types* and left a trail of
enumerators and one function.

**Concrete improvement.** Add a six-line "new symbol" note at the top
of `themes.h` and `render.h`: new enumerators are `RENDER3D_*`; do
not introduce another `GLR_*` in this directory; do not rename the
existing `GRID_THEME_*` / `WIREFRAME_*` / `PROJ_*` sets (they are the
cfg X-macro source and a mass-rename is golden-file churn for no
behavior). Rename only the cheap leftovers when next in the file:
`render_3d_scene_pass` → `render3d_scene_pass`, `g_scene_palette` →
`g_render3d_palette`, `GLR_ORTHO_REF_*` → `RENDER3D_ORTHO_REF_*`.
In `render.h`, one comment: `Render3dViewMode` is the caller's
discrete 2D/3D request; `projection_mix` is the renderer's
continuous blend; `Render3dProjectionDesc.projection` is the snapped
mode export reads.

---

### 7. New guides should take pre-resolved numbers, not another walk of `GLCmd[]`

**Priority: medium** (growth rule, not a rewrite)

**Where.**
`guides_shared.h:14-16` (includes `repl/command.h` and
`repl/flatten.h` — allowed by `check-render3d-no-upper-layers`);
`guides_shared.h:44-147` (the snapshot is the live input buffer +
source cmds + `FlatProgramView` + five bags of pre-parsed cursor
args); `transform_guides.c:200-245` (`compute_before_cursor_matrix`
/ `compute_after_cursor_origin` push an identity modelview and
replay transforms through `apply_tracked_transform`);
`transform_guides.c:1060-1193` (`prepare` walks the flat program
for src-line matching).

**What is limiting.** The *vertex / normal / clip-plane / live-xform*
bags are the right shape: the controller evaluates, the renderer
draws. Transform-guide *placement* still walks the program inside
render3d and mutates the GL matrix stack to do it. That walk is why
the snapshot must carry `GLCmd[]` and `FlatProgramView`, and why
`render3d_demo` cannot link the guides objects without the REPL.

This is a documented exception, not an accident. The 2026-05 audit
deferred a CPU-math rewrite of `compute_before_cursor_matrix`
(#25) because it would dwarf the current GL-stack scaffold.

**Why it matters.** A new guide that copies `draw_clip_plane_guide`
(pre-parsed args) stays cheap. A new guide that copies
`transform_guides_prepare` (another `repl_cmd_is_*` walk) grows the
REPL surface inside the renderer and another implicit GL-stack
contract ("modelview is whatever the caller left, we push identity,
we pop"). The next author will not know which pattern is preferred
unless it is written down next to the snapshot.

**Concrete improvement.** Do not rewrite transform guides. Add a
short contract comment at the top of `guides_shared.h`: new cursor
guides fill a pre-evaluated arg bag on this snapshot and draw from
it; do not add another `source_cmds` / `flat_program` walk. If a
future guide needs "the transform in force at the cursor," compute
that matrix in the controller / `edit_overlays` (which already walk
the program) and pass the 16 floats in. The GL-stack scaffold stays
until a bug forces the CPU rewrite the old audit deferred.

---

### 8. `validate_render_config` covers the 2026-05 surface, not the enums added since

**Priority: low**

**Where.**
`render.c:95-138`. Grid/axes/backdrop ranges and the accum ladder
are checked, including an explicit integer range check for
`accum_effect` (the config field is currently an `int`, not the enum type).
`wireframe`, `post_filter_mode`, `winding_view`, and
`highlight_light_slot` are not checked.

**Why it matters.** The function's job is "reject values that hang
the GLUT loop or invoke undefined behavior." An out-of-range
`wireframe` currently falls through to the normal fill. Not a hang.
An out-of-range `post_filter_mode` is already a no-op in
`postprocess_filter_render`. The gap is consistency: a zeroed
config is a hard error for the grid and a silent default for
everything added later.

**Concrete improvement.** When next adding an enum to the config,
add the range check in the same patch. Optionally tighten
`wireframe` / `post_filter_mode` now to match the backdrop check.
Do not grow this into a schema validator.

---

### 9. The same quality flags are applied in three helpers, slightly differently

**Priority: low**

**Where.**
`render.c:472-477` (`render3d_apply_quality_config` — MSAA + line
smooth); `grid.c:874-879` (identical); `axes.c:227-232` (identical).
`render.c:479-482` then additionally sets `GL_LINE` polygon mode
for `WIREFRAME_PLAIN`. Lights and backdrop do not re-apply quality
flags; they inherit the pass setup.

**Why it matters.** Harmless duplication of four lines. The trap is
a fourth helper that copies only one of the two flags, or that
re-applies wireframe and fights `WIREFRAME_PLAIN`'s later restore
(`render.c:776-777`).

**Concrete improvement.** One `render3d_apply_quality_config` in a
tiny internal header, or just call the `render.c` helper from the
others if it is moved next to `render_types.h` as a static inline.
Only worth doing when touching all three files anyway.

---

### 10. Bitmap text has a shared helper that transform guides and axes do not use

**Priority: low**

**Where.**
`overlays.c:31-35` (`render3d_draw_bitmap_text`);
`overlays.h:40-47` (says lights, overlay labels, *and the orbit
gizmo* should go through it); `transform_guides.c:158-164`, `:742-745`
(raw `glutBitmapCharacter` loops); `axes.c:130-136` (same, one
character).

**Why it matters.** Not a visual bug. The helper exists specifically
so this loop is not rewritten. A font or raster-pos change has three
homes.

**Concrete improvement.** Route the transform-guide and axes label
sites through `render3d_draw_bitmap_text` (axes can pass a one-char
string). No new abstraction.

---

### 11. Winding-view materials are the one draw-color set outside `palette.h`

**Priority: low**

**Where.**
`render.c:717-721` (front green / back red / model ambient /
headlight, all file-local `static const GLfloat[]`).
`palette.h:21-38` already carves lighting *coefficients* out of the
palette on purpose — `lm_amb` in `lights.c` is the cited example —
but the winding front/back colors are `glMaterialfv` of *face
identity*, i.e. the thing the winding view exists to show, and they
are closer to `RENDER3D_CLR_WIREFRAME_VISIBLE` / `_HIDDEN` than to
a reflectance coefficient.

**Why it matters.** Changing the winding palette means finding a
comment in `render.c` rather than a token. Low, because the view is
a single pass with one pair of colors.

**Concrete improvement.** Either add `RENDER3D_CLR_WINDING_FRONT` /
`_BACK` and document a "material-as-draw-color" exception, or add
one sentence to the `palette.h` carve-out listing this pass next to
`lights.c lm_amb`. Either is fine; pick one so the next color
change has a home.

---

### 12. `FONT_*` reaches axes and guides only through the force-include

**Priority: low**

**Where.**
Makefile `OBJ_CFLAGS` force-includes `config.h`. `render.c:15`,
`overlays.c:11`, and `lights.c:7` include it explicitly.
`axes.c:135` and the guide files use `FONT_MONO` / `FONT_SMALL` /
`FONT_TINY` without including `config.h`. The normal build and
`check-c99` both force-include `config.h`, so neither lane tests whether
these translation units declare their own font dependency.

**Why it matters.** A TU compiled outside the Makefile recipe (a
one-off syntax check or an IDE configuration that does not mirror the
force-include) fails on `FONT_MONO` with no local clue. The overlays header
already documents the fonts as `void *` GLUT bitmap pointers. This is
self-contained-TU hygiene; the existing C99 guard does not catch it.

**Concrete improvement.** `#include "config.h"` in `axes.c` and
the two guide `.c` files, matching `lights.c`. No new font
indirection.

---

## Good patterns worth preserving

- **Helpers push their own `GL_ALL_ATTRIB_BITS` even though the pass
  already has an outer guard** (`render.c:144-146`). Side effects stay
  local. The 2026-05 audit deferred narrowing the masks (#15);
  leave the wrappers until a profiler says the nested full-bit
  pushes are expensive.
- **Chrome vs report, enforced by one stencil bracket**
  (`render.c:851-864`). New host chrome goes inside
  `render3d_pass_helpers`. New geometry reporting goes through
  `post_overlays_fn`. Naming no viz vocabulary in that bracket is
  load-bearing.
- **X-macro theme lists in `themes.h`.** Cfg symbols, enum, and
  count stay in lockstep. Backdrops and light themes already show
  the cheap end of this: one list, one table or switch.
- **`Render3dExecutePurpose`.** Side-effecting callbacks run only on
  `MAIN_FILL`. Hidden-line and winding reuse the same hook without
  a second function pointer. New scaffolding passes add an enum
  value, not a new callback type.
- **Capability bits on the config, never queried per frame**
  (`point_parameter_supported` + proc, `nv_fog_distance_supported`).
  The renderer does not call `glGetString`.
- **Per-helper draw contexts** (`GridDrawContext`, `AxesDrawContext`)
  that stamp resolved fade once and thread it through every color
  setter. That is why the 2026-05 `g_xn_*` statics could die.
- **Guide label sink** (`guides_shared.h:20-42`, `:160-184`).
  Header-only, no-op when empty, keeps decluttering out of the
  guide renderers.
- **Comments that argue why.** The two-background-color block, the
  accum-vs-caller-clear block, the stencil-chrome block, the
  `render3d_hash.h` vs `repl_randf` block, `occluded_ghost.h`. New
  comments should explain a constraint, not restate the next line.

---

## Highest-value changes

1. **Bring the written pipeline back in line with `render3d_scene_pass`**
   (Finding 1). Comments and `ARCHITECTURE.md` only. Highest leverage
   per minute; stops the next change from targeting a renderer that
   no longer exists.
2. **Put camera world pose on `Render3dFrameRenderContext`**
   (Finding 2). This is the type's stated job. Removes the hidden
   coupling between `lights.c`, `grid.c`, and `glr_camera_load_modelview`.
3. **Drive grid fade/fog/reveal membership from one trait table**
   (Finding 3). Makes the next grid theme a one- or two-edit change
   instead of a four-list scavenger hunt.
4. **Stop shipping `GlrPostFx*` through `render_types.h`**
   (Finding 4). Restores the renderer/app split the rest of the
   module already has, and makes the next post effect one enum.
5. **Delete the unread lighting flag from the render3d config**
   (Finding 5). One less "what does this do?" field on the central
   struct.

Findings 6–12 are convention and small consistency fixes; fold them
into whatever patch already touches the file.

## Overall assessment

`src/render3d` is a real renderer with a real contract, not a leftover
`scene_*` dumping ground. The 2026-05 audit did the hard work: instance
state is caller-owned, camera apply left the module, helpers take an
explicit frame snapshot, and the standalone demo still links. The
live pipeline in `render.c` is easy to follow once you are looking at
the functions rather than the mermaid.

The remaining cost of change is concentrated in three places, none of
which wants a framework:

- **narrative drift** (docs and a handful of comments still describe
  the pre-audit / pre-rename module);
- **an unused derived-state slot** that helpers compensate for by
  re-deriving camera pose;
- **grid-theme membership** spread across a spec table, a dispatch
  switch, and two predicates.

Everything else — hook-injected overlays, the transition machine, the
palette policy, the two background colors, the execute-purpose enum —
should be left alone. Add the next backdrop, post effect, or host
overlay by copying the existing neighbour, after the docs say what
that neighbour actually is.
