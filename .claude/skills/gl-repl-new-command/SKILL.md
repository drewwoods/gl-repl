---
name: gl-repl-new-command
description: Add a new GL/GLUT command (or REPL primitive) to the gl-repl language - where the canonical checklist lives, enum-slot kinds, parse policies, and the tests that must be updated. Use when asked to "add glFoo", "support glSomething", extend the supported-command set, or when touching CmdType / k_enum_command_specs / k_std_command_specs.
---

# Adding a GL command to the REPL

## The checklist is `docs/ARCHITECTURE.md` → *Adding A New Command*

**Read it and work from it.** It is the canonical, numbered, branch-by-shape
list (bound GL/GLU/GLUT command · REPL primitive · math function · structured
syntax), and it is kept current with the code. Do not work from a remembered
summary - including this file's. What follows is only the detail that document
does not need to repeat.

Two shape notes worth having up front, because they are the two most common
wrong turns:

- **A table-driven bound command needs no `parser.c` edit and no
  `flatten_range()` edit.** A `k_std_command_specs[]` / `k_enum_command_specs[]`
  row is what teaches the generic parser the spelling, and `flatten_range()`
  special-cases only control flow, assignments, scratch writes and source-only
  markers - everything else falls through to `flatten_reparse_line`, which
  re-reads the same tables. Neither `a4056e54` (`glVertex4f`) nor `4c693a35`
  (`glPolygonMode`/`glPolygonOffset`) touched either file.
- **Custom parse / lowering work belongs to *structured* syntax only** - a new
  block construct, branch separator, or context-sensitive statement. That path
  is `docs/ARCHITECTURE.md` → *Structured & Control-Flow Command Pipeline*.

## Two required steps that are easy to skip

- **`src/repl/command_descriptions.txt`** - one `[command CMD_*]` section per
  bound GL/GLU/GLUT `CmdType`. `scripts/gen_command_descriptions.py` is
  exhaustive over the enum, so a missing section fails the build.
- **`src/repl/attrib_bits.c` + `src/repl/gl_state_inspector.c`** - required for
  any command that writes attribute-scoped GL state. The inspector's switch is
  `-Werror=switch`-enforced; `attrib_bits` is where the omission is silent, and
  it gates the coverage sweep in `tests/test_repl_state.c` that would otherwise
  catch you. Do `attrib_bits` first.

## `CmdType` classification

If the command belongs to an existing control-flow family, make sure the inline
predicates in `command.h` (`repl_cmd_is_transform`, `repl_cmd_emits_vertex` /
`repl_cmd_emits_immediate_vertex`, `repl_cmd_is_block_head` / `_end`) classify
it. Never write ad-hoc `||` chains at call sites. `tests/test_replay_walk.c`
has a drift test asserting the control-flow taxonomy agrees with the visual
`CmdSyntaxCategory` one - the two taxonomies are separate and must not be
folded through each other.

`GLCmd` is a pure parse result - no `source[]` field; the canonical per-line
text lives in the editor buffer (`EditorState`), not on the command.
**Enum args live in `args[]` - there is no `GLCmd.mode` field, and its absence
is the invariant.**

## Command spec tables - `src/repl/command_spec.c`

**Keep `k_std_command_specs[]` and `k_enum_command_specs[]` alphabetical by GL
name.** This is a standing convention, not a suggestion.

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
line-by-line through `repl_load_apply_line()` (`src/repl/load.h`) - the
compile + apply path shared by file import, catalog load and replace. It is
**not** `editor_feed_line()`; `load.h` is explicit that it replaces it for
non-editor callers. `tests/test_repl_export_all_commands.c` covers every
command through the export/import round-trip; add your command there.

## Commit-path gotcha

Two commit paths that differ:

- **Interactive `;`** - the input buffer does *not* contain the `;`. Handlers
  must accept input without a trailing semicolon.
- **`editor_feed_line()`** (interactive-equivalent line feeding) - copies the
  full line *including* `;`.

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
