# Vendor freeglut as a static library + license acknowledgement

> Line numbers below are **approximate** (current Makefile/source); match by
> content/symbol, not by line - earlier edits shift them.

## Context

Today the REPL links against a **private freeglut fork** outside the repo at
`~/src/freeglut-fork`, as a **shared** library, via hardcoded paths in the
Makefile (`GL_HEADER_CFLAGS` ~L41-45, `GL_LDFLAGS` ~L121-127) *and* in
`scripts/check-c99.sh` (~L68, L72). The build is therefore **not reproducible**
off this machine and ships a `libglut.dylib` rpath that won't resolve elsewhere.
We want freeglut **vendored into the repo and statically linked**, using the
native macOS **Cocoa** backend (`-DFREEGLUT_COCOA=ON`), plus proper third-party
license acknowledgement (none exists in the repo today).

**Key facts established/verified during planning (against the pinned tip):**

- The Cocoa backend is upstreamed. We pin the **current tip of `freeglut/freeglut`
  master**, SHA `463cef14281f41a8ae88a86590b64dba55b20846` (Apr 16 2026) - not a
  release. It has `src/cocoa/` (15 files), `OPTION(FREEGLUT_COCOA … OFF)`,
  `OPTION(FREEGLUT_BUILD_STATIC_LIBS … ON)`, `FREEGLUT_REPLACE_GLUT` ON on
  non-Windows → static archive output name `glut` → `build/lib/libglut.a`.
- **Cmd-key behavior is preserved - verified, the load-bearing fact.** Our editor
  relies on `GLUT_ACTIVE_SUPER` (a non-standard freeglut constant) so macOS
  Cmd+S/Z/C/`/` work; `src/editor/input.c` has a `#ifndef GLUT_ACTIVE_SUPER →0`
  fallback that would *silently disable* Cmd if upstream didn't expose it. On the
  pinned tip: `include/GL/freeglut_ext.h` defines `GLUT_ACTIVE_SUPER 0x0008`
  (public, pulled in via `<GL/freeglut.h>`), and `src/cocoa/fg_window_cocoa.m`'s
  `updateModifiers:` sets it on `NSEventModifierFlagCommand`. So the fallback
  won't trigger and Cmd handling is identical to the fork.
- **Framework list is exactly** `Cocoa OpenGL IOKit CoreVideo` (upstream's Cocoa
  target). Only `-framework CoreVideo` is new; Foundation/AppKit come via the
  Cocoa umbrella; CoreAudio/CoreFoundation/AudioToolbox are pre-existing audio deps.
- cmake (4.x present) configures it - freeglut's `cmake_minimum_required` uses the
  `3.1...4.x` range form, so CMake 4's "<3.5 removed" rule is satisfied.

## Decisions (confirmed with user)

- **Source:** upstream `freeglut/freeglut`, **current master tip** (not the fork,
  not a tagged release). Re-pinnable via the vendor script.
- **Vendor method:** copy a curated **allowlist** subset into `third_party/freeglut/`,
  plus a **script** (`scripts/vendor-freeglut.sh [<ref>]`) to re-vendor any ref.
- **Linux scope:** macOS-only vendoring. Linux (gracemont CI) keeps the system
  `-lglut -lGL -lGLU` path untouched.
- **Licensing:** add `THIRD_PARTY_LICENSES.md` at repo root (freeglut + miniaudio);
  keep freeglut's `COPYING`/`AUTHORS` inside `third_party/freeglut/`. No new
  project-wide LICENSE.
- **`-DFREEGLUT_STATIC`:** **add it** to `COMMON_CFLAGS` (firm - no-op on
  macOS/Linux, correct for static linkage and Windows-portability; matches the
  static target's own `FREEGLUT_STATIC` PUBLIC define).

## Part A - Vendor script: `scripts/vendor-freeglut.sh`

New executable script (style of `scripts/keymap.sh`; `set -euo pipefail`, temp-dir
cleanup on EXIT trap). Usage: `scripts/vendor-freeglut.sh [<ref>]`, **default
`master`** (so a bare run re-pins to current tip). Steps:

1. Shallow-clone `https://github.com/freeglut/freeglut.git`, `git checkout <ref>`,
   resolve the full SHA (`git rev-parse HEAD`).
2. Wipe `third_party/freeglut/` and repopulate from an **allowlist** (tighter and
   more predictable than a denylist - new upstream dirs won't silently land):
   `CMakeLists.txt`, `cmake/`, `src/` (incl. `src/cocoa/`), `include/`, `COPYING`,
   `AUTHORS`, `ChangeLog`, `README*`, and the root `*.pc.in` / `*Config.cmake.in`
   templates the build references. The script **hard-fails** if any allowlisted
   path is missing in the checkout (so an upstream restructure surfaces loudly
   instead of producing a broken vendor tree). Trade-off acknowledged: an upstream
   layout change may require editing the allowlist - that's intentional. The
   hard-fail only guards *listed* paths, not a needed-but-omitted one; the real
   completeness net is verification #2's clean configure - if cmake errors on a
   missing `CONFIGURE_FILE`/`include()` input, add that path to the allowlist.
3. Write `third_party/freeglut/VENDORED.txt`: upstream URL, requested ref, resolved
   SHA, and "produced by scripts/vendor-freeglut.sh".
4. Print a reminder to update the SHA line in `THIRD_PARTY_LICENSES.md` and to run
   `make freeglut-clean` before the next build (see Part C - the lib rule has no
   source dependency, so a re-vendor needs an explicit clean to take effect).

## Part B - Vendored tree + licensing

- `third_party/freeglut/` - the allowlisted upstream tree (via Part A), with
  freeglut's own `COPYING` and `AUTHORS` riding along inside it.
- `.gitignore` - the existing unanchored `build/` pattern **already** ignores
  `third_party/freeglut/build/`; add an explicit anchored entry only for clarity
  (belt-and-suspenders). Caveat of that same unanchored pattern: any vendored dir
  literally named `build/` would be skipped - eyeball `git status` after vendoring.
- `THIRD_PARTY_LICENSES.md` (new, repo root):
  - **freeglut** - X-Consortium / MIT-style. Reproduce the `COPYING` notice
    **verbatim** and reproduce author/maintainer credits **verbatim from the
    vendored `AUTHORS`/`COPYING`** (do not hand-retype names - avoids attribution
    drift across re-vendors). Link the upstream repo; state the pinned SHA
    (`463cef14`) and that the source lives in `third_party/freeglut/`. This license
    *requires* the notice be included in distributions - this file satisfies that.
  - **miniaudio** - dual Unlicense (public domain) / MIT-0 by David Reid; vendored
    at `include/miniaudio.h`. Legally requires nothing; acknowledged as a courtesy.

## Part C - Makefile: build + statically link vendored freeglut (macOS only)

All changes confined to the `ifeq ($(UNAME_S),Darwin)` branch; Linux and
`USE_GL_STUBS` paths untouched.

1. **Header include (`GL_HEADER_CFLAGS`, ~L41-45):** in the non-stubs branch,
   replace `-I$(HOME)/src/freeglut-fork/include` with `-I$(FREEGLUT_SRC)/include`
   placed **before** `-I/opt/homebrew/include` (hermeticity - a stray
   `brew install freeglut` must not shadow the vendored `<GL/freeglut.h>` while we
   link the vendored `.a`). Keep `/opt/homebrew/include` for `<GL/gl.h>`/`<glu.h>`/
   `<glext.h>` (vendored freeglut's `include/GL/` only ships the freeglut headers -
   `freeglut.h`/`freeglut_std.h`/`freeglut_ext.h`/`glut.h` - *not* `gl.h`/`glu.h`/
   `glext.h`, so putting it first can't shadow the system GL headers; confirm with
   the `ls` in verification #1). `include/gl_includes.h` needs **no change**.

   **`make glut` interaction:** the `glut` target overrides `GL_LDFLAGS` and adds
   `-DUSE_GLUT` but does **not** override `GL_HEADER_CFLAGS`, so the vendored `-I`
   stays on the compile line. That's harmless: `gl_includes.h` gates on
   `#if defined(__APPLE__) && defined(USE_GLUT)` and takes the `<GLUT/glut.h>`
   (Apple framework) path, so `<GL/freeglut.h>` is never `#include`d under
   `make glut` - the vendored header can't conflict.

2. **New variables + build rule** (near the Darwin block):
   ```make
   FREEGLUT_SRC        := third_party/freeglut
   FREEGLUT_BUILD      := $(FREEGLUT_SRC)/build
   FREEGLUT_STATIC_LIB := $(FREEGLUT_BUILD)/lib/libglut.a
   FREEGLUT_VENDOR     ?= 1   # `make glut` passes 0 to skip the vendored build

   $(FREEGLUT_STATIC_LIB):
   cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) \
     -DFREEGLUT_COCOA=ON -DFREEGLUT_BUILD_STATIC_LIBS=ON \
     -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build $(FREEGLUT_BUILD) --target freeglut_static
   ```
   Built under `third_party/freeglut/build/` (survives `make clean`, which only
   removes the top-level `./build`). **This rule has no dependency on the freeglut
   sources** - after a re-vendor, `make freeglut-clean` is required to rebuild.

3. **`make freeglut-clean`** target: `rm -rf $(FREEGLUT_BUILD)`. Add it to `.PHONY`.

4. **Define a dependency var** so Linux/stubs stay empty:
   ```make
   ifeq ($(UNAME_S),Darwin)
   ifneq ($(USE_GL_STUBS),1)
   ifeq ($(FREEGLUT_VENDOR),1)
   FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
   endif
   endif
   endif
   ```

5. **Link flags (`GL_LDFLAGS`, Darwin, ~L121-127):** drop the shared-lib `-L`,
   the `-Wl,-rpath,...`, and `-lglut`; link the archive **by path** via
   `$(FREEGLUT_LIB)` and add `-framework CoreVideo`:
   ```make
   GL_LDFLAGS = \
       -L/opt/homebrew/lib \
       $(FREEGLUT_LIB) -lm -lpthread \
       -framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo \
       -framework CoreAudio -framework CoreFoundation -framework AudioToolbox
   ```
   (Verified framework set; the static archive carries no framework deps so the
   consumer must list them.)

6. **Order-only prereq on the FULL set of GL-linking targets** (a built archive,
   unlike the old prebuilt fork lib, must exist before any link recipe that names
   its path runs - otherwise those links fail, serially, with nothing building it).
   A single guarded multi-target line covers all of them:
   ```make
   ifeq ($(FREEGLUT_VENDOR),1)
   ifeq ($(UNAME_S),Darwin)
   ifneq ($(USE_GL_STUBS),1)
   $(SAMPLE_BIN) $(SCENE_DEMO_BIN) $(REPL_DEMO_BIN) $(EDITOR_DEMO_BIN) \
   $(MEMPROF_DEMO_BIN) $(CPUPROF_DEMO_BIN) $(VARIABLE_PANEL_DEMO_BIN) \
   $(COLOR_PICKER_DEMO_BIN) \
   $(addprefix $(BINDIR)/,$(TEST_BINS) $(BENCH_BINS) $(GL_TEST_BINS)): | $(FREEGLUT_STATIC_LIB)
   endif
   endif
   endif
   ```
   **Placement is load-bearing.** This is a *static* target list (no
   `.SECONDEXPANSION`), so make expands every target name when it parses the line.
   `$(SAMPLE_BIN)` + the 7 demo bins are defined ~L760-767 and `$(GL_TEST_BINS)`
   ~L973 - all *later* than `TEST_BINS`/`BENCH_BINS` (~L755). So the block must go
   **after `GL_TEST_BINS` is defined - i.e. right after the `gl-tests` rules
   (~L973-992)**, not at ~L755. (At ~L755 those vars expand to empty and the prereq
   silently attaches to nothing - `make gl-repl`, every demo, and gl-tests would
   then link without the prereq, the exact race step 6 exists to prevent.) make
   accumulates prerequisites across separate lines and the recipes live elsewhere,
   so the late placement is fine.
   This covers: the 8 sample/demo rules (~L843,855,868,892,905,918,931,944),
   `test_audio` (it's in `TEST_BINS`, and step 6 uses `TEST_BINS` not the
   test_audio-excluding `CORE_TEST_BINS`) + all core tests/benches that link
   `$(GL_LDFLAGS)` via the `core_test_binary`/`bench_binary` templates, and the 2
   `gl-tests` rules (~L975,983). Listing a few extra non-GL tests as order-only
   prereqs is harmless (they still don't link it).

7. **`make glut` (~L1512):** keep its `GL_LDFLAGS=$(GLUT_GL_LDFLAGS)` override
   **and** pass **`FREEGLUT_VENDOR=0`** so the prereq line in step 6 is skipped -
   no pointless cmake build during the Apple-framework fallback. (Even if a stale
   `libglut.a` exists, it's an orphan never referenced by the override link line -
   harmless. `make glut`'s internal `$(MAKE) clean` won't wipe the vendored build.)

8. **`-DFREEGLUT_STATIC`** added to `COMMON_CFLAGS` (decided above).

New build prerequisite on macOS: **cmake**. Note it gates not just `make gl-repl`
but the macOS **`make test`** (non-stub) build too, since core tests + `test_audio`
link `$(GL_LDFLAGS)`.

## Part D - Source/doc updates (drop stale fork references)

- **`scripts/check-c99.sh`** (~L68, L72): replace `$HOME/src/freeglut-fork/include`
  with `$ROOT/third_party/freeglut/include` in both the `-isystem` list and the
  `have_real_gl` probe loop, so the macOS C99 ratchet still resolves `<GL/freeglut.h>`.
  Also fix the cosmetic `homebrew/freeglut-fork` comment at ~L56 in the same edit.
- **Comment fixes** (the fork-only rationale is now partly false - mainline freeglut's
  Cocoa backend *does* set `GLUT_ACTIVE_SUPER`; the `#ifndef…0` fallback still
  correctly covers the `make glut` Apple-framework path and X11/Linux, which don't):
  `src/editor/input.c` (~L69-70, L159), `src/editor/input.h` (~L85),
  `tools/editor_demo/input.c` (~L456), `tests/test_glr_actions.c` (~L1216). Keep the
  `#ifndef GLUT_ACTIVE_SUPER #define …0` fallback as-is.
- **`CLAUDE.md`**: update **Build** section + the gracemont note - vendored freeglut
  (`third_party/freeglut/`, built static + Cocoa), the `scripts/vendor-freeglut.sh`
  re-vendoring flow; drop `~/src/freeglut-fork` references.
- **`include/README.md`**: note freeglut is now vendored under `third_party/freeglut/`.
- **`README.md`**: one line pointing at `THIRD_PARTY_LICENSES.md`.

## Files to create / modify

| Path | Change |
|------|--------|
| `scripts/vendor-freeglut.sh` | **new** - re-pin-to-ref vendoring script (allowlist) |
| `third_party/freeglut/**` | **new** - allowlisted upstream master-tip tree (via script) |
| `third_party/freeglut/VENDORED.txt` | **new** - upstream URL / ref / resolved SHA |
| `THIRD_PARTY_LICENSES.md` | **new** - freeglut + miniaudio acknowledgement |
| `Makefile` | header include swap (vendored first); freeglut static build rule + vars; `FREEGLUT_VENDOR`/`FREEGLUT_LIB`; Darwin `GL_LDFLAGS` rewrite + `-framework CoreVideo`; full-set order-only prereq; `freeglut-clean` (+ `.PHONY`); `-DFREEGLUT_STATIC` in `COMMON_CFLAGS`; `make glut` passes `FREEGLUT_VENDOR=0`; **`check-trailing-whitespace` recipe excludes `third_party/`** |
| `scripts/check-c99.sh` | vendored include path replaces fork path (2 sites) |
| `src/editor/input.{c,h}`, `tools/editor_demo/input.c`, `tests/test_glr_actions.c` | comment updates (fork → "freeglut Cocoa backend / fork") |
| `.gitignore` | explicit `third_party/freeglut/build/` (clarity; already covered) |
| `CLAUDE.md`, `include/README.md`, `README.md` | doc updates |

### `check-trailing-whitespace` exclusion (required - else the vendor commit can't push)

The guard runs `git --no-pager diff --check "$merge_base"` over everything since
`origin/main`; the vendored upstream tree carries trailing whitespace and would
fail it (it's wired into `test-stubs` and the pre-push hook). Add a pathspec
exclusion to both the merge-base branch and the cached/working fallback:
`git --no-pager diff --check "$merge_base" -- . ':(exclude)third_party/'`. Other
guards are safe: `SRCS`/`HDRS` are explicit lists (not wildcards) and no guard
recipe `find`s from repo root - but still run the full
`make check-state-ownership && make test-stubs` after vendoring to be sure no
`scripts/*.sh` guard greps the new tree.

## Verification

1. **Vendor:** `scripts/vendor-freeglut.sh` (no arg → master tip) populates
   `third_party/freeglut/` + `VENDORED.txt` with the resolved SHA;
   `third_party/freeglut/src/cocoa/` present. `ls third_party/freeglut/include/GL/`
   shows only freeglut headers (no `gl.h`/`glu.h`/`glext.h`) - confirms the
   vendored `-I` can't shadow system GL headers.
2. **Build static:** `make gl-repl` → cmake configures/builds `libglut.a`, binary
   links it: `ls third_party/freeglut/build/lib/libglut.a`.
3. **Static-link proof:** `otool -L gl-repl` shows **no** `libglut*.dylib` - only
   system frameworks (Cocoa, OpenGL, IOKit, CoreVideo, CoreAudio, …). Cross-check
   the cmake-configured framework list matches `GL_LDFLAGS`.
4. **Runtime + Cmd parity:** `./gl-repl` opens a **native Cocoa** window (no XQuartz),
   renders a typed triangle, and **Cmd+S / Cmd+Z / Cmd+C / Cmd+/** behave exactly as
   on the fork (the verified `GLUT_ACTIVE_SUPER` path - the single most regression-prone item).
5. **macOS C99 ratchet:** `make check-c99` passes (vendored headers resolve `<GL/freeglut.h>`).
6. **Other GL binaries link:** `make scene_demo repl_demo editor_demo memprof_demo
   cpuprof_demo variable_panel_demo color_picker_demo` and `make test` (non-stub;
   note this now invokes cmake on macOS - cmake is a dev prereq for it too).
   `make gl-tests` if a display is available.
7. **Whitespace guard / commit:** `make check-trailing-whitespace` passes with the
   `third_party/` exclusion; `make check-state-ownership && make test-stubs` green.
8. **Fallback intact:** `make glut` builds against the Apple GLUT framework and does
   **not** trigger the cmake freeglut build (`FREEGLUT_VENDOR=0`).
9. **Headless/stubs:** `make test-stubs` unaffected (stub headers/libs, no freeglut, empty `FREEGLUT_LIB`).
10. **Linux (gracemont):** `ssh gracemont '… git pull --ff-only && make check-c99 && make test-stubs'`
    - Linux path unchanged, still system `-lglut -lGL -lGLU`.
11. **Re-vendor:** `scripts/vendor-freeglut.sh <other-ref>` updates `VENDORED.txt`;
    `make freeglut-clean && make gl-repl` rebuilds against the new source.
