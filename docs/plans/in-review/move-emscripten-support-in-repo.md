# Move the Emscripten web build in-repo

## Context

The gl-repl WebAssembly build currently lives out-of-repo in
`../OpenGL-Vibe/emscripten/`: `build.sh` (orchestration + dep checks +
serve), `gl4es_bootstrap.c` (extension shim + browser input guards TU),
`shell.html` (themed emscripten shell), a freeglut Emscripten-backend
patch (Drew's own authorship, 15 new `src/emscripten/` files + CMake
hooks), and local `GLU/` + `freeglut/` checkouts, plus `~/src/gl4es`.

The repo side is already prepared: `src/app/glr_web_io.c` (scene
import/export bridge), `examples/catalog-emscripten.ini`, `__EMSCRIPTEN__`
gates in `menu_bar.c`/`glr_audio.c`, `#ifndef`-guarded
`CFG_DEFAULT_VERTEX_OUTLINES/POINTS` in `src/app/glr_defaults.h`, and the
Makefile override seams build.sh drives (`GL_HEADER_CFLAGS`, `GL_LDFLAGS`,
`SAMPLE_BIN`, `OBJDIR`, `EXAMPLES_CATALOG`).

Goal: make the web build a first-class in-repo target so it survives
independent of the OpenGL-Vibe tree and its flag knowledge lives in the
build system.

**Decisions (confirmed with Drew):**
- gl4es + GLU: **fetch-and-build script** into gitignored dirs, pinned SHAs
- freeglut wasm: **stack the emscripten patch onto `~/src/freeglut-fork` and re-vendor** — one vendored tree serves Cocoa / OSMesa / wasm
- Project-owned web files live in **`packaging/web/`**
- Entry point: **Makefile-native `WEB=1` block + thin wrapper script** (mirrors the `FREEGLUT_OSMESA=1` pattern)

## Steps

### 1. Re-vendor freeglut with the emscripten backend (one-time, fork-side)

- In `~/src/freeglut-fork`: create branch `emscripten-backend` stacked on
  `capture-windowed-backends`, apply
  `../OpenGL-Vibe/emscripten/0001-feat-add-Emscripten-WebAssembly-platform-support.patch`
  via `git am` (preserves authorship), commit.
- Re-vendor: `FREEGLUT_REPO=~/src/freeglut-fork scripts/vendor-freeglut.sh emscripten-backend`.
  The allowlist in `scripts/vendor-freeglut.sh` copies `src/` and
  `include/` recursively, so `src/emscripten/` and
  `include/GL/emscripten_hide_glut.h` are picked up with **no allowlist
  change**. (Push the branch to the fork's remote for a portable pin, per
  the note in VENDORED.txt.)
- The patch is additive (CMake gates on the EMSCRIPTEN toolchain), so the
  Cocoa and OSMesa backends are untouched — verify with `make gl-repl` and
  `make gl-repl FREEGLUT_OSMESA=1` after re-vendoring.
- Update the pinned SHA note in `docs/THIRD_PARTY_LICENSES.md`.
- The `0001-...patch` file then retires (lives in fork history); do not
  copy it into this repo.

### 2. `packaging/web/` — project-owned web files

Move in (adjusting only path comments):
- `packaging/web/shell.html`
- `packaging/web/gl4es_bootstrap.c` — compiled at **link time** (listed in
  `GL_LDFLAGS`, as today), so it never joins `$(SRCS)`; the C99 ratchet
  (`check-c99`) is untouched even though this TU needs gnu99/EM_ASM.
- `packaging/web/README.md` — deps, build flow, browser-input/shim notes
  (distill the memory-file knowledge: JS GLUT windowing vs freeglut
  solids, wheel/backspace/Ctrl fixes, Display-P3 tagging).

### 3. `scripts/web-deps.sh` — fetch & build gl4es + GLU

Modeled on `scripts/vendor-freeglut.sh`'s pin discipline but building into
**gitignored** `third_party/web/`:

- Requires `emcc` on PATH (caller sources emsdk; see step 5) — hard error
  with install instructions otherwise (reuse build.sh's help text).
- `third_party/web/gl4es`: clone `https://github.com/ptitSeb/gl4es`,
  checkout pinned SHA (record the SHA of the current working
  `~/src/gl4es` HEAD), build per build.sh:
  `emcmake cmake .. -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON && emmake make`
  → `lib/libGL.a`.
- `third_party/web/GLU`: clone `https://github.com/ptitSeb/GLU`, pinned
  SHA, `autoreconf -fi; emconfigure ./configure --disable-shared
  --enable-static; emmake make` with the gl4es `-include GL/gl.h
  -DUSE_MGL_NAMESPACE` CFLAGS (copy from build.sh `build_glu`).
- Record resolved SHAs in `third_party/web/PINNED.txt`.
- Env overrides `GL4ES_DIR` / `GLU_DIR` so an existing `~/src/gl4es`
  checkout can be reused without a re-clone.
- Idempotent: skip clone/build when the static libs already exist.
- Add `third_party/web/` to `.gitignore`.
- Acknowledge gl4es + GLU licenses in `docs/THIRD_PARTY_LICENSES.md`.

### 4. Makefile — `WEB=1` platform block + `web` targets

Follow the `FREEGLUT_OSMESA` / `USE_GL_STUBS` precedents:

- **Config knobs** (top, near `FREEGLUT_OSMESA`):
  `WEB ?= 0`; under `WEB=1`: `CC = emcc`,
  `GL4ES_DIR ?= third_party/web/gl4es`, `GLU_DIR ?= third_party/web/GLU`,
  `FREEGLUT_BUILD := $(FREEGLUT_SRC)/build-wasm`,
  `FREEGLUT_STATIC_LIB := $(FREEGLUT_BUILD)/lib/libglut.a`,
  `EXAMPLES_CATALOG = examples/catalog-emscripten.ini`,
  OBJDIR suffix `-web` (alongside the existing `-gl-stubs` / `-osmesa`
  suffixes → objects in `build/release-web/`),
  `SAMPLE_BIN = $(BINDIR)/index.html`.
- **Flags override block** (after the Darwin/Linux blocks, mirroring the
  `USE_GL_STUBS` override at ~line 296):
  - `GL_HEADER_CFLAGS`: `-include $(GL4ES_DIR)/include/GL/gl.h
    -I$(GL4ES_DIR)/include -I$(GLU_DIR)/include -I$(FREEGLUT_SRC)/include
    -DUSE_MGL_NAMESPACE -DCFG_DEFAULT_VERTEX_OUTLINES=0
    -DCFG_DEFAULT_VERTEX_POINTS=0 -std=gnu99` — the trailing `-std=gnu99`
    lands after `COMMON_CFLAGS`' `-std=c99` on the compile line (miniaudio's
    EM_ASM needs gnu mode); keep build.sh's comment explaining this.
  - `GL_LDFLAGS` / `GLUT_GL_LDFLAGS`: `packaging/web/gl4es_bootstrap.c
    $(GL4ES_DIR)/lib/libGL.a $(GLU_DIR)/.libs/libGLU.a
    $(FREEGLUT_STATIC_LIB) --shell-file packaging/web/shell.html
    -sUSE_WEBGL2=1 -sFULL_ES2=1 -sINITIAL_MEMORY=805306368
    -sSTACK_SIZE=8388608 -sGL_MAX_TEMP_BUFFER_SIZE=67108864
    -sEXPORTED_FUNCTIONS=_main,_glr_web_new_scene,_glr_web_load_scene_text,_glr_web_export_scene
    -sEXPORTED_RUNTIME_METHODS=ccall,FS $(WEB_PRELOAD)` — cross-reference
    `src/app/glr_web_io.c` next to the exports list.
  - `WEB_PRELOAD`: `$(wildcard ...)`-based — `assets/favorite` dir →
    `--preload-file assets/favorite@/assets`, else `assets/sample.mp3` →
    single-file preload, else empty (the `make app` small-download policy;
    keeps file_packager from following the multi-hundred-MB playlist
    symlink).
- **Vendored freeglut wasm build**: parameterize the existing
  `$(FREEGLUT_STATIC_LIB)` rule with a launcher variable
  (`FREEGLUT_CMAKE_LAUNCHER`, empty natively, `emcmake` under `WEB=1`) and
  wasm backend flags (`FREEGLUT_CMAKE_BACKEND` under WEB=1:
  `-DFREEGLUT_REPLACE_GLUT=ON -DCMAKE_C_FLAGS="-include
  $(abspath $(GL4ES_DIR))/include/GL/gl.h -I$(abspath $(GL4ES_DIR))/include"`;
  no PKG_CONFIG_PATH). `build-wasm/` coexists with `build/` and
  `build-osmesa/`, same as OSMesa today; `make freeglut-clean` covers it
  via `$(FREEGLUT_BUILD)`.
- **Link-rule prerequisites** under WEB=1: add `packaging/web/shell.html`
  and `packaging/web/gl4es_bootstrap.c` to `$(SAMPLE_BIN)` deps so editing
  either relinks (emcc emits `index.{html,js,wasm,data}` next to
  `SAMPLE_BIN`).
- **Targets**:
  - `web:` — fail fast with a clear hint if `emcc` is missing
    ("run scripts/build-web.sh or source emsdk_env.sh"), run
    `scripts/web-deps.sh`, then build `$(SAMPLE_BIN)` with `WEB=1`.
    Implement as a delegating recipe (`$(MAKE) WEB=1 $(SAMPLE_BIN)`-style,
    like `make glut` does with `FREEGLUT_VENDOR=0`) so plain `make web`
    works without remembering the flag.
  - `web-serve:` — `python3 -m http.server` rooted at the web BINDIR,
    printing the URL.
- `WEB=1` must not disturb any native path: every change is inside
  `ifeq ($(WEB),1)` guards or new variables that default to today's
  values. Tests/demos are not wired for WEB=1 (unsupported, as today).

### 5. `scripts/build-web.sh` — thin wrapper

For the "cold start" path (no emsdk in the shell):
- `EMSDK ?= ~/src/emsdk`; source `emsdk_env.sh` (error with build.sh's
  install instructions if absent).
- Run `scripts/web-deps.sh`, then `emmake make web`.
- `--serve` flag → `make web-serve` after the build (serve is opt-in now;
  `make web` alone never blocks on a server).

### 6. Docs + cleanup

- CLAUDE.md: new "Web build (Emscripten)" section under Build — one
  paragraph + the two commands (`scripts/build-web.sh`, `make web` /
  `make web-serve`), and note `packaging/web/` ownership.
- `docs/MODULES.md` / README build sections: brief mention.
- Out-of-repo: leave `OpenGL-Vibe/emscripten/build.sh` alone for the
  legacy one-file samples, but its `^gl-repl:` branch becomes obsolete —
  note this in the commit message. (Optionally follow up there separately.)
- Update the `emscripten-gl4es-build` memory file to point at the new
  in-repo flow after implementation.

## Verification

1. **Fork/re-vendor sanity**: after step 1, `make gl-repl` and
   `make gl-repl FREEGLUT_OSMESA=1` still build; `make test-stubs` green.
2. **Web build**: `scripts/build-web.sh` from a shell without emsdk →
   produces `build/release-web/index.html` (+ .js/.wasm/.data);
   `make web` works in an emsdk-activated shell.
3. **Browser smoke**: `make web-serve`, load the page — scene renders,
   wheel zoom in the 3D scene, backspace + Ctrl shortcuts in the editor,
   File menu hidden, examples come from `catalog-emscripten.ini`, audio
   starts. (Headless: the CDP `shot.js` pattern from the 2026-07-07
   session if needed.)
4. **Native regression gate**: `make check-c99`, `make test`,
   `make check-state-ownership` untouched and green locally; sync +
   `make check-c99 && make test-stubs` on gracemont (Makefile changes are
   portability-sensitive).
