# `src/repl/` Audit-2 — Tier A Implementation Plan

Status: **in-review** — concrete commit sequence for the Tier A
findings from `plans/in-review/src-repl-code-smell-audit-2.md`
(2026-05-26). Each batch lists files, target diffs, and a verify
gate. No Tier B/C items here; those wait for their own plans.

## Scope

This plan closes the 22 Tier A findings from the audit. The list
splits cleanly into three batches, each independently committable
and build-safe:

| Batch | Findings | Net LOC | Risk |
|---|---|---|---|
| B1 — Real bugs | #3, #9, #11, #13 | +60 / -20 | low for #9/#11/#13; **moderate** for #3 — the cascade through `ReplayRuntimeState` + `ReplayFadePlan` touches 5 files. May split as B1.1a / B1.1b — see B1.1 sizing note |
| B2 — Dead-code sweep | #53–#67 | +20 / -350 | near-zero (every deletion verified zero callers) |
| B3 — Naming + duplication + couplings | #34, #38, #39, #40, #46, #52 | +40 / -80 | low (mechanical; B3.2 churn-check first) |

(An earlier draft contained a "B4 — apply.c hardening" batch
labelled `#46`. That was a finding-number mix-up: audit `#46` is the
`help_text.c` tab-count coupling — Tier A; the apply.c
DECLARE+SET_VALUE redundancy is audit `#48` — Tier B. `#48` is
deferred below; the real `#46` lives in B3 now.)

**Out of scope** (deferred to a separate plan):
- #1 (help_text.c layering breach) — needs a design call between
  (a) move the file to `src/app/` or (b) inject F-key data via
  callback. Tier B sized.
- #2 (parse_cfg double-apply) — needs a decision on which path is
  the source of truth. Tier B sized.
- #5 (UserScene stack pressure) — real overflow risk but the fix
  (heap allocate vs. static scratch) is a design call worth
  isolating. Tier B sized.
- #14 (`_mut()` regression across 5 files + ratchet) — Tier C; the
  cross-file sweep + `check-state-ownership` script is the largest
  single piece of leftover work.
- #48 (apply.c DECLARE+SET_VALUE redundancy) — Tier B per the
  audit's tier list (line 1273). The clean fix introduces
  `repl_eval_declare_predef_var_with_value(...)`, which is an API
  shape change worth isolating; defer to a Tier B plan.
- All structural items #68–#82 — Tier B/C.

## Verify gates

After every batch:

```
make check-c99       # syntax-only ratchet (real gcc -std=c99)
make test-stubs      # all binaries + tests; no regression vs. branch start
```

The plan assumes you're on a fresh feature branch off `main`. If
any gate fails after a batch, revert that batch's commits and
re-plan; do not paper over with follow-up fixes inside the same
batch.

---

## Batch 1 — Real bugs (#3, #9, #11, #13)

Four tight correctness fixes. Each is its own commit so the bug
fix shows up in `git log` independently of cosmetic sweep.

### Commit B1.1 — #3 predef-values snapshot: cascade the full-snapshot pair through every long-lived path

**File set (the cascade — they have to move together):**
- `src/repl/eval.h:229-235` — contract comment.
- `src/subsystems/replay/replay.c:841, 975-988, 1170` — the three
  replay-baseline sites and the two helpers
  `replay_copy_baseline_predef_values` /
  `replay_restore_baseline_predef_values` exported in
  `replay.h:179-180`.
- `src/subsystems/replay/replay_state.h:52` —
  `ReplayRuntimeState.baseline_predef_vals` storage; widen to also
  hold names + count.
- `src/app/glr_ctrl.h:12-22` — `ReplayFadePlan` struct; widen to
  match (`baseline_predef_names`, `baseline_predef_count`).
- `src/app/glr_ctrl.c:226, 264` — the values-only restore + copy
  on the fade-plan storage; switch to the full-snapshot pair.

The frame-level pair at `glr_ctrl.c:1844, 1953` and the autonormal
scratch at `flatten.c:677, 681` stay on the values-only pair —
they are intra-frame and intra-pass respectively, no source / predef
mutation can interleave there.

**Why the partial fix isn't enough (reviewer pushback):**
- An earlier draft returned the captured count from
  `repl_copy_predef_values` and made `repl_restore_predef_values`
  accept it. That fixes overread on *grow* but does **not** fix the
  actual triggerable bug — a *reorder* or *shrink* of the predef
  table between copy and restore makes the saved floats land in
  the wrong slots after restore, because the values-only pair
  carries no names.
- The next draft proposed switching only `replay.c`'s three sites
  to the full-snapshot pair. That was still incomplete:
  `ReplayFadePlan.baseline_predef_vals` is built from the replay
  baseline via `replay_copy_baseline_predef_values` (values-only,
  `replay.c:986`) and read back at `glr_ctrl.c:226`. If
  `ReplayRuntimeState` gains names + count but
  `replay_copy_baseline_predef_values` and `ReplayFadePlan` stay
  values-only, the fade restore silently truncates names back to
  floats and the same bug persists on the fade-tick path.
- The cascade is therefore the minimum sufficient fix.

**Triggerable failure today:** a workspace switch (or any source /
predef-table mutation routed through user input) while replay is
active rebinds the predef table; the next fade-plan restore writes
saved floats into slots holding different vars. Tutorial steps are
**not** a trigger — tutorial SET/REQUIRE step kinds carry cfg
slugs only, validated through `repl_cfg_known` (tutorial.c:440,
455); the catalog validator hard-rejects unknown slugs. Predef
declarations have no tutorial-step encoding.

**Alternative scoping option (worth weighing before committing):**
stop replay before any source / predef-table mutation (workspace
load, scene switch, undo across an `@declare`, F12 cycle). That
shrinks the cascade to "audit-and-document" — every long-lived
snapshot is then guaranteed not to span a table mutation, and the
values-only pair becomes safe everywhere. Smaller code change but
a UX policy decision (does the user lose their replay state on
every workspace load?). Pick this *before* expanding the cascade
if the answer is "yes, that's acceptable"; otherwise the cascade
is the implementation path.

**Callers to inspect — do a live sweep before editing:**

```bash
rg -n "repl_(copy|restore)_predef_values" src/ tests/ bench/
rg -n "baseline_predef|ReplayFadePlan" src/
```

At time of writing the second sweep returns: `replay_state.h:52`
(storage), `replay.c:841, 975-988, 1170` (the three sites + two
helpers), `replay.h:179-180` (the helper declarations),
`glr_ctrl.h:12-22` (fade-plan struct), `glr_ctrl.c:224-264`
(restore + copy on the fade plan). These are all in the cascade.

**Struct-size note:** both `ReplayRuntimeState` and
`ReplayFadePlan` grow by ~`MAX_PREDEF_VARS * REPL_PREDEF_NAME_MAX`
bytes (≈ 384 B at the current 24×16 dimensions). If either struct
is `memcpy`'d wholesale (e.g., in `replay_state_capture` /
`_restore`), the larger struct flows through the existing copy
paths automatically — fine for correctness, but worth a
`grep -n "ReplayRuntimeState\|ReplayFadePlan" src/` sweep before
committing to confirm nothing assumes the old layout (a sibling
struct paired with a fixed-size header, etc.).

**Contract comment:** update `eval.h:229-232` to spell out the
table-shape-stable prerequisite the comment currently elides
("values-only; caller must guarantee the predef table shape is
unchanged between copy and restore — see the full-snapshot pair
above for callers that can't").

**Verify:** add a unit test in `tests/test_repl_executor.c` that
exercises the *reorder* path through the full-snapshot pair (this
is the bug the cascade is closing):

1. Declare three vars (X, Y, Z) with distinct values
   (recalling that A/B/C are reserved scratch-array names —
   see the `reserved_predef_var_names` note in the project memory).
2. `repl_eval_copy_predef_vars(buf_vals, buf_names, &n);`
3. `repl_eval_undeclare_predef_var("Y")` — table shrinks, indices
   shift so Z now sits where Y used to be.
4. `repl_eval_restore_predef_vars(buf_vals, buf_names, n);`
5. Assert: X, Y, Z are all back, with their original values.
   Crucially, Z's value is **not** the old Y value, which is what
   the values-only pair would silently produce.

If we instead picked option 3 from the audit (add an assert to the
values-only restore), this test would assert-trip rather than
verify-correct; the audit's option 1 (full-snapshot cascade) is
the one this plan implements.

**Out of scope of this commit:** A separate API rename / deletion
of `repl_copy_predef_values` would be Tier B (signature change
across all remaining call sites). Keep the values-only pair for
the two intra-frame/intra-pass callers that don't need names; the
cascade just upgrades the long-lived paths.

**Sizing note:** with the cascade through both `ReplayRuntimeState`
and `ReplayFadePlan`, this commit is at the upper edge of the
"tight correctness fix" envelope — five files instead of one or two.
If review pushback prefers, split into `B1.1a` (replay-side
storage + three sites) and `B1.1b` (fade-plan storage + the two
controller sites). Each half is independently build-green because
the two helpers in the middle (`replay_copy_baseline_predef_values`
/ `_restore_baseline_predef_values`) can be kept signature-compatible
during the transition by storing names + count internally even when
the call sites still pass only floats. A clean two-commit split is
preferable to a one-commit five-file change; the only reason to do
it as one commit is if the intermediate state would leave the
fade path broken.

### Commit B1.2 — #9 load-side `fclose` / `ferror` check

**File:** `src/repl/export.c:3157-3158` (and the surrounding
`fgets` loop in `repl_export_load_from_file`)

**Current shape:**
```c
while (fgets(line, sizeof(line), f)) { ... }
if (fclose(f) != 0)
    return 0;
```
`fclose` failure is silent; `ferror` never checked.

**Target shape:** Mirror the save-side pattern (already in this
file post-prior-audit #6):
```c
while (fgets(line, sizeof(line), f)) { ... }
int had_read_err = ferror(f);
int close_failed = (fclose(f) != 0);
if (had_read_err || close_failed) {
    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Error: cannot read %s", path);
    repl_set_status_error(msg);
    return 0;
}
```

**Verify:** no new test; manual sanity (open a workspace file,
verify no regression). The fix surfaces an error path that
previously was silent — there's nothing to assert "old behavior"
about.

### Commit B1.3 — #11 source_scope buf_sz≤0 guard

**File:** `src/repl/source_scope.c:92-104` (`cmd_indent`) and
`:145-156` (`cmd_tess_indent`)

**Current shape:** Both functions skip the guard that their two
siblings (`begin_indent` L111, `tess_close_indent` L122) have. With
`buf_sz == 0`, the `spaces` clamp lands at 0 and `buf[0] = '\0'`
writes one byte past the zero-length buffer.

**Target shape:** Add `if (buf_sz <= 0) return;` at the top of
both, matching the siblings.

**Verify:** unit test in `tests/test_repl_core_extra.c` (or a new
slot) calling each with `buf_sz = 0` — should be a no-op with no
write.

### Commit B1.4 — #13 `repl_eval_parse_exprs` doc-fix

**File:** `src/repl/eval.h:267-271`

**Current shape:** Header says `"Returns the number of expressions
parsed (up to max), -1 on error."` — implementation only returns
`n` (loop break on no progress). Callers that check for `-1` treat
malformed lists as "zero args, success."

**Target shape:** Drop the `-1` clause from the doc. This is the
safer of the two fixes (no behavior change). If a future caller
actually needs the error signal, they can switch to a different
parser entry that does provide it.

```c
/* Returns the number of expressions parsed (up to max). Stops on
 * malformed input; the caller distinguishes "stopped short" by
 * comparing against the expected count. */
int repl_eval_parse_exprs(const char *src, float *out_vals, int max,
                          const ExprVar *vars, int num_vars);
```

**Verify:** no test change needed; this is a pure doc fix.
Recompile sanity-check is enough.

---

## Batch 2 — Dead-code sweep (#53–#67)

Mechanical removals. Bundle these as ONE commit per cluster so the
sweep is auditable without paper-cut commits. The big one is #53
(~200 LOC out of `format.c` / `format.h`).

### Commit B2.1 — #53 delete dead `repl_format_*` functions

**File:** `src/repl/format.c:9-82`, `src/repl/format.h:1-121`,
`tests/test_format.c` (delete the corresponding test cases)

**Current shape:** Seven `repl_format_*` functions with zero
production callers. The only live one is
`repl_format_reindent_from_parsed` (called once by `core.c:198`).
Plus `ReplFmtCmd` / `ReplFmtType` types exist only for the dead
formatters.

**Delete:**
- `repl_format_tess_depth`
- `repl_format_begin_depth`
- `repl_format_indent`
- `repl_format_end_indent`
- `repl_format_tess_end_indent`
- `repl_format_tess_leaf_indent`
- `repl_format_reindent_expr`
- `ReplFmtCmd`, `ReplFmtType` (if unused after the above)
- All `tests/test_format.c` cases that exercise the deleted functions

**Keep:** `repl_format_reindent_from_parsed` (and any helpers it
genuinely needs).

**Verify:** `make test-stubs` — `test_format` will shrink but
should still pass for the one remaining function. Confirm no other
binary referenced the deleted symbols (linker would have caught it
otherwise, but a `grep -rn "repl_format_tess_depth\|repl_format_begin_depth\|..."`
sweep before deleting catches stragglers).

### Commit B2.2 — Dead parameters and one-line forwarders

Mechanical fixes, one file each (or grouped if they're in adjacent
sections):

- **#54** — `src/repl/compile.c:184-187` — delete
  `compile_scope_indent` wrapper; inline the one call site at L977.
- **#55** — `src/repl/autonormal.c:36-50` — delete `insert_pos`
  parameter from `make_auto_normal`; both callers (L261, L276) drop
  the discarded arg.
- **#56** — `src/repl/flatten.c:58-70` — delete `cmd` parameter
  from `flatten_get_for_var_name`; single caller at L211 simplifies.
- **#57** — `src/repl/executor.c:28-30, h:112` — delete the
  `repl_executor_camera_distance_source` getter (test-only seam).
  Update `tests/test_repl_executor.c` to use round-trip via the
  setter pointer.
- **#58** — `src/repl/eval.c:1002-1005` — delete dead `args[1] =
  0.0f` reset and the surrounding conditional.
- **#59** — `src/repl/flatten.c:237-245` — collapse the always-true
  `if (lnv < MAX_EXPR_VARS)` guard.
- **#61** — `src/repl/export.c:1878` — delete dead `(void)warnings;`
- **#62** — `src/repl/export.c:2884-2891` — the audit's "drop both"
  prescription is internally inconsistent: the loop's `section->enabled`
  call (L2888) currently relies on every row having a non-NULL
  thunk, and seven of the eleven rows in `EXPORT_SCAFFOLD_SECTIONS[]`
  (L2870-2882) name `export_section_always` to satisfy that.
  Two coherent options:
  - **(a) drop the thunk, keep the null check** — switch
    `export_section_always` rows to `NULL` (now the
    "always-on" signal); the `!section->enabled` half of the
    condition becomes load-bearing as that signal's reader.
    Strictly more code deleted (one thunk gone, no caller changes
    elsewhere).
  - **(b) keep the thunk, drop the null check** — `export_section_always`
    becomes the documentation that a row is always on; the loop
    just calls `section->enabled(ctx)` unconditionally.
  Pick **(a)**: it deletes the dead thunk *and* keeps the
  call-site self-explanatory (`NULL` enabled = always on). Either
  way, do not delete both at once — the build will pass but the
  loop will then call NULL on every "always" row.
- **#63** — `src/repl/export.c:1289-1298` — delete `int off =` and
  `+=` operators in `write_canonical_cmd_as_c`.
- **#64** — `src/repl/replay_annotations.c:57-60, 680, 723` — drop
  `void *ctx` from `ReplayBeforeStepFn` typedef and signature; the
  one caller passing NULL collapses.
- **#65** — `src/repl/replay_annotations.c:209` — delete redundant
  `len = (int)(p - start);`.

**Verify:** `make check-c99 && make test-stubs`. Each removal is
verified-no-callers by the build linker; tests should be unchanged.

### Commit B2.3 — #60 surface `vis_total` from `repl_load_apply_line` *(moved out of the dead-code batch — this is a behavior change)*

**Note on categorization:** An earlier draft of this plan grouped
#60 with the dead-code sweep because the local read is missing.
But the fix turns a previously-silent truncation into a real
diagnostic — that is a behavior change. It properly belongs with
the correctness commits in Batch 1 (or could move to Batch 3 as a
coupling fix). It stays here for plan-numbering stability, but
**treat it like a B1.x commit for review and verification
purposes**: hold the rest of Batch 2 if this one needs to be
revisited.

**File:** `src/repl/load.c:177-179`

**Current shape:** `int vis_total = 0;` populated but never read;
truncation is silent in import. The interactive editor surfaces
the same condition via `warn_if_scope_truncated`.

**Target shape:** Emit the warning to **stderr**, not the `err`
buffer. The loader processes many lines per invocation; the `err`
buffer feeds the status bar (one slot), so each line would
overwrite the previous and the user would see only the warning for
the *last* truncated line. stderr keeps every warning visible:

```c
int vis_total = 0;
int num_vis_vars = collect_visible_vars(insert_idx, vis_vars,
                                        MAX_EXPR_VARS, &vis_total);
if (vis_total > MAX_EXPR_VARS)
    fprintf(stderr, "load: scope truncated at line %d: "
                    "%d vars visible, only %d used\n",
            line_idx, vis_total, num_vis_vars);
```

(If `line_idx` isn't already in scope, pull it from the caller;
otherwise drop it and the line context still helps via the
adjacent lines stderr prints.)

**Verify:** manual smoke — load a workspace with > MAX_EXPR_VARS
visible vars at some line, verify stderr carries the warning *and*
the load otherwise succeeds. Optional unit test in
`test_repl_core_io.c` only if there's an existing convention for
capturing stderr in that suite (no need to invent one).

### Commit B2.4 — #66 delete 13 unused `state.c` macros

**File:** `src/repl/state.c:103-168` (the whole macro band, not
just the `:150-168` tail — two unused macros live earlier).

**Current shape:** 13 of 25 file-scope macros across lines 103-168
have zero readers — leftovers from the pre-`ReplRuntimeState`
era. The 13 with `grep -c '\bg_<name>\b' src/repl/state.c == 1`
(definition only, no readers) at time of writing:

| Line | Macro |
|---|---|
| 108 | `g_flat_cmds` |
| 109 | `g_flat_cmd_local_vars` |
| 150 | `g_lights` |
| 151 | `g_clear_color` |
| 160 | `g_example_idx` |
| 161 | `g_workspace_dir` |
| 162 | `g_workspace_header_lines` |
| 163 | `g_workspace_header_line_count` |
| 164 | `g_render_state_lines` |
| 165 | `g_cam_lines` |
| 166 | `g_export_scene_name_hint` |
| 167 | `g_pending_scene_name` |
| 168 | `g_pending_workspace_dir` |

Before deleting, re-run the sweep over the whole band — line
numbers may drift, and a future commit could give one of these a
reader, in which case it must stay:

```bash
for name in g_cmds g_num_cmds g_normals_dirty g_flat_cmds \
            g_flat_cmd_local_vars g_num_flat_cmds g_flat_dirty \
            g_user_lighting_enabled g_current_block_begin \
            g_current_block_end g_current_block_line g_anim_time \
            g_t_playing g_t_var_idx g_lights g_clear_color \
            g_example_idx g_workspace_dir g_workspace_header_lines \
            g_workspace_header_line_count g_render_state_lines \
            g_cam_lines g_export_scene_name_hint \
            g_pending_scene_name g_pending_workspace_dir; do
  c=$(grep -c "\\b$name\\b" src/repl/state.c)
  echo "$c $name"
done
```

Any macro with count `> 1` stays. Only delete the macros with
count `== 1`.

**Keep** the 12 macros that have readers — they aid readability of
the implementation functions.

**Verify:** `make check-c99`. If a macro was inadvertently used
inside a function body, the compile fails; restore it.

### Commit B2.5 — #67 phase-N.M comment sweep *(also folds in #70 doc-correction — split if reviewer prefers)*

**Files:**
- `src/repl/parser.c:29-30, 889`
- `src/repl/parser.h:38, 62-65`
- `src/repl/compile.c:171-173`
- `src/repl/compile.h:79, 214-217`
- `src/repl/executor.c:177-178`
- `src/repl/pipeline.h:19`
- `src/repl/eval.h:42-88`

**Current shape:** "phase H.5 commit 40", "β forbids …", "phase
3.6.3" — construction-history references. Some reference deleted
modules (`eval.h` mentions `editor_undo.h`, `replay.c / glr_ctrl.c`,
`FlatScope`).

**Target shape:** Drop phase coordinates. Keep policy statements
("REPL pipeline does not reach into editor_state_*") where they
add value. In `eval.h:42-88`, update the three "Used in" paths.

**Audit #70 fold-in (separate doc-correction, but in the same
file):** `eval.h:42-88` also has a `MAX_EXPR_VARS` /
`MAX_PREDEF_VARS` mislabel — the heading says one while the body
describes the other. That's a *semantic* fix to a live API
comment, not phase-coord cleanup, but it lives in the same span
the sweep edits anyway. Calling it out so a reviewer expecting
pure cosmetic comment churn doesn't miss the semantic correction.
If the reviewer prefers strict commit-scope hygiene, split into
B2.5a (phase sweep) and B2.5b (#70 doc swap).

**Verify:** `make check-c99`. Pure comment changes (no runtime
behavior).

---

## Batch 3 — Naming + duplication + couplings (#34, #38, #39, #40, #46, #52)

Mechanical convergence on canonical names + one-shot helper
extractions.

### Commit B3.1 — #34 collapse workspace_dir name pair

**Files:** `src/repl/state.c:478-484`, `src/repl/scenes.c:1016-1023`,
all callers

**Current shape:** `repl_workspace_dir` (scenes.c) and
`repl_state_workspace_dir` (state.c) are two names for the same
getter; same for the setter pair.

**Target shape:** Delete the state.c shims; rewrite the 4 callers
(`src/repl/export.c:273` + 3 tests) to call the scenes.c names
directly.

```bash
# verify no other consumers
grep -rn "repl_state_workspace_dir\|repl_state_workspace_set_dir" src/ tests/
```

**Verify:** `make test-stubs`. Caller-rename only.

### Commit B3.2 — #38 collapse examples API name pair *(check site count first — may want to defer)*

**Files:** `src/repl/examples.h:48-57`, `src/repl/example_loader.c:503-513`,
all callers

**Current shape:** Plural `repl_examples_*` in the header; singular
`repl_example_*` trampolines in example_loader.c (plus the tag/
subheading API which is already singular).

**Target shape:** Pick singular (matches tag/subheading already).
Rename the plural exports; delete the singular trampolines.

**Churn check (run before committing):**

```bash
grep -rn -E "repl_examples_" src/ tests/ bench/ tools/ | wc -l
grep -rn -E "repl_example_"  src/ tests/ bench/ tools/ | \
    grep -v "repl_examples_" | wc -l
```

At time of writing: 48 plural sites and 137 singular sites — so the
rename touches ~48 lines, not the full ~185. That's the size of a
single mechanical commit and should stay in Tier A. **If a future
run shows the plural side has crossed ~80-100 sites** (e.g. a new
caller class appeared), reclassify as Tier B and split: one commit
per call-site cluster. The trampolines already work; both names
resolve; the cosmetic benefit is finite, so churn budget matters.

Sweep both `repl_examples_*` (plural) and `repl_example_*` (singular)
in the same edit pass so a single set of call sites doesn't accidentally
end up routed through a deleted trampoline.

**Verify:** `make check-c99 && make test-stubs`. Sweep needs to
catch both `repl_examples_count` and `repl_example_count` etc.

### Commit B3.3 — #39 collapse the three `?:` cfg-get ternaries

**File:** `src/repl/export.c:873-878, 1454-1457, 2678-2683`

**Current shape:** Three open-coded `g_export_cfg_bridge &&
g_export_cfg_bridge->get_int ? ... : fallback` expressions.

**Target shape:** Each becomes `repl_cfg_get_int(slug, default)`
(already exists in `cfg_baseline.c`).

**Verify:** Confirm `repl_cfg_get_int` has the same semantics
(bridge → fallback) as the inline ternaries. Then sweep + test.

### Commit B3.4 — #40 funnel predef-var capture through `repl_eval_*` *(verify signature match before claiming "mechanical")*

**File:** `src/repl/scenes.c:208-213, 294-299, 393-398, 433-438,
458-463`

**Current shape:** Five sites manually loop:
```c
for (int i = 0; i < N; i++) {
    dst->predef_vals[i] = g_predef_vars[i].value;
    memcpy(dst->predef_names[i], g_predef_vars[i].name, sizeof(...));
}
```

**Layout check (do first):** Open `src/repl/scenes.c:65-83`
(`UserScene` struct) and `src/repl/eval.h:223-228` (the
`repl_eval_copy_predef_vars` signature). Verify:

1. `UserScene.predef_vals` is `float[MAX_PREDEF_VARS]` (the eval
   helper's `dst_vals` shape).
2. `UserScene.predef_names` is
   `char[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX]` (the eval
   helper's `dst_names` shape — same dimensions, same dim order).
3. `UserScene.num_predef_vars` is `int` (the helper's
   `*dst_count`).

At time of writing all three match (the inner `memcpy` already
uses `sizeof(s->predef_names[i])` which is `REPL_PREDEF_NAME_MAX`,
so the layouts are by construction equivalent). If a future
`UserScene` reshape makes them diverge, **do not** add a wrapper
that converts on the fly — that's a layering smell. Instead,
either reshape the struct back to match the eval API, or accept
that this de-duplication is no longer free and defer to Tier B.

**Target shape:** Call `repl_eval_copy_predef_vars()` /
`_restore_predef_vars()` (already exist in `eval.h`) directly with
the struct's fields:

```c
repl_eval_copy_predef_vars(dst->predef_vals, dst->predef_names,
                           &dst->num_predef_vars);
```

**Verify:** Scene save/load round-trip tests
(`test_repl_core_io.c`) cover this — they should pass unchanged
because the per-field semantics are identical.

### Commit B3.5 — #52 unify struct terminators

**File:** `src/repl/command_spec.c:384, 407`

**Current shape:** `k_enum_command_specs[]` terminator at L384 uses
5 positional zeros for a 6-field struct (`args[]` zero-init).
`k_std_command_specs[]` terminator at L407 uses 6 positional fields.
Rows above both terminators use `.name = ...` / `.args = ...`
designated initializers. That's three conventions in one file.

**Target shape:** Make both end-of-table terminators use the same
convention. `{ NULL }` (C99 zero-fill, designator-friendly because
`.name` is the first field) is the cleanest:

```c
{ NULL }      /* end-of-table */
```

**Verify:** `make check-c99 && make test-stubs`. Pure
initializer-syntax change; the table walker keys on the sentinel
shape (`.name == NULL`), unchanged.

### Commit B3.6 — #46 derive `g_content.tab_count` from `g_tabs[]`

**File:** `src/repl/help_text.c:253, 405` (the two unrelated `3`s)

**Current shape:** `g_tabs[3] = { ... }` at L253 and
`g_content.tab_count = 3` at L405 — two literal `3`s that must
agree by hand. Adding a fourth tab requires editing both sites; a
mismatch silently truncates the tab list or reads past the array.

**Target shape:** Derive the count from the array via `ARRAY_LEN`
(or `sizeof(g_tabs) / sizeof(g_tabs[0])` if the project doesn't
have an `ARRAY_LEN` macro at hand):

```c
g_content.tab_count = (int)(sizeof(g_tabs) / sizeof(g_tabs[0]));
```

(Optional, if a single literal across the file is worth eliminating
too: introduce `ReplHelpTabIdx { HELP_TAB_OVERVIEW, …, HELP_TAB_COUNT }`
and key the tab table off that — but that's a larger refactor; the
single-line derive is the minimal Tier A fix.)

**Verify:** `make check-c99 && make test-stubs`. The F1 help
overlay's tab-cycle test should still pass; if not, the array
literal and the old hard-coded `3` were drifted, and the derive
exposes it.

---

## Deferred to dedicated plans

(Mirrors the "Out of scope" list at the top with a short reason
each — keep both lists in sync if you add items.)

- **#1 layering breach in help_text.c** — needs a design call
  (move file vs. callback). Not on the audit's Tier A/B/C list;
  treat as Tier B for sizing.
- **#2 cfg double-apply** — semantics question (which path wins).
  Audit Tier B.
- **#5 UserScene stack pressure** — fix is small but the design
  call (heap vs. static scratch) deserves isolation. Audit Tier B.
- **#10 flatten_source_lighting_enabled** — needs to walk the flat
  program; touches flatten contract enough that it's Tier B-sized.
- **#48 apply.c DECLARE+SET_VALUE redundancy** — audit Tier B; the
  clean fix adds a new eval API (`declare_with_value`). Bundle
  with `#46` follow-on work if a help_text rewrite goes ahead.

---

## Post-batch hygiene

After all three batches land:

1. **Update the audit doc.** Mark the closed findings in
   `plans/in-review/src-repl-code-smell-audit-2.md`:
   - For each closed finding, suffix the heading with
     `(done — Tier A closed YYYY-MM-DD; commit SHA)`.
   - Update the "Sequencing → One-afternoon pass" section's
     checklist.
   - Update any "Bottom line" / "Headline take" counts.
2. **Decide Tier B kickoff.** Choose between:
   - The cross-cutting `_mut()` sweep + `check-state-ownership`
     ratchet (#14) — high leverage, one focused PR.
   - The `editor_compile_* / repl_compile_*` parse-core extraction
     (#15) — closes the largest remaining drift surface.
   - The `export.c` save/load TU split (#69) — biggest single LOC
     win.
   Open a follow-up plan for the chosen one.

## Expected outcome

After this plan completes (3 batches, ~9 commits, ~400 LOC net
reduction):

- All Tier A findings closed (audit's Tier A list, *including*
  `#53` — the explicit Tier A enumeration in the audit's tier
  table once read `#54-#67`, missing `#53`; the audit-side typo is
  fixed in the matching update). The one Tier A item *not* closed
  here is `#48`, deferred because the clean fix (`declare_with_value`)
  adds a new eval API — Tier B sized despite the Tier A label.
- 4 real correctness bugs fixed (#3, #9, #11, #13)
- ~200 LOC of dead `format.c` removed
- 13 dead state.c macros removed
- 5 sites of duplicated predef-var loops funneled through one
  helper
- 2 name pairs collapsed to one canonical form each
- `help_text.c` tab-count derived from the array (no more
  hand-synced `3`s)
- One audit-document update reflecting the Tier A closeout

The bigger Tier B work (cross-file `_mut()` sweep, helper
extractions, file splits, `#48` `declare_with_value`) is left
intact for its own plan.

## Time estimate

- Batch 1: ~1.5-2 hours (#9/#11/#13 are tight; #3 is now a
  5-file cascade through `ReplayRuntimeState` and
  `ReplayFadePlan` — see B1.1 file set and sizing note. May split
  into B1.1a / B1.1b for review)
- Batch 2: ~1 hour (mostly delete + verify)
- Batch 3: ~1.5 hours (B3.2 and B3.4 each touch multiple files;
  B3.5 + B3.6 are single-file)

Total: ~3-4 hours of focused work; ~9 commits.

## Verification matrix

| Gate | When | Expected |
|---|---|---|
| `make check-c99` | After every commit | OK |
| `make test-stubs` | After every commit | all test binaries pass; test count ≥ pre-branch baseline (B1.1 and B1.3 add tests, so a small *increase* is expected, never a decrease) |
| `ssh gracemont …` | Once at the end | `make check-c99 && make test-stubs` both green |
| Manual sanity | After batch 1 | start `./gl-repl`, type a few cmds, replay, verify nothing visually regressed |

*Note:* the binary count and exact test count are intentionally
not pinned here — they drift as the suite grows and adding
hardcoded values to a plan invites bit-rot. The criterion is "no
regression vs. the branch starting point," which the developer
captures with `make test-stubs 2>&1 | tail -5` before starting
work.

## Rollback

Each batch is its own commit (or commit group) on a feature branch.
Roll back with `git revert <sha>` per batch. The bugs in batch 1 are
unrelated to each other; reverting any one doesn't compromise the
others.
