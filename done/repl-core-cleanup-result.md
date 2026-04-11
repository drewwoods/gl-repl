# REPL Core Cleanup

## Status

Completed.

`repl_core.c` was split into focused sibling translation units:

- `repl_search.c`
- `repl_export.c`
- `repl_editor.c`

The public API in `repl_core.h` stayed unchanged. Shared runtime/UI state still
flows through `sample.h`, and internal cross-module helpers remain in
`repl_core_internal.h`.

## Final Ownership

- `repl_core.c`: parsing, normalization, flattening, execution, replay,
  example/load orchestration, GL initialization.
- `repl_search.c`: search state, row/text helpers, match navigation, search
  keyboard handling.
- `repl_export.c`: scaffold strings, init bootstrap tables, save/load,
  import/export translation, code-panel dump helpers.
- `repl_editor.c`: editor state, undo/redo, selection, clipboard, feed-line
  helper, block commits, GLUT input handlers.

## Verification Baseline

The working baseline is fully green:

- `test_repl_core_search`: `38/38`
- `test_repl_core_parse`: `32/32`
- `test_repl_core_format`: `58/58`
- `test_repl_core_io`: `75/75`
- `test_repl_core_examples`: `184/184`
- `test_repl_core_commit`: `141/141`

## Follow-up Rules

- Keep `repl_core.h` stable unless behavior or public API genuinely changes.
- Prefer `sample.h` for shared runtime/UI state and `repl_core_internal.h` for
  non-public cross-module helpers.
- New refactors should preserve the current green test counts as the contract.
- `ARCHITECTURE.md` is the module-level source of truth for ownership and data
  flow after the split.
