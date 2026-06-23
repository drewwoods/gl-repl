# Accumulation-Buffer Motion Blur

## Context

The REPL already uses the accumulation buffer for antialiasing: a single
5-state "Accum AA" config (Off/2x/4x/8x/16x, F2) drives a jitter loop inside
`scene_render_3d_scene()`. We want to reuse that accum machinery for **motion
blur** — blurring camera movement and/or the `t` animation across sub-frames.

Motion blur is expensive (it re-renders the scene N times with no temporal
reuse), so it must be **off by default**. The "Accum AA" setting is reworked
into two config items:

- **Accum effect**: `Off | AA | Blur` (3 states, F2)
- **Accum passes**: `1 | 2 | 4 | 8 | 12 | 16` (6 states, Ctrl+= / Ctrl+−)

Blur behavior per frame:
- **Camera blur** — if the camera matrix changed since last frame, interpolate
  the previous↔current camera pose across the N passes.
- **Time blur** — otherwise, if `t` is playing, sample `t` over the trailing
  16 ms window `[t−dt, t]`, one sub-step per pass.
- **Fallback** — if `t` is paused *and* the camera is unchanged, fall back to
  AA jitter (desired, low-cost; falls out of the design for free).

The final image is an equal-weight blend of the N accumulated passes.

## Decisions (from user)

- **Keybinding:** F2 cycles the *effect* (Off→AA→Blur; Shift+F2 reverse).
  Repurpose the existing Ctrl+= / Ctrl+− accum fine-adjust to step *passes*.
- **Backward compat:** legacy `@cfg accum_aa = Nx` lines are silently ignored
  on load (unknown-key behavior). No alias code. Note the break in the PR.
- **Default effect:** `Off` (blur and AA both cost per frame).

## How animation actually works (critical correctness facts)

Two facts, verified in source, that drive the design:

1. **Animation = reflatten-per-frame, not execute-time re-eval.** The executor
   consumes *baked* `flat_cmds[].args` for nearly all commands (e.g.
   `CMD_VERTEX3F`, executor.c:542); only control-flow conditions re-evaluate at
   execute time. The flat program is rebuilt every frame when `g_flat_dirty` is
   set — `repl_state_time_advance` sets it each playing frame (glr_ctrl.c:1333 →
   `repl_flatten_commands`). So **a time sub-step must reflatten** at the
   sub-step `t`; merely poking predef `t` will not move baked geometry.
2. **Main-fill execution is not isolated.** `scene_execute_adapter` snapshots /
   restores predef+scratch+render only for *non*-main-fill purposes
   (glr_ctrl.c:638). A program with `A[0]=A[0]+1` or `t=t+1` style state mutates
   predef/scratch/render during a pass, so **each accumulation sub-pass must be
   reset to a shared baseline** or samples leak/compound into each other.

## Architecture decision

Keep the accumulation loop **in the scene** (`scene_render_3d_scene`), where the
accum buffer, per-pass clears, `glAccum`, and the once-per-frame `post_filter`
already live. Inject per-pass policy via a new **callback** that receives a
**mutable per-pass copy of the config**, mirroring the existing `execute_fn` /
`post_fill_fn` injection idiom. The scene stays camera/REPL-agnostic; the
controller (owns camera + `t`) supplies the per-pass policy.

For `effect==Blur` the scene, per pass, makes a local `SceneRenderConfig
pass_cfg = *config;`, calls
`setup_subframe_fn(ud, pass_idx, pass_count, &pass_cfg)`, recomputes the active
projection from `pass_cfg`, and renders that pass with jitter=0. The controller
callback:
- **resets predef/scratch/render to the frame baseline** (per-pass isolation, P2),
- **camera blur**: writes the interpolated pose into `pass_cfg->cam_*` *and*
  loads the interpolated modelview — so grid/axes/orbit-target/light-indicators
  and the ortho projection (which read `cam_*` / are recomputed from `pass_cfg`)
  **blur with the camera**, per the user's requirement;
- **time blur**: sets the predef-`t` sub-step and **reflattens** at that `t`
  (P1) — camera `cam_*` left at the current pose.

When the callback is `NULL` (paused + unchanged-camera), the scene uses the AA
jitter path — that *is* the fallback.

## Changes

### 1. Scene render contract — `src/scene/render_types.h`

Add an effect enum and replace the accum fields:
```c
typedef enum SceneAccumEffect {
    SCENE_ACCUM_EFFECT_OFF = 0,
    SCENE_ACCUM_EFFECT_AA,
    SCENE_ACCUM_EFFECT_BLUR,
} SceneAccumEffect;
```
In `SceneRenderConfig` (replace `accum_aa_enabled` / `accum_samples` at lines
161-163; keep `use_accum`). The callback takes the **mutable per-pass config**
so it can write interpolated `cam_*`:
```c
int use_accum;          /* --noaccum master gate */
int accum_effect;       /* SceneAccumEffect */
int accum_passes;       /* resolved pass count: 1,2,4,8,12,16 */
void (*setup_subframe_fn)(void *user_data, int pass_idx, int pass_count,
                          struct SceneRenderConfig *pass_config);
void  *setup_subframe_user_data;
```

### 2. Scene loop — `src/scene/render.c` (`scene_render_3d_scene`, 711-725)

Generalize the loop: `do_accum = use_accum && accum_effect != OFF && passes > 1`.
Per pass clear COLOR|DEPTH; then:
- `effect==BLUR && setup_subframe_fn`: `SceneRenderConfig pass_cfg = *config;`
  call `setup_subframe_fn(ud, p, passes, &pass_cfg)`, then
  `scene_compute_active_projection(state, &pass_cfg)` (so ortho scale follows
  the interpolated `cam_dist`), then `render_3d_scene_pass(state, &pass_cfg, 0, 0)`.
- else (AA): `render_3d_scene_pass(state, config, g_jitter_table[p % MAX_ACCUM_SAMPLES]…)`.

`glAccum(GL_ACCUM, 1/passes)` per pass, `glAccum(GL_RETURN, 1)` after. The
once-before-loop `scene_compute_active_projection` (render.c:709) stays for the
AA/non-blur paths; the blur branch recomputes per pass from `pass_cfg`. The
jitter table already supports 12 (first 12 of 16 entries).

`validate_render_config` (render.c:97) is **reject-only on a const config** — do
not "clamp". Add: `accum_effect ∉ [0,2]` → fail (always). Validate the
`accum_passes` ladder `{1,2,4,8,12,16}` **only when accumulation is active**
(`use_accum && accum_effect != OFF`), mirroring the grid block's
`grid_theme != GRID_THEME_OFF` guard (render.c:109). This keeps the
`memset(0)` config path valid for non-accum callers — `tools/scene_demo/scene_demo.c`
(scene_demo.c:169 builds via memset and never sets accum fields, then calls
`scene_render_3d_scene` at :361) needs **no change**: effect=OFF ⇒ passes
unchecked. The existing `tests/test_scene_render.c` setup is the one place that
turns AA on, so it must set `accum_passes` (see Tests).

### 3. Camera helpers — `src/app/glr_camera.{h,c}`

Reuse the existing static `shortest_angle_delta` (glr_camera.c:148) and the
`tick_target_ease` lerp math (184-189). Add two exported helpers:
```c
GlrCameraPose glr_camera_pose_lerp(const GlrCameraPose *a,
                                   const GlrCameraPose *b, float f);
int           glr_camera_pose_changed(const GlrCameraPose *prev,
                                      const GlrCameraPose *cur);
```
`pose_lerp` interpolates rx/ry via `shortest_angle_delta`, dist/tx/ty/tz
linearly. `pose_changed` compares with epsilons (reuse the existing
`CAM_TARGET_*_EPS` values). Keep `shortest_angle_delta` static (both helpers
are same-TU).

### 4. Transient time setter — impl `src/repl/state.c`, decl `src/repl/state_owners.h`

`repl_state_time_set` is unusable here (it overwrites `g_anim_time` and sets
`g_flat_dirty`). Add a setter that writes only the predef-`t` slot; the **caller
then reflattens** at that `t` (this is what actually re-bakes geometry — see P1).
Declare it in **`state_owners.h`** alongside the other mutable time accessors
(`repl_state_time_advance/_reset_to_zero/_set`, state_owners.h:62-64), not the
read-only `state.h` facade:
```c
/* Sets only predef 't' (no g_anim_time, no g_flat_dirty). For a motion-blur
 * sub-pass: set the sub-step t, then call repl_flatten_commands() to re-bake
 * the flat program at that t before the sub-pass renders. */
void repl_state_time_set_transient(float value);  /* writes g_predef_vars_mut[g_t_var_idx].value */
```

### 5. Controller wiring — `src/app/glr_ctrl.c`

- `GlrSubframeCtx` (file-static `g_subframe_ctx`) carries the per-pass policy
  **and the frame baseline** the callback resets to each pass (P2):
  ```c
  typedef enum { BLUR_NONE, BLUR_CAMERA, BLUR_TIME } GlrBlurMode;
  typedef struct {
      GlrBlurMode   mode;
      GlrCameraPose prev, cur;     /* BLUR_CAMERA endpoints */
      float t_end, dt;             /* BLUR_TIME window [t_end-dt, t_end] */
      int   t_var_idx, edit_line;  /* for set-t + reflatten */
      float          base_predef[MAX_PREDEF_VARS];
      float          base_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
      ReplRenderState base_render; /* light_enabled_mask / clear_color etc. */
  } GlrSubframeCtx;
  ```
  Plus `g_prev_frame_pose` (+`_valid`) and `g_cur_frame_pose`.
- Replace the inline camera-load block (1424-1428): capture
  `g_cur_frame_pose = glr_camera_pose_from_state(&cam)` and load it as the base
  modelview.
- Resolve blur mode just before the render call (in/near
  `glr_ctrl_build_scene_config`), only when `accum_effect==BLUR && use_accum &&
  passes>1 && !replay_active()`: `BLUR_CAMERA` if `prev_valid &&
  glr_camera_pose_changed(prev,cur)`, else `BLUR_TIME` if
  `repl_state_variables().time_playing`, else `BLUR_NONE`. For `BLUR_NONE` leave
  `setup_subframe_fn = NULL` (scene → AA jitter fallback).
- **Replay degrades blur → AA (user directive).** When `replay_active()`, mode
  is forced to `BLUR_NONE` so `setup_subframe_fn` stays NULL; with
  `accum_effect ∈ {AA, Blur}` the scene's `do_accum` branch then renders the AA
  jitter path. This sidesteps the reflatten/replay-clip conflict entirely:
  per-subpass `repl_flatten_commands()` would reset the flat count to the full
  program (flatten.c:731), clobbering the replay-narrowed count set by
  `replay_prepare_frame` (glr_ctrl.c:1380) that `scene_execute_adapter` renders
  (glr_ctrl.c:640). So during replay, AA-or-Blur ⇒ AA; Off ⇒ no accum.
  When blur is active, fill `g_subframe_ctx`: poses, `t_end` (current predef t),
  `dt = GLR_FRAME_DT_SECS`, `t_var_idx`, `edit_line`, and **snapshot the baseline**
  via `repl_copy_predef_values(base_predef,…)`, `repl_eval_copy_scratch_arrays(base_scratch)`,
  `base_render = repl_state_render()` (reuse the same helpers
  scene_execute_adapter uses).
- Add `glr_ctrl_setup_subframe(ud, p, N, pass_cfg)`; `f = N>1 ? p/(N-1) : 0`
  (endpoints inclusive). First, **reset to baseline** every pass:
  `repl_restore_predef_values(base_predef,…)`, `repl_eval_restore_scratch_arrays(base_scratch)`,
  `*repl_state_render_mut() = base_render`. Then:
  - **BLUR_CAMERA**: `pose = glr_camera_pose_lerp(&prev,&cur,f)`; write
    `pose.{rx,ry,dist,tx,ty,tz}` into `pass_cfg->cam_rx/ry/cam_dist/cam_tx/ty/tz`;
    `glr_camera_load_modelview(&pose)`.
  - **BLUR_TIME**: `repl_state_time_set_transient(t_end - dt*(1-f))` then
    `repl_flatten_commands(edit_line)` to re-bake the flat at the sub-step
    (modelview/`cam_*` already at current pose from frame top / `*config`).
- Populate `config->accum_effect/accum_passes/setup_subframe_fn/_user_data` in
  the build site (replaces lines 782-784).
- After the render call, **mark the flat program dirty** so the next consumer
  rebuilds at the true frame `t` (time blur left it baked at the last sub-step);
  the frame-level `repl_restore_predef_values` (1524) already restores predef.
- At the **end** of `glr_ctrl_display_frame`, capture
  `g_prev_frame_pose = g_cur_frame_pose; g_prev_frame_pose_valid = 1;`.

**Cost note:** time blur reflattens + re-executes N times per frame; camera blur
re-executes N times. Inherent to motion blur; gated off by default.

### 6. Config plumbing

- **`src/app/glr_config.h`**: replace `GLR_CONFIG_ACCUM_AA` with
  `GLR_CONFIG_ACCUM_EFFECT` and `GLR_CONFIG_ACCUM_PASSES`.
- **`src/app/glr_state.h`** (GlrRenderState, 71-73): replace
  `accum_aa_enabled`/`accum_samples` with `accum_effect` (0/1/2) and
  `accum_passes` (actual count 1..16). Default `accum_effect = OFF`,
  `accum_passes = 8`.
- **`src/app/glr_config.c`**: `accum_effect` is a plain backing field
  (`config_value_ptr` + `glr_config_get` switch, no special-casing). `accum_passes`
  is a cycle — replace `accum_aa_get/set_cycle` (34-55) with
  `accum_passes_get/set_cycle` over `k_accum_pass_steps[6] = {1,2,4,8,12,16}`,
  and update the three dispatch sites (NULL ptr / get / set) accordingly.
- **`src/app/glr_actions.c`**: replace `accum_aa_names` with
  `accum_effect_names[] = {"Off","AA","Blur"}` and
  `accum_passes_names[] = {"1","2","4","8","12","16"}`; replace the single
  `g_cfg_items[]` row with two: effect (`GLR_ACCUM_EFFECT`, special/F2, 3 states)
  and passes (no key, 6 states).
- **`keymap.h`** (line 106): rename `GLR_ACCUM_AA` → `GLR_ACCUM_EFFECT` (keeps
  `GLUT_KEY_F2, 0`); update the nearby comment.
- **`src/app/glr_ctrl_router.c`** (192-218): repoint the Ctrl+= / Ctrl+− accum
  fine-adjust from `GLR_CONFIG_ACCUM_AA` to `GLR_CONFIG_ACCUM_PASSES`; gate on
  `use_accum && accum_effect != OFF`; status string "Accum passes: %s".

### 7. Status-bar text — `src/ui/app/repl_code_panel.c` (1816-1828)

Reads `snap->render.accum_aa_enabled && accum_samples` → `"AA %dx"`. Update to
the new fields: `off` when `accum_effect==OFF`, else `"AA %dx"` /
`"Blur %dx"` from `accum_effect` + `accum_passes`. (This text appears in the
golden fixtures — regen, below.)

### 8. Docs

`CLAUDE.md` F2 key-table row (1174), the "accum-AA Ctrl+=/−" mention (428), and
the jitter-sample comment (698); `src/repl/help_text.c:160-161` ("Accumulation
Buffer AA … On by default") — rewrite for Off/AA/Blur + passes and the corrected
default.

## Reused existing code

- `g_jitter_table` + `scene_apply_projection` frustum-shift (render.c) — AA path
  is unchanged.
- `SceneRenderConfig` callback-injection idiom (`execute_fn`, `post_fill_fn`).
- `shortest_angle_delta` + `tick_target_ease` lerp math (glr_camera.c).
- `repl_copy_predef_values` / `repl_restore_predef_values` frame bracket
  (glr_ctrl.c) — gives free per-frame `t` restore.
- `cfg_slug_from_label` auto-derives the new `accum_effect`/`accum_passes` @cfg
  slugs — no export/import bridge edits needed.

## Tests (full scope — old fields are referenced widely)

Replace `accum_aa` / `accum_samples` / `accum_aa_enabled` / `GLR_CONFIG_ACCUM_AA`
across the compiled tests:
- `tests/test_glr_actions.c`: `CFG_ITEM_COUNT` loops auto-adapt, but bump
  hard-coded item/section counts (RENDERING section gains one row; the
  section-structure arithmetic near 425/478) and the F2 row table at 1204
  (`GLR_CONFIG_ACCUM_AA, "Accum AA"` → `GLR_CONFIG_ACCUM_EFFECT, "Accum effect"`).
- `tests/test_glr_ctrl.c:233-234`: `accum_aa_enabled=0; accum_samples=1;` →
  `accum_effect = SCENE_ACCUM_EFFECT_OFF; accum_passes = 1;`.
- `tests/test_repl_core_examples.c`: the `g_accum_aa_enabled` macro (22), its
  uses (124, 1428), and the `// @cfg accum_aa = 0` strings (1409, 1451) →
  the new effect/passes slugs/fields.
- `tests/test_repl_editor.c`: the F2-cycles-Accum-AA block (565-571) → effect;
  the Ctrl+=/− fine-adjust block (3231-3278, `glr_ctrl_router_handle_accum_samples_key`)
  → drive `GLR_CONFIG_ACCUM_PASSES` and assert `accum_passes` over the
  `{1,2,4,8,12,16}` ladder (note the ladder now includes 12).
- `tests/test_scene_render.c:57`: `cfg.accum_aa_enabled = 1;` →
  `cfg.accum_effect = SCENE_ACCUM_EFFECT_AA; cfg.accum_passes = …;`.
- Golden fixtures `tests/testdata/repl_examples_ui/*.golden.txt`: the status-bar "AA …"
  text changes — regen with a **debug build** (per the golden-regen rule) after
  the code lands.

New unit tests:
- `glr_camera_pose_lerp` — f=0→prev, f=1→cur, midpoint, ry wrap (350↔10 → ~0/360,
  not 180); `glr_camera_pose_changed` epsilon (identical→0, sub-eps→0, supra-eps→1).
- Config-state round-trips: effect (0/1/2) and passes (cycle index →
  `{1,2,4,8,12,16}`).
- **Per-pass isolation (P2):** if the scene-render test harness runs under a GL
  stub, install a counting `setup_subframe_fn` + a stub `execute_fn` that bumps
  a scratch var, and assert (a) the callback fires `passes` times with
  `pass_idx` 0..N-1, and (b) each pass starts from the same baseline (the bumped
  var does not compound). Otherwise cover the baseline reset by unit-testing the
  callback's restore logic directly.

## Verification

1. `make test` (debug ASan+UBSan) — config-state + camera-lerp tests pass.
2. `make check-c99` and `make check-state-ownership` (keymap-no-dup, include
   style, ownership guards) stay green.
3. `make gl-repl` then `./gl-repl --example torus`: F2 cycles Off→AA→Blur;
   Ctrl+= / Ctrl+− steps passes; drag the camera with effect=Blur and confirm
   camera-direction smear; with the camera still and `t` playing (Ctrl+T)
   confirm temporal smear on animated geometry; pause `t` with a still camera
   and confirm it falls back to clean AA. Start replay (Ctrl+R) with effect=Blur
   and confirm geometry renders correctly (AA, not blurred — only the replay
   slice draws, no whole-program leak).
4. Headless sanity (optional): `make gl-repl FREEGLUT_OSMESA=1` +
   `scripts/record-gif.sh --example 2 --duration 2` with blur on to eyeball the
   accumulated frames.
5. Cross-check on gracemont: `make check-c99 && make test-stubs`.
