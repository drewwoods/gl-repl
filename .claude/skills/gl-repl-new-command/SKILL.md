---
name: gl-repl-new-command
description: Add a new GL/GLUT command (or REPL primitive) to the gl-repl language - the CmdType → parser → executor → flatten → command_spec checklist, enum-slot kinds, and the tests that must be updated. Use when asked to "add glFoo", "support glSomething", extend the supported-command set, or when touching CmdType / k_enum_command_specs / k_std_command_specs.
---

# Adding a GL command to the REPL

Five edits, all required. Miss one and the command parses but does nothing, or
executes but never survives a reflatten.

## 1. `CmdType` enum - `src/repl/command.h`

Add the enumerator. `GLCmd` is a pure parse result - no `source[]` field; the
canonical per-line text lives in the editor buffer (`EditorState`), not on the
command.

If the command belongs to an existing control-flow family, make sure the inline
predicates in `command.h` (`repl_cmd_is_transform`, `repl_cmd_emits_vertex`,
`repl_cmd_is_block_head` / `_end`) classify it. Never write ad-hoc `||` chains
at call sites. `tests/test_replay_walk.c` has a drift test asserting the
control-flow taxonomy agrees with the visual `CmdSyntaxCategory` one - the two
taxonomies are separate and must not be folded through each other.

## 2. Parser - `repl_parser_parse_command_ctx()` in `src/repl/parser.c`

Parse args into `GLCmd.args[]`. **Enum args live in `args[]` - there is no
`GLCmd.mode` field, and its absence is the invariant.**

## 3. Executor - `repl_execute_program()` in `src/repl/executor.c`

Emit the actual GL call.

## 4. `flatten_range()` - static in `src/repl/flatten.c`

Without this the command never reaches the flat array, so it renders once and
vanishes on the next reflatten. Animation is reflatten-per-frame, not
execute-time re-eval.

## 5. Command spec tables - `src/repl/command_spec.c`

- `g_command_type_specs[]` - one entry, with the right `CmdSyntaxCategory`
  (the *visual* taxonomy: how the editor colors and groups it).
- Enum-arg commands → append to `k_enum_command_specs[]`.
- Float-arg commands → append to `k_std_command_specs[]`.

**Keep both tables alphabetical by GL name.** This is a standing convention, not
a suggestion.

### Per-slot enum kinds - `ReplEnumSlotKind`

| Kind | Use |
|---|---|
| `ENUM_ONLY` | default |
| `ENUM_OR_CONST_VALUE` | bool masks; 0/1 reverse-mapped to `GL_TRUE`/`GL_FALSE` |
| `ENUM_OR_EXPR` | only `glLightModeli` slot 1 - don't add more without a reason |

### Bitfield slots

`glClear` and `glPushAttrib` are the ENUM_BITFIELD cases: `|`-joined tokens
only, emitted in table order, deduped. If your new command takes a mask, follow
that precedent rather than inventing a parse.

## Parsing gotcha

Splitting comma-separated call args goes through `repl_scan_next_arg_delim()`
(`src/repl/eval.h`) - **never** bare `strchr(s, ',')`. Bare strchr is
paren-naive and truncates `cos(i + phase)`.

## Flat-shorthand canonicalization

If the command takes a compound literal (`(GLfloat[]){...}`), decide whether it
also accepts flat args. `glMaterialfv`, `glFogfv`, and `glClipPlane` accept
flat form (`face, pname, r, g, b, a`) and are rewritten to compound-literal
form at parse time. `glDepthMask`/`glColorMask` accept 0/1, canonicalized to
`GL_TRUE`/`GL_FALSE`.

## Export / import round-trip

`src/repl/export.c` writes standalone C; `src/repl/import.c` reverses it
line-by-line through `editor_feed_line()`. The `IMPORT_EXPORT_STATE` macro
block is deliberately duplicated verbatim across the two TUs - keep them in
sync. `tests/test_repl_export_all_commands.c` covers every command through the
export/import round-trip; add your command there.

## Commit-path gotcha

Two commit paths that differ:

- **Interactive `;`** - the input buffer does *not* contain the `;`. Handlers
  must accept input without a trailing semicolon.
- **`editor_feed_line()`** (file/example loading) - copies the full line
  *including* `;`.

`editor_load_line_to_input()` strips the trailing `;`, so re-committing an
existing line takes the no-semicolon path. Any handler checking for `;` must
also accept end-of-string.

## Verify

```bash
make test                       # ASan + UBSan
make test_repl_core_parse
make check-c99                  # everything is -std=c99, no exceptions
make check-state-ownership      # full guard suite
```

If the command calls a GL/GLU/GLUT symbol not yet used in the tree, extend the
matching stub header under `tests/gl-stubs/include/` (minimal no-op), then
verify all three: `make test-stubs`, `make gl-repl USE_GL_STUBS=1`,
`make gl-repl`.
