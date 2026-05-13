# Plan: Editor as a Generic Text Editor

## Status

Not started. Branch: `feature/source-tree-reorg`.

## Load-Bearing Contract

The editor is a text editor. It owns text, text-editing features (cursor,
selection, scroll, search, autocomplete UI, clipboard, undo, comment
toggle, inline rename, etc.), and a small commit boundary. It does NOT
interpret the language being edited.

Before any committed text change lands in the REPL command store, the
editor calls a validation callback (today: `EditorServices.compile`) and
the REPL parser/compile decides what the line means. The editor surfaces
the resulting `ReplCompiledChange` outcome — success message, diagnostic,
or capacity rejection — but does not construct, inspect, or pre-validate
the parse result.

This is the "longer code" path: prefer round-tripping through the
validation callback over inlining grammar checks, even when an inline
shortcut would be faster or smaller. Concept isolation wins over line
count.

## What "doing REPL work in the editor" looks like

Forbidden patterns in editor files (everything outside
`editor_commit.{c,h}` and `editor_services.{c,h}`):

1. Reading `cmd->type` or comparing against `CMD_*` enum values.
2. Constructing a `GLCmd` directly (setting `type`, `args`, `num_args`,
   etc.).
3. Reading or writing `g_predef_vars[]` directly.
4. Walking the document `cmds[]` array looking for specific kinds.
5. Calling `repl_eval_*` functions (the eval table is REPL grammar).
6. Calling `repl_parser_*` directly (parser is REPL's job; the editor's
   commit boundary calls `services.compile`, not the parser).
7. Calling `repl_command_store_*` mutators directly (must go through
   the apply seam).

Allowed editor reads/writes:
- Text content via `editor_buffer_view()` / `editor_buffer_replace_line()`.
- Validation callbacks via `EditorServices`.
- Configuration providers (completion, syntax rules, comment prefix,
  color scheme).
- REPL-side query helpers documented as part of the editor seam (e.g.,
  block-extent and decl-presence queries from the clipboard plan).

## Inventory of remaining violations

After landing #1 (autocomplete rename) and #2 (delete-range compile),
the following editor files still do REPL work:

### V1 — Comment-toggle handler bypasses every guardrail

`editor_input.c::handle_comment_toggle_key_route` (lines 1006–1156)
is the worst remaining offender. The single function:

- Reads `cur->type == CMD_COMMENT` (line 1012) and `cur->type != CMD_FOR_BEGIN
  && cur->type != CMD_FOR_END` (line 1129) — direct grammar reads.
- Builds a `GLCmd` with `commented.type = CMD_COMMENT` (line 1139) — direct
  GLCmd construction.
- On uncomment fallback, reaches into `repl_eval_find_predef_var_idx`,
  `repl_eval_validate_expression_idents`, `repl_eval_input_has_predef_vars`,
  `repl_eval_expr` (lines 1068–1100) — inline grammar evaluation.
- Sets `new_cmd.type = CMD_VAR_ASSIGN` (line 1096) and writes
  `g_predef_vars[var_idx].value = val` (line 1106) — direct cmd construction
  + direct predef-table write.
- Calls `repl_command_store_replace_one` directly (lines 1118, 1144) —
  bypasses the compile/apply seam entirely.
- Hardcodes `// ` (lines 1017–1020 strip; line 1138 prepend) — should be
  config.

### V2 — Clipboard (separate plan)

`editor_clipboard.c` and `editor_state.h`. Already covered in
`done/repl-agnostic-clipboard.md`.

### V3 — Replay annotations consumer

`editor_code_panel_document.c` includes `repl_replay_annotations.h`. The
editor-owns-text-completion plan landed virtual-line snapshots; verify
this consumer reads through `EditorVirtualLineList` rather than calling
the annotation API directly.

### V4 — Inline scene rename

`editor_inline_rename.c` includes `repl_core.h`. Likely fine (rename is
a user-scene API call, not a parser call), but warrants a quick read
to confirm.

### V5 — Search reads document_count

`editor_search.c` reads `repl_state_document_count()` for line counts.
Cosmetic — should read `editor_buffer_view().line_count`. No behavior
change.

### Out-of-scope (deferred)

Cursor migration (`edit_line_idx` canonical on EditorState), broader
`repl_state_document_count()` reads in `editor_input.c`, and the
two-ring undo split are bigger structural moves. They don't block the
contract — the contract is "editor doesn't interpret REPL grammar",
and that's met once V1–V5 are closed.

## Phases

### Phase 1 — Comment-prefix config seam (~2 hr)

Add a configurable comment prefix:

```c
// editor_state.h
void        editor_set_line_comment_prefix(const char *prefix);
const char *editor_line_comment_prefix(void);
```

Storage in `editor_state.c`. Default unset; the comment-toggle key is
a no-op until the controller registers a prefix. The controller
(`imrepl_ctrl.c`, sibling of `repl_autocomplete_register_provider()`)
calls `editor_set_line_comment_prefix("// ")` at startup explicitly.

No behavior change yet — Phase 2 consumes the config.

### Phase 2 — Add `repl_compile_toggle_comment` (~1 day)

The REPL fully owns toggle semantics. Editor expresses intent (line
index + configured prefix); the compile entry decides what to do —
single-line toggle, comment-strip-and-reparse, or block-batch toggle
for structural heads.

Signature (declared in `repl_compile.h`, body in `repl_compile.c`):

```c
ReplCompileResult repl_compile_toggle_comment(int line_idx,
                                              const char *prefix,
                                              const ReplCompileContext *ctx,
                                              ReplCompiledChange *out,
                                              char *err, int err_size);
```

Body decides per the cmd at `line_idx`:

- **Plain non-comment line** (anything except CMD_COMMENT and
  structural block heads/ends): prepend `prefix` after leading
  whitespace; build `text[0]` accordingly. Return REPLACE_ONE at
  `line_idx` with `cmds[0]` set to CMD_COMMENT. Set `commit_message`
  to `"Commented out 1 line"`.
- **CMD_COMMENT line**: strip `prefix` (after leading whitespace);
  call `repl_compile_dispatch` on the stripped text to parse what the
  line used to be. Coerce the result to REPLACE_ONE at `line_idx`
  (override kind/pos/count, preserve `cmds[0]`, `text[0]`,
  `predef_ops`, `scratch_ops`). Pass through compile errors as
  REPL_COMPILE_ERROR. Set `commit_message` to `"Uncommented 1 line"`.
- **Block head** (CMD_FOR_BEGIN, CMD_FUNC_DEF, CMD_IF_BEGIN) **or
  end** (CMD_FOR_END, CMD_FUNC_END, CMD_IF_END): walk to the matching
  end via `repl_source_scope_find_block_end` (from the head) or scan
  backward (from the end). For each line in [head..end] inclusive,
  build prefix-toggled text and a CMD_COMMENT cmd. Return INSERT_MANY
  at `head` with `delete_pos = head, delete_count = end - head + 1`
  (uses the existing combined-shape support on `ReplCompiledChange`).
  Set `commit_message` to `"Commented out N lines"` /
  `"Uncommented N lines"`. Predef ops aggregated from any
  DECLARE/UNDECLARE the uncomment path produces.

Pure: never mutates state, never calls set_status. Returns
REPL_COMPILE_OK on success, REPL_COMPILE_ERROR with `err` filled on
parse failure during uncomment.

### Phase 3 — Rewrite `handle_comment_toggle_key_route` (~2 hr)

Replace the body (lines 1006–1156) with ~25 lines:

```c
static int handle_comment_toggle_key_route(unsigned char key) {
    if (key != '/' || !(editor_get_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;
    const char *prefix = editor_line_comment_prefix();
    if (!prefix || !prefix[0]) return 1;
    if (editor_insert_mode()) return 1;

    int line = repl_state_edit_line();
    if (line >= repl_state_document_count()) return 1;

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX] = "";

    if (repl_compile_toggle_comment(line, prefix, &ctx, &change,
                                    err, sizeof err) != REPL_COMPILE_OK) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof msg, "Toggle failed: %s",
                 err[0] ? err : "not a valid command");
        set_status(msg);
        return 1;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE) return 1;

    if (!editor_commit_apply_external_change(&change, /*capture_undo=*/1)) {
        set_status("Command buffer error");
        return 1;
    }
    set_status(change.commit_message);
    return 1;
}
```

Delete:
- The `cur->type == CMD_COMMENT` / `!= CMD_FOR_BEGIN` checks.
- The uncomment-fallback inline `CMD_VAR_ASSIGN` build.
- The inline `commented.type = CMD_COMMENT` construction.
- The direct `repl_command_store_replace_one` calls.
- The `g_predef_vars[var_idx].value = val` write.
- The `repl_eval_*` direct calls.

### Phase 4 — Verification

```bash
make test                       # 28 / 3382 stay green
make check-state-ownership

# Editor input no longer touches REPL grammar:
grep -nE "CMD_VAR_ASSIGN|CMD_VAR_DECLARE|CMD_COMMENT|CMD_FOR_BEGIN|CMD_FOR_END" editor_input.c
# Expected: 0 hits  (down from 5 today)

grep -n "g_predef_vars\|repl_eval_\|repl_parser_" editor_input.c
# Expected: 0 hits in dispatch paths

grep -n "repl_command_store_" editor_input.c
# Expected: only via editor_commit_apply_* — no direct mutator calls
```

Manual smoke:
- Comment then uncomment `glColor3f(1, 0, 0);` — round-trips, status
  shows compile-supplied commit_message.
- Comment then uncomment `x = 5;` — round-trips through var-assign
  compile path. No special fallback.
- Comment then uncomment `float x;` — round-trips through float-decl
  compile.
- Toggle on `for (i, 0, 10) {` — comments the entire block. Toggle
  again on the now-comment line — uncomments the entire block.
- Toggle on FOR_END / FUNC_END / IF_END — same block-batch behavior.
- Pre-Phase-1 (controller hasn't called
  `editor_set_line_comment_prefix`): Ctrl-/ is a no-op, no status
  change.

### Phase 5 — Replay-annotation consumer audit (~1 hr)

Read `editor_code_panel_document.c`. If it calls
`repl_replay_annotations_*` directly, route those reads through
`EditorVirtualLineList` (per editor-owns-text-completion Phase 6).
If it already does, drop the `#include "repl_replay_annotations.h"`
if unused.

### Phase 6 — Inline rename audit (~1 hr)  [LANDED 2026-05-08]

Read `editor_inline_rename.c`. If it touches anything beyond
user-scene rename APIs (text + status + scene name change), document
and queue.

**Audit result: clean, no code changes needed.** The file's only
REPL calls are coarse scene-management ops on the user-scene API
(`repl_user_scene_slot_used`, `repl_user_scene_name`,
`repl_user_scene_rename`). Zero grammar reads (no `CMD_*`, `GLCmd`,
parser/eval/compile/store-mutator touches). Status messages are
editor-framed for the input session (`"Renamed to: %s"`,
`"Scene name cannot be empty"`). Scene rename mutates metadata not
the command store, so the compile/apply seam doesn't apply here —
the direct user-scene API call is the right shape.

### Phase 7 — Search line-count source (~30 min)

Replace `repl_state_document_count()` reads in `editor_search.c` with
`editor_buffer_view().line_count`. Cosmetic; behavior-preserving.

## Behavior Changes

Phase 2 changes one user-visible behavior: **block heads (FOR_BEGIN,
FOR_END, FUNC_DEF, FUNC_END, IF_BEGIN, IF_END) become toggleable, and
toggling them batch-toggles the entire block**. Today the editor
refuses (the inline `cur->type != CMD_FOR_BEGIN && cur->type !=
CMD_FOR_END` check at line 1129).

This is strictly nicer UX (Ctrl-/ on a `for` header comments out the
whole block in one stroke) and falls out naturally from giving the
REPL full ownership of toggle semantics. If preserving "refuse
structural" is preferred, `repl_compile_toggle_comment` returns
REPL_COMPILE_ERROR with `"Cannot toggle on structural line"` for
block heads/ends instead of doing the batch — same shape, less code
in the body, no editor-side change.

## Open Questions

All four prior questions resolved during plan review:

1. ~~**REPLACE_ONE forcing.**~~ Retired. The compile entry produces
   the change shape directly; the editor applies whatever kind comes
   back. No coercion helper needed.
2. ~~**Structural-kind query location.**~~ Retired. The compile entry
   handles each cmd type internally; no separate query exists.
3. ~~**Default comment prefix.**~~ Resolved: editor default is unset;
   the controller calls `editor_set_line_comment_prefix("// ")` at
   startup explicitly.
4. ~~**Status string preservation.**~~ Resolved: REPL fills
   `change.commit_message` for success and `err` for failure; editor
   uses `commit_message` verbatim for success, frames
   `"Toggle failed: <err>"` for failure. Existing tests asserting on
   `"Cannot uncomment: not a valid command"`,
   `"undeclared variable '%s' - use 'float %s;' first"`, and similar
   inline diagnostics will need updates to match the compile-side
   wording.

One new question:

5. **Block-batch as default vs. error.** Phase 2 makes block-head
   toggle batch the entire block. If the project prefers the existing
   "refuse structural" behavior, the compile entry returns an error
   instead. **Lean**: batch-toggle (better UX, falls out of the
   design, no extra editor knowledge required).

## Critical Files

| File | Phase | Change |
|---|---|---|
| `editor_state.h` | 1 | Declare `editor_set_line_comment_prefix` and `editor_line_comment_prefix`. |
| `editor_state.c` | 1 | Storage and accessors. |
| `imrepl_ctrl.c` | 1 | Register `"// "` at startup, sibling of `repl_autocomplete_register_provider()`. |
| `repl_compile.h` | 2 | Declare `repl_compile_toggle_comment`. |
| `repl_compile.c` | 2 | Body — handles plain / comment / block cases internally; uses existing `repl_source_scope_find_block_end` for block extent and `repl_compile_dispatch` for re-parsing the stripped text. |
| `editor_input.c` | 3 | Rewrite `handle_comment_toggle_key_route` (~25 lines); delete inline GLCmd construction, inline eval-table touches, direct cmd-store mutator calls. |
| `editor_code_panel_document.c` | 5 | Audit + migrate any direct annotation reads. |
| `editor_inline_rename.c` | 6 | Audit. |
| `editor_search.c` | 7 | Use `editor_buffer_view().line_count`. |

## Dependency Order

```
Phase 1 (comment-prefix config) ── independent
   ↓
Phase 2 (repl_compile_toggle_comment)
   ↓
Phase 3 (editor handler rewrite, depends on 1 + 2)
   ↓
Phase 4 (verification)
   ↓
Phases 5, 6, 7 (audits/cleanups; can land in any order, in parallel)
```

## Relationship to existing plans

- `done/repl-agnostic-clipboard.md` (V2 of this inventory) is a
  sibling, not a dependency. Both can land in parallel; together they
  retire the bulk of `editor_*.c` direct REPL-grammar reads.
- `done/predef-var-compaction-on-apply.md` is the precedent: pure
  compile entry + editor apply orchestration. Phase 2 here follows
  the same shape, with the addition that the compile entry can
  produce multi-line changes (block-batch).
- `done/editor-owns-text-completion-revised.md` is the parent
  contract; this plan is the operational closure of remaining gaps.
