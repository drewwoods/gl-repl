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

## Architecture decision

Keep the accumulation loop **in the scene** (`scene_render_3d_scene`), where the
accum buffer, per-pass clears, `glAccum`, and the once-per-frame
`post_filter` already live. Inject per-pass camera/time variation via a new
**callback**, mirroring the existing `execute_fn` / `post_fill_fn` /
`post_overlays_fn` injection idiom in `SceneRenderConfig`. The scene stays
camera/REPL-agnostic; the controller (which owns camera + `t`) supplies the
per-pass policy. This avoids lifting the loop to the controller and duplicating
the clear/accum/return/post-filter sequencing.

For `effect==Blur` the scene calls `setup_subframe_fn(ud, pass_idx, pass_count)`
before each pass and renders with jitter=0; the controller's callback loads an
interpolated modelview (camera blur) or sets the predef-`t` sub-step (time
blur). When the callback is `NULL` (the paused + unchanged-camera case), the
scene automatically uses the AA jitter path — that *is* the fallback.

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
161-163; keep `use_accum`):
```c
int use_accum;          /* --noaccum master gate */
int accum_effect;       /* SceneAccumEffect */
int accum_passes;       /* resolved pass count: 1,2,4,8,12,16 */
void (*setup_subframe_fn)(void *user_data, int pass_idx, int pass_count);
void  *setup_subframe_user_data;
```

### 2. Scene loop — `src/scene/render.c` (`scene_render_3d_scene`, 711-725)

Generalize the loop: `do_accum = use_accum && accum_effect != OFF && passes > 1`.
For each pass clear COLOR|DEPTH; if `effect==BLUR && setup_subframe_fn` call it
then render with jitter 0,0; else render with `g_jitter_table[p % MAX_ACCUM_SAMPLES]`.
`glAccum(GL_ACCUM, 1/passes)` per pass, `glAccum(GL_RETURN, 1)` after. The
existing jitter table already supports 12 (uses the first 12 of 16 entries).
Extend `validate_render_config` to clamp `accum_effect ∈ [0,2]`, `accum_passes ≥ 1`.

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

### 4. Transient time setter — `src/repl/state.{h,c}`

`repl_state_time_set` is unusable here (it overwrites `g_anim_time` and sets
`g_flat_dirty`). Add:
```c
/* Sets only predef 't' for one sub-pass re-eval; does NOT touch g_anim_time
 * or g_flat_dirty. The frame already snapshots/restores predef values. */
void repl_state_time_set_transient(float value);  /* writes g_predef_vars_mut[g_t_var_idx].value */
```
Safe because the executor re-evaluates `has_vars` flat commands from current
predef values at execute time — no reflatten needed. The existing per-frame
`repl_copy_predef_values` / `repl_restore_predef_values` bracket
(`glr_ctrl.c:1377` / `1524`) already surrounds the whole accum loop, so the true
`t` is restored automatically.

### 5. Controller wiring — `src/app/glr_ctrl.c`

- File statics: `g_prev_frame_pose` (+`_valid`), `g_cur_frame_pose`, and a
  `GlrSubframeCtx { mode, prev, cur, t_end, dt, t_var_idx }` stored in
  `g_subframe_ctx`.
- Replace the inline camera-load block (1424-1428): capture
  `g_cur_frame_pose = glr_camera_pose_from_state(&cam)` and load it as the base
  modelview.
- Resolve blur mode before the render call (in/near `glr_ctrl_build_scene_config`):
  `BLUR_CAMERA` if `prev_valid && glr_camera_pose_changed(prev,cur)`, else
  `BLUR_TIME` if `repl_state_variables().time_playing`, else `BLUR_NONE`. Only
  when `accum_effect==BLUR && use_accum && passes>1`. For `BLUR_NONE` leave
  `setup_subframe_fn = NULL` (scene → AA jitter fallback).
- Add `glr_ctrl_setup_subframe(ud, pass_idx, pass_count)`: `f = pass_idx/(pass_count-1)`
  (endpoints inclusive). Camera → `glr_camera_load_modelview(pose_lerp(prev,cur,f))`.
  Time → `repl_state_time_set_transient(t_end - dt*(1-f))` (trailing window).
- Populate `config->accum_effect/accum_passes/setup_subframe_fn/_user_data` in
  the build site (replaces lines 782-784).
- At the **end** of `glr_ctrl_display_frame`, capture
  `g_prev_frame_pose = g_cur_frame_pose; g_prev_frame_pose_valid = 1;`.

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

### 7. Docs

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

## Tests

- `tests/test_glr_actions.c`: `CFG_ITEM_COUNT` loops auto-adapt, but bump any
  hard-coded item/section counts (RENDERING section gains one row; the
  section-structure arithmetic near 425/478). Replace `accum_aa`/`ACCUM_AA`
  references with the two new keys (effect 3 states, passes 6 states).
- New unit tests: `glr_camera_pose_lerp` (f=0→prev, f=1→cur, midpoint, and the
  ry wrap case 350↔10 → ~0/360 not 180); `glr_camera_pose_changed` epsilon
  (identical→0, sub-eps→0, supra-eps→1); config-state round-trips for effect
  (0/1/2) and passes (cycle index → {1,2,4,8,12,16}).
- The GL blur loop itself isn't unit-testable without a context; if a GL-stub
  scene-render harness already exists, optionally install a counting
  `setup_subframe_fn` + stub `execute_fn` and assert it fires `passes` times.

## Verification

1. `make test` (debug ASan+UBSan) — config-state + camera-lerp tests pass.
2. `make check-c99` and `make check-state-ownership` (keymap-no-dup, include
   style, ownership guards) stay green.
3. `make gl-repl` then `./gl-repl --example torus`: F2 cycles Off→AA→Blur;
   Ctrl+= / Ctrl+− steps passes; drag the camera with effect=Blur and confirm
   camera-direction smear; with the camera still and `t` playing (Ctrl+T)
   confirm temporal smear on animated geometry; pause `t` with a still camera
   and confirm it falls back to clean AA.
4. Headless sanity (optional): `make gl-repl FREEGLUT_OSMESA=1` +
   `scripts/record-gif.sh --example 2 --duration 2` with blur on to eyeball the
   accumulated frames.
5. Cross-check on gracemont: `make check-c99 && make test-stubs`.
