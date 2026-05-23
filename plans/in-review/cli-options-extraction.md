# Where should CLI/arg handling live? (decision pending)

Status: **in-review** — competing options, no decision yet. This folder is
for plans whose *direction* is still contested; do not implement until a
direction is chosen and the file is moved to `not-started/` (or dropped).

## 2026-05-23 audit

Current code matches **Option C (status quo)**: `sample.c:96` still
defines `print_usage`; the flag loop and `glr_ctrl_set_program_name(argv[0])`
call sit in `main()` (lines 164, 175+). No `src/app/glr_cli.{c,h}` exists.
No decision recorded, so the file stays in `in-review/`.

## Context

`sample.c::main()` currently does three things: (1) parse CLI flags
(`-h/--help`, `--noaccum`, `--no-audio`, `--dump-code`, `--dump-flat`,
`--dump-state-layout`, positional `input_file`) + `print_usage`,
(2) GLUT/window setup + callback registration, (3) call
`glr_ctrl_bootstrap_repl()`. A recent change added
`glr_ctrl_set_program_name(argv[0])` (basename kept; default `"gl-repl"`)
so the File>Load-Scene message and other UI text can name the binary.

Question raised: should we push `argc/argv` straight into `glr_ctrl`
(or a new `glr_init(argc, argv)`) to move "the glr logic" out of
`sample.c` — which would also subsume `glr_ctrl_set_program_name`?

## Options

### A. Push `argc/argv` into `glr_ctrl` / new `glr_init(argc,argv)`

Pros:
- One entry point owns startup; `sample.c` shrinks.
- `glr_ctrl_set_program_name` disappears (argv[0] captured in `glr_init`).

Cons (decisive):
- **Fights the GLUT-isolation invariant.** `main` also does
  `glutInit(&argc,argv)`, window creation, callback registration. The
  `check-gl-boundaries` guard requires GLUT/GL calls to stay in
  `sample.c`/`executor.c`. GLUT setup *cannot* move into `glr_ctrl`
  without tripping the guard, so argv handling ends up split across two
  places anyway — more seams, not fewer.
- **Layering inversion.** CLAUDE.md: `sample.c` is the thin shell that
  forwards to `glr_ctrl_*`; `glr_ctrl` is the per-frame controller
  (display/reshape/input). CLI parsing + usage text is a startup/shell
  concern, not a frame-controller concern.
- **Multiple entrypoints.** `tools/scene_demo`, `tools/repl_demo`, and
  the test binaries have their own `main`s and must NOT pull CLI
  parsing. `set_program_name`'s `"gl-repl"` default exists precisely for
  these; absorbing CLI into `glr_ctrl` forces every entrypoint to reason
  about it.
- Removes a ~12-line, well-scoped helper at the cost of a large
  structural change — net complexity up.

### B. Extract a CLI/options module (`glr_cli.{c,h}`) — recommended if we act

`int glr_cli_parse(int argc, char **argv, GlrOptions *out)` returns a
parsed struct (`input_file`, `no_audio`, `use_accum`, window size, dump
flags, …) + owns `print_usage`. `sample.c` becomes: parse → struct →
GLUT setup → `glr_ctrl_bootstrap_repl(opts)`.

Pros:
- Centralizes CLI logic; `sample.c` thins out.
- Keeps shell→controller→GLUT layering and the boundary guard intact
  (no GLUT moves; `glr_ctrl` never sees argv).
- Parser is pure → unit-testable headless (current `main` arg handling
  is not).
- Program-name capture lives here naturally, subsuming
  `glr_ctrl_set_program_name` *without* coupling the frame controller
  to argv.
- Incremental, low blast radius.

Cons:
- A new module + `GlrOptions` struct (small) — only worth it if
  `sample.c` size / CLI testability is a real goal, not just to delete
  one helper.

### C. Status quo

Keep `main` parsing inline + `glr_ctrl_set_program_name`. Pros: zero
work, helper is tiny and correct. Cons: CLI logic stays in the shell,
not independently testable; `sample.c` keeps growing if flags multiply.

## Recommendation

**Reject A.** Prefer **C now**, **B when CLI growth/testability justifies
it** (do it as an `glr_cli` options-struct extraction, not a
`glr_ctrl` change). The program-name accessor stays as-is until B is
done, at which point B absorbs it.

## If B is chosen (sketch)

- New `src/app/glr_cli.{c,h}`: `GlrOptions`, `glr_cli_parse`,
  `glr_cli_print_usage`. No GLUT/GL.
- `sample.c`: replace the arg loop with `GlrOptions o; if
  (glr_cli_parse(argc,argv,&o)) return ...;` then use `o`.
- Move `glr_ctrl_set_program_name`'s basename logic into
  `glr_cli_parse` (set via existing accessor or fold the accessor into
  `glr_cli`). Keep the `"gl-repl"` default for non-`main` entrypoints.
- Tests: `tests/test_glr_cli.c` (pure, core) covering flag/positional
  parsing + usage exit codes. Add to `TEST_BINS`.
- Update CLAUDE.md File Layout + MODULES.md.
- Verify `check-gl-boundaries`, `make test`, `make test-stubs`.

## Folder note

Created `plans/in-review/` for contested-direction plans. Lifecycle:
`in-review` → (decision) → `not-started` → `active` → `done`, or
deleted if rejected.
