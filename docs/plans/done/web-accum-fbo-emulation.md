# Accumulation buffer on the web build: detection + FBO emulation

Status: done - **Part 1 and Part 2 both implemented** (accum-bits
detection + the gl4es FBO emulation, shipped as local patch #7
`packaging/web/patches/gl4es-accum-fbo.patch`; the companion web
keyboard-shim fix delivers Ctrl+= / Ctrl+- so the passes step works
in the browser)
Date: 2026-07-16
Scope: Emscripten/WebGL2 build (gl4es); Part 1 also touches every backend's
startup path.

## Problem

`glAccum` is an explicit no-op stub in gl4es
(`third_party/web/gl4es/src/gl/wrap/glstub.c`, `STUB(glAccum)`), and the
Emscripten canvas has no accumulation buffer to begin with. The app has **no
runtime detection** - `use_accum` is only the `--noaccum` CLI flag
(`gl_repl.c`), and the accum loop in `render3d_draw_scene`
(`src/render3d/render.c`, the `do_accum` block around lines 839-897) trusts
it. Consequences on web today:

- **Accum AA** (the default effect, 2 passes): every frame renders the whole
  3D scene **twice**; both `glAccum(GL_ACCUM, 0.5)` calls and the final
  `glAccum(GL_RETURN, 1.0)` do nothing, so the user sees the last pass only -
  2x GPU/CPU cost for zero visual benefit.
- **Blur / Blur Cam** (up to 16 passes, each with a per-pass reflatten):
  up to 16x cost, again with no effect.

Two-part plan: a cheap detection fix that stops the waste immediately, then a
gl4es patch that implements the accum buffer with an FBO - which flips the
same detection back on and makes Blur/Blur Cam actually work in the browser.

## Part 1 - runtime accum detection (all backends) - IMPLEMENTED

**Change (as landed).** Query the context's real accumulation depth once at
GL init and force `use_accum` off when it is zero:

1. `glr_ctrl_init_gl()` (`src/app/glr_ctrl.c`) - next to the existing
   `glGetIntegerv(GL_SAMPLES, &samples)` precedent - queries
   `GL_ACCUM_RED_BITS` into a variable **initialized to 0**, then one
   `(void)glGetError()` to clear the `GL_INVALID_ENUM` a GLES context
   raises, and records the result in the new `GlrRenderState.accum_bits`
   field (compile-time default `-1` = never probed, so tests and the
   dump-only paths that skip `glr_ctrl_init_gl` keep their old behavior).
2. `glr_ctrl_set_accum(enabled)` (`src/app/glr_ctrl.c`) masks with it:
   a probed `accum_bits == 0` forces `use_accum` off. This placement
   matters - `main()` calls `glr_ctrl_set_accum(use_accum)`
   (`gl_repl.c:727`) *after* `glr_ctrl_init_gl()` (`gl_repl.c:663`), so a
   force-off inside init alone would be overwritten.
3. When accum was requested but is unavailable, one stderr line
   (`accum: GL context has no accumulation buffer; accum effects disabled`)
   makes the silent behavior change discoverable.
4. Supporting: the stub GL header gained `GL_ACCUM_RED_BITS`, `GL_NO_ERROR`,
   and a counted no-op `glGetError` stub (`tests/gl-stubs/include/GL/gl.h`,
   `gl_stub_counts.h`); the stub `glGetIntegerv` already returns 0 for
   unknown pnames, so stub builds honestly report "no accum".

**Why the query shape is safe per backend:**

| Backend | `GL_ACCUM_RED_BITS` result | Effect |
|---|---|---|
| macOS Cocoa / Linux GLX (native) | real bits (window requested `GLUT_ACCUM`, `gl_repl.c:658`) | accum stays on - no behavior change |
| OSMesa (headless captures) | **16** - the vendored freeglut backend maps `GLUT_ACCUM` to 16 accum bits (`third_party/freeglut/src/osmesa/fg_window_osmesa.c:40`) | accum stays on - `scripts/docs-assets.sh`'s `GLR_ACCUM_PASSES=16` accumulation AA is unaffected (verified in source; re-render one full-UI doc asset as the regression check) |
| Emscripten / gl4es | gl4es's getter has no `GL_ACCUM_*_BITS` case, so the pname falls through to the WebGL driver → `GL_INVALID_ENUM`, out-param untouched → stays 0 | accum forced off - the waste stops. Hence init-to-0 + the `glGetError()` clear |
| GL stubs (`USE_GL_STUBS=1`) | no-op getter → 0 | accum off; stub builds don't render, harmless |

**Non-goals for Part 1:** no menu/config surface changes. The Accum effect
config row still cycles; with `use_accum == 0` the `do_accum` branch simply
never runs (existing `config->use_accum` gating). Optionally the status line
could note "accum unavailable" when cycling the effect on web - nice-to-have,
not required.

## Part 2 - gl4es patch: accum buffer via FBO - IMPLEMENTED

**As landed** (deltas from the sketch below):

- New `src/gl/accum.c` (+`accum.h`) in gl4es; ops draw through a private
  `uScale` shader (one program serves ACCUM/LOAD/RETURN - additive blend
  on for ACCUM, off for LOAD/RETURN) rather than `gl4es_blitTexture`
  itself, but follow its exact state-hygiene pattern (raw GLES draws
  under `glPushAttrib(GL_ENABLE_BIT|GL_COLOR_BUFFER_BIT)`, tracked
  `gleshard` program/attrib state updated so user fpe re-realizes).
- **The format probe cannot use hardext hints.** The plan's
  "extension check" option is a dead end on the very target this is
  for: Emscripten runs `GetHardwareExtensions` with `notest` (no
  `gles_getProcAddress` hook is installed), so `hardext.floatfbo` /
  `halffloattex` stay 0 there. The landed probe tries formats outright
  - RGBA16F/`GL_HALF_FLOAT`, then unsized `GL_HALF_FLOAT_OES`, then
  RGBA8 - judged by a one-time 4x4 FBO-completeness test.
- Drawable-size resolution for the FBO-0 case (gl4es tracks no canvas
  size on Emscripten): the `getMainFBSize` hook if installed, else the
  Emscripten drawing-buffer query, else `mainfbo_*` / viewport extent.
- `glClearAccum` is also implemented (was a stub too); the getter cases
  live in `gl4es_commonGet` so `glGetIntegerv` and `glGetFloatv` both
  serve `GL_ACCUM_*_BITS`.
- **The framebuffer snapshot texture is GL_RGB, not RGBA.** GLES3/WebGL2's
  `glCopyTexSubImage2D` compatibility rule requires the destination's
  components to be a subset of the read buffer's, and the WebGL canvas
  has no alpha channel (GLUT never requests one → `alpha:false` →
  effectively RGB8), so an RGBA destination fails with
  `GL_INVALID_OPERATION` and the copy silently drops - the first cut
  shipped that way and RETURN painted the whole scene rect black
  (found by interactive testing; the 2D overlays drawn after RETURN
  survived, which is what localized the failure to the accum content).
  RGB works against both RGB8 and RGBA8 read sources.

Verified (2026-07-16, headful Chrome with `GLR_ACCUM_PASSES=2` forced -
two gotchas made the first verification pass vacuous: headless Chrome
barely pumps rAF frames so `glAccum` never ran at all, and the passes
default is 1 so even headful sessions never enter the accum loop
unassisted): pixel-level readbacks logged from inside the ops show the
scratch copy captures the exact backbuffer value and the accum texture
holds the correctly weighted sum (`0.5×` per pass, `Σ=1`), `GL_RETURN`
reconstructs the frame, "LIBGL: Accum buffer emulation active (16 bits
per channel)" on startup, Part 1's disable notice gone (detection
re-enabled accum with zero app changes), and the WebGL console-error
count matches a no-patch baseline. Patch mechanics: `git apply --check`
clean on the pristine pin alone and after the other six, full 7-patch
stack byte-identical to the working tree.

### Original Part 2 plan

### What actually needs emulating

The app's usage (the only accum client) is narrow - `src/render3d/render.c`:

```
glClear(... | GL_ACCUM_BUFFER_BIT)      // once per frame, full window
loop N:  render pass; glAccum(GL_ACCUM, 1/N)
glAccum(GL_RETURN, 1.0)                  // optionally scissored to scene rect
```

No `GL_LOAD` / `GL_ADD` / `GL_MULT`, no non-1.0 `GL_RETURN` scale. The FBO
translation is the textbook accumulate-in-a-texture pattern:

| GL op | Emulation |
|---|---|
| `glClear(GL_ACCUM_BUFFER_BIT)` | clear the internal accum FBO. Interception point already exists: `gl4es_glClear` masks the accum bit out today (`src/gl/gl4es.c:1146`) |
| `glAccum(GL_ACCUM, w)` | `glCopyTexSubImage2D` the current read framebuffer into a scratch texture, then draw it into the accum FBO as a fullscreen quad with additive blending scaled by `w` (`glBlendColor` + `GL_CONSTANT_ALPHA`, or a tiny private shader) |
| `glAccum(GL_RETURN, s)` | draw the accum texture to the current draw framebuffer, replace mode, modulated by `s` |
| `glAccum(GL_LOAD, w)` | same as ACCUM without blending (trivial; include for completeness) |
| `GL_ADD` / `GL_MULT` | leave TODO - nothing in gl-repl (or most fixed-function code) uses them |

Scissor semantics come for free: real `glAccum` honors the scissor box, and
the emulation's copies/quad draws inherit the live scissor state - so the
app's `use_accum_aa_scissors` option maps 1:1.

### Why gl4es is a good host

The risky machinery already exists in-tree:

- **`gl4es_blitTexture`** (`src/gl/blit.c`) - the internal "draw a texture
  into the current framebuffer with a private shader while preserving user
  fpe/program state" helper the bitmap-font emulation uses mid-frame. That is
  exactly the state-hygiene problem the accum ops have, already solved and
  battle-tested.
- **FBO wrappers** (`src/gl/framebuffers.c`) for the internal accum target.
- `glCopyTexSubImage2D` from the (possibly MSAA) canvas backbuffer performs
  an implicit resolve in WebGL2 - the same mechanism the app's Post FX filter
  already uses successfully on web.

New code is essentially an `accum.c`: glstate fields (accum texture + FBO +
scratch texture + dims), lazy creation sized to the drawable, recreate on
canvas resize, the ops, the clear hook, and replacing the `STUB(glAccum)`
export. Estimated **300-400 lines** - larger than the attrib-stack patches,
smaller than color-material. Ships as local patch #7
(`packaging/web/patches/gl4es-accum-fbo.patch`), registered last in
`scripts/web-deps.sh` `GL4ES_PATCHES` and listed in
`docs/THIRD_PARTY_LICENSES.md`, same as the existing six.

### Format, capability probe, and the detection tie-in

- **Accum target format: RGBA16F.** Rendering/blending into half-float needs
  WebGL2's `EXT_color_buffer_float` - effectively universal on modern desktop
  browsers. gl4es's `hardext` probes float *texturing* only
  (`floattex`/`halffloattex` in `src/glx/hardext.h`), so add a renderability
  probe (extension check, or a one-time FBO-completeness test).
- **RGBA8 fallback** when float renderability is absent: fine at the AA
  default (2 passes; each contribution quantizes to 1/255), visibly bands at
  8-16-pass Blur. Acceptable as a degraded mode.
- **Report `GL_ACCUM_RED/GREEN/BLUE/ALPHA_BITS`** (16 or 8) from gl4es's
  getter when the emulation is live. This is the tie-in that makes Part 1
  self-healing: the app's detection sees nonzero bits and re-enables accum on
  web automatically - no app-side changes in Part 2.
- Precision: 16F (10-bit mantissa) is comfortable for ≤16 passes of
  1/N-weighted adds; a real accum buffer was typically 16-bit integer.

### Costs and one expectation-setting caveat

Per pass the emulation adds one backbuffer copy plus one quad draw on top of
the scene re-render, which already dominates (Blur even reflattens the
program per pass). Same cost class as the Post FX filter's per-frame
`glCopyTexSubImage2D`, which is already usable on web.

Caveat: `packaging/web/shell.html` sets no `webglContextAttributes`, so the
canvas likely has MSAA on by default. That makes accum **AA** partly
redundant on web (and each copy pays an MSAA resolve). The emulation's real
payoff is **Blur / Blur Cam**, which have no MSAA substitute. If AA-2-pass
proves visually redundant on web, consider defaulting the effect to Off there
while leaving Blur available - a follow-up, not part of this plan.

### Risks

- **State save/restore hygiene inside the ops** (FBO binding, blend, depth
  mask, program). Mitigated by routing draws through `gl4es_blitTexture` and
  restoring the tracked gl4es state around the op.
- **Resize lifecycle**: the accum/scratch textures must track drawable size;
  recreate on mismatch at op time (accum contents are per-frame scratch for
  this app, so a resize glitch frame is harmless).
- **Existing-checkout migration**: same note as the attrib-stack amendments -
  a stale `third_party/web/gl4es` needs `git checkout -- src` (or deletion)
  if patch stacking ever reports "already applied" spuriously.

## Alternatives considered

- **App-side ping-pong running average** (`glCopyTexSubImage2D` + blended
  quads, GL 1.1 only): no gl4es change, but 8-bit precision (banding worse at
  high pass counts, and the i/(i+1) running-average compounds rounding), and
  it pollutes the portable fixed-function app code with a web-shaped path.
  Rejected - the project's pattern is to fix GL-stack gaps in gl4es patches
  and keep the app authored against real GL semantics.
- **Do nothing / default accum off on web**: stops the waste (Part 1 achieves
  this generically) but leaves Blur/Blur Cam permanently dead in the browser.

## Verification

Part 1 (done at implementation time):
- Native: `FREEGLUT_CAPTURE_FRAMES=2 ./gl-repl --no-audio` - no accum
  notice on stderr (real context reports accum bits), frames render.
- `make test`, `make test-stubs`, `make check-state-ownership`,
  `make check-c99`, `make gl-repl USE_GL_STUBS=1` all green. (The `make
  test` run surfaced a pre-existing, unrelated failure: the two newest
  GL-state-popup tests called `glr_ctrl_display_frame()` with the popup
  visible without the file's `#ifdef GL_STUBS` guard, crashing real-GL
  test binaries inside `ui_gl_state_panel_render`'s `gl2d_begin`; both
  sites now carry the established guard pattern.)
- Web: build links; the runtime notice + the frame-cost halving on the
  default example are browser-observable (`make web-serve`).
- OSMesa: not re-verified here (needs the Homebrew Mesa build); the
  backend requests 16 accum bits under `GLUT_ACCUM`
  (`third_party/freeglut/src/osmesa/fg_window_osmesa.c:40`), so detection
  keeps accum on. Re-run one `scripts/docs-assets.sh` asset when
  convenient to confirm pixel-identical output.

Part 2:
- Web: `GL_ACCUM_RED_BITS` reports 16 (detection re-enables accum); Accum AA
  visibly antialiases with MSAA forced off (`webglContextAttributes` in a
  test shell); Blur on a `t`-animated example (e.g. orbit-ring) shows motion
  trails matching the native build; 8-bit fallback path exercised by forcing
  the probe off.
- Native + OSMesa: unchanged (patch only affects the gl4es build).
- Patch mechanics: `git apply --check` clean on the pristine pin with and
  without the other patches, full `scripts/build-web.sh` from a reverted
  tree, byte-compare tree vs. patch application (the workflow used for
  patches #5/#6).
