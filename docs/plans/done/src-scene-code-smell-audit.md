# `src/scene/` - Code-Smell Audit

> Audit produced 2026-05-24. Resolution pass landed 2026-05-25
> (commits `2d1b9b6` through `222054c`; main fast-forwarded to the
> SceneExecutePurpose review at `49bf979`, scene-smells continues
> past it with the god-function splits and the RESERVED-field
> doc cleanup). See the **Status** section below for per-finding
> state. Findings come from four parallel reviews of
> `src/scene/` (render core; grid + axes + scene_transition;
> visual-content modules - backdrop/lights/overlays/postprocess +
> palette/themes/occluded_ghost; the `guides/` subtree) plus targeted
> spot-verification of the most actionable claims. File:line
> references are exact at the time of writing - check `git log` on the
> cited files before acting if this doc has aged.

## Status (2026-05-25)

**54 of 65 findings done** across the resolution pass:
- 🔴 **10 / 10** bugs - every red finding closed
- 🟡 **20 / 25** boundary/drift items
- 🟢 **7 / 9** dead-code items
- 🔵 **17 / 21** structural items (every god-function split closed)

Tests: 7228 → 7463 (+235 from drift tests, new predicate coverage,
and the SceneRendererState init/independence/AA-invariant tests).
`check-state-ownership` green; C99 ratchet green; `make test-full`
and `make test-stubs` on gracemont (real GCC 13.x) both pass at
`6a6a4cd` (the #11 camera move). Subsequent commits on main -
`9c13f60` / `6ccb6d7` (peer-subsystem cleanup, not part of this
audit) and `f723382` (the audit-doc updates themselves) - don't
touch scene code, so the green claim still holds at the current
HEAD by inspection; re-run the gates if any code commit lands on
top of those.

| Finding | Status | Commit |
|---|---|---|
| 🔴 #1 transform-cmd parity drift | ✅ drift test added | `2d1b9b6` |
| 🔴 #2 SceneExecutePurpose | ✅ enum + adapter snapshot/restore | `08fc94b`, `49bf979` |
| 🔴 #3 g_xn statics in grid+axes | ✅ scene_overlay_xn_resolve + per-ctx threading | `cc481c2` |
| 🔴 #4 g_guide_alpha_mul threaded | ✅ alpha_mul as parameter | `3023624` |
| 🔴 #5 g_active_projection statics | ✅ caller-owned SceneRendererState | `89e2b17` |
| 🔴 #6 cityscape RNG collisions | ✅ stride 13 → 512 | `b2739b8` |
| 🔴 #7 glPointParameterfv comment | ✅ documented as load-bearing reset | `b2739b8` |
| 🔴 #8 underwater push/pop ordering | ✅ disable inside push | `905cb0d` |
| 🔴 #9 transform_source_unmodified param | ✅ dropped, renamed | `5dd0e65` |
| 🔴 #10 transitive includes | ✅ explicit math/string/ctype | `5dd0e65` |
| 🟡 #11 scene_apply_camera placement | ✅ moved to glr_camera.c as glr_camera_load_modelview | `6a6a4cd` |
| 🟡 #12 scene_apply_projection split | ✅ split into compute (once) + apply (per sample) | `89e2b17` |
| 🟡 #13 include style sweep | ✅ scene/foo.h → foo.h where in-dir | `d30ff28` |
| 🟡 #14 lights.c gl_includes | ✅ explicit include added | `5dd0e65` |
| 🟡 #15 targeted attrib masks | ⏸️ deferred - invasive across modules | - |
| 🟡 #16 NV_fog save/restore | ✅ explicit getIntegerv + restore | `c2e7009` |
| 🟡 #17 postprocess g_saved_matrix_mode | ✅ threaded through begin_2d/end_2d | `08fc94b` |
| 🟡 #18 int → enum sweep | ✅ post_filter_mode, grid_theme | `dc6ed83` |
| 🟡 #19 viewport_w/h fields | ✅ dropped from SceneRenderConfig | `dc6ed83` |
| 🟡 #20 grid_xn_phase/axes_xn_phase | ✅ marked RESERVED with explicit doc | `222054c` |
| 🟡 #21 SceneFrameRenderContext.focus | ✅ duplicate dropped | `dc6ed83` |
| 🟡 #22 theme spec ABI | ⏸️ deferred - design decision | - |
| 🟡 #23 standard-theme switch | ✅ moved to default arm | `e9225cd` |
| 🟡 #24 is_geometry_emit_cmd predicate | ✅ promoted to repl_cmd_starts_geometry_emit | `e9225cd` |
| 🟡 #25 compute_before_cursor_matrix | ⏸️ deferred - CPU-math rewrite would dwarf current GL-stack scaffold | - |
| 🟡 #26 snapshot input NULL policy | ✅ documented in guides_shared.h | `655cbef` |
| 🟡 #27 accum_samples == 1 carve-out | ✅ ladder check, kept | `c2e7009` |
| 🟡 #28 accum_samples ladder validation | ✅ now {1,2,4,8,16} enforced | `c2e7009` |
| 🟡 #29 clamp01f helper | ✅ scene_clamp01f in render_types.h | `08fc94b` |
| 🟡 #30 cityscape y0/y1 shadow | ✅ renamed to y_base/y_top | `9ee978b` |
| 🟡 #31 M_PI consolidation | ✅ moved to gl_includes.h | `d30ff28` |
| 🟡 #32 postprocess graduation | ⏸️ deferred - scope question | - |
| 🟡 #33 scene_xn API names | ✅ cheat-sheet in header | `08fc94b` |
| 🟡 #34 stale filename prefixes | ✅ swept across scene/ | multiple |
| 🟡 #35 palette token review | ⏸️ deferred - design decision | - |
| 🟢 #36 redundant pre-pop teardown | ✅ removed from lights/grid/axes/render | `905cb0d`, `dc6ed83` |
| 🟢 #37 inline _push_state wrappers | ⏸️ skipped - keeps the seam for #15 | - |
| 🟢 #38 dead cityscape clamps | ✅ pruned to the live boundary | `b2739b8` |
| 🟢 #39 transform_source_unmodified param | ✅ covered by #9 | `5dd0e65` |
| 🟢 #40 guides_shared flat_program | ⏸️ deferred - accept current trade-off | - |
| 🟢 #41 redundant glEnable(GL_LIGHTING) | ✅ removed from 5 guide draws + axes/grid/lights | `5dd0e65`, `dc6ed83` |
| 🟢 #42 ACCUM_STEP_COUNT | ✅ removed (unused in scene) | `dc6ed83` |
| 🟢 #43 stale tess-preview comments | ✅ removed | `dc6ed83` |
| 🟢 #44 g_saved_matrix_mode init | ✅ covered by #17 | `08fc94b` |
| 🔵 #45 scene_grid_render god-fn | ✅ split into 5 named phases | `2b9e194` |
| 🔵 #46 draw_cityscape god-fn | ✅ split into setup + box + windows helpers | `ef92d2c` |
| 🔵 #47 draw_rotate_guide god-fn | ✅ split into arc / helix / pulse helpers | `4c64c97` |
| 🔵 #48 render_3d_scene_pass split | ✅ split into setup/fill/helpers/overlays | `17301b8` |
| 🔵 #49 make_arrow_basis helper | ✅ extracted | `709455f` |
| 🔵 #50 clamp_head_len helper | ✅ extracted + axis-tighter constants named | `709455f` |
| 🔵 #51 draw_guide_axis_plane helper | ✅ replaces yz/xz/xy_plane triplet | `709455f` |
| 🔵 #52 axes per-theme extraction | ✅ extracted to match grid's pattern | `8301ce1` |
| 🔵 #53 scene_draw_bitmap_text helper | ✅ replaces 4 raster+for loops | `c2e7009` |
| 🔵 #54 scene_apply_camera 6 floats | ✅ SceneCameraPose struct on render.h | `9ee978b` |
| 🔵 #55 magic numbers | ⏸️ deferred - large surface | - |
| 🔵 #56 hoist tan() in scene_apply_projection | ✅ SCENE_DEFAULT_HALF_FOVY_TAN macro | `9ee978b` |
| 🔵 #57 post_fill_fn / post_overlays_fn names | ⏸️ deferred - naming bikeshed | - |
| 🔵 #58 goto bad style outlier | ⏸️ deferred - stylistic | - |
| 🔵 #59 SCENE_PROBE_BOX macro chain | ✅ dependency documented in comment | `9ee978b` |
| 🔵 #60 fb[96*1024] named constant | ✅ SCENE_PROBE_FEEDBACK_FLOATS | `9ee978b` |
| 🔵 #61 scene_xn_init/show share | ⏸️ deferred - not worth it | - |
| 🔵 #62 inline decls inside glBegin/glEnd | ✅ lights.c star-burst table hoisted | `9ee978b` |
| 🔵 #63 STATIC_ASSERT count check | ✅ now sizeof-derived from table | `9ee978b` |
| 🔵 #64 alias style | ✅ alias cheat-sheet added | `655cbef` |
| 🔵 #65 stale init_gl docs | ✅ rewritten to match actual behavior | `dc6ed83` |

**Deferred work clusters** (each warrants its own session due to scope):

- **State lifting cluster closed:** #3, #5, #11, #12 all landed
  (`cc481c2`, `89e2b17`, `6a6a4cd`). Camera apply now lives at
  src/app/glr_camera.c as glr_camera_load_modelview; renderer state
  + projection split + per-renderer ortho ref are all caller-owned.
  Scene module is purely the renderer now - every "scene does not
  own X" contract the audit flagged is honored by code shape, not
  convention.
- **Boundary tightening continued:** #15 (narrow `GL_ALL_ATTRIB_BITS`
  to targeted masks per pass) interacts with #37 (inlining the
  `_push_state` wrappers); the wrappers stay until #15 decides the
  per-pass mask shape.
- **Theme spec ABI** (#22): widening the GridThemeSpec to absorb the
  custom themes (OCEAN/FOCUS/RULER/PLANES/RADAR) or going N
  per-theme functions. The per-theme axes extraction (#52, landed)
  gives a working model of the all-functions side; the audit prefers
  whichever lets ALL themes go through the same shape.
- **Magic numbers** (#55): per-theme constants are the natural home;
  the current literals encode tuning knobs that ought to be named.
- **Cosmetic remaining** (#57 post_fill_fn rename, #58 goto bad
  style, #61 scene_xn_init/show seed helper): each a sub-15-minute
  drive-by; the audit itself marks #58 and #61 as "leave / probably
  not worth it." Group them into a sweep when next in the area.
>
> Scope: every file under `src/scene/` (`*.c/h` + `guides/`). Tests
> under `tests/` were read where they document a contract, but not
> audited.
>
> The single most important contract for this directory (per
> `src/scene/README.md`):
> **scene code renders. It does not parse, edit, save, or dispatch UI
> actions; `scene_*` and `ui_*` do not include each other's headers;
> scene renderers consume snapshots/configs and never read
> `ReplState`, `EditorState`, or `UiState` directly; the standalone
> `scene_demo` binary is the layer-independence proof - if scene code
> grew a hard dep on editor/controller/UI, that binary stops linking.**
>
> Headline: **the layering invariant holds**. Zero `editor/`,
> `repl/`, or `ui/` includes in any scene file. `scene_transition.c`
> is genuinely pure (no GL, no globals, no stdlib). The `scene_demo`
> proof works. The findings below cluster around three other themes:
> (1) **module-private mutable globals** that should be parameters or
> per-frame state (cursor-projection cache, ghost-pass alpha, grid/
> axes transition alpha); (2) **two parallel implementations** that
> must stay in sync but aren't checked (executor's vs `transform_utils.h`'s
> transform-cmd dispatch; the matching `g_xn_*` statics in grid.c and
> axes.c); (3) **god-functions** in the largest files (`scene_grid_render`
> 156 lines, `draw_cityscape` 224 lines, `draw_rotate_guide` 155
> lines) that mix dispatch, GL state, and per-theme math.

## How to read this

Severity grouping mirrors the previous audits:

- **🔴 Actual bugs / hazards (verified)** - correctness or
  parity-drift issues with a concrete failure mode that exists in
  current production code. Pick these up first.
- **🟡 Drift / boundary hazards** - module-private mutable state,
  duplicated patterns across files, missing predicate enforcement,
  ambiguous-intent code that works today but is one edit away from
  misbehaving.
- **🟢 Dead code / dead fields** - code with no callers, unused
  parameters, redundant cleanups before `glPopAttrib`. Pure surface
  reduction.
- **🔵 Structural concerns** - god-functions, near-duplicate
  helpers, magic numbers, comment archaeology. Bigger refactors;
  higher cost.

Each finding cites file + line, names the smell, says why it
matters, and suggests a one-line fix. Cross-cutting findings (the
same pattern recurring in multiple files) carry a **🔀 cross-file**
tag.

## 🔴 Actual bugs / hazards (verified)

### 1. Transform-cmd parity drift: `transform_utils.h` and `executor.c` keep two parallel switches

**Where:** `src/scene/guides/transform_utils.h:20-52` vs
`src/repl/executor.c:204-247`

**Smell:** Both files implement an `apply_tracked_transform(cmd, &depth)`
switch over the six transform `CmdType`s
(`CMD_PUSH_MATRIX` / `CMD_POP_MATRIX` / `CMD_LOAD_IDENTITY` /
`CMD_TRANSLATE3F` / `CMD_SCALEF` / `CMD_ROTATEF`). The header
comment says this is *"so they can mirror executor-style matrix
tracking without depending on src/repl/executor.h"* - but there is
NO compile-time check, predicate, or drift test linking the two.

**Why it matters:** If a contributor adds `CMD_MULT_MATRIX`,
`CMD_LOAD_MATRIX`, or `CMD_MATRIX_MODE` to `executor.c` (and updates
`repl_cmd_is_transform`), the controller's overlay walks and the
transform-guide walks silently fall into `default: break;` - guides
land at wrong positions, outline/vertex-point overlays drift from
the live scene. The `repl_cmd_is_transform` predicate (which is the
peer's iteration filter) would correctly include the new case, so
the walk *visits* the cmd but does nothing.

**Fix:** Either (a) collapse to one implementation - `transform_utils.h`
calls the executor functions (the scene module stays insulated via
the header), or (b) add a `STATIC_ASSERT` pinning `repl_cmd_is_transform`
to exactly six known bits plus a coverage test that walks every
`CmdType` reported by the predicate through `apply_tracked_transform`
and asserts a non-default branch.

### 2. `scene_probe_eye_dist` invokes the user's geometry callback as a side pass without telling it

**Where:** `src/scene/render.c:156-244`

**Smell:**
```c
glRenderMode(GL_FEEDBACK);
glPushMatrix();
{
    SceneExecuteContext ctx = { 0 };
    config->execute_fn(&ctx, config->execute_user_data);
}
glPopMatrix();
```
`SceneExecuteContext` is documented (`render_types.h:44-48`) as a
placeholder so future scene-to-callback metadata can be added (e.g.
"why is this callback being invoked: main fill, fade overlay,
picking pass"). The probe is exactly such a callback purpose - but
the context is left `{0}` and the callback can't distinguish a real
fill from a feedback-only probe.

**Why it matters:** Any side-effecting `execute_fn` (audio,
incrementing state, RNG advance, REPL's `t` evaluation, dirty-flag
writes) runs twice per frame in ortho mode, every frame in
per-frame mode. The doc-comment promised the seam; the code never
delivered.

**Fix:** Add a `SceneExecutePurpose` enum (`MAIN_FILL`,
`DEPTH_PROBE`, `FADE_OVERLAY`, …) field to `SceneExecuteContext`;
fill it here (`DEPTH_PROBE`) and at the fade-overlay invocation;
executors opt out of side effects on non-fill passes. Until then,
document loudly that probe re-entry exists.

### 3. 🔀 Module-private mutable globals across grid.c and axes.c

**Where:** `src/scene/grid.c:78-79` and `src/scene/axes.c:59-60`

**Smell:** Both files maintain identical parallel statics:
```c
static float g_xn_opacity = 1.0f;
static float g_xn_alpha   = 1.0f;
```
Plus identically-named `gl_color(r, g, b, a)` static that multiplies
alpha by `g_xn_alpha`, identical clamp logic at
`grid.c:699-702` / `axes.c:237-240`, identical 10-line header
comment block. Hidden coupling between two TUs masquerading as
"file private."

**Why it matters:** Can't render grid+axes pair where you want to
override one alpha but not the other - both statics tick together
because both modules read the same `config->grid_opacity` /
`axes_opacity` fields. Clamp logic is triplicate (grid, axes, plus
the FOG-style branch). Any future "one renderer for both" refactor
hits the parallel statics first.

**Fix:** Either thread `xn_alpha` as a parameter through each draw
helper, or factor a shared `scene_overlay_xn.h` with a
`scene_overlay_xn_resolve(opacity, has_own_fog) -> { alpha, fog_tf }`
pure helper. Both modules then call it once at entry.

### 4. `g_guide_alpha_mul` is hidden inter-pass state for transform guides

**Where:** `src/scene/guides/transform_guides.c:33`

**Smell:**
```c
static float g_guide_alpha_mul = 1.0f;
```
The two-pass ghost render in `scene_transform_guides_render_if_due`
(`:738-760`) flips this between `SCENE_OCCLUDED_GHOST_ALPHA` and
`1.0f`. Every `tg_color4f` reads it. Write-then-read across function
boundaries with no parameter.

**Why it matters:** Non-reentrant for no reason. If any future
early-return or assertion-abort skips the `g_guide_alpha_mul = 1.0f`
reset on line 759, subsequent guide renders get stuck at 40% alpha
forever. The `tg_color_tok` helper (`:42-48`) exists *only* because
`scene_clr_a` would bypass the global - the workaround is what gave
the global away.

**Fix:** Thread `alpha_mul` as a parameter through `draw_translate_guide` /
`draw_scale_guide` / `draw_rotate_guide` / `draw_pulse_segment` /
`draw_arrow_head` / `tg_color4f`. `tg_color_tok` becomes a
`scene_clr_a` call.

### 5. `g_active_projection` / `g_ortho_ref_dist` / `g_ortho_active` are unsynchronized mutable state

**Where:** `src/scene/render.c:118-134`

**Smell:** Three statics carry frame-to-frame state inside a module
that everywhere else accepts a `SceneRenderConfig` snapshot.

**Why it matters:**
1. Two concurrent viewports (the `scene_demo` and REPL both link this
   code) would corrupt each other; the second viewport is plausible
   given the controller already builds a config-per-call.
2. `g_active_projection` is updated *inside* the accum-jitter loop
   (per sample via `scene_apply_projection`); `scene_get_active_projection`
   sees a transient snapshot. The cached `ortho_top`/`fovy_deg`
   doesn't contain jitter terms today - so it's deterministic by
   accident, not design.
3. `g_ortho_active` is an edge detector for the "frozen" mode. If
   `scene_render_3d_scene` returns early via `validate_render_config`
   (`:605`), `g_ortho_active` isn't reset; a fail/recover oscillation
   desyncs the edge logic.

**Fix:** Fold into a `SceneRendererState` struct owned by the caller,
or at minimum expose an init/reset entry point and document the
single-renderer assumption. The postprocess module already has a
`_reset()` pattern; mirror it.

### 6. Cityscape RNG hash collisions across buildings

**Where:** `src/scene/backdrop.c:96, 217, 219, 223`

**Smell:** `base = bi * CITY_RNG_STRIDE` with `stride = 13` and
offsets `+50`, `+200`, `+300`. The hash inputs collide on a regular
pattern. Building 0's `warmth` (`base+200=200`) equals building 15's
`h4` height random (`base+5 = 195+5 = 200`). Building 0's
`bldg_phase` slot (`base+50=50`) lands on building 3's `base+11`
slot.

Same problem in the window-ID computation: `wid = bi*13 + 300 +
(wc*17 + wr)` plus per-window offsets `wid+1`, `wid+7`, `wid+11`
collide with neighboring windows' base `wid`.

**Why it matters:** Two unrelated decisions in two different
buildings end up with identical `city_rng` values. The squared
distribution and 3-way palette mostly hide it, but it's a real
low-frequency regularity that defeats the "scatter per-building
detail" claim in the file comment.

**Fix:** Route `bi`, `wc`, `wr` through `city_rng` (hash composition)
before combining, or use a stride larger than the max in-building
offset range (`max wid - base` is ~449; stride should be ≥ 512).
Window-offset `+1/+7/+11` should likewise be separate hash calls
keyed on `(wid, slot_id)`.

### 7. `glPointParameterfv` in starry sky is dead-on-arrival

**Where:** `src/scene/backdrop.c:311-312`

**Smell:**
```c
if (point_parameter_supported)
    glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,
                       (GLfloat[]){1, 0, 0.00});
```
Coefficients are `(1, 0, 0)` - identity: `size *= 1/sqrt(1 + 0·d + 0·d²) = 1`.
There is no attenuation regardless of support. The supported-vs-
unsupported branch is visually identical, and the gated call is
pure overhead.

**Why it matters:** The whole CLAUDE.md infrastructure around
`GLR_NO_POINT_PARAMETER` (capability gate, env-var override,
fallback comment) is wired into a no-op. The comment that says
"when unsupported the stars still render at a fixed glPointSize
(acceptable degradation, no distance attenuation)" lies - there's
no degradation because there's no attenuation in either branch.

**Fix:** Either pick non-identity coefficients (e.g.
`(0.2, 0, 0.15)` as used elsewhere) so the gate is actually
load-bearing, or delete the call and the `point_parameter_supported`
parameter wiring through `draw_starry_sky`.

### 8. Underwater branch has inverted depth/lighting push/pop

**Where:** `src/scene/grid.c:383-403`

**Smell:**
```c
if (camera_world_y < 0.0f) {
    glDisable(GL_DEPTH_TEST);             /* (1) state already mutated */
    gl_color(0.05f, 0.25f, 0.35f, 0.75f);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    gluOrtho2D(0, config->viewport_w, 0, config->viewport_h);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT);  /* (2) push after mutation */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glRectf(0, 0, (float)config->viewport_w, (float)config->viewport_h);
    glPopAttrib();
    ...
    glEnable(GL_DEPTH_TEST);              /* (3) manual restore outside push */
}
```
`glDisable(GL_DEPTH_TEST)` at line 385 happens *before* the
`glPushAttrib(GL_DEPTH_BUFFER_BIT|GL_LIGHTING_BIT)` at line 394 -
so the pre-push state is already wrong, and the matching
`glEnable(GL_DEPTH_TEST)` at line 403 has to re-fix it manually
outside the pushed scope.

**Why it matters:** This is the kind of mis-bracketing that "works
because of the outer `glPushAttrib(GL_ALL_ATTRIB_BITS)` in
`scene_grid_render`"; if the outer push is ever narrowed, this
becomes a depth-test leak.

**Fix:** Move `glDisable(GL_DEPTH_TEST)` inside the push at line 394;
delete the manual `glEnable(GL_DEPTH_TEST)` at line 403; the pop
handles it.

### 9. `transform_source_unmodified` takes a `const GLCmd *source_cmd` it never reads

**Where:** `src/scene/guides/transform_guides.c:620-622`

**Smell:**
```c
static int transform_source_unmodified(const SceneGuideSnapshot *snapshot,
                                       const GLCmd *source_cmd) {
    (void)source_cmd;
```
The parameter is unused (`(void)source_cmd;`). The single call site
(`:664`) computes `source_cmd` two lines earlier just to pass it
here. The function's *actual* check is a text-compare between input
and committed text - wrong for what its name promises.

**Why it matters:** If the user retypes a transform with identical
args but different formatting (`glTranslatef(1,0,0)` vs `glTranslatef(1, 0, 0)`),
the `strncmp` rejects the guide even though the *parsed cmd is
unmodified*. The presence of `source_cmd` suggests the function was
meant to compare `cmd->args[]` against a re-parse of the input but
never grew that body.

**Fix:** Either drop the parameter and rename to
`transform_input_matches_committed` (honest about behavior), or
actually use it - compare `source_cmd->args` against a re-parse of
the input.

### 10. 🔀 Guide files rely on transitive `<math.h>` / `<string.h>` / `<ctype.h>` via `repl/eval.h`

**Where:** `src/scene/guides/geometry_guides.c`,
`src/scene/guides/transform_guides.c` (no explicit math/string/ctype
includes)

**Smell:** Both files use `sqrtf` / `fminf` / `strncmp` /
`cosf` / `sinf` / `fabsf` / `fmodf` / `M_PI` / `isspace` / `strlen`,
none of which are included directly. They arrive through the chain
`guides_shared.h` → `repl/command.h` → `repl/eval.h` (which
includes the math/string/ctype headers).

**Why it matters:** If a future refactor removes those from
`repl/eval.h`, all guide TUs break with implicit-function-
declaration errors - and `-std=c99` makes that a hard error. The
C99 ratchet won't flag a missing include unless a path actively
calls the helper.

**Fix:** Add the headers explicitly to both `.c` files. Cheap; same
issue also applies to `lights.c` (which needs `gl_includes.h`
explicitly; see #14).

## 🟡 Drift / boundary hazards

### 11. `scene_apply_camera` lives in render.c but the renderer refuses to call it

**Where:** `src/scene/render.c:381-389`, `render.h:35-36`

**Smell:** README and header explicitly say "callers must invoke
`scene_apply_camera()` before `scene_render_3d_scene`" and "the
scene module does not own camera state." Yet the helper that
applies the camera math lives in scene code, and
`scene_render_3d_scene` at `:537-538` notes "the caller's prior
`scene_apply_camera()` left the modelview correctly populated; we
just need the mode set." Hidden temporal coupling, enforced by
convention only.

**Why it matters:** One wrong-order edit (moving
`scene_apply_quality_config` above `scene_apply_projection`, or
anyone touching the projection matrix between camera-apply and
scene-render) corrupts user geometry silently. The 9-line comment
defending the contract documents fragility rather than fixing it.

**Fix:** Pick one - internalize the camera into `scene_render_3d_scene`
(`SceneRenderConfig` already carries `cam_rx/ry/dist/tx/ty/tz`), or
move `scene_apply_camera` to `src/app/glr_camera.c`. Don't have a
scene-namespaced public function that scene code refuses to call.

### 12. `scene_apply_projection` is misnamed - also caches export state

**Where:** `src/scene/render.c:276-379`

**Smell:** The function name says "apply projection" but also writes
to `g_active_projection` (the export-facing global). The cache happens
*every* sample of the accum loop (16× per frame), all writes
producing the same value - idempotent waste.

**Why it matters:** Side effect is buried in a "looks pure" name.
Future readers tracing "where does `scene_get_active_projection`'s
data come from?" have to dig into the matrix-apply body.

**Fix:** Split into `compute_active_projection(config, &desc)` (pure,
fills the desc + updates the global once) and `apply_projection_matrix(desc, jitter_x, jitter_y)`
(mutates GL state per sample). Call the pure half once before the
accum loop.

### 13. 🔀 Include-style inconsistency: `"scene/palette.h"` vs `"palette.h"`

**Where:** `src/scene/lights.c:5`, `src/scene/guides/geometry_guides.c:6`,
`src/scene/scene_transition.c:2`

**Smell:** Three files reach for sibling headers with a
directory-prefixed path:
```c
#include "scene/palette.h"           /* lights.c */
#include "scene/scene_transition.h"  /* scene_transition.c */
```
Every other `src/scene/*.c` includes its own sibling with the bare
name (`#include "palette.h"` / `#include "render_types.h"`). The
`check-include-style` guard documented in CLAUDE.md doesn't catch
this directory-prefix variation.

**Fix:** Use bare names for same-directory includes. Sweep the three
outliers.

### 14. `lights.c` reaches `glColor4f` / `glutBitmapCharacter` without `gl_includes.h`

**Where:** `src/scene/lights.c:4-9`

**Smell:** No `#include "gl_includes.h"`. GL symbols arrive
transitively via `"scene/palette.h"` → `"gl_includes.h"`. Sibling
files (`overlays.c`, `postprocess_filter.c`) include explicitly.

**Fix:** Add `#include "gl_includes.h"` explicitly.

### 15. 🔀 `GL_ALL_ATTRIB_BITS` used where targeted bits would document intent

**Where:** `src/scene/backdrop.c:48`, `lights.c:12`,
`postprocess_filter.c:53`, plus the `*_push_state` wrappers in
`grid.c:54-60`, `axes.c:33-39`, `render.c:104-110`

**Smell:** Every scene module that pushes attribs uses
`GL_ALL_ATTRIB_BITS`. Compare to `grid.c:394` (`GL_DEPTH_BUFFER_BIT |
GL_LIGHTING_BIT`) and `transform_guides.c:738`
(`GL_DEPTH_BUFFER_BIT`) - narrower and self-documenting.

**Why it matters:** Each pass touches a knowable subset (backdrop
touches FOG/DEPTH/COLOR/POINT/LINE/CURRENT; lights touches
LIGHTING/DEPTH/BLEND/CURRENT/LINE/POINT; postprocess touches
TEXTURE/VIEWPORT/COLOR/DEPTH/LIGHTING/FOG/CURRENT). The catch-all
makes the contract opaque and incurs unnecessary state-save cost.

**Fix:** Switch to targeted bitmasks per pass.

### 16. NV-fog-distance mode may not be saved by `GL_FOG_BIT`

**Where:** `src/scene/backdrop.c:90-91`

**Smell:**
```c
if (nv_fog_distance_supported)
    glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
```
Wrapped in `GL_ALL_ATTRIB_BITS`. Whether `GL_FOG_DISTANCE_MODE_NV`
is included depends on the driver's interpretation of the NV
extension - it's not in the Khronos `GL_FOG_BIT` spec.

**Why it matters:** State leak across the pop. The existing test in
`tests/test_scene_render.c:510-519` exists because this leaked
before. Current defense relies on driver good-citizenship.

**Fix:** Explicitly save/restore via `glGetIntegerv(GL_FOG_DISTANCE_MODE_NV,…)`
around the call, or restore the default mode at function tail
before pop.

### 17. Postprocess's `g_saved_matrix_mode` couples `begin_2d` and `end_2d` invisibly

**Where:** `src/scene/postprocess_filter.c:20, 54, 93`

**Smell:** Module-global `g_saved_matrix_mode` initialized to `0`
(GL_NONE - invalid). `begin_2d` writes via `glGetIntegerv`; `end_2d`
reads. No assertion the pair is balanced.

**Why it matters:** An unbalanced `end_2d` would silently set
matrix mode to `GL_NONE`. The pair is always called together today,
but a future second caller could miss it.

**Fix:** Return the saved mode from `begin_2d` and pass into
`end_2d`; or skip the save and always restore to `GL_MODELVIEW` at
the tail (matching the pre-existing scene convention).

### 18. 🔀 `int` parameters where enum types exist in the same header

**Where:**
- `src/scene/render_types.h:159` - `int post_filter_mode` (enum
  `ScenePostFilterMode` exists 4 lines away in
  `postprocess_filter.h`)
- `src/scene/grid.h:24` - `int scene_grid_theme_uses_fog(int grid_theme)`
  (`themes.h` has `SceneGridTheme`)
- `src/scene/postprocess_filter.h:26` - `int mode` parameter on
  `scene_postprocess_filter_mode_name`

**Smell:** Storing typed values as raw `int` weakens type checking
and forces casts at the boundaries.

**Fix:** Mechanically change to the matching enum type. The bools
(`int point_parameter_supported`, `int nv_fog_distance_supported`)
can stay as `int` per project convention.

### 19. `viewport_w/h` exported on render_types.h but only one consumer reads them

**Where:** `src/scene/render_types.h:101-102`

**Smell:** `viewport_w` / `viewport_h` are read only by `grid.c:390/397`
(underwater HUD). `render.c` uses `scene_x/y/w/h` exclusively for
all viewport math. Header exports a contract one consumer ignores.

**Fix:** Drop the fields and have `grid.c` pull viewport from the
scene rect, or comment as "consumer-specific" with a single
documented owner.

### 20. `grid_xn_phase` / `axes_xn_phase` on render_types are reference-only

**Where:** `src/scene/render_types.h:168, 174`

**Smell:** Only referenced in tests (`test_glr_ctrl.c:605-667`). The
comment at `:166` admits "advisory direction hint (unused by FADE
v1, plumbed for fog)."

**Fix:** Either remove (let `grid.c` / `axes.c` accept their own
phase when they need it) or mark as `/* RESERVED - reads pending */`
with an issue link.

### 21. `SceneFrameRenderContext` duplicates `focus` from `config.focus`

**Where:** `src/scene/render_types.h:188-191`, `render.c:403-407`

**Smell:**
```c
typedef struct SceneFrameRenderContext {
    SceneRenderConfig config;
    SceneFocusVertex focus;       /* already in config */
} SceneFrameRenderContext;
...
ctx->config = *config;
ctx->focus = config->focus;     /* duplicate */
```
`grid.c:316` reads `frame_ctx->focus` (not `frame_ctx->config.focus`).

**Fix:** Drop the duplicate `focus` and have `grid.c` read
`frame_ctx->config.focus`. Or drop `SceneFrameRenderContext`
entirely and pass `const SceneRenderConfig *` everywhere.

### 22. Theme dispatch is half-table, half-switch

**Where:** `src/scene/grid.c:280-297` (`g_grid_theme_specs[]`) vs
`:792-838` (the switch). Similar split in `axes.c:264-507`.

**Smell:** Five themes (CLASSIC/FOG/TRON/EMBER/FAINT) flow through
the table-driven `draw_grid_standard_theme`. Four others
(FOCUS/OCEAN/XZRULER/PLANES/RADAR) bypass it entirely with ad-hoc
render functions. No comment explains the rule. The five "ruler" /
"planes" / "ocean" themes all paint colored x-vs-z line pairs that
*could* share a `(GridLineColorFn xz_split)` spec extension.

Same pattern in axes.c: `AXES_THEME_NEON` is inline (lines 24-31
explicitly apologize for it). The apologetic comment is itself a
smell - the abstraction was set up too narrowly.

**Fix:** Either widen the spec ABI (add `GridPerCellLineColorFn`
plus optional `setup`/`teardown` hooks) so the custom themes become
table entries, or drop the spec table entirely and have nine clean
per-theme functions that share helpers more aggressively.

### 23. Standard-theme `switch` lists five cases that fall through to one body

**Where:** `src/scene/grid.c:794-803`

**Smell:**
```c
case GRID_THEME_CLASSIC:
case GRID_THEME_FOG:
case GRID_THEME_TRON:
case GRID_THEME_EMBER:
case GRID_THEME_FAINT: {
    const GridThemeSpec *spec = grid_theme_spec(grid_theme);
    if (spec) draw_grid_standard_theme(&grid_ctx, spec);
    break;
}
```
When a new standard theme lands as a `GridThemeSpec` entry, the
author must also touch this list. Two parallel lists must agree.

**Fix:** Handle custom themes first; `default:` looks up the spec.
Adding/removing a standard theme becomes one edit.

### 24. `is_geometry_emit_cmd` is the kind of ad-hoc predicate chain CLAUDE.md warns against

**Where:** `src/scene/guides/transform_guides.c:53-59`

**Smell:**
```c
static int is_geometry_emit_cmd(CmdType type) {
    return (type == CMD_BEGIN ||
            type == CMD_GLUT_TORUS || type == CMD_GLUT_CUBE ||
            type == CMD_GLUT_SPHERE || type == CMD_GLUT_TEAPOT ||
            type == CMD_GLUT_CONE ||
            type == CMD_TESS_BEGIN_POLYGON);
}
```
CLAUDE.md: *"`CmdType` set tests go through the inline predicates in
`src/repl/command.h`, not ad-hoc `||` chains."* Also drifts from
`repl_cmd_emits_vertex` (which is BEGIN-free). A new
`CMD_GLUT_DODECAHEDRON` would silently be treated as non-emit.

**Fix:** Add `repl_cmd_starts_geometry_emit` to `src/repl/command.h`
with a drift-test in `tests/test_replay_walk.c`.

### 25. `compute_before_cursor_matrix` mutates GL state to read it back

**Where:** `src/scene/guides/transform_guides.c:77-92`

**Smell:**
```c
glPushMatrix();
glLoadIdentity();
int depth = 0;
for (int i = 0; i < cursor_flat_idx; i++) {
    if (!flat_cmds[i].valid) continue;
    if (repl_cmd_is_transform(flat_cmds[i].type))
        apply_tracked_transform(&flat_cmds[i], &depth);
}
glGetFloatv(GL_MODELVIEW_MATRIX, out);
unwind_transform_stack(&depth);
glPopMatrix();
```
Same shape in `compute_after_cursor_origin` (`:99-122`). "Pure"
functions that compute a matrix from the flat program by trashing
and restoring the GL stack.

**Why it matters:** Depends implicitly on matrix-mode being
`GL_MODELVIEW`. The scaffolding `glPushMatrix` uses the same
primitives as user transforms - `depth` only counts user pushes; a
richer tracker could pop our scaffolding.

**Fix:** Compute the matrix in CPU memory (`mat4_mul_col_major`
already exists at `:61`). The helpers are called once per frame in
`prepare`, not from a GL hot path.

### 26. Snapshot's `input` pointer NULL policy diverges across files

**Where:** `src/scene/guides/geometry_guides.c:68, 75, 157` vs
`src/scene/guides/transform_guides.c:623-624`

**Smell:** `geometry_guides.c` assumes `snapshot->input != NULL`;
`transform_guides.c` defensively guards `edit_line_committed_text`
but never `snapshot->input`. Two consumers, opposite policies.

**Fix:** Document the invariant in `guides_shared.h` (e.g. *"`input`
is always a valid C string, possibly empty; never NULL"*) and pick
one defensive policy.

### 27. `accum_samples == 1` with `accum_aa_enabled` is a meaningless state

**Where:** `src/scene/render.c:619-633`, `validate_render_config:87-88`

**Smell:** Validator accepts `accum_samples == 1` as valid; the
loop skips the AA branch (`accum_samples > 1`). So `accum_aa_enabled=1,
accum_samples=1` produces the same output as `accum_aa_enabled=0`.

**Fix:** Either reject `accum_samples == 1 && accum_aa_enabled`, or
comment the carve-out ("ladder UI stays linear, 1-sample is a valid
'off' position").

### 28. `accum_samples > MAX_ACCUM_SAMPLES` not validated when AA flag is off

**Where:** `src/scene/render.c:86-89`, defensive cap at
`:622, 626`

**Smell:** Validator checks `accum_samples` only when both
`use_accum` AND `accum_aa_enabled` are true. The loop defends with
`g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES]` - the wrong place
to defend.

Also: the jitter table comment (`:32-33`) says "the first N entries
form a good N-sample set" - true only for N ∈ {1, 2, 4, 8, 16}, but
the validator allows any value 1..16.

**Fix:** Validate whenever AA is requested, and constrain
`accum_samples` to the supported ladder.

### 29. 🔀 Inline clamp-01 idiom repeated everywhere instead of a `clamp01f` helper

**Where:** `src/scene/grid.c:699-702, 223, 241, 332, 346, 479`,
`src/scene/axes.c:237-240`, plus many `fminf(v, 1.0f)` single-side
clamps in `grid.c:163-164, 170, 508-509` etc.

**Smell:**
```c
g_xn_opacity = config->grid_opacity;
if (g_xn_opacity < 0.0f) g_xn_opacity = 0.0f;
if (g_xn_opacity > 1.0f) g_xn_opacity = 1.0f;
```
Three-line clamp pattern repeated ~10 times across scene files.

**Fix:** `static inline float clamp01f(float v)` in `render_types.h`
(or as part of the lifted alpha helper from #3).

### 30. Cityscape uses local `y0`/`y1` shadowing math.h Bessel functions

**Where:** `src/scene/backdrop.c:6, 137-138`

**Smell:**
```c
#include <math.h>
...
float y0 = -0.05f;
float y1 = bh;
```
`y0` and `y1` are POSIX Bessel function names declared in
`<math.h>`. MEMORY.md already cites a worked-around export
collision for exactly this. C99 allows the shadow, but a future
`-Wshadow=local` would flag it.

**Fix:** Rename to `y_lo`/`y_hi` or `base_y`/`top_y`.

### 31. 🔀 `M_PI` redefined in three scene files

**Where:** `src/scene/render.c:21-23`, `grid.c:7-9`, `axes.c:8-10`

**Smell:** Same `#ifndef M_PI / #define M_PI 3.14159…` block in
three places. Other modules in the project keep this in `gl_repl.h`
(per CLAUDE.md: *"Minimal legacy header: standard includes and
`M_PI`"*). Three-place definition diverges from the canonical
location.

**Fix:** Include the project's canonical M_PI header (or
`gl_includes.h` if it has one) instead of redefining.

### 32. Postprocess header comments labels module "Experimental" / "Iteration 1"

**Where:** `src/scene/postprocess_filter.c:4`,
`postprocess_filter.h:2, 9`

**Smell:**
```
/* Iteration 1: chromatic aberration. ... */
/* postprocess_filter.h - Experimental scene-viewport post-processing. ... */
/* Experimental: not in the Config menu, not persisted via @cfg, no GlrConfigKey. */
```
Three independent admissions of unfinished status. Not bugs - but
"hidden Ctrl+N shortcut, not in Config menu" is unusual.

**Fix:** Either fold into the standard config pipeline or formalize
"experimental" with graduation criteria.

### 33. `scene_xn_init`/`set`/`show`/`tick` API is uneven

**Where:** `src/scene/scene_transition.h:33-51`

**Smell:** Four verbs (`init` = snap to theme; `set` = request
change; `show` = fade-in from off; `tick` = advance). Naming
asymmetry: a reader sees the four verbs and has to read the doc to
know which is which.

**Fix:** Rename for clarity (`scene_xn_snap_to_steady` /
`scene_xn_request_change` / `scene_xn_fade_in_from_off` /
`scene_xn_tick`) or add a one-line verb cheat-sheet at the top of
the header.

### 34. 🔀 Header docs use stale filename prefixes ("scene_geometry_guides.c", etc.)

**Where:** `src/scene/guides/geometry_guides.c:2`,
`geometry_guides.h:2`, `transform_guides.c:2`, `transform_guides.h:2`,
`transform_utils.h:2`, `guides_shared.h:2`

**Smell:** Files were renamed (per the `src/` restructure noted in
user memory) but doc-comments still say `scene_geometry_guides.c`
etc. Cosmetic but indicates the rename didn't get a thorough scrub.

**Fix:** One-line edit per header comment.

### 35. `palette.h` over-engineers a 32-token table where most tokens are used once

**Where:** `src/scene/palette.h:52-102`

**Smell:** Counted across `src/scene/` + `src/app/glr_ctrl.c`, the
maximum use-count for any single token is **2**; the median is **1**.
Each token is materially used in one file. The pattern adds: enum +
initializer array + 3 inline functions + `STATIC_ASSERT` count
guard + regression test + bucket policy documentation.

**Why it matters:** Every new color pays the tax (enum addition,
table addition, count bump, golden-test churn) for tokens that
mostly aren't shared.

**Fix:** Keep the central palette only for tokens shared *across*
files (a handful - `SCENE_CLR_OUTLINE_EDGE`,
`SCENE_CLR_ORBIT_GLOW_INNER`, paired normal/guide colors); demote
single-file tokens to file-local statics. Larger refactor; just
flagging.

## 🟢 Dead code / dead fields

### 36. 🔀 Redundant cleanup before `glPopAttrib(GL_ALL_ATTRIB_BITS)`

**Where:** `src/scene/lights.c:158-162`, `src/scene/grid.c:841-844`,
`src/scene/render.c` (around `draw_orbit_target`'s teardown)

**Smell:**
```c
/* lights.c:158 - before glPopAttrib */
glPointSize(1.0f);
glDisable(GL_BLEND);
glEnable(GL_DEPTH_TEST);
if (user_lighting_enabled) glEnable(GL_LIGHTING);
scene_lights_pop_state();   /* glPopAttrib(GL_ALL_ATTRIB_BITS) */
```
Pop restores all of these. Every restoration line is dead. The
`user_lighting_enabled` capture at `lights.c:42` (just to re-assert
state that pop handles) can also go.

`axes.c` has the inverse asymmetry: it doesn't manually `glDisable(GL_FOG)`
before pop while grid.c does at `:843`.

**Fix:** Either trust `glPopAttrib` everywhere (drop all manual
teardown) or document why each module differs. Sweep all three.

### 37. Trivial wrapper functions in three modules

**Where:** `src/scene/backdrop.c:47-53`, `lights.c:11-17`,
`grid.c:54-60`, `axes.c:33-39`, `render.c:104-110`

**Smell:** Five copies of:
```c
static void scene_X_push_state(void) { glPushAttrib(GL_ALL_ATTRIB_BITS); }
static void scene_X_pop_state(void)  { glPopAttrib(); }
```
Zero abstraction over a single GL call. No project convention
documented that wraps these.

**Fix:** Inline. Or, if the goal is `prof_begin(PROF_PUSH_ATTRIB)`
instrumentation, add that and document the seam.

### 38. Stack zero-init / dead defensive clamps in cityscape

**Where:** `src/scene/backdrop.c:198-203`

**Smell:**
```c
int wcols = 1 + (int)(bw / 0.65f);  /* bw in [0.9, 2.7] → wcols in [2, 5] */
int wrows = 1 + (int)(bh / 0.60f);
if (wcols < 1) wcols = 1;      /* unreachable */
if (wcols > 9) wcols = 9;      /* unreachable: max 5 */
if (wrows < 2) wrows = 2;      /* unreachable: min 3 */
if (wrows > 14) wrows = 14;    /* boundary only */
```
Three of four clamps are unreachable given the building-size ranges
at the file top.

**Fix:** Drop the dead clamps; convert to `assert()` so future
range changes trip the guard rather than silently bumping through
dead branches.

### 39. `transform_source_unmodified` unused parameter (covered in #9)

Pure delete after #9's fix.

### 40. `guides_shared.h` exposes `flat_program` to consumers that don't use it

**Where:** `src/scene/guides/guides_shared.h:31`

**Smell:** Only `transform_guides.c` reads `flat_program`;
`geometry_guides.c` never does. The shared struct couples both
renderers to `repl/flatten.h`. Conversely, `normal_base_pos` /
`vertex_args` are geometry-only.

**Fix:** Either accept the unified-snapshot trade-off (current -
fine) or split into `SceneVertexGuideSnapshot` /
`SceneTransformGuideSnapshot` with a `SceneGuideHeader` for the
four common fields.

### 41. 🔀 `user_lighting_enabled` redundant `glEnable(GL_LIGHTING)` before pop

**Where:** `src/scene/guides/geometry_guides.c:148, 307`,
`transform_guides.c:273, 460, 616`

**Smell:** Every guide draw function ends with:
```c
if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
glPopAttrib();
```
`glPopAttrib(GL_ALL_ATTRIB_BITS)` restores lighting; the explicit
enable is dead. Some reader will conclude the push doesn't cover
lighting (wrong) and try to "fix" the asymmetry.

**Fix:** Remove the explicit enable. The `user_lighting_enabled`
snapshot field may then become unused - verify before dropping it
from the header too.

### 42. `ACCUM_STEP_COUNT` exported on `render.h` but unused inside scene

**Where:** `src/scene/render.h:24`

**Smell:** `#define ACCUM_STEP_COUNT 5` is the controller's ladder
length (1, 2, 4, 8, 16 is 5 steps). Scene code uses
`MAX_ACCUM_SAMPLES`, not the step count. Belongs on the controller
side.

**Fix:** Move to `glr_state.h` / config or an actions header.

### 43. Stale comments describing the removed tess-preview overlay

**Where:** `src/scene/render.c:413-418, 514-519`

**Smell:** Two comment blocks describe code that moved out. Useful
design rationale but they document an absence; a reader grepping
for "tess preview" finds an absence.

**Fix:** Move to `docs/scene-history.md` or delete after the
boundary has bedded in.

### 44. `init_g_saved_matrix_mode = 0` (GL_NONE) is an invalid initial value (covered in #17)

The fix in #17 removes the static; this becomes moot.

## 🔵 Structural concerns

### 45. `scene_grid_render` is a 156-line god-function with 4 unrelated concerns

**Where:** `src/scene/grid.c:691-846`

**Smell:** Input validation → clamp → FOG-style alpha knee → GL state push
→ index/clamp → context build → FAR fog → theme switch → teardown.
Each phase has its own `#if GRID_XN_STYLE` branch, and the
file-scope `g_xn_*` statics tie them together.

**Fix:** Extract `grid_xn_resolve_alpha(opacity, has_own_fog) → {alpha, opacity}`,
`grid_setup_blend_depth(config)`, `grid_build_draw_context(config) → GridDrawContext`,
`grid_apply_distance_fog(config, extent, opacity)`. Drops
`scene_grid_render` to ~30-40 lines.

### 46. `draw_cityscape` is a 224-line god-function with copy-pasted box geometry

**Where:** `src/scene/backdrop.c:72-295`

**Smell:** GL setup + fog config + per-building math + tier-1 box
rendering (5 quads, 21 lines) + tier-2 box rendering (almost
identical 5-quad block at `:146-167` vs `:177-195`) + window-grid
sizing + per-window color/light + per-window quad emission. Nested
loop depth 3 (`bi → wc → wr`).

**Fix:** Extract `setup_city_gl_state(int nv_fog_distance_supported)`,
`draw_building_box(corners, y0, y1, base_color)`,
`draw_building_windows(geom, anim_time, base)`. `draw_cityscape`
becomes the outer seed/scatter loop. Same treatment makes
`draw_starry_sky` (95 lines, 4-band loop) more readable.

### 47. `draw_rotate_guide` is a 155-line god-function with three responsibilities

**Where:** `src/scene/guides/transform_guides.c:464-618`

**Smell:** (a) Builds rotation matrix + sweeps arc, (b) builds
helix for origin-rotation case, (c) animates a pulse along the arc.
Three nested visual languages in one function. The `use_helix`
switch (`:505`) effectively splits it into two unrelated geometry
generators sharing only the post-render pulse animation.

**Fix:** Extract `build_rotate_arc(...)` and `build_rotate_helix(...)`;
the pulse animation that consumes `arc[]` stays.

### 48. `render_3d_scene_pass` is 80 lines and 5 distinct phases

**Where:** `src/scene/render.c:521-602`

**Smell:** setup / fill / post-fill / helpers / overlays, with
profiling sprinkled throughout. Not a god-function but harder to
skim than necessary.

**Fix:** Extract per-phase helpers, each owning its prof bracket.
The outer function shrinks to ~25 lines that read as the high-level
frame layout.

### 49. 🔀 Near-duplicate orthonormal-basis construction repeated three times in transform_guides

**Where:** `src/scene/guides/transform_guides.c:202-212, 280-290, 519-533`

**Smell:** Three near-identical Gram-Schmidt blocks (`draw_translate_guide`,
`draw_arrow_head`, partial in `draw_rotate_guide`). The `0.9f`
axis-aligned threshold is a magic number repeated in all three.

**Fix:** Extract `make_arrow_basis(const float dir[3], float r[3], float b[3])`
static helper. Drop the parallel arrowhead loop in `draw_translate_guide`
(predates `draw_arrow_head`).

### 50. Arrowhead clamp duplicated three times - third copy uses different magic numbers

**Where:** `src/scene/guides/transform_guides.c:214-216, 355-357, 430-432`

**Smell:** Two sites use the `TG_HEAD_LEN_*` macros; the third
(scale-axis branch) hard-codes `0.2f / 0.05f` (vs the macros'
`0.25f / 0.06f`). Either the macros are wrong, the inline values
are wrong, or the discrepancy is intentional and undocumented.

**Fix:** Extract `clamp_head_len(float dlen)` using the macros. If
the axis branch is intentionally tighter, give it its own named
constants and a one-line comment.

### 51. Copy-paste plane drawers (`draw_guide_yz_plane` / `xz_plane` / `xy_plane`)

**Where:** `src/scene/guides/geometry_guides.c:17-56`

**Smell:** Three near-identical 13-line functions differing only in
which slot of the `GL_QUADS` vertex gets the constant `v` and which
palette tokens are bound.

**Fix:** Single `draw_guide_axis_plane(int free_axis, float v, float sz, float as)`
with a `[3][3]` basis table and a `SceneColorToken fill_tok[3]` /
`edge_tok[3]` indexed by `free_axis`.

### 52. Axes inlines every theme; grid extracts custom themes - pattern split

**Where:** `src/scene/axes.c:266-505` vs `src/scene/grid.c:792-838`

**Smell:** Axes has `case X: { ... inline body ... break; }` for
every theme; grid extracts custom themes into named functions. Same
shape, different conventions.

**Fix:** Apply grid's pattern uniformly to axes. Drops
`scene_axes_render` to ~30 lines.

### 53. 🔀 Bitmap-text loop repeated 4 times across scene files

**Where:** `src/scene/lights.c:122-128, 149-154`,
`src/scene/overlays.c:20-23`, `src/scene/render.c:497-502`

**Smell:** Four near-identical:
```c
char label[N];
snprintf(label, sizeof(label), "...");
glRasterPos3f(...);
for (const char *c = label; *c; c++)
    glutBitmapCharacter(FONT_X, (unsigned char)*c);
```
Variation is font (`FONT_MONO` vs `FONT_SMALL`) and the format.

**Fix:** `scene_draw_bitmap_text(font, x, y, z, str)` helper. Also
forces a decision: `overlays.c::scene_draw_vertex_number_label`
uses `FONT_MONO` while `lights.c::scene_lights_render` uses
`FONT_SMALL` for what is essentially the same purpose - neither
file documents why.

### 54. `scene_apply_camera` takes 6 positional floats - easy to swap

**Where:** `src/scene/render.c:381-389`, `render.h:35-36`

**Smell:** `(float rx, float ry, float dist, float tx, float ty, float tz)`
- six floats, no compile-time check that `dist` isn't a target
coordinate. The struct fields exist on `SceneRenderConfig`; the
caller hand-passes six floats out.

**Fix:** Take `const SceneRenderConfig *` (already carries the
fields), or a small `SceneCameraPose` struct.

### 55. 🔀 Magic numbers everywhere

**Where:**
- `src/scene/grid.c:318, 459-460, 529, 646, 668, 678, 732` - fade
  radius, surface step, tick lengths, RADAR segments, ping/sweep
  speeds, z-fight nudge
- `src/scene/axes.c:280, 298, 418, 489` - pulse speed, trail
  length, gizmo plane size, ruler tick lengths
- `src/scene/guides/transform_guides.c` - many lines of alpha
  values (`0.40f * as`, `0.95f`, `0.85f`, ...), point sizes
  (`8.0f`, `6.0f`, `7.0f`, ...), line widths
  (`2.0f`, `3.0f`, `3.5f`, ...), pulse parameters
  (`0.6f`, `0.8f`, `0.2f`)
- `src/scene/backdrop.c:312` - `(GLfloat[]){1, 0, 0.00}` mixes
  three numeric forms
- `src/scene/guides/transform_guides.c:467-468` - `while (angle_deg > 720.0f) angle_deg -= 360.0f;`
  brute-force angle wrap

**Smell:** Per-theme tuning knobs are bare literals. Hard to skim,
hard to tune, hard to consistency-check.

**Fix:** Add per-theme constants (e.g., `GRID_FOCUS_FADE_RADIUS`,
`GRID_RADAR_SEG`, `GRID_OCEAN_SURFACE_STEP`,
`TG_PULSE_SPEED`, `TG_PULSE_GLOW_AMP`, `TG_POINT_SIZE_TIP`). Replace
brute-force wrap with `fmodf(angle_deg, 720.0f)` plus a one-line
comment on the 2-turn allowance.

### 56. `scene_apply_projection` recomputes `tan(SCENE_DEFAULT_FOVY_DEG * M_PI / 360.0)` three times

**Where:** `src/scene/render.c:288, 311, 326`

**Smell:** Three `tan()` calls per pass (16× per frame at 16× AA).
Trivially hoisted to a single local.

**Fix:** Hoist into a single local at function entry, or
`#define SCENE_HALF_FOVY_TAN tan(SCENE_DEFAULT_FOVY_DEG * M_PI / 360.0)`.

### 57. `post_fill_fn` / `post_overlays_fn` callback names mislead

**Where:** `src/scene/render.c:563-598`

**Smell:** `post_overlays_fn` runs after `scene_lights_render` - but
"overlays" in this codebase canonically means vertex labels, normal
arrows, polygon outlines, cursor guides. Those used to be in scene
code but moved upward; the name "post_overlays_fn" now means "where
the caller installs their own overlays."

**Fix:** Rename to `pre_helpers_fn` (between fill and grid/axes/
backdrop) and `post_lights_fn` (between lights and final pop) -
pipeline-position names.

### 58. `validate_render_config` uses `goto bad` in a file that otherwise uses straight returns

**Where:** `src/scene/render.c:64-95`

**Smell:** Tab-aligned `goto bad` is the only one of its kind in
the file. Stylistic outlier.

**Fix:** Inline `errno = EINVAL; return -1;` or wrap in a `BAD()`
helper macro for consistency. (Or leave - defensible pattern, just
note as outlier.)

### 59. `SCENE_PROBE_BOX` is `#define`d to `SCENE_DEFAULT_FAR_Z` by chain

**Where:** `src/scene/render.c:27-28`

**Smell:** Two concepts (probe box size, far plane) linked by macro
chain rather than explicit derivation. If someone bumps the far
plane, the probe box silently grows; the `fb[96*1024]` budget
(magic; see #60) may not keep up.

**Fix:** Make relationship explicit (`#define SCENE_PROBE_BOX (SCENE_DEFAULT_FAR_Z * 1.0)`)
and `STATIC_ASSERT(SCENE_PROBE_BOX >= SCENE_DEFAULT_FAR_Z)`.

### 60. Magic `static GLfloat fb[96 * 1024]` probe buffer

**Where:** `src/scene/render.c:157`

**Smell:** `96 * 1024` floats with no named constant. The comment
notes size and overflow but the constant isn't searchable.

**Fix:** `#define SCENE_PROBE_FEEDBACK_FLOATS (96 * 1024)`.

### 61. `scene_xn_init` and `scene_xn_show` could share more

**Where:** `src/scene/scene_transition.c:4-10, 27-33`

**Smell:** Differ only in `phase`/`opacity` initial value.

**Fix:** Optional `seed(s, theme, phase, opacity)` helper. File is
58 lines; probably not worth it.

### 62. Inline declarations inside `glBegin/glEnd`

**Where:** `src/scene/lights.c:107-119`

**Smell:**
```c
glBegin(GL_LINES);
float dirs[][3] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
};
for (int r = 0; r < 6; r++) { ... }
glEnd();
```
Legal C99 + legal GL, but easy to misread as vertex data.

**Fix:** Hoist the table outside the begin/end (mark `static const`).

### 63. `STATIC_ASSERT(SCENE_CLR_COUNT == 32, ...)` is brittle count check

**Where:** `src/scene/palette.h:166-167`

**Smell:** Requires bumping the literal `32` every time a token is
added - three places to touch instead of one.

**Fix:** `STATIC_ASSERT(sizeof(g_scene_palette)/sizeof(g_scene_palette[0]) == SCENE_CLR_COUNT, ...)`
- "table matches enum" instead of "enum matches a hand-maintained
literal."

### 64. Inconsistent alias style for matrix elements

**Where:** `src/scene/render.c:349`

**Smell:**
```c
double or_ = ortho_right + ortho_dx;
```
The block has `ol`, `or_`, `ob`, `ot`, `pl`, `pr`, `pb`, `pt`
(abbreviated because `or` is a C++ keyword/iso646.h alternative).
Abbreviation density is high relative to surrounding
`persp_top`/`ortho_right` style.

**Fix:** Spell them out or keep + add a one-line alias-table comment.

### 65. Stale `init_gl` doc-comment

**Where:** `src/scene/render.h:26-28` vs implementation at `render.c:53-56`

**Smell:** Header promises "create display lists, compile shaders,
allocate tessellator, set up default light state." Body only calls
two helpers. No shaders (fixed-function), no tessellator allocation,
no display lists.

**Fix:** Update header doc to reflect actual behavior.

## Sequencing

### One-afternoon pass - ✅ landed (2026-05-25)

Every item in this pass shipped across commits `d30ff28`, `b2739b8`,
`905cb0d`, `5dd0e65`, `dc6ed83`. The original ordering is preserved
below for reference.

1. **#7** ✅ - `glPointParameterfv` identity coefficients turned out
   to be load-bearing (overrides the init bootstrap's non-identity
   default to keep stars at consistent sizes). Comment rewritten;
   call kept.
2. **#10** ✅ - Explicit `<math.h>` / `<string.h>` / `<ctype.h>` added
   to guide files; #14 followed with `gl_includes.h` in `lights.c`.
3. **#9** ✅ - Param dropped, renamed to
   `transform_input_matches_committed`.
4. **#18** ✅ - `post_filter_mode` is `ScenePostFilterMode`;
   `scene_grid_theme_uses_fog` parameter is `SceneGridTheme`;
   `scene_postprocess_filter_*` signatures updated.
5. **#19** ✅ + **#20** ⏸️ + **#42** ✅ - `viewport_w/h` and
   `ACCUM_STEP_COUNT` removed. `grid_xn_phase`/`axes_xn_phase` kept
   for tests (referenced-only fields still pass through `glr_ctrl`).
6. **#21** ✅ - Duplicate `focus` dropped from
   `SceneFrameRenderContext`.
7. **#34** ✅ - Filename-prefix sweep across all scene headers and
   sources (filenames + `scene_render.c` body references).
8. **#36** ✅ - Trusted `glPopAttrib(GL_ALL_ATTRIB_BITS)` everywhere;
   manual teardown removed from lights / grid / axes / orbit
   target.
9. **#37** ⏸️ - Skipped; the wrappers stay until #15 decides the
   per-pass attribute mask (inlining now would have to be reverted
   then).
10. **#38** ✅ - Three unreachable wcols/wrows clamps trimmed; the
    one boundary-effective clamp stays.
11. **#41** ✅ - Redundant `glEnable(GL_LIGHTING)` removed from 5
    transform-guide / geometry-guide draws + lights + grid + axes.
12. **#30** ⏸️ - `y0`/`y1` shadowing math.h Bessel functions still
    open; cosmetic.
13. **#31** ✅ - `M_PI` fallback moved to `gl_includes.h`; local
    `#ifndef M_PI` blocks dropped from `gl_repl.h`, `glr_camera.c`,
    and all four scene files.
14. **#13** ✅ - `"scene/foo.h"` → `"foo.h"` swept where in-dir
    (`lights.c`, `scene_transition.c`). Guides files stay
    `"scene/foo.h"` because palette.h / occluded_ghost.h are
    one directory up.
15. **#43** ✅ + **#65** ✅ - Stale comments / doc-comments cleaned
    (tess-preview and replay-fade comment paragraphs removed;
    `scene_render_init_gl` header rewritten to match body).

Shipped: ~13 commits, ~150 LOC net reduction. One reframed bug
(#7's reset is load-bearing, not no-op) plus #8's real underwater
push/pop bug fixed.

### One-week pass - hidden-state cluster ✅ closed

| Item | Status | Notes |
|---|---|---|
| **#4** g_guide_alpha_mul | ✅ `3023624` | alpha_mul threaded through 5 helpers; `tg_color_tok` inlined to `scene_clr_a` |
| **#17** g_saved_matrix_mode | ✅ `08fc94b` | begin_2d returns saved mode; end_2d accepts it back |
| **#2** SceneExecutePurpose | ✅ `08fc94b`, `49bf979` | enum on ctx + REPL adapter snapshot/restore around non-MAIN_FILL purposes (review caught the adapter ignored ctx in the initial commit, so the probe still mutated predef/scratch/render state) |
| **#3** g_xn_alpha / g_xn_opacity | ✅ `cc481c2` | shared `scene_overlay_xn_resolve` in overlay_xn.h; GridDrawContext.xn_alpha + new AxesDrawContext; ~60 gl_color → grid_color / axes_color sweep |
| **#5** g_active_projection statics | ✅ `89e2b17` | caller-owned `SceneRendererState` (controller + scene_demo each hold one static); state-init API + threaded through scene_render_3d_scene + scene_get_active_projection |
| **#11** scene_apply_camera | ✅ `6a6a4cd` | moved to `src/app/glr_camera.c` as `glr_camera_load_modelview`; `SceneCameraPose` → `GlrCameraPose`; scene_demo inlines its own matrix calls to preserve the boundary proof |
| **#12** scene_apply_projection split | ✅ `89e2b17` | pure `scene_compute_active_projection` runs once before the AA loop (sole writer to state); per-sample apply is read-only |

### One-week pass - parity-drift cluster (partial)

| Item | Status | Notes |
|---|---|---|
| **#1** drift test for transform dispatch | ✅ `2d1b9b6` | covers both `apply_tracked_transform` and `repl_executor_apply_tracked_transform_cmd` against `repl_cmd_is_transform`; uses GL stub call counters |
| **#24** repl_cmd_starts_geometry_emit | ✅ `e9225cd` | predicate promoted to `src/repl/command.h` next to existing ones; old inline chain dropped; test pins the expected set |
| **#23** standard-theme switch | ✅ `e9225cd` | custom themes still get explicit cases; standard themes fall through to `default:` which does the spec-table lookup |
| **#22** theme spec ABI | ⏸️ deferred | Design decision: widen `GridThemeSpec` (per-cell color fn, optional setup/teardown hooks) to absorb the custom themes vs drop the table and have N per-theme functions |

### One-week pass - god-function cluster (deferred)

Every item in this pass remains open. The duplication helpers landed
in `709455f` (#49-#51) and `c2e7009` (#53) are partial scaffolding
that each split would extract anyway, so the splits are slightly
smaller than the original line counts suggest.

1. **#45** ⏸️ - Split `scene_grid_render` (156 lines) into
   resolve-alpha / setup / build-ctx / fog / theme-dispatch /
   teardown helpers.
2. **#46** ⏸️ - Split `draw_cityscape` (224 lines) into city-state /
   box draw / window draw helpers.
3. **#47** ⏸️ - Split `draw_rotate_guide` (155 lines) into
   arc-builder / helix-builder + shared pulse animator.
4. **#48** ⏸️ - Split `render_3d_scene_pass` (80 lines, 5 phases).
5. **#52** ⏸️ - Apply grid's per-theme-function extraction pattern
   uniformly to axes.

### One-week pass - duplication cluster (partial)

| Item | Status | Notes |
|---|---|---|
| **#49** make_arrow_basis | ✅ `709455f` | shared Gram-Schmidt helper; draw_translate_guide and draw_arrow_head both use it |
| **#50** clamp_head_len | ✅ `709455f` | helper + axis-branch constants (`TG_HEAD_LEN_AXIS_MIN/MAX`) named separately |
| **#51** draw_guide_axis_plane | ✅ `709455f` | single helper replaces yz/xz/xy_plane triplet; per-axis tokens indexed by `k_guide_plane_fill[]`/`edge[]` |
| **#53** scene_draw_bitmap_text | ✅ `c2e7009` | helper exposed via `overlays.h`; replaces 4 raster+for loops in lights / overlays / render |
| **#29** clamp01f helper | ✅ `08fc94b` | `scene_clamp01f` in `render_types.h`; grid+axes opacity-clamp triplets replaced |
| **#55** magic numbers | ⏸️ deferred | Large surface; per-theme constants are the natural home but each renderer needs its own pass |

### One-week pass - boundary tightening (partial)

| Item | Status | Notes |
|---|---|---|
| **#15** targeted attrib masks | ⏸️ deferred | Invasive across backdrop / lights / postprocess; ties into **#37** |
| **#16** NV_fog save/restore | ✅ `c2e7009` | `glGetIntegerv(GL_FOG_DISTANCE_MODE_NV)` snapshot + tail restore in backdrop *and* grid (the audit only flagged backdrop but grid had the same pattern) |
| **#27** + **#28** accum_samples ladder | ✅ `c2e7009` | now enforces `{1, 2, 4, 8, 16}` whenever AA is on |
| **#33** scene_xn API names | ✅ `08fc94b` | Cheat-sheet added to header rather than renaming (audit's lower-cost option) |
| **#32** postprocess graduation | ⏸️ deferred | Scope question - fold into standard cfg pipeline vs formalize "experimental" |
| **#35** palette token review | ⏸️ deferred | Design decision: which tokens are genuinely shared (worth the central table) vs file-local |

### Out of scope

- The `scene_demo` boundary proof is doing its job; the audit
  confirmed no `editor/` / `repl/` / `ui/` includes in any scene
  file. Don't introduce them.
- `scene_transition.c` is **already** pure as advertised - no GL,
  no globals, no stdlib. The naming concern (#33) is the only smell.
- `occluded_ghost.h` is a tidy 33-line constants header consumed by
  five modules - earns its centralization. Don't touch.
- `overlays.c` is exactly what its header advertises (per-vertex
  labels + normal arrows). The "ownership escaped upward" history
  is accurately reflected in the file comment. Don't refactor
  here; the larger overlay-extraction work is in the `src-app`
  audit (#14 there).
- `themes.h` is appropriately small. The unenforced "tables must
  match enum order" invariant is worth a follow-up
  `STATIC_ASSERT` per table but is out of scope.

## Method note

This audit was produced by four parallel review agents:

- `render.{c,h}` + `render_types.h` - the frame orchestration
  contract
- `grid.{c,h}` + `axes.{c,h}` + `scene_transition.{c,h}` +
  `themes.h` - environment + transitions (the biggest file in
  the directory, `grid.c` at 846 lines, plus the second-biggest,
  `axes.c` at 514)
- `backdrop.{c,h}` + `lights.{c,h}` + `overlays.{c,h}` +
  `postprocess_filter.{c,h}` + `palette.h` + `occluded_ghost.h` -
  visual content
- `guides/` subtree - REPL-aware overlay passes (the biggest file
  in `guides/`, `transform_guides.c` at 764 lines, plus
  `geometry_guides.c`, `transform_utils.h`, `guides_shared.h`)

Each agent was asked for ~15-25 highest-signal findings, with
particular attention to layering violations (none found), GL state
hygiene, and the parity contracts between scene and executor. The
most actionable claims (real-bug findings above) were verified
against the source. The 🟡 / 🟢 / 🔵 findings are reported as the
agents framed them; spot-check before acting on the more
mechanical ones.

The headline outcome of the audit: **the documented `src/scene`
boundary holds**. The bugs cluster around module-private mutables
(`g_active_projection`, `g_guide_alpha_mul`, `g_xn_*`), parity drift
between scene and executor (`transform_utils.h` vs `executor.c`,
`is_geometry_emit_cmd` vs the canonical predicate set), and god-
functions in the largest files. None of those threaten the
`scene_demo` layer-independence proof.
