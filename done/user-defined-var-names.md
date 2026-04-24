# Rename-able Predef Vars via `float` Declarations

## Context

The REPL ships with a hardcoded set of eleven single-letter vars (`x, y, z, i, j, k, a, b, c, n, t`) in `g_predef_vars[]`. They can be reassigned but not renamed, so algorithms that would read better with `radius`, `angle`, `count`, `spin`, etc. can't have them. The user wants to declare vars by name (`float a, b, c;`) before use - keeping state low (stay inside `MAX_PREDEF_VARS`), without introducing a heap-allocated symbol table - and have the variable panel show only declared names.

`g_predef_vars[]` is the right home: fixed-size, already name-keyed everywhere it matters (eval, panel, workspace header, C export), already iterated by `g_num_predef_vars`. We start it nearly empty and let the user fill it via a new `float …;` source command that lives in the code panel like any other line.

## Approach

1. **`init_predef_vars()` registers only `t`.** `t` is special (Ctrl+T toggles, C export binds it to `glutGet(GLUT_ELAPSED_TIME)`), so it stays reserved at slot 0 forever. `g_num_predef_vars` starts at 1. Bump `MAX_PREDEF_VARS` from 11 → 16 so users get 15 declarable slots.

2. **New source command `CMD_VAR_DECLARE`**, syntax `float a, b, c;`. Lives in the code panel as a normal line - visible, scrollable, deletable, undoable, copy-paste-able like any other source line. Stores declared names in new `GLCmd` fields `char var_names[MAX_NAMES_PER_DECL][16]; int var_decl_count;` where `MAX_NAMES_PER_DECL = 8`. No-op in `execute_commands()` and `flatten_range()` - registration into `g_predef_vars[]` happens at *commit* time via `declare_predef_var()`.

   **Cap of 8 names per declaration line is a hard limit, not a metadata accident.** With `MAX_PREDEF_VARS = 16` and 15 user slots, a maximally-greedy single line could declare 15 names and overflow the metadata buffer. Two options exist: bump `var_names` to 15 (~4 MB extra BSS across `g_cmds` + `g_flat_cmds`) or reject `float` lines with more than 8 names. **Pick the reject path** - it's simpler, saves the memory, and matches the project's "punt to user when it simplifies the REPL" preference. Status message on overflow: `"too many names per declaration (max 8); split across multiple lines"`. Workaround is a two-second user edit.

3. **New commit handler `try_commit_float_decl()` in `repl_editor.c`.** Matches `^\s*float\s+<ident>(\s*,\s*<ident>)*\s*;\s*$`. For each name calls `declare_predef_var(name, err, errsz)`. On any failure (capacity, reserved, invalid ident, dup) sets a status message and aborts the whole line atomically. On success, emits a `CMD_VAR_DECLARE` source line, sets status `"declared a, b, c"`.

4. **Dispatch order: `try_commit_float_decl` runs *before* `try_assign_variable`** at every dispatch site (`repl_editor.c:1503-1533, 1599-1665, 1803-1819, 2614-2628`, plus `feed_line` at `:2614`). Otherwise `float x;` would be misread as an assignment target.

5. **`CMD_VAR_ASSIGN` keeps its existing index in `cmd.num_args`** (`repl_editor.c:368`). No `var_name[16]` field, no per-frame name resolution. Slot indices are kept stable by the delete handler (step 7) - the only way they ever change is through a `CMD_VAR_DECLARE` deletion, which we guard with an in-use check + post-delete compaction.

6. **Undeclared identifiers are rejected at parse time, everywhere.** Not just on the LHS of an assignment - any expression that references an unknown name must be rejected, otherwise `glVertex3f(radius, 0, 0)` with undeclared `radius` silently evaluates to `glVertex3f(0, 0, 0)` (because `eval_primary` at `repl_eval.c:137` returns `0.0f` for unknown identifiers, and `cmd->has_vars = input_has_any_visible_vars(...)` at `repl_core.c:1625` only flags vars that *are* visible - it never errors on unknown ones).

   New helper `validate_expression_idents(const char *src, const ExprVar *vars, int num_vars, char *err, int errsz)` in `repl_eval.c`. Walks the expression text, tokenizes identifiers, skips constants (`PI`, `TAU`), function calls (identifier followed by `(`), then checks loop locals (`vars[]`) and `g_predef_vars[]`. On the first unknown identifier, writes `"undeclared variable 'foo'"` to `err` and returns 0. Otherwise returns 1.

   Wire it into every commit-time call site that takes an expression string. Bottleneck strategy: every site in `repl_core.c` that calls `parse_exprs` (`:1573, :1621, :1720, :1776, :1896`) gets a `validate_expression_idents()` call immediately before, with the same `vars/num_vars` context. On failure, the parser sets a status message and returns the same way it currently rejects malformed input. Same in `try_assign_variable` (RHS), `try_commit_for_loop` (start/end/step), `try_commit_if_block` (condition), and the function-call argument parser. There is no hot path concern - validation runs once at commit, never during per-frame re-evaluation of `has_vars` commands.

   Status message also covers the simpler case currently handled by `try_assign_variable`: `radius = 1.0;` with no prior `float radius;` → `"undeclared variable 'radius' - use 'float radius;' first"`. The existing silent `return 0` at `repl_editor.c:345-353` becomes a proper status.

7. **Delete is guarded by an in-use check, with compaction on success.** Wrap the existing `delete_cmd_range()` at `repl_editor.c:251` so that, before deleting, it scans the range for any `CMD_VAR_DECLARE` and rejects if any declared name is referenced from a line *outside* the deletion range:

   ```c
   void delete_cmd_range(int start, int count, const char *what) {
       int end = start + count;
       /* Refuse if any var-decl in [start,end) declares a name still used elsewhere. */
       for (int i = start; i < end && i < g_num_cmds; i++) {
           if (g_cmds[i].type != CMD_VAR_DECLARE) continue;
           for (int n = 0; n < g_cmds[i].var_decl_count; n++) {
               const char *name = g_cmds[i].var_names[n];
               for (int j = 0; j < g_num_cmds; j++) {
                   if (j >= start && j < end) continue;       /* skip lines in the delete set */
                   if (source_uses_ident(g_cmds[j].source, name)) {
                       set_status("variable '%s' is in use, cannot delete", name);
                       return;
                   }
               }
           }
       }
       /* Snapshot removed names for post-delete compaction. */
       char removed_names[16][16]; int n_removed = 0;
       for (int i = start; i < end && i < g_num_cmds; i++) {
           if (g_cmds[i].type != CMD_VAR_DECLARE) continue;
           for (int n = 0; n < g_cmds[i].var_decl_count && n_removed < 16; n++)
               strncpy(removed_names[n_removed++], g_cmds[i].var_names[n], 15);
       }
       /* ... existing delete logic ... */
       /* For each removed name: find its slot, shift g_predef_vars down,
        * decrement num_args of CMD_VAR_ASSIGN whose index is past the removed slot. */
       for (int r = 0; r < n_removed; r++)
           undeclare_predef_var(removed_names[r]);
   }
   ```

   The in-use scan uses a new `source_uses_ident(const char *src, const char *name)` helper that does word-boundary identifier matching (mirrors `input_has_predef_vars` at `repl_eval.c:31-44`, but for one specific name). Lines in the deletion set are skipped so you can delete `float a;` *together with* `a = 1;` and the geometry that uses `a` in one Backspace.

   `undeclare_predef_var(name)` does the compaction: locate the slot, `memmove` later slots down by one, decrement `g_num_predef_vars`, and walk `CMD_VAR_ASSIGN` decrementing `num_args` for any assignment whose index was greater than the removed slot. Because the in-use check has already verified no remaining line references the removed name, no assignment can have `num_args == removed_slot`, so the decrement is unambiguous.

8. **Variable panel needs no code change.** `ui_panels.c:3125, 3139, 3185` already iterate `g_num_predef_vars` - when the table only has `t`, only `t` renders. Update the panel title from `"Variables"` to `"Variables (declared)"`. Eyeball geometry at `g_num_predef_vars == 1`.

9. **Save/load.** The existing `// @var name = value` workspace header (`repl_export.c:41-94`) is name-based and round-trips values automatically. Declarations themselves don't need a header line - they live in the source body as `float …;` lines and are saved/loaded by the normal source-command pipeline. **However**, on load, the executor encounters the `CMD_VAR_DECLARE` line and must re-register the names into `g_predef_vars[]`. Easiest path: in the load loop, when `feed_line()` re-feeds a `float …;` line, `try_commit_float_decl` runs and re-registers naturally. Confirm that the workspace-header `// @var` lines are applied *after* the source body has been fed (so the names exist when the values are restored). If the current order is reversed, fix it - or have `// @var` auto-declare a missing name as a fallback.

10. **C export.** `repl_export.c:1320-1340` already iterates `g_num_predef_vars` to emit `static float NAME = 0.0f;` and `reset_repl_vars()`. No change needed. The `CMD_VAR_DECLARE` source lines will appear in the exported `display()` body as `float a, b, c;` C statements - that's valid C inside a function (compiler accepts them as locals), and they shadow nothing because the predef globals have the same names. Verify the C export emits `CMD_VAR_DECLARE` lines verbatim (or just skips them - the globals already exist). Skipping is cleanest; the `display()` body does not need them.

11. **Undo captures the declared-name list.** `UndoSnapshot` at `repl_editor.c:152-157` stores `float predef_vals[MAX_PREDEF_VARS]`. Extend with `char predef_names[MAX_PREDEF_VARS][16]` and `int num_predef_vars`. `snapshot_save`/`snapshot_restore` at `:169-186` copy all three. Without this, undoing a `float foo;` line would leave the slot orphaned; undoing a delete would not restore the slot.

12. **Scene lifecycle restores declared names, not just values.** Three lifecycle entry points reset or replay session state and currently know nothing about declared names:

    - **`UserScene` struct at `repl_core.c:446`** stores `predef_vals[MAX_PREDEF_VARS]` only. After this change, declared *names* are part of session state, so they must be in the snapshot too. Extend with `char predef_names[MAX_PREDEF_VARS][16]` and `int num_predef_vars`. `save_user_scene()` copies all three; `restore_user_scene()` at `repl_core.c:3895-3910` overwrites `g_predef_vars[]` and `g_num_predef_vars` from the snapshot (replacing, not merging - any declarations the example added are discarded), then copies the values.

    - **`load_example_lines()` at `repl_core.c:3913`** clears `g_cmds` but leaves `g_predef_vars[]` populated with whatever the previous session declared. After this change, switching from a user scene with `float radius, angle;` into an example would leave `radius` and `angle` in the table on top of the example's own declarations. Reset the predef table to "just `t`" at the top of `load_example_lines()` before calling `feed_line()` for each example line - the example's own `float …;` lines will repopulate the table cleanly.

    - **`repl_reset_state()` at `repl_core.c:4173`** resets command arrays, time, panel state, etc., but doesn't reset declared vars. Add a call to `init_predef_vars()` (which now installs only `t`) inside the reset so the new-session and file-load code paths start from the same baseline. The existing tests at `test_repl_core_examples.c:211, :320` and `test_repl_editor.c:33, :52, :66, :78, :91, :104, :117` and `test_repl_core_internal.c:107, :120` and `test_repl_core_parse.c:24, :112` all call `repl_reset_state()` to start clean - they'll now also start with the right empty predef table.

    - **`repl_load_from_file()`** is preceded by `repl_reset_state()` (see `test_repl_core_examples.c:320-322`), so once `repl_reset_state` resets the predef table, the load path is clean. The file's `float …;` source lines feed through `try_commit_float_decl` and re-register names; the `// @var name = value` workspace-header lines then restore values into the freshly-declared slots. Confirm this ordering: source body must be parsed *before* the workspace header values are applied (or add the auto-declare fallback in the `// @var` parser as belt-and-suspenders).

13. **Reserved names** rejected at declaration time. New `is_reserved_ident(name)` helper near the top of `repl_eval.c`, checking against `{t, PI, TAU, float, var}` plus the expression-function names from `eval_primary()` (`sin, cos, tan, sqrt, abs, pow, min, max, floor, ceil, fmod, rand`). Same helper rejects `float t;`, `float sin;`, etc. Place adjacent to `eval_primary()`'s function dispatch.

14. **Examples get a mechanical audit pass.** Entries in `repl_examples.c` that reference non-`t` predef vars outside a `for`-loop gain a leading `float …;` line. For-loop locals (`i`, `j`, `k`) still don't need declarations - `ExprCtx::vars[]` precedence in `repl_eval.c:99-109` shadows the predef table. With step 6 (undeclared-identifier rejection) in effect, any missed example will fail to load loudly with a clear status, not silently render zero - so the audit is self-checking.

## Files to modify

Primary:

- `repl_eval.h:27` - bump `MAX_PREDEF_VARS` to 16.
- `repl_eval.h` - declare `find_predef_var_idx`, `declare_predef_var`, `undeclare_predef_var`, `is_reserved_ident`, `source_uses_ident`.
- `repl_eval.c:19-29` - rewrite `init_predef_vars()` to register only `t`; add the five new helpers and the reserved-name list.
- `sample.h:129` - add `CMD_VAR_DECLARE` to `CmdType`.
- `sample.h:180-193` - add `#define MAX_NAMES_PER_DECL 8` (or near `MAX_PREDEF_VARS`), and `char var_names[MAX_NAMES_PER_DECL][16]; int var_decl_count;` to `GLCmd`. Verify struct size impact: ~132 bytes added × `MAX_COMMANDS=8192` × 2 arrays (`g_cmds` + `g_flat_cmds`) = ~2.1 MB extra BSS. Acceptable. If a future memory audit needs to shrink this, the new fields can be unioned over `args[8]` (`CMD_VAR_DECLARE` doesn't use `args`) for a 32-byte-per-cmd savings.
- `repl_editor.c` - new `try_commit_float_decl()` helper; register before `try_assign_variable` at every dispatch site (`:1503-1533, :1599-1665, :1803-1819, :2614-2628`, plus `feed_line` at `:2614`). One-line comment at each site explaining the ordering constraint.
- `repl_editor.c:334-418` - improve `try_assign_variable()`'s rejection path to set a clear status message ("undeclared variable 'foo' - use 'float foo;' first").
- `repl_editor.c:251` - wrap `delete_cmd_range()` with the in-use guard and post-delete compaction (see step 7).
- `repl_editor.c:152-157` - extend `UndoSnapshot` with name array + count; update `snapshot_save`/`snapshot_restore` at `:169-186`.
- `repl_core.c` - in `execute_commands()` and `flatten_range()`: add `case CMD_VAR_DECLARE: break;` (no-op).
- `repl_core.c:1573, :1621, :1720, :1776, :1896` - add `validate_expression_idents()` calls before each `parse_exprs()` call; on failure, set status and reject the command (returns the same way malformed input is currently rejected).
- `repl_core.c:446` - extend `UserScene` struct with `predef_names[MAX_PREDEF_VARS][16]` and `num_predef_vars`.
- `repl_core.c:3895-3910` - `restore_user_scene()`: overwrite `g_predef_vars[]` and `g_num_predef_vars` from the snapshot before copying values.
- `repl_core.c` - `save_user_scene()`: copy `g_predef_vars[]` names + count into the new fields.
- `repl_core.c:3913-3935` - `load_example_lines()`: call `init_predef_vars()` near the top (after clearing commands, before `feed_line()` loop) so the example starts with a clean predef table.
- `repl_core.c:4173-4204` - `repl_reset_state()`: call `init_predef_vars()` near the top so every reset (tests, file load, new session) starts with only `t`.
- `repl_eval.c` - new `validate_expression_idents()` helper that walks identifiers and reports the first unknown one.
- `repl_editor.c` - call `validate_expression_idents()` from `try_assign_variable` (RHS), `try_commit_for_loop` (start/end/step), `try_commit_if_block` (condition), and the function-call argument parser.

Secondary:

- `repl_export.c:41-94` - confirm load order: source body fed first, then `// @var` values restored. Add an auto-declare fallback in the `// @var` parser for safety against legacy files.
- `repl_export.c:1320-1340` - `write_predef_var_globals` and `write_predef_var_reset_func` already iterate `g_num_predef_vars`, no change needed. Verify.
- `repl_export.c` (display body writer, around `:1352-1364`) - skip `CMD_VAR_DECLARE` lines in the C-body emitter (the globals already declare them at file scope).
- `ui_panels.c:3167-3171` - update panel title to `"Variables (declared)"`. Eyeball geometry at `g_num_predef_vars == 1`.
- `repl_examples.c` - prepend `float …;` line to examples that need it.

## Reuse

- `find_predef_var_idx(name)` - linear scan, `N ≤ 16`. Reused by `declare_predef_var`, `undeclare_predef_var`, `validate_expression_idents`, and the `// @var` workspace-header loader at `repl_export.c:87-91` (which currently inlines the same loop).
- `declare_predef_var(name, err, errsz)` - single append site with bounds, dup, and reserved-name guards. Reused by `try_commit_float_decl` and the load-time `// @var` auto-declare fallback.
- `undeclare_predef_var(name)` - compacts `g_predef_vars[]` and shifts `CMD_VAR_ASSIGN` indices. Single call site (the new delete handler).
- `source_uses_ident(src, name)` - token-aware substring match. Single call site (the new delete handler). Note: `source_uses_ident` is a single-name check; `validate_expression_idents` is a "every identifier must be known" check. They share the same identifier-tokenization loop pattern (`repl_eval.c:31-44` style) and could share a static helper.
- `validate_expression_idents(src, vars, num_vars, err, errsz)` - strict-mode parse-time check, called from every commit site that takes an expression.
- `is_reserved_ident(name)` - single source of truth for forbidden names.

## Edge cases

- **Redeclaration** (`float a; float a;`) → no-op, status `"already declared"`. Value is preserved.
- **Redeclaring `t`** → rejected.
- **Capacity overflow** → whole `float` line atomic-rejected with `"variable table full (max 16)"`.
- **Invalid identifier** (`float 3x;`, `float abc.d;`, name ≥ 15 chars) → rejected.
- **Name collision with GL functions or keywords** (`float sin;`, `float float;`) → rejected by `is_reserved_ident`.
- **Deleting a `float a;` line while `a` is referenced anywhere outside the deletion range** → rejected with status `"variable 'a' is in use, cannot delete"`. User must remove the references first.
- **Deleting a `float a;` line *together with* `a = 1;` and any geometry that uses `a` as a block** → allowed (the in-use scan skips lines inside the deletion range).
- **Declaring `x` / `y` / `z`** → allowed; they're no longer pre-registered.
- **Multi-name decl with one bad name** (`float a, t, c;`) → whole line rejected atomically; `a` is not partially declared.
- **More than 8 names in a single `float` line** (`float a, b, c, d, e, f, g, h, i;`) → rejected with status `"too many names per declaration (max 8); split across multiple lines"`. Workaround: two `float …;` lines.
- **Geometry referencing an undeclared name** (`glVertex3f(radius, 0, 0)` with no `float radius;`) → rejected at commit time with `"undeclared variable 'radius'"`. Was previously a silent zero. (Step 6.)
- **Switching examples mid-session** - user has `float radius, angle;`, switches to a built-in example via F12. The example's own `float …;` lines populate the table after `load_example_lines()` resets it; on F12-cycle back to "Your scene", `restore_user_scene()` re-installs `radius, angle` from the saved snapshot. (Step 12.)
- **Loading a saved file via `repl_load_from_file()`** - `repl_reset_state()` runs first, clearing predef vars to just `t`; the file's own `float …;` source lines re-declare; `// @var` header lines restore values. (Step 12.)

## Verification

1. `make test` - existing tests that assume `g_num_predef_vars == 11` will fail. Update each test's fixture to call `declare_predef_var()` for the names it needs.
2. New tests under `test_repl_core_vars.c`:
   - `test_float_decl_single`, `test_float_decl_multi`
   - `test_float_decl_reserved` (`float t;`, `float sin;`, `float float;` → rejected)
   - `test_float_decl_duplicate` (idempotent, value preserved)
   - `test_float_decl_capacity` (16 OK, 17 rejected)
   - `test_float_decl_atomic_partial` (`float a, t, c;` → none registered)
   - `test_float_decl_max_per_line` (`float a,b,c,d,e,f,g,h;` → OK; nine names → rejected with status)
   - `test_undeclared_in_geometry` (`glVertex3f(radius, 0, 0)` with no `float radius;` → rejected, doesn't render zero)
   - `test_undeclared_in_for_header`, `test_undeclared_in_if_condition` (same rejection in those parsers)
   - `test_user_scene_round_trip_with_decls` (declare, save user scene, load example, restore user scene → declarations and values both come back)
   - `test_load_example_resets_decls` (declare `float radius;`, switch example, verify only the example's vars are in the table)
   - `test_repl_reset_state_resets_decls` (declare, call `repl_reset_state`, verify only `t` remains)
   - `test_assign_undeclared` (rejected with clear status)
   - `test_float_decl_undo` (declare → undo → `g_num_predef_vars` restored)
   - `test_float_decl_delete_in_use` (delete refused with status)
   - `test_float_decl_delete_block` (delete `float a;` together with `a = 1;` succeeds)
   - `test_float_decl_delete_compacts` (after delete, later vars' `num_args` are decremented, sliders still control the right slots)
   - `test_float_decl_save_load` (round-trip through `repl_export`)
3. `make sample && ./sample` - manual verification:
   - Fresh session shows only `t` in the variable panel.
   - `float radius, angle;` then `radius = 2;` → both appear in the panel; the `float radius, angle;` line is visible in the code panel.
   - Backspace on `float radius;` while `radius = 2;` exists → status "variable 'radius' is in use".
   - Select-and-delete a block containing both `float radius;` and `radius = 2;` → succeeds, panel updates.
   - Ctrl+Z restores deleted declarations, including their slot.
   - F12-cycle through every example in `repl_examples.c`; each renders without "undeclared variable" errors.
   - Ctrl+S → reopen → declared names and values round-trip.
4. `./sample existing_old_output.c` - confirm the auto-declare fallback in the `// @var` loader rescues legacy saves.

## Risks

1. **Dispatch ordering bug** - if `try_commit_float_decl` runs *after* `try_assign_variable` at any site, `float x;` gets misread. Comment every site.
2. **Compaction shifts `num_args` of `CMD_VAR_ASSIGN`** - the decrement loop must run for *every* `CMD_VAR_ASSIGN` whose old `num_args > removed_slot`. If any other code path reads `num_args` for `CMD_VAR_ASSIGN` and caches the value across a delete, the cache goes stale. Grep `CMD_VAR_ASSIGN` consumers and confirm no caching.
3. **`GLCmd` size growth** - `var_names[8][16]` + `var_decl_count` adds ~132 bytes. With `MAX_COMMANDS=8192` × 2 arrays, that's ~2.1 MB extra BSS. Acceptable; if not, put the new fields in a union with existing per-type-only fields (e.g. `mode`, `args[8]`) since `CMD_VAR_DECLARE` doesn't use them.
4. **Save/load order dependency** - `// @var name = value` lines must be parsed *after* the `float …;` source lines in the body, otherwise the names don't exist yet. Add an auto-declare fallback in the `// @var` parser as a belt-and-suspenders fix.
5. **Reserved-list drift** - `is_reserved_ident()` must be kept in sync with `eval_primary()`'s function dispatch. Place them adjacent in `repl_eval.c` with a comment.
6. **Example audit miss** - a single example using `a` without `float a;` will silently fail to render. Click-test every example.
7. **`source_uses_ident` false positives in comments / strings** - if `g_cmds[j].source` contains the name inside a `// comment`, the scan flags it as in use. For simplicity, accept the false positive; the user can edit the comment. (Strings don't really exist in REPL syntax, so only comments matter.)
