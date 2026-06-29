# File-Backed Built-In Examples

Status: done.

## Summary

Built-in example source now lives in hand-editable scene files under
`examples/scenes/`, while `examples/catalog.ini` owns catalog order, display
names, tag membership, and Scene-menu groups. The examples are still compiled
into the binary through generated C data by default, so `--example`, F12
cycling, app bundles, and tests work without reading `examples/` at runtime.
For authoring, `--examples-dir <dir>` can load the same catalog shape directly
from disk.

Implemented in commit `482d6259` (`Move built-in examples to file-backed
catalog`).

## Final Directory Shape

- `examples/scenes/` contains the authored built-in scene sources.
- `examples/catalog.ini` contains ordered catalog metadata.
- `examples/README.md` contains authoring rules and the shared Dusk palette
  guidance that used to live in `src/repl/examples.c`.
- `build/generated/repl_examples_data.inc` is generated during the build and is
  not committed.

`tools/repl_live_demo/scenes/` remains tool-local live-edit fixture data and is
not used for built-in examples.

## Final Catalog Format

The catalog uses stable section IDs and an explicit required display name:

```ini
[lit-cube]
file = scenes/lit-cube.glr
name = Lit cube
tags = 3D, Polygons
group = Basics

[animated-ring-for-t]
file = scenes/animated-ring-for-t.glr
name = Animated ring (for + t)
tags = 3D, Lines
group = Basics
```

Final rules:

- Section headers are stable catalog IDs, not display names.
- `name` is required and becomes `repl_example_name(idx)`.
- There is no `@cfg name` support and no filename-derived display-name
  fallback.
- Section order is the flat example order used by F12, `--example <idx>`, and
  golden fixture numbering.
- `group` maps to `repl_example_subheading`.
- `tags` uses the existing labels: `2D`, `3D`, `Polygons`, `Lines`.
- `All` remains synthetic and must not appear in the INI.
- `.glr` scene files keep only runtime scene metadata: leading `// @cfg ...`
  presentation lines, optional `// camera`, then REPL source.
- `.c` scene files are complete exported/importable C files and load through
  the same importer as `./gl-repl output.c`.

## Implementation Notes

`src/repl/examples.h` kept the existing query API and added source-format plus
runtime-catalog helpers. `src/repl/examples.c` is now a small query facade that
keeps the tag labels and synthetic `All` behavior, then includes generated
example source arrays and the generated `ReplExampleEntry g_example_entries[]`
table unless `--examples-dir` has installed a runtime catalog.

`scripts/gen_examples.py` uses only Python stdlib. It reads the INI plus scene
files, validates metadata, escapes source lines as C strings, and writes
`build/generated/repl_examples_data.inc`.

The generator validates:

- missing required keys: `file`, `name`, `tags`, `group`
- unknown keys
- unknown tags
- accidental use of synthetic tag `All`
- empty names, tags, groups, or files
- duplicate display names
- duplicate scene files
- generated C symbol collisions
- missing files
- scene paths outside `examples/scenes/`
- scene files whose extension is not `.glr` or `.c`

The Makefile now generates the include before compiling `src/repl/examples.c`,
before `make check-c99`, and before test/build targets that use the examples
object. `make check-examples-catalog` runs the generator in check mode.

`gl-repl --examples-dir <dir>` validates and loads `<dir>/catalog.ini` plus
files under `<dir>/scenes/` at runtime. `.glr` entries use the concise example
loader; `.c` entries use the existing full-C import path.

## Migration Result

All static example arrays were extracted from `src/repl/examples.c` into
`examples/scenes/*.glr`, preserving source lines, comments, blank lines, `@cfg`
presentation settings, and camera blocks. Catalog entries may now also point to
full `.c` sources for examples that should stay in exported-C form.

The former hand-written catalog table was moved into `examples/catalog.ini`.
Lit cube remains example 0 to preserve startup and test assumptions. The
existing Rotating cube scene was kept and catalogued after Lit cube, which is
why UI golden fixture numbering changed from example 1 onward and
`tests/testdata/repl_examples_ui/28.golden.txt` was added.

Docs updated:

- `docs/CONTRIBUTING.md`
- `docs/MODULES.md`
- `docs/ARCHITECTURE.md`
- `src/repl/README.md`
- `src/repl/ARCHITECTURE.md`
- `CLAUDE.md`

Tests updated:

- `tests/test_repl_core_examples.c` now checks catalog metadata integrity and
  covers runtime `--examples-dir`-style catalogs with both `.glr` and `.c`
  entries.
- `tests/testdata/repl_examples_ui/*.golden.txt` was regenerated for the final
  catalog order.

## Validation

Local validation passed:

- `make check-examples-catalog`
- `build/release/test_repl_core_examples`
- `make check-c99`
- `make test-stubs`
- `make test`

Linux / real-GCC validation:

- `make check-c99` passed on `gracemont` using a copied local worktree.
- `make test-stubs` did not complete on `gracemont`: GCC 13 hit an internal
  compiler error in the sanitizer pass while compiling existing `eval` sources.
  An unsanitized retry then hit an existing `glr_audio.c` / miniaudio compile
  issue. Those failures were outside the file-backed example migration.

## Assumptions Confirmed

The requested “group” field maps to the existing Scene-menu subheading returned
by `repl_example_subheading`.

Tags remain the current known enum. Adding future tag categories still requires
updating `examples.h`, tag labels, and the app tag-default policy.

The live demo informed the repository shape. Built-in examples still use the
compiled-in catalog by default, while `--examples-dir` provides an explicit
runtime override for local authoring.
