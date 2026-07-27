# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

This file is the compact agent brief: commands, conventions, and gotchas.
Deep detail lives in the docs — consult them instead of guessing:

- [`docs/MODULES.md`](docs/MODULES.md) — layered module overview, ownership
  diagram, boundary rules, guard summary, where new code goes.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — frame pipeline, controller /
  render3d / UI layers, state ownership, host bridges, runtime GL capability
  detection, theming, replay, keymap dispatch order.
- [`src/repl/README.md`](src/repl/README.md) + [`src/repl/ARCHITECTURE.md`](src/repl/ARCHITECTURE.md)
  — the language pipeline (parse → compile → apply → flatten → execute),
  standalone `repl_demo`, rebake/dep-routing model.
- [`docs/ADVANCED_USAGE.md`](docs/ADVANCED_USAGE.md) — full CLI + env-var
  reference, headless OSMesa builds, screenshot/GIF/video capture, doc-media
  regeneration, `@cfg` slug list, music/assets, diagnostics, keymap tooling.
- [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) — user-facing command semantics
  (fog, clip planes, blending, materials, labels, …).
- [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) — guard suite, how to extend
  the REPL.
- [`docs/RELEASE.md`](docs/RELEASE.md) — `make release` orchestration (build
  plan, SHA pinning, staging/upload) and `make app` bundle/signing detail.
- [`packaging/web/README.md`](packaging/web/README.md) — Emscripten/gl4es web
  build shims.

Task-scoped checklists live in [`.claude/skills/`](.claude/skills/README.md) and
load on demand — `gl-repl-new-command`, `gl-repl-scene-authoring`,
`gl-repl-config-toggle`, `gl-repl-capture`. Read the matching skill *before*
starting one of those tasks; what stays below is the always-on trip-wire set.

GNU sed is available as `gsed` on macOS (`brew install gnu-sed`).

## Linux / real-gcc verification (gracemont)

Local dev is macOS (`gcc` = Apple clang). Portability-sensitive changes
(build, sanitizer flags, C99) should be cross-checked under real GCC:

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
  git pull --ff-only origin main && \
  make check-c99 && make test-stubs'
```

Host: Ubuntu 24.04, gcc 13.x. Repo path there has **no `src/` segment**
(`~/code/openGL/samples/gen-ai/gl-repl`). `test-stubs` needs no GL dev libs
(headless-safe, picks up ASan+UBSan); `make test` only if GL/GLUT dev
packages are installed; `make debug-msan` for MemorySanitizer.

## Build

```bash
make gl-repl          # Main binary (vendored static freeglut, macOS Cocoa)
make glut             # Fallback: system GLUT (Apple framework)
make test             # Build + run all tests (debug: ASan + UBSan)
make test-stubs       # Tests against bundled no-op GL stubs (no GL libs needed)
make test-msan        # Stubbed tests under MemorySanitizer
make debug-msan       # Everything under MSan (Clang/runtime permitting)
make check-c99        # C99 ratchet (sample + demos + bench)
make check-state-ownership  # Full boundary/contract guard suite
make freeglut-clean   # Drop vendored freeglut CMake build (forces rebuild)
make app              # (macOS) Bundle gl-repl.app — see docs/RELEASE.md
make release          # Build macOS+Linux artifacts, confirm, upload — see docs/RELEASE.md
make fetch-music      # Download music pack from GitHub release into ./assets
make web              # Emscripten build (needs emcc; see below)
make clean            # Remove binaries
```

Requires gcc (C99), OpenGL, GLUT/freeglut; macOS also needs **cmake** (first
`make gl-repl` builds the vendored freeglut in `third_party/freeglut/build/`,
gitignored, survives `make clean`).

- **Vendored freeglut** (`third_party/freeglut/`): static lib, native Cocoa
  backend, linked by archive path. Linux uses system `-lglut -lGL -lGLU`.
  Re-pin with `scripts/vendor-freeglut.sh [<ref>]` (SHA recorded in
  `VENDORED.txt`; `FREEGLUT_REPO=<url-or-path>` accepts a local fork), then
  `make freeglut-clean`. The pinned tree carries the fork's OSMesa backend
  *and* the SIGUSR1/record capture support on all backends — no re-vendor
  needed for capture work.
- **Headless OSMesa**: `make gl-repl FREEGLUT_OSMESA=1` (after
  `brew install mesa mesa-glu`) renders with no window — for headless
  geometry/feedback tests and captures. Separate `build-osmesa/` dirs, so
  Cocoa and OSMesa builds coexist. Compatibility GL only.
- **Capture/recording**: `kill -USR1 <pid>` writes a PPM frame (native +
  OSMesa); `FREEGLUT_CAPTURE_FRAMES=N` records N frames then exits;
  `FREEGLUT_CAPTURE_STREAM` pipes frames to ffmpeg. Scripts:
  `scripts/docs-assets.sh` (regenerates `docs/images/`; deterministic
  frame-based settling), `scripts/record-gif.sh`, `scripts/record-video.sh`
  (scripted interaction via `GLR_POINTER_SCRIPT` pointer scripts with
  symbolic targets — same engine powers the in-app **Tours** menu:
  `tours/*.pointer` + `tours/catalog.ini`, compiled by
  `scripts/gen_tours.py`, validated by `make check-tours-catalog`).
  Full detail: `docs/ADVANCED_USAGE.md`.
- **Web build**: `make web` needs `emcc` on `PATH` — per-shell
  (`source <emsdk>/emsdk_env.sh`); `scripts/build-web.sh` is self-contained
  (sources emsdk itself; checkout via `EMSDK`, default `~/src/emsdk`).
  `make web-serve` serves at `http://localhost:8000/`. gl4es → WebGL2;
  deps fetched by `scripts/web-deps.sh` into gitignored `third_party/web/`.
  Release web links with `-g0` (`DEBUG_INFO_CFLAGS`) — DWARF in the `.wasm`
  costs 3x payload and blocks binaryen opts for no runtime gain.
  `make bench-web` runs `bench_repl` as wasm under node: wasm cost is **not**
  a fixed multiple of native (1.2x–2.2x per-op across sub-benchmarks), so
  `make bench` alone mis-ranks web hot spots. CPU pipeline only — no GPU
  under node, so `fade_batches` skips and the gl4es→WebGL2 draw cost is
  invisible. See `packaging/web/README.md`.
- **Sanitizers**: test targets default `BUILD=debug` = ASan + UBSan
  (`-fno-sanitize-recover`). `make debug-msan`/`test-msan` = MSan + origin
  tracking (Clang; test run sets `GLR_AUDIO_NO_DEVICE=1`). `NO_SAN=1`
  disables debug sanitizers. `gl-repl`/`bench`/demos stay `BUILD=release`;
  an explicit `BUILD=...` always wins (`make coverage`, `make test
  BUILD=release`).

### C99 standard

**Everything compiles `-std=c99`, project-wide, no exceptions** (old-GCC /
Linux portability). Non-pedantic by default — GNU extensions GCC accepts in
`-std=c99` are fine; there is no C2x build and no `STD` knob.
`make check-c99` syntax-checks the sample + demos + bench under
`gcc -std=c99 -fsyntax-only` (project dirs `-I`, GL headers `-isystem`;
tests excluded but build under plain `-std=c99` too).

Conventions for genuinely-old-GCC portability:

- `STATIC_ASSERT(expr, msg)` from [`include/c_compat.h`](include/c_compat.h)
  — **never raw `_Static_assert`**.
- Keep every TU non-empty even when `#ifdef`-gated off.
- Prototyped function-pointer typedefs, not old-style `void (*)()`.
- Plain `, __VA_ARGS__` (no GNU `##`); one `typedef` per type name.

#### `#include` style

Anything in this tree uses quoted `""`; system/vendored headers use `<>`.
Enforced by `make check-include-style` (tracks the ambiguous bare names
[`c_compat.h`](include/c_compat.h), `gl_includes.h`, [`keys.h`](include/keys.h), [`gl_2d.h`](src/ui/core/gl_2d.h)).

```c
#include <stdio.h>           // system — angle
#include <GL/gl.h>           // system GL — angle
#include <miniaudio.h>       // vendored third-party — angle
#include "gl_includes.h"     // project-local — quoted
#include "support/cpuprof.h" // project-local subdir — quoted
```

### GL stub headers

`tests/gl-stubs/include/` ships no-op GL/GLU/GLUT headers so machines
without GL dev packages can compile + run non-rendering tests
(`make test-stubs`, `make gl-repl USE_GL_STUBS=1`; objects go to
`build/*-gl-stubs`). Stubs are compile-only — no window, no pixels. If the
sample calls a new GL/GLU/GLUT symbol, extend the matching stub header;
keep stubs minimal no-op. After touching stubs verify: `make test-stubs`,
`make gl-repl USE_GL_STUBS=1`, `make gl-repl`.

## Run

```bash
./gl-repl                  # Fresh session
./gl-repl output.c         # Reload saved session
./gl-repl workspace/       # Load every *.c as a user scene
./gl-repl --example torus  # Built-in example (name or 1-based index)
./gl-repl --tour editing   # Guided tour on launch
./gl-repl --list-examples | --list-tours | --dump-code
./gl-repl --no-audio       # Skip audio init (isolates ma_engine_init stalls)
```

Full CLI + the `GLR_*` headless-capture env hooks, OSMesa builds, screenshot /
GIF / video recording, docs-media regen, music/assets: **skill
`gl-repl-capture`**, or `docs/ADVANCED_USAGE.md`.

`t` starts at 0 and advances 1/60 s per simulation tick while playing. An
always-on `[init +N.NNNs] <phase>` stderr trace runs in `main()`.

## Test

```bash
make test                  # All tests
make test_eval             # Expression evaluator
make test_format           # Indentation/formatting
make test_repl_core_parse | test_repl_core_format | test_repl_core_commit | test_repl_core_io
```

Test sources under `tests/`, shared helpers `tests/support/`; binaries land
at the repo root (`./test_eval`, …).

**Guards**: `make check-state-ownership` runs the full ownership/contract
inventory (includes `check-c99`, `check-include-style`, `check-keymap-no-dup`,
`check-palette`, …; see the Makefile). Outside that aggregate, two
hard-failing guards: `check-duplicate-api-decls` and
`check-trailing-whitespace` (commits since `origin/main`; also in
`test-stubs` and the pre-push hook).

Design/audit rationale too big for a commit message goes in `docs/plans/`;
root docs describe the current design first and link plans for background.

## File Layout

Band-level map. **Per-file responsibilities: `docs/MODULES.md`** — that is the
authority; don't re-derive a file's job from its name.

| Path | Band |
|------|------|
| [`gl_repl.c`](gl_repl.c)/`.h` | `main()`, GLUT callback registration, window/timer scheduling; forwards to `glr_ctrl_*` |
| [`config.h`](config.h) / [`keymap.h`](keymap.h) / [`accent_palette.h`](accent_palette.h) / [`prof_sections.h`](prof_sections.h) | Force-included root headers: constants, key bindings, palette anchors, profile-section catalog |
| `include/` | Project-agnostic: [`keys.h`](include/keys.h), [`c_compat.h`](include/c_compat.h), [`gl_includes.h`](include/gl_includes.h) |
| **src/app/** | Frame-time controller: display/reshape/init-GL, input routing, config, camera, audio, PLY export, profiling. See [`src/app/README.md`](src/app/README.md) |
| **src/app/boot/** | Startup lifecycle — pre/without frame loop, reached only from [`gl_repl.c`](gl_repl.c). CLI parsing, `--dump-*`, init trace, capture env, frame pacer, splash. **Guard `check-app-boot-band`: the controller must not include these.** |
| **src/repl/** | Language pipeline: parse → compile → apply → flatten → execute, plus specs, eval, scenes, export/import, tutorials. See `src/repl/ARCHITECTURE.md` |
| **src/editor/** | Text document: key dispatch, commit transaction, [`EditorState`](src/editor/state.h#L199), clipboard/undo/search/replace. `edit_ops.{c,h}` stays REPL-free (`check-edit-ops-pure`) |
| **src/subsystems/** | Editor/UI-independent peers: `replay/`, `tutorial/`, `color_picker/`, `variable_panel/`, `edit_overlays/` |
| **src/render3d/** | 3D scene renderer — **no REPL dependency** (proof: `render3d_demo`). Grid/axes/backdrop/lights/overlays/depth-viz/guides |
| **src/ui/** | 2D view rendering + hit-test, **pure over snapshots**. `core/` primitives, `app/` panels + menus + layout, `subsystems/` peer renderers, `support/` prof panels |
| **src/support/** | Neutral utilities: `cpuprof`, `gpuprof` (GL-free TU), `mesh_ply` |
| `tests/` | Tests, `tests/support/` harness, `tests/gl-stubs/` no-op GL headers |

Load-bearing single-source-of-truth files: [`src/app/glr_defaults.h`](src/app/glr_defaults.h)
(`CFG_DEFAULT_*`), [`keymap.h`](keymap.h) (bindings), [`command_spec.c`](src/repl/command_spec.c)
(command metadata), [`src/repl/state.h`](src/repl/state.h) / [`src/editor/state.h`](src/editor/state.h) (state facades).

## Conventions

- File-private statics use `g_` prefix. Cross-module runtime state goes
  through typed facades: [`src/repl/state.h`](src/repl/state.h) (REPL),
  [`src/editor/state.h`](src/editor/state.h) (editor), peer accessors
  ([`replay_state_view()`](src/subsystems/replay/replay_state.h#L126), `variable_panel_state_view()`). Static helpers are
  file-scoped; public API through module headers.
- Prefixes express ownership: `repl_*` (language/program model), `editor_*`
  (text document), `glr_*` (app shell/controller/services), `render3d_*`
  (3D), `ui_*` (2D view), neutral (`prof`) for generic utilities. Don't
  introduce new top-level prefixes without documenting the boundary.
- **Config toggles** → skill `gl-repl-config-toggle`. One-line version: append a
  [`ReplConfigItem`](src/repl/cfg_baseline.h#L29) to `g_cfg_items[]` in [`src/app/glr_actions.c`](src/app/glr_actions.c)
  under the right `### ` section; count + flyout membership auto-compute.
- **New GL commands** → skill `gl-repl-new-command` (five required edits:
  [`CmdType`](src/repl/command.h#L44), parser, executor, `flatten_range()`, spec tables).
- Enum args live in `GLCmd.args[]` (there is **no `GLCmd.mode` field** — its
  absence is the invariant). Per-slot [`ReplEnumSlotKind`](src/repl/command_spec.h#L77): `ENUM_ONLY`
  (default), `ENUM_OR_CONST_VALUE` (bool masks, 0/1 reverse-mapped),
  `ENUM_OR_EXPR` (only `glLightModeli` slot 1). Spec tables stay **alphabetical
  by GL name**.
- [`CmdType`](src/repl/command.h#L44) set tests use the inline predicates in [`command.h`](src/repl/command.h)
  (`repl_cmd_is_transform`, `repl_cmd_emits_vertex`, `repl_cmd_is_block_head`
  / `_end`), never ad-hoc `||` chains. That's the *control-flow* taxonomy;
  [`CmdSyntaxCategory`](src/repl/command_spec.h#L153) is the separate *visual* one — don't fold one through
  the other. A drift test in [`tests/test_replay_walk.c`](tests/test_replay_walk.c) asserts agreement.
- Splitting comma-separated call args: [`repl_scan_next_arg_delim()`](src/repl/eval.h#L438) from
  [`src/repl/eval.h`](src/repl/eval.h), never bare `strchr(s, ',')` — those
  are paren-naive and truncate `cos(i + phase)`.
- **Keyboard bindings**: one [`keymap.h`](keymap.h) pair per action; call
  sites use `keymap_event_is(key, GLR_X)` and never spell out modifiers;
  `KM_KEY`/`KM_MODS` for case labels / initializers. Guard:
  `make check-keymap-no-dup`; `make keymap-list` shows bindings + free
  slots. [`editor_handle_key()`](src/editor/input.h#L56) takes ASCII (Ctrl+X arrives as X & 0x1F),
  [`editor_handle_special()`](src/editor/input.h#L57) F-keys/arrows. Cross-subsystem routing (replay /
  save / config / audio / camera / tutorial-ack) lives in the
  `glr_ctrl_router_*` helpers, dispatched before `editor_handle_key`.
  macOS Cmd+letter is normalized to Ctrl form at the top of
  `glr_ctrl_keyboard`.
- Expression variables: [`ExprVar`](src/repl/eval.h#L136) in eval.h; predef set via
  `repl_state_variables()` + [`repl_eval_declare_predef_var()`](src/repl/eval.h#L319).

## Architecture — agent gotchas

Full prose: `docs/ARCHITECTURE.md` and `src/repl/ARCHITECTURE.md`. What
follows is the trip-wire list.

### Frame & rendering

[`glr_ctrl_display_frame()`](src/app/glr_ctrl.h#L182) drives each frame: rebuild autonormals + flat
program if dirty → build [`Render3dRenderConfig`](src/render3d/render_types.h#L140) → clear chrome + load camera
+ scissor (all **controller** policy — render3d owns no camera type, sets no
scissor, clears no color/depth) → [`render3d_draw_scene()`](src/render3d/render.h#L137) (projection → user
geometry callback → replay fades → grid/axes/backdrop → overlays → replay
HUD) → 2D overlays. `render3d_demo` is the load-bearing proof that
`src/render3d/` has no REPL dependency; `make render3d-hot` is its
dlopen-based live-reload variant (state lives in the host TU; a
[`Render3dState`](src/render3d/render.h#L97) layout change still needs a relaunch).

### Accumulation effects

Accum effect Off/AA/Blur/Blur Cam × accum passes (1..16). The **effect picks
the blur axis** — camera motion never overrides it: Blur = animation-time
blur (re-bakes the flat program at sub-step `t` via
`repl_state_time_set_transient` + `repl_refresh_flat_program_for_deps`;
works while paused), Blur Cam = camera-pose lerp only. Blur and AA jitter
are never combined; an axis with nothing to blur falls back to AA jitter.
Replay forces the fallback (a per-sample reflatten would clobber the
replay-narrowed flat count). Each sample resets predef/scratch/render to a
frame baseline so accumulating programs don't compound.

### Two-level command model

Source `GLCmd[]` (per-line canonical **text lives in [`EditorState`](src/editor/state.h#L199)'s editor
buffer, not on [`GLCmd`](src/repl/command.h#L121)**) → flat array (loops unrolled, funcs inlined, ifs
resolved; each flat cmd records `src_cmd_idx` / `call_src_cmd_idx` /
`func_scope_mask`) → executor emits GL. Any edit marks the flat array dirty;
rebuilt next frame. Budgets: `MAX_FLATTEN_VISIT_BUDGET` = 200000,
`MAX_FLATTEN_CALL_DEPTH` = 64. Animation is reflatten-per-frame, not
execute-time re-eval.

### Commit paths (two, and they differ)

- **Interactive `;` key**: the input buffer does **not** contain the `;` —
  handlers must accept input without a trailing `;`.
- **[`editor_feed_line()`](src/editor/input.h#L188)** (file/example loading): copies the full line
  *including* `;`, then runs the same chain.
- Enter may or may not have `;`. [`editor_load_line_to_input()`](src/editor/input.h#L182) strips the
  trailing `;`, so re-committing an existing line takes the no-semicolon
  path — handlers checking for `;` must also accept end-of-string.

The chain is consolidated in [`src/editor/commit.c`](src/editor/commit.c):
`editor_try_commit_var_statements` / `_block_structs` / `_any` /
`_var_statements_then_insert` (used by overwrite-mode Enter). Add new
handlers to the right helper, not to call sites. **Ordering is
load-bearing**: `editor_try_commit_float_decl` MUST run before
`editor_try_commit_assign_variable` or `float x;` parses as an assignment.
Handlers return 1 if consumed (success or error), 0 if no match; fallthrough
is [`repl_parse_and_normalize()`](src/repl/normalize.h#L20) → `parse_command()`.

### Float declarations (`CMD_VAR_DECLARE`)

- **Storage is keyword-first, cursor-second.** `static float x;` is a global
  from any cursor position; plain `float x;` is a **function-scoped local**
  when there is an enclosing `CMD_FUNC_DEF` and a global at top level. A local
  row is marked `var_idx == REPL_VAR_IDX_LOCAL` (no payload field — the decl
  arm already dominates the union) and emits no predef op.
- New decls insert at the **top of non-decl code** regardless of cursor (so
  every reference follows its declaration) — top of the *document* for a
  global, top of the enclosing *function body* for a local, hoisting from any
  nesting depth; editing an existing decl overwrites in place (carried-over
  names are exempt from the dup check).
- No-op in executor/flatten — registration happens at commit time via
  [`repl_eval_declare_predef_var()`](src/repl/eval.h#L319). Locals register
  nowhere: `flatten_bind_func_locals` re-derives them from
  `payload.decl.names[]` per call.
- [`GLCmd`](src/repl/command.h#L121) payload is a tagged union keyed on `type`
  (`payload.decl.*`, `payload.assign.prev_local_value`, `payload.label.fmt`,
  `payload.matrix.m[]`); other types must not read it. A flat local assignment
  captures its pre-write target value in the assignment arm because the
  ordinary `FlatCmdLocalVars` snapshot is post-write.
- Deleting a decl range goes through [`repl_compile_delete_range()`](src/repl/compile.h#L557) which
  validates no variable is still referenced outside the range. Cut/copy/
  paste of decl rows is blocked outright.
- Export writes `// @declare` markers; import reconstructs decls bypassing
  the commit handler. `MAX_PREDEF_VARS` = 32 (1 reserved for `t`). A local
  exports as a real C automatic with **explicit zero initializers**
  (`float a = 0.0f, b = 0.0f;`) — behavior parity, not syntax: the REPL binds
  locals to 0 on call entry and C would leave them indeterminate. Import lowers
  exactly that literal-zero form back to `float a, b;` and nothing else, so a
  hand-written `float a = 5;` in a body still hits the no-initializer rejection.
- Locals follow **C's scope rules, not a shadowing ban**: colliding with a
  parameter or another local of the same body is a redefinition (rejected);
  shadowing a global or an enclosing loop iterator is legal, innermost wins.
  PARAM/LOOP bindings stay unwritable, so a binder edit that would *capture* an
  existing assignment is rejected (`compile_binder_captures_assignment`).
  Capacity is whole-function: `params + locals + deepest loop nesting <=
  MAX_EXPR_VARS` (`compile_func_scope_peak`). Any dep feeding a local
  assignment is reported **structural** — frozen `FlatCmdLocalVars` snapshots
  make a value-only rebake unable to propagate a local forward.

### User scenes & auto-promotion

Up to `MAX_USER_SCENES` = 8 slots in `g_user_scenes[]`
([`src/repl/scenes.c`](src/repl/scenes.c)); no automatic startup scene.
Editing an example auto-promotes it into a fresh slot — the hook is
[`editor_undo_push_snapshot()`](src/editor/undo.h#L123) → [`repl_promote_example_if_needed()`](src/repl/scenes.h#L46) before
every mutation. LRU eviction to `<workspace_dir>/<slug>.c` only when a
workspace is bound (else promotion is rejected with a status message).
F12 cycles examples → user scenes → back. Inline rename filters
path-unsafe chars. Workspace round-trips via `@scene-name` /
`@workspace-dir` headers.

### Example metadata & presentation reset

**Every example load resets the non-camera presentation settings (incl.
`view_mode`) to `CFG_DEFAULT_*` (in [`src/app/glr_defaults.h`](src/app/glr_defaults.h)
— the single source of truth; reuse the macros, never duplicate literals)
before applying the example's `@cfg`.** Camera is deliberately excluded
(inherited unless a `// camera` header is present); `restore_user_scene()`
restores commands + predef vars only.

Full authoring detail — `@cfg` slugs, the five files that move together, size
budgets, index-keyed goldens: skill `gl-repl-scene-authoring`.

### Save / load

Export ([`src/repl/export.c`](src/repl/export.c)) writes standalone C:
header directives (`@cfg`, `@scene-name`, `@workspace-dir`), camera as raw
transforms, predefs + scratch arrays as globals, funcs, `display()` body.
Import ([`src/repl/import.c`](src/repl/import.c)) reverses it line-by-line,
feeding geometry through [`editor_feed_line()`](src/editor/input.h#L188). The `IMPORT_EXPORT_STATE`
macro block is deliberately duplicated verbatim across the two TUs.
`repl_cfg_get_int`/`_set_int` etc. go through the installed config bridge
only (`check-repl-export-via-bridge`).

### Replay

State in [`replay_state_view()`](src/subsystems/replay/replay_state.h#L126); playback/fade/input split across
`src/subsystems/replay/`. During playback the flat count is clamped to
[`replay_exec_limit()`](src/subsystems/replay/replay.h#L79); fade-batch ring renders old geometry in a blended
pass. Fade replays skip `CMD_CLEAR`, and clamps never cut below the
program's leading `glClear` (`replay_frame_setup_limit`).
Assignment annotations associate prior rows with the current invocation by
stable flat provenance (`func_scope_mask`, `call_depth`, immediate/root call
sites) and backward execution order — never by `FlatCmdLocalVars` values,
which legitimately change between commands after local assignments.

### Undo

32-slot global rings (not per-scene). **Any wholesale replacement of the
live document must call [`editor_undo_clear()`](src/editor/undo.h#L145) first** or post-switch Ctrl+Z
restores the previous scene into the new one (call sites:
`glr_ctrl_reset_all`, F12 cycle, load-example/scene/workspace actions in
[`glr_actions.c`](src/app/glr_actions.c)). Push clears the redo stack; push is also the
auto-promotion hook.

### Cursor edit guides

`glr_ctrl_build_guide_snapshot()` text-parses the input line with a
**predef-only** evaluator — funcN-locals/loop vars evaluate to 0. Fix path:
the render always walks the flat program (`replay_walk_user_vertices`, with
`ctx.stop_flag` for early-out) and overrides `vertex_args`/`normal_args`
from the flat cmd at the cursor. Vertex-arg slot parsing uses
`repl_scan_next_arg_delim` (nested parens).

### Autocomplete / search

Modes: `AC_MODE_FUNC_PREFIX`, `AC_MODE_ENUM_SLOT` (active slot = top-level
commas before cursor), `AC_MODE_POINT_PARAM`. Enum modes also fire mid-line
when the cursor ends the token being completed and the tail is only
trailing args (Tab splices, keeps tail); the inline ghost stays
end-of-input-only; function-name completion/hints too. `AC_MODE_FUNC_PREFIX`
draws from the static `k_func_completions[]` table *plus* runtime func-slot
aliases (candidate text materialized in glr_completion.c statics, filtered to
slots with a live `CMD_FUNC_DEF` — alias names outlive deleted defs). Param
hints resolve the callee through `repl_scan_func_name_token`, so `funcN(` and
`drawCube(` behave alike. Search: Ctrl+F, state via
[`editor_state_search()`](src/editor/state.h#L429).

### Find / replace

One [`EditorSearchState`](src/editor/state.h#L136) holds both fields plus `whole_word` and a 3-stop
focus ring (find field / replace field / word chip) that **Tab** cycles —
there is no free Ctrl slot in [`keymap.h`](keymap.h), so the ring *is* the keyboard
path. `whole_word` belongs to *matching*, not replacing: it feeds the
match/count/ordinal helpers and the code-panel highlight pass, so what is
highlighted is exactly what a replace rewrites. Match primitives live in
[`ui/core/text_search.c`](src/ui/core/text_search.c) (`_opts` twins take the flag; short names keep
plain-substring behavior for the editor-demo twin).

Replace is a **whole-document transaction**, not a sequence of commits:
renaming a variable or funcN alias is invalid at every intermediate step.
[`src/editor/replace.c`](src/editor/replace.c) substitutes text across the committed buffer (never
the live input row, so half-typed lines can't fail the operation) and
[`repl_document_rebuild()`](src/repl/replace.h#L60) ([`src/repl/replace.c`](src/repl/replace.c))
replays it through `repl_load_apply_line` — the file/example loader — under
a [`SceneSnapshot`](src/repl/scene_snapshot.h#L17) that is restored wholesale if any line is rejected.
Carry-overs the loader would otherwise drop: predef *values* (by name, plus
the `rename_from`/`rename_to` pair) and `is_auto` on auto-normal rows (by
row position; a substitution never changes the row count). One undo
snapshot per replace; the ring is rewound via
[`editor_undo_ring_state_restore()`](src/editor/undo.h#L116) when the
rebuild fails, so a rejected replace leaves no trace.

Replace-current addresses its match by **occurrence ordinal within the row**,
not char offset: search rows read the unindented input buffer for the edit
line while the document row carries its indentation.

### Config / Tutorials menus (shared flyout engine)

`### ` rows in `g_cfg_items[]` define sections → one parent row each +
synthetic trailing **All** row, hover-opening flyouts (generic engine in
[`src/ui/app/menu_bar.c`](src/ui/app/menu_bar.c), shared with Scene +
Tutorials; wheel-scroll for tall flyouts). Parent/tag rows are **inert on
click** (hover-open only — the activate branches return 0); flyout item
clicks route via `route_submenu_item_hit` (`glr_cfg_cycle_row` for config,
`tutorial_start` for tutorials); right-press cycles backward. Tutorials
mirror the example tag system (`REPL_TUTORIAL_TAG_*` mask; `.tags` required
— metadata test fails on 0/unknown bits); optional `.subheading` renders
`### ` group headers — entries sharing one must be contiguous per tag
(enforced by `test_catalog_subheading_metadata`). Tutorial step kinds
(COMMAND / NOTE / SET / REQUIRE / REQUIRE_VAR incl. declaration steps and
setup scaffolds): see `src/repl/tutorials.{c,h}` headers and
`src/subsystems/tutorial/`.

## Key Controls

`make keymap-list` prints the live bindings + free slots — it reads
[`keymap.h`](keymap.h), so it never goes stale. User-facing table:
`docs/USER_GUIDE.md`.

The handful worth knowing cold: `;` commits the current line, Enter inserts a
new one, F12 / Shift+F12 cycle examples and scenes, Ctrl+R replays, Ctrl+F
finds, Ctrl+Z undoes, F1 is help.

## Supported Commands

```
glBegin/glEnd, glVertex3f/glVertex2f, glNormal3f, glColor3f/glColor4f
glClearColor, glClear(mask), glClearDepth, glClearStencil
glTranslatef/glScalef/glRotatef, glPushMatrix/glPopMatrix/glLoadIdentity
glMultMatrixf((GLfloat[]){m0..m15}) | glMultMatrixf(A)   (column-major 4x4:
                               16 inline expressions, or a scratch array)
glPushAttrib(mask)/glPopAttrib, glEnable/glDisable(CAP)
glFogi/glFogf/glFogfv, glClipPlane, glShadeModel, glPointSize, glLineWidth
glLineStipple, glPointParameterfv, glBlendFunc, glColorMaterial, glMaterialfv
glLightModeli, glFrontFace, glCullFace, glDepthFunc, glDepthMask, glColorMask
glStencilFunc, glStencilOp, glStencilMask
glPolygonMode(face, mode), glPolygonOffset(factor, units)
glutSolidTorus/Cube/Sphere/Teapot/Cone, glRasterPos3f
label("fmt", ...)              (bitmap text; REPL primitive)
for(var, start, end[, step]) { }   func0..func9(params) { }   if(expr) { }
:name / goto name              (goto labels — not label())
float name[, ...];    var = expr;    A[i] = expr;    // comment
static float name[, ...];      (global from anywhere; plain `float` inside a
                                function body is a function-scoped local)
```

Exact signatures, arg policies, and the math/expression language: skill
`gl-repl-scene-authoring`. Semantics: `docs/USER_GUIDE.md`.

Two policies that bite from a distance, so they stay here:

- **`glClear` is load-bearing** — nothing clears the scene rect on the
  program's behalf (identical to the exported C); deleting the line smears the
  frame. Replay fade batches skip `CMD_CLEAR`, and replay clamps never cut
  below the leading clear.
- **Overlay passes** replay clip/cull/`glFrontFace` state as they walk
  (`overlay_gl_track_cmd` in edit_overlays.c) so outlines/points match the
  frame; color-mask gates those walks instead of being replayed. The
  Polygon-highlight On state suspends clip planes + culling for the cursor
  highlight only.

## Math

`sin cos tan asin acos atan atan2 sqrt abs pow log ln min max clamp lerp
smoothstep sign floor ceil fmod rem rand rand2` — `log` is base-10, `ln`
natural; `asin`/`acos` clamp their input to [-1, 1] (the evaluator stays
total); `lerp` is deliberately unclamped. `clamp`/`lerp`/`smoothstep`/`sign`
have no libm twin, so export emits a `repl_*f` helper per used one
(`write_shape_helpers`) — keep those bodies identical to [`eval.c`](src/repl/eval.c)'s. Constants
`PI`, `TAU`, `e`. Only `t` is predefined; others need `float name;`
(`MAX_PREDEF_VARS` = 32, 31 user slots — function-scoped locals consume none
of them; they live in the `MAX_EXPR_VARS` = 32 per-call scope array instead).
Scratch arrays `A`/`B`/`C[16]` are fixed globals — those names, plus `t`, `PI`,
`TAU`, `float`, `var`, reject a declaration.

Decl tags `// @tune` / `// @config`, budgets, and authoring gotchas: skill
`gl-repl-scene-authoring`.
