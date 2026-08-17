# Tests

85 test binaries plus the harness, the no-op GL stubs, the golden fixtures and
the opt-in scene corpora. Everything here runs headless — the default lane
links [`gl-stubs/`](gl-stubs/README.md) instead of a real driver, so no display
and no GL dev packages are needed.

Contributor-facing policy (what to run before pushing, sanitizer knobs, the
guard suite) lives in [`docs/CONTRIBUTING.md`](../docs/CONTRIBUTING.md). This
file is the map of *what is in here* and *which binaries are worth driving by
hand when something breaks*.

## Running

```bash
make test                      # the gate: checks + whole suite vs GL stubs
make test-stubs                # same suite, skipping nothing, NO_SAN=1 default
make test NO_SAN=0             # ...with ASan + UBSan
make test-msan                 # MemorySanitizer (Linux/Clang)
make test-scenes               # opt-in tests/scenes corpora (export + cc each)
make test-web                  # the same binaries as wasm under node
make gl-tests                  # the five tests that need a real GL context
make test-full                 # all of the above plus demos and benchmarks
```

One binary at a time — every test has a `run-` target with dashes for
underscores, and `TEST_ARGS` passes flags through:

```bash
make run-test-repl-core-parse
make run-test-repl-core-examples TEST_ARGS='--dump-index 2'
make run-test-export-trace-parity TEST_ARGS='--full --keep-traces'
```

Other knobs: `TEST_JOBS=N` caps runner parallelism (default: all at once),
`TEST_VERBOSE=1` (or `make test-detailed`) turns on per-example export/compile
logging, `TEST_LOG_DIR` relocates the per-binary logs (default
`build/test-logs/run-$$`), `NO_COLOR` drops ANSI output. Binaries land in
`$(BINDIR)`; the runner is [`scripts/run-tests.sh`](../scripts/run-tests.sh).

## Debugging toolkit

Four of these are not just regression tests — they are instruments, with their
own `--help`. Reach for them first when a scene renders wrong, an export
misbehaves, or an expression evaluates to something surprising.

### `test_export_trace_parity` — *"the exported C does not match what I see"*

The differential between `repl_execute_program()` and the exported C compiled
by `cc` and run for real. It compares **argument values, not just call
counts**, over several `t` values run as successive frames of one session — so
it catches a frozen vertex and a stale value-only rebake, neither of which a
counter can see. On mismatch it prints the differing counters, the differing
trace lines, and a unified `diff(1)` of the two traces so you can see *when in
time* the divergence started.

```bash
make run-test-export-trace-parity TEST_ARGS='--help'
make run-test-export-trace-parity TEST_ARGS='--full'            # every built-in example
make run-test-export-trace-parity TEST_ARGS='--scenes-dir tests/scenes/general'
make run-test-export-trace-parity TEST_ARGS='--keep-traces'     # keep .repl.tr / .child.tr on FAIL
```

Buckets are `PASS` / `FAIL` / `XFAIL` / `XPASS`; the expected-failure list is
`g_example_xfail` in the source, and an `XPASS` fails loudly so the list cannot
rot. Stub-only (both legs need the stub counters live).

### `test_repl_core_examples` — *"the catalog changed and 32 goldens disagree"*

Despite the name this is the catalog/export omnibus. It owns the index-keyed
golden fixtures under [`testdata/repl_examples_ui/`](testdata/repl_examples_ui/)
— the code-panel text of every built-in example — and runs three legs in this order, each
announced by a `--- … ---` banner:

1. **Catalog checks** — loader limits, catalog/tag/subheading metadata, `@cfg`
   symbolic names, tag-default dispatch and collisions, presentation reset,
   clear-color ordering.
2. **Built-in catalog** — per example: golden code-panel text vs
   `NN.golden.txt` (`--show-mismatch` prints context here), export + a real
   `cc`, import round-trip (the `Loaded N commands from …` chatter),
   re-export + `cc`. One progress line each:

   ```
   example 29/39  447 cmds  golden=ok cc=ok import=ok recc=ok  Orrery (labels track 3D orbits)
   ```
3. **Scene corpora** (opt-in, see below) — load, no-invalid-cmds, export,
   compile, one line per scene. No goldens and no round-trip: those scenes
   exist to be corner cases, and a golden apiece would turn every deliberate
   edit into a regen. They run **last** because a runtime catalog displaces the
   built-in one.

```bash
make run-test-repl-core-examples TEST_ARGS='--help'
make run-test-repl-core-examples TEST_ARGS='--show-mismatch'   # context diff around a text mismatch
make run-test-repl-core-examples TEST_ARGS='--dump-index 7'    # one example's panel text to stdout
make run-test-repl-core-examples TEST_ARGS='--keep-temp'       # leave the exported .c files behind
make run-test-repl-core-examples TEST_ARGS='--scenes-dir tests/scenes/general'
REPL_SCENE_CORPUS=1 make run-test-repl-core-examples            # the standard corpora
make rebuild-golden                                            # = --update-golden, all fixtures
```

The banners and progress lines print unconditionally — before they existed the
only sign of life was the loader's `Loaded N commands from …` chatter, which
comes from leg 2's round-trip and which leg 3 does not do at all, so a corpus
run looked identical to a run without one but for the trailing `N/N passed`
count. Passing assertions still print nothing; `FAIL` lines and `DETAIL`
dumps come from the assertions, the progress line is the map, not the
diagnosis. When no corpus is selected the run says so rather than staying
quiet about it.

Environment: `REPL_SCENE_CORPUS=1`, `REPL_EXPORT_VERBOSE=1`,
`REPL_EXPORT_KEEP_TEMP=1`, `REPL_EXPORT_CC`, `REPL_EXPORT_COMPILE_CFLAGS`,
`NO_COLOR`. Regenerating goldens needs a **debug** build
(`make test_repl_core_examples BUILD=debug` first) — the `run-` targets
otherwise give you release.

A corpus scene is walked only if `catalog.ini` lists it; an unlisted `.glr`
in the directory is silently ignored, so check the scene count if a new
corner case seems to have no effect.

### `test_eval` — an interactive expression REPL

Without arguments it is a shell on the expression evaluator, which is far
faster than launching the app to answer "what does this expression do?":

```bash
./test_eval                 # interactive
./test_eval --run-tests     # the built-in suite (what `make test` runs)
./test_eval --rand-dist N   # rand() uniformity table over N samples
```

Interactive commands: bare `<expr>` evaluates, `set <var> <value>` binds a
predef, `to_c <expr>` / `to_repl <expr>` show the export/import lowering both
ways, `for <header>` / `cfor <header>` parse the two loop-header dialects,
`vars` lists bindings, `randdist [N]`, `quit`.

### GL stub traces — *"what calls did this actually make?"*

[`gl-stubs/`](gl-stubs/README.md) counts every GL call and can log each one
with its arguments. `gl_stub_trace_open(path)` starts a trace,
`gl_stub_counts_reset()` / `gl_stub_counts_dump()` bracket a count comparison.
That machinery is what the parity test and every draw-assertion test is built
on, and it is the fastest way to instrument a new test.
[`export_trace_driver.c`](export_trace_driver.c) is the standalone child
program the parity test compiles against an exported scene:

```
export_trace_driver <counts-file> [<trace-file> [<t> ...]]
```

It `#include`s the exported `.c` (the geometry helpers are `static`) and runs
only the user-geometry body — one frame per `t` argument.

Also worth knowing: `test_glr_cli` drives the binary's whole `argv` surface, so
CLI questions are answerable without launching the app; and the two **real-GL
differential oracles** — `test_attrib_bits_gl` (does `attrib_bits.c`'s cell→bit
table match what `glPushAttrib`/`glPopAttrib` really saves?) and
`test_gl_state_inspector_gl` (does the pure state fold model the driver?) —
answer "is our model of GL wrong?" rather than "did we regress?". Both need
`make gl-tests` (a display, or `FREEGLUT_OSMESA=1`).

## Layout

| Path | What |
|------|------|
| `test_*.c` | one binary each; sources sit flat in this directory |
| [`support/`](support/) | [`test_harness.h`](support/test_harness.h) (`TEST_ASSERT_*` + report), [`repl_test_support.h`](support/repl_test_support.h), [`scene_corpus.h`](support/scene_corpus.h) (the `REPL_SCENE_CORPUS` gate — read it nowhere else), [`camera_bridge_stub.h`](support/camera_bridge_stub.h), `prof_sections_wide.h` |
| [`gl-stubs/`](gl-stubs/README.md) | no-op GL/GLU/GLUT headers + the counter/trace TU |
| [`testdata/`](testdata/) | goldens: `repl_examples_ui/NN.golden.txt`, `camera-order/` |
| [`scenes/stress/`](scenes/stress/), `scenes/general/` | opt-in `.glr` corpora, loaded as runtime catalogs (`make test-scenes`) |
| [`export_trace_driver.c`](export_trace_driver.c) | not a test — the child program for export parity |

Adding a scene directory to the standard set is one edit — the `dirs[]` list
in [`scene_corpus.h`](support/scene_corpus.h), which
[`test_repl_core_examples`](test_repl_core_examples.c) and
[`test_export_trace_parity`](test_export_trace_parity.c) both iterate — plus a
`parity_walk_dir_checked()` line in
[`test_camera_header_parity`](test_camera_header_parity.c), which still names
its dirs. A walk over a missing directory returns 0 and would otherwise pass
silently.

Both corpus-walking binaries also take a repeatable **`--scenes-dir D`** to
walk an arbitrary `.glr` catalog directory, on the same precedence: an
explicit flag replaces the `REPL_SCENE_CORPUS` set rather than adding to it.

## Lanes

- **Stubs (default).** `TEST_BINS` — everything below, minus the real-GL set.
- **Web.** `make test-web` builds the same binaries as wasm under node. Two are
  excluded (`WEB_TEST_EXCLUDE`): `test_audio` and `test_ui_menu_bar`, each
  asserting behavior the web build deliberately does not have. Prefer a
  `#if defined(__EMSCRIPTEN__)` arm around the affected assertions over
  extending that list. No gl4es in the link, so the WebGL2 draw path is
  invisible here.
- **Real GL.** `GL_TEST_BINS` = `test_ui_gl_state`,
  `test_scene_underwater_fill_gl`, `test_attrib_bits_gl`,
  `test_tour_overlay_feedback`, `test_gl_state_inspector_gl`. Never built by
  `make test`.
- **Scene corpora.** Only `test_repl_core_examples`,
  `test_export_trace_parity` and `test_camera_header_parity` read
  `REPL_SCENE_CORPUS` (all three via `support/scene_corpus.h`); the first two
  also take `--scenes-dir`. `make test-scenes` builds and runs the first and
  third.

## The catalog

### Language pipeline — parse → compile → apply → flatten → execute

| Test | Covers |
|------|--------|
| `test_repl_core_parse` | command parsing, arg policies, usage/error text |
| `test_repl_compile` | compile/apply boundary: compile never mutates buffer/store/status/undo; local + binder scope rules |
| `test_repl_core_commit` | the commit chain and its load-bearing handler ordering |
| `test_repl_command_store` | the source `GLCmd[]` store |
| `test_repl_executor` | GL emission (includes `repl_executor.c` directly for its statics) |
| `test_repl_flatten_deps` | dependency-mask derivation and the dirty routing it drives; exact-equality asserts so overbroad masks fail too |
| `test_repl_flatten_differential` | flatten fast paths vs `force_reparse`, over the whole built-in corpus |
| `test_repl_flatten_rebake` | in-place value-only rebake reproduces a full flatten, command for command |
| `test_repl_locals` | function-scoped locals: per-invocation lifetime, innermost-first lexical resolution, structural dep routing |
| `test_expr_program` | compiled evaluator vs text evaluator, bit-exact (`memcmp`, so NaN parity counts) + dep-mask algebra |
| `test_eval` | the expression evaluator — **and an interactive REPL**, see above |
| `test_repl_autonormal` | auto-generated normals |
| `test_repl_state` | the REPL state facade + the pure GL-state fold sweep |
| `test_repl_core_internal` | internals not reachable from the public surface |
| `test_repl_core_extra` | leftover core coverage |
| `test_format`, `test_repl_core_format` | `repl_format_reindent_from_parsed()` and formatting |

### Export / import

| Test | Covers |
|------|--------|
| `test_export_trace_parity` | **the value-level differential** — see above |
| `test_repl_core_examples` | **golden fixtures + catalog export/compile** — see above |
| `test_repl_export_all_commands` | every supported GL command through export→import, stable text |
| `test_repl_export_clearcolor` | `glClearColor` stays in the body, in source order relative to `glClear` |
| `test_repl_export_lights` | code-panel light text and exported light text share one generator |
| `test_repl_tune` | `// @tune` knobs: collector, cap, generated HUD/keyboard code, round-trip, compile gate |
| `test_camera_header` | the `@camera` reader alone — every accept/reject rule, deferred pose merge |
| `test_camera_header_parity` | file loader vs catalog loader must agree: text, commands, pose, predefs, diagnostics |
| `test_camera_apply_modes` | each caller passes the mode it means (snap/ease × becomes-scene-default) |
| `test_repl_core_io` | save/load paths |
| `test_mesh_ply` | the pure PLY writer, driven with synthetic feedback buffers |

### Editor

| Test | Covers |
|------|--------|
| `test_repl_editor` | the text document end to end |
| `test_editor_input_selection` | anchor lifecycle, selection derivation, atomic extend, buffer-shrink invariant |
| `test_repl_core_search`, `test_repl_core_search_extra` | Ctrl+F search state |
| `test_repl_replace` | whole-document replace transactions, rename, rollback, undo rewind |
| `test_repl_autocomplete`, `test_editor_completion` | the three completion modes, ghost text, param hints |
| `test_repl_var_drag` | drag-to-scrub on numeric literals |
| `test_repl_code_panel_document` | the document model behind the code panel |
| `test_repl_code_panel_layout` | panel layout math (header-only target) |
| `test_repl_code_panel_syntax` | per-kind argument syntax classifier and its color model |

### App shell / controller

| Test | Covers |
|------|--------|
| `test_glr_ctrl` | the controller — the largest binary here (~6k lines) |
| `test_glr_actions` | menu/keyboard actions, config rows, scene + workspace loads |
| `test_glr_cli` | **the whole `argv` surface** + the `--dump-*` exit-before-window contract |
| `test_glr_capture_env` | the `GLR_*` headless-capture env hooks, each observed through the state it writes |
| `test_glr_camera` | camera boundaries, easing, auto-rotation |
| `test_glr_frame_pacer` | absolute-deadline 60 Hz pacer |
| `test_glr_init_trace` | the `[init +N.NNNs]` startup trace (stderr captured) — the startup-stall diagnostic |
| `test_splash` | startup splash banner |
| `test_audio` | audio engine + hitch threshold (native-only; excluded from the web lane) |
| `test_scene_file_menu` | Scene/File menu actions + save filename derivation |

### 3D renderer (`src/render3d/`, no REPL dependency)

| Test | Covers |
|------|--------|
| `test_render3d_render` | grid, axes, backdrop, lights, overlays |
| `test_render3d_transition` | the pure grid/axes fade state machine |
| `test_render3d_guides` | geometry guides |
| `test_render3d_palette` | palette data + `render3d_rgba` (header-only target) |
| `test_hidden_lines` | the hidden-line walk's push/pop scoping and which pass runs the program's `glClear` |
| `test_depth_viz` | depth-map conversion core, synthetic buffers, no GL |
| `test_stencil_viz` | stencil scan/map: zero-transparent, deterministic palette, histogram, RAMP over non-zero |
| `test_edit_overlays` | the edit-overlay walkers and the GL state they replay |
| `test_replay_walk` | `replay_walk_user_vertices` invariants the cursor-guide stack rests on |

### 2D UI (pure over snapshots)

| Test | Covers |
|------|--------|
| `test_ui` | the general 2D surface |
| `test_ui_panels` | scene-status panel + code-panel newline rows |
| `test_ui_menu_bar` | menu bar and the shared flyout engine (excluded from the web lane) |
| `test_ui_scene_tabs` | tab derivation, geometry/hit, `band_h` lockstep, double-click rename |
| `test_ui_text_panel` | text panel rendering |
| `test_ui_tabbed_overlay` | modal tabbed overlay geometry + the help over-scroll clamp regression |
| `test_ui_status_history` | 16-entry message ring + messages-button hit-test |
| `test_ui_theme` | theme palette integrity (header-only target) |
| `test_overlay_layout` | the floating-panel layout solver and its eased positions |
| `test_buffer_viz_legend` | legend row selection (stencil top-N by count, call depth ascending) + the panel's pure solve |
| `test_call_depth_viz` | call-depth scan + ramp: binning, observed-range normalization, monotonic warmth |
| `test_ui_gl_state` | **real GL**: `gl2d_begin()`/`gl2d_end()` fully restores what it touches |
| `test_ui_cpuprof`, `test_ui_memprof` | profiler panels |

### Subsystems

| Test | Covers |
|------|--------|
| `test_repl_replay` | replay playback, clamps, fade batches |
| `test_assign_plot` | assignment-value capture: rate gate, both X modes, decimation, ring scroll, target drift |
| `test_ui_assign_plot` | the plot panel renderer + hit-test, views built by hand (proves the renderer calls nothing back) |
| `test_tutorial_runner` | tutorial step kinds, setup scaffolds, teardown |
| `test_tutorial_match` | the step-matching predicate, stub-linked |
| `test_glr_tour_snapshot` | whole-app tour baseline: capture, mutate everything, restore, compare |
| `test_glr_tour_transport` | the controlled-tour transport state machine (pause/step/backstep/seek) |
| `test_tour_overlay_feedback` | **real GL**: tour overlay/HUD geometry via `GL_FEEDBACK` |
| `test_scene_underwater_fill_gl` | **real GL**: the OCEAN underwater fill regression |
| `test_attrib_bits_gl` | **real GL oracle**: the cell→bit table vs the driver |
| `test_gl_state_inspector_gl` | **real GL oracle**: the state fold vs the driver |

### Support modules

| Test | Covers |
|------|--------|
| `test_memprof` | `src/support/memprof.c` (links memprof.o only) |
| `test_gpuprof` | the GPU timer-query profiler, driven by scripted fake queries — no GL |

## Adding a test

1. Drop `tests/test_<name>.c` next to its peers, include
   [`support/test_harness.h`](support/test_harness.h), assert through
   `TEST_ASSERT_TRUE` / `_INT` / `_STR` / `_FLOAT`, and exit non-zero on
   failure — most binaries here end with `test_harness_report()`.
2. Add it to `TEST_BINS` in the Makefile with its `_OBJS` / `_LDLIBS` (and
   `_RUN` if it needs arguments). Link the narrowest object set that works —
   several tests here deliberately link one `.o`, or none, to prove a module
   has no dependencies.
3. If it needs a real GL context, put it in `GL_TEST_BINS` instead, and name it
   `*_gl.c`.
4. If it calls a GL/GLU/GLUT symbol the stubs lack, extend the matching header
   under `gl-stubs/include/`, then verify `make test-stubs`,
   `make gl-repl USE_GL_STUBS=1`, and `make gl-repl`.
5. Never call `repl_save_default_output()` — it writes `./output.c` into the
   repo root, and `check-no-test-default-output` fails the build for it.
