# CRT Post-Process Shader

## Context

The REPL renders user geometry into the scene viewport with optional
accumulation-buffer AA. There is no post-processing stage today — and no
texture usage anywhere in the codebase. We want an opt-in retro "CRT"
look: after the 3D scene resolves, copy the rendered pixels into a
texture and re-draw them as a full-screen, slightly barrel-curved quad
with scanline darkening — all in **pure fixed-function GL** (no shaders,
no FBOs). It is a presentation effect on the scene viewport only (the
code panel must stay crisp). Modeled as a **multi-state cycle**
(`Off` / `CRT`, extensible to more looks later), reachable from the
**Config menu only** (no keybinding — F1/F2–F11/F12 are all taken).

## Approach

Add a new self-contained scene module that owns one GL texture and a
render step, invoked at the very end of `scene_render_3d_scene()` after
the accum resolve. Config flows through a new `SceneRenderConfig` field,
so the effect also works for the non-REPL `scene_demo` binary and keeps
`src/scene/` REPL-independent.

### New module: `src/scene/postprocess.c` + `src/scene/postprocess.h`

File-scope state: `static GLuint g_crt_tex`, `static int g_tex_w,
g_tex_h` (allocated dimensions).

Public API (prefix `scene_postprocess_`):

- `void scene_postprocess_reset(void)` — invalidate the cached texture
  handle (set to 0; next render reallocates). Called from
  `scene_render_init_gl()` so a fresh GL context never reuses a stale
  texture name.
- `void scene_postprocess_render(int mode, int sx, int sy, int sw, int sh)` —
  the whole effect. `mode==0` returns immediately (defensive; caller
  also guards). For `mode>=1`:
  1. **Lazy (re)allocate** the texture if `g_crt_tex==0` or the scene
     grew past `g_tex_w/g_tex_h`. Use a **power-of-two** size
     (`next_pow2(sw) x next_pow2(sh)`) for legacy fixed-function
     robustness: one-time `glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,texW,
     texH,0,GL_RGB,GL_UNSIGNED_BYTE,NULL)`, `GL_LINEAR` min/mag,
     `GL_CLAMP_TO_EDGE` wrap. Track `g_tex_w/g_tex_h`. (POT + a
     persistent texture lets us use `glCopyTexSubImage2D` every frame
     instead of reallocating with `glCopyTexImage2D`.)
  2. **Capture:** `glBindTexture` then
     `glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0, sx,sy,sw,sh)` — copies
     the resolved scene pixels into the texture's lower-left. Visible
     region in texcoords is `umax=sw/(float)texW`, `vmax=sh/(float)texH`.
  3. **Redraw curved textured quad:** use private helpers inside
     `postprocess.c` (e.g. `postprocess_begin_2d(sx,sy,sw,sh)` /
     `postprocess_end_2d()`) rather than `src/ui/gl_2d.h`. `src/scene/`
     must not include UI headers, and the existing layer guard enforces
     that. The helper should first `glPushAttrib(GL_ALL_ATTRIB_BITS)`,
     then set the viewport to the scene rect, push projection/modelview
     matrices, and install `gluOrtho2D(0,sw,0,sh)`, so texture, blend,
     color, depth, lighting, and viewport state are restored. Then
     `glEnable(GL_TEXTURE_2D)`,
     `glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE)`. Draw a
     tessellated grid (e.g. 24×18 cells) of `GL_QUADS`/`GL_TRIANGLES`:
     screen position is the cell's straight grid point; the **texcoord**
     is barrel-distorted around screen center (sample slightly pinched so
     the image bulges) and scaled by `umax/vmax`. Slight edge vignette by
     multiplying vertex `glColor3f` darker toward the corners (works
     because `GL_REPLACE` → switch to `GL_MODULATE` for the vignette, or
     do vignette as the scanline pass below).
  4. **Scanline pass:** disable texture, `glEnable(GL_BLEND)`,
     `glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA)`, draw horizontal
     dark translucent `GL_LINES`/thin quads every 2–3 px across
     `0..sw,0..sh`. Optionally a dark radial vignette quad-fan here.
  5. `glDisable(GL_TEXTURE_2D)`, `glDisable(GL_BLEND)`,
     `postprocess_end_2d()`.

The private 2D bracket must restore GL exactly as it found it for the
subsequent replay HUD and code-panel overlays. Do not rely on
`src/ui/gl_2d.h`: besides the layer violation, its current attrib mask
does not include texture or blend state.

### Wire the render step — `src/scene/render.c`

- `#include "postprocess.h"`.
- In `scene_render_init_gl()` (line 42, after
  `scene_lights_init_global_ambient()`): call
  `scene_postprocess_reset()`.
- In `scene_render_3d_scene()` (lines 303–328): after the accum
  `if/else` block and **before `return 0;`** (line 327), add:
  ```c
  if (config->crt_mode > 0)
      scene_postprocess_render(config->crt_mode,
                               config->scene_x, config->scene_y,
                               config->scene_w, config->scene_h);
  ```
  This runs once per frame on the fully resolved image (covers both the
  accum and non-accum branches) and before any 2D overlay.

### Config field — `src/scene/render_types.h`

Add `int crt_mode;` to `SceneRenderConfig` near `backdrop_mode`
(line ~117).

### Config plumbing (mirror `backdrop_mode` exactly)

1. `src/app/glr_config.h` — add `GLR_CONFIG_CRT,` before
   `GLR_CONFIG_COUNT` (line ~69).
2. `src/app/glr_defaults.h` — add `#define CFG_DEFAULT_CRT_MODE 0`
   (next to `CFG_DEFAULT_BACKDROP_MODE`, line 41).
3. `src/app/glr_state.h` — add `int crt_mode;` to
   `GlrPresentationState` (next to `backdrop_mode`, line 40).
4. `src/app/glr_state.c` — add `.crt_mode = CFG_DEFAULT_CRT_MODE,` to
   the presentation defaults initializer, and reset `p->crt_mode =
   CFG_DEFAULT_CRT_MODE;` in `glr_state_presentation_reset_example_defaults()`
   because CRT is part of the scene-bound config subset. Without this,
   examples or scenes that omit `@cfg crt_shader` can inherit the prior
   scene's CRT state.
5. `src/app/glr_config.c` — add to `config_value_ptr()` (line ~55):
   `case GLR_CONFIG_CRT: return &glr_state_presentation_mut()->crt_mode;`
6. `src/app/glr_actions.c`:
   - State-name array near `backdrop_mode_names` (line 37):
     `static const char *crt_mode_names[] = { "Off", "CRT" };`
   - `g_cfg_items[]` entry near the Backdrop row (line 115), menu-only
     (`key_code 0`, `is_special 0`):
     `{ "CRT shader", 0, 0, GLR_CONFIG_CRT, 2, crt_mode_names, 0 },`
     (`state_count` = `sizeof(crt_mode_names)/sizeof(*)` count = 2;
     bump when adding looks later.)
   - Add `case GLR_CONFIG_CRT:` to `cfg_key_in_scene_subset()`
     (line 157, alongside `GLR_CONFIG_BACKDROP`) so the CRT choice is
     captured per-scene and round-trips through the `@cfg crt_shader`
     workspace/example header (slug auto-derives from the "CRT shader"
     label — no export.c change needed).
7. `src/app/glr_ctrl.c` — in the scene-config builder (near line 1138,
   beside `config->backdrop_mode = presentation.backdrop_mode;`):
   `config->crt_mode = presentation.crt_mode;`

### GL stub headers (keep `USE_GL_STUBS=1` / `make test-stubs` building)

`tests/gl-stubs/include/GL/gl.h` already has `glBindTexture`,
`glTexCoord2f`, `glTexEnvi`, `GL_TEXTURE_2D`, `GL_LINEAR`, `GL_REPLACE`,
`GL_TEXTURE_ENV`, `GL_TEXTURE_ENV_MODE`. Add (keep file's existing style
— inline no-op + `gl_stub_tick`, alpha-ish ordering):

- Functions: `glGenTextures`, `glDeleteTextures`, `glTexImage2D`,
  `glCopyTexSubImage2D`, `glTexParameteri`.
- Constants: `GL_RGB 0x1907`, `GL_RGBA 0x1908`,
  `GL_UNSIGNED_BYTE 0x1401`, `GL_NEAREST 0x2600`,
  `GL_TEXTURE_MIN_FILTER 0x2801`, `GL_TEXTURE_MAG_FILTER 0x2800`,
  `GL_TEXTURE_WRAP_S 0x2802`, `GL_TEXTURE_WRAP_T 0x2803`,
  `GL_CLAMP_TO_EDGE 0x812F`, plus `GL_MODULATE 0x2100` if the
  vignette path uses texture-color modulation.

`tests/gl-stubs/include/GL/gl_stub_counts.h` — add matching
`X(glGenTextures)`, `X(glDeleteTextures)`, `X(glTexImage2D)`,
`X(glCopyTexSubImage2D)`, `X(glTexParameteri)` entries to the
`GL_STUB_COUNTER_LIST` X-macro.

Real builds use system GL headers, which already declare all of these —
no real-header changes needed.

### Makefile

Add `src/scene/postprocess.c` to the scene object list (same place
`src/scene/backdrop.c` is listed, including the `scene_demo` target so
the contract binary still links).

## Critical files

| File | Change |
|---|---|
| `src/scene/postprocess.c` / `.h` | **new** — texture + curved/scanline render |
| `src/scene/render.c` | call from `scene_render_3d_scene()` + `scene_render_init_gl()` |
| `src/scene/render_types.h` | `int crt_mode;` in `SceneRenderConfig` |
| `src/scene/postprocess.c` | private 2D ortho/attrib bracket; do not include `src/ui/gl_2d.h` |
| `src/app/glr_config.h/.c`, `glr_state.h/.c`, `glr_defaults.h`, `glr_actions.c`, `glr_ctrl.c` | config plumbing (mirror `backdrop_mode`) |
| `tests/gl-stubs/include/GL/gl.h`, `gl_stub_counts.h` | texture stubs |
| `Makefile` | add `postprocess.c` (incl. `scene_demo`) |

## Verification

1. `make sample` — build the real renderer.
2. `./sample`, load an example (e.g. F12 to a geometry scene), open the
   **Config** menu, cycle **CRT shader** to **CRT**: scene should show a
   gently bulged image with horizontal scanlines; **Off** restores the
   flat image. Confirm the code panel/menu/HUD remain undistorted and
   crisp (effect is scene-viewport only).
3. Toggle **MSAA / accum AA** (Ctrl+U and the AA config) with CRT on —
   verify the effect still captures the resolved (AA'd) image and there
   is no flicker or stale-frame ghosting.
4. Resize the window larger and smaller — image stays correctly mapped
   (texture reallocation on growth; no smearing/garbage).
5. Save a workspace/output.c with CRT on, reload it — confirm the
   `// @cfg crt_shader = 1` header round-trips and CRT comes back on.
6. `make test-stubs` and `make sample USE_GL_STUBS=1` — both compile
   (stub texture symbols present).
7. `make scene_demo` — links and runs (proves `src/scene/` stays
   REPL-independent). Optional but useful: add a small scene_demo key/HUD
   toggle for CRT if we want the demo to visually exercise the effect rather
   than only proving linkage.
8. `make test` — full suite green; `make check-state-ownership` clean.
