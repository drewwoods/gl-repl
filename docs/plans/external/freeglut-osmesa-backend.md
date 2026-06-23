# Add an OSMesa (off-screen, window-system-less) backend to freeglut

> **External plan — IMPLEMENTED.** This work is done **in a freeglut fork**,
> not in gl-repl. It lives here only as the spec that motivated it (gl-repl
> wants a headless GL context so `--export-ply`'s `GL_FEEDBACK` capture —
> including the GLUT solids — can be validated in CI without a display).
>
> **Where the work lives:** the freeglut fork at
> <https://github.com/drewwoods/freeglut>, branch **`osmesa-backend`**. That
> branch carries the OSMesa backend described below **plus** the headless
> capture extras gl-repl drives (the `SIGUSR1` PPM screenshot and the
> `FREEGLUT_CAPTURE_FRAMES` record mode). gl-repl vendors it via
> `scripts/vendor-freeglut.sh` (`FREEGLUT_REPO=https://github.com/drewwoods/freeglut
> scripts/vendor-freeglut.sh osmesa-backend`); the resolved SHA is pinned in
> `third_party/freeglut/VENDORED.txt`. Upstreaming to freeglut proper is still
> open.
>
> Reference checkout used to write this plan: `~/src/freeglut-fork`
> (freeglut **3.8.0**; macOS Cocoa build) — now published as the fork above.
> File paths below are relative to the freeglut source root. Line numbers are
> approximate — match by symbol.

## Why

freeglut creates the GL **context** via the platform's native mechanism
(GLX on X11, WGL on Windows, NSOpenGL on Cocoa, EGL on the GLES path). OSMesa
("Off-Screen Mesa") is a *different* context-creation path: it renders into a
**CPU memory buffer you allocate** with **no window system at all** (no X11, no
GLX, no Cocoa, no EGL). API surface is tiny: `OSMesaCreateContextExt`,
`OSMesaMakeCurrent(ctx, buffer, type, w, h)`, `OSMesaDestroyContext`,
`OSMesaGetProcAddress`, `OSMesaPixelStore`, `OSMesaGetColorBuffer`.

The motivating consumer (gl-repl) needs a **legacy/compatibility** desktop-GL
context that supports **`glRenderMode(GL_FEEDBACK)`** and freeglut's
`glutSolid*` shapes. OSMesa (software Mesa, llvmpipe/swrast) provides exactly
that, on any OS, with no display server. EGL-surfaceless was rejected: not
native on macOS/Windows and GLES-oriented (no feedback); Xvfb works but is
X11-only and needs a virtual server. An OSMesa backend makes freeglut itself
run truly headless and cross-platform.

## What OSMesa is (and isn't), precisely

- OSMesa provides **both the context and the framebuffer**; the framebuffer is
  memory **you** `malloc`. There is **no surface/window/display object**.
- `OSMesaCreateContextExt(format, depthBits, stencilBits, accumBits, share)`
  returns a context; `format = OSMESA_RGBA` (or `OSMESA_BGRA`). On modern Mesa
  this is a **compatibility** context (fixed-function + feedback available).
- `OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, w, h)` binds the context to a
  buffer of `w*h*4` bytes. All subsequent GL renders into `buf`.
- Pixel origin defaults to `OSMESA_Y_UP = 1` (OpenGL bottom-up). Relevant only
  if anyone reads pixels back; the gl-repl feedback path never reads pixels.
- It is **not** layered on X11/EGL and must not be combined with one — it
  *replaces* the window-system context path.

## freeglut architecture this plugs into (verified in the fork)

freeglut cleanly separates a **window-system backend** from a **context
provider**:

- Window backends: `src/x11/`, `src/cocoa/`, `src/wayland/`, `src/mswin/`,
  `src/android/`, `src/ogc/`. Each provides window open/close, the event loop
  (`fg_main_*`), input, cursor, gamemode, menus.
- Context providers: the X11 backend pairs with **GLX** (`src/x11/fg_*_x11_glx.c`)
  or **EGL** (`src/egl/fg_*_egl.c`), selected by the `FREEGLUT_GLES` CMake
  option. `src/egl/` is a reusable context layer (create/destroy context,
  make-current, swap, config select, proc address) — **the closest template for
  OSMesa.**

The window's context lives on `SFG_Window.Window` (`SFG_Context`,
`src/fg_internal.h:513`):

```c
struct tagSFG_Context {
    SFG_WindowContextType Context;   /* the GL context handle (per-platform typedef) */
    ...
    SFG_PlatformContext pContext;    /* per-provider data: a union — .egl.Surface, X11 FBConfig, Win DC */
};
```

The generic layer calls a fixed set of **~50 `fgPlatform*` functions** that each
backend implements (enumerated from `grep fgPlatform src/fg_*.c`). The generic
entry points to study:

| Generic file | Role | Key calls into the backend |
|---|---|---|
| `src/fg_init.c` | `glutInit`, teardown | `fgPlatformInitialize`, `fgPlatformInitWork`, `fgPlatformCloseDisplay` |
| `src/fg_window.c` | `glutCreateWindow` / destroy | `fgPlatformOpenWindow`, `fgPlatformCloseWindow`, `fgPlatformSetWindow`, `fgPlatformProcessWork` |
| `src/fg_main.c` | `glutMainLoop[Event]` | `fgPlatformProcessSingleEvent`, `fgPlatformMainLoopPreliminaryWork`, `fgPlatformSleepForEvents`, `fgPlatformSystemTime` |
| `src/fg_display.c` | `glutSwapBuffers` | `fgPlatformGlutSwapBuffers` |
| `src/fg_state.c` | `glutGet`/mode | `fgPlatformGlutGet`, `fgPlatformGlutGetModeValues` |
| `src/fg_gl2.c` | version detect | reads `glGetString(GL_VERSION)` → sets `fgState.HasOpenGL20` |

**Why the solids work for free:** `glutSolidTeapot`/`Sphere`/etc.
(`src/fg_teapot.c`, `src/fg_geometry.c`) only need a current context plus the
GL entry points. With OSMesa current, `glGetString` succeeds, `fgInitGL2()`
runs, and (because gl-repl never calls `glutSetVertexAttrib*`) the
`fghDrawGeometrySolid11` client-vertex-array path executes — identical to the
Cocoa build. No solid-specific work is needed.

## Design decision: one new `osmesa` platform (not a separate null window backend)

Because OSMesa has **no window system**, there is nothing for a context layer to
share with. So rather than "new null window backend + OSMesa context provider"
(two dirs), implement **a single platform `src/osmesa/`** that:

- implements the context/display bits with OSMesa, and
- stubs the entire window-manager/input surface (no events, no cursor, no
  gamemode, no joystick/spaceball, no menus-as-windows).

Gate it with a new CMake option `FREEGLUT_OSMESA` → define `TARGET_HOST_OSMESA`
(OS-agnostic; **not** `TARGET_HOST_POSIX_*`, since OSMesa builds on
Linux/macOS/Windows). It is mutually exclusive with `FREEGLUT_GLES`/`WAYLAND`.

### Context storage

Add an `osmesa` arm to the platform unions (mirroring egl), in
`src/fg_internal.h` and a new `src/osmesa/fg_internal_osmesa.h`:

```c
/* SFG_WindowContextType becomes OSMesaContext under TARGET_HOST_OSMESA */
typedef struct {
    void   *Buffer;     /* malloc'd w*h*4 RGBA framebuffer */
    GLsizei Width, Height;
} SFG_PlatformContextOSMesa;   /* -> SFG_PlatformContext.osmesa */
/* SFG_PlatformDisplay.osmesa: essentially empty (OSMesa has no "display") */
```

### Context lifecycle (mirror `src/egl/fg_window_egl.c`)

- `fghCreateNewContextOSMesa(window)`: map `glutInitDisplayMode` →
  `OSMesaCreateContextExt(OSMESA_RGBA, depthBits, stencilBits, accumBits, share)`.
  Use `fgState.DisplayMode` to pick depth/stencil/accum bits (see mapping below).
- `fgPlatformOpenWindow`: create the context, `malloc(w*h*4)`, store in
  `pContext.osmesa`, then `OSMesaMakeCurrent`. Synthesize the initial **reshape**
  and **visibility** callbacks the generic code expects (a real WM would deliver
  these as events; the null backend posts them directly).
- `fgPlatformSetWindow(window)`: `OSMesaMakeCurrent(Context, Buffer,
  GL_UNSIGNED_BYTE, Width, Height)` (re-bind on focus switch / resize).
- `fgPlatformCloseWindow` / `fgPlatformDestroyContext`: `OSMesaDestroyContext`,
  `free(Buffer)`.
- `fgPlatformGlutSwapBuffers`: **no-op** (single buffer). Optionally `glFinish()`
  so a subsequent `OSMesaGetColorBuffer`/readback sees a complete frame.
- `fgPlatformGetProcAddress` / `fgPlatformGetGLUTProcAddress`:
  `OSMesaGetProcAddress`. `fgPlatformExtSupported`: parse `GL_EXTENSIONS`.
  `fgPlatformInitSwapCtl` / `fgPlatformSwapInterval`: no-ops.

### Display-mode mapping (`glutInitDisplayMode` → OSMesa)

| GLUT flag | OSMesa handling |
|---|---|
| `GLUT_RGBA`/`GLUT_RGB` | `OSMESA_RGBA` format |
| `GLUT_DEPTH` | `depthBits = 24` (else 0) |
| `GLUT_STENCIL` | `stencilBits = 8` (else 0) |
| `GLUT_ACCUM` | `accumBits = 16` — **but** llvmpipe usually lacks accum; treat as best-effort and document (gl-repl should pass `--noaccum`) |
| `GLUT_DOUBLE` | single buffer; swap is a no-op |
| `GLUT_MULTISAMPLE` | ignored (no MSAA in swrast); document |
| `GLUT_INDEX` | unsupported; force RGBA |

### Event loop (null, callback-driven)

- `fgPlatformInitialize`: init the monotonic time base; **do not** open any
  display. `fgPlatformSystemTime`: `clock_gettime(CLOCK_MONOTONIC)` (or the
  existing per-platform helper).
- `fgPlatformProcessSingleEvent`: no-op (no input events ever).
- `fgPlatformMainLoopPreliminaryWork`: ensure the first window gets its
  reshape+visibility+display posted.
- `fgPlatformSleepForEvents`: short sleep / return immediately so the timer +
  redisplay work list still drives `display`/`timer` callbacks. `glutPostRedisplay`
  must result in the display callback firing.
- Consumers that want a finite render (e.g. gl-repl `--export-ply`, which exits
  after frame 1) rely only on "first display callback fires"; a fuller idle loop
  (render N frames, honor `glutTimerFunc`) should still work for interactive-ish
  headless use.

### Stubs (return sane defaults; never touch a WM)

`fgPlatformSetCursor`, `fgPlatformWarpPointer`, `fgPlatform{Enter,Leave}GameMode`,
`fgPlatformGetGameModeVMaxExtent`, `fgPlatformRemember/RestoreState`,
`fgPlatformChangeDisplayMode`, `fgPlatform{CopyColormap,GetColor,SetColor}`,
`fgPlatformJoystick*`, `fgPlatform{HasSpaceball,InitializeSpaceball,Spaceball*}`,
`fgPlatformRegisterDialDevice`, `fgPlatformDeinitialiseInputDevices`,
`fgPlatformGlut{SetWindowTitle,SetIconTitle}` (store/ignore),
`fgPlatformHideWindow`, `fgPlatformVisibilityWork`, `fgPlatformPosResZordWork`.
`fgPlatformGlutGet` must answer `GLUT_WINDOW_WIDTH/HEIGHT`, `GLUT_WINDOW_RGBA`,
`GLUT_WINDOW_DOUBLEBUFFER`, depth/stencil/accum bits from the stored config.

## Files to create / modify (in the freeglut fork)

| Path | Change |
|---|---|
| `src/osmesa/fg_internal_osmesa.h` | **new** — `SFG_PlatformDisplay`/`SFG_PlatformContext` osmesa arms, `SFG_WindowContextType = OSMesaContext` |
| `src/osmesa/fg_init_osmesa.c` | **new** — `fgPlatformInitialize/InitWork/CloseDisplay`, time base |
| `src/osmesa/fg_window_osmesa.c` | **new** — context create/destroy, open/close window, `fgPlatformSetWindow` (make-current), buffer alloc, initial reshape/visibility |
| `src/osmesa/fg_display_osmesa.c` | **new** — `fgPlatformGlutSwapBuffers` (no-op), swap-ctl no-ops, `fgPlatformExtSupported` |
| `src/osmesa/fg_state_osmesa.c` | **new** — `fgPlatformGlutGet`, `fgPlatformGlutGetModeValues`, config getters |
| `src/osmesa/fg_main_osmesa.c` | **new** — null event loop: `ProcessSingleEvent`, `MainLoopPreliminaryWork`, `SleepForEvents`, `SystemTime` |
| `src/osmesa/fg_structure_osmesa.c` | **new** — per-window struct alloc/free (mirror `src/egl/fg_structure_egl.c`) |
| `src/osmesa/fg_cursor_osmesa.c` / `fg_ext_osmesa.c` / `fg_gamemode_osmesa.c` / `fg_input_devices_osmesa.c` / `fg_joystick_osmesa.c` / `fg_spaceball_osmesa.c` | **new** — stubs (model after the leanest existing backend, e.g. `src/blackberry/` + `src/android/`) |
| `src/fg_internal.h` | add the `osmesa` union arm to `SFG_PlatformDisplay`/`SFG_PlatformContext`; `#elif TARGET_HOST_OSMESA` typedef for `SFG_WindowContextType` |
| `CMakeLists.txt` | add `OPTION(FREEGLUT_OSMESA ...)`; a new branch in the `IF(WIN32)/ELSEIF(ANDROID...)/ELSEIF(OGC)/ELSE` chain (~L161+) that lists the `src/osmesa/*` sources when `FREEGLUT_OSMESA`; `find_package`/`pkg_check_modules(OSMesa osmesa)`; link `OSMesa::OSMesa`/`-lOSMesa`; `add_definitions(-DTARGET_HOST_OSMESA)` |
| `README.osmesa` (or `README.md`) | document the new build option + deps |

## Build & dependencies

- **Linux:** `apt-get install libosmesa6-dev` (pulls llvmpipe). Configure:
  `cmake -DFREEGLUT_OSMESA=ON -DFREEGLUT_GLES=OFF -DFREEGLUT_BUILD_DEMOS=ON ..`
- **macOS:** `brew install mesa` (provides `libOSMesa.dylib`; confirmed present
  at `/opt/homebrew/lib/libOSMesa.dylib`, `pkg-config osmesa` → yes).
- Resulting `libglut` links `-lOSMesa` and is windowing-system-free. It can
  coexist with a normal freeglut build (separate build dir / lib name suffix,
  e.g. `libglut_osmesa`).

## Verification (in the freeglut fork)

1. **Self-test demo:** build a freeglut `progs/` demo (or a 30-line program):
   `glutInit` → `glutInitDisplayMode(GLUT_RGBA|GLUT_DEPTH)` →
   `glutInitWindowSize(64,64)` → `glutCreateWindow` → in `display`, clear to a
   known color + draw a triangle, then `OSMesaGetColorBuffer` and assert the
   center pixel matches. Proves context + rasterization headless.
2. **GLUT-solid capture:** a `glutSolidTeapot(1)` demo with
   `glRenderMode(GL_FEEDBACK)` → assert a non-zero `GL_POLYGON_TOKEN` count.
   This is the property gl-repl depends on.
3. **No window manager touched:** run under `ssh` with no `DISPLAY` / no Cocoa
   session and confirm it still runs (the whole point).
4. **Cross-platform:** build + run the self-test on Linux **and** macOS.

## Downstream integration (gl-repl — separate, later, not part of this plan)

Once the fork ships `libglut_osmesa`, gl-repl adds a `USE_OSMESA=1` Make variant
that links it + `-lOSMesa` instead of the Cocoa/X11 freeglut, then a CI target:
`./gl-repl <teapot scene> --export-ply out.ply` (already implemented) and assert
the triangle count. This is the headless validation the whole effort unlocks —
but it is **gl-repl** work, tracked separately; this plan stops at freeglut.

## Risks / caveats

- **Accumulation buffer & MSAA:** llvmpipe/swrast typically lack accum and MSAA.
  `GLUT_ACCUM`/`GLUT_MULTISAMPLE` become best-effort/ignored. Document; the
  feedback-export consumer does not need either.
- **Callbacks without events:** the null backend must *synthesize* the initial
  reshape/visibility and honor `glutPostRedisplay`, or `display` never fires.
  This is the subtlest part — study `src/fg_main.c`'s work-list + how
  `src/android/fg_main_android.c` (also event-poor) drives redisplay.
- **`SFG_WindowContextType` typedef churn:** adding a `TARGET_HOST_OSMESA` arm
  touches shared `src/fg_internal.h`; keep it strictly additive so other
  backends are untouched.
- **Mesa feedback support:** `GL_FEEDBACK` is legacy GL; confirm the Mesa build
  routing OSMesa (gallium/llvmpipe vs classic swrast) honors it. It does on
  standard distro Mesa; pin the tested Mesa version in CI.
- **Menus / overlays:** freeglut menus are sub-windows; leave unsupported under
  OSMesa (assert/no-op) — out of scope for headless rendering.
- **Upstreaming:** target a freeglut fork; if upstreaming, follow freeglut's
  backend conventions and add the `FREEGLUT_OSMESA` option to its CI matrix.

## Effort estimate

- Context provider + window create/make-current/destroy (mirror `src/egl/`): ~1 day.
- Null event loop + initial-callback synthesis (the fiddly part): ~1–2 days.
- Stubs + CMake/option/find-OSMesa wiring: ~0.5 day.
- Self-test demos + cross-platform shakeout (Linux + macOS): ~1 day.
- **Total: ~3.5–5 days**, most of the risk concentrated in the event-loop /
  first-frame-callback synthesis. The context half is mechanical (OSMesa's API
  is tiny and the egl provider is a direct template).

## References

- freeglut context-provider template: `src/egl/{fg_window_egl.c,
  fg_display_egl.c, fg_state_egl.c, fg_init_egl.c, fg_structure_egl.c,
  fg_internal_egl.h}`.
- Minimal window backend (stub shape): `src/blackberry/`, `src/android/`.
- Platform contract: the ~50 `fgPlatform*` functions (grep `fgPlatform` across
  `src/fg_*.c`); context storage `SFG_Context` at `src/fg_internal.h:513`.
- CMake selection chain + `FREEGLUT_GLES` pattern: `CMakeLists.txt` (~L69 option,
  ~L161+ platform branches).
- OSMesa API: Mesa `GL/osmesa.h`; `OSMesaCreateContextExt` / `OSMesaMakeCurrent`
  / `OSMesaGetProcAddress` (Mesa docs, "Off-screen Rendering").
- Motivating consumer: gl-repl `src/app/glr_mesh_export.c` (the `GL_FEEDBACK`
  capture) and `plans/done/ply-feedback-export.md`.
