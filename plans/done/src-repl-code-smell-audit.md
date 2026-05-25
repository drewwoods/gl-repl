# `src/repl/` — Code-Smell Audit

> Audit produced 2026-05-23. Findings come from five parallel reviews of
> `src/repl/` plus targeted spot-verification of the most actionable
> claims. File:line references are exact at the time of writing — check
> `git log` on the cited files before acting if this doc has aged.
>
> Scope: every file under `src/repl/` except `examples.c` (mostly
> verbatim data). Tests under `tests/` were read where they document a
> contract, but not audited.

## Backlog closeout (2026-05-24)

The original audit's afternoon + one-week passes shipped on the dates
shown below. A follow-up review on 2026-05-24 sorted the remaining
unimplemented items into action tiers (see *Tier system* below);
**Tiers A and B landed in commits `f5d0c50` / `cbe73d2` / `17a574b` /
`783d7e3`** on branch `audit-tier-ab-cleanup`. Verified clean against
`make check-c99` and `make test-stubs` (45/45 binaries, 6815/6815
tests).

Status by finding:

- **🔴 Bugs:** all 11 (#1–#11) done in the original passes.
- **🟡 Drift hazards:** all 12 (#12–#23) done.
- **🟢 Dead code:** all 12 (#24–#35) done.
- **🔵 Structural:** #36 already done before the audit ran (the
  declared mutator had moved to `state_owners.h`); #37, #38 done in
  the original passes; **#41 + #42 + #43 + #45 + #46 done in the
  closeout** (Tier A/B — see below); **#39, #40 done in the Tier C
  pass** (see below); **#44 done in the closeout** (Tier B).

### Tier system

Used during the 2026-05-24 backlog review to triage what was left:

- **Tier A — small, real fix, near-zero risk.** 5–30 line changes
  with no architectural exposure. Always worth doing once they're
  identified; no reason to defer.
- **Tier B — moderate effort, clear value.** 50–200 line changes,
  one or two files of churn, a real test impact. Worth doing when
  next in the area, or as a focused pass.
- **Tier C — defer (high cost, low payoff).** Real wins but touch
  a wide surface; the audit's own "Out of scope" cluster lives here.
  Revisit if the surrounding code is being reworked anyway.
- **Tier D — keep deferred.** Findings the audit landed wrong, or
  where the original intent (a kept-on-purpose trampoline, a
  test-pinned bug) makes "fixing" them worse than living with them.

### Findings by tier

- **Tier A (done — commit `f5d0c50`):**
  - **#42** — Add `GLCmd.var_idx`, stop overloading `num_args` for
    `CMD_VAR_ASSIGN`. Closes a documented foot-gun.
  - **#43** — Add `display_name` to `ReplCommandTypeSpec`; replace
    the 14-case `cmd_display_name_for_begin_error` switch.
  - **#45** — Rename `repl_state_mark_normals_dirty` →
    `_mark_source_dirty` (and the `repl_mark_*` wrapper). The
    function invalidates more than normals; the new name says so.
  - **#46** — Move `repl_copy_predef_values` / `_restore_predef_values`
    from `executor.c` to `eval.c` (executor never touched them).
- **Tier B (done — commits `cbe73d2` and `17a574b`):**
  - **#41** — Build `func_def_idx[REPL_FUNC_SLOT_COUNT]` once per
    flatten so CMD_CALL handlers index directly instead of scanning
    `source_cmds[]` per call. (`cbe73d2`)
  - **#44** — Embed `ReplExportConfig cfg` in `UserScene`; fold
    `g_pre_example_cfg`/`g_pre_example_valid` into one wrapped struct.
    Kills the parallel-array invariant. (`17a574b`)
- **Tier C (done — branch `repl-audit-test-gaps`, commits `7bf748e`
  and `925b135`):**
  - **#40** — `flatten_range` extraction (378 → 59 lines). Extracted
    `flatten_reparse_line`, `flatten_for_loop`, `flatten_call`,
    `flatten_if_block`. (`7bf748e`)
  - **#39** — `parse_command` extraction (903 → 456 lines). Extracted
    `parse_label`, `parse_materialfv`, `parse_materialf`,
    `parse_point_parameter_fv`, `parse_func_call`. (`925b135`)
  - Size ratchet added (`check-tier-c-function-size`) to prevent
    future growth past the new baselines. (`8925633`)
- **Tier D:** none from this audit (the equivalent in
  `src-ui-code-smell-audit.md` is #37, kept as a deliberate
  namespace-boundary marker).
- **Already done before the audit ran:**
  - **#36** — `repl_state_document_reset` is in `state_owners.h:36`,
    not `state_views.h`.

The 🔵 cluster's outstanding work was always small enough to fit in
a one-afternoon pass; the audit just hadn't gotten around to it. The
"out of scope" Tier C entries remain real wins for the day someone
opens the relevant files for unrelated work — both `parse_command`
and `flatten_range` would shed ~200-400 lines under a per-handler
extraction.

## How to read this

Findings are grouped by severity, not by file:

- **🔴 Actual bugs (verified)** — correctness or data-loss issues with a
  concrete failure mode. These should be picked up first.
- **🟡 Drift hazards** — parallel structures (two parsers, two tables,
  two walkers) that aren't enforced by anything. Working today; a
  one-side edit will silently diverge.
- **🟢 Dead code / dead fields** — code with no callers, enum values
  with no producers, parameters silenced with `(void)`. Pure surface
  reduction.
- **🔵 Structural / boundary concerns** — long functions, misplaced
  helpers, the `_mut()`-for-reads pattern. Bigger refactors; higher
  cost.

Each finding cites file + line, names the smell, says why it matters,
and suggests a one-line fix. Where the same root cause shows up in
multiple files, it's one finding with multiple references — don't fan
the fix out across separate PRs.

## 🔴 Actual bugs (verified)

### 1. `format_evaluated_cmd` silently drops single-arg shape annotations (done)

**Where:** `src/repl/replay_annotations.c:1093-1122`

**Smell:** `eval_fmt_for_type` returns `nargs=1` for `CMD_GLUT_CUBE` and
`CMD_GLUT_TEAPOT`, but the dispatch `switch` only handles `case 2..6`
and falls to `default: return 0`. The guard at L1095 is `nargs < 1`
(accepts 1), so execution flows into the switch and the EVAL overlay is
silently dropped.

**Why it matters:** Real correctness bug — `glutSolidCube(t * 0.5)`
during replay loses the evaluated overlay. No test caught it because
the suite covers only multi-arg shapes.

**Fix:** Add `case 1: snprintf(out+oi, out_size-oi, fmt, cmd->args[0]); break;`.

### 2. Live-state shuttles and workspace import leak `func_aliases` (done)

**Where:** `src/repl/scenes.c:384-407`, L416-457, L576-603,
L612-658 (vs. `save_scene_to_slot` at L195-240,
`load_scene_from_slot` at L283-332), plus
`src/repl/export.c:3628-3673`

**Smell:** Save/load capture and restore `func_aliases` (L212-218,
L300-304). The live-state shuttle variants — used by
`repl_save_workspace`, `repl_load_workspace`,
`repl_promote_example_if_needed`, `repl_load_scene_as_new_slot`, and
LRU eviction — do not. Workspace import has the same additive-state
shape: `load_scene_file_into_slot()` loads each file into the live alias
table, while `repl_export_load_from_file()` only adds aliases from
`// @func` directives and never clears aliases absent from the file.

**Why it matters:** Workspace iteration calls `install_scene_into_live`
per slot. Without alias restore there, the wrong `// @func N = name`
lines can be emitted for later slots. During workspace load, a scene
with no `// @func` can inherit the previous imported file's aliases
before `save_scene_to_slot()` captures it, so the bad mapping becomes
part of the in-memory scene.

**Fix:** Add the same `func_aliases[]` capture/restore in
`stash_live_state` / `restore_live_from_stash` / `install_scene_into_live`,
and make per-file import start from a clean alias table (or explicitly
snapshot/restore aliases around each import). Add a workspace regression
with one aliased scene followed by one unaliased scene.

### 3. `g_search_*` / `g_ac_*` macros reference fields that no longer exist (done)

**Where:** `src/repl/state.c:156-169`

**Smell:** 14 macros expand to `g_repl_state.search.*` /
`g_repl_state.autocomplete.*`. `ReplRuntimeState` has no `search` or
`autocomplete` slices (moved to `EditorState` in an earlier phase). The
macros work only because no one expands them.

**Why it matters:** Anyone "helpfully" using these to read state gets
an opaque struct-member error, not a missing-symbol error.

**Fix:** Delete the macros.

### 4. `glPointParameterfv` parsing sets `has_vars` from wrong scope (done)

**Where:** `src/repl/parser.c:842`

**Smell:** `cmd->has_vars = (num_vars > 0);` — i.e. "does any var exist
in the world?", not "does this command reference one?". Every other
table-driven branch (lines 160, 218, 479, 627, 771, 940, 1031) uses
`input_has_any_visible_vars(args, vars, num_vars)`.

**Why it matters:** The command is re-evaluated each frame even with
literal floats. `has_vars` also affects autonormal / replay
source-preservation; spurious "true" can prevent normalization.

**Fix:** Replace with `input_has_any_visible_vars(rest, vars, num_vars)`.

### 5. `validate_expression_idents` error gets discarded in const-value branch (done)

**Where:** `src/repl/parser.c:158-163` (vs. L200-205)

**Smell:** The `ENUM_OR_EXPR` branch surfaces the evaluator's `verr`
("undefined variable 'foo'"). The `ENUM_OR_CONST_VALUE` branch swaps in
`as->usage` ("Try GL_TRUE or GL_FALSE") even when the actual failure
was an undeclared ident.

**Why it matters:** `glDepthMask(undef_var)` reports the wrong cause.

**Fix:** In the const-value branch, treat undeclared-ident failure the
same as expr-form — surface `verr`.

### 6. `repl_export_save_output` ignores fclose / ferror (done)

**Where:** `src/repl/export.c:3414-3440`

**Smell:** `fclose(f)` return is discarded; `"Saved to output.c (N
commands)"` is posted regardless of write errors.

**Why it matters:** ENOSPC / EIO produces "success" on a half-written
file. This cannot be fixed fully inside the callee alone because the
function returns `void`: `repl_save_workspace()` increments `written`
after every call, and `repl_save_active_scene()` overwrites the callee's
status with its own success message.

**Fix:** Check `ferror(f)` before fclose; surface via
`repl_set_status_error`; change `repl_export_save_output` to return
success/failure and make all callers preserve failure status instead of
posting a later success.

### 7. `%g` everywhere — lossy float round-trip (done)

**Where:** `src/repl/export.c` — 25+ sites use bare `%g` for float
persistence. Sample: L295, L1076, L1081, L1086, L1114, L1184-L1193,
L1812, L1854, L1860, L1868, L2006, L2344, L2610, L2622, L2625, L2810,
L2851, L2888.

**Smell:** `%g` defaults to 6 significant digits. `0.1234567f` becomes
`0.123457` on save.

**Why it matters:** Files don't round-trip — they lose precision every
save/load cycle. The comment at L1029 acknowledges `%g` is for
trailing-zero strip, but it's applied to *all* persistence, not just
rendered C.

**Fix:** Use `%.9g` for float32-safe round-trip in the save path
(rendered-C cases that justify trimming can keep bare `%g`).

### 8. `fgets` truncation in load is silent (done)

**Where:** `src/repl/export.c:3645-3653`

**Smell:** A line longer than `MAX_LINE_LEN-1` is split across two
reads; each half is processed as if it were a complete line.

**Fix:** Detect "buffer full but no newline", drain through the next
newline/EOF so the remainder is not parsed as a second logical line,
and warn/skip or fail the import consistently.

### 9. Tutorial teardown fires before input validation (done)

**Where:** `src/repl/scenes.c:615-617` (`repl_load_workspace`), L898-907
(`repl_load_user_scene_idx`)

**Smell:**
```c
int repl_load_workspace(const char *dir) {
    repl_dispatch_tutorial_teardown();
    if (!dir || !*dir) return 0;            // teardown already done
    ...
}
```

**Why it matters:** Clicking a disabled scene tab (empty slot) destroys
the in-progress tutorial because the validation guard fires *after*
teardown.

**Fix:** Move `repl_dispatch_tutorial_teardown()` after the validity
guards.

### 10. Workspace scene filename slugs can collide (done)

**Where:** `src/repl/scenes.c:169-190`, L459-470, L515-522,
L546-564

**Smell:** `derive_unique_scene_name` only checks display-name
uniqueness. `scene_filename_slug()` normalizes distinct names to the
same path stem (`"A B"`, `"A-B"`, and `"A_B"` all become `a_b`). The
old "after 999 tries" concern is not the practical failure mode under
the default `MAX_USER_SCENES == 8`; the real collision is display names
that are unique but normalize to the same filename.

**Why it matters:** `repl_save_workspace()` writes each scene to
`<workspace>/<slug>.c`. A later slot can overwrite an earlier one, and
loading the workspace back loses a scene.

**Fix:** Make workspace path generation slug-unique too (for example,
dedupe slugs with a numeric suffix or include the slot id), and report
any skipped/failed write.

### 11. `repl_load_workspace` never clears existing slots (done)

**Where:** `src/repl/scenes.c:612-658`

**Smell:** Iterates the directory and calls `load_scene_file_into_slot`
for each `.c`, which finds the next free slot. Never calls
`repl_scenes_reset` first.

**Why it matters:** Loading a workspace into a session that already has
N user scenes *merges* the files in; files past `MAX_USER_SCENES`
silently disappear (the `loaded` counter only tracks successes). The
semantic the user expects ("load this workspace") doesn't match what
the function does.

**Fix:** Decide on the semantic — clear slots first, or report "skipped
N files: slots full".

## 🟡 Drift hazards (parallel structures, no enforcement)

### 12. Tag/subheading API is byte-duplicated between examples and tutorials (done)

**Where:** `src/repl/tutorials.c:184-203, 339-413` vs.
`src/repl/examples.c:1318-1337, 1419-1510`

**Smell:** Eight functions per side (`tag_count` / `_label` / `_mask` /
`_has_tag` / `_count_for_tag` / `_index_for_tag` / `_visible_tag_count`
/ `_visible_tag_at`), structurally identical, byte-for-byte in many
lines. Same goes for the `subheading` field, the "fold ALL into the
mask" idiom, and the catalog `STATIC_ASSERT`. The validating test
exists only on the example side.

**Why it matters:** Bugs need to be fixed twice; only one side has a
catch test. The menu side already proves the abstraction works
(`src/ui/app/menu_bar.c::catalog_flyout_row_at` + `CatalogFlyoutOps`).

**Fix:** Hoist a generic `CatalogTagOps` vtable + one set of walkers.

### 13. `replay_simulate`-shaped walker is duplicated (done)

**Where:** `src/repl/replay_annotations.c:544-655` and L662-777

**Smell:** Two ~110-line near-identical forward simulations
(CMD_VAR_ASSIGN / CMD_SCRATCH_ASSIGN / CMD_IF_BEGIN skip / CMD_GOTO
follow with `REPL_GOTO_LOOP_LIMIT`). The doc-comments at L580-585 and
L706-707 explicitly say "mirror the other function" — the duplication
is acknowledged.

**Fix:** Extract one `replay_simulate(target_pc,
callback_for_each_pc_before_step, vals*, scratch*)` walker; the
snapshot path supplies a snapshot callback, the copy path supplies a
no-op.

### 14. `// @cfg` slug parsing has three vocabularies (done)

**Where:** `src/repl/export.c:478-496`
(`repl_export_extract_cfg_slug`), L498-525 (`parse_cfg`),
`src/repl/example_loader.c:213-237` (`example_cfg_slug_allowed[]`),
plus the runtime bridge's `fill_scene_subset` callback.

**Smell:** README claims `repl_export_extract_cfg_slug` is the "single
source of truth". In practice each parser has its own buffer width and
stop conditions; the example-loader hardcodes the allow-list
independently; the bridge has the *actual* runtime list.

**Why it matters:** A new `@cfg` slug requires updates in three places;
forgetting one most often drops it silently from example loads but not
from workspace saves.

**Fix:** Push the allow-list query into the bridge
(`bridge->slug_is_scene_subset(slug)`); have `parse_cfg` call
`repl_export_extract_cfg_slug`.

### 15. Three `funcN`-token parsers (done)

**Where:** `src/repl/export.c:436-461` (`parse_func_alias`),
L1342-1374 (`parse_func_name_token`), L2646-2660 (inline in
`import_make_repl_func_header`).

**Fix:** Have `import_make_repl_func_header` call
`parse_func_name_token`.

### 16. Two identifier-list parsers (done)

**Where:** `src/repl/export.c:1276-1302` (`parse_identifier_list`)
vs. L2690-2714 (inline `float name1, name2` walker in
`import_make_repl_func_header`). The inline version uses raw `15`
instead of `sizeof(name)-1`.

**Fix:** Extract a shared helper that accepts an optional
leading-keyword sentinel.

### 17. `eval_primary` dispatch is hand-coded `strcmp` chain with parallel reserved-idents table (done)

**Where:** `src/repl/eval.c:228-234` (`s_reserved_idents[]`) and
L869-886 (`eval_primary` dispatch)

**Smell:** Comment says "Keep in sync with `eval_primary()`'s function
dispatch below". Enforced by nothing. Bonus footgun: `args[3]` at L846
silently accepts extra function arguments past the storage capacity.
Fixed-arity functions usually fall through to `0.0f` when arity does
not match, but variadic-ish functions such as `rand` / `rand2` ignore
the extras because they only consult `args[0]` and `args[1]`.

**Fix:** One `{name, arity_min, arity_max, fn_ptr}` table that drives
both reserved-idents and dispatch, rejects unsupported arity, and caps
argument storage explicitly.

### 18. Six independent identifier tokenizers in `eval.c` (done)

**Where:** `src/repl/eval.c:302, 373, 786, 1034, 1203` and
`src/repl/export.c:1470`

**Smell:** Each implements its own "if `isalpha(*p) || *p=='_'`,
advance while `isalnum || _`" tokenizer.

**Fix:** One `static const char *eat_identifier(const char *p, const
char **out_start)` at the top of `eval.c`.

### 19. Indent formula duplicated three times in `parser.c` (done)

**Where:** `src/repl/parser.c:942-948, 1006-1011, 1062-1067`

**Smell:** The `gluEnd`, funcN-call, and goto branches all open-code
indent as `(bb ? 4 : 2) + fdepth * 2` — mirroring `repl_source_scope_cmd_indent`,
but the funcN/goto inline forms *exclude* tess depth. A funcN call
inside `gluBegin(...) { ... }` gets wrongly indented.

**Fix:** funcN/goto branches should call
`repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind))`;
`gluEnd` should grow a `repl_source_scope_tess_close_indent()` helper.

### 20. Std-command emit ladder switches on `def->num_args` (done)

**Where:** `src/repl/parser.c:485-510`

**Smell:** A `case 1: snprintf(... args[0]); case 2: args[0],args[1];
... case 6: ...` cascade because `printf` is varargs and can't accept
an array. The `CMD_CLEAR_COLOR` clamp branch (L513-532) immediately
walks the same ladder again.

**Fix:** Extract `format_def_args(def, args, out, out_sz)`.

### 21. Slug strings hard-coded in `export.c` despite "opaque bag" contract (done)

**Where:** `src/repl/export.c` — `"point_attenuation"` (L631/633/657),
`"msaa"` (L970), `"line_smooth"` (L973), `"vertex_outlines"`
(L1973/L3200), `"vertex_points"` (L1974/L3203)

**Smell:** The bridge docstring (`export.h:97-99`) says keys are
opaque; in practice `export.c` knows the controller's vocabulary.
`vertex_outlines` and `vertex_points` are looked up in two different
functions and must stay consistent.

**Fix:** Gather into `static const char *const k_export_cfg_slugs[]`,
or push the gating decisions into the bridge
(`bridge->geometry_passes_enabled(...)`).

### 22. `repl_cfg_known` uses a fragile two-probe trick (done)

**Where:** `src/repl/export.c:125`

**Smell:** `b->get_int(slug, 0) == b->get_int(slug, 1)` to detect
"known". Relies on `fallback` being used *only* for unknown slugs. A
bridge returning `fallback` for an "off-but-known" key silently lies.

**Fix:** Add explicit `bridge->is_known(slug)`.

### 23. `@declare` import bypasses the workspace-directive table (done)

**Where:** `src/repl/export.c:547-554` (`WORKSPACE_DIRECTIVES[]`)
vs. L2260 (inline `@declare` parse) and L1809 (inline `@declare` emit)

**Smell:** `@scene-name`, `@workspace-dir`, `@var`, `@func`, `@cfg`
flow through the table; `@declare` is parsed and emitted inline. New
markers now have two completely different addition patterns.

**Fix:** Extend `WorkspaceDirective` with an optional in-snippet
handler kind, or move `@declare` to its own marker table.

## 🟢 Dead code / dead fields (verified)

### 24. `REPL_COMPILED_LOAD_ALL` has zero producers (done)

**Where:** `src/repl/compile.h:66`; consumer cases in `src/repl/apply.c:64,125`,
`src/repl/compile.c:126,144,1409`, `src/editor/state.c:225`.

**Verification:** Every `REPL_COMPILED_LOAD_ALL` match in `src/` is a
declaration, switch arm, or doc reference. Nothing sets
`out->kind = REPL_COMPILED_LOAD_ALL`.

**Fix:** Either wire it up (the command_store side already supports
load) or delete the enum value, the cases, and the doc references.

### 25. `needs_block_indent` field has no readers (done)

**Where:** `src/repl/command_spec.c:483` (accessor),
`src/repl/command_spec.h:172` (field). Verified: zero call sites for
`repl_cmd_type_needs_block_indent` outside its own definition.

**Smell:** 51 hand-tuned rows in `g_command_type_specs[]` carry the
field for nothing.

**Fix:** Delete the field, accessor, and prototype.

### 26. `g_for_depth_prefix` is computed but never queried (done)

**Where:** `src/repl/source_scope.c:12, 34, 40, 66`

**Smell:** Per-line for-loop depth maintained in a private prefix
array. The consumer that ever existed was unified into
`g_block_depth_prefix`.

**Fix:** Delete the array and the four lines that touch it.

### 27. `repl_state_predef_vars_storage` has no declarations or callers (done)

**Where:** `src/repl/state.c:184-190`

**Smell:** Defined but not declared in any header and not called
anywhere.

**Fix:** Delete.

### 28. `repl_state_variables_reset` has no callers and is buggy (done)

**Where:** `src/repl/state.c:399-404`

**Smell:** Declared in `state_owners.h`; no production callers. And
when someone *does* wire it up they'll hit a stale-flat-program bug —
the function resets predef vars and rebinds eval storage but doesn't
call `repl_state_mark_flat_dirty()` / `_mark_normals_dirty()`. Compare
with `repl_state_reset_program` which marks both.

**Fix:** Delete; or, if kept, append the dirty-marks.

### 29. `matched_alias` is set but only read via `(void)matched_alias;` (done)

**Where:** `src/repl/parser.c:883, 891, 960`

**Fix:** Delete the variable and the `(void)` line.

### 30. `flatten_src_text`'s `GLCmd *src_cmd` parameter is unused (done)

**Where:** `src/repl/flatten.c:25-30`

**Smell:** The `(void)src_cmd;` cast + comment "kept for symmetry with
earlier helpers" confess dead weight.

**Fix:** Delete the parameter; update the ~10 call sites.

### 31. Unreachable PUSH/POP cases in `repl_executor_apply_transform_cmd` (done)

**Where:** `src/repl/executor.c:218-223`

**Smell:** Cases `CMD_PUSH_MATRIX` and `CMD_POP_MATRIX` are never
reached because the only caller (`apply_tracked_transform_cmd`, L232)
intercepts those before falling through.

**Fix:** Delete the two cases (or `assert(0)` in default), and rename
the static to `_apply_non_stack_transform_cmd`.

### 32. UNDECLARE/SET_VALUE stash dance is built on a false premise (done)

**Where:** `src/repl/compile.c:993-1029`

**Smell:** The comment claims the stash is needed because
"UNDECLARE-first matches the order `repl_apply_predef_ops` expects."
Verified: `repl_apply_predef_ops` (`apply.c:140-165`) makes two
independent passes — one filtering UNDECLARE, one filtering
DECLARE/SET_VALUE. Cross-kind ordering doesn't matter.

**Fix:** Delete the `pending_set` juggling; just append UNDECLAREs
after the parked SET_VALUE.

### 33. `repl_dump_code_panel_visual_text` is `#if 0`-blocked, with 7 dead public fields (done)

**Where:** `src/repl/export.c:3750-3833` (function),
`src/repl/export.h:270-278` (`ReplExportLayout` fields:
`viewport_w/h`, `scene_x/y`, `code_panel_w`, `wrap_at_comma`,
`show_vertex_indices`)

**Smell:** Live code reads only `scene_w/h`. `src/app/glr_ctrl.c:2375-2392`
still populates all 9. The comment "kept so the wrapping approach can
be restored from history" is what `git log` is for.

**Fix:** Delete the `#if 0` block and the 7 dead fields, or restore the
function.

### 34. Stale `x = ` / `y = ` … autocompletions (done)

**Where:** `src/repl/command_spec.c:260-267`

**Smell:** Lists `x = ` / `y = ` / `z = ` / `i = ` / `j = ` / `k = ` /
`n = ` Tab completions. Only `t` is predeclared; every other name
requires `float x;` first.

**Why it matters:** Tab suggests `x = ` that immediately fails to
commit with "undeclared variable".

**Fix:** Keep only `t = `, or gate via the live predef table.

### 35. `funcN` boilerplate hand-rolled 20× in command_spec (done)

**Where:** `src/repl/command_spec.c:239-259`

**Smell:** 20 rows like `{ "func0() {", "func0() {", 0, { NULL } }`
for `func0..func9` × `() {` and `()`, with drifted indentation between
L240 and L241.

**Fix:** Generate via a macro loop, or move funcN handling out of the
table-driven path.

## 🔵 Structural / boundary concerns

### 36. `state_views.h` declares a mutator (already done before audit)

**Where:** `src/repl/state_views.h:159`

**Smell:** `void repl_state_document_reset(void);` is declared in the
"view-only" header. The header is supposed to be the read-only seam
`scene_*` / `ui_*` include.

**Fix:** Move to `state_owners.h`.

### 37. `_mut()` accessors used for reads (done)

**Where:** Hot spots:
- `src/repl/replay_annotations.c` (~39 sites — pure annotation module
  that never mutates)
- `src/repl/core.c:153, 154, 501, 503, 725, 740`
- `src/repl/flatten.c:167-175, 639, 669-711`
- `src/repl/parser.c:916-918`

**Smell:** Per CLAUDE.md, `_mut()` is for owner modules and the
controller. These pipeline files call `repl_state_document_cmds_mut()[i]`
only to read `.valid`, `.type`, `.args[0]`. The const-returning variant
is declared in `state_views.h`.

**Fix:** Replace with the const variant. Add a `check-state-ownership`
ratchet to prevent regression.

### 38. ~10 generic REPL helpers live in `export.c` (done)

**Where:** `src/repl/export.c` — non-`static`, declared in
`core_internal.h`:
`trim_in_place` (L1221), `repl_extract_paren_payload` (L1230),
`extract_for_args_text` (L1245), `parse_identifier_list` (L1276),
`parse_expr_list_exact` (L1304), `parse_repl_func_signature` (L1376),
`extract_func_call_args_text` (L1415), `format_func_header` (L1439),
`input_has_expr_vars` (L1468), `input_has_any_visible_vars` (L1483),
`repl_extract_label_name` (L1487), `repl_extract_goto_label` (L1498),
`repl_extract_assignment_parts` (L1511),
`repl_extract_assignment_target_parts` (L1524),
`split_top_level_args` (L1609), `parse_func_name_token` (L1342).

**Smell:** Consumed by `executor.c`, `compile.c`, `core.c`,
`flatten.c`, `parser.c`, `glr_completion.c`. Nothing to do with
persistence.

**Why it matters:** This is the single reason `export.c` is the
heaviest file in the directory. Moving them out drops `export.c` by
~1500 lines (~40%).

**Fix:** Move to a new `src/repl/text_helpers.c` (or merge into
`parser.c`).

### 39. `parse_command` is 870 lines mixing parsing, validation, formatting, and clamping (done — Tier C)

**Where:** `src/repl/parser.c:250-1121`

**Smell:** Dispatches comments, table-driven enum commands, glEnd,
table-driven std commands, label, glMaterialfv, glPointParameterfv,
glPush/Pop/LoadIdentity, funcN calls, glu* tess ops, goto/label, and
"reserved-ident-as-command" fallback — all in one function. Each new
command grows another `if (strcmp(func, ...) == 0)` arm. The
"format → if clamped, re-format" shape for `CMD_CLEAR_COLOR` (L513-532)
is uniquely embedded in this one function. Findings #5, #20, #43 are
sub-bugs of this.

**Fix:** Extract each ad-hoc branch into a per-command static handler
with a uniform `(line, args, cmd, text_out, text_sz, ctx) → int`
signature.

### 40. `flatten_range` is a 380-line god-function (done — Tier C)

**Where:** `src/repl/flatten.c:198-576`

**Smell:** Dispatches on `CmdType`, evaluates expressions, mutates
`g_predef_vars` / scratch arrays, re-parses source text, and recurses.
Three near-identical re-parse branches at L517-572 differ only in
`vars/nv` passing and `has_vars` setting.

**Fix:** Extract per-CmdType handlers (`flatten_for`, `flatten_call`,
`flatten_if`) and a `flatten_reparse_line()` for the tail; let the
loop be a dispatch table.

### 41. Linear `CMD_FUNC_DEF` lookup per `CMD_CALL` (done — Tier B, commit `cbe73d2`)

**Where:** `src/repl/flatten.c:294-352`

**Smell:** Each call scans `source_cmds[0..source_count)` for the
matching def, even though slots are a fixed 10. With N calls × K
unrolled iterations × M source cmds, that's K*N*M scans per frame.

**Fix:** Build a `func_def_idx[REPL_FUNC_SLOT_COUNT]` lookup once per
flatten.

### 42. `GLCmd.num_args` is overloaded for `CMD_VAR_ASSIGN` (done — Tier A, commit `f5d0c50`)

**Where:** `src/repl/flatten.c:422`, `src/repl/executor.c:695`;
`src/repl/command.h:93` documents it as an exception.

**Smell:** The "if it's an arg count, it's also a var index" rule is
implicit and not type-checked. A maintainer adding bounds checks or
arg-count assertions to the executor will silently corrupt VAR_ASSIGN.

**Fix:** Add a dedicated `GLCmd.var_idx` field (or tagged union).

### 43. `cmd_display_name_for_begin_error` is a manual switch parallel to the spec tables (done — Tier A, commit `f5d0c50`)

**Where:** `src/repl/parser.c:1127-1147`

**Smell:** Maps ~14 CmdType values to "glBegin", "glPointSize", etc.
The std/enum spec tables already carry these names.

**Why it matters:** A new not-valid-in-begin command is picked up by
the spec table automatically but falls through here to the raw `CMD_*`
identifier.

**Fix:** Add a `display_name` field to `ReplCommandTypeSpec` or
reverse-lookup the spec tables by type.

### 44. Per-slot cfg storage parallel to user-scene array (done — Tier B, commit `17a574b`)

**Where:** `src/repl/scenes.c:81-92`

**Smell:** `g_user_scenes[MAX_USER_SCENES]` and
`g_scene_cfg[MAX_USER_SCENES]` are sibling tables indexed by the same
slot. The invariant that every `used=0` flip clears the cfg is
convention-only (today, one call site: L815-816). Same for
`g_pre_example_cfg` + `g_pre_example_valid`.

**Fix:** Embed `ReplExportConfig cfg;` directly in `UserScene`;
roll `g_pre_example_valid` into a discriminator field on
`ReplExportConfig`.

### 45. Hidden cache-coupling: depth invalidation rides on `mark_normals_dirty` (done — Tier A, commit `f5d0c50`)

**Where:** `src/repl/state.c:367-371`

**Smell:** `repl_source_scope_depth_cache_invalidate` only fires when
callers explicitly invoke it — *or* when `repl_state_mark_normals_dirty()`
happens to call it inside the same function. `apply.c` mutates the
command store, which calls `command_store_invalidate_after_mutation` →
`mark_normals_dirty` → depth-cache invalidate. Three layers of unnamed
coupling.

**Why it matters:** A future refactor that splits flat-program-dirty
from depth-cache-dirty will silently produce stale indent/scope
answers.

**Fix:** Rename to `repl_state_mark_source_dirty()` (covers both), or
wire `command_store_invalidate_after_mutation` directly so the
dependency is explicit.

### 46. `repl_copy_predef_values` / `_restore_predef_values` live in `executor.c` but never called by it (done — Tier A, commit `f5d0c50`)

**Where:** `src/repl/executor.c:177-197`

**Smell:** Pure manipulations of `g_predef_vars`. Called only from
`src/app/glr_ctrl.c` and `src/subsystems/replay/replay.c`.

**Fix:** Move next to `repl_eval_declare_predef_var` /
`_undeclare_predef_var` in `eval.c`; declare in `eval.h` (or a new
`predef_storage.h`).

## Sequencing

### One-afternoon pass

- [x] **#1** — `format_evaluated_cmd` missing case 1 (single-arg shape
  replay bug). Surgical add of one case arm.
- [x] **#2** — `func_aliases` stash/import hole. Mirror what
  `save_scene_to_slot` already does for live stash/install/restore,
  and add the per-file import clear/restore so unaliased scenes do not
  inherit previous aliases.
- [x] **#3** — Delete the `g_search_*` / `g_ac_*` landmine macros.
- [x] **#4** — `glPointParameterfv` `has_vars`. One line.
- [x] **#5** — Const-value branch error surfacing. Two-line swap.
- [x] **#29** + **#30** + **#31** — Delete dead variables / unused
  parameters / unreachable cases.
- [x] **#24** — Delete `REPL_COMPILED_LOAD_ALL` (one enum value + six
  switch arms across four files; check no tests guard the empty
  "load-all" branch first).

That's ~7 small commits, all with focused scope.

### One-week pass

- [x] **#37** — `_mut()` for reads in `replay_annotations.c` (~39 sites in
  one TU, easy to do in one commit).
- [x] **#38** — Move the ~10 generic helpers out of `export.c`. Biggest
  single file-size reduction in the directory.
- [x] **#33** — Delete `repl_dump_code_panel_visual_text` and its 7 dead
  `ReplExportLayout` fields; remove the `glr_ctrl.c` population code.
- [x] **#21** + **#22** — Add `bridge->is_known(slug)` and clean up the
  hardcoded slugs in `export.c`.
- [x] **#14** — Single source of truth for the `@cfg` slug allow-list
  (push into the bridge).
- [x] **#12** — Hoist `CatalogTagOps` for examples/tutorials tag duplication.
- [x] **#13** — Extract `replay_simulate` from the twin walkers.

### Out of scope

- The `g_cfg_items[]` / spec-table pattern is working as designed; the
  project's CLAUDE.md explicitly calls it out as the canonical "add a
  row" idiom.
- The two-level command model (source → flat) or any of `command.h`'s
  control-flow predicates. Load-bearing.
- The `STEP_APPEND` / `STEP_SET` / `STEP_REQUIRE` positional macros
  in `tutorials.c:10-36` — worth switching to designated initializers
  in principle, but every existing call site already initializes by
  position, so the field-reorder hazard is theoretical.
- ~~`parse_command` extraction (#39) and `flatten_range` extraction (#40)~~
  — **Done** (2026-05-25, branch `repl-audit-test-gaps`). Both functions
  decomposed with per-command static handlers; a size ratchet
  (`check-tier-c-function-size`) prevents future growth.

## Method note

This audit was produced by five parallel review agents, each scoped to
a slice of `src/repl/`:

- parse / compile / apply / command_spec / source_scope
- eval / flatten / executor / autonormal / format
- export / load (the heaviest file in the directory)
- state / scenes / examples / example_loader / core / command_store
- tutorials / help_text / replay_annotations / source_scope / pipeline / util

Each agent was asked for ~15-20 highest-signal findings only, not a
comprehensive sweep. The most actionable claims (real-bug findings
above) were verified against the source before being escalated to
🔴 status — see the `Bash` calls in the audit transcript for the
verification commands. The 🟡 / 🟢 / 🔵 findings are reported as
the agents framed them; spot-check before acting on the more
mechanical ones.
