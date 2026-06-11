# Contributing

Thanks for looking under the hood. Start with [`MODULES.md`](MODULES.md) —
the one-page map of the source tree and who owns what — and skim
[`ARCHITECTURE.md`](ARCHITECTURE.md) when you need the deep version.

## Build & test

```bash
make gl-repl          # main binary (macOS needs cmake for the vendored freeglut;
                      #  Linux: apt install freeglut3-dev)
make test             # build and run all tests (debug: ASan + UBSan)
make test-stubs       # the suite against bundled no-op GL headers — no GL
                      #  dev packages needed, works headless
make check-c99        # the C99 ratchet (gcc -std=c99 syntax check, shipped sources)
make check-state-ownership   # the full ownership / contract guard suite
```

Test targets default to `BUILD=debug`, which compiles with
AddressSanitizer + UndefinedBehaviorSanitizer (UB aborts). `make test
BUILD=release` is the fast unsanitized run.

## The guard suite

CI-grade checks live in the Makefile, and a PR is expected to keep them
green:

- **`make check-c99`** — the whole project compiles `-std=c99`,
  project-wide, no exceptions. The target is old machines and old GCC;
  GNU extensions GCC accepts under `-std=c99` are fine.
- **`make check-state-ownership`** — an inventory of boundary guards:
  module isolation, mutator placement, UI purity, include style
  (project-local headers use `"quotes"`, system/vendored use `<angle>`),
  keymap duplicate detection, and more. Run it before pushing; each
  failing guard prints what rule it enforces.
- **No trailing whitespace** — enforced on commits since `origin/main`
  (also wired into `test-stubs` and the pre-push hook).

Portability conventions the guards can't fully machine-check: use
`STATIC_ASSERT(expr, msg)` from `include/c_compat.h` (never raw
`_Static_assert`), plain `, __VA_ARGS__` (no GNU `##`), and prototyped
function-pointer typedefs (no old-style `void (*)()`).

## Where code goes

Prefixes express ownership: `repl_*` for the language/program model,
`editor_*` for the text document, `glr_*` for the app shell/controller,
`scene_*` for 3D rendering, `ui_*` for 2D rendering. Runtime state crosses
module boundaries only through the typed facades (`src/repl/state.h`,
`src/editor/state.h`, peer-subsystem accessors). When in doubt,
[`MODULES.md`](MODULES.md) has a "Where To Put New Code" section.

## Extending the REPL

The most common contributions, recipe-style:

**A new GL command**
1. Add a `CmdType` to `src/repl/command.h`.
2. Parse it in `repl_parser_parse_command_ctx()` (`src/repl/parser.c`) —
   for a `glEnable`-shaped enum-arg command or a standard float-arg
   command, you only need a new row in `k_enum_command_specs[]` /
   `k_std_command_specs[]` in `src/repl/command_spec.c` (keep the tables
   alphabetically sorted by GL name).
3. Execute it in `repl_execute_program()` (`src/repl/executor.c`) and
   handle it in `flatten_range()` (`src/repl/flatten.c`).
4. Add a `g_command_type_specs[]` entry in `src/repl/command_spec.c` with
   the right `CmdSyntaxCategory` for syntax highlighting.
5. If it's a new GL/GLU/GLUT symbol, extend the matching stub header
   under `tests/gl-stubs/include/` and verify both `make test-stubs` and
   `make gl-repl`.

**A new config toggle** — append a `ReplConfigItem` descriptor to
`g_cfg_items[]` in `src/app/glr_actions.c` under the right `### ` section.
The count auto-computes and the item joins its section's flyout menu.

**A new built-in example** — add it to `src/repl/examples.c` with optional
leading `// @cfg` lines and a `// camera` block. Mind the 4096 flat-command
budget (hoist loop-invariant assignments out of loops).

**A new tutorial** — add a `TutorialStep[]` entry to `g_tutorials[]` in
`src/repl/tutorials.c` with a `.tags` mask; the metadata tests fail if you
forget the tags.

**A new keyboard shortcut** — one `#define GLR_<ACTION> <key>, <mods>` pair
in `keymap.h`; `make keymap-list` prints current bindings and free slots.

## Plans

Long-form design / audit docs live under `plans/`, bucketed by state
(`not-started/`, `in-review/`, `active/`, `partial/`, `done/`). Substantial
features usually start as a plan doc; small fixes don't need one.

## Pull requests

- Keep `make test`, `make check-c99`, and `make check-state-ownership`
  green.
- New behavior gets a test next to its peers under `tests/`.
- Match the surrounding code's style; the guards enforce the load-bearing
  parts.
