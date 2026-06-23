# Experimental Post-Processing Filter

## Context

The REPL renders user geometry into the scene viewport with optional
accumulation-buffer AA. There is no post-processing stage today, and no texture
usage anywhere in the codebase.

We want an opt-in experimental post-processing filter. The first iteration
should be deliberately simple: a chromatic-aberration effect. After the 3D
scene resolves, copy the rendered scene pixels into a texture, redraw the base
image, then redraw red/blue channel-only passes with small horizontal offsets.
This stays pure fixed-function GL: no shaders and no FBOs.

Later iterations can expand the same post-processing stage into a fuller CRT
treatment: barrel mesh distortion, shadow-mask / aperture-grille patterns,
scanlines, vignette, and other display artifacts. Do not take on that full CRT
surface in the first pass.

The filter is a presentation effect on the scene viewport only. The code panel,
menus, replay HUD, and other 2D UI must stay crisp. Because this is
experimental, it is not exposed in the Config menu and is not persisted through
`@cfg`. It is controlled only by a hidden keyboard shortcut.

Proposed shortcut: `Ctrl+N`, cycling `Off -> Chromatic Aberration -> Off`.
F1 and F2-F12 are already occupied, and `Ctrl+N` is currently unused in the
controller/editor routes.

## Approach

Add a self-contained scene module that owns one GL texture and one render step,
invoked at the very end of `scene_render_3d_scene()` after the accumulation
resolve. The selected mode flows through `SceneRenderConfig`, so the effect
also works for the non-REPL `scene_demo` binary and keeps `src/scene/`
REPL-independent.

Do not add a `GlrConfigKey`, Config-menu item, config bridge slug, or scene
subset entry. This is a runtime-only experimental toggle.

## New Module: `src/scene/postprocess_filter.c` + `.h`

File-scope state:

- `static GLuint g_filter_tex`
- `static int g_tex_w, g_tex_h`

Public API, using the `scene_postprocess_filter_` prefix:

```c
typedef enum ScenePostFilterMode {
    SCENE_POST_FILTER_OFF = 0,
    SCENE_POST_FILTER_CHROMATIC_ABERRATION,
    SCENE_POST_FILTER_COUNT
} ScenePostFilterMode;

const char *scene_postprocess_filter_mode_name(int mode);
void scene_postprocess_filter_reset(void);
void scene_postprocess_filter_render(int mode, int sx, int sy, int sw, int sh);
```

`scene_postprocess_filter_reset()` invalidates the cached texture handle and
allocated dimensions. If called while a live context owns `g_filter_tex`, delete
it with `glDeleteTextures(1, &g_filter_tex)` before zeroing the handle.
`scene_render_init_gl()` should call this so a fresh GL context never reuses a
stale texture name.

`scene_postprocess_filter_render()` returns immediately for
`SCENE_POST_FILTER_OFF` or invalid rectangles. For
`SCENE_POST_FILTER_CHROMATIC_ABERRATION`:

1. Lazy-allocate or grow the texture if `g_filter_tex == 0` or the scene grew
   beyond `g_tex_w/g_tex_h`.

   Use power-of-two dimensions (`next_pow2(sw) x next_pow2(sh)`). This is
   **required**, not just conservative: NPOT textures are an OpenGL 2.0
   feature (`ARB_texture_non_power_of_two`); OpenGL 1.1 — the
   fixed-function baseline this codebase targets (no shaders, no FBOs) —
   only supports power-of-two `GL_TEXTURE_2D` dimensions. Do **not** add a
   runtime NPOT check: the POT-texture-plus-`umax/vmax`-subregion path
   below renders correctly on both GL 1.1 and GL 2.0+, so branching on
   NPOT support buys nothing but complexity. Allocate with:

   ```c
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex_w, tex_h,
                0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
   ```

   Set `GL_LINEAR` min/mag filtering and `GL_CLAMP_TO_EDGE` wrapping. Track the
   allocated dimensions so the normal frame path uses `glCopyTexSubImage2D`
   instead of reallocating.

   Defensive guard: query `GL_MAX_TEXTURE_SIZE` once. If the required POT size
   exceeds the maximum, skip the filter for that frame instead of attempting an
   invalid allocation.

2. Capture the resolved scene image:

   ```c
   glBindTexture(GL_TEXTURE_2D, g_filter_tex);
   glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx, sy, sw, sh);
   ```

   Coordinate origin: `glCopyTexSubImage2D` reads the framebuffer in
   GL window coordinates (**bottom-left origin**). `sx, sy, sw, sh` are
   the exact rect `config->scene_x/y/w/h` — the same rect the scene was
   rendered into via `glViewport(sx, sy, sw, sh)` — so the copy region
   and the redraw viewport are consistent by construction (no Y flip is
   needed; do not introduce one). If a future change makes `scene_y`
   top-left-origin, this copy and the redraw below must both flip
   together — assert/keep them defined in the same convention.

   The visible texture region is:

   ```c
   umax = sw / (float)g_tex_w;
   vmax = sh / (float)g_tex_h;
   ```

3. Redraw the base textured image over the same scene rect.

   Use private helpers inside `postprocess_filter.c`, for example
   `postprocess_filter_begin_2d(sx, sy, sw, sh)` and
   `postprocess_filter_end_2d()`. Do not include `src/ui/gl_2d.h`; `src/scene/`
   must not depend on UI headers.

   The begin helper should:

   - `glPushAttrib(GL_ALL_ATTRIB_BITS)`
   - save the current matrix mode with `glGetIntegerv(GL_MATRIX_MODE, ...)`
   - set `glViewport(sx, sy, sw, sh)`
   - push/load projection and modelview matrices, then `gluOrtho2D(0, sw, 0, sh)`
   - push/load the texture matrix as identity
   - set known 2D draw state: disable depth test, depth writes, lighting, fog,
     culling, scissor, alpha test, and stencil test; set polygon mode to fill
   - enable `GL_TEXTURE_2D`
   - set `GL_TEXTURE_ENV_MODE` to `GL_REPLACE`

   Draw a single screen-aligned textured quad using `0..umax` and `0..vmax`
   texcoords.

4. Draw the chromatic-aberration channel offsets.

   Keep texture enabled and blending disabled. Use `glColorMask` to redraw only
   one channel at a time over the base image.

   The offset is applied to the quad's **vertex X positions**, in the
   `gluOrtho2D(0, sw, 0, sh)` pixel space — i.e. translate the screen
   quad by ±dx pixels. **Texcoords are unchanged** (still `0..umax` /
   `0..vmax`); do not offset in texture space. (Vertex-space shift keeps
   the offset an exact pixel amount regardless of the POT texture size;
   a texcoord-space shift would have to be rescaled by `umax/vmax`.)

   - red-only pass: `glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE)` and
     draw the same textured quad translated `+dx` px in X
     (`dx` ≈ `1.5f`–`2.0f`)
   - blue-only pass: `glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE)` and
     draw the same textured quad translated `-dx` px in X
   - restore full color writes with
     `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE)`

   Leave green unshifted from the base pass. This gives the first iteration a
   visible post-processing effect without introducing the more complex CRT mesh
   and mask logic.

5. Restore exactly.

   Pop texture/modelview/projection matrices, restore the saved matrix mode,
   and `glPopAttrib()`. The subsequent replay HUD and code-panel overlays must
   see GL state as it was before the filter.

## Wire The Render Step: `src/scene/render.c`

- Include `"postprocess_filter.h"`.
- In `scene_render_init_gl()`, after
  `scene_lights_init_global_ambient()`, call:

  ```c
  scene_postprocess_filter_reset();
  ```

- In `scene_render_3d_scene()`, after the accum `if/else` block and before
  `return 0;`, add:

  ```c
  if (config->post_filter_mode > SCENE_POST_FILTER_OFF)
      scene_postprocess_filter_render(config->post_filter_mode,
                                      config->scene_x, config->scene_y,
                                      config->scene_w, config->scene_h);
  ```

This runs once per frame on the fully resolved scene image, covering both the
accum and non-accum branches, and still happens before any 2D overlay.

## Scene Config: `src/scene/render_types.h`

Add:

```c
int post_filter_mode;
```

Place it near `backdrop_mode` in `SceneRenderConfig`.

## Experimental App State And Shortcut

Keep this out of the config descriptor system.

1. `src/app/glr_state.h`

   Add `int post_filter_mode;` to `GlrPresentationState` near
   `backdrop_mode`.

2. `src/app/glr_state.c`

   Add `.post_filter_mode = SCENE_POST_FILTER_OFF,` to the presentation
   defaults initializer.

   Do not add it to `glr_state_presentation_reset_example_defaults()`. The
   filter is a session-level experimental toggle, not scene-bound metadata.

3. `keys.h`

   Add:

   ```c
   #define KEY_CTRL_N 14 /* experimental post-processing filter */
   ```

4. `src/app/glr_ctrl.h` / `src/app/glr_ctrl.c`

   Add a controller router helper, for example:

   ```c
   int glr_ctrl_router_handle_post_filter_key(unsigned char key);
   ```

   Route it in `glr_ctrl_keyboard()` before the editor handler, near the other
   controller-owned shortcuts:

   ```c
   glr_ctrl_router_handle_post_filter_key(key) ||
   ```

   The helper should:

   - return 0 unless `key == KEY_CTRL_N`
   - cycle `glr_state_presentation_mut()->post_filter_mode`
     modulo `SCENE_POST_FILTER_COUNT`
   - set a short status message such as `Post filter: Chromatic aberration` /
     `Post filter: Off`
   - call `editor_request_redraw()`
   - return 1

5. `src/app/glr_ctrl.c`

   In the scene-config builder, beside:

   ```c
   config->backdrop_mode = presentation.backdrop_mode;
   ```

   add:

   ```c
   config->post_filter_mode = presentation.post_filter_mode;
   ```

## Do Not Add Config Plumbing

Because this is experimental and shortcut-only, do not change:

- `src/app/glr_config.h`
- `src/app/glr_config.c`
- `g_cfg_items[]` in `src/app/glr_actions.c`
- `cfg_key_in_scene_subset()`
- workspace/example `@cfg` handling

There should be no Config menu row and no emitted `// @cfg post_filter = ...`
or `// @cfg crt_shader = ...` line.

## GL Stub Headers

Keep `USE_GL_STUBS=1` and `make test-stubs` building.

`tests/gl-stubs/include/GL/gl.h` already has `glBindTexture`,
`glTexCoord2f`, `glTexEnvi`, `glGetIntegerv` (used for the
`GL_MAX_TEXTURE_SIZE` guard — do **not** re-add it), `GL_TEXTURE_2D`,
`GL_LINEAR`, `GL_REPLACE`, `GL_TEXTURE_ENV`, and `GL_TEXTURE_ENV_MODE`.

Add, following the file's existing inline no-op + `gl_stub_tick` style:

- Functions: `glGenTextures`, `glDeleteTextures`, `glTexImage2D`,
  `glCopyTexSubImage2D`, `glTexParameteri`
- Constants: `GL_RGB 0x1907`, `GL_RGBA 0x1908`,
  `GL_UNSIGNED_BYTE 0x1401`, `GL_NEAREST 0x2600`,
  `GL_TEXTURE_MIN_FILTER 0x2801`, `GL_TEXTURE_MAG_FILTER 0x2800`,
  `GL_TEXTURE_WRAP_S 0x2802`, `GL_TEXTURE_WRAP_T 0x2803`,
  `GL_CLAMP_TO_EDGE 0x812F`, `GL_MAX_TEXTURE_SIZE 0x0D33`

`tests/gl-stubs/include/GL/gl_stub_counts.h` needs matching entries in
`GL_STUB_COUNTER_LIST`:

- `X(glGenTextures)`
- `X(glDeleteTextures)`
- `X(glTexImage2D)`
- `X(glCopyTexSubImage2D)`
- `X(glTexParameteri)`

Real builds use system GL headers, which already declare these.

## Makefile

Add `src/scene/postprocess_filter.c` to the scene source list in the same place
as `src/scene/backdrop.c`. This automatically pulls it into `SCENE_SRCS` and
therefore into the standalone `scene_demo` link set.

## Critical Files

| File | Change |
|---|---|
| `src/scene/postprocess_filter.c` / `.h` | new texture capture + chromatic-aberration filter renderer |
| `src/scene/render.c` | reset filter state at GL init; call filter after scene resolve |
| `src/scene/render_types.h` | add `int post_filter_mode;` |
| `src/app/glr_state.h` / `.c` | store session-level experimental mode defaulting to off |
| `keys.h` | add `KEY_CTRL_N` |
| `src/app/glr_ctrl.h` / `.c` | add and route hidden shortcut; pass mode into scene config |
| `tests/gl-stubs/include/GL/gl.h`, `gl_stub_counts.h` | add texture stubs |
| `Makefile` | add `postprocess_filter.c` to scene sources |

## Verification

1. `make sample`
2. `./sample`, load a geometry example, press `Ctrl+N`: the scene should show a
   subtle red/blue chromatic-aberration offset. Press `Ctrl+N` again: the flat
   scene should return.
3. Open the Config menu and confirm there is no post-processing filter row.
4. Toggle MSAA and accum AA while the filter is on. The filter should capture
   the resolved image with no flicker or stale-frame ghosting.
5. Resize the window larger and smaller. The image should remain correctly
   mapped, with texture reallocation only on growth.
6. Save a workspace/output.c while the filter is on. Confirm no
   `// @cfg post_filter = ...` or `// @cfg crt_shader = ...` line is emitted.
   Restarting the app and loading that file should default the filter back to
   off.
7. `make test-stubs` and `make sample USE_GL_STUBS=1`
8. `make scene_demo`
9. `make test`
10. `make check-state-ownership`

## Later CRT Expansion

Once the first chromatic-aberration pass is stable, add additional filter modes
behind the same `ScenePostFilterMode` cycle. A full CRT mode can then reuse the
texture capture path and replace the simple screen quad with:

- a tessellated barrel-distortion mesh
- scanline darkening
- shadow-mask or aperture-grille overlays
- edge vignette / bloom-style darkening

Keep those as later iterations so the first implementation proves the
post-processing hook, texture capture, shortcut state, and GL-state restoration
with the smallest useful effect.
