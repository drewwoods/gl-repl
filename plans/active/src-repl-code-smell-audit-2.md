# `src/repl/` — Code-Smell Audit (Follow-up)

> Audit produced 2026-05-26 as a refresh of the 2026-05-23 audit
> (`plans/done/src-repl-code-smell-audit.md`). Findings come from five
> parallel reviews of `src/repl/` plus targeted spot-verification of
> the most actionable claims. The reviewers were instructed not to
> re-flag items the prior audit closed; everything below is new
> material, a regression of a previously-closed item, or an item the
> prior audit closed only partially.
>
> File:line references are exact at the time of writing — check
> `git log` on the cited files before acting if this doc has aged.
>
> Scope: every file under `src/repl/` except `examples.c` (mostly
> verbatim data, but cross-reference smells in adjacent files are
> included).
>
> **Context — what landed since 2026-05-23:** 35 findings closed in
> the original closeout (#1–#35); Tier A 2026-05-24 closed #36, #42,
> #43, #45, #46; Tier B 2026-05-24 closed #41 and #44; the
> follow-up [P2]/[P3] pass swept the `var_idx`/parallel-cfg leftovers.
> Two helpers landed during this work: `src/repl/text_helpers.c` (from
> the original #38) and `src/repl/cfg_baseline.{c,h}` (from the
> subsystems audit's #30). `export.c` is still the heaviest file at
> 3254 lines despite shedding ~1500 lines to `text_helpers.c`.

## How to read this

Severity grouping mirrors the previous audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or data-loss
  issues with a concrete failure mode that exists in current
  production code. Pick these up first.
- **🟡 Drift / boundary hazards** — parallel structures, layering
  inversions, ambiguous-intent code that works today but is one edit
  away from misbehaving.
- **🟢 Dead code / dead fields** — code with no callers, unused
  parameters, redundant wrappers. Pure surface reduction.
- **🔵 Structural concerns** — long functions, magic numbers, comment
  archaeology. Bigger refactors; higher cost.

Each finding cites file:line, names the smell, says why it matters,
and suggests a one-line fix. Cross-file findings (the same shape
recurring in multiple TUs) carry a **🔀 cross-file** tag.

## 🔴 Actual bugs / hazards (verified)

### 1. ✅ `help_text.c` is the only `src/repl/` file that includes an `app/` header

**Where:** `src/repl/help_text.c:11`

**Smell:** `#include "app/glr_config.h"` — `glr_config_items()` /
`GlrConfigItem` / `GLR_CONFIG_NONE` reach into the descriptor table
that `glr_actions.c` owns. A grep across `src/repl/*.{c,h}` shows
this is the **only** `app/` include in the layer.

**Why it matters:** Direct layering inversion: per `MODULES.md`,
`src/repl/` is the language core and must not depend on the app
shell. The absence of a `check-repl-no-app-includes` ratchet let
this slip in. If the `scene_demo` link surface is exercised, this
file would be the breaker.

**Fix:** Either (a) move the F-key dynamic rows into the controller
and have it inject "F2..F10" lines via a small caller-supplied
callback, or (b) move `help_text.c` into `src/app/` since it
already reads `g_cfg_items`. Add a guard to lock the boundary.

**Status (2026-05-26):** ✅ Closed via option (a). Added a
`ReplHelpFkeyProvider` typedef + `repl_help_text_install_fkey_provider()`
hook in `src/repl/help_text.h`; the controller installs a static
`glr_ctrl_help_fkey_label(fn)` lookup that walks `g_cfg_items[]`
inside `glr_ctrl_install_app_services`. `src/repl/help_text.c` no
longer includes `app/glr_config.h` — when no provider is installed
(the standalone `scene_demo`) the F-Key Toggles section renders
empty rather than dragging in the controller's vocabulary. The
existing `scripts/check-repl-no-app.sh` ratchet was already in
place; its baseline drops from 1 to 0, so any new `#include "app/..."`
from src/repl/ is now a hard error.

### 2. 🟡 `parse_cfg` double-applies every `// @cfg` line — design hazard, not currently triggerable

(*Severity note: downgraded from 🔴 to 🟡 after closer reading.
The double-fire is a real contract violation but the only
observable side effect today — `tutorial_notify_state_changed` —
is a no-op when no tutorial is active and a near-no-op even when
one is, so no current user-visible failure mode has been
identified. The "actual bug" classification was over-strong;
this is a drift hazard waiting on a fragile contract.*)

**Where:** `src/repl/export.c:400-426` (per-line apply) and L3182
(end-of-load drain)

**Smell:**
```c
/* parse_cfg fires per-line: */
if (g_export_cfg_bridge && g_export_cfg_bridge->apply) {
    ReplConfigBag single;
    repl_config_bag_clear(&single);
    repl_config_bag_set_int(&single, slug, val);
    g_export_cfg_bridge->apply(&single);
}
repl_config_bag_set_int(&g_import_cfg_accumulator, slug, val);
/* …then end of repl_export_load_from_file: */
import_cfg_accumulator_apply_and_reset();   // applies the bag AGAIN
```

**Why it matters:** Every `@cfg` line triggers `bridge->apply` once
during parse and once at drain. The bridge implementation in
`glr_config.c` ends every `glr_config_set` with
`tutorial_notify_state_changed`. Comment at `export.c:412` justifies
this as "same set of pairs" — but `apply` is not contractually
idempotent, only happens to be in practice today because:

- Tutorial-notify (`glr_config.c:163-169`) early-returns when no
  tutorial is active and when the current step is not REQUIRE.
- A REQUIRE step that matches and advances on the per-line apply
  will be on a different step by the drain — so the drain's
  re-fire still hits the early-return.

The pathological case (tutorial REQUIRE step N matches on per-line
apply; advance to step N+1, also a REQUIRE on the *same* slug
matching the *same* value — would auto-advance through the
showcase) is constructible but not in any shipped tutorial.

**Fix:** Pick one path. Either drop the per-line apply (let the
drain do it all), or drop the accumulator (drain becomes a no-op).
Document which is the source of truth. Tier B sized; not blocking
any current user-facing behavior.

### 3. ✅ `repl_copy/restore_predef_values` is values-only by contract and assumes table-shape stability — currently no enforcement, and at least one caller can violate the assumption

**Where:** `src/repl/eval.c:233-253` (the just-moved functions from
the closed #46); contract comment at `src/repl/eval.h:229-232`.

**Smell:** The pair documents itself as "Values-only snapshot …
Names + count are NOT preserved here." That contract has a silent
prerequisite the comment doesn't state: the *table shape*
(`g_num_predef_vars` and the name→slot mapping) must be unchanged
between copy and restore. The implementation neither captures nor
asserts that. Specifically: copy uses `n = min(g_num_predef_vars,
max_vals)`; restore uses the same `min` against the live count at
restore time. If the table reordered or shrank in between, restore
either drops the snapshot tail (shrink) or writes saved floats into
slots now holding *different* variables (reorder / shrink-then-grow).

**Why it matters:** The autonormal scratch pair (`flatten.c`) is
short-lived enough that the table cannot change between save and
restore in normal operation — there the values-only contract is
fine. Everything else is a *long-lived* path that can span a table
mutation:

- `replay.c`'s start-of-replay baseline (three sites) — survives
  multiple frames; the user can switch workspace mid-replay,
  which rebinds the predef table.
- `ReplayFadePlan.baseline_predef_vals` (`glr_ctrl.h:17`,
  populated at `glr_ctrl.c:264` via the values-only
  `replay_copy_baseline_predef_values`, restored at
  `glr_ctrl.c:226` via values-only restore). The fade plan is
  built once per replay-start and read back across every fade
  tick, so its lifetime matches the replay baseline above.
- The frame-level pair at `glr_ctrl.c:1844, 1953`
  (`live_predef_vals[]` saved at the top of a frame's render,
  restored at the bottom). Within a single frame the executor
  re-evaluates expressions that *write* through the predef table,
  but it doesn't add or remove vars, so the table shape is stable
  *in normal operation*. The contract risk here is much smaller
  than the multi-frame paths.

The triggerable failure today: workspace switch (or any
source/predef-table mutation routed through user input) while a
replay is active rebinds the table; the fade-plan restore then
writes saved floats into slots now holding different vars.

**Fix (pick one — they are not stacking options):**

1. **Cascade the full-snapshot pair through every long-lived
   caller.** That means: switch `replay.c`'s three sites to
   `repl_eval_copy/restore_predef_vars`; widen
   `ReplayRuntimeState.baseline_predef_vals` to also store names
   and count; rewrite `replay_copy_baseline_predef_values` and
   `replay_restore_baseline_predef_values` (in `replay.c:975-988`)
   and the matching `ReplayFadePlan` storage (`glr_ctrl.h:17`) to
   carry names + count too; update `glr_ctrl.c:226` and `:264`
   accordingly. The short-lived autonormal pair stays values-only;
   the frame pair (`:1844, :1953`) is a judgement call — see (3)
   below for the smaller-scope alternative.
2. **Stop replay before any source / predef-table mutation.**
   Tighter contract: replay must terminate before a workspace
   load, scene switch, undo across an `@declare`, etc. No
   long-lived snapshot can then span a table mutation, so the
   values-only pair is safe everywhere. Smaller code change, but
   it's a UX policy decision (does the user lose their replay
   state on every workspace load?). Worth scoping before
   choosing.
3. **Keep all values-only callers on the values-only pair and add
   an `assert(g_num_predef_vars == saved_count &&
   names[i]==saved_names[i])` guard in `repl_restore_predef_values`**
   — but that requires capturing names too, which is effectively
   option (1) without the API rename.
4. **Document the prerequisite in the contract comment and leave
   the runtime bug latent** — only acceptable if every long-lived
   caller is audited and proven not to span a table mutation.
   Brittle long-term; deferred-cost fix.

**Do not** simply have copy return the captured count and have
restore consume that count without also restoring names: the count
half alone fixes overread on *grow* but does nothing for *reorder*
or *shrink*, which is the actual triggerable bug here.

**Do not** convert only `replay.c`'s three sites without the
fade-plan cascade: `ReplayFadePlan` reads from the replay baseline
via the values-only helper at `replay.c:986`, so a partial fix
leaves the fade path silently truncating names back to floats. The
fade and replay storage have to move together.

**Status (2026-05-26):** ✅ Closed via option 1 (cascade), with a
small twist that avoids the frame-level cascade option 1 would have
required. The triggerable path was the fade-plan restore in
`glr_ctrl_replay_restore_baseline`, which sits inside the
controller's per-frame values-only save/restore at the top/bottom
of `glr_ctrl_display_frame`. Going through
`repl_eval_restore_predef_vars` (full replacement, including
`g_num_predef_vars`) would have changed the live table's COUNT
mid-frame, leaving the frame-end values-only restore unable to
repopulate any slots the fade restore had dropped — that would have
turned a values-only bug into a full-table loss.

The fix:

1. **Widened storage.** `ReplayRuntimeState.baseline_predef_*` and
   `ReplayFadePlan.baseline_predef_*` now hold `vals + names + count`
   (not just floats). The two storage layouts moved together, as the
   audit required.

2. **Full-snapshot capture.** All three `replay.c` sites that
   captured the baseline switched from `repl_copy_predef_values` to
   `repl_eval_copy_predef_vars` so the names + count travel with
   the values. Added a new
   `replay_copy_baseline_predef_snapshot(vals, names, count)` API for
   the fade plan to read the full snapshot; the existing
   `replay_copy_baseline_predef_values` (values-only) stays for the
   short-lived within-frame annotation simulator.

3. **By-name restore.** Added a new
   `repl_eval_restore_predef_values_by_name(src_vals, src_names,
   src_count)` that pairs saved values with the LIVE predef slots by
   name match. It deliberately leaves the live table's
   count/names/slot order untouched — only updates values where the
   snapshot's name still exists in the live table. Saved names that
   no longer exist are silently dropped; live names not in the
   snapshot keep their current values. Both
   `glr_ctrl_replay_restore_baseline` (production fade restore) and
   `replay_restore_baseline_predef_values` (test helper) route
   through this. The full-replacement
   `repl_eval_restore_predef_vars` stays available for callers that
   legitimately need to clobber the live table shape (e.g. an undo).

4. **Annotation simulator caveat documented.** The per-line
   annotation simulator in `replay_annotations.c::build_visible_vars_from_predef_values`
   still pairs CURRENT predef-table names with the saved value array.
   It also has the slot-mismatch bug under reshape, but its consumer
   (per-frame annotation rendering) is short-lived and its
   correctness story is fuzzier than the fade restore's — a separate
   follow-up. The values-only `replay_copy_baseline_predef_values`
   API was kept specifically for this caller.

Regression test
`test_replay_baseline_restore_survives_predef_reshape` in
`tests/test_repl_replay.c` declares X/Y/Z after `t`, sets values
{10, 20, 30}, calls `replay_start()`, undeclares Y (which shifts Z
into Y's old slot), clobbers live values, calls
`replay_restore_baseline_predef_values`, and asserts X == 10 AND
Z == 30. Pre-fix the values-only restore put Y's saved value (20)
into Z's slot — verified by temporarily restoring the old body and
watching the `Z gets Z's saved value (30), NOT Y's (20)` assertion
fail.

### 4. ✅ `repl_config_bag_set` silently truncates oversized keys/values and returns success

**Where:** `src/repl/cfg_baseline.c:15-30`

**Smell:**
```c
snprintf(cfg->items[cfg->count].key,   REPL_CFG_KEY_MAX,   "%s", key);
snprintf(cfg->items[cfg->count].value, REPL_CFG_VALUE_MAX, "%s", value);
cfg->count++;
return 1;   /* always success */
```
`REPL_CFG_KEY_MAX = 24`. A 26-char slug truncates and is stored;
`return 1` lies. Subsequent `repl_config_bag_get(cfg, full_slug)`
returns NULL because the keys don't match.

**Why it matters:** If both ends use the bag's own get/set, a long
slug is silently invisible. The bridge's `is_known` path can mask
this in one direction.

**Fix:** Detect truncation (snprintf return ≥ size) and return 0;
or `STATIC_ASSERT(REPL_CFG_KEY_MAX >= longest_known_slug + 1)`.

**Status (2026-05-26):** ✅ Closed. Added pre-flight `strlen` rejects
plus a `fits()` helper that compares snprintf's return against the
destination size; truncation on any of the three write sites (key,
new-entry value, replace-path value) now returns 0 without
mutating the bag. The replace-path explicitly preserves the prior
value on rejection so callers don't accidentally turn an overflow
into a silent corruption. `repl_config_bag_set_int` checks its
local "%d" buffer the same way, with a
`STATIC_ASSERT(REPL_CFG_VALUE_MAX >= 12)` pinning that
`REPL_CFG_VALUE_MAX` can hold every 32-bit decimal int (so the
runtime check is for defense, never a hot path). Regression tests
in `tests/test_repl_core_internal.c` exercise oversized key,
oversized new value, oversized replace value, oversized set_int
key, plus the `KEY_MAX - 1 / KEY_MAX` boundary cases — pre-fix 9
assertions fail, post-fix all 221 pass.

### 5. ✅ `UserScene` stack allocations carry ~1 MB+ each; multiple concurrent stashes exceed small-thread stacks

**Where:** `src/repl/scenes.c:601, 731, 798, 848, 857, 949`;
`UserScene` definition at `src/repl/scenes.c:65-83`.

**Smell:** `UserScene` carries two large fixed arrays —
`GLCmd cmds[MAX_COMMANDS]` (4096 entries) and
`char lines[MAX_COMMANDS][MAX_LINE_LEN]` (4096 × 256 = 1 MB on its
own), plus predef/scratch/func-alias arrays. `sizeof(UserScene)` is
already on the order of ~1–1.5 MB *before* any cfg work.
`repl_load_scene_as_new_slot` holds two stashes at once (`stash +
evicted_stash` ≈ ~2–3 MB on the frame); `try_evict_lru` adds a
`live_temp` (third); the caller frame may already carry a fourth via
`repl_promote_example_if_needed`. Default macOS/Linux main-thread
stack is 8 MB; POSIX worker threads often 512 KB – 2 MB.

**Why it matters:** Real overflow risk on smaller-stack threads;
ASan bait on the main thread. This is the same shape as the
original audit's #1/#2 (which moved similar stack allocations to
file statics or the heap); the fix never propagated to the user-
scene paths.

**Causal note (revised):** An earlier draft of this finding
attributed the bloat to Tier B #44's `ReplConfigBag` embed. That
was wrong: with `REPL_CFG_KEY_MAX=24`, `REPL_CFG_VALUE_MAX=16`, and
`REPL_CFG_MAX_ITEMS=32` (see `cfg_baseline.h:12`),
`sizeof(ReplConfigBag)` is ≈ 1.3 KB — well under 0.2% of the struct.
The cfg embed is incidental to the stack hazard; `cmds[]` + `lines[]`
already dwarfed everything else. Treat this finding as a pre-existing
`UserScene` stack-copy hazard, not as fallout from the cfg work.

**Fix:** Heap-allocate the stash buffers (`malloc(sizeof(UserScene))`
with `free()` at exit) or hoist a single `static UserScene
g_scratch_stash` (these functions are not reentrant; the static
scratch matches the original #1/#2 fix pattern). Either is Tier B
sized.

**Status (2026-05-26):** ✅ Closed via heap allocation (option 1 —
static scratch was rejected because `repl_load_scene_as_new_slot`
holds two scratches simultaneously while `try_evict_lru` allocates
a third, so a single shared static would clobber). Added
`scene_scratch_alloc` / `scene_scratch_free` helpers near the
existing `stash_live_state` / `restore_live_from_stash` pair;
converted all six sites (`repl_save_workspace`, `repl_load_workspace`,
`try_evict_lru`, `repl_load_scene_as_new_slot` (×2),
`repl_promote_example_if_needed`) from stack `UserScene` to heap
`UserScene *`. OOM at alloc time returns the function's pre-existing
failure code (`-1` / `-2`) so callers see a graceful no-op rather
than a crash. Regression test
`test_user_scene_load_scratch_alloc_lifecycle` in
`tests/test_repl_core_extra.c` drives the worst-case 3-allocation
path (load-at-capacity through `repl_load_scene_as_new_slot` +
`try_evict_lru`) twice; the existing `test_user_scene_promote_lru_evict`
covers the `repl_promote_example_if_needed` inner LRU branch. ASan
in `BUILD=debug` catches any missing free / double free / UAF
across all six paths.

### 6. `repl_state_capture/restore` excludes `g_user_scenes[]` despite header claiming "scene bookkeeping"

**Where:** `src/repl/state.c:502-521`, header doc at `state.h:18-19`,
orphan globals in `scenes.c:85-95`

**Smell:** `ReplSceneRuntimeState` (in the captured set) holds only
`active_example_idx` and `workspace_dir`. The user-scene catalog
itself (`g_user_scenes[]`, `g_active_user_scene`, `g_user_scene_tick`,
`g_pre_example`) lives in scenes.c file-statics and is NOT captured.

**Why it matters:** `repl_state_capture` then `_restore` will lose
the catalog if anything modified it between the two calls. Today
only tests use the pair so the bug is latent; if anyone wires this
into snapshot-undo or session-save, scenes silently disappear on
restore. The header docstring's "scene bookkeeping" is misleading.

**Fix:** Either move the four scene-catalog file-statics into
`ReplRuntimeState.scenes` (the BSS would move from scenes.c into
the runtime struct — invasive but principled), or update the
docstring to say "scene bookkeeping = active-example-idx +
workspace-dir only; the user-scene catalog is NOT in this snapshot."

### 7. `source_scope.c` reads through `_mut()` accessors (regression of audit #37)

**Where:** `src/repl/source_scope.c:41, 42, 161, 175`

**Smell:** Inside `depth_cache_rebuild`:
```c
if (repl_state_document_cmds_mut()[i].valid) {
    CmdType t = repl_state_document_cmds_mut()[i].type;
```
Pure read-only operations. Audit #37 cleaned up the same pattern in
`replay_annotations.c` (~39 sites) and recommended a
`check-state-ownership` ratchet "to prevent regression." The ratchet
was never added, and source_scope.c regressed (or never got fixed).
Confirmed inconsistent within the file: L200 uses the const
accessor for the same kind of read.

**Why it matters:** Same pattern as the closed #37; the audit's
intended ratchet would have caught it.

**Fix:** s/_mut()/non_mut_accessor/g at the 4 sites; add a grep
ratchet to `check-state-ownership` against `_mut()` outside owner
modules (state.c, apply.c, command_store.c, controller).

### 8. ✅ `import_feed_one_line` ignores `load_err` populated by `repl_load_apply_line`

**Where:** `src/repl/export.c:2502-2522` (plus the same anti-pattern
at L3026-3033)

**Smell:** `load_err` is declared and passed to three
`repl_load_apply_line` call sites, but never read. When a line
fails to parse (`load_err = "command store at capacity (max N)"`
or similar), the user only sees the generic `"Warning: could not
parse line:"` with no detail. Capacity overflows during workspace
import are invisible.

**Fix:** Surface `load_err` via `fprintf(stderr, "Warning: %s: %s\n",
line, load_err)` (when non-empty), or accumulate into a warnings
buffer the importer reports in the final status.

**Status (2026-05-26):** ✅ Closed. Both `import_feed_one_line` and
the `import_try_function_header` warning emit now check whether
`load_err[0]` is populated; if so, the Warning line ends with
`(<load_err>)`. While auditing the path I also found that the
plain-command tail of `repl_load_apply_line` (`load.c:219`) didn't
populate `err` on capacity failure — the structured-change path
above did, but plain commands fell through silently. Added the
same `"command store at capacity (max %d)"` message to that branch
so both compile shapes surface the same diagnostic. Regression test
in `tests/test_repl_core_io.c` writes a `// Snippet start` /
`glVertex3f(1, 2, 3);` / `// Snippet end` fixture, forces the
cmd-store at capacity, redirects fd 2 (stderr) to a temp file via
`dup2`, and asserts the captured stderr contains
`"(command store at capacity"`. Pre-fix the warning lacked that
detail; verified by running the test before and after the load.c
edit.

### 9. ✅ `repl_export_load_from_file` swallows `fclose` and `ferror` (asymmetric with the prior #6 fix)

**Where:** `src/repl/export.c:3157-3158` plus the full `fgets` read
loop (no `ferror` check inside or after)

**Smell:** `fclose` failure is silent (no status/log). The `fgets`
loop never checks `ferror(f)` — returning NULL is treated as EOF
even on real I/O error. The prior audit's #6 fix landed the
save-side check; the load-side never got the symmetric treatment.

**Fix:** Mirror the save pattern: `int had_read_err = ferror(f);
int close_failed = fclose(f) != 0; if (had_read_err || close_failed)
repl_set_status_error("Error: cannot read X");`.

**Status (2026-05-26):** ✅ Closed. Mirrored the save-side
`ferror`/`fclose` pair after the import loop, with a matching
`"Error: cannot read %s"` status message. Regression test in
`tests/test_repl_core_io.c` points the loader at `/tmp` (a directory
— `fopen` succeeds, the first `fgets` returns NULL with
`ferror=1`) and asserts the function returns failure; pre-fix this
returned success.

**Follow-up (2026-05-26, reviewer):** the first revision returned
early on read/close error AND on the older `truncated_line` path
without clearing the per-load accumulator state populated by
`parse_cfg` (`g_import_cfg_accumulator`), `parse_workspace_header_line`
(`g_deferred_var_values` / `g_pending_scene_name` /
`g_pending_workspace_dir`). A partial parse that aborts mid-stream
leaves those populated; a later `repl_export_apply_pending_cfg()`
call (every example load goes through `example_loader.c:437`) then
drains the stale slugs through the bridge and reapplies cfg values
from the failed import. Added a `repl_export_load_reset_accumulators`
helper called from both error returns (and reused at function entry,
so the entry/exit reset is symmetric). Added a focused regression
test in `tests/test_repl_core_io.c` that writes `// @cfg wireframe = 1\n`
+ a 600-byte unterminated line (triggering `truncated_line`), then
manually resets live `wireframe` to 0 and calls
`repl_export_apply_pending_cfg()`; pre-follow-up the drain re-applies
`wireframe = 1` from the leaked accumulator, post-fix the drain is
a no-op (verified by reverting the source change and watching the
new assertion flip).

### 10. ✅ `flatten_source_lighting_enabled` ignores control flow

**Where:** `src/repl/flatten.c:575-588`

**Smell:**
```c
for (int i = 0; i < source_count; i++) {
    if (source_cmds[i].valid && source_cmds[i].type == CMD_ENABLE &&
        (GLenum)source_cmds[i].args[0] == GL_LIGHTING)
        user_lighting_enabled = 1;
    /* ...same for CMD_DISABLE... */
}
```
Walks the *source* array linearly, treating `glDisable(GL_LIGHTING)`
inside an `if(0) { }`, `func9 { }`, or never-entered `for(i, 0, 0)`
body as effective. Result propagates to `ReplFlattenResult` and the
scene render.

**Why it matters:** This is exactly the source-text false-positive
that flatten exists to resolve everywhere else.

**Fix:** Walk the *flat* program after expansion; or accept that
the executor's tracking is the only correct answer and remove this
signal.

**Status (2026-05-26):** ✅ Closed. Renamed the helper to
`flatten_flat_lighting_enabled` and rewired it to walk
`ctx.flat_cmds` (the post-expansion program) instead of `ctx.source_cmds`.
Regression tests in `tests/test_repl_core_internal.c` (section 8b)
cover: unreached `if(0)` body, unreferenced funcN body (both report
OFF correctly), `glDisable` inside an empty `for(i, 0, 0)` body
(no longer disables the top-level enable), and a reached `for` loop
(legitimate enables behind control flow still detected).

### 11. ✅ `source_scope_cmd_indent` / `_cmd_tess_indent` write 1 byte past buffer when `buf_sz == 0`

**Where:** `src/repl/source_scope.c:92-104, 145-156`

**Smell:** Sibling functions on the same page (`begin_indent` L111,
`tess_close_indent` L122) have `if (buf_sz <= 0) return;` guards.
The two externally-called surface functions (`cmd_indent`,
`cmd_tess_indent`) don't, and with `buf_sz == 0` end up writing
`buf[0] = '\0'` to a zero-length buffer.

**Fix:** Add `if (buf_sz <= 0) return;` to match the siblings.

**Status (2026-05-26):** ✅ Closed. Added the guard at the head of
`repl_source_scope_cmd_indent` and `repl_source_scope_cmd_tess_indent`,
matching the existing pattern in `begin_indent` / `tess_close_indent`.
Regression test in `tests/test_repl_core_internal.c` (section 6b)
surrounds a 1-byte target with sentinels and confirms all four indent
helpers no-op on `buf_sz == 0`; also pins the `buf_sz == 1` case for
`cmd_indent` to write only the terminator.

### 12. ✅ `try_apply_example_camera_header` return value discarded; skips 5 lines unconditionally

**Where:** `src/repl/example_loader.c:439-443`

**Smell:** Function returns 0 if the block has fewer than 5 lines
or fails the shape check; loader skips 5 lines anyway. A malformed
camera block silently swallows subsequent geometry lines.

**Fix:** Capture the return; if 0, either leave the lines for
ordinary parsing or fail the load with a status error.

**Status (2026-05-26):** ✅ Closed. Gated the 5-line skip on the
`try_apply_example_camera_header` return value — on validation
failure the lines are left for ordinary parsing instead of silently
eaten. Updated the existing `invalid_camera_example` test in
`tests/test_repl_core_examples.c` to assert the new "all lines
survive" invariant (camera state still untouched because the bridge
rejected the block), and added a `truncated_camera_example`
regression that pins the precise data-loss scenario (`// camera` +
2 transforms + 3 geometry lines used to eat the marker + both
transforms + 2 geometry lines under the 5-line skip).

### 13. `repl_eval_parse_exprs` docstring promises `-1 on error`; implementation never returns -1

**Where:** `src/repl/eval.h:267-271` vs. `eval.c:1089-1103`.

**Smell:** The header says "returns the number of expressions parsed
(up to max), -1 on error." The implementation only returns `n`
(no-progress loop break). The two values disagree on what failure
looks like.

**Why it matters — doc drift, not an actual bug:** A sweep of
callers (`parser.c:216, 460, 605, 693, 756, 1046, 1154`,
`compile.c:1625`, `editor/commit.c:383`, `app/glr_ctrl.c:185`,
`tests/test_eval.c:44, 591`) shows none rely on `-1` as a sentinel.
The dominant pattern is "compare parsed count against the expected
arity": e.g. `parser.c:1046-1051` does
`cmd->num_args = repl_eval_parse_exprs(...); if (cmd->num_args <
def->num_args) <incomplete-arg-error> else <usage-error>;`. A `-1`
return would land in the `<incomplete-arg-error>` branch, which is
already an error path — the diagnostic stays consistent. The
"treats malformed lists as zero args, success" phrasing in the
original draft of this finding was overstated; no current caller
exhibits that behavior.

**Fix:** Drop the `-1` clause from the docstring so behavior and
contract agree. This is doc drift, not a runtime bug — Tier A as
written (single-comment edit) and safe. If a future caller wants a
hard error signal it can switch to a parse entry that provides
one, but no such caller exists today.

## 🟡 Drift / boundary hazards

### 14. 🔀 `_mut()` accessors used for read-only work — multi-file regression

**Where:**
- `src/repl/source_scope.c:41, 42, 161, 175` (covered as #7 above)
- `src/repl/autonormal.c:114-176, 199-258, 287-308` (~31 sites)
- `src/repl/scenes.c:18-24, 427` (~24 sites via the `SCENE_STATE` /
  `g_workspace_dir` macros)
- `src/repl/export.c` (sites total ~22; classify per below before
  acting)
- `src/repl/replay_annotations.c:7` (`#include "repl/state_owners.h"`
  but the file uses zero `_mut` APIs — leftover include from the
  closed #37 sweep)
- `src/repl/compile.c:567` (predef-vars read via `_mut()`-backed
  macro alias inside an `ExprCtx` init)

**Smell:** Audit #37 cleaned up `replay_annotations.c` and
recommended a ratchet; the ratchet never landed, and the pattern is
back in five other files. Some of this is direct `_mut()` usage,
some is hidden behind the `g_predef_vars` / `SCENE_STATE` macros
that resolve to `_mut()` accessors.

**Why it matters:** Defeats the constness contract the typed-facade
encodes. Any future "enforce read-only paths" refactor can't.

**Classify before sweeping — not every `_mut()` call site is read-only
drift.** Three categories overlap in this finding; only the first is
fair game for the const-replacement sweep:

1. **Pure read drift (replaceable):** the file mentions the variable
   only to read it (e.g., `printf`, comparison, length). Examples:
   most `source_scope.c` sites, the autonormal range-walks, the
   `replay_annotations.c` orphan include. *This* is what the ratchet
   should catch.
2. **Legitimate owner mutation (must not flip to `_const`):** real
   writes that need a mutable handle. Examples: `export.c:328`
   (`g_predef_vars[idx].value = val;` during import-side var resolve)
   and `export.c:1721` (same pattern inside `// @var name=value`
   restore). Misclassifying these as "read-only drift" silently
   breaks import.
3. **Snapshot-owner reads (const accessor already exists):** the
   caller is itself the snapshot owner taking a base-pointer to
   `memcpy` out of (e.g., `scenes.c:202` does
   `memcpy(s->cmds, repl_state_document_cmds_mut(), …)`). The
   const accessor `repl_state_document_cmds()` already exists at
   `state_owners.h:17` / `state.c:201` — these callers just need
   to switch to it. Do **not** treat as raw drift.

**Fix:** Per-file sweep, but split into three commits matching the
categories above so reviewers can audit each separately:

1. *Const-replacement sweep* — pure read drift only (replace
   `_mut()` calls with the existing const accessors where the
   caller only reads).
2. *Reroute snapshot-owners and add the one missing const helper.*
   The document-array snapshot-owners (`scenes.c:202`, etc.)
   already have `repl_state_document_cmds()` available — just
   switch them over. The one missing helper is the predef-var
   const variant: today `g_predef_vars` is a macro routing to a
   `_mut()`-backed accessor, so reads can't expressively distinguish
   themselves from writes. Add `repl_eval_predef_vars()` (const)
   and reroute the read-side `g_predef_vars` callers through it,
   keeping the `_mut()` form on the write helpers internal to
   eval.c.
3. *Audit the genuine writers* — confirm each surviving `_mut()`
   reference is a real write; document why (file ownership) so the
   ratchet has an allowlist.

Then add `check-state-ownership` to grep for `_mut()` outside the
documented owner files, with the post-(3) allowlist baked in.

### 15. `repl_compile_*` block validators duplicate `editor_compile_*` (~430 lines vs ~860 lines)

**Where:** `src/repl/compile.c:1512-1814` (compile-side close_brace
/ if_block / func_def / for_loop) vs. `src/editor/commit.c:212,
318, 536, 814` (editor-side same four shapes)

**Smell:** Two near-identical compile implementations per block
kind. compile.c is the lean-loader subset; the editor version
handles edit-time semantics (header-replace, oneliner body,
matched-existing-end). Comment at compile.c:1492 says "Step 5c
(deferred) can refactor the editor wrappers to call these pure
versions." Concrete drift caught while reading: `editor_compile_for_loop`
uses `%.9g` for for-loop bounds; `repl_compile_for_loop` uses
`repl_format_source_float` — same shape, different formatter.

**Fix:** Step 5c — make `editor_compile_*` call `repl_compile_*`
for parse/validate, layer editor effects on top. Or extract a
shared `parse_for_loop_header()` and have both wrappers call it.

### 16. `repl_compile_dispatch` covers float-decl + var-assign only; other entry points are out-of-band

**Where:** `src/repl/compile.c:62-89` (dispatcher), vs.
`src/repl/load.c:85-112` (explicit per-kind chain)

**Smell:** Dispatcher's NAME doc promises "the registered compile
handlers in canonical order"; in practice it knows 2 of 6. Adding
a new entry needs hand-wiring at every caller (load.c does it,
`editor_try_commit_*` does it separately).

**Fix:** Extend dispatch to call all six in canonical order
(load-bearing — float_decl before var_assign, close_brace before
for/func/if), or rename to `_dispatch_var_or_assign` so the limited
scope is in the name.

### 17. Three extracted parser handlers re-implement the same trim/split skeleton

**Where:** `src/repl/parser.c:517-635` (`parse_materialfv`),
`:637-711` (`parse_materialf`), `:713-777` (`parse_point_parameter_fv`)

**Smell:** Tier C extracted these out of `parse_command`
independently — each carries its own copy of "find comma, strncpy,
trim leading/trailing spaces" (L532-547 vs L652-667), face-token
lookup (L552-562 vs L671-676), and the validate-then-parse sequence
(L598-609 vs L686-697 vs L750-761). Newly-exposed by the Tier C
extraction.

**Fix:** Extract `parse_face_pname(args, face_str, pname_str, err)`
shared by the two material handlers, and `parse_canonical_float_list(
text, vars, num_vars, cmd_args, expected_count, err)` for the
validate-then-parse.

### 18. Three identical 18-line cache-prologue blocks in `replay_annotations.c`

**Where:** `src/repl/replay_annotations.c:745-759, 890-903, 927-940`

**Smell:** `build_replay_assignment_inline_comment`,
`replay_build_subst_annotation`, and `replay_build_eval_annotation`
all open with the same 4-clause cache check + 2 memcpys + fallback
call. Byte-equivalent modulo whitespace. Closeout #13 extracted the
simulator but missed the per-call cache-fetch.

**Fix:** Extract `static int replay_load_runtime_state_for(int
cmd_idx, int flat_idx, float *predef_vals, float scratch_vals[..][..])`
returning 0/1; the three call sites collapse to one line each.

### 19. Five "extract slug + walk again" duplicates that the closed #14 missed

**Where:**
- `src/repl/example_loader.c:176-212` (`example_cfg_extract_slug` —
  fresh parser parallel to `cfg_baseline.c:76-101`)
- `src/repl/export.c:400-411` (`parse_cfg` extracts then re-walks
  the same characters to find `=`)

**Smell:** The closed #14 claimed a single source of truth for the
@cfg slug allow-list. The example loader has its own parser
(different prefix expectations) and `parse_cfg` re-scans the
identifier it just extracted to advance to `=`.

**Fix:** Make `example_cfg_extract_slug` a thin wrapper around
`repl_config_extract_slug`; have the canonical slug extractor
accept an optional `end_out` pointer so callers can resume parsing
without re-walking.

### 20. Two near-identical identifier-walking validators in `eval.c`

**Where:** `src/repl/eval.c:552-604` (`expr_range_has_runtime_values`)
and `:606-721` (`validate_expression_idents_range`)

**Smell:** Same 70-line `[//-comment break] [skip numeric literal]
[eat_identifier] [PI/TAU check] [scratch subscript] [paren skip]
[find in vars then predef]` pipeline. One returns "saw a runtime
ref"; the other returns "all valid + writes errors." Fresh
occurrence of the closed #18 pattern.

**Fix:** Extract `expr_walk_identifiers(src, end, vars, num_vars,
ctx, on_ident_fn)` callback shape; the two functions become
specialized callback users.

### 21. `eval.c`'s 25-entry REPL↔C function map is duplicated three ways

**Where:** `src/repl/eval.c:303-318` (`k_expr_builtins[14]`) vs
L1227-1244 (`map[16]` in `repl_eval_expr_to_c`) vs L1398-1414
(`map[15]` in `repl_eval_c_expr_to_repl`)

**Smell:** Three tables enumerating the same REPL↔C function set.
Adding `atan(x)` requires updates to all three; nothing checks
consistency.

**Fix:** Extend `ExprBuiltin` with `const char *c_name;` and drive
both translator tables from `k_expr_builtins[]` (plus a separate
small const-pair list for PI/TAU↔M_PI).

### 22. Executor includes `subsystems/replay/*` for one if-guard (layering breach)

**Where:** `src/repl/executor.c:9-10, 706`

**Smell:**
```c
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
/* ... */
if (!(replay_active() && replay_mode() == REPLAY_MODE_VERTEX)) {
    if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); ... }
```
`subsystems/replay` is a peer module; the executor reaching across
to check replay's mode is exactly the layering creep the executor's
"narrow GL dispatch" charter is supposed to police.

**Fix:** Add `int finalize_tess;` to `ReplExecutionOptions`; replay
sets it to 0 for VERTEX-mode batches; live frame path sets it to 1
(the default). Drop both `subsystems/replay/*` includes.

### 23. Executor writes status messages directly to live REPL state

**Where:** `src/repl/executor.c:615`

**Smell:** `repl_set_status_error("goto: loop limit reached");` —
same purity concern as #22. The executor's contract is "emit GL
calls"; status-bar UI side effects belong to the caller (or via a
result struct like `ReplFlattenResult.status[]`).

**Fix:** Add `char status[REPL_DIAG_TEXT_MAX];` to a
`ReplExecutionResult`; controller posts it after the call.

### 24. CMD_IF_BEGIN evaluator is duplicated between flatten and executor

**Where:** `src/repl/executor.c:634-665` and `src/repl/flatten.c:335-356`

**Smell:** Both call `repl_extract_paren_payload` →
`repl_eval_c_expr_to_repl` → build `ExprCtx` → `repl_eval_expr`.
CMD_VAR_ASSIGN's comment at L671 says "flatten owns the eval; we
just apply args[0]" — but CMD_IF_BEGIN re-evaluates because of goto
dynamism. Undocumented; duplicated logic is unguarded.

**Fix:** Either (a) document why CMD_IF's `has_vars` re-eval is
required for goto+IF interaction, or (b) extract
`static float repl_eval_if_condition(const GLCmd *cmd, ...)` and
call from both.

### 25. `WRITE_TEXT` macro inside `parse_command` adds a third "write text" pattern

**Where:** `src/repl/parser.c:901-904` (macro), 12 call sites, vs.
extracted handlers (`parse_label` L506-512, `parse_func_call`
L843-855) that inline the `if (text_out && text_sz > 0)
snprintf(...)` by hand.

**Smell:** Three patterns for one thing: function-local macro inside
the parent, hand-coded if/snprintf in extracted helpers, and
`repl_format_fits` elsewhere.

**Fix:** Promote to a file-private static `write_text(char *out,
int sz, const char *fmt, ...)`; macro and inline copies go away.

### 26. `REPL_FUNC_SLOT_LIST` hard-codes 10 entries; not derived from `REPL_FUNC_SLOT_COUNT`

**Where:** `src/repl/command_spec.c:121-131`

**Smell:** Audit #35 macroified the funcN boilerplate; the macro
still hard-codes `OP(0)..OP(9)`. Canonical constant is
`REPL_FUNC_SLOT_COUNT == 10`. If `REPL_FUNC_SLOT_COUNT` ever moves
to 12, the funcN autocomplete table silently misses 2 slots.

**Fix:** `STATIC_ASSERT(REPL_FUNC_SLOT_COUNT == 10, ...)` next to
the macro, or codegen via a different idiom.

### 27. Stray `"func0(var0) {"` autocomplete row between the two macro-generated runs

**Where:** `src/repl/command_spec.c:268-270`

**Smell:** Macros emit 10+10 boilerplate rows uniformly; a
hand-written 11th row sits in the middle, giving only func0 a
parameterized hint. Reads like leftover from the #35 macro
extraction.

**Fix:** Either remove (boilerplate is enough) or replicate the
parameterized hint for func1..9 via macro.

### 28. `replay_apply_state_cmd`'s CmdType list is parallel to `repl_execute_program`'s dispatch arms

**Where:** `src/repl/executor.c:240-326` and the case-label cascades
at L469-475, L482-485, L498-500

**Smell:** Every CmdType handled by `repl_apply_state_cmd`'s switch
is also listed in the main `repl_execute_program` switch as a case
that delegates back. A new state CmdType needs both updates.

**Fix:** Let the main switch's default fall through to a
`repl_apply_state_cmd(&flat_cmds[pc], …)` attempt; `return 1`
means "handled."

### 29. `g_pre_example.valid` is still a parallel scalar — Tier B #44 left it half-done

**Where:** `src/repl/scenes.c:89-95`.

**Smell:** Closed #44 explicitly said: *"Embed `ReplExportConfig cfg;`
directly in `UserScene`; **roll `g_pre_example_valid` into a
discriminator field on `ReplExportConfig`**."* First half landed;
second didn't. `valid` is still a sibling scalar.

**Fix:** Add a `valid` (or `populated`) discriminator field to
`ReplConfigBag` itself, so the bag is self-describing and the
caller stops needing a parallel scalar. Fold `g_pre_example.valid`
into `g_pre_example.cfg.valid` and update the two read sites and
the reset site accordingly. Tier B sized (touches `cfg_baseline.h`
+ the few callers in scenes.c).

**Do not** use `count == 0` as the sentinel. The comment block at
`scenes.c:89-91` explicitly calls out that the code needs to
distinguish "valid empty capture" (a pre-example state with no
overrides — restorable as a no-op) from "no capture yet" (don't
restore at all). A `count == 0` collapse would merge those two
cases and silently re-enter restore on every example load.

### 30. `g_pending_workspace_dir` / `g_pending_scene_name` are write-then-caller-reads-then-clears

**Where:** `src/repl/export.c:24-25` (state aliases), L262-291
(writers), L3124-3125 (reset on entry), callers at `scenes.c:696`

**Smell:** Load function "returns" scene name and workspace dir
via module-scope state. The doc-comment acknowledges this but the
contract is fragile: caller must read before the next call resets,
no compile-time enforcement.

**Fix:** Return both via an out-pointer pair on `repl_export_load_from_file`
(or a `ReplImportResult` struct).

### 31. `text_helpers.c` has no `text_helpers.h`; decls live in `core_internal.h`

**Where:** `src/repl/core_internal.h:62-113` declares; implementations
in `src/repl/text_helpers.c`

**Smell:** The closed #38 moved helpers to `text_helpers.c` but
declarations stayed in `core_internal.h` — the very junk drawer the
new home was meant to relieve. `text_helpers.c` is the only
`src/repl/*.c` (besides examples.c, which is data) without a
matching header.

**Fix:** Promote decls into a sibling `text_helpers.h`;
`core_internal.h` re-exports it transitively for legacy callers.

### 32. 🔀 Hardcoded `[16]` / `[64]` widths instead of named constants

**Where:**
- `src/repl/text_helpers.c:121, 142, 218, 282` and `core_internal.h:77,
  85, 90` — `char names[][16]`, `ni >= 15` (REPL_PREDEF_NAME_MAX is 16)
- `src/repl/replay_annotations.c:642, 649`,
  `src/repl/executor.c:610, 621`, `src/repl/core.c:675, 682`,
  `src/repl/parser.c:1209` — six `char label[64]` declarations for
  goto/label names
- `src/repl/parser.c:1182, 1209, 523, 643, 719` and
  `src/repl/source_scope.c:172, 177` — multiple unrelated `[64]`
  buffers (label names, block-nesting depth, per-slot enum capacity)
- `src/repl/parser.c:985` — bare `char end_ind[32]` while the rest
  of the file uses `REPL_INDENT_TEXT_MAX`

**Smell:** Hardcoded sizes scattered through the layer; mechanically
inconsistent (in the same file in places).

**Fix:** Add `REPL_PREDEF_NAME_MAX` (already defined; just use it),
`REPL_GOTO_LABEL_MAX = 64`, `REPL_MAX_BLOCK_NEST_DEPTH = 64`, etc.;
sweep call sites.

### 33. `MAX_DEFERRED_VAR_VALUES = 64` is divorced from `MAX_PREDEF_VARS = 24`

**Where:** `src/repl/export.c:193-196, 332-338`

**Smell:** Constant unrelated to `MAX_PREDEF_VARS`. Overflow path
is silent — no warnings++, no status error. User could lose `@var`
values from a workspace file.

**Fix:** `#define MAX_DEFERRED_VAR_VALUES MAX_PREDEF_VARS`, or
assert ≥ MAX_PREDEF_VARS at init.

### 34. `repl_workspace_dir` / `repl_state_workspace_dir` — two names for the same getter

**Where:** `src/repl/state.c:478-484` and `src/repl/scenes.c:1016-1023`

**Smell:** The state.c shim calls through to scenes.c's pair. Two
parallel names for the same getter and setter; refactoring callers
requires checking both.

**Fix:** Pick one (the `repl_state_workspace_*` shims add no value;
delete and rewrite the 4 callers).

### 35. `core.c` header comment is stale — "diagnostic dumps pending move" but none exist

**Where:** `src/repl/core.c:1-20`

**Smell:** The "Diagnostic dumps — pending move" bullet promises
work that already happened (grep finds zero `dump_*` in core.c).
Other "pending" items have been pending so long the comment misleads.

**Fix:** Delete the diagnostic-dumps bullet. Either commit to the
other moves or rewrite the comment to drop the "awaiting
redistribution" framing.

### 36. `command_store.h` exposes a dead inline helper + a domain-policy query

**Where:** `src/repl/command_store.h:56-80, 120-124`

**Smell:**
- `repl_command_store_source_to_line` — 25-line inline definition,
  zero callers
- `repl_command_store_first_non_decl` — introduces a `CMD_VAR_DECLARE`-
  specific policy hook into a header documented as "array mechanics"

**Fix:** Delete the inline helper. Move `first_non_decl` into
`command_store_policy.h` or fold into the export.c caller as static.

### 37. `GLCmd.text[64]` + `var_names[8][16]` carried by every command for CmdType-specific use

**Where:** `src/repl/command.h:101-103`

**Smell:** `sizeof(GLCmd) == 268`; `text[64]` is written once in
CMD_LABEL; `var_names[8][16]` (+ `var_decl_count`) used only by
CMD_VAR_DECLARE. ~196 bytes of per-command overhead that 99% of
commands don't use. Across MAX_COMMANDS × 8 user scenes that's ~14
MB of dead feature overhead.

**Why it matters:** Compounds with #5 (UserScene stack pressure).

**Fix:** Tagged union — `union { struct { char text[64]; } label;
struct { char names[8][16]; int count; } decl; }` keyed on
`CmdType`. Drops `sizeof(GLCmd)` ~50%. Invasive; long-term win.

### 38. `examples.h` and `example_loader.c` expose parallel singular/plural API names

**Where:** `src/repl/examples.h:48-57` vs. `src/repl/example_loader.c:503-513`

**Smell:** `examples.h` uses `repl_examples_*` (plural) for the
catalog-query trio but `repl_example_tag_*` (singular) for tags.
`example_loader.c` adds singular trampolines for the catalog-query
trio. Two names for the same data; new code can pick either.

**Fix:** Pick singular (matches tag/subheading APIs). Convert
callers; delete trampolines.

### 39. Three `g_export_cfg_bridge && g_export_cfg_bridge->get_int ? ... : fallback` ternaries

**Where:** `src/repl/export.c:873-878, 1454-1457, 2678-2683`

**Smell:** `init_bootstrap_toggle_get` (L543-548) already encapsulates
this. Three other sites re-implement verbatim. `repl_cfg_get_int`
in `cfg_baseline.c` does exactly this.

**Fix:** Replace all three sites with `repl_cfg_get_int(slug, default)`.

### 40. Predef-var capture/restore code is duplicated 5× across scenes.c

**Where:** `src/repl/scenes.c:208-213, 294-299, 393-398, 433-438, 458-463`

**Smell:** Same pattern in 5 places. `repl_eval_copy_predef_vars()` /
`_restore_predef_vars()` in `eval.h` already exist; two sites could
use them directly.

**Fix:** Funnel through `repl_eval_copy_predef_vars()` /
`_restore_predef_vars()`. Optionally add
`repl_eval_snapshot_for_userscene(UserScene *)` to absorb the
alias-copy + scratch-array dance.

### 41. Tutorial GRID_THEME literals decoupled from `src/scene/themes.h` enum

**Where:** `src/repl/tutorials.c:164, 167`

**Smell:** Catalog hardcodes integer values for grid themes; comments
name the symbolic constant but no machine link. Reordering
`SceneGridTheme` silently breaks the Feature Tour without a build
failure.

**Fix:** Add a tutorial-side test in `tests/test_tutorial_runner.c`
that goes through the **public catalog accessors** (the catalog's
`g_tutorial_feature_tour_steps[]` array is `static` in
`src/repl/tutorials.c:143` and is not part of the public API).
The Feature Tour has **two** consecutive grid SET steps —
`grid = 10` (`GRID_THEME_RADAR`) at `tutorials.c:164` and
`grid = 6` (`GRID_THEME_FOCUS`) at `tutorials.c:167` — so the
test asserts both literals in catalog order:

```c
/* Find "Feature Tour" by name; do not assume catalog index. */
int n = repl_tutorial_count();
int tour_idx = -1;
for (int i = 0; i < n; i++) {
    if (strcmp(repl_tutorial_name(i), "Feature Tour") == 0) {
        tour_idx = i;
        break;
    }
}
ASSERT_TRUE("Feature Tour exists", tour_idx >= 0);

/* Collect every "grid" SET step in catalog order. Locating by
 * slug + kind (rather than absolute index) keeps the test robust
 * to step reordering — but the assertion still depends on the
 * showcase order Radar-then-Focus matching the on-screen reveal. */
int s = repl_tutorial_step_count(tour_idx);
int grid_steps[8];
int grid_step_count = 0;
for (int i = 0; i < s; i++) {
    const char *slug = repl_tutorial_step_cfg_slug(tour_idx, i);
    if (slug && strcmp(slug, "grid") == 0 &&
        repl_tutorial_step_kind(tour_idx, i) ==
        TUTORIAL_STEP_KIND_SET) {
        if (grid_step_count < 8)
            grid_steps[grid_step_count++] = i;
    }
}
ASSERT_INT_EQ("Feature Tour has two grid SET steps",
              grid_step_count, 2);

/* Each literal in the catalog must equal the enum value the
 * scene-side header exports. The test TU includes themes.h. */
ASSERT_INT_EQ("first grid step is GRID_THEME_RADAR",
              repl_tutorial_step_cfg_value(tour_idx, grid_steps[0]),
              GRID_THEME_RADAR);
ASSERT_INT_EQ("second grid step is GRID_THEME_FOCUS",
              repl_tutorial_step_cfg_value(tour_idx, grid_steps[1]),
              GRID_THEME_FOCUS);
```

If the showcase order changes (Focus-then-Radar) the assertions
flip rather than silently drift — that's the desired failure
mode. If a third grid SET step is added later, the
`grid_step_count == 2` assertion fails loud, prompting the test
author to decide whether to extend the test or pin the count
intentionally.

(Alternative: push slug-to-int into the bridge so the catalog
records `"GRID_THEME_RADAR"` / `"GRID_THEME_FOCUS"` and the test
just asserts string equality. Heavier refactor; the
public-accessor test above is the minimal closure.)

### 42. 25 lines of per-side CatalogTagOps glue remain after closed #12

**Where:** `src/repl/tutorials.c:250-268, 360-394` and
`src/repl/examples.c:1420-1497`

**Smell:** Closed #12 hoisted the algorithm; both .c files still
have ~25 lines of byte-equivalent wrappers (`repl_*_tag_label`,
`_count_for_tag`, `_index_for_tag`, `_visible_tag_count`,
`_visible_tag_at`, `_has_tag`) that just call `repl_catalog_*`
with the local ops struct.

**Fix:** Add `REPL_DEFINE_CATALOG_WRAPPERS(prefix)` macro in
`catalog_tags.h`; or expose `repl_catalog_*` directly and let
callers use the ops pointer.

### 43. `repl_example_tag_bit` / `repl_tutorial_tag_bit` are byte-identical inline functions

**Where:** `src/repl/examples.h:96-101` and `src/repl/tutorials.h:154-159`

**Smell:** Same 6-line body in two headers, differing only by which
`_tag_count()` they call. Both include `<limits.h>` solely for
`CHAR_BIT`.

**Fix:** Move the bit math into `catalog_tags.h`'s ops struct (have
`tag_bit(int)` supplied automatically given `tag_count()`), or
expose a single helper that both call.

### 44. `cmd_emit` silently drops rows past `HELP_CMD_LINES_MAX`

**Where:** `src/repl/help_text.c:280-288, 346-349`

**Smell:** Overflow returns same `n`; subsequent calls also no-op;
help tab silently truncates. No assert, no log. Buffer was bumped
from 192 to 224 in a prior pass; the next bump won't reliably get
caught.

**Fix:** Have `cmd_emit` `assert(n < HELP_CMD_LINES_MAX)`. Better:
emit a stderr warn once per session since this is presentation
code where ASan in release won't fire.

### 45. `help_group_header` switch silently drops new groups

**Where:** `src/repl/help_text.c:256-271`

**Smell:** `ReplHelpGroup` has no `REPL_HELP_GROUP_COUNT` sentinel.
The 11 switch cases and the 11 explicit `cmd_emit_group()` calls in
`repl_help_text_build` (L327-337) are unsynchronized hand-maintained
parallels. Add a new group, `default: return NULL` produces
header-less rows.

**Fix:** Add `REPL_HELP_GROUP_COUNT` sentinel; drive the
`cmd_emit_group` calls from a loop; `help_group_header` reaches
`assert(0)` on unknown groups.

### 46. `g_tabs[3]` and `g_content.tab_count = 3` must agree by hand

**Where:** `src/repl/help_text.c:253, 405`

**Smell:** Two unrelated `3`s. No `STATIC_ASSERT`, no
`ARRAY_LEN(g_tabs)`, no enum.

**Fix:** `g_content.tab_count = (int)(sizeof(g_tabs)/sizeof(g_tabs[0]));`,
or `ReplHelpTabIdx { HELP_TAB_OVERVIEW, ..., HELP_TAB_COUNT }` enum.

### 47. `import_make_repl_tess_line` has three near-identical scrape loops

**Where:** `src/repl/export.c:2238-2371` (134 lines)

**Smell:** Three structurally-identical blocks scraping brace-block
assignments (`_tn` 3 components, `_tc` 4, `_v->pos` 3). Adding a
4th flavor needs a 4th copy.

**Fix:** Extract `parse_tess_brace_block(line, key_prefix,
max_components, out_exprs[][MAX_LINE_LEN], out_count)`; dispatch on
component count for format string.

### 48. `repl_apply_predef_ops` DECLARE+SET_VALUE redundantly validates and re-looks-up the slot

**Where:** `src/repl/apply.c:144-149`

**Smell:** `ReplPredefOp` has a `has_value` field intended to fold
init into declare. `repl_eval_declare_predef_var` has no value
parameter, so apply does declare → O(n) name lookup → patch. The
return value (new slot index) is discarded.

**Fix:** Add `repl_eval_declare_predef_var_with_value(name, value,
err, errsz)` or change the existing signature; use the return as
the slot index.

### 49. ✅ `repl_apply_can_apply_compiled_change` vs. `repl_apply_compiled_change`: preflight validates, apply trusts

**Where:** `src/repl/apply.c:52` vs. `:101-104`

**Smell:** Preflight rejects `change->count > MAX_COMMIT_CMDS`;
apply forwards without re-check. The contract (apply.h:62-65) says
"callers should preflight" but `repl_apply_compiled_change` is
public.

**Fix:** Move preflight rejection into `repl_apply_compiled_change`
too; `_can_apply_compiled_change` becomes the no-mutation peek.

**Status (2026-05-26):** ✅ Closed. `repl_apply_compiled_change` now
re-runs `repl_apply_can_apply_compiled_change` at the top before any
mutation, so a malformed change (over-MAX_COMMIT_CMDS insert, OOB
pos, compound pre-delete + REPLACE_ONE, OOB pre-delete range) can no
longer half-apply — the pre-insert delete cannot fire and then leave
the cmd-store in an unexpected shape. The header docstring's
"callers should preflight" caveat was rewritten to describe the
new internal-preflight contract while still recommending callers
preflight at the wrapper level so they can skip predef-ops /
editor-buffer steps when apply would reject. Regression tests in
`tests/test_repl_compile.c` (the `#49 regression` block at the end
of `main`) bypass `editor_commit_apply_external_change` and call
`repl_apply_compiled_change` directly with four malformed shapes
plus a well-formed sanity case.

### 50. `MAX_SCRATCH_OPS_PER_COMMIT = MAX_COMMIT_CMDS` is wildly oversized

**Where:** `src/repl/compile.h:119-121`, only consumer
`src/repl/compile.c:932-935` (writes `scratch_ops[0]` only)

**Smell:** `ReplCompiledChange` carries `ReplScratchOp scratch_ops
[MAX_COMMIT_CMDS]` (~32+ slots); compile only ever writes slot 0.
~512 bytes wasted per instance; the array bound is misleading.

**Fix:** Reduce to `MAX_SCRATCH_OPS_PER_COMMIT = 1` until the data
model needs more, or inline `ReplScratchOp scratch_op; int
scratch_op_valid;`.

### 51. Five file-level mutable statics for per-frame execution state in executor.c

**Where:** `src/repl/executor.c:22, 40, 54-56, 59, 68`

**Smell:** `g_execute_alpha_scale` and `g_execute_skip_geom_before_pc`
are per-frame state set via `repl_execute_set_fade_context`. Anyone
writing `repl_execute_program(&opts)` without remembering to
set/clear the fade context gets whatever the previous frame left.

**Fix:** Move both into `ReplExecutionOptions` (as
`float fade_alpha_scale; int skip_geom_before_pc;` defaulting to
`1.0f / 0`). Delete `repl_execute_set_fade_context`.

### 52. `command_spec.c` enum-spec terminator uses positional zeros where rows use designators

**Where:** `src/repl/command_spec.c:384`

**Smell:** Every row above uses `.args =` designator (mixed with
positional fields); the terminator drops to 5 positional fields for
a 6-field struct (`args[]` zero-init). Different convention again
at the std-command terminator (`:407` uses 6 fields). Three
patterns in the same file.

**Fix:** Use `{ NULL }` (C99 zero-fill) or `{ .name = NULL }` to
mirror the designator style.

## 🟢 Dead code / dead fields

### 53. Six of seven `repl_format_*` functions have zero production callers — `format.c` is mostly dead

**Where:** `src/repl/format.c:9-82`

**Smell:** Searched every `.c` under `src/`, `tools/`, `bench/`.
Outside `tests/test_format.c`, the only production caller is
`core.c:198` invoking `repl_format_reindent_from_parsed`. The other
seven functions (`_tess_depth`, `_begin_depth`, `_indent`,
`_end_indent`, `_tess_end_indent`, `_tess_leaf_indent`,
`_reindent_expr`) and the `ReplFmtCmd` / `ReplFmtType` types exist
only for tests.

**Why it matters:** Biggest single cleanup in the audit. ~80 lines
of format.c, ~120 lines of format.h, plus the test rationale
evaporates.

**Fix:** Either (a) wire production formatter callers
(`compile.c::compile_scope_indent`, parser's indent ladder,
`autonormal.c::normal_indent`) onto these helpers, or (b) delete
the dead seven + reduce `format.{c,h}` to `_reindent_from_parsed`
only and inline into core.c (its sole caller).

### 54. `compile_scope_indent` is a one-line forwarder with a stale comment

**Where:** `src/repl/compile.c:184-187`

**Smell:** `static void compile_scope_indent(...) { repl_source_scope_cmd_indent(...); }`.
One caller (`compile.c:977`). Comment references
`fill_scope_indent` in editor_commit.c — function no longer exists.

**Fix:** Inline the one call site; delete the wrapper.

### 55. `make_auto_normal`'s `insert_pos` parameter has `(void)insert_pos;`

**Where:** `src/repl/autonormal.c:36-50`

**Smell:** Two callers pass real values that are discarded. Same
shape as closed #30 (`flatten_src_text`'s `src_cmd` dead param).

**Fix:** Delete the parameter; drop the cast.

### 56. `flatten_get_for_var_name`'s `cmd` parameter is dead

**Where:** `src/repl/flatten.c:58-70`

**Smell:** `(void)cmd;` — same shape as #55 and the closed #30.

**Fix:** Delete the parameter.

### 57. `repl_executor_camera_distance_source()` getter has zero production callers

**Where:** `src/repl/executor.c:28-30`, declared in `executor.h:112`

**Smell:** The setter is used by `glr_ctrl.c:2277`; the getter only
by tests. Pure test seam in production header.

**Fix:** Drop the getter (the test can do round-trip via the setter)
or move to `executor_test_internal.h`.

### 58. `args[1] = 0.0f;` reset in `eval_primary` is a no-op

**Where:** `src/repl/eval.c:1002-1005`

**Smell:** `args` already initialized to `{0.0f, 0.0f}`; conditional
re-zero is dead.

**Fix:** Delete the conditional.

### 59. Defensive `if (lnv < MAX_EXPR_VARS)` guard in `flatten_for_loop` can never fail

**Where:** `src/repl/flatten.c:237-245`

**Smell:** Per-iteration dead branch — `MAX_EXPR_VARS == 32` and
`lnv == 0` at that point.

**Fix:** Hoist the iterator binding out of the if.

### 60. `vis_total` in `repl_load_apply_line` is populated but never read

**Where:** `src/repl/load.c:177-179`

**Smell:** `vis_total` carries the untruncated count for "scope
truncation" warnings; `editor/input.c` uses it via
`warn_if_scope_truncated`. The loader silently drops it.

**Fix:** Emit the warning through `err`, or remove the parameter
from the loader's call.

### 61. Dead `(void)warnings;` cast at end of `parse_snippet_declare`

**Where:** `src/repl/export.c:1878`

**Smell:** `warnings` is dereferenced multiple times in the body
(L1799, 1814, 1851, 1873); the trailing `(void)warnings;` is
vestigial.

**Fix:** Delete the line.

### 62. `!section->enabled` check in `emit_export_scaffold` is dead

**Where:** `src/repl/export.c:2884-2891`

**Smell:** Every entry of `EXPORT_SCAFFOLD_SECTIONS[]` has a
non-NULL `.enabled` field. The null-check never fires.

**Fix:** Drop the `!section->enabled ||` half; drop the
`export_section_always` thunk too.

### 63. `off` accumulator in `write_canonical_cmd_as_c` is `(void)`-ed

**Where:** `src/repl/export.c:1289-1298`

**Smell:** `off += fprintf(...)` accumulated then `(void)off;` —
never bounds-checked, never used.

**Fix:** Delete the `int off =` and `+= ` operators; call `fprintf`
without aggregating.

### 64. `ReplayBeforeStepFn ctx` parameter is never used

**Where:** `src/repl/replay_annotations.c:57-60, 680, 723`

**Smell:** Typedef carries `void *ctx`; the only callback ignores
it; the only caller passes NULL; the other caller passes no
callback at all.

**Fix:** Drop `void *ctx` from typedef and function signature.

### 65. `replay_subst_scratch_reads` recomputes `len = (int)(p - start)` redundantly

**Where:** `src/repl/replay_annotations.c:203, 209`

**Smell:** L209 reassigns `len` to the same expression already
evaluated on L203 — dead store.

**Fix:** Delete L209.

### 66. `state.c` has 13 file-scope macros with no readers

**Where:** `src/repl/state.c:150-168`

**Smell:** Counted by name inside state.c: 13 of 25 macros have a
use count of 1 (the definition itself). Leftover hooks from the
pre-`ReplRuntimeState` era. Each is a foot-gun in waiting (an
editor who reaches for `g_workspace_dir` would bypass the typed-
facade policy).

**Fix:** Delete the 13 unused macros. The 12 used ones can stay (or
be inlined for clarity).

### 67. Stale "phase N.M commit X" comment crumbs across multiple files

**Where:** `src/repl/parser.c:29-30, 889`; `parser.h:38, 62-65`;
`compile.c:171-173`; `compile.h:79, 214-217`;
`src/repl/executor.c:177-178`; `pipeline.h:19`; `eval.h:42-88`

**Smell:** Construction-history references ("phase H.5 commit 40",
"β forbids", "implemented in phase 3.6.3"). Navigational during
migration; noise now. Some reference deleted modules (e.g. `eval.h`
mentions `editor_undo.h`, `replay.c / glr_ctrl.c`, `FlatScope`
which doesn't exist).

**Fix:** Mechanical sweep — drop phase coordinates; keep policy
statements where useful.

## 🔵 Structural concerns

### 68. `repl_execute_program` is a 324-line god switch (the new #39/#40-shaped target)

**Where:** `src/repl/executor.c:388-711`

**Smell:** Dispatches on ~30 CmdTypes, embeds CMD_LABEL printf
rewriting (40 lines), CMD_GOTO label search (30 lines), and
CMD_IF_BEGIN evaluation (30 lines). With closed #39 (`parse_command`
903→456) and the original-audit #40 (`flatten_range` 378→160 at
some point in the closeout), this is the obvious next target.

**Fix:** Extract per-CmdType handlers `exec_label`, `exec_goto`,
`exec_if_begin`, `exec_glut_shape`; roll state-cmd cases into a
fall-through to `repl_apply_state_cmd` (overlaps with #28). Adds
the function to the size ratchet.

### 69. `export.c` is still 3254 lines after the closed #38 extraction

**Where:** `src/repl/export.c`

**Smell:** Closed #38 moved ~1500 lines of generic helpers out.
Post-extraction the heaviest functions are import-side translators:
`import_make_repl_*` (134 + 92 + 68 + 67 + 57) and
`write_canonical_cmd_as_c` (176 lines). The save/load split is
natural and the file is begging for it.

**Fix:** Split into `src/repl/export.c` (writer side) +
`src/repl/import.c` (new — `repl_export_load_from_file`,
`ImportState`, all `import_*` functions, deferred-var apply,
snippet directive table). Both halves ~1500/1700 lines, similar to
post-Tier-C parser.c.

### 70. `eval.h`'s `MAX_EXPR_VARS` doc is mislabelled and references deleted modules

**Where:** `src/repl/eval.h:42-88`

**Smell:** Block heading says `MAX_EXPR_VARS - global user-declared
variables` but the body describes `MAX_PREDEF_VARS`. The "Used in"
list cites `editor_undo.h` (now `src/editor/undo.h`), `replay.c /
glr_ctrl.c` (replay is now `src/subsystems/replay/replay.c`), and
`FlatScope` (does not exist anywhere).

**Fix:** Swap the two macro labels, update three paths, drop the
`FlatScope` reference.

### 71. `g_predef_vars` macros always route reads through `_mut()`

**Where:** `src/repl/eval.h:172-173`

**Smell:** Every read site goes through the mutable accessor. The
`repl_eval_predef_view()` read-only API exists but is barely used.

**Fix:** Add `repl_eval_predef_vars(void)` returning `const ExprVar *`;
have the macros route reads to it. The mutating helpers keep using
`_mut()` internally. Then a ratchet over `\bg_predef_vars\b`
outside writer helpers becomes possible.

### 72. `repl_state_init_defaults` is a one-line forwarder to `repl_state_reset_program`

**Where:** `src/repl/state.c:532-534`

**Smell:** Two names mean the same thing; demo uses one, glr_ctrl
the other. The names imply different semantics that don't exist.

**Fix:** Delete one; collapse the 4-5 callers.

### 73. `repl_state_get_defaults()` has a hidden mutation side effect

**Where:** `src/repl/state.c:88-101`

**Smell:** Function named `get_defaults` returning `const
ReplRuntimeState *` patches the live `g_repl_state` on first call.
Comment justifies it; static analyzer / reader won't expect it.

**Fix:** Split into `repl_state_defaults()` (pure read) and
`repl_state_ensure_sentinels()` (the side effect path); have
`repl_state_reset_program` call the second explicitly.

### 74. `command_store.h::repl_command_store_first_non_decl` leaks `CMD_VAR_DECLARE` policy

(also covered in #36)

### 75. `repl_dispatch_*` family: 4 of 12 trampolines are tutorial-only

**Where:** `src/repl/core.c:105-165`, `src/repl/core.h:134-208`

**Smell:** `host_cursor_park`, `completion_clear`, `completion_update`,
`host_input_get` have only tutorial.c callers in production. The
"host effects" framing suggests broad applicability — they're
really a private tutorial-to-controller path.

**Fix:** Rename to reflect tutorial-only usage, or move into a
separate `ReplTutorialHostEffects` struct.

### 76. `scenes.c::WORKSPACE_DIR_MAX` alias + `+ 256` magic filename allowance

**Where:** `src/repl/scenes.c:139, 746`

**Smell:** Macro `WORKSPACE_DIR_MAX REPL_WORKSPACE_DIR_MAX` is pure
noise. `+ 256` filename allowance assumes POSIX NAME_MAX=255 but
never references the actual limit.

**Fix:** Delete the alias. Replace `+ 256` with `+ NAME_MAX + 1`
(after `#include <limits.h>`) or a named constant.

### 77. `scenes.c` "Observably overwritten…" comment marks a no-op call kept "for future-proofing"

**Where:** `src/repl/scenes.c:308-312`

**Smell:** Comment explicitly says the call is observably a no-op
today, kept against speculative future behavior. Reader has to
parse the comment to know whether they can delete the line.

**Fix:** Either delete the call (add a comment about when it'd be
needed) or commit to the sparse-cfg change as a real plan.

### 78. `core.c::scroll_to_display_function` brittlely string-matches `"void display() {"`

**Where:** `src/repl/core.c:813-823`

**Smell:** `g_header_pre` is owned by `export.c`; if a future change
renames the function or adds spacing, this scroll helper silently
breaks.

**Fix:** Expose `#define G_HEADER_DISPLAY_LINE "void display() {"`
in `export.h` referenced from both sides, or add a "display function
start index" accessor.

### 79. `format_evaluated_cmd`: dead `case 5:` / `case 6:` mask a future single-arg drop

**Where:** `src/repl/replay_annotations.c:1061-1070`

**Smell:** `eval_fmt_for_type` only ever returns `nargs ∈ {0,1,2,3,4}`.
Cases 5/6 are dead but read `cmd->args[4..5]` (in-bounds of the
8-slot array, but unreachable). Looks defensive but is misleading
— hides the same dispatch shape that produced the original #1
single-arg drop.

**Fix:** Replace the whole switch with a loop or single `vsnprintf`
over `cmd->args[0..nargs-1]`. `assert(nargs >= 1 && nargs <= 4)`.

### 80. `s_replay_text_view` is hidden file-level state with set-then-read semantics

**Where:** `src/repl/replay_annotations.c:49, 837, 1140, 63`

**Smell:** Documented as "set at public entry points, read by file-
private helpers." `replay_annotations_invalidate()` does not reset
it; if a caller invokes `replay_code_panel_get_command_display_text`
after the parent frees the view storage, this reads stale memory.

**Fix:** Thread `SourceTextView` through the private function
signatures (verbose but explicit), or wrap reads in
`replay_view_valid_or_warn()`.

### 81. `TUTORIAL_LOCKED_LINE_MAX` doubles as runtime cap and catalog validator ceiling

**Where:** `src/repl/tutorials.h:45`

**Smell:** One constant carries two meanings (max steps per tutorial
vs max simultaneously-locked source lines). Comment acknowledges;
agreement is structural rather than enforced.

**Fix:** Split into `TUTORIAL_MAX_STEPS` and `TUTORIAL_LOCKED_LINE_MAX`;
pin the relationship via STATIC_ASSERT if it must hold.

### 82. `tutorial_step_at()` does O(N) sentinel walk per accessor call

**Where:** `src/repl/tutorials.c:277-293`

**Smell:** Each of 8 accessors (`_step_comment`, `_step_expected`,
…) calls `tutorial_step_at(idx, step_idx)` which linearly walks
from index 0. Rendering a step row calls 5+ accessors → O(N²) per
tutorial paint.

**Fix:** Expose `const TutorialStep *repl_tutorial_step_get(int, int)`
returning the whole struct; let the menu reader walk fields off
the pointer. Keep the per-field getters as inline shims.

## Sequencing

### One-afternoon pass — focused on bugs + dead code

1. **#3** — predef-values count bug. One-line save/restore signature
   change.
2. **#11** — source_scope buf_sz≤0 guard. Two lines.
3. **#9** — load-side fclose/ferror check. Mirror save-side.
4. **#13** — `repl_eval_parse_exprs` doc fix or behavior fix.
5. **#53** — delete six dead `repl_format_*` functions. ~80 lines
   of format.c, ~120 lines of format.h.
6. **#54** + **#55** + **#56** + **#57** + **#58** + **#59** +
   **#60** + **#61** + **#62** + **#63** — mechanical dead-code
   removal across multiple files.
7. **#64** + **#65** + **#66** — replay_annotations + state.c
   dead-code/dead-macros cleanup.
8. **#67** — phase-N.M comment sweep across the layer.

(*#10 `flatten_source_lighting_enabled` is a real correctness
fix but touches the flatten contract enough to be Tier B —
deferred from the afternoon pass to keep tier classifications
consistent. See the "Tier-classified outstanding work" section
below.*)

That's ~9 commits; net ~400-500 LOC reduction; closes three
bug-level findings.

### One-week pass — the cluster work

The dominant work is **closing the `_mut()`-for-reads regression
(#14)**, **finishing the half-done Tier B items (#29, #31)**, and
**addressing the executor / export god functions (#68, #69)**.

1. **#14** — per-file sweep replacing reads with const accessors
   across source_scope.c, autonormal.c, scenes.c, export.c,
   replay_annotations.c. Then add the `check-state-ownership`
   ratchet to prevent regression. This was the audit's #37 ratchet
   that never landed.
2. **#7** — folds into #14.
3. **#15** — extract shared parse cores for the four block kinds
   so editor_compile_* and repl_compile_* share validation. Closes
   the largest remaining drift surface (#15 line count: ~430 +
   ~860).
4. **#16** — extend `repl_compile_dispatch` to cover all six entry
   points, or rename it.
5. **#17** + **#20** + **#21** + **#25** — eval.c / parser.c
   helper extraction sweep.
6. **#29** — finish Tier B #44's roll-into-discriminator.
7. **#31** — promote text_helpers.c declarations into a sibling
   `text_helpers.h`.
8. **#32** — name the four 16/64 magic widths in shared headers.
9. **#5** — heap-allocate UserScene stash buffers (or hoist a single
   static scratch). Real overflow risk.
10. **#37** — tagged union for GLCmd's CmdType-specific fields.
    Invasive; the natural follow-up to #5.

### One-week pass — finish the file-size reductions

1. **#69** — split `export.c` into `export.c` (writer side) +
   `import.c` (reader side). ~1500/1700 each, mirroring the
   parser.c diet.
2. **#68** — split `repl_execute_program` into per-CmdType handlers.
3. **#47** — extract `parse_tess_brace_block` for the 134-line
   import function.

### Tier-classified outstanding work

Following the Tier A/B/C/D system the prior audit landed:

- **Tier A (small, near-zero risk):** #3, #9, #11, #13, #34, #38,
  #39, #40, #46, #52, #53-#67 — most of the afternoon pass.
- **Tier B (moderate, focused pass):** #2, #4, #5, #6, #8, #10,
  #15, #29, #31, #32, #33, #44, #45, #48, #50, #51, plus the
  per-file parts of #14.
- **Tier C (high cost, broad surface):** #14 (ratchet), #16, #37,
  #68, #69, #41 (test-side change for tutorial enum coupling).
- **Tier D (kept on purpose):** #75 (the tutorial-only dispatch
  trampolines could stay if renamed and grouped; consult tutorial
  owner before "fixing"), #80 (`s_replay_text_view` — documented
  trade-off).

### Out of scope

- The two-level command model (source → flat) — load-bearing.
- `examples.c`'s 1496 lines of mostly-data — touch only when
  bumping example content.
- `command.h`'s control-flow predicates (`repl_cmd_is_transform`,
  etc.) — well-named, well-used.
- The closed audit's #39 (`parse_command` extraction) and #40
  (`flatten_range` extraction) — Tier C in the original audit;
  both have been worked down (parse_command 903→456,
  flatten_range 378→160), no further extraction urgent.

## Method note

This audit was produced by five parallel review agents, each scoped
to a slice of `src/repl/`:

- parse / compile / apply / command_spec / source_scope
- eval / flatten / executor / autonormal / format
- export / load / text_helpers / cfg_baseline
- state / scenes / examples / example_loader / core / command_store
- tutorials / help_text / replay_annotations / catalog_tags

Each agent was given the prior audit's "Backlog closeout" section
plus the original audit's tier-tagged findings and instructed
**not to re-flag items already ✅ done**, but to flag regressions,
new smells introduced by cleanup, or items the prior audit closed
only partially.

Verified prior-audit closures held: #1 (single-arg drop), #38
(text_helpers extraction), #41 (`func_def_idx`), #42 (`var_idx`),
#43 (`display_name`), #44 (cfg embed), #45 (`mark_source_dirty`),
#46 (predef storage). No regressions detected in those.

The dominant theme this pass is **half-finished cleanups**: the
prior audit's individual fixes mostly landed, but the broader
ratchet/policy that would have prevented re-occurrence didn't —
hence #14 (`_mut()` regression in 5 files), #29 (#44's discriminator
half), #31 (text_helpers' missing header), #67 (phase comments
that survived #11). The non-trivial new findings are #1 (layering
breach), #2 (cfg double-apply correctness), #3 (predef-values count
bug), and #5 (UserScene stack pressure — direct consequence of the
otherwise-clean Tier B #44).
