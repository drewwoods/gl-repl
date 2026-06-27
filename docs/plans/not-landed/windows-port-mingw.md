# Windows port (MinGW-w64), with legacy-Windows support

> Line numbers below are **approximate** (current Makefile/source); match by
> content/symbol, not by line — earlier edits shift them.

## ⚠️ NOT LANDED — awaiting Windows vetting

The implementation lives on the **`windows-support`** branch and is
substantially complete, but it has **not been tested on a real Windows
machine**. The branch is not merged to `main` until a MinGW-w64 build and
smoke-test run has been verified on actual Windows hardware. Everything
below describes the design as implemented on that branch.

## Context

The REPL builds today for macOS (vendored static freeglut, Cocoa backend) and
Linux (system `-lglut -lGL -lGLU`). There is **no Windows path**: `uname -s`
(Makefile ~L30) detects only `Darwin`, and every other host falls through to the
Linux branch and its X11/`-ldl` link line (Makefile ~L155-164), which cannot
link on Windows.

The codebase is, however, already well-positioned for a port. It is straight
C99 (the `make check-c99` ratchet enforces this project-wide), miniaudio
(`include/miniaudio.h`) already ships native Windows backends, and the
host-specific modules (`src/support/memprof.c`, `src/support/cpuprof.c`,
`src/app/glr_audio.c`) already branch on `__APPLE__` / `__linux__` — so Windows
is largely a matter of **adding the third arm** plus a Makefile branch, not a
rewrite. The blockers are concentrated, not pervasive.

## Decision: MinGW-w64 (not MSVC) — and why

**We target MinGW-w64.** Rationale, briefly:

- **Toolchain identity is preserved.** GCC + `-std=c99` + `-Wall` + the
  ASan/UBSan debug flags + `make` + `scripts/check/check-c99.sh` all work unchanged.
  MSVC has no `-std=c99` (only `/std:c11`+), and every warning / sanitizer /
  coverage flag would need translation — a large, ongoing tax for no benefit
  here.
- **pthreads come for free.** `src/app/glr_audio.c` uses pthreads directly
  (`pthread_create/join`, recursive mutex, condvar). MinGW's `winpthreads`
  compiles this **as-is**; MSVC would force a `CreateThread` /
  `CRITICAL_SECTION` / `CONDITION_VARIABLE` abstraction layer. This single fact
  is the strongest argument for MinGW.
- **Legacy is the goal.** Modern MSVC will not target old Windows, and old MSVC
  will not compile this code. MinGW-w64 can pin an older `_WIN32_WINNT` and
  reach XP/Vista-era Windows while keeping the same C99 source — consistent with
  the project's existing old-GCC ethos.

MSVC is explicitly **out of scope**. If a Visual Studio-native artifact is ever
needed, that is a separate, larger effort.

## Scope / non-goals

- In scope: build + run `gl-repl.exe` under MinGW-w64; `make test` green;
  legacy-Windows reach via toolchain pinning.
- Out of scope: MSVC, a CMake/Meson rewrite of the project build (freeglut keeps
  its own CMake), an installer/packaging story, ARM64 Windows.
- macOS and Linux paths must be **untouched** in behavior — every change is
  additive behind `_WIN32` / a new Makefile branch.

---

## Phase 1 — compile & run (`gl-repl.exe` under MinGW-w64)

Goal: the shipped binary builds and runs. Tests deferred to Phase 2.

### 1A. Makefile — add a Windows branch
- Detect Windows: `uname -s` under MSYS2 reports `MINGW64_NT-*` / `MSYS_NT-*`;
  branch on a `findstring MINGW`/`MSYS` (or `$(OS)` == `Windows_NT`) alongside
  the existing `ifeq ($(UNAME_S),Darwin)` / Linux `else` (Makefile ~L127-164).
- Link set: replace the Linux `-lglut -lGL -lGLU -lm -lpthread -ldl` with
  `-lfreeglut -lopengl32 -lglu32 -lgdi32 -luser32 -lwinmm -lm`
  (`-lpthread` via winpthreads; `-lwinmm` for miniaudio's WinMM/timer path).
  No `-ldl` (miniaudio links its Windows backends directly, no dlopen).
- `ln -sfn` (Makefile ~L894 and ~8 sibling sites) creates the convenience
  symlinks (`gl-repl` → `build/release/gl-repl`). cmd.exe can't symlink; under
  MSYS2 bash `ln -s` works, but to be safe make the recipe fall back to `cp`
  on the Windows branch (or guard the symlink behind non-`_WIN32`). The binary
  itself should carry `.exe`.
- Keep `-std=c99` and all warning/sanitizer flags as-is (MinGW GCC accepts
  them). `check-c99.sh` should pass unchanged once GL headers resolve.

### 1B. `include/gl_includes.h` — Windows GL header order (hard stop)
`<GL/gl.h>` on Windows requires `<windows.h>` first (for `WINGDIAPI` /
`APIENTRY` calling-convention macros) or the GL headers error out wholesale.
Add ahead of the existing non-Apple `#include <GL/gl.h>` block (~L16-22):

```c
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif
```

Small change, but a non-negotiable prerequisite for any TU that pulls the GL
headers.

### 1C. Time APIs — add a `_WIN32` arm to existing ladders
Four sites, each already split on `__APPLE__`/`__linux__`:
- `gl_repl.c:~33` `gettimeofday` (startup init trace) → `QueryPerformanceCounter`
  (or `GetSystemTimeAsFileTime`).
- `src/app/glr_audio.c:~230` `clock_gettime(CLOCK_MONOTONIC)` (worker hitch
  detector) → `QueryPerformanceCounter`.
- `src/support/cpuprof.c:~56` `clock_gettime(CLOCK_MONOTONIC)` → same.
- `bench/bench_repl.c:~74` `now_seconds()` → same. (bench is not the shipped
  binary but is in the C99 gate, so it must still build.)

A single shared `qpc_now_ns()` helper would avoid four copies, but keep it
inside each module's existing `#if` ladder to preserve the current
host-agnostic boundaries (don't introduce a new cross-module time header
without a separate decision).

### 1D. Filesystem / directory shims
- **`opendir`/`readdir`/`closedir`** — `gl_repl.c:~91-104` (audio playlist scan)
  and `src/repl/scenes.c:~816-853` (workspace load). MinGW **provides
  `<dirent.h>`**, so these compile as-is under MinGW; no shim strictly required
  for Phase 1. (Only an MSVC port would need a `FindFirstFile` wrapper — out of
  scope.)
- **`mkdir(dir, 0755)`** — 2-arg POSIX form at `src/repl/scenes.c:~639,733,770,
  1021` **and now `gl_repl.c` `ensure_dir()`** (the per-user music folder —
  see 1F). Windows `_mkdir` is 1-arg. Add, near the includes of *each* TU that
  calls it (or in a shared `os_compat.h`):
  ```c
  #if defined(_WIN32)
  #include <direct.h>
  #define mkdir(path, mode) _mkdir(path)
  #endif
  ```
  (and the `errno != EEXIST` check still works.)
- **`NAME_MAX`** — `src/repl/scenes.c:~16,848`. Undefined on Windows; add a
  `#ifndef NAME_MAX #define NAME_MAX 255 #endif` fallback (or use `MAX_PATH`).
- **Hardcoded `/`** (`scenes.c:~849`, `gl_repl.c:~100`) — Windows APIs accept
  `/`, so this is **cosmetic, not blocking**. Leave for now.

### 1E. Audio — verify only
miniaudio is self-contained on Windows (WASAPI / DirectSound / WinMM, selected
at runtime). No source changes expected. Confirm `MINIAUDIO_IMPLEMENTATION` in
`src/app/glr_audio.c` compiles under MinGW and that `-lwinmm` (and whatever else
miniaudio's Windows path references) is on the link line.

### 1F. Audio asset / music-folder discovery — Windows arms (`gl_repl.c`)

The playlist build (`build_mp3_playlist`, with `scan_dir_into` /
`executable_dir` / `user_music_dir` / `ensure_dir`) gained two
host-specific helpers that currently have only `__APPLE__` / `__linux__`
arms and fall through to a no-op (`#else return 0`) on Windows. Add the
`_WIN32` arms — without them the app still runs and audio still works via
the cwd-relative source and `--assets`, but the bundled-beside-exe and
per-user-folder sources silently do nothing on Windows.

- **`executable_dir()`** (the mac `_NSGetExecutablePath` / Linux
  `/proc/self/exe` ladder): add
  ```c
  #elif defined(_WIN32)
      if (GetModuleFileNameA(NULL, buf, (DWORD)buflen) == 0) return 0;
  ```
  Then the filename-strip must also cut a backslash, since
  `GetModuleFileNameA` returns `\`-separated paths — strip the last `/`
  **or** `\` (the current code only does `strrchr(buf, '/')`).
- **Bundled-assets path is mac-`.app`-shaped.** `build_mp3_playlist`
  forms `<exe>/../Resources/assets` (the bundle layout). Windows has no
  `.app`; the natural standalone layout is `assets/` **beside the exe**
  (see Phase 4). Make source 2 platform-aware: `../Resources/assets` on
  `__APPLE__`, `assets` (i.e. `<exedir>/assets`) on Windows (and this
  also helps a relocated Linux install). With this in place, a
  double-clicked `gl-repl.exe` finds its shipped `assets/` regardless of
  the launch cwd — strictly better than the cwd-only assumption in 4B.
- **`user_music_dir()`** (`~/Library/Application Support/...` on mac, XDG
  / `$HOME` otherwise): native-Windows console has no `HOME`/`XDG_*`, so
  the current `#else` returns 0. Add:
  ```c
  #elif defined(_WIN32)
      const char *appdata = getenv("APPDATA");      /* %APPDATA% */
      if (!appdata || !appdata[0]) return 0;
      snprintf(buf, buflen, "%s/gl-repl/Music", appdata);
  ```
  Build it with `/` separators (Windows APIs and `_mkdir` accept them) so
  `ensure_dir`'s `/`-splitting `mkdir -p` loop works unchanged; otherwise
  it would also need to split on `\`.
- **`ensure_dir()`** uses 2-arg `mkdir(path, 0755)` — covered by the 1D
  `_mkdir` shim, which must be in scope in `gl_repl.c` (add the shim
  there too, or via the shared `os_compat.h`).

`--assets <dir>` / `GLR_ASSETS_DIR` are pure string + `opendir`, so they
work on MinGW with **no Windows-specific change** — the recommended
override for Windows users who keep music elsewhere.

**Phase 1 exit:** `make gl-repl` produces `gl-repl.exe` under MinGW-w64; it
launches, shows a window, accepts commands, plays audio (cwd `assets/`,
`<exe>/assets`, or the `%APPDATA%\gl-repl\Music` folder), exports PLY.

---

## Phase 2 — full parity (`make test` green)

### 2A. Memory profiler — implement the Windows path
`src/support/memprof.c` already has an explicit `_WIN32` stub returning `0.0`
(~L16-18, L89-93) with a comment pointing at `GetProcessMemoryInfo`/`<psapi.h>`.
Implement it: include `<windows.h>` + `<psapi.h>`, call
`GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)` and read
`WorkingSetSize`; for the monotonic clock use `QueryPerformanceCounter`. Add
`-lpsapi` to the Windows link set. Until done, the RSS panel reads zero (not a
crash), so this can trail Phase 1.

### 2B. Test suite — Windows arms
Non-shipped, so it doesn't block running gl-repl, but `make test` needs these
(all in `tests/`):
- `unlink` → `_unlink`; `rmdir` → `_rmdir` (or `<dirent.h>`/`<direct.h>` shims).
- `getpid` → `_getpid` (or `GetCurrentProcessId`).
- `getcwd` → `_getcwd` (`tests/test_glr_ctrl.c`, `test_repl_editor.c`,
  `test_glr_actions.c`).
- Hardcoded `/tmp/...` temp dirs (`tests/test_repl_core_extra.c:~364,433,766,
  839`) → `GetTempPath` / `%TEMP%`.
- `strdup` → `_strdup` (`tests/test_repl_export_all_commands.c`).
- `strtok_r` → `strtok_s` (`tests/test_mesh_ply.c:~110,136`).

A small `tests/support/` portability header (`os_compat.h`) collecting these
`#define`s would localize the churn rather than scattering `#ifdef`s.

### 2C. C99 ratchet on MinGW
Run `make check-c99` under MinGW and confirm it stays green (GL stubs fallback
in `scripts/check/check-c99.sh` should cover the no-system-GL case identically).

**Phase 2 exit:** `make test` and `make check-c99` pass on Windows/MinGW; RSS
panel shows real numbers.

---

## Phase 3 — legacy-Windows reach

- Pin an older MinGW-w64 toolchain and set `_WIN32_WINNT` to the target floor
  (e.g. `0x0501` XP, `0x0600` Vista). Confirm the source still compiles (it is
  C99, so this is mostly a header-define exercise).
- Confirm freeglut's minimum Windows version for the vendored/pinned build, and
  that miniaudio's **DirectSound / WinMM** backends (XP-capable) are reachable
  at runtime when WASAPI (Vista+) is absent — miniaudio selects automatically.
- Document the realistic floor in `MODULES.md` / build notes once verified.

**Phase 3 exit:** documented, tested minimum Windows version with a pinned
toolchain.

---

## freeglut on Windows (cross-cutting)

macOS vendors freeglut statically (Cocoa backend). Linux uses the system
package. Windows needs a third story. Two options:

1. **System/prebuilt freeglut** (MSYS2 `mingw-w64-x86_64-freeglut`) — fastest to
   stand up; `-lfreeglut` resolves from the MSYS2 prefix. But it pulls in a
   `freeglut.dll` runtime dependency, which is exactly what we don't want for a
   presentable release binary (see Phase 4).
2. **Vendor freeglut for Windows too (recommended)** — reuse the existing
   vendored tree and build it statically, mirroring macOS. This is both more
   reproducible *and* the path to a single self-contained `.exe`.

### What the vendored Windows build looks like

The good news: **the vendored tree already contains everything.**
`scripts/vendor-freeglut.sh` copies `src/` recursively (allowlist ~L34-44), and
freeglut's Windows backend lives under `src/mswin/` — so it is **already
vendored**; no change to the allowlist is needed. The pinned tip
(`third_party/freeglut/VENDORED.txt`, SHA `463cef1…`) carries it.

The Windows build mirrors the macOS CMake rule (Makefile ~L878-883), differing
only by dropping the Cocoa flag (freeglut auto-selects its `src/mswin/` backend
on Windows) and by the resulting archive name:

```make
# macOS today:
cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) \
  -DFREEGLUT_COCOA=ON -DFREEGLUT_BUILD_STATIC_LIBS=ON \
  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF
cmake --build $(FREEGLUT_BUILD) --target freeglut_static

# Windows (MinGW) analog:
cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) -G "MinGW Makefiles" \
  -DFREEGLUT_BUILD_STATIC_LIBS=ON \
  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF
cmake --build $(FREEGLUT_BUILD) --target freeglut_static
```

Key differences to wire into the new Makefile Windows branch:
- **Archive name / path.** On non-Windows freeglut has `FREEGLUT_REPLACE_GLUT`
  ON, so the static archive is `libglut.a` (hence the macOS
  `FREEGLUT_STATIC_LIB := …/lib/libglut.a`). On Windows the static target
  produces `libfreeglut_static.a` (MinGW) under `build/lib/`. The Windows branch
  must point `FREEGLUT_STATIC_LIB` at that name and link it by archive path,
  exactly like macOS (no `-lfreeglut`, no DLL).
- **`-DFREEGLUT_STATIC` is already set** project-wide in `COMMON_CFLAGS`
  (Makefile ~L83) — required on Windows so the freeglut headers don't emit
  `__declspec(dllimport)` against a static lib. Nothing to add.
- **Static freeglut's own deps** must be on the consumer link line (freeglut
  static carries no transitive libs, same lesson as the Cocoa frameworks):
  `-lopengl32 -lglu32 -lgdi32 -luser32 -lwinmm` (Phase 1A already lists these).
- The `$(FREEGLUT_STATIC_LIB)` build rule and the `| order-only` prereqs
  (Makefile ~L878, L1048-1054) become shared across macOS+Windows by selecting
  the cmake flags / lib name per platform; Linux stays on the system path with
  an empty `FREEGLUT_LIB`. `make freeglut-clean` works unchanged.

`GLUT_ACTIVE_SUPER` (the editor's Cmd-key handling) is macOS-only and already
has a `#ifndef …→0` fallback, so Windows correctly gets no Super modifier — no
action needed.

---

## Phase 4 — Windows release binary (packaging)

A separate **packaging branch** is producing a presentable macOS artifact
(`.app`/`.dmg`). Phase 4 is the Windows analog: a **single, self-contained,
double-clickable `gl-repl.exe`** a non-developer can run without installing
MSYS2, freeglut, or any MinGW runtime DLLs. Coordinate the directory/zip layout
and version-stamping conventions with that branch so the two releases match.

Requirements for a standalone Windows binary:

- **Static freeglut** — use the vendored static path above (option 2), not the
  MSYS2 DLL, so there is no `freeglut.dll` to ship.
- **Fold in the MinGW runtime.** A MinGW `gl-repl.exe` otherwise depends on
  `libwinpthread-1.dll` (winpthreads, pulled in by `src/app/glr_audio.c`'s
  pthread use) and `libgcc_s_seh-1.dll`. Link the release with
  `-static -static-libgcc -Wl,-Bstatic,--whole-archive -lwinpthread
  -Wl,--no-whole-archive` (or simply `-static`) so these are baked in. Verify
  the result with `ldd`/`Dependency Walker` shows only system DLLs
  (`opengl32.dll`, `gdi32.dll`, `kernel32.dll`, `winmm.dll`).
- **Strip** the release exe (`strip` or link `-s`) to shrink it.
- **Optional polish:** an app icon + version info via a `.rc` resource compiled
  with `windres` into an object linked alongside (gated on the Windows release
  branch only). Lets the exe show an icon and a Properties→Details version.
- **Bundle assets.** The audio playlist now scans three sources
  (`build_mp3_playlist`, `gl_repl.c`): the cwd `assets/`, an exe-relative
  copy, and the per-user folder (see 1F). Ship `assets/` (with at least
  `sample.mp3`, as the macOS bundle does) **next to the exe** in the zip.
  Once 1F's Windows exe-relative arm resolves `<exe>/assets`, a
  double-click finds the music regardless of cwd; until then it relies on
  Explorer launching with cwd = the exe's folder (true for double-click,
  but `--assets`/`GLR_ASSETS_DIR` is the robust override). The
  `%APPDATA%\gl-repl\Music` folder (1F) is where end users add their own
  tracks — document it in the zip's README.
- **Deliverable.** A zip (`gl-repl-win64.zip`) containing `gl-repl.exe` +
  `assets/` + a short README. No installer needed for "present the binary"; an
  installer (NSIS/Inno) is a later option if desired.
- **CI angle (optional).** A GitHub Actions `windows-latest` runner with MSYS2
  can produce this on tag, parallel to whatever the packaging branch sets up for
  macOS. Defer unless the macOS branch establishes the CI pattern first.

---

## Verification matrix

| Build | Command | Phase |
|---|---|---|
| MinGW shipped | `make gl-repl` → `gl-repl.exe` runs | 1 |
| macOS unchanged | `make gl-repl && make test` | 1 (regression) |
| Linux unchanged | gracemont `make check-c99 && make test-stubs` | 1 (regression) |
| MinGW tests | `make test` | 2 |
| MinGW C99 gate | `make check-c99` | 2 |
| Legacy | pinned toolchain, target `_WIN32_WINNT` floor | 3 |
| Standalone exe | `gl-repl.exe` runs on a clean Windows VM (no MSYS2/DLLs) | 4 |

Every phase must re-run the macOS and Linux regressions — the whole point of the
additive `_WIN32` arms is that the existing two platforms stay byte-for-byte the
same.

---

## Status

| Item | State |
|---|---|
| 1A Makefile Windows branch | not started |
| 1B `gl_includes.h` windows.h ordering | not started |
| 1C Time-API `_WIN32` arms (4 sites) | not started |
| 1D Filesystem shims (mkdir/NAME_MAX; mkdir now also in gl_repl.c) | not started |
| 1E Audio verify | not started |
| 1F Audio discovery `_WIN32` arms (executable_dir / user_music_dir / exe-relative assets) | not started |
| 2A memprof psapi path | not started |
| 2B Test-suite Windows arms | not started |
| 2C C99 ratchet on MinGW | not started |
| Vendored freeglut Windows static build (Makefile branch) | not started |
| 3 Legacy toolchain pin + doc | not started |
| 4A Static-link runtime (winpthread/libgcc) + strip | not started |
| 4B Bundle assets/ + zip deliverable | not started |
| 4C Icon/version resource (optional) | not started |
| 4D CI windows-latest packaging (optional) | not started |

## Effort estimate

- Phase 1: a few focused days — shallow changes, `#if` ladders mostly already
  exist, pthreads/audio free under MinGW.
- Phase 2: the longer tail, dominated by test-file count (mechanical).
- Phase 3: mostly toolchain pinning + testing, little code.

Honest one-liner: **a portability port, not a rewrite.**
