# Runtime point-parameter detection (replace NO_POINT_PARAMETER)

Status: **DONE - implemented and landed** on branch
`feature/runtime-point-parameter-detection` (commit 87d1bcc; implemented
directly at user request, bypassing the not-started/active steps).
Scope as locked: **silent executor fallback** (exact behavior parity
with the old `NO_POINT_PARAMETER` - no commit-time rejection, status
note, or autocomplete/help changes); runtime override is the env var
**`GLR_NO_POINT_PARAMETER`** (any non-empty value forces the unsupported
path). Verification gate all green: `make test` (36/36, 4867/4867),
`make test-stubs` (42/42, 5370/5370), `make sample`, `make sample
USE_GL_STUBS=1`, `make check-state-ownership`; no compile-time
`NO_POINT_PARAMETER` mechanism remains.

## Context

`glPointParameterfv` (distance-attenuated point size) isn't available on
every GL. Today this is a **compile-time** `NO_POINT_PARAMETER` macro: a
platform without it must be rebuilt with `make … NO_POINT_PARAMETER=1`.
That's the wrong axis - support is a property of the runtime GL context,
not the build. Switch to a **runtime** check via
`glutExtensionSupported`, make the REPL executor stop calling the
unsupported entry point (falling back to the existing camera-distance
`glPointSize` approximation so points still attenuate visually), and keep
an env-var override so the disabled path stays testable on hardware that
*does* support it.

One deliberate improvement over the old flag: today's
`#ifdef NO_POINT_PARAMETER` never actually guarded the executor's
`CMD_POINT_PARAMETER_FV` `glPointParameterfv` call (only the `glPointSize`
redefine). The new runtime gate *does* skip that call when unsupported -
the user's explicit "disable from the REPL when not supported"
requirement.

## Current wiring (verified)

- `src/repl/executor.c:53-67` - `#ifdef NO_POINT_PARAMETER` defines
  `_repl_point_size(sz)` (scales by camera distance from the
  controller-installed `g_camera_distance_source`) and
  `#define glPointSize _repl_point_size`, text-substituting **every**
  `glPointSize(` in the TU below line 66 (parity must be preserved).
- `src/repl/executor.c:298-302` - `CMD_POINT_PARAMETER_FV` calls
  `glPointParameterfv(...)` **unconditionally** (not currently gated).
- `src/repl/executor.c:~481` - `CMD_POINT_SIZE` → `glPointSize(...)`
  (main user-facing site; enumerate all `glPointSize(` below the old
  `#define` and route them through the new runtime helper).
- `src/repl/export.c:531` - `#ifndef NO_POINT_PARAMETER` gates the
  `point_attenuation`-slugged `glPointParameterfv(...)` init-bootstrap
  entry (apply + exported standalone C). A slug/toggle mechanism
  (`init_bootstrap_toggle_get`, `point_attenuation` slug) already exists
  for the disable path - reuse it; no parallel mechanism.
- `src/repl/executor.h:98-112` -
  `repl_executor_install_camera_distance_source` is the established
  "controller installs a value the GL-free REPL needs" pattern to mirror.
- `src/app/glr_ctrl.c:1864 glr_ctrl_init_gl()` already does post-context
  GL queries (`glGetIntegerv(GL_SAMPLES)` ~1872) and installs the
  camera-distance source (~1724). Detection belongs here (app layer may
  call `glut*`; REPL may not - `check-gl-boundaries`).
- `scripts/check-no-point-parameter-builds.sh` (+ Makefile `.PHONY`
  ~162, aggregator ~924, target ~1006-1007) is a
  compile-with-`-DNO_POINT_PARAMETER` syntax guard; obsolete once the
  macro is gone.
- Compile-time conditionals in tests:
  `tests/test_repl_executor.c:430,455` (`#ifdef`),
  `tests/test_repl_core_io.c:142,229,235` (`#ifndef`).

## Approach

### 1. Executor: runtime flag + helper (`src/repl/executor.c` / `.h`)

- `static int g_point_parameter_supported = 1;` (default **supported** -
  demo/tests/no-install behave like today's default build).
- Public API mirroring the camera-distance source:
  - `void repl_executor_set_point_parameter_supported(int supported);`
  - `int  repl_executor_point_parameter_supported(void);` (for export.c)
  Document in `executor.h`; refresh the `NO_POINT_PARAMETER` comments
  there.
- Delete the `#ifdef NO_POINT_PARAMETER … #define glPointSize
  _repl_point_size` block. Replace with an always-compiled helper:
  ```c
  static void repl_exec_point_size(GLfloat sz) {
      if (!g_point_parameter_supported) {
          float d = g_camera_distance_source ? g_camera_distance_source() : 0.0f;
          glPointSize(d > 0.0f ? sz * (2.0f / (0.5f * d)) : sz);
      } else {
          glPointSize(sz);
      }
  }
  ```
- Route **every** `glPointSize(` site in `executor.c` that was under the
  old `#define` (enumerate via grep; at minimum `CMD_POINT_SIZE` ~481)
  through `repl_exec_point_size()` - preserves the macro's whole-TU
  substitution exactly.
- Gate `CMD_POINT_PARAMETER_FV` (298): call `glPointParameterfv(...)`
  only when `g_point_parameter_supported`; else no-op (the
  `repl_exec_point_size` path supplies the visual fallback).

### 2. Export: runtime-gate the bootstrap entry (`src/repl/export.c`)

- Remove the `#ifndef NO_POINT_PARAMETER` around the `point_attenuation`
  bootstrap entry; keep the array entry unconditionally compiled.
- When `!repl_executor_point_parameter_supported()`, **skip the
  `point_attenuation` bootstrap entry entirely** - do not apply it and
  do not emit it in exported standalone C. **Do NOT route through the
  existing toggle "disabled" path** (`export.c:693-702`): that path
  *neutralizes* by calling `apply_state_cmd()` with a no-attenuation
  `CMD_POINT_PARAMETER_FV`, which **still invokes `glPointParameterfv`**
  - exactly the unsupported entry point we must avoid. Add an explicit
  "supported?" skip ahead of (and independent of) the toggle-disable
  branch in `repl_apply_init_bootstrap()` and the export emitter.
- The executor-side `CMD_POINT_PARAMETER_FV` gate (§1) remains as
  defense-in-depth, but bootstrap must not *rely* on it - it must not
  apply/emit the command at all when unsupported.

### 3. Controller: detect + override (`src/app/glr_ctrl.c`)

- **Ordering is load-bearing.** `glr_ctrl_init_gl()` calls
  `repl_apply_init_bootstrap()` at **line 1869**, *before* the
  `glGetIntegerv(GL_SAMPLES)` block (~1872). Detection +
  `repl_executor_set_point_parameter_supported()` must run **before**
  `repl_apply_init_bootstrap()` (the GL context is already current at
  `glr_ctrl_init_gl` entry - it is called post-`glutInit`/window).
  Place it immediately after `repl_executor_init_resources()` and
  before `repl_apply_init_bootstrap()`. Putting it in the
  `glGetIntegerv` area (the original draft) is too late and would still
  hit the bootstrap `glPointParameterfv` path on unsupported hardware.
- **Detection must not be extension-only.** `glPointParameterfv` is
  core GL 1.4, so an ARB/EXT-only check false-negatives on a 1.4+ core
  context that doesn't advertise the extension string. Check the GL
  version first:
  ```c
  int gl_major = 0, gl_minor = 0;
  const char *ver = (const char *)glGetString(GL_VERSION);
  if (ver) sscanf(ver, "%d.%d", &gl_major, &gl_minor);
  int pp = (gl_major > 1) || (gl_major == 1 && gl_minor >= 4)
        || glutExtensionSupported("GL_ARB_point_parameters")
        || glutExtensionSupported("GL_EXT_point_parameters");
  const char *ov = getenv("GLR_NO_POINT_PARAMETER");
  if (ov && ov[0]) pp = 0;
  repl_executor_set_point_parameter_supported(pp);
  ```
  (`GL_VERSION` strings may have a vendor suffix after `major.minor`;
  `sscanf("%d.%d")` stops at the first non-numeric, which is fine.)
  `getenv`/`sscanf` → `<stdlib.h>`/`<stdio.h>` (already included).
  Update the `NO_POINT_PARAMETER` comments at `glr_ctrl.c:1556` and
  `:1718` to the runtime model.
- Also feed the capability into the per-frame scene config so the
  scene layer can gate its own direct call (see §3a).

### 3a. Scene backdrop direct call (`src/scene/backdrop.c`, `render_types.h`)

`src/scene/backdrop.c:266` calls
`glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, …)` directly for the
star backdrop - independent of the REPL executor. With
`GLR_NO_POINT_PARAMETER=1` (or genuinely unsupported HW) and
`Backdrop = Stars` / `City + Stars`, this still hits the unsupported
entry point. Gate it:

- Add `int point_parameter_supported;` to `SceneRenderConfig`
  (`src/scene/render_types.h`, near `backdrop_mode`), mirroring the
  `post_filter_mode` capability-flow pattern.
- In the controller scene-config builder (`src/app/glr_ctrl.c`, beside
  `config->backdrop_mode = …`), set
  `config->point_parameter_supported = repl_executor_point_parameter_supported();`
  (single source of truth - the executor flag set in §3).
- In `backdrop.c`, guard the `glPointParameterfv(...)` call on
  `config->point_parameter_supported`; when unsupported, skip it (the
  stars still render at a fixed `glPointSize` - acceptable visual
  degradation, no attenuation).
- **Non-REPL scene callers (`tools/scene_demo/scene_demo.c`) are left
  as-is, by design.** `scene_demo.c:121 build_config()` memsets the
  config, so `point_parameter_supported` defaults to **0 = unsupported**
  → its star backdrop skips attenuation. This is the *safe* default
  (never call the entry point unless a caller has explicitly confirmed
  support) and mirrors how `post_filter_mode` defaults off via the same
  memset. Do **not** add detection to scene_demo (it has no GL-context
  capability query path and is a link-proof harness, not a feature
  surface). Document this in the scene_demo build_config comment block.

### 4. GL stubs (`tests/gl-stubs/include/GL/gl.h` + `…/freeglut.h`)

- `…/freeglut.h`: add
  `static inline int glutExtensionSupported(const char *name) { (void)name; return 1; }`
  (plain no-op style like `glutInit`; returns 1 so the stub-built
  controller defaults to "supported" == today's default; no
  `gl_stub_counts` entry needed).
- `…/gl.h`: the §3 detection adds a **new dependency on `GL_VERSION`
  and `glGetString`, neither of which exists in the stub `gl.h`** -
  `USE_GL_STUBS=1` builds (and `make test-stubs`) will not compile
  without them. Add:
  - `#define GL_VERSION 0x1F02`
  - `static inline const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)"2.1 stub"; }`
    (also add `GL_EXTENSIONS 0x1F03`, `GL_RENDERER 0x1F01`,
    `GL_VENDOR 0x1F00` constants for completeness; only `GL_VERSION` is
    consumed). Returning `"2.1"` makes the stub `sscanf` yield major=2
    → `pp` true, i.e. stub default = "supported", consistent with the
    `glutExtensionSupported`→1 stub and today's default build. Match
    the file's existing inline no-op style; a `gl_stub_tick` /
    `gl_stub_counts` entry is optional (other queries like
    `glGetIntegerv` do tick - follow whatever the neighboring query
    stubs do for consistency).

### 5. Remove the obsolete compile guard and the NO_POINT_PARAMETER knob

- Delete `scripts/check-no-point-parameter-builds.sh`.
- Remove its three Makefile lines: `.PHONY` (~162),
  `check-state-ownership` aggregator entry (~924), and the target
  (~1006-1007). Confirm the aggregator still runs clean without it.
- **Remove the build knob itself**: `Makefile:127-128`
  (`ifeq ($(NO_POINT_PARAMETER),1) / CFLAGS += -DNO_POINT_PARAMETER`)
  and the help-text line `Makefile:1236`
  (`@printf "No PointParameter: add NO_POINT_PARAMETER=1 …"`).
- **README.md**: remove the `### NO_POINT_PARAMETER=1` section
  (`README.md:3-13`, incl. the `make sample/glut NO_POINT_PARAMETER=1`
  examples).
- Update `CLAUDE.md` if it documents the script / `NO_POINT_PARAMETER`.

### 6. Tests (compile-time → runtime)

- `tests/test_repl_executor.c:430-456`: replace the `#ifdef
  NO_POINT_PARAMETER` block with two runtime cases -
  `repl_executor_set_point_parameter_supported(0)` then assert
  `CMD_POINT_PARAMETER_FV` does **not** tick the `glPointParameterfv`
  stub counter and `CMD_POINT_SIZE` applies the camera-distance scaling;
  `(1)` then assert it ticks. Reset to `1` after. (Verify
  `GL_STUB_glPointParameterfv` exists in `gl_stub_counts.h`; add if
  missing.)
- `tests/test_repl_core_io.c:142,229,235`: replace `#ifndef
  NO_POINT_PARAMETER` with runtime - default (supported) asserts the
  exported init body **contains** the `glPointParameterfv` line; after
  `repl_executor_set_point_parameter_supported(0)` it asserts the line
  is **omitted**. Restore the flag at end of block.
- This runtime executor test also replaces the behavioral value of the
  deleted `check-no-point-parameter-builds.sh`.

## Critical files

| File | Change |
|---|---|
| `src/repl/executor.c` | drop `#ifdef`; runtime flag + `repl_exec_point_size`; gate `CMD_POINT_PARAMETER_FV`; route all `glPointSize(` sites |
| `src/repl/executor.h` | set/get supported API + doc; refresh `NO_POINT_PARAMETER` comments |
| `src/repl/export.c` | runtime-gate: **skip** `point_attenuation` apply+emit when unsupported (NOT the neutralize-via-`apply_state_cmd` path); remove `#ifndef` |
| `src/app/glr_ctrl.c` | detect (GL_VERSION≥1.4 ∥ ARB ∥ EXT) + `GLR_NO_POINT_PARAMETER` **before `repl_apply_init_bootstrap()` (line 1869)**; install into executor; set `config->point_parameter_supported` in scene-config builder; comment refresh |
| `src/scene/render_types.h` | add `int point_parameter_supported;` to `SceneRenderConfig` (near `backdrop_mode`) |
| `src/scene/backdrop.c` | gate the direct `glPointParameterfv` (line ~266) on `config->point_parameter_supported`; skip when unsupported |
| `tools/scene_demo/scene_demo.c` | **no code change** - memset default (0=unsupported) is the intended safe default; add a one-line comment in `build_config()` documenting it |
| `tests/gl-stubs/include/GL/freeglut.h` | add `glutExtensionSupported` stub (returns 1) |
| `tests/gl-stubs/include/GL/gl.h` | **add `GL_VERSION` (+`GL_EXTENSIONS/RENDERER/VENDOR`) constants and a `glGetString` stub** returning `"2.1 stub"` - required or `USE_GL_STUBS=1` / `make test-stubs` won't compile the new §3 detection |
| `Makefile` | remove `check-no-point-parameter-builds` (.PHONY ~162 + aggregator ~924 + target ~1006-7) **and** the build knob (`~127-128`) + help line (`~1236`) |
| `scripts/check-no-point-parameter-builds.sh` | delete |
| `tests/test_repl_executor.c`, `tests/test_repl_core_io.c` | compile-time `#ifdef/#ifndef` → runtime via the new setter |
| `README.md` | remove the `### NO_POINT_PARAMETER=1` section (lines ~3-13) |
| `CLAUDE.md` | drop/replace `NO_POINT_PARAMETER` references |

## Reuse / conventions

- Mirror `repl_executor_install_camera_distance_source`
  (executor.h:98-112) for the controller→REPL plumbing; keep REPL
  GL-free (`check-gl-boundaries`).
- Reuse the `point_attenuation` slug, but the unsupported case is an
  **explicit skip** (no apply/emit), *not* the existing toggle
  neutralize path (which re-invokes `glPointParameterfv`).
- Scene-capability flow mirrors the `post_filter_mode`
  `SceneRenderConfig` field pattern.
- Env-var override (not a CLI flag) chosen for test ergonomics.

## Verification

1. Branch `feature/runtime-point-parameter-detection` off `main`.
2. `make test-stubs` - full suite + `check-state-ownership` green
   **without** the removed guard (confirm aggregator still passes);
   `make test`; `make sample`; `make sample USE_GL_STUBS=1`.
3. Focused: `make test_repl_executor USE_GL_STUBS=1`,
   `make test_repl_core_io USE_GL_STUBS=1`.
4. `grep -rn NO_POINT_PARAMETER src tests scripts Makefile README.md
   CLAUDE.md` returns nothing (scope to the live tree - historical
   `plans/` docs legitimately still mention it; do not grep `plans/`).
5. Manual (real GL): `./sample` - points attenuate normally;
   `GLR_NO_POINT_PARAMETER=1 ./sample` - points still size-attenuate via
   the `glPointSize` approximation; Ctrl+S yields an `output.c` whose
   `init()` omits the `glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,
   …)` line; and switching Backdrop to **Stars** / **City + Stars**
   does not call `glPointParameterfv` (no GL error / crash on
   genuinely-unsupported HW). Both runs stable.

## Resolved open question - user-authored `glPointParameterfv` in export

Only the *injected* `point_attenuation` bootstrap entry is gated by
support (§2). A **user-typed** `glPointParameterfv(...)` command is
intentionally **left in exported standalone C unchanged**, even when
the local runtime doesn't support it (the live executor still no-ops
it via the §1 gate). Rationale + decision (least-work, and the more
correct behavior): exported C is a portable program the user may
compile on *different* hardware that does support the entry point;
stripping user-authored source based on the authoring machine's
runtime capability would be surprising and lossy. This is **not a
real compatibility path** - no export-skip logic and no extra tests
for user-authored `CMD_POINT_PARAMETER_FV`. Documented here as
intentional so it isn't re-raised.

## Folder note

`plans/done/` = implemented and verified. Lifecycle for this plan:
in-review → (implemented directly at user request) → `done/`.
