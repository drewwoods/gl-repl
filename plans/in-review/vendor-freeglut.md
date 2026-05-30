# Vendor freeglut as a static library + license acknowledgement

## Context

Today the REPL links against a **private freeglut fork** living outside the
repo at `~/src/freeglut-fork`, as a **shared** library, via hardcoded Makefile
paths:

```make
# Makefile lines 41-44  (GL_HEADER_CFLAGS)
-I/usr/include -I/opt/homebrew/include -I$(HOME)/src/freeglut-fork/include
# Makefile lines 121-127 (GL_LDFLAGS, the default `gl-repl` path)
-L/opt/homebrew/lib -L$(HOME)/src/freeglut-fork/build/lib \
-Wl,-rpath,$(HOME)/src/freeglut-fork/build/lib -lglut ...
```

That means the build is **not reproducible** off this machine, depends on an
out-of-tree checkout, and ships a `libglut.dylib` rpath that won't resolve
elsewhere. We want freeglut **vendored into the repo and statically linked**,
using the native macOS **Cocoa** backend (`-DFREEGLUT_COCOA=ON`), plus proper
third-party license acknowledgement (none exists in the repo today).

**Key fact established during planning:** the Cocoa backend was upstreamed.
Current `freeglut/freeglut` **v3.8.0** (SHA `3db1649ce1f5e42f1338b51e3fa14849be547d5d`)
ships `src/cocoa/` (15 `.m`/`.c`/`.h` files), `OPTION(FREEGLUT_COCOA ... OFF)`,
and `OPTION(FREEGLUT_BUILD_STATIC_LIBS ... ON)`. So we vendor **upstream**, not
the fork — no fork patches are needed for Cocoa.

## Decisions (confirmed with user)

- **Source:** upstream `freeglut/freeglut`, **not** the drewwoods fork. Pin **v3.8.0**.
- **Vendor method:** copy a trimmed subset into `third_party/freeglut/`, plus a
  **script** (`scripts/vendor-freeglut.sh <sha-or-tag>`) to re-vendor any version.
- **Linux scope:** macOS-only vendoring. Linux (gracemont CI) keeps the existing
  system `-lglut -lGL -lGLU` path untouched.
- **Licensing:** add `THIRD_PARTY_LICENSES.md` at repo root (freeglut + miniaudio);
  keep freeglut's `COPYING`/`AUTHORS` inside `third_party/freeglut/`. No new
  project-wide LICENSE.

## Part A — Vendor script: `scripts/vendor-freeglut.sh`

New executable script. Usage: `scripts/vendor-freeglut.sh [<sha-or-tag>]`
(default `v3.8.0`). Steps:

1. Clone `https://github.com/freeglut/freeglut.git` into a temp dir, `git checkout <ref>`,
   resolve the full SHA (`git rev-parse HEAD`).
2. Wipe and repopulate `third_party/freeglut/` with a **denylist** copy — copy
   everything **except** the bulky/unneeded dirs: `.git .github man progs doc
   altbuild android .vscode .cache`. This keeps `CMakeLists.txt`, `cmake/`,
   `src/` (incl. `src/cocoa/`), `include/`, `COPYING`, `AUTHORS`, `ChangeLog`,
   the `README.*`, and the `*.pc.in` / `*Config.cmake.in` templates the build
   references. (Demos under `progs/` are dropped; the Makefile configures with
   `-DFREEGLUT_BUILD_DEMOS=OFF` so they're never needed.)
3. Write `third_party/freeglut/VENDORED.txt` recording the upstream URL, the
   requested ref, the resolved SHA, and a note that it was produced by this script.
4. Print a reminder to update the SHA line in `THIRD_PARTY_LICENSES.md`.

Match the existing `scripts/` style (`scripts/keymap.sh`); `set -euo pipefail`,
clean up the temp dir on exit.

## Part B — Vendored tree + licensing

- `third_party/freeglut/` — the trimmed upstream tree (produced by Part A), with
  freeglut's own `COPYING` and `AUTHORS` riding along inside it.
- `.gitignore` — add `third_party/freeglut/build/` (the CMake output dir from Part C).
- `THIRD_PARTY_LICENSES.md` (new, repo root):
  - **freeglut** — X-Consortium / MIT-style. Reproduce the `COPYING` notice
    verbatim, credit Pawel W. Olszta + maintainers (John F. Fay, Diederick C.
    Niehorster, John Tsiombikas), link the upstream repo, state the vendored
    SHA (v3.8.0 / `3db1649c`) and that the source lives in `third_party/freeglut/`.
    This license *requires* the notice be included in distributions — this file
    satisfies that.
  - **miniaudio** — dual Unlicense (public domain) / MIT-0 by David Reid; vendored
    at `include/miniaudio.h`. Legally requires nothing, acknowledged as a courtesy.

## Part C — Makefile: build + statically link vendored freeglut (macOS only)

All changes confined to the `ifeq ($(UNAME_S),Darwin)` branch; Linux and the
`USE_GL_STUBS` paths are untouched.

1. **Header include (lines 41-45):** in the non-stubs `GL_HEADER_CFLAGS`, replace
   `-I$(HOME)/src/freeglut-fork/include` with `-I$(FREEGLUT_SRC)/include`. Keep
   `-I/opt/homebrew/include` (still the source of `<GL/gl.h>`/`<GL/glu.h>`/`<GL/glext.h>`;
   vendored freeglut only supplies `<GL/freeglut.h>`). `include/gl_includes.h`
   needs **no change** — its `#include <GL/freeglut.h>` now resolves to the vendored copy.

2. **New variables + build rule** (near the Darwin block):
   ```make
   FREEGLUT_SRC        := third_party/freeglut
   FREEGLUT_BUILD      := $(FREEGLUT_SRC)/build
   FREEGLUT_STATIC_LIB := $(FREEGLUT_BUILD)/lib/libglut.a

   $(FREEGLUT_STATIC_LIB):
   	cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) \
   	  -DFREEGLUT_COCOA=ON -DFREEGLUT_BUILD_STATIC_LIBS=ON \
   	  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF \
   	  -DCMAKE_BUILD_TYPE=Release
   	cmake --build $(FREEGLUT_BUILD) --target freeglut_static
   ```
   (`FREEGLUT_REPLACE_GLUT` defaults ON on non-Windows, so the static target's
   output name is `glut` → `libglut.a`. Building under `third_party/freeglut/build/`
   — not `build/` — means `make clean` won't wipe it; add a `freeglut-clean`
   target that `rm -rf $(FREEGLUT_BUILD)` for explicit rebuilds.)

3. **Link flags (lines 121-127, `GL_LDFLAGS`, Darwin):** drop the shared-lib
   `-L .../freeglut-fork/build/lib`, the `-Wl,-rpath,...`, and `-lglut`. Link the
   archive **by path** and add the framework the Cocoa backend pulls in:
   ```make
   GL_LDFLAGS = \
       -L/opt/homebrew/lib \
       $(FREEGLUT_STATIC_LIB) -lm -lpthread \
       -framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo \
       -framework CoreAudio -framework CoreFoundation -framework AudioToolbox
   ```
   `CoreVideo` is new (freeglut's Cocoa target links it; static archives don't
   carry their own framework deps, so the consumer must). Optionally add
   `-DFREEGLUT_STATIC` to `COMMON_CFLAGS` (no-op on macOS/Linux, correct for portability).

4. **Make the lib an order-only prereq of the GL link rules** so it builds once
   and is reused. The rules that link `$(GL_LDFLAGS)` are `$(SAMPLE_BIN)` (line 842),
   `$(SCENE_DEMO_BIN)` (854), and the other demo binaries. Gate the dependency on
   Darwin (e.g. `ifeq ($(UNAME_S),Darwin)` … add `| $(FREEGLUT_STATIC_LIB)` to those
   targets, or a single `$(SAMPLE_BIN) $(SCENE_DEMO_BIN) …: | $(FREEGLUT_STATIC_LIB)`
   line under the Darwin guard).

5. **`make glut` (line 1512) unchanged** — it overrides `GL_LDFLAGS` with the Apple
   GLUT framework path and stays the XQuartz-free fallback.

New build prerequisite on macOS: **cmake** (already used for the fork build).

## Part D — Docs

- `CLAUDE.md`: update the **Build** section and the gracemont note to describe the
  vendored freeglut (`third_party/freeglut/`, built static + Cocoa) and the
  `scripts/vendor-freeglut.sh` re-vendoring flow; drop the `~/src/freeglut-fork`
  references.
- `include/README.md`: note freeglut is now vendored under `third_party/freeglut/`
  (alongside the miniaudio mention).
- `README.md`: one line pointing at `THIRD_PARTY_LICENSES.md`.

## Files to create / modify

| Path | Change |
|------|--------|
| `scripts/vendor-freeglut.sh` | **new** — pin-a-SHA vendoring script |
| `third_party/freeglut/**` | **new** — trimmed upstream v3.8.0 tree (via script) |
| `third_party/freeglut/VENDORED.txt` | **new** — recorded upstream URL/ref/SHA |
| `THIRD_PARTY_LICENSES.md` | **new** — freeglut + miniaudio acknowledgement |
| `Makefile` | header include swap; freeglut static build rule + vars; Darwin `GL_LDFLAGS` rewrite; order-only prereq; `freeglut-clean` |
| `.gitignore` | ignore `third_party/freeglut/build/` |
| `CLAUDE.md`, `include/README.md`, `README.md` | doc updates |

## Verification

1. **Vendor:** `scripts/vendor-freeglut.sh v3.8.0` → populates `third_party/freeglut/`
   + `VENDORED.txt` with SHA `3db1649c…`; `third_party/freeglut/src/cocoa/` present.
2. **Build static:** `make gl-repl` → cmake configures/builds `libglut.a`, then the
   binary links it. Confirm the archive exists: `ls third_party/freeglut/build/lib/libglut.a`.
3. **Static link proof:** `otool -L gl-repl` shows **no** `libglut*.dylib` entry —
   only system frameworks (Cocoa, OpenGL, IOKit, CoreVideo, CoreAudio, …).
4. **Runtime:** `./gl-repl` opens a **native Cocoa** window (no XQuartz) and renders;
   type a triangle, confirm geometry + input work.
5. **Fallback intact:** `make glut` still builds against the Apple GLUT framework.
6. **Headless/tests:** `make test-stubs` unaffected (stub headers/libs, no freeglut).
7. **Linux (gracemont):** `ssh gracemont '… git pull --ff-only && make check-c99 && make test-stubs'`
   — Linux path unchanged, still system `-lglut -lGL -lGLU`.
8. **Re-vendor smoke test:** re-running the script with a different ref (e.g. a SHA)
   updates `VENDORED.txt` and a subsequent `make freeglut-clean && make gl-repl` rebuilds.
