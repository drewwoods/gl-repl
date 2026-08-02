## Plan: Repository Layout and Test Consolidation

Keep the current root-level source layout for now, but formalize a simple rule set: source-backed modules keep paired `.c/.h` files together at the root, `include/` is reserved for normal-build header-only helpers and vendored single-header dependencies, GL stubs live under `tests/gl-stubs/`, and all test programs move under `tests/` with shared support code in `tests/support/`. This gives the repo consistent ownership boundaries without forcing a risky full `src/` migration.

**Steps**
1. Inventory the current files into three buckets: paired modules, header-only support, and tests. Use the existing prefixes (`repl_`, `scene_`, `ui_`, `imrepl_`) as the first classifier, then confirm the edge cases that currently cause ambiguity, especially `include/gl_2d.h`, `cmd_format.h`, and the GL stub wrappers.
2. Codify a placement rule in the docs: keep compiled modules and their headers adjacent in the repo root; use `include/` only for normal-build header-only utilities and vendored single-header dependencies; put no-op GL stubs under `tests/gl-stubs/`; do not move source-backed headers into `include/` just for symmetry.
3. Create a `tests/` hierarchy and move every `test_*.c` there while preserving executable names. Start with a flat `tests/` directory unless the move becomes hard to navigate, then optionally split by test kind (`unit`, `core`, `render`, `internal`) only if that improves discoverability.
4. Add shared test infrastructure under `tests/support/`:
   - common counters and summary output
   - `ASSERT_TRUE`, `ASSERT_INT`, and `ASSERT_STR`
   - small shared REPL setup/reset helpers used across many tests
   - optional specialized helpers for eval/float-heavy tests
   Keep file-local macros only where a test truly needs custom behavior.
5. Update `Makefile` so `TEST_BINS`, per-test object rules, and `test_*_OBJS` resolve from `tests/` without changing binary names or invocation behavior. Keep the include-as-unit tests supported, but make the special-case linking explicit and colocated with the moved test sources.
6. Update the developer docs (`MODULES.md`, `CLAUDE.md`, and any test note that explains layout) to describe the new layout rule and the intended home for future utilities and tests.
7. Verify with focused tests first, then the full suite. Cover one pure utility test, one GL-free REPL test, one include-as-unit test, and one stubbed GL path if the include split touches header resolution.

**Relevant files**
- `include/gl_2d.h` - current example of header-only utility placement
- `cmd_format.h` / `prof.h` - source-backed utility headers that should stay paired with their `.c` files
- `Makefile` - test binary list, object wiring, and include-as-unit special cases
- `MODULES.md` - existing file-layout overview that should be updated with the new rule
- `CLAUDE.md` - agent-facing repo brief that already describes the current layout
- `tests/test_eval.c` - representative self-contained test
- `tests/test_format.c` - representative pure utility test that should adopt the shared harness
- `tests/test_repl_editor.c` - representative large REPL test with repeated macros and setup helpers
- `tests/test_imrepl_ctrl.c` - representative include-as-unit test that needs explicit build handling

**Verification**
1. Run representative tests after the move: `make test_eval`, `make test_format`, `make test_repl_core_parse`, `make test_repl_editor`, and `make test_imrepl_ctrl`.
2. Run the full suite with `make test`.
3. If include paths or stub headers change, run `make test-stubs`, `make sample USE_GL_STUBS=1`, and `make sample`.

**Decisions**
- Keep the scope to a light cleanup, not a full repository migration into `src/` and `include/`.
- Treat `include/` as the home for normal-build header-only support and vendored single-header dependencies, not as a replacement for root-level module headers or test-only stubs.
- Separate tests physically from implementation, but keep executable names stable so the Makefile and developer workflow do not change more than necessary.

**Further Considerations**
1. The first cut can keep all tests in one flat `tests/` directory; subdirectories are optional and should only be added if they materially help navigation.
2. If the repo later needs external packaging, a second phase can introduce a fuller `src/` + `include/project/` layout without undoing this test split.
