# `src/subsystems/` — Code-Smell Audit

> Audit produced 2026-05-24. Findings come from four parallel reviews of
> `src/subsystems/` (replay; tutorial; variable_panel + color_picker;
> cross-cutting consistency) plus targeted spot-verification of the
> most actionable claims. File:line references are exact at the time
> of writing — check `git log` on the cited files before acting if
> this doc has aged.
>
> Scope: every file under `src/subsystems/`. Tests under `tests/` were
> read where they document a contract, but not audited.
>
> The single most important contract for this directory (per
> `src/subsystems/README.md`):
> **a peer subsystem owns its state and mutates it directly; the
> editor does not know it exists; UI renders it but does not own it;
> input is *routed in* by `src/app/glr_ctrl.c`, never *reached out for*
> by the peer; its one write path into the program is the editor
> commit transaction.** The dominant theme in the findings below is
> *that contract is unevenly enforced*: two peers (replay, tutorial)
> carry significant state outside the `_state.c` owner; one peer
> (color_picker) doesn't have a separate `_state.c` at all; the
> tutorial peer routinely *writes* into editor cursor/insert-mode/
> completion state and uses `repl_load_apply_line` instead of the
> editor commit; the four peers also disagree on basic lifecycle
> verbs and accessor naming.

## How to read this

Severity grouping mirrors the previous audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or
  reset-completeness issues with a concrete failure mode that exists
  in current production code. Pick these up first.
- **🟡 Drift / boundary hazards** — layering reaches, naming drift,
  parallel statics outside the owner struct, ambiguous-intent code
  that works today but is one edit away from misbehaving. Most of the
  cross-peer inconsistency lives here.
- **🟢 Dead code / dead fields** — code with no callers, unused
  declarations, dead defensive guards. Pure surface reduction.
- **🔵 Structural concerns** — long functions, magic numbers,
  comment archaeology, awkward sub-helpers. Bigger refactors;
  higher cost.

Each finding cites file + line, names the smell, says why it matters,
and suggests a one-line fix. Cross-peer findings (where the same
shape recurs in multiple subsystems) carry a **🔀 cross-peer** tag.

## Progress update — 2026-05-25 (branch `subsystem-smells`, reviewed at `d610de4`)

Current verification during this review: `make check-c99` passed on the
local macOS checkout, and the focused touched-area binaries passed:
`test_repl_replay` (57/57), `test_tutorial_runner` (453/453), and
`test_repl_var_drag` (36/36). The previous sweep recorded full
`make test` passing (39/39 test binaries, 6335/6335 tests); this review
did not rerun that full suite.

The original finding bodies below are retained as audit evidence, so
some line numbers and examples are intentionally stale. Treat this
progress block plus the Sequencing section as the live plan.

Closed in the current repo:

- **#1-#14** — Tutorial editor/completion writes route through host
  effects; tutorial baseline storage lives in `TutorialRuntimeState`;
  tutorial capture/restore exists; replay's parallel runtime statics and
  macro layer are gone; the `repl_load_apply_line` tutorial carve-out is
  documented; replay cancellation is visible; color-picker press
  docs/behavior match; header guards are synced; replay dirty-flatten
  copies are gated; dead color-picker locals are gone; replay expand
  toggles through the cfg bridge.
- **#16-#18, #20-#22, #24, #27, #28, #33, #34, #36-#48, #50, #55** —
  Variable-panel accessor/header cleanup landed; co-located subsystem
  layouts and color-picker reset are documented/implemented; variable
  drag state moved/renamed; tutorial explicit init replaced the
  constructor; replay `_impl` trampolines are gone; outside-picker
  teardown calls `color_picker_stop`; replay tess-depth and out-param
  cleanup landed; tutorial step-entry, pending-reset, status, and label
  helpers are factored; dead tutorial/replay/color-picker code was
  removed; replay's stale batch-consumer comment was updated.

- **#25, #26, #29, #31, #32, #35** — Refactored
  `cp_compute_rects()` to take explicit coordinates; refactored variable
  panel reads to use `repl_eval_predef_view()`; moved `ReplayFadePlan`
  out of public peer headers to `app/glr_ctrl.h`; renamed tutorial's
  noncommand commit block to `tutorial_reject_noncommand_commit_with_hint`;
  documented `expected_commit_line` persistence; unified probe checks into
  `tutorial_cfg_matches_target` helper.

Partially resolved / still tracked:

- **#15** — The variable-panel visibility setter is adopted for the
  controller reset path and documented, but lifecycle verb vocabulary is
  still intentionally mixed across peers (`start/stop`, `exit/teardown`,
  `set_visible`).
- **#19** — The worst ownership leak (`EditorVariableDragState`) moved
  into the variable-panel peer. `ReplReplayRuntimeState` still lives in
  `repl/state_views.h`, and `UiVariablePanelState` keeps its UI-prefixed
  value type; either document those as accepted snapshot-type carve-outs
  or move them in a future shape pass.
- **#23 / #30** — Tutorial now includes `repl/cfg_baseline.h` instead of
  `repl/export.h`, but the neutral cfg-baseline surface still exposes
  `ReplExport*` names and tutorial still includes `repl/state_owners.h`
  for broad REPL state access.
- **#59** — Keep open for the broader stale-comment sweep. New examples
  from `f2cdf759`: `replay.c` comments that say `Bug 7` / `Bug 14`.

Still open / next items:

- **#49, #51-#54, #56-#58** remain open.
- The partially resolved items above (**#15, #19, #23/#30, #59**) remain
  tracked until the plan explicitly accepts their carve-outs or finishes
  the cleanup.

## 🔴 Actual bugs / hazards (verified)

### 1. Tutorial peer writes directly into editor cursor / insert-mode / completion state

**Where:** `src/subsystems/tutorial/tutorial.c:281, 493, 556, 687, 707-708, 713, 728-729, 732, 752-753, 758, 646-647`

**Smell:** The runner directly calls `editor_insert_mode_set(0)`,
`editor_state_edit_line_set(line)`, `editor_completion_clear()`,
`editor_completion_update()`. `src/subsystems/README.md:19-22` is
explicit: *"a subsystem owns its state and mutates it directly; the
editor does not know the subsystem exists… Input reaches a subsystem
by being routed to it (by `src/app/glr_ctrl.c`), not by the editor
delegating."* The peer is reaching *out* to mutate an upper layer
instead of being routed *in*.

Compare to the color_picker peer (which writes back through
`editor_commit_apply_external_change`) and the replay peer (which uses
the installed `repl_dispatch_follow_cursor` host effect). Tutorial
bypasses both seams.

**Why it matters:** Concrete coupling — tutorial.c includes
`editor/state.h`, `editor/completion.h`, and `repl/state_owners.h`
because of these writes. The peer can no longer be lifted into a
demo without dragging the editor along.

**Fix:** Add `repl_dispatch_editor_cursor_park(line, mode)` (and a
matching completion-refresh dispatch) to the host-effects sink
installed in `glr_ctrl.c`. The peer posts via the dispatch; the
controller does the editor write. Tutorial.c drops the editor and
completion includes.

### 2. Tutorial baseline bag is hidden state outside `_state.c`; `tutorial_state_reset` doesn't clear it

**Where:** `src/subsystems/tutorial/tutorial.c:44-45`

**Smell:**
```c
static ReplExportConfig g_tut_cfg_baseline_bag;
static int              g_tut_cfg_baseline_valid;
```
Two file-statics live in the runner, not in `tutorial_state.c`.
`tutorial_state_reset()` (`tutorial_state.c:42-44`) zeroes only the
`TutorialRuntimeState` struct, so the bag and its valid flag survive
any "reset". The only thing that clears them is
`tutorial_cfg_baseline_clear()` called from `tutorial_cfg_baseline_restore()`
— so any external reset path that doesn't fully route through
`tutorial_teardown()` leaks a stale baseline.

**Why it matters:** Future code paths that call `tutorial_state_reset`
directly (a natural-looking name) would expect a complete reset and
get a partial one. The peer-owned-state contract claims "_state.c
owns storage"; this finding is exactly the violation it forbids.

**Fix:** Move `g_tut_cfg_baseline_bag` and `g_tut_cfg_baseline_valid`
into `TutorialRuntimeState` (or a sub-struct), and zero them in
`tutorial_state_init_defaults`.

### 3. Replay has four parallel statics outside the peer struct; reset/capture cover only the struct

**Where:** `src/subsystems/replay/replay.c:24-27`

**Smell:**
```c
static float g_replay_baseline_predef_vals[MAX_PREDEF_VARS];
static float g_replay_baseline_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
static int   g_replay_saved_t_playing = 1;
static int   g_replay_last_src_line = -1;
```
Plus `g_replay_fade_batches` at `:44-45`. None of these are part of
`ReplReplayRuntimeState`. `replay_state_reset()` (called from
`glr_app_reset_all` at `glr_ctrl.c:2203`) zeroes the struct only.
After a workspace switch + Ctrl+R, the first comparison still sees
the old `g_replay_last_src_line`; `replay_restore_baseline_predef_values()`
would restore stale predef values captured in a different scene if
called before `replay_start()` repopulates them.

**Why it matters:** Same root cause as #2; the peer-owned-state
contract is broken. Compounds with #14 (capture/restore claim is
also incomplete).

**Fix:** Move all four (plus the fade-batches ring) into
`ReplReplayRuntimeState`. The doc-comment for `replay_state.h`
explicitly says capture/restore is for full-world snapshots —
those snapshots are currently missing this state.

### 4. Tutorial uses `repl_load_apply_line` instead of the editor commit transaction

**Where:** `src/subsystems/tutorial/tutorial.c:282` (in
`tutorial_emit_instruction_comment`)

**Smell:** `src/subsystems/README.md:87` claims peers reach the
program "through the editor *commit* transaction — the one
sanctioned path." Tutorial writes through `repl_load_apply_line`,
which is "the non-editor load path" used by file importer / example
loader / integration tests. It bypasses the editor commit pipeline
entirely (no undo capture, no editor-buffer cursor protocol).

**Why it matters:** The README's stated invariant is literally false
for the tutorial peer. A future reviewer relying on the contract
("any peer write goes through editor commit") would miss tutorial's
parallel path.

**Fix:** Either (a) document the tutorial carve-out explicitly in
`README.md` (instruction-comment injection is a programmatic load,
not user-driven editing, so it intentionally bypasses undo) and
adjust the wording from "one write path" to "one user-driven write
path"; or (b) route through editor commit with an
`apply_skip_undo` flag.

### 5. `tutorial_state_reset` doesn't pair with capture/restore that the other peers expose

**Where:** `src/subsystems/tutorial/tutorial_state.c:34-44`

**Smell:** Replay and variable_panel both expose
`*_state_capture` / `*_state_restore` / `*_state_reset` triples.
Tutorial offers only `_reset()`. The subsystems README claims a
uniform "two-file shape… with capture/restore/reset and narrow
accessors."

**Why it matters:** No external caller of the existing
capture/restore exists (see #14), so this is mostly contract-drift
rather than a runtime bug — but it confuses readers comparing the
four peers' shapes. The CLAUDE.md table also implies capture/restore
exist for every `_state.c`.

**Fix:** Either add `tutorial_state_capture/_restore` matching the
peer sibling shape (and use them where `tutorial_teardown` is called
from workspace-load), or document the carve-out in the README + on
`tutorial_state.h`. Decide once.

### 6. Replay state-ownership leak: `g_replay_*` macro layer routes every read through `_mut()`

**Where:** `src/subsystems/replay/replay.c:12-22`

**Smell:**
```c
#define REPLAY_STATE (replay_state_mut())
#define g_replay_active      (REPLAY_STATE->active)
#define g_replay_state       (REPLAY_STATE->state)
... (every field aliased)
```
CLAUDE.md says `_mut()` accessors are "owner modules and controller
only." Replay IS the owner, so policy-wise this is OK *inside the
runner*. The real defect: the macro layer makes `_mut()` the
*default* read path, and `replay_state_view()` exists at
`replay_state.c:18` but is never called from `replay.c`. The narrow
accessors at `replay_state.c:46-76` (`replay_active`, `replay_pc`,
`replay_mode`, etc.) are also unused by the runner.

**Why it matters:** Any new code in `replay.c` will follow the
established pattern and take `_mut()` for reads. The macro name
`g_replay_state` also shadows the file-static of the same name in
`replay_state.c:21`, creating a maintenance hazard (search-and-
replace, debug prints).

**Fix:** Drop the macro layer. Either use the narrow accessors for
reads (already defined in `_state.c`) or take `replay_state_view()`
once per function. Reserve the `_mut()` pointer for the multi-field
write sites (`replay_start`/`_stop`/`_advance`). Also rename the
file-static in `replay_state.c` to avoid the shadow collision.

### 7. Replay's silent-stop on unrecognized keys swallows the user's intent

**Where:** `src/subsystems/replay/replay.c:1073-1074, 1110-1112`

**Smell:**
```c
/* (after all explicit keys handled) */
replay_stop();
return 0;
```
The controller dispatches "any key during active replay" to
`replay_handle_key`. Returning 0 means "I didn't consume this; pass
it on" — but the side effect of returning 0 here is `replay_stop()`.
So Ctrl+C, Ctrl+T, Ctrl+S — the editor's status-quo shortcuts —
silently tear down replay state.

**Why it matters:** Visible UX surprise. User types Ctrl+S to save
during a paused replay → replay collapses without a status message.
The peer is also making routing decisions in a leaf function, the
inverse of the routed-input contract.

**Fix:** Two options:
1. Stop in only one place — let the controller own the "any
   non-replay key during replay stops replay" policy. `replay_handle_key`
   becomes a pure consume-or-not predicate.
2. Status-message it (`repl_set_status("Replay: cancelled (key)")`)
   so the silent teardown becomes visible.

### 8. `color_picker_handle_press` "no-consume fall-through" branch claimed in header doesn't exist

**Where:** `src/subsystems/color_picker/color_picker_state.h:92-94`,
implementation at `color_picker_state.c:308-354`

**Smell:** The header doc says:
```
* - inside the picker bounds but outside slider rects: returns no-consume
*   so callers can fall through (rare; today returns the close-fallthrough
*   branch below)
```
There is no such branch in `color_picker_handle_press`. Any press
outside the SV / hue / alpha slider rects falls into the "dismiss"
branch at line 348 — including a click on the preview strip *inside
the picker frame*. The doc immediately contradicts itself
("returns no-consume" vs "today returns the close-fallthrough").

**Why it matters:** Clicks on the preview strip inside the picker
frame dismiss the picker, which is almost certainly not what users
expect. Either a missing branch (real bug) or a stale doc.

**Fix:** Decide whether clicks inside the picker frame but outside
sliders should be (a) consumed-no-op or (b) dismiss. Either implement
the missing branch or delete the contradictory bullet from the doc.

### 9. `color_picker_handle_press` "changed=1" header guarantee is stronger than the code provides

**Where:** `src/subsystems/color_picker/color_picker_state.h:91`,
implementation at `color_picker_state.c:324, 333, 343`

**Smell:** Header claims:
```
* - inside a slider rect: { consumed=1, closed=0, changed=1 } (drag begins)
```
But `res.changed = color_picker_write_cmd();` — and `color_picker_write_cmd`
can return 0 (snprintf overflow, parse failure). The header is
overpromising.

**Fix:** Weaken to "`changed=1` iff the writeback succeeded."

### 10. Header guard symbol mismatches between `#ifndef` and `#endif` comment

**Where:** `src/subsystems/color_picker/color_picker_state.h:23-24, 107`
and `src/subsystems/variable_panel/variable_panel_state.h:20-21, 88`

**Smell:**
```c
/* color_picker_state.h */
#ifndef COLOR_PICKER_STATE_H
#define COLOR_PICKER_STATE_H
...
#endif /* COLOR_PICKER_H */          /* mismatched name */

/* variable_panel_state.h */
#ifndef VARIABLE_PANEL_H              /* too generic */
#define VARIABLE_PANEL_H
...
#endif /* VARIABLE_PANEL_H */
```
The variable_panel guard symbol is `VARIABLE_PANEL_H` (too generic;
clashes-on-include with any future non-state `variable_panel.h`).

**Why it matters:** Pure mechanical hazard; if anyone adds a
`variable_panel.h` they'll get a silent guard collision.

**Fix:** Rename guards to match the file basename
(`COLOR_PICKER_STATE_H` already correct; `VARIABLE_PANEL_STATE_H`
to match). Sync the endif comments.

### 11. `replay_state_capture/restore` is documented for "full-world snapshots" but never used

**Status:** Resolved (by `subsystem-smells`) — Documented that variable-panel and replay state capture/restore are exclusively test/verification contracts; added the missing `tutorial_state_capture/restore` helper pair.

**Where:** `src/subsystems/replay/replay_state.c:24-32`

**Smell:** Doc-comments claim capture/restore is for app-wide
snapshot paths. Grep across the codebase: no external caller exists
for `replay_state_capture` or `replay_state_restore`. Same for
`variable_panel_state_capture/_restore` (`variable_panel_state.c`).
`repl_state_capture` (the REPL-owned snapshot) explicitly excludes
peer state — `src/repl/state.h:33-34` says *"Neither function
touches controller/editor/UI peer state."* `repl/scenes.c::stash_live_state`
captures only commands / predef vars / scratch arrays / func
aliases / cfg.

**Why it matters:** The peer subsystems have implemented a snapshot
contract that nobody invokes. Either dead code or a missing wire-up
of `repl_state_capture` into the peer snapshot helpers.

**Fix:** Either wire `repl_state_capture` to also capture each peer's
state (extending the contract) or delete the capture/restore
functions as dead code and update the README and header doc-comments
to match.

### 12. Replay's `replay_start` dead-code branch: save runs unconditionally; restore only inside the dirty branch

**Where:** `src/subsystems/replay/replay.c:817-829`

**Smell:**
```c
repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
repl_eval_copy_scratch_arrays(live_scratch_arrays);
if (repl_state_flat_program_dirty()) {
    repl_flatten_commands(...);
    repl_state_flat_program_clear_dirty();
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(live_scratch_arrays);
}
```
The save before the `if` always runs; the restore happens only inside
the dirty branch. If the program isn't dirty, the save was pure dead
work (~96 + 24 floats memcpy'd into stack locals that fall out of
scope). The matching block in `replay_seek_to_src_line` (`:771-780`)
correctly brackets save and restore together inside the dirty
branch.

**Fix:** Move the two `copy` calls inside the `if
(repl_state_flat_program_dirty())` block, mirroring
`replay_seek_to_src_line`.

### 13. `color_picker_state.c` reads `cmd` only for a null check, then drops it

**Where:** `src/subsystems/color_picker/color_picker_state.c:308-318, 356-365`

**Smell:** `cmd` is fetched in both `_handle_press` and
`_handle_motion`, used only for `if (!cmd) return res;`, then never
referenced. The write side (`color_picker_write_cmd`) re-fetches
`cmd` itself.

**Why it matters:** Not strictly a bug, but the unused local makes
the input handlers look like they care about the command's shape
when they don't.

**Fix:** Replace with a `cp_line_alive()` predicate (or inline
`if (!cp_cmd_at(g_cp_line)) return res;`).

### 14. Replay's `expand_args` has two writers with different code paths

**Where:** `src/subsystems/replay/replay.c:1063-1066` (E key) and
`src/app/glr_config.c:79` (config menu)

**Smell:**
```c
/* replay.c — E key path */
if (key == 'e' || key == 'E') {
    g_replay_expand_args = !g_replay_expand_args;
    return 1;
}
```
vs. the cfg-menu path `glr_config_set(GLR_CONFIG_REPLAY_EXPAND, ...)`
which (per CLAUDE.md) notifies the tutorial runner via
`tutorial_notify_state_changed` so REQUIRE steps observe every cfg
write. The E key bypasses the notification. The E shortcut also
emits no status message, while neighbors `'m'`/`'M'`, `'+'`/`'-'`
do.

**Why it matters:** Two mutation paths, only one notifies the
tutorial subsystem. A REQUIRE step gated on `replay_expand_args`
would fail to advance when toggled via E.

**Fix:** Route E through `glr_config_set(GLR_CONFIG_REPLAY_EXPAND,
!current)`; emit a status. Single mutation path.

## 🟡 Drift / boundary hazards
 
### 15. 🔀 Lifecycle verbs disagree across all four peers

**Status:** Partially resolved — `glr_ctrl.c` now uses
`variable_panel_set_visible()` for controller reset and the setter/mutator
split is documented, but the cross-peer lifecycle vocabulary remains mixed.

**Where:** cross-peer:
- replay: `replay_start()` / `replay_stop()` / `replay_restart_from_beginning()` / `replay_toggle_play_pause()`
- tutorial: `tutorial_start()` / `tutorial_exit()` / `tutorial_teardown()`
- color_picker: `color_picker_open()` / `color_picker_close()`
- variable_panel: **no session verb**; the only "lifecycle" entry is
  `variable_panel_set_visible(int)` (`variable_panel_state.c:54`),
  which production code never calls

**Smell:** Four subsystems, four lifecycle vocabularies. No
documented rationale for which subsystem uses which.

**Why it matters:** A new contributor wanting to "exit" a peer has
to memorize four naming styles. The
`variable_panel_set_visible(int)` setter exists but production
writes the field directly via `_view_mut()->visible` (in
`glr_config.c:98` and `glr_ctrl.c:1935`) — the setter is used only
by tests. Either the setter is the API or the field write is the API;
can't be both.

**Fix:** Pick one — `_start`/`_stop` recommended (matches the most
common verb pair). Update the variable_panel write sites to call
the setter. Document the verbs in `subsystems/README.md`.

### 16. 🔀 Accessor naming: `_mut` vs `_view_mut` vs no-mut

**Status:** Resolved (by `subsystem-smells`) — Aligned variable panel naming to standard `variable_panel_state_mut()` and updated its usages, and documented the design pattern in `src/subsystems/README.md`.

**Where:** cross-peer:
- replay: `replay_state_view()` / `replay_state_mut()`
- tutorial: `tutorial_state_view()` / `tutorial_state_mut()`
- variable_panel: `variable_panel_view()` / **`variable_panel_view_mut()`**
  (and `variable_panel_drag()` / `variable_panel_drag_mut()`)
- color_picker: `color_picker_view()` only — **no mutable accessor**

**Smell:** `variable_panel_view_mut` is the only "view_mut" name in
the codebase — every other accessor uses `_mut` directly. Reading
"view_mut" suggests "mutable view," which is semantically odd: a
view is by-convention read-only. `color_picker_view()` is a
different beast entirely — it returns a derived projection
(`ColorPickerView`) rather than the live storage state.

**Fix:** Either (a) rename `variable_panel_view_mut` →
`variable_panel_state_mut` (and `_view` → `_state_view`) to match
replay/tutorial, or (b) document a deliberate slice-naming
convention in the README. The color_picker projection is fine as-is
but should be documented as "render-projection view, not snapshot
view."

### 17. 🔀 `_state.c + *.c` two-file shape is fiction for color_picker

**Status:** Resolved (by `subsystem-smells`) — Updated `src/subsystems/README.md` to legalize co-located single-file architectures (like `color_picker_state.c`) and document peer subsystem layout exceptions.

**Where:** `src/subsystems/color_picker/`

**Smell:** README (lines 39-42) asserts every subsystem has a
two-file shape: `*_state.c` (storage) + `*.c` (runner). Color_picker
has **only** `color_picker_state.c` (374 lines) — no separate
runner. It mixes storage, lifecycle, input handlers, writeback
runner, and pure helpers in one file. Variable_panel inverts the
rule (runner is named `_drag.c`, not `variable_panel.c`).

**Fix:** Either split color_picker into `color_picker_state.c` +
`color_picker.c` to match the documented shape, or update the README
to acknowledge single-file peers as legal. Same call for
variable_panel's `_drag.c` runner naming.

### 18. 🔀 `glr_app_reset_all` clears three peers explicitly; color_picker is reset transitively

**Status:** Resolved (by `b3af6ca`) — Added explicit reset / teardown for color_picker and documented navigation closing policies.

**Where:** `src/app/glr_ctrl.c:2200-2209`

**Smell:**
```c
editor_state_reset();
ui_state_reset();
variable_panel_state_reset();
replay_state_reset();
... tutorial_teardown();        /* note: not tutorial_state_reset */
editor_reset_transients();      /* this is where color_picker_close() lives */
```
The peer-explicit list calls `replay_state_reset` and
`variable_panel_state_reset` (peer-owned verbs), then
`tutorial_teardown` (a peer-owned verb with cfg-baseline restore),
then `editor_reset_transients` — which transitively calls
`color_picker_close()` (`src/editor/input.c:355`). The editor
helper's comment at line 29-34 acknowledges the cross-layer fan-out
as "Known boundary exception."

Color_picker is also closed in **eight other places** (`glr_actions.c:445`,
`glr_ctrl.c:3188`, `:3242`, `:3253`, `:3271`, `:3307`,
`editor/input.c:355`, `:942`, `:1003`) — none of the other peers
get closed on click/escape/navigate.

**Why it matters:** Color_picker's "close on every navigation"
pattern is unique. The current placement makes it look like
editor-owned state when it's a peer.

**Fix:** Add `color_picker_state_reset()` (or rename `_close()` to
the canonical reset verb) and call it explicitly in
`glr_app_reset_all` next to the others. Keep the per-navigation
`_close()` calls but document the policy.

### 19. 🔀 Type ownership of state structs is scattered across four directories

**Status:** Partially resolved — `EditorVariableDragState` was renamed and
moved to the variable-panel peer as `VariablePanelDragState`. Snapshot value
types remain split by design today: `ReplReplayRuntimeState` still lives in
`repl/state_views.h`, and `UiVariablePanelState` still keeps its UI-prefixed
name inside the peer header.

**Where:**
- `ReplReplayRuntimeState` defined in `src/repl/state_views.h:106-117`
  (NOT in the peer header)
- `TutorialRuntimeState` in `src/subsystems/tutorial/tutorial_state.h:47-78`
- `VariablePanelState` in `src/subsystems/variable_panel/variable_panel_state.h:34-37`
- `EditorVariableDragState` in `src/editor/state.h:142-149`
  (NOT in the peer; owned by the editor module by name only)
- `UiVariablePanelState` in `src/subsystems/variable_panel/variable_panel_state.h:27-30`
  (has the `Ui*` prefix despite living in subsystems)
- color_picker: no separate runtime-state struct — file-static
  scalars only

**Smell:** The replay type sits in `repl/state_views.h` with a
comment claiming "type stays here so snapshots can pass replay state
by value without depending on the peer's API." But the same argument
applies to `TutorialRuntimeState` (also in `UiRenderSnapshot`),
and tutorial *does* keep its type in the peer header. Inconsistent.

`EditorVariableDragState` is the worst case: name starts with
`Editor*`, lives in `editor/state.h`, but is used **only** by the
variable_panel peer (and one controller line). `MODULES.md:125`
explicitly lists "Variable-panel drag (now on the variable_panel
peer)" under "EditorState NOT-responsible" — but the type itself
never moved.

**Fix:** Move `ReplReplayRuntimeState` into
`subsystems/replay/replay_state.h` (or keep the snapshot type in
`state_views.h` but document the carve-out and apply it
consistently). Move `EditorVariableDragState` →
`subsystems/variable_panel/variable_panel_state.h` and rename to
`VariablePanelDragState`. Decide whether `UiVariablePanelState`'s
`Ui*` prefix is right (it's used by `glr_config.c` and
`glr_ctrl.c`, not by UI render code).

### 20. 🔀 `_state.h` files include very different things

**Status:** Resolved (by `subsystem-smells`) — Cleaned up circular dependencies in `variable_panel_state.h` by removing upper-layer `"editor/state.h"` and `"ui/app/state_types.h"` includes and replacing them with `"config.h"`.

**Where:**
- `replay_state.h:19`: `#include "repl/state_views.h"`
- `tutorial_state.h:13`: `#include "repl/tutorials.h"`
- `variable_panel_state.h:23-25`: `#include "editor/state.h"` +
  `"ui/app/state_types.h"` + `"subsystems/variable_panel/variable_panel_drag.h"`
- `color_picker_state.h`: **no includes**

**Smell:** `variable_panel_state.h` reaches into both `editor/state.h`
AND `ui/app/state_types.h` — a peer header that pulls in two
upper-layer surfaces. The `variable_panel_drag.h` shim exists ONLY
to dodge a circular header dependency (acknowledged in the file's
top comment).

**Why it matters:** Every TU that includes the peer header pulls in
the editor and UI state types. The peer is wedged between layers
more than its siblings.

**Fix:** Once `EditorVariableDragState` moves into the peer (see
#19), `variable_panel_state.h` can drop `editor/state.h`. The
`UiVariablePanelState` -> snapshot inclusion path needs review —
either move that type out, or accept that the peer header is
serving both purposes and inline the shim.

### 21. `EditorVariableDragState` is owned by the variable_panel peer but lives in `editor/state.h`

**Where:** `src/editor/state.h:142-149`

**Smell:** Type is named `Editor*` but used only by:
- `subsystems/variable_panel/variable_panel_drag.c`
- `subsystems/variable_panel/variable_panel_state.c`
- `src/app/glr_ctrl.c:2963`

It's not referenced by any other editor file. `MODULES.md` already
documents it as not-editor-owned.

**Fix:** Move to `subsystems/variable_panel/variable_panel_state.h`;
rename to `VariablePanelDragState`.

### 22. Tutorial's `_state` constructor uses `__attribute__((constructor))` — gcc/clang-only

**Where:** `src/subsystems/tutorial/tutorial_state.c:27-32`

**Smell:**
```c
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void tutorial_state_module_init(void) {
    tutorial_state_init_defaults(&g_tutorial_state);
}
#endif
```
The other three peers use plain C99 designated-initializer macros
(`REPLAY_STATE_INITIAL`, etc.) that work on any compiler. Tutorial
uniquely depends on a GCC extension to initialize its `-1` sentinels
(`tutorial_idx`, `fade_line_idx`, `expected_commit_line`,
`instruction_line_for_step[i]`, `pending`).

**Why it matters:** On a hypothetical non-GCC/non-clang compiler the
constructor wouldn't fire; `g_tutorial_state` would be BSS-zero,
giving `tutorial_idx = 0` (not -1), `fade_line_idx = 0` (not -1),
etc. The chicken-and-egg with `tutorial_active()` (which reads
`g_tutorial_state.active`) means `tutorial_start` would still see
"not active" and behave correctly — but the silently-different
sentinel values are a footgun if any code path skips
`tutorial_state_reset` and relies on `-1` defaults.

The project targets C99 / old-GCC portability (CLAUDE.md goes into
this in detail), so today the only compilers in play do support the
attribute.

**Fix:** Either (a) drop the `#if` (the project already requires
GCC/clang per CLAUDE.md), or (b) wire `tutorial_state_init_defaults`
into `glr_ctrl_init_gl()` so first-use correctness doesn't depend on
`__attribute__((constructor))`. Option (b) is more portable.

### 23. Tutorial includes `repl/state_owners.h` for the cfg-bridge — pulls in the entire mut surface

**Status:** Partially resolved / retargeted — the cfg bridge now lives behind
`repl/cfg_baseline.h`, but `tutorial.c` still includes
`repl/state_owners.h` for broad REPL state operations such as document-count
queries, dirty marks, variable-time reads, and workspace-header parsing. The
remaining task is narrower than the original finding: split or ratchet the
non-cfg state access that tutorial still needs.

**Where:** `src/subsystems/tutorial/tutorial.c:13`

**Smell:** `repl/state_owners.h` exposes ALL the `_mut()` accessors,
not just the cfg-bridge ones. CLAUDE.md's row for that header even
notes the tutorial carve-out: *"single-slug bridge accessors used by
`src/subsystems/tutorial/tutorial.c`"* — but the whole surface comes
along.

**Why it matters:** A subsystem reading/writing only its own state
ends up with the entire owner-only surface in scope. Any future
misuse of `repl_state_*_mut` from `tutorial.c` would pass the build
without comment from the boundary check.

**Fix:** Split `state_owners.h`: extract the typed cfg wrappers
(`repl_cfg_get_int` / `_set_int` / `_known`) into a narrow
`repl/cfg_bridge.h` that tutorial includes. Add a
`check-tutorial-no-repl-state-mut` ratchet that rg's the file for
forbidden symbols.

### 24. Replay's two-trampoline `_impl` exposure on the public header

**Where:** `src/subsystems/replay/replay.h:215-216`,
trampolines at `replay_state.c:83-87`

**Smell:**
```c
int replay_handle_key_impl(unsigned char key);
int replay_handle_special_key_impl(int key);
```
These are only called from one-line trampolines in `replay_state.c`:
```c
int replay_handle_key(unsigned char key) {
    return replay_handle_key_impl(key);
}
```
Same args, same return. The `_impl` suffix leaks a private convention
onto the public surface. The trampoline does nothing.

**Fix:** Define `replay_handle_key` / `_special` directly in
`replay.c`; drop the `_impl` symbols from the header; delete the
trampolines.

### 25. `cp_compute_rects` reads file-globals while taking an output pointer (half-pure)

**Status:** Resolved (by `subsystem-smells`) — `cp_compute_rects` now takes
explicit `(px, py, out)` arguments, and each call site passes the picker
coordinates directly.

**Where:** `src/subsystems/color_picker/color_picker_state.c:47-59`

**Smell:**
```c
static void cp_compute_rects(ColorPickerRects *r) {
    int px = g_cp_px;
    int py = g_cp_py;
    ...
}
```
Reads `g_cp_px`/`g_cp_py` from file scope rather than taking them as
parameters. Half-pure: not testable in isolation, not obviously
stateful from the signature.

**Fix:** Take `(int px, int py, ColorPickerRects *out)` so the call
sites pass `g_cp_px`/`g_cp_py` explicitly.

### 26. Variable_panel reads predef table via `_mut` macros for read-only work

**Status:** Resolved (by `subsystem-smells`) — Refactored `variable_panel_drag.c` to use the read-only `repl_eval_predef_view()` instead of the mutable macros.

**Where:** `src/subsystems/variable_panel/variable_panel_drag.c:43, 49, 95`

**Smell:**
```c
if (row < 0 || row >= g_num_predef_vars) return;
...
drag->start_value = g_predef_vars[row].value;
```
`g_predef_vars` / `g_num_predef_vars` are macros over
`repl_eval_predef_vars_mut()` / `*repl_eval_predef_count_mut()`
(`src/repl/eval.h:156-157`). The peer reads from these — but
`repl_eval_predef_view()` exists for exactly this case.

**Fix:** Route through `repl_eval_predef_view()` (returns
`{const ExprVar *vars, int count}`). Reserve the `_mut` macros for
the eval/executor/apply owner sites.

### 27. Color_picker's "click outside" path open-codes the same teardown as `color_picker_close()`

**Where:** `src/subsystems/color_picker/color_picker_state.c:235-241, 349-352`

**Smell:**
```c
/* color_picker_close() at :235 */
g_cp_line = -1;
g_cp_drag = 0;
g_cp_undo_captured = 0;

/* "click outside" path at :349 */
g_cp_line = -1;
g_cp_drag = 0;
g_cp_undo_captured = 0;
res.closed = 1;
```
Two places to update when a new session-state field is added.

**Fix:** Replace the inline teardown with `color_picker_close(); res.closed = 1;`.

### 28. Replay tess-depth state machine has five inconsistent copies

**Status:** Resolved (by `subsystem-smells`) — Lifted unified `replay_advance_tess_depth(CmdType, int)` helper to `src/subsystems/replay/replay.c` and integrated it across both `replay_compute_fade_skip_limits` and `replay_walk_user_vertices`.

**Where:** `src/subsystems/replay/replay.c:85-116, 213-234, 537-547,
614-630, 678-694`

**Smell:** Five copies of the "track tess polygon/contour depth"
walker. They disagree:
- Three use `if (tess_depth == 1)` to advance to 2 (strict)
- `replay_walk_user_vertices:538` uses `if (tess_depth > 0) tess_depth++` (lax)

A nested `CMD_TESS_BEGIN_POLYGON` would lose the outer polygon under
the strict model and just keep climbing under the lax one. Whether
the parser ever produces nesting is one question; the *inconsistency*
between five copies is the smell.

**Fix:** Lift a `replay_advance_tess_depth(CmdType, int *depth)`
helper and call it from each walker. The mismatch becomes visible
once they share code.

### 29. `ReplayFadePlan` lives on the peer header but is only used by the controller

**Status:** Resolved (by `subsystem-smells`) — Moved `ReplayFadePlan` out of the public replay peer header into `app/glr_ctrl.h` alongside an explicit include of `repl/state_views.h`.

**Where:** `src/subsystems/replay/replay.h:54-63`,
used at `src/app/glr_ctrl.c:221-322`

**Smell:** Definition on the peer header. The replay peer doesn't
allocate, write, or read it. The comment "Snapshot the controller
assembles per frame" acknowledges this is controller-owned.

**Fix:** Move into `src/app/glr_ctrl.c` or a controller-private
header. The pure peer types — `ReplayFadeBatch`,
`ReplayFadeBatchView`, `REPLAY_FADE_BATCH_MAX` — stay on `replay.h`.

### 30. Tutorial reaches into `repl/export.h` for the cfg-baseline bag

**Status:** Partially resolved — tutorial now includes
`repl/cfg_baseline.h`, not `repl/export.h`. The remaining cleanup is the
export-flavored API naming (`ReplExportConfig`, `repl_export_config_*`,
`ReplExportConfigBridge`) on the neutral cfg-baseline surface.

**Where:** `src/subsystems/tutorial/tutorial.c:72, 108, 410-415, 517`

**Smell:** Tutorial uses `repl_export_config_bridge()`,
`repl_export_config_clear()`, `repl_export_config_set_int()`,
`repl_export_extract_cfg_slug()`, plus
`repl_state_parse_workspace_header_line()` — the *export* surface —
to take a baseline snapshot of cfg state and replay it later. The
bag and the round-trip mechanism were designed for export/import,
not for tutorial baselines.

**Why it matters:** Conceptually the wrong header to reuse. Future
changes to `ReplExportConfig` driven by export needs ripple into
tutorial.

**Fix:** Extract a `repl/cfg_baseline.{c,h}` (or rename
`ReplExportConfig` to a neutral name and move it to a more general
header) that both export and tutorial can depend on without
implying export semantics.

### 31. Tutorial's `tutorial_block_noncommand_commit` is both predicate and status-setter

**Status:** Resolved (by `subsystem-smells`) — Renamed it to `tutorial_reject_noncommand_commit_with_hint` to cleanly reflect its role and document it.

**Where:** `src/subsystems/tutorial/tutorial.c:849-868`

**Smell:** Function name reads as "should I block?" — but the body
fires `repl_set_status(...)` as a side effect. The header
acknowledges this dual role. Idempotent re-polls would re-overwrite
the status.

**Fix:** Either rename (`tutorial_reject_noncommand_commit_with_hint`)
or split into a pure `tutorial_should_block_commit` predicate plus a
separate `tutorial_emit_block_hint` emitter.

### 32. Tutorial's `tutorial_cancel_pending` clears pending but not `expected_commit_line`

**Status:** Resolved (by `subsystem-smells`) — Added documentation explaining that `expected_commit_line` persists so users can retry the commit without having their work locked.

**Where:** `src/subsystems/tutorial/tutorial.c` (cancel vs
advance/notify/ack pending-reset sites)

**Smell:** Every other "step transition" path clears both
`pending.*` and `expected_commit_line`. Cancel clears only pending.
So after a cancel, `expected_commit_line` still marks a row that no
in-flight commit will land on — `tutorial_guard_source_change`
continues blocking mutations at that row. Plausibly intentional, but
nowhere documented.

**Fix:** Either add a comment to `tutorial_cancel_pending` explaining
why `expected_commit_line` survives cancellation, or clear it too.

### 33. Tutorial's `notify_state_changed` can advance during a SET step's own cfg write

**Status:** Resolved (by `subsystem-smells`) — Implemented `in_enter_step` re-entrancy gate to `TutorialRuntimeState` and gated `tutorial_notify_state_changed` to resolve loops.

**Where:** `src/subsystems/tutorial/tutorial.c:808` (notify) +
`:726` (`repl_cfg_set_int` inside SET branch of `tutorial_enter_step`)

**Smell:** `tutorial_notify_state_changed` fires on every cfg write,
including writes made by `tutorial_enter_step` during a SET step.
If a SET step's write matches a REQUIRE target on the same slug in a
later step, the notify hook would advance the tutorial mid-setup.

**Why it matters:** Unlikely in the shipped catalog but a footgun
for future catalog authors.

**Fix:** Either gate `notify_state_changed` on a re-entrancy flag
("not currently inside an enter-step"), or document the catalog
invariant ("SET-then-REQUIRE on the same slug must use distinct
values").

### 34. 🔀 Naming drift: file-header comment in `variable_panel_state.c` says wrong filename

**Where:** `src/subsystems/variable_panel/variable_panel_state.c:2`

**Smell:**
```c
/*
 * variable_panel.c - Variable slider panel peer subsystem.
 */
```
File is `variable_panel_state.c`. The header was likely correct
before the rename. Tutorial's `_state.c` has no leading comment at
all; replay's is terse-but-correct.

**Fix:** Sync the header comment to the actual filename. Either add
or remove a leading comment for tutorial_state.c — pick a
convention.

### 35. Tutorial duplicates the "probe with distinct fallback" pattern twice

**Status:** Resolved (by `subsystem-smells`) — Unified the duplicate checks into a clean `tutorial_cfg_matches_target` helper utilizing the robust `repl_cfg_known` API.

**Where:** `src/subsystems/tutorial/tutorial.c:746, 818`

**Smell:**
```c
int probe_fb = (target == 0) ? -1 : 0;
if (repl_cfg_get_int(slug, probe_fb) == target)
```
The trick exists because `repl_cfg_get_int` returns a caller-supplied
default for unknown slugs — and an unknown slug shouldn't be
mistaken for "already satisfied." But `repl_cfg_known(slug)` is
already exposed; using it would replace the magic-fallback trick.

**Fix:** Use `repl_cfg_known(slug)` for the known-check; collapse the
probe-fb trick into one helper `tutorial_cfg_matches_target(slug,
target)`.

## 🟢 Dead code / dead fields

### 36. Dead state field: `TutorialMatchResult.last_result` is write-only

**Where:** `src/subsystems/tutorial/tutorial_state.h:77`,
`tutorial_state.c:22-24`, `tutorial.c:118-122`

**Smell:** Repo-wide grep confirms `last_result` is only written
(by `tutorial_store_result`, `tutorial_state_init_defaults`).
Nothing reads it. The `tutorial_store_result` helper writes to both
a caller-supplied `dst` and the state field; the dst-null branch is
never exercised (the only caller passes non-NULL).

**Fix:** Remove the field, drop `tutorial_store_result`, have
`tutorial_handle_commit_attempt` write directly through `*out`.

### 37. Dead enum variants: `TUT_MISMATCH_COMMAND` / `TUT_MISMATCH_ARG`

**Where:** `src/subsystems/tutorial/tutorial_state.h:21-22`

**Smell:** Two variants reserved for a v2 matcher that never
landed. `plans/done/tutorial-system-revised.md:88-89` labels them
"reserved for v2." No code path produces either. The sister
`arg_index` field on the result struct is also never written.

**Fix:** Delete the variants and the `arg_index` field. Reintroduce
when (and if) v2 ships.

### 38. Dead/unused setter: `variable_panel_set_visible` never called from production

**Status:** Resolved (by `subsystem-smells`) — Setter adopted at all production write sites (`glr_ctrl.c:1969`), making it active and robust.

**Where:** `src/subsystems/variable_panel/variable_panel_state.c:54-56`

**Smell:** Production writes the field directly via
`variable_panel_view_mut()->visible = …` at `src/app/glr_ctrl.c:1935`
and `src/app/glr_config.c:98`. Only tests
(`tests/test_glr_ctrl.c:417, 485`) call the setter.

**Fix:** Either adopt the setter at the two production call sites
(also removes two `_mut()` uses for non-owner writes) or delete it.

### 39. Dead-defensive RGB clamps in `color_picker_state.c`

**Where:** `src/subsystems/color_picker/color_picker_state.c:110-113`

**Smell:**
```c
if (cmd->type == CMD_CLEAR_COLOR) {
    if (r > CP_CLEAR_MAX_V) r = CP_CLEAR_MAX_V;
    if (g > CP_CLEAR_MAX_V) g = CP_CLEAR_MAX_V;
    if (b > CP_CLEAR_MAX_V) b = CP_CLEAR_MAX_V;
```
By HSV definition `r, g, b ≤ V`. When V is already clamped to
`CP_CLEAR_MAX_V` (at `:211` and `:298`), the RGB clamps can never
fire.

**Fix:** Drop the three clamps; trust the V-side clamp. If the
concern is float roundoff in `color_picker_hsv_to_rgb`, replace with
`min(CP_CLEAR_MAX_V, ...)` on V only and document the reason.

### 40. Dead capture/restore on replay and variable_panel (covered in #11)

**Status:** Resolved (by `subsystem-smells`) — Handled as part of finding #11; documented the capture/restore behaviors as test/verification contracts.

Functions exist; no callers. Either wire them in or delete them.

### 41. `replay_enabled()` is a one-line wrapper around `g_replay_active`

**Where:** `src/subsystems/replay/replay.c:52-54`

**Smell:** Only two callers (`replay_advance:870`,
`replay_exec_limit:915`); every other site uses `g_replay_active`
directly. One layer of indirection without payoff.

**Fix:** Inline; or use everywhere if the intent was to abstract
over "active vs benchmark mode."

### 42. `REPLAY_FLAT_STATE` macro needs `__attribute__((unused))` on its outputs

**Where:** `src/subsystems/replay/replay.c:47-50`

**Smell:**
```c
#define REPLAY_FLAT_STATE \
    FlatProgramView flat_program = repl_state_flat_program_view(); \
    const GLCmd *g_flat_cmds __attribute__((unused)) = flat_program.cmds; \
    int g_num_flat_cmds __attribute__((unused)) = flat_program.cmd_count
```
A macro that needs `unused` annotations on its outputs is signaling
that callers regularly use only a subset. `replay_compute_fade_skip_limits:184-187`
inlines the macro manually — confirming the abstraction is leaky.

**Fix:** Drop the macro; name the locals what the function actually
uses. The two-line "view + count" boilerplate is short enough to
type.

### 43. Throwaway `&(int){-1}` out-params hide non-NULL requirements

**Where:** `src/subsystems/replay/replay.c:879-881, 733-734`

**Smell:**
```c
next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
        ? replay_next_polygon_limit(old_pc, &(int){ -1 }, &(int){ -1 })
        : replay_next_vertex_limit(old_pc, &(int){ -1 }, &(int){ -1 });
```
The callees (`replay_next_*_limit`) unconditionally write through the
out-params. `replay_advance` doesn't care about the fade extents but
must provide writable scratch via C99 compound literals.

**Fix:** Make `fade_begin`/`fade_end` NULL-tolerant in both callees
(one `if (out)` per writer). Then `replay_advance` and
`replay_prev_limit` can pass plain NULL.

## 🔵 Structural concerns

### 44. `tutorial_enter_step` is a 90-line god-function with three branches

**Status:** Resolved (by `subsystem-smells`) — Decomposed the god-function into kind-specific helper functions (`tutorial_enter_step_command`, `tutorial_enter_step_set`, `tutorial_enter_step_require`) and a shared host effect cursor-parking dispatcher.

**Where:** `src/subsystems/tutorial/tutorial.c:677-765`

**Smell:** Eight responsibilities by my count: sentinel detection,
instruction-line resolution, comment emit, step-kind read, three
near-identical branches for COMMAND / SET / REQUIRE (each carrying
its own `editor_state_edit_line_set` / `editor_insert_mode_set` /
`editor_completion_update` triplet), unknown-kind terminal.

The SET and REQUIRE branches have identical "park cursor and clear
expected_commit_line" code.

**Fix:** Three per-kind handlers (`enter_step_command`,
`enter_step_set`, `enter_step_require`) called from a dispatch table,
plus a shared `tutorial_park_cursor_after(line)` helper. Bonus:
makes the existing `1/0/-1` tri-state contract easier to audit.

### 45. Tutorial's pending-clear copy-paste appears five times verbatim

**Status:** Resolved (by `subsystem-smells`) — Consolidated the duplicate pending reset statements into a single, clean helper `tutorial_pending_reset()`, and unified step advancement via `tutorial_advance_step()`.

**Where:** `src/subsystems/tutorial/tutorial.c:793-797, 824-828,
840-844, 1083-1085, 1107-1109`

**Smell:** Five sites reset the `pending.*` triplet to `-1`; three
of them also clear `expected_commit_line` and bump `step` before
calling `tutorial_advance_loop`. The cancel path clears pending only
(see #32) — no documented invariant explains why.

**Fix:** Add `tutorial_pending_reset()` (one-line clear of pending)
and `tutorial_advance_step()` (resets pending + expected_commit_line,
bumps step, runs advance loop). The three "user acked / matched /
requirement satisfied" sites collapse to one call; cancel keeps its
narrower semantics by calling only `_pending_reset()`.

### 46. Tutorial uses three return-code conventions in one file

**Status:** Resolved (by `subsystem-smells`) — Named step flow return values with a descriptive `TutorialStepResult` enum (`TUTORIAL_STEP_TERMINAL`, `_AUTOADVANCE`, `_PAUSE`) and refactored the loops to use it.

**Where:** `src/subsystems/tutorial/tutorial.c`

**Smell:**
- `tutorial_handle_ack_key` / `_block_noncommand_commit`: `1/0`
  ("consumed/not")
- Most helpers: `1/0` ("ok/error")
- `tutorial_enter_step`: `1/0/-1` ("paused/auto-advance/terminal")

Three meanings of zero in one TU.

**Fix:** Name the tri-state with an enum
(`TUTORIAL_STEP_PAUSED`, `_AUTOADVANCE`, `_DONE`) so the advance
loop's `while (1) { if (r != 0) return; }` becomes
`while (... == TUTORIAL_STEP_AUTOADVANCE) state->step++;`.

### 47. Tutorial duplicates two status strings across guard and enter-step

**Status:** Resolved (by `subsystem-smells`) — Extracted shared user status messages into reusable `tutorial_set_status_ack_set()` and `tutorial_set_status_require()` helper methods.

**Where:** `src/subsystems/tutorial/tutorial.c:731 & 854, 756 & 862`

**Smell:**
```c
/* :731 (tutorial_enter_step, SET branch) */
repl_set_status("Press Enter / Tab / Space to continue");
/* :854 (tutorial_block_noncommand_commit) */
repl_set_status("Press Enter / Tab / Space to continue");
```
And `"Set %s = %d to continue"` duplicated with a divergent
NULL-guard on `slug` at the two sites.

**Fix:** Two helpers (`tutorial_set_status_ack_set()`,
`tutorial_set_status_require(slug, target)`) used by both sites.
Decide on one slug NULL policy.

### 48. "Tutorial step target label is unresolved" reused for four distinct failures

**Status:** Resolved (by `subsystem-smells`) — Differentiated the unresolved target step errors with precise messages in `tutorial_step_instruction_line()`.

**Where:** `src/subsystems/tutorial/tutorial.c:354, 368, 374, 379`

**Smell:** Four code paths (target string empty, no earlier step
carries the label, target_step >= MAX, instruction_line out of
range) all emit the same user-facing string.

**Fix:** Differentiate the messages or rely on the catalog validator
at start time (lines 374 / 379 should be unreachable if validation
runs).

### 49. 🔀 Comment density and rationale style diverge across peers

**Where:**
- `tutorial.c` — heavy block-comment narrative (~24% of lines).
  Examples: 7-line block at `:101-107` explaining `tutorial_cfg_baseline_restore`
  order sensitivity; 9-line block at `:532-540` repeating the
  reasoning at the teardown site; 20-line block at `:470-489` on
  `tutorial_start`'s teardown-then-baseline-then-reset ordering;
  9-line block at `:505-513` on tag-mask cross-pumping
- `replay.c` — terse; longest narrative is 3 lines
- `color_picker_state.c` / `variable_panel_*.c` — medium

**Smell:** Tutorial's comments are genuinely warranted (the
cfg-baseline semantics are intricate), but the four peers don't
agree on whether code should be self-explanatory or carry full
bug-genealogy.

**Fix:** Move tutorial's multi-paragraph implementation-history
rationine into commit messages / `plans/done/`. Keep one-line "what"
+ "why" in source. Tutorial header (`tutorial.h:164 lines`,
~60 lines of paragraph explanation) can shed its implementation
contracts into the .c.

### 50. Replay duplicates the "flatten-if-dirty, restore live vars" prologue

**Status:** Resolved (by `subsystem-smells`) — Factored out `repl_ensure_flat_program_with_live_vars()` under `src/repl/flatten.c` and integrated it in `replay_seek_to_src_line` and `replay_start`.

**Where:** `src/subsystems/replay/replay.c:771-780, 818-829`
(plus a similar block without var-preserve at
`src/app/glr_ctrl.c:1701-1708`)

**Smell:** Three places open-code the same dirty-flatten + var-
preserve dance. Easy to get wrong (see #12).

**Fix:** Factor `repl_ensure_flat_program_with_live_vars()` in
`repl/pipeline.h` (or `core_internal`); both replay callers and any
future "I need a fresh flat program right now" caller use it.

### 51. Magic numbers in color_picker: `g_cp_drag` is an untyped int for a 4-value enum

**Where:** `src/subsystems/color_picker/color_picker_state.c:36, 322,
331, 341, 366`

**Smell:**
```c
static int g_cp_drag = 0;    /* 0=none 1=SV 2=hue 3=alpha */
```
The 1/2/3 magic numbers leak into the press/motion/release branches
and into `cp_apply_drag_at(int which, ...)`.

**Fix:** `enum CpDragTarget { CP_DRAG_NONE=0, CP_DRAG_SV, CP_DRAG_HUE,
CP_DRAG_ALPHA };`.

### 52. Color_picker's `cp_*` prefix vs full `color_picker_*` is inconsistent

**Where:** `src/subsystems/color_picker/color_picker_state.c`

**Smell:** File-private helpers use `cp_*` (`cp_compute_rects`,
`cp_clamp01`, `cp_apply_drag_at`, `cp_rgb_to_hsv`, `cp_cmd_at`);
public functions use `color_picker_*`. Other peers use full prefix
for statics (`variable_panel_drag.c` doesn't abbreviate).

**Fix:** Pick one — accept `cp_*` as a documented file-local
shorthand (add a one-liner at the top of the file) or rename to
`color_picker_*` throughout.

### 53. Magic transient buffer sizes (64, 96, 128, 256)

**Where:**
- `src/subsystems/replay/replay.c:1003, 1024, 925` — `char msg[64]`
- `src/subsystems/tutorial/tutorial.c:392-393, 592, 755, 861` —
  `char normalized_*[256]`, `char msg[128]`, `char msg[96]`

**Smell:** Tutorial defines `TUTORIAL_STATUS_MAX` (128) on the
header but only `tutorial_start` and the validator use it; per-step
status buffers ignore it. Replay has three `[64]` buffers with no
constant.

**Fix:** `TUTORIAL_STATUS_MAX` consistently for status buffers;
add `REPLAY_STATUS_MSG_LEN` and use it.

### 54. Frame-dt constant duplicated: `REPLAY_FADE_INITIAL_AGE 0.016f`

**Where:** `src/subsystems/replay/replay.c:36` and
`src/app/glr_ctrl.c:2101` (`GLR_FRAME_DT_SECS 0.016f`)

**Smell:** Two-place definition of the same physical constant.

**Fix:** Lift `GLR_FRAME_DT_SECS` to a shared header
(`app/frame_clock.h` or `config.h`); reuse for
`REPLAY_FADE_INITIAL_AGE`.

### 55. Stale "scene consumes" doc-comment on `replay.h`

**Where:** `src/subsystems/replay/replay.h:77-79`

**Smell:**
```c
/* ... The REPL replay state machine produces batches; the
 * scene module consumes them. */
```
The replay peer is no longer in `src/repl/`, and the consumer is no
longer `src/scene/` — it's the controller (`glr_ctrl.c::glr_ctrl_render_replay_fade_batches`).

**Fix:** Rewrite as "The replay peer produces batches; the controller
consumes them in its per-frame fade-render hook."

### 56. `static const ReplayTessPreviewCallbacks` / etc. use `g_` prefix

This is in `src/app/glr_ctrl.c`, not subsystems — but worth noting
because the peer's callback contract bleeds back into the controller.
Covered as finding #64 in the `src-app` audit.

### 57. Color_picker's `color_picker_open` clamp algorithm

**Where:** `src/subsystems/color_picker/color_picker_state.c:222-225`

**Smell:**
```c
if (ppx + pw > win_w - CP_SCREEN_MARGIN) ppx = cp_x - pw - CP_SCREEN_MARGIN;
if (ppx < CP_SCREEN_MARGIN) ppx = CP_SCREEN_MARGIN;
if (ppx + pw > win_w - CP_SCREEN_MARGIN) ppx = win_w - pw - CP_SCREEN_MARGIN;
if (ppx < CP_SCREEN_MARGIN) ppx = CP_SCREEN_MARGIN;
```
Four sequential corrections with the same predicates. Hard to see
convergence for `pw > win_w - 2*CP_SCREEN_MARGIN`.

**Fix:** Extract `clamp_popup_x(prefer_right_x, prefer_left_x,
popup_w, win_w, margin)` returning the chosen anchor with one
end-clamp.

### 58. Variable_panel column-alignment drift

**Where:** `src/subsystems/variable_panel/variable_panel_state.h:46-49`

**Smell:** Two-space gap on `view` lines, three-space gap on `drag`
lines. Manual column alignment fights an asymmetric type-name length.

**Fix:** Drop manual padding; single space.

### 59. Stale phase-name references in source comments

**Where:** `src/subsystems/variable_panel/variable_panel_drag.c:15-18`,
`src/subsystems/tutorial/tutorial.c:225`, `tutorial.h:27, 49, 122`,
`color_picker_state.c:10` (stale rationale: `repl/core.h /* set_status,
MAX_LINE_LEN */` — function was renamed to `repl_set_status`;
`MAX_LINE_LEN` is in `config.h`), plus the newer plan-coupled comments
in `src/subsystems/replay/replay.c` that say `Bug 7` / `Bug 14`.

**Smell:** "Phase F commit 31" / "Phase J7" / "v1 catalog" — internal
phase nomenclature without git-hash anchors.

**Fix:** Mechanical sweep — drop phase references; keep "why this is
here" notes that survive after the plan is forgotten.

## Sequencing

### Next one-afternoon pass

1. **#59** — Do the stale-comment sweep first, including the new
   `Bug 7` / `Bug 14` comments in `replay.c`, the old phase references,
   and the stale `repl/core.h /* set_status, MAX_LINE_LEN */` rationale.
2. **#51** + **#52** — Give color-picker drag targets a small enum and
   either document or remove the `cp_*` private-prefix shorthand.
3. **#53** + **#54** — Consolidate status-buffer and frame-dt constants.
4. **#57** + **#58** — Simplify color-picker popup clamping and drop the
   remaining manual column-alignment drift in `variable_panel_state.h`.

### Next one-week pass — boundary and shape decisions

1. **#23 / #30** — Finish the cfg-boundary cleanup: either rename the
   neutral cfg-baseline API away from `ReplExport*` names and split the
   remaining tutorial access out of `state_owners.h`, or explicitly
   document the retained broad state access and add a ratchet for what
   tutorial may call.
2. **#15** — Decide whether the mixed lifecycle verbs are accepted API
   vocabulary. If accepted, document the rationale in
   `src/subsystems/README.md`; otherwise rename toward one convention.
3. **#19** — Decide whether snapshot value types stay centralized in
   `repl/state_views.h` / UI-prefixed value structs, or move fully into
   peer headers. Document the chosen carve-out if no move is planned.
4. **#49** — Trim tutorial's long rationale comments into concise source
   comments and move implementation-history detail to plan/done notes.

### Done clusters removed from sequencing

The earlier state-ownership leak pass, replay silent-stop / `_impl`
cleanup, color-picker reset/header fixes, color-picker rect parameter
cleanup, variable-panel predef read cleanup, replay fade-plan move,
replay tess-depth factoring, tutorial noncommand/pending/probe helper
cleanup, and tutorial step-entry decomposition are already reflected in
the current source. They should not be used as next-step work unless a
new regression is found.

### Out of scope

- The walkers in `replay.c` (`replay_walk_user_vertices`,
  `replay_walk_tess_preview`) deliberately emit GL via callback so
  the scene module stays GLCmd-free. The peer's GL exposure is a
  soft contract violation (acknowledged in source comments) that's
  been consciously taken; don't try to "fix" it without rethinking
  the GLCmd → GL split.
- The peer-to-peer dependency check came up clean — no
  `subsystems/<other>/` includes between peers. The
  `replay_lift_px` coupling-by-name is the controller's
  responsibility, not the peer's.
- Tutorial's catalog (the lesson content) lives in
  `src/repl/tutorials.c` and was out of scope for this audit.

## Method note

This audit was produced by four parallel review agents:

- `replay/replay.{c,h} + replay_state.{c,h}` (1537 lines total —
  the biggest peer)
- `tutorial/tutorial.{c,h} + tutorial_state.{c,h}` (1408 lines)
- `variable_panel/*` + `color_picker/*` (761 lines, two smaller peers
  combined)
- A cross-cutting consistency review across all four, focusing on
  naming / lifecycle / accessor / snapshot-participation /
  reset-semantics divergences

Each agent was asked for ~15-25 highest-signal findings. The most
actionable claims (real-bug findings above) were verified against
the source. The 🟡 / 🟢 / 🔵 findings are reported as the agents
framed them; spot-check before acting on the more mechanical ones.

The cross-cutting review (#15-#20, #34, #49) is the highest-leverage
input for the "unify the peer shape" pass — those findings are
about the README contract being out of sync with what the four
subsystems actually do, and the fix is largely deciding on a
canonical shape and migrating the outliers.
