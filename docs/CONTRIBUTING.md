# Contributing (Draft)

Thanks for looking under the hood. Start with [`MODULES.md`](MODULES.md) -
the one-page map of the source tree and who owns what - and skim
[`ARCHITECTURE.md`](ARCHITECTURE.md) when you need the deep version.

## Build & test

```bash
make gl-repl          # main binary (macOS needs cmake for the vendored freeglut;
                      #  Linux: apt install freeglut3-dev)
make test             # run the portable headless test gate (checks + GL stubs)
make test-msan        # run stubbed tests with MemorySanitizer, if supported
make debug-msan       # build everything with MemorySanitizer, if supported
make test-stubs       # the suite against bundled no-op GL headers - no GL
                      #  dev packages needed, works headless
make check-c99        # the C99 ratchet (gcc -std=c99 syntax check)
make check-state-ownership   # the full ownership / contract guard suite
```

Test binaries default to `BUILD=debug`; when enabled, its sanitizer mode is
AddressSanitizer + UndefinedBehaviorSanitizer (UB aborts). The broad
`make test` / `make test-stubs` suite defaults to `NO_SAN=1` to stay quick.
`make test NO_SAN=0` enables ASan + UBSan for that stub suite. `make
debug-msan` builds the full target set with MemorySanitizer and origin
tracking when the compiler/runtime support it; `make test-msan` runs the
stubbed suite the same way, and is included by `make test-full`. The MSan
targets default to `MSAN_CC=clang`; override that if your LLVM compiler
has a versioned name. `test-msan` also sets `GLR_AUDIO_NO_DEVICE=1` so
the audio tests exercise the engine without opening host audio backends.
`make test BUILD=release` is the fast release-mode run. `make gl-repl` is the
production OpenGL compile/link smoke check, while `make gl-tests` runs the
small set of tests that require an actual GL context.

### Fidelity to OpenGL

Two things gl-repl shows you are answers *about* OpenGL that it works out
itself, without asking the driver: the state inspector re-implements the
parts of the GL state machine a program can reach - matrix composition, the
lighting equation, `glPushAttrib` group semantics - and attribute scope
decides which state each mask bit covers. Both are claims about the spec, so
both are tested as claims rather than against fixtures. `make gl-tests` runs
them as **differential oracles against a live driver**:

- `test_gl_state_inspector_gl` - one `GLCmd` program is driven through *both*
  the real executor (against a real context) and the pure state model; then
  every row the inspector reports is read back with `glGet*` and compared.
- `test_attrib_bits_gl` - for each `glPushAttrib` bit, both directions are
  asserted: state the table says the bit covers is restored by `glPopAttrib`,
  and state it says the bit does *not* cover is not - so the mapping can be
  neither too narrow nor too broad.

Reach for that pattern whenever a pure module re-implements GL semantics.

Running those checks on more than one driver can also expose differences
between driver implementations. The differences found in this comparison all
affect `GL_CURRENT_RASTER_COLOR`: the color `label()` bitmap text is drawn
with, latched at the `glRasterPos` and unchangeable by a later `glColor3f`.
Three tested driver configurations produced these results:

| Deviation | Apple M2 (2.1 Metal) | Mesa 25.2.8 (Intel) | NVIDIA 595.84 |
|---|---|---|---|
| `GL_COLOR_MATERIAL` enabled at the `glRasterPos` call | tracked components light as **zero** | correct | correct |
| Raster position lit from its **object-space** position - wrong light vector under the tested transformed modelview | correct | **wrong** | correct |
| `GL_NORMALIZE` honoured on that path | correct | **ignored** | correct |
| Unlit latch clamped to [0, 1] (GL 2.1 §2.14.6) | clamps | stores **raw** | clamps |

On each row gl-repl follows the specification. The driver-specific behavior
was characterized by matching the observed values to six decimals, and those
values are recorded in the test beside the corresponding skip gate. A skip
prints in the test output, so it remains visible when a driver-specific case
is not compared.

The one that bites users is Apple's: a scene that enables `GL_COLOR_MATERIAL`
before its `glRasterPos` gets **black label text**. The workaround lives
under [`label()`](USER_GUIDE.md#bitmap-text---label) in the user guide.

When a bug turns out to be the driver's, it gets reduced to a standalone
program that depends on nothing here and written up with its spec citation in
[`third_party/bugs/`](../third_party/bugs/). The four deviations above are
three reports there (Mesa's two raster-lighting symptoms share one). The two
lighting reproducers check the driver against *itself*: they compare the
raster colour with the colour that same driver gives a vertex under identical
state, which the specification defines as the same computation. The
unclamped-colour probe instead checks the specified [0, 1] range and includes
the lit path as a control. A fourth report was found from a scene rendering
differently on Mesa: retargeting `glColorMaterial` to another face discards
the colour the outgoing face was tracking, turning the *glr-logo* example's
exterior black on the tested Mesa configurations.

The same standard applies to export. The C that `Ctrl+S` writes is compiled
and *run* in the test suite, and its GL call stream is compared against the
REPL executor's - call for call and **argument for argument**, over several
values of `t` run as successive frames. A frozen vertex or a drifted matrix
cell fails the build; what you see in the REPL is what the standalone program
draws.

[`tests/README.md`](../tests/README.md) maps every binary in the suite and
highlights the ones with their own `--help` - `test_export_trace_parity`,
`test_repl_core_examples`, `test_eval` - which are debugging instruments, not
just regression gates.

`make web` (or `scripts/build-web.sh` for a cold start with no emsdk
sourced yet) builds the Emscripten/wasm browser target; see
[`packaging/web/README.md`](../packaging/web/README.md).

## The guard suite

CI-grade checks live in the Makefile, and a PR is expected to keep them
green:

- **`make check-c99`** - the whole project compiles `-std=c99`,
  project-wide, no exceptions. The target is old machines and old GCC;
  GNU extensions GCC accepts under `-std=c99` are fine.
- **`make check-state-ownership`** - an inventory of boundary guards:
  module isolation, mutator placement, UI purity, include style
  (project-local headers use `"quotes"`, system/vendored use `<angle>`),
  keymap duplicate detection, and more. Run it before pushing; each
  failing guard prints what rule it enforces.
- **No trailing whitespace** - enforced on commits since `origin/main`
  (also wired into `test-stubs` and the pre-push hook).

Portability conventions the guards can't fully machine-check: use
`STATIC_ASSERT(expr, msg)` from [`include/c_compat.h`](../include/c_compat.h) (never raw
`_Static_assert`), plain `, __VA_ARGS__` (no GNU `##`), and prototyped
function-pointer typedefs (no old-style `void (*)()`).

## Where code goes

Prefixes express ownership: `repl_*` for the language/program model,
`editor_*` for the text document, `glr_*` for the app shell/controller,
`render3d_*` for 3D rendering, `ui_*` for 2D rendering. Runtime state crosses
module boundaries only through the typed facades ([`src/repl/state.h`](../src/repl/state.h),
[`src/editor/state.h`](../src/editor/state.h), peer-subsystem accessors). When in doubt,
[`MODULES.md`](MODULES.md) has a "Where To Put New Code" section.

## Extending the REPL

The most common contributions, recipe-style:

**A new GL command**
1. Add a [`CmdType`](../src/repl/command.h#L48) to [`src/repl/command.h`](../src/repl/command.h).
2. Parse it in [`repl_parser_parse_command_ctx()`](../src/repl/parser.h#L105) ([`src/repl/parser.c`](../src/repl/parser.c)) -
   for a `glEnable`-shaped enum-arg command or a standard float-arg
   command, you only need a new row in `k_enum_command_specs[]` /
   `k_std_command_specs[]` in [`src/repl/command_spec.c`](../src/repl/command_spec.c) (keep the tables
   alphabetically sorted by GL name).
3. Execute it in [`repl_execute_program()`](../src/repl/executor.h#L286) ([`src/repl/executor.c`](../src/repl/executor.c)) and
   handle it in `flatten_range()` ([`src/repl/flatten.c`](../src/repl/flatten.c)).
4. Add a `g_command_type_specs[]` entry in [`src/repl/command_spec.c`](../src/repl/command_spec.c) with
   the right [`CmdSyntaxCategory`](../src/repl/command_spec.h#L152) for syntax highlighting.
5. If it's a new GL/GLU/GLUT symbol, extend the matching stub header
   under `tests/gl-stubs/include/` and verify both `make test-stubs` and
   `make gl-repl`.
6. If it affects **clearing** - the clear color, which channels a clear
   writes, or which clears execute - update cursor execution *once*, in
   [`src/repl/executor.c`](../src/repl/executor.c), and let the background
   observation fall out of it. Emission and observation are paired inside
   `repl_exec_cursor_emit_clear_color()` / `_emit_clear()` precisely so they
   cannot drift; a specialized pass drives those instead of calling GL itself.
   Do **not** add an analyzer-side execution path that predicts the frame's
   background - a second walk with its own program counter and attribute
   stack is what this design removed, after it needed two semantic
   corrections its own tests did not catch.
   [`src/repl/gl_state_inspector.c`](../src/repl/gl_state_inspector.c) is not
   that path and keeps its own fold: it answers what `GL_COLOR_CLEAR_VALUE` is
   *attributed to* at a source position, not which color a clear wrote into
   framebuffer channels, so a clear-affecting command touches it too.

**A new config toggle** - append a [`GlrConfigItem`](../src/app/glr_config.h#L106)
descriptor to `g_cfg_items[]` in [`src/app/glr_actions.c`](../src/app/glr_actions.c)
under the right `### ` section, including an explicit stable lowercase `.slug`.
The count auto-computes and the item joins its section's flyout menu. Use
`make config-list` to inspect slugs and `make check-config-slugs` to validate
scene headers.

**A new built-in example** - add a scene under [`examples/scenes/`](../examples/scenes/)
and a matching entry in [`examples/catalog.ini`](../examples/catalog.ini).
Use `.glr` for the short REPL snippet format; use `.c` only when the example
should stay as a full exported/importable C file. `.glr` files may start with
optional leading `// @cfg` lines and a `// camera` block; display name, tags,
and group live in the catalog. Literal colors (`glColor3f` / `glColor4f` /
`glClearColor`) in covered scenes must be anchors of the active palette in
[`accent_palette.h`](../accent_palette.h) (the repo-root single source of truth for the
scene/brand accent family; `make palette-list` prints the anchors, `make
check-palette` enforces the contract - including the brand-mark vocabulary the
logo scene and `docs/images/logo.svg` are held to). Mind the 8192 flat-command
budget (hoist loop-invariant assignments out of loops), then run
`make check-examples-catalog`.
Use `./gl-repl --examples-dir examples --example <name-or-idx>` to test catalog
edits without rebuilding.

**A corner-case scene is not a built-in example.** Scenes that exist to exercise
the parser, flattener, evaluator or GL-state handling live in
[`tests/scenes/stress/`](../tests/scenes/stress/README.md), a runtime
`--examples-dir` catalog that is not compiled in and does not appear in the
Scene menu. It also takes tags outside the built-in `2D` / `3D` / `Polygons` /
`Lines` vocabulary, which is what lets an entry name the corner case it probes.

**A new tutorial** - add a `TutorialStep[]` entry to `g_tutorials[]` in
[`src/repl/tutorials.c`](../src/repl/tutorials.c) with a `.tags` mask; the metadata tests fail if you
forget the tags. Use the catalog macros for the intended step shape:
`STEP_APPEND` / `STEP_AT` for commented command steps, `STEP_CMD` for a
comment-less command taught by ghost text, `STEP_NOTE` for comment-only
acknowledgement, and `STEP_SET` / `STEP_REQUIRE` / `STEP_REQUIRE_VAR` for
state-driven steps. Step arrays terminate only on the pair-NULL sentinel
(`comment == NULL && expected == NULL`), so comment-less command steps are valid
real steps.

**A new keyboard shortcut** - one `#define GLR_<ACTION> <key>, <mods>` pair
in [`keymap.h`](../keymap.h); `make keymap-list` prints current bindings and free slots.

## Design notes

Use `docs/plans/` for long-form design or audit notes when the rationale would be
too large for a commit message or issue. Keep reader-facing docs focused on
the current design; link to a plan only after explaining what extra background
the plan provides.

## Pull requests

- Keep `make test`, `make check-c99`, and `make check-state-ownership`
  green.
- New behavior gets a test next to its peers under `tests/`.
- Match the surrounding code's style; the guards enforce the load-bearing
  parts.
