# Plan: Move Predef-Var Compaction Onto `repl_apply`

## Status

Landed 2026-05-08 on `feature/source-tree-reorg`. All four phases shipped
in a single commit; verification numbers matched the plan (28/28 binaries,
3382/3382 tests; `CMD_VAR_*` refs in `editor_input.c` dropped from 5 to 2,
the residual 2 being unrelated comment-toggle code).

## Load-Bearing Contract

`editor_input.c` does not read or interpret REPL command grammar. Delete-range
goes through the same compile/apply transaction as every other source-changing
operation: the editor describes the cut, the REPL compiles a
`ReplCompiledChange`, the controller applies it with full atomicity and undo.

## What's wrong today

`remove_cmd_range_unchecked()` in `editor_input.c:191-233` and
`delete_cmd_range_allowed()` in `editor_input.c:165-189` are the last places
where the editor's input dispatcher reaches directly into REPL command grammar:

- Walks `CMD_VAR_DECLARE` to snapshot variable names before delete.
- After delete, calls `repl_eval_undeclare_predef_var()` then walks every
  `CMD_VAR_ASSIGN` to renumber `num_args > slot`.
- Pre-validates references with `repl_eval_source_uses_ident()` over the
  editor buffer, surfacing `set_status("Cannot remove '%s': still referenced")`
  inline.

This duplicates work already supported by the compile/apply seam:
`repl_apply.c:109-125`'s `repl_apply_predef_ops()` performs the same UNDECLARE
+ cascade, and `REPL_COMPILED_DELETE_RANGE` already exists. There is no
compile entry that fills in the predef ops for a delete range - so editor
code took the shortcut.

## Target shape

```text
delete_cmd_range(start, count, what)
  -> repl_compile_delete_range(start, count, ctx, &change, err, sizeof err)
       Pure. Validates references. Populates UNDECLARE predef ops for any
       CMD_VAR_DECLARE in the range. Sets commit_message.
       Returns REPL_COMPILE_ERROR on still-referenced names.

  on error:
    set_status(err); return.

  on ok + DELETE_RANGE:
    editor_commit_apply_external_change(&change, capture_undo=1)
       (drives undo / preflight / predef apply / editor buffer / cmd store)

  post-effects:
    cursor adjust, load_line_to_input, mark_normals_dirty, clear selection,
    set_status(change.commit_message)
```

After landing, `editor_input.c:165-233` carries no `CMD_VAR_DECLARE` /
`CMD_VAR_ASSIGN` references. (Lines 1102 / 1136 are unrelated comment-toggle
construction work and stay as-is.)

## Phases

### Phase 1 - Capacity bump (~5 min)

`MAX_PREDEF_OPS_PER_COMMIT` is currently `MAX_NAMES_PER_DECL * 2 + 1` = 17.
A worst-case delete-range can UNDECLARE up to `MAX_PREDEF_VARS` = 24 names
(every live predef declared in the deleted range). Bump to `MAX_PREDEF_VARS`
in `repl_compile.h:117`. No behavior change for existing callers; only the new
compile entry uses the extra room.

### Phase 2 - Add `repl_compile_delete_range` (~1 day)

**Signature** (declared in `repl_compile.h`, body in `repl_compile.c`):

```c
ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size);
```

**Body**:

1. Normalize `(start, count)` against `ctx->document_count`. Empty range →
   `REPL_COMPILED_NO_CHANGE`.
2. Reference check: for each `CMD_VAR_DECLARE` in the range, for each name in
   that decl, scan all lines outside the range via
   `editor_buffer_view_line(ctx->text, j)` + `repl_eval_source_uses_ident()`.
   On hit, write `"Cannot remove '%s': still referenced"` into `err` and
   return `REPL_COMPILE_ERROR`.
3. Populate `out->predef_ops[]` with `REPL_PREDEF_OP_UNDECLARE` entries for
   each variable name in each `CMD_VAR_DECLARE` row in the range. Cap at
   `MAX_PREDEF_OPS_PER_COMMIT` (post-Phase-1 value); if the cap would be
   exceeded, return `REPL_COMPILE_ERROR` with diagnostic
   `"Too many declarations in range"`.
4. Set `out->kind = REPL_COMPILED_DELETE_RANGE`, `out->pos = start`,
   `out->count = count`. Format `out->commit_message` as
   `"%s %d line%s"` is **not** done here - the message is caller-provided
   today via the `what` parameter to `delete_cmd_range`. Either thread `what`
   through context, or have the caller overwrite `commit_message` after
   compile. Recommended: **caller overwrites** `commit_message` post-compile,
   since the verb ("Cut" / "Removed") is editor-side framing.
5. Pure: never call `set_status`, never mutate REPL or editor state.

**No new tests yet - wait for Phase 3.**

### Phase 3 - Migrate `remove_cmd_range_unchecked` (~1 day)

Replace the body of `remove_cmd_range_unchecked()` with:

```c
static void remove_cmd_range_unchecked(int start, int count, const char *what) {
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    repl_compiled_change_init(&change);
    char err[REPL_STATUS_TEXT_MAX];
    err[0] = '\0';

    if (repl_compile_delete_range(start, count, &ctx, &change,
                                   err, sizeof err) != REPL_COMPILE_OK) {
        if (err[0]) set_status(err);
        return;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE) return;

    /* Editor framing: verb chosen by caller, not by compile. */
    snprintf(change.commit_message, sizeof change.commit_message,
             "%s %d line%s", what, count, count > 1 ? "s" : "");

    if (!editor_commit_apply_external_change(&change, /*capture_undo=*/1)) {
        set_status("Command buffer error");
        return;
    }

    /* Editor post-effects */
    repl_state_edit_line_set(start);
    if (repl_state_edit_line() > repl_state_document_count())
        repl_state_edit_line_set(repl_state_document_count());
    load_line_to_input(repl_state_edit_line());
    mark_normals_dirty();
    editor_clipboard_clear_selection();
    set_status(change.commit_message);
}
```

Delete `delete_cmd_range_allowed()` in the same commit (logic is in compile).
Update the caller `delete_cmd_range()` (`editor_input.c:235-244`) to drop the
`if (!delete_cmd_range_allowed(...))` early-return - compile now owns that
check.

### Phase 4 - Verification

```bash
make test                      # 28 binaries / 3382 tests must stay green
make check-state-ownership     # all guards green
grep -nE "CMD_VAR_DECLARE|CMD_VAR_ASSIGN" editor_input.c | wc -l
# Expected: 2 hits remaining (lines ~1102, ~1136 - comment-toggle, unrelated)
# Down from: 5 hits today
```

Manual smoke test (build with `make sample USE_GL_STUBS=1` is enough; the test
suite already covers the path):

- Type `float x;` then `x = 5;` then `;` to commit nothing. Cut the
  declaration line - should reject with "still referenced".
- Type `float x;`, `x = 5;`, then move cursor to the assignment, cut. Should
  succeed. Then cut the declaration. Should succeed (no references remain).
- Type `float x;`, `float y;`, `y = 1;`. Select both decl lines and cut.
  Should reject (because `y = 1` references `y`) - same as today.

## Risks

1. **Capacity overflow.** Phase 1 covers the predef cap. The cmd cap
   (`MAX_COMMIT_CMDS = 16`) is unaffected - `DELETE_RANGE` doesn't use
   `cmds[]` storage.

2. **`apply_predef_ops` cascade order vs. cmd-store mutation order.**
   `editor_commit_apply_external_change` calls `apply_predef_ops` before
   `apply_compiled_change`. The cascade walks `repl_state_document_cmds_mut()`
   which still has the deleted range present at that moment. Today's inline
   code does the same (cascades after delete). **Verify**: walk through
   `editor_commit_apply_external_change` to confirm the order matches today's
   semantics. If `apply_predef_ops` runs *before* the delete, the
   `num_args--` loop scans deleted rows too, but those rows are about to be
   removed anyway so it's harmless. The non-harmless case would be if a
   surviving CMD_VAR_ASSIGN's `num_args` was being held by a deleted decl
   that's still in the array at the time of cascade - but undeclare clears
   the slot in `g_predef_vars`, so no surviving cmd can reuse the freed slot
   index in this single-transaction window. Safe.

3. **Diagnostic message wording.** `delete_cmd_range_allowed` says
   `"Cannot remove '%s': still referenced"`; the migrated path keeps the
   same wording (Phase 2 step 2). Test expectations may match this string -
   spot-check `tests/test_repl_editor.c` and similar before landing.

## Critical Files

| File | Change |
|---|---|
| `repl_compile.h` | Bump `MAX_PREDEF_OPS_PER_COMMIT` to `MAX_PREDEF_VARS`; declare `repl_compile_delete_range`. |
| `repl_compile.c` | Add `repl_compile_delete_range` body. |
| `editor_input.c` | Replace `remove_cmd_range_unchecked` body; delete `delete_cmd_range_allowed`; update `delete_cmd_range` to drop the prevalidation call. |

No test file changes expected - the migrated path is behavior-preserving.
