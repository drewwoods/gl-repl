# Depth Buffer Visualization Mode

## Context

gl-repl renders fixed-function GL geometry but gives no insight into the depth
buffer - useful for teaching depth testing, debugging z-fighting, and
understanding depth precision. This adds a **"Depth view"** config cycle that
visualizes the depth buffer as a grayscale image, with linearized depth and a
scene-normalized mode (user geometry only, grid/backdrop excluded), plus a
split-screen comparison mode. It must work during replay.

**User decisions (confirmed):** single 4-state cycle `Off / Linear / Scene /
Split`; Split shows the normal render with the right half overlaid by the
scene-normalized depth image (pixel-aligned). Shortcut: **plain Ctrl+N** (the
one free Ctrl slot, noted unbound at `keymap.h:114`; pairs with Ctrl+Shift+N =
Normal vectors). Ctrl-ASCII cfg shortcuts dispatch before replay forwarding
(`glr_ctrl_router.c` keyboard_dispatch), so it works mid-replay - F-keys would
cancel replay and are all taken anyway.

## Design summary

- **One readback, placed for free grid exclusion.** Read the scene-rect depth
  buffer (`glReadPixels(..., GL_DEPTH_COMPONENT, GL_FLOAT, ...)` - precedent:
  `edit_overlays.c:1474`) at the **end of `render3d_pass_fill`**, i.e. after
  user geometry + replay-fade `post_fill_fn`, before `render3d_pass_helpers`
  draws backdrop/grid (which write depth). The same buffer serves as both the
  image source and the scene min/max scan - no feedback probe needed.
- **Linearize** via the cached `Render3dProjectionDesc`
  (`render3d_get_active_projection`, computed once per frame in `render.c`):
  perspective `L = n*f / (f - z*(f-n))` with `n/f = desc->near_z/far_z`
  (0.1/200 defaults); ortho `L = z` (already linear).
  - **Linear mode:** `d01 = (L-n)/(f-n)` (persp) or `z` (ortho).
  - **Scene mode:** `d01 = (L-Lmin)/(Lmax-Lmin)`, min/max scanned over pixels
    with `z < 1.0 - 1e-6` (clear depth is exactly 1.0). EMA-smooth the range
    (`s += 0.25*(raw-s)`) to avoid flicker under animation; snap when no prior
    valid range or the range jumps >2x. Empty scene → fall back to Linear map.
  - **Degenerate/overshoot handling (required, not optional):** a
    constant-depth scene makes `Lmax - Lmin == 0`, and EMA lag can put current
    samples outside the smoothed range. If the (smoothed) span is below an
    epsilon (`span < 1e-6f`), map every in-range pixel to mid-gray
    (`d01 = 0.5`) instead of dividing; and always **clamp `d01` to [0,1]**
    after the range division, before the byte conversion - never rely on the
    range containing the sample.
  - **Mapping:** near = bright; `lum = 1 - d01` (Linear), `1 - 0.9*d01`
    (Scene, so the farthest user pixel stays distinguishable from background).
    Background (`z >= 1-eps`) = black.
- **Draw as a `GL_LUMINANCE` textured quad, never `glDrawPixels`** (absent on
  web/gl4es and from GL stubs). POT texture + `glTexSubImage2D` sub-rect
  upload, `GL_NEAREST` filter (1:1 pixel overlay), `GL_UNPACK_ALIGNMENT`
  saved/set-1/restored around the upload. Full modes cover the scene rect;
  **Split** uses one integer split coordinate for both geometry and texture
  mapping so odd widths stay pixel-aligned: `int split = sw / 2;` → vertices
  `[split, sw]`, texcoords `[(float)split / g_tex_w, umax]` (never
  `0.5*umax`, which for `sw = 801` puts the texel origin at 400.5 while the
  vertex sits on boundary 400). Zero extra render passes; optional 1px
  divider line.
- **Accum interaction:** capture on the **last pass only** (depth after
  `glAccum(GL_RETURN)` is the last pass's anyway; this keeps the left half of
  Split fully antialiased). Thread a `capture_depth` flag through
  `render_3d_scene_pass`.
- **Draw order:** depth quad renders **before** `render3d_postprocess_filter_render`
  (between the `post_resolve_overlays_fn` block and the post-filter block in
  `render3d_draw_scene`), so Post FX (grain/scanlines) applies uniformly across
  both halves of a split - no seam.
- **Web gating (accum-bits pattern):** WebGL can't read depth from the default
  framebuffer. Probe once in `glr_ctrl_init_gl` (clear GL error, 1×1 depth
  read, check `glGetError()`), plus hard-off under `#if defined(__EMSCRIPTEN__)`;
  one stderr line when disabled. Controller forces `config->depth_viz = 0`
  when unsupported; the cfg row stays settable (point-parameter philosophy).
- **@cfg / reset policy - winding_view parity:** the slug `depth_view`
  auto-derives from the label and round-trips through workspace @cfg headers,
  but the key is **not** added to `cfg_key_in_scene_subset()` and **not**
  reset per example load - like Winding, it's a debugging view that should
  survive F12 example cycling. (Optional follow-up if examples should set it
  in headers: add the switch case + example reset seed.)
- **Replay:** replay uses the same `render3d_draw_scene` call (only the flat
  exec limit differs), so the visualization works mid-replay automatically;
  cfg-row cycling does not stop replay (pinned by `test_cfg_cycle_stops_replay`).
- **2D view / ortho:** handled by the ortho branch of the linearization; no
  special-casing (transient mid-transition frames use the snapped projection -
  acceptable).

## Implementation steps

### 1. New module `src/render3d/depth_viz.{c,h}`
Picked up automatically by the Makefile wildcard (`RENDER3D_SRCS`); must stay
pure GL (no repl/app includes - `check-pure-render3d-no-repl-state` guard,
and it links into `render3d_demo`).

```c
typedef enum Render3dDepthVizMode {
    RENDER3D_DEPTH_VIZ_OFF = 0, RENDER3D_DEPTH_VIZ_LINEAR,
    RENDER3D_DEPTH_VIZ_SCENE, RENDER3D_DEPTH_VIZ_SPLIT,
    RENDER3D_DEPTH_VIZ_COUNT
} Render3dDepthVizMode;
void render3d_depth_viz_reset(void);                       /* free bufs, delete tex, clear EMA */
void render3d_depth_viz_capture(int sx,int sy,int sw,int sh); /* fill-end, last pass; full rect even in Split */
void render3d_depth_viz_render(Render3dDepthVizMode mode,
                               const Render3dProjectionDesc *proj,
                               int sx,int sy,int sw,int sh);

/* Pure (no GL calls) depth->luminance conversion, exposed so synthetic
 * depth-map tests can drive it directly. Linearizes, normalizes per
 * `mode` (updating the caller-owned EMA range state), clamps, and
 * writes bytes. render() is a thin GL wrapper around this. */
typedef struct Render3dDepthVizRange { float lo, hi; int valid; } Render3dDepthVizRange;
void render3d_depth_viz_map(const float *depth, int count,
                            Render3dDepthVizMode mode,
                            const Render3dProjectionDesc *proj,
                            Render3dDepthVizRange *range,   /* EMA state, in/out */
                            unsigned char *lum_out);
```

Statics: persistent `float *g_depth` + `unsigned char *g_lum` (grown on
resize; malloc failure → capture-invalid no-op), POT texture id + dims,
`GL_MAX_TEXTURE_SIZE` guard (mirror `postprocess_filter.c:181`), EMA range
(a `Render3dDepthVizRange`). File banner documents the math + conventions.
Keeping the conversion GL-free in its own function is what makes the
synthetic-buffer tests in step 6 possible without a GL context.

### 2. Export the 2D bracket from postprocess_filter
Rename static `postprocess_filter_begin_2d/_end_2d`
(`src/render3d/postprocess_filter.c:106/:141`) to exported
`render3d_post_2d_begin/_end` in `postprocess_filter.h`; update the ~4
internal callers. (They already handle matrix-mode snapshot + `glPushAttrib`
+ ortho setup - exactly what the depth quad needs.)

### 3. render.c / render_types.h integration
- `render_types.h`: add `int depth_viz;` to `Render3dRenderConfig` near
  `winding_view` (memset-zero default = Off keeps `render3d_demo` safe).
- `render.c`:
  - `#include "depth_viz.h"`; call `render3d_depth_viz_reset()` in
    `render3d_init_gl` (stale texture names on context re-init).
  - `validate_render_config`: reject out-of-range `depth_viz`.
  - `render_3d_scene_pass(...)`: add trailing `int capture_depth`; between
    `render3d_pass_fill(config)` and `render3d_pass_helpers(...)` call
    `render3d_depth_viz_capture(config->render3d_x, ..., render3d_h)`.
    Call sites: blur + jitter branches pass `dv_on && pass_idx == accum_passes-1`;
    single-pass branch passes `dv_on`.
  - In `render3d_draw_scene`, after the `post_resolve_overlays_fn` block and
    before the post-filter block:
    `if (config->depth_viz) render3d_depth_viz_render(mode, &state->active_projection, sx, sy, sw, sh);`

### 4. Config / keymap / controller plumbing (mirror winding_view)
- `src/app/glr_config.h`: add `GLR_CONFIG_DEPTH_VIZ` before `GLR_CONFIG_COUNT`.
- `src/app/glr_state.h`: `int depth_viz;` in `GlrPresentationState` next to
  `winding_view` (:64).
- `src/app/glr_defaults.h`: `#define CFG_DEFAULT_DEPTH_VIZ 0`;
  `src/app/glr_state.c`: add to `GLR_STATE_DEFAULTS_INITIALIZER`. Do **not**
  add to `glr_state_presentation_reset_example_defaults`.
- `src/app/glr_config.c` (compiler-enforced switches): `config_value_ptr` →
  `&glr_state_presentation_mut()->depth_viz`; `glr_config_get` arm; no special
  `glr_config_set` arm (generic path suffices and fires the tutorial notify).
- `src/app/glr_actions.c`:
  `static const char *depth_viz_names[] = {"Off","Linear","Scene","Split"};`
  + row under **### GEOMETRY** after "Winding" (keeps `test_config_sections`'
  7-section expectation intact):
  `{ .label = "Depth view", .key = GLR_CONFIG_DEPTH_VIZ, .state_count = ARRAY_LEN(depth_viz_names), .state_names = depth_viz_names, .key_code = KM_KEY(GLR_DEPTH_VIZ), .modifiers = KM_MODS(GLR_DEPTH_VIZ) }`
- `keymap.h`: `#define GLR_DEPTH_VIZ  KEY_CTRL_N, 0  /* pairs w/ Normal vectors */`;
  update the stale "plain Ctrl+N is unbound" comment on `GLR_NORMAL_VECTORS`.
- **Claiming Ctrl+N breaks existing assertions/docs - update in the same
  change** (guaranteed `make test-stubs` failure otherwise):
  - `tests/test_glr_actions.c:1629` - flip
    `ASSERT_INT("plain Ctrl+N not claimed by cfg", ..., 0)` to assert plain
    Ctrl+N **is** claimed and cycles `depth_viz` (leave the Ctrl+E / Ctrl+L
    fall-through assertions alone); adjust the comment above it.
  - `tests/test_glr_ctrl.c:4127-4132` - the "Ctrl+N is a complete no-op"
    comment is now stale: it still must not touch
    `post_filter_mode`/`compositor_filter_mode` (assertions stand), but
    reword the comment and add an assertion that the keystroke cycled
    `glr_state_presentation().depth_viz` instead.
  - `docs/USER_GUIDE.md` "Scene & rendering" shortcut table (~:1425): add
    `| Ctrl+N | Depth view |`. Also add the row to CLAUDE.md's Key Controls
    table. The F1 help overlay needs **no** edit - `help_text.c` walks
    `g_cfg_items[]`, so the row appears automatically.
- `src/app/glr_ctrl.c`:
  - `static int g_depth_readback_supported = 1;` + init-GL probe next to the
    accum-bits probe (clear error → 1×1 depth `glReadPixels` → check
    `glGetError()`; `#if defined(__EMSCRIPTEN__)` force 0); stderr line when off.
  - `glr_ctrl_build_scene_config` next to the `winding_view` copy:
    `config->depth_viz = g_depth_readback_supported ? presentation.depth_viz : 0;`

### 5. GL stubs
`tests/gl-stubs/include/GL/gl.h` has `glReadPixels` / `glTexImage2D` /
`GL_DEPTH_COMPONENT` / `GL_LUMINANCE`; **add a no-op `glTexSubImage2D`**
(follow the `glTexImage2D` stub pattern + `gl_stub_counts.h` entry). Verify
`GL_UNPACK_ALIGNMENT` token exists; add if missing. The stub `glReadPixels`
fills 1.0 → depth viz sees "empty scene" and takes the Linear fallback -
good structural coverage. Then verify all three builds per CLAUDE.md:
`make test-stubs`, `make gl-repl USE_GL_STUBS=1`, `make gl-repl`.

### 6. Tests
- **Synthetic depth-map tests** (new `tests/test_depth_viz.c`, linked against
  the stub-built `depth_viz.o` with an explicit per-test OBJS list - the
  `test_render3d_transition` Makefile pattern): drive
  `render3d_depth_viz_map()` with hand-built float buffers and a fixed
  `Render3dProjectionDesc`:
  - perspective linearization: known z → expected luminance (Linear mode);
  - Scene mode: two-depth buffer → min maps to 255-ish, max to `0.1*255`;
  - background pixels (`z = 1.0`) → 0 in every mode;
  - **zero-span**: constant-depth buffer → mid-gray, no NaN/div-by-zero;
  - **EMA overshoot**: pre-seed the range state so samples fall outside it →
    output still clamped to [0,255], no wraparound;
  - EMA snap: range jump >2x resets rather than lags;
  - ortho branch: `L = z` path.
- `tests/test_render3d_render.c`: each `depth_viz` mode renders OK in
  single-pass and accum branches; out-of-range value → validate fails (−1).
  (The stub `glReadPixels` fills 1.0, so these only exercise the empty-scene
  path - the synthetic tests above carry the math coverage.)
- `tests/test_glr_actions.c`: "Depth view" row exists, 4 states, expected
  names; slug is `depth_view` (`glr_config_item_slug`); extend
  `test_cfg_cycle_stops_replay` to cycle the depth-view row and assert replay
  stays active (pins the "works during replay" requirement); flip the plain
  Ctrl+N assertion (step 4).
- `tests/test_glr_ctrl.c`: update the Ctrl+N no-op block (step 4).
- No `test_repl_core_examples.c` changes (not in the scene subset).

### 7. Profiling section
Add `PROF_RENDER3D_DEPTH_VIZ` to `prof_sections.h` and its
`{ label, depth, is_total }` row in `src/app/glr_prof.c` (depth 1 under the
render3d total, like the accum/overlays subsections; **not** in
`k_gpu_sections[]` - the readback is a CPU-side stall, the CPU column is
what we want to see). Bracket the capture and the render/upload with
`prof_begin`/`prof_end` so the cost is visible in the Ctrl+W Sections panel
and the demos' `prof_section_info` tables stay untouched (they're
per-binary). The standalone demos don't compile `glr_prof.c`, and cpuprof
falls back gracefully when a section id is absent from a catalog.

## Verification

```bash
make check-c99 && make check-keymap-no-dup && make check-state-ownership
make test-stubs
make gl-repl USE_GL_STUBS=1 && make gl-repl

# Headless visual verification (OSMesa + SIGUSR1 PPM capture):
make gl-repl FREEGLUT_OSMESA=1
GLR_TYPE_KEYS=$'\x0e' ./build/release-osmesa/gl-repl --example torus --no-audio &   # Ctrl+N ×1 = Linear
sleep 2; kill -USR1 $!; sleep 1; magick freeglut-0000.ppm depth-linear.png
# ×2 = Scene (full-contrast, no grid), ×3 = Split (left normal / right depth,
# pixel-aligned at the divider - verify with an odd --window width too).
# Replay: GLR_TYPE_KEYS=$'\x12\x0e\x0e' (Ctrl+R then Ctrl+N×2) - replay HUD
# visible + depth view active.
# Accum: GLR_ACCUM_PASSES=16 + Split - left half stays antialiased.

# Ortho/2D headless: GLR_TYPE_KEYS cannot carry GLUT_ACTIVE_SHIFT, so
# Ctrl+Shift+E/V are NOT reachable this way. Instead:
#   2D view:  GLR_VIEW_TOGGLE_AT=0.5 GLR_TYPE_KEYS=$'\x0e\x0e' ... (toggles
#             view mode at t=0.5s; capture after the transition settles)
#   Ortho:    stage a snippet scene with a leading `// @cfg projection = PROJ_ORTHO`
#             header (docs-assets staging pattern) and load it as the file arg.
# The Ctrl+Shift+E/V keystroke paths themselves are native interactive checks.

make web                       # if emsdk on PATH: compiles; depth view degrades to Off
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && git pull --ff-only origin main && make check-c99 && make test-stubs'
```

**Measured (2026-07-17, implementation landed):** native Cocoa, 1200×800
window (retina 2× framebuffer), torus example, Depth view = Scene:
Render 3D CPU 5.27 ms vs 1.23 ms baseline → **~4.0 ms attributable**,
FPS holds ~57–60, frame total 7.5 ms. Within budget; no downsampling
lever needed. OSMesa headless: functional (all four modes captured).

**Performance acceptance (on-mode cost is real: at 2400×1600 each frame
synchronously reads ~15 MB of depth, scans ~3.8M pixels, and uploads ~3.8 MB
- a pipeline stall plus ~1 GB/s of transfers at 60 FPS):**
- Native (Cocoa, default window size): with Depth view = Scene on the torus
  example, the Ctrl+W Sections panel's `PROF_RENDER3D_DEPTH_VIZ` row (step 7)
  must stay under **~4 ms** CPU, and the FPS plot must hold 60 FPS. Record
  the measured number in the PR description.
- Native at a large window (~2400×1600): degradation is acceptable but must
  be *attributed* - the depth-viz row should own the cost, not smear into
  other sections.
- OSMesa: functional only (software rasterizer; no frame-rate criterion).
- If the native budget is blown, the fallback lever is documented in
  Risks (capture-rect downsampling), not silently shipped.

## Risks / notes
- Buffer cost: ~15 MB float + ~4 MB bytes at 2400×1600, persistent while the
  feature has ever run; freed by `_reset`. Zero per-frame cost when Off.
- Per-frame cost when On: synchronous `glReadPixels` stalls the pipeline and
  the CPU convert scans every pixel - see the performance acceptance criteria
  in Verification. If the budget is blown at default window size, the fallback
  lever is capturing at half resolution (read every-other row/col or
  `glPixelZoom`-free box sampling on the CPU side) and letting the LINEAR
  texture filter stretch it - a quality/cost knob, deferred unless needed.
- Scene-normalized range breathes with animation despite EMA - acceptable;
  Linear mode is the stable-reference alternative. Zero-span and EMA-overshoot
  are handled by the epsilon fallback + clamp (Design summary) and pinned by
  the synthetic tests.
- Mid 2D↔3D transition frames linearize with the snapped projection desc -
  momentary, invisible in practice.

## Critical files
- `src/render3d/depth_viz.{c,h}` (new), `tests/test_depth_viz.c` (new)
- `src/render3d/render.c`, `src/render3d/render_types.h`,
  `src/render3d/postprocess_filter.{c,h}`
- `src/app/glr_ctrl.c`, `src/app/glr_config.{c,h}`, `src/app/glr_actions.c`,
  `src/app/glr_state.{c,h}`, `src/app/glr_defaults.h`, `keymap.h`
- `prof_sections.h`, `src/app/glr_prof.c` (profiling section)
- `tests/gl-stubs/include/GL/gl.h`, `tests/test_render3d_render.c`,
  `tests/test_glr_actions.c`, `tests/test_glr_ctrl.c` (Ctrl+N no-op block),
  `Makefile` (test_depth_viz target)
- `docs/USER_GUIDE.md` + `CLAUDE.md` shortcut tables
