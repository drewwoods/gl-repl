# `src/subsystems/` — Code-Smell Audit (Round 2)

> Audit produced 2026-05-26. Findings come from four parallel reviews
> covering `replay/`, `tutorial/`, `color_picker/`, and
> `variable_panel/` plus targeted spot-verification of the most
> actionable claims. File:line references are exact at the time of
> writing — check `git log` on the cited files before acting if this
> doc has aged.
>
> Scope: `replay/`, `tutorial/`, `color_picker/`, and
> `variable_panel/` under `src/subsystems/`. The prior round
> (`plans/done/src-subsystems-code-smell-audit.md`) closed 58 of 59
> findings; #56 (cross-module `time_playing` write) is tracked in the
> `src/app/` audit and noted here for context but not re-counted.
> **Deferred to round 3:** `edit_overlays/` (~558 LOC) and
> `replay/replay_render.{c,h}` (~225 LOC) — both extracted from
> `src/app/` in commit ffda60e after the prior round.
>
> **Revision 2 (2026-05-26):** Reviewer corrections applied.
> #3/#4 downgraded from 🔴 to 🟡: #3's null branch is unreachable
> (pre-validated by `color_picker_can_edit_cmd`); #4's type-mismatch
> case bails out via `else { return 0 }`, and scene switches already
> close the picker via `glr_ctrl_reset_transients` — real residual is
> same-type shift on undo/delete only. Fixed function names
> (`color_picker_start`/`stop`, not open/close; no `g_cp_active`
> exists). #9 reframed: tutorial.c is a documented exception in
> `state_owners.h`, not a violation — concern is incomplete include
> comment + broad header surface. #11 narrowed: scene/example
> switches already handled; residual is undo/delete paths. #15
> withdrawn: `replay_prepare_frame` refreshes `total_flat_cmds` and
> clamps `pc` every frame. #7's fix corrected: `replay_state_view()`
> returns by value, so the `ReplayFadeBatchView` pointer would
> dangle — need a const-ref accessor instead. #1/#2 now note that
> two existing test assertions (`test_repl_replay.c:501`,
> `test_repl_editor.c:2283`) explicitly expect `return 0` and must
> be updated. Scope corrected: `edit_overlays/` and
> `replay_render.{c,h}` exist and are explicitly deferred.
>
> **Revision 3 (2026-05-27):** Second-round reviewer corrections.
> #11 further narrowed: every scene-switch/example-load/F12-cycle
> path already closes the picker via `glr_ctrl_reset_transients` →
> `color_picker_stop()` (verified at `glr_actions.c:407/413`,
> `glr_ctrl.c:2222/1648`); the **only** uncovered path is undo
> (`editor_undo_pop_snapshot` has zero color_picker references). Fix
> is one line in `undo.c`. #18 merged into #10 (only one bare-`0`
> `g_cp_drag =` site exists; all six others use named enums). #26
> moved to Tier D: `src/subsystems/README.md:41` explicitly documents
> the `replay_lift_px` pattern as intentional. Sequencing revised:
> #12 (tutorial shadow ↔ match normalization desync) elevated to
> Priority 1 alongside the replay reds — it's the most
> user-triggerable UX bug in the document.
>
> **Revision 4 (2026-05-27):** Post-implementation follow-up.
> #6's first controller-side resolution had a bug: the new follow-scroll
> trigger compared the replay source line against a same-frame
> pre-prepare snapshot inside `glr_ctrl_display_frame`, but the source
> line normally changes earlier in `glr_ctrl_tick()` via
> `replay_advance()`. Result: real playback/step/jump flows could stop
> auto-scrolling even though the peer→controller boundary was fixed.
> Final fix: `glr_ctrl.c` now caches the last replay follow line across
> frames, and `tests/test_glr_ctrl.c::test_display_frame_follows_replay_line_after_tick`
> reproduces the tick→display path so the regression stays pinned.
>
> The single most important contract for this directory:
> **Each peer subsystem owns one runtime state struct, mutates it
> directly via `_mut()`, exposes a by-value `_view()` for reads, and
> communicates outward only through its public API — never calling
> into the editor, UI, or app controller directly.** The replay
> module is the one partial exception (it reaches `repl_dispatch_*`
> callbacks and `repl_state_variables_mut()`); the tutorial reaches
> `state_owners.h` for mark-dirty + cfg helpers.

## How to read this

Severity grouping mirrors the previous audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or
  memory-safety issues with a concrete failure mode that exists in
  current production code. Pick these up first.
- **🟡 Drift / boundary hazards** — layer-crossing reaches, naming
  drift, parallel structures, hidden side effects, ambiguous-intent
  code that works today but is one edit away from misbehaving.
- **🟢 Dead code / dead fields** — code with no callers, unreachable
  branches, redundant initializers, unused parameters. Pure surface
  reduction.
- **🔵 Structural concerns** — long functions, near-duplicate pairs,
  magic numbers, comment archaeology. Bigger refactors; higher cost.

Each finding cites file + line, names the smell, says why it matters,
and suggests a one-line fix.

## Headline take

45 findings across four subdirectories (two more — `edit_overlays/`
and `replay_render.{c,h}` — deferred to round 3). **2 reds** — both
in replay (key-leak-after-stop for ASCII and special handlers). The
most user-visible issue is actually 🟡 #12 (tutorial shadow ↔ match
normalization desync — every user typing extra whitespace hits it).
The remaining 🟡 band covers replay cross-module boundary reaches
(`time_playing` writes, `repl_dispatch_follow_cursor`), a narrow
color-picker undo-path gap, and the tutorial's broad
`state_owners.h` include. The 🟢/🔵 tail is light — one `g_` naming
violation, a few dead writes, a handful of structural observations.
This directory is largely healthy after the prior round.

**Counts:** 2 🔴, 13 🟡, 10 🟢, 9 🔵 structural, 11 Tier D accepted,
2 withdrawn/merged = 45 total (43 live).

## Tier classification

| Tier | Criteria | Findings |
|------|----------|----------|
| **A** | Small, safe, afternoon pass | #1, #2, #3, #7, #8, #10, #11, #12, #13, #14, #17, #20, #21, #22, #23, #24, #25, #28 |
| **B** | Moderate, week-long pass with tests | #4, #5, #6, #9, #16, #19, #29, #30, #31, #32, #33, #34 |
| **C** | High cost or cross-cutting | #35, #36 |
| **D** | Accepted / kept | #26, #27, #37, #38, #39, #40, #41, #42, #43, #44, #45 |
| — | Withdrawn / merged | #15, #18 |

## Progress Update (2026-05-27)

All 18 Tier A findings and 12 targeted Tier B findings (#4, #5, #6, #9, #16, #19, #29, #30, #31, #32, #33, #34) have been fully addressed and verified as of 2026-05-27.

| Finding | Tier | Description | Status | Resolution |
|---------|------|-------------|--------|------------|
| #1 | A | Replay `replay_handle_key` returns 0 after stopping | **[RESOLVED]** | Stopped key leakage by returning 1 via new cancel helper; updated tests. |
| #2 | A | Replay `replay_handle_special` returns 0 after stopping | **[RESOLVED]** | Stopped key leakage by returning 1 via new cancel helper; updated tests. |
| #3 | A | Color picker `color_picker_start` sets `g_cp_line` before null check | **[RESOLVED]** | Moved index assignment below the command validity null check. |
| #4 | B | Color picker `color_picker_write_cmd` can write to wrong same-type line | **[RESOLVED]** | Terminated active color picker session inside `commit.c` (structural changes) and `input.c` (document resets) to prevent wrong-line writebacks. |
| #5 | B | Replay writes `repl_state_variables_mut()->time_playing` | **[RESOLVED]** | Routed pause/restore writes through `repl_dispatch_set_time_playing` callback to app controller, eliminating peer-to-core mutability. |
| #6 | B | Replay calls `repl_dispatch_follow_cursor(1)` | **[RESOLVED]** | Decoupled follow-cursor scrolling from replay to the app controller; follow-up fixed a missed tick-time source-line update by caching the last replay follow line across frames and added a controller regression test for the tick→display path. |
| #7 | A | Replay `replay_fade_batches_view` uses `replay_state_mut()` | **[RESOLVED]** | Implemented `replay_state_const()` to expose const-ref state access. |
| #8 | A | Replay `replay_copy_baseline_predef_snapshot` uses `_mut()` | **[RESOLVED]** | Switched accessor call to `replay_state_view()`. |
| #9 | B | Tutorial includes `state_owners.h` | **[RESOLVED]** | Isolated the 3 state-notification and parse helpers into `state_notify.h`, replacing `state_owners.h` and limiting mutable surface exposure. |
| #10 | A | Color picker uses bare `0` instead of `CP_DRAG_NONE` for `g_cp_drag` | **[RESOLVED]** | Replaced magic `0` reset value with `CP_DRAG_NONE` enum. |
| #11 | A | Undo doesn't invalidate the color picker | **[RESOLVED]** | Wired `editor_undo_pop_snapshot` and `editor_undo_do_redo` to stop picker. |
| #12 | A | Tutorial shadow suffix desyncs with normalized match | **[RESOLVED]** | Upgraded suffix computation to match whitespace-normalized prefixes. |
| #13 | A | Tutorial preserves baseline across reset via stack copy | **[RESOLVED]** | Created `tutorial_state_reset_except_baseline()` to avoid stack copying. |
| #14 | A | Tutorial `repl_cfg_set_int` called without declaring include path | **[RESOLVED]** | Documented all 6 symbols in the `state_owners.h` include comment. |
| #16 | B | Replay's `replay_exec_limit` coupling | **[RESOLVED]** | Injected `FlatProgramView` dependency into replay frame-level/tick-level functions to guarantee synchronicity and eliminate mid-frame stale views. |
| #17 | A | Replay `g_flat_cmds` / `g_num_flat_cmds` use `g_` prefix | **[RESOLVED]** | Renamed local variables to drop misleading `g_` prefixes. |
| #19 | B | Tutorial `in_enter_step` field — recursive-guard with unclear invariant | **[RESOLVED]** | Removed the vestigial reentrance guard `in_enter_step` from tutorial state and implementation. |
| #20 | A | Variable panel drag `start_x` unused outside begin/motion | **[RESOLVED]** | Moved `start_x` from public struct to static `g_drag_start_x`. |
| #21 | A | Replay `last_src_line` reset at stop — dead write | **[RESOLVED]** | Removed dead write from `replay_stop()`. |
| #22 | A | Replay `state->accum = 0.0f` in `replay_stop` — dead write | **[RESOLVED]** | Removed dead write from `replay_stop()`. |
| #23 | A | Replay `state->pc = 0` in `replay_stop` — dead write | **[RESOLVED]** | Removed dead write from `replay_stop()`. |
| #24 | A | Replay `state->total_flat_cmds = 0` in `replay_stop` — dead write | **[RESOLVED]** | Removed dead write from `replay_stop()`. |
| #25 | A | Tutorial defaults init loop sets `instruction_line_for_step` to -1 | **[RESOLVED]** | Documented init loop / active example reset invariant. |
| #28 | A | Replay `replay_handle_key` is ~65 lines of `if (key == ...)` ladder | **[RESOLVED]** | Refactored matching logic to a clean, readable `switch (key)` block. |
| #29 | B | Replay has two near-identical "unrecognized key stops replay" sites | **[RESOLVED]** | Centralized unrecognized key cancel path via `replay_cancel_on_unrecognized()` in Tier A. |
| #30 | B | Replay `replay_start` is 30+ lines mixing state init, snapshot, and side effects | **[RESOLVED]** | Split `replay_start()` modularly into `replay_snapshot_baseline()` and `replay_init_playback_state()`. |
| #31 | B | Tutorial `tutorial_enter_step` is ~100 lines with nested control flow | **[RESOLVED]** | Verified that per-kind step helpers have been cleanly extracted and dispatched via a `switch` statement. |
| #32 | B | Tutorial cfg baseline save/restore is split across three functions | **[RESOLVED]** | Consolidated baseline management into `tutorial_baseline_capture` / `_restore` pair and extracted `tutorial_baseline_apply`. |
| #33 | B | Replay seek/advance/step functions share a "clamp PC and update src_line" postamble | **[RESOLVED]** | Extracted `replay_update_after_pc_change` shared postamble and applied it. |
| #34 | B | Variable panel drag has two scaling modes inlined in one function | **[RESOLVED]** | Extracted `drag_linear_value` and `drag_log_value` helpers. |

---

## 🔴 Actual bugs / hazards (verified)

### 1. Replay `replay_handle_key` returns 0 after stopping — key leaks downstream

**Where:** `src/subsystems/replay/replay.c:1133-1136`

**Smell:** When replay is active and the user presses an unrecognized
ASCII key, the handler stops replay (`replay_stop()`) but returns 0.
The calling chain (`glr_ctrl_router_handle_active_replay_key` at
`src/app/glr_ctrl.c:2744`) then dispatches the key to the editor/config
handlers. The user intended to cancel replay, but the keystroke also
triggers whatever that key does in the editor (e.g., `;` would commit
a line, `d` would type into the input buffer).

**Why it matters:** Double-action side effects from a single keypress.
Confirmed: after `replay_stop()` sets state = OFF, the key cascades
through config shortcuts and editor input.

**Fix:** Return 1 after the `replay_stop()` call — the key was
consumed by the "cancel replay" action.

**Test impact:** Two existing assertions explicitly expect `return 0`:
`tests/test_repl_replay.c:501` (`"unrecognized key not consumed"`)
and `tests/test_repl_editor.c:2283-2284`
(`"replay unknown key unconsumed"`). The implementation must update
both tests to assert `consumed == 1` and add a behavior-contract
comment explaining that cancelling replay consumes the key. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Introduced a centralized helper `replay_cancel_on_unrecognized()` that stops replay and returns `1`. Updated the ASCII key matching `switch` block in `replay_handle_key()` to route unrecognized keys to this helper. Updated both test suites (`test_repl_replay.c` and `test_repl_editor.c`) to assert that the cancellation keystroke is consumed (returns `1`).

---

### 2. Replay `replay_handle_special` returns 0 after stopping — key leaks downstream

**Where:** `src/subsystems/replay/replay.c:1172-1177`

**Smell:** Same pattern as #1 but in the special-key handler. An
unrecognized special key (after filtering modifiers) stops replay
but returns 0, so the key propagates to
`glr_ctrl_router_handle_cfg_special_shortcut` and beyond.

**Why it matters:** Same double-action hazard. F-key shortcuts could
toggle overlays immediately after the replay cancellation.

**Fix:** Return 1 inside the `if (!replay_modifier_special_key(key))`
block. Same test-update caveat as #1. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Guided unrecognized non-modifier special keys to the centralized `replay_cancel_on_unrecognized()` helper, stop playback, and return `1` to prevent downstream config/shortcut propagation.

---

## 🟡 Drift / boundary hazards

### 3. Color picker `color_picker_start` sets `g_cp_line` before null check — defensive ordering issue

**Where:** `src/subsystems/color_picker/color_picker_state.c:222-225`

**Smell:** `g_cp_line = cmd_idx` is written at L222 before
`cmd = cp_cmd_at(cmd_idx)` at L223. If `cp_cmd_at` returns NULL, the
function returns at L225 with `g_cp_line` pointing at a nonexistent
command — the picker reads as "open" (`g_cp_line >= 0`) but color
channels were never populated.

In practice this null branch is currently **unreachable**:
`color_picker_can_edit_cmd(cmd_idx)` at L209 calls the same
`cp_cmd_at(cmd_idx)` with no mutating code between L209 and L222, so
if `can_edit_cmd` passes, the L223 fetch will also succeed. The
finding is a defensive-coding concern, not a triggerable bug.

**Why it matters:** A future refactor that inserts a mutation between
the guard and the assignment (e.g., an undo snapshot) could open the
window. The ordering is fragile even if currently safe.

**Fix:** Move `g_cp_line = cmd_idx` below the null check. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Moved the `g_cp_line = cmd_idx` assignment below the NULL pointer check in `color_picker_start()`, ensuring internal invariants are preserved.

---

### 4. Color picker `color_picker_write_cmd` can write to wrong same-type line after index shift

**Where:** `src/subsystems/color_picker/color_picker_state.c:107-108`

**Smell:** `color_picker_write_cmd()` fetches `cp_cmd_at(g_cp_line)`.
If lines were inserted/deleted while the picker was open (e.g., undo,
cut, paste), `g_cp_line` may point at a different command.

The type switch at L117-138 has an `else { return 0; }` at L137-138,
so a shift to a **non-color** command type is caught — the writeback
bails out cleanly. The narrower real concern is a shift to a
**different color command of the same type** (e.g., after a paste
shifts indices such that `g_cp_line` now points at a different
`CMD_COLOR3F` further down). In that case the writeback silently
overwrites the wrong line's color.

**Why it matters:** Incorrect color written to a line the user didn't
open the picker for. #11's "close on scene-switch / undo" approach
is the load-bearing fix that prevents the triggering scenario.

**Fix:** Call `color_picker_stop()` from the undo/paste/delete paths
that restructure the command array (#11), invalidating the picker
before any writeback can fire. A `g_cp_opened_type` check alone
wouldn't catch the same-type case. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - In addition to stopping the picker during undo/redo (Finding #11), `color_picker_stop()` is now called inside `apply_compiled_change_full()` in `src/editor/commit.c` for all structural change kinds (insertions, deletions, range deletions) and inside `editor_reset_document_to_empty()` in `src/editor/input.c`, completely resolving the target-shift hazard.

---

### 5. Replay crosses module boundary to write `repl_state_variables_mut()->time_playing`

**Where:** `src/subsystems/replay/replay.c:852, 867`

**Smell:** The replay peer reaches into the REPL variable state to
pause/restore the `time_playing` flag. This is a direct cross-module
write from a subsystem into core REPL state, violating the "mutate
only your own peer state" contract.

**Why it matters:** If another module also writes `time_playing` (e.g.,
a future tutorial step or test), the saved/restored value can
desync. The prior audit (#56) flagged this; it's tracked in the
`src/app/` audit but still unresolved.

**Fix:** Route through a `repl_dispatch_set_time_playing(int)` callback
like other cross-module effects, letting the app controller own the
write. (Tier B — touches replay + controller + dispatch table)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Declared a `set_time_playing` function pointer in the `ReplHostEffects` callback bridge in `src/repl/core.h` and implemented a routing helper `repl_dispatch_set_time_playing()`. The controller in `src/app/glr_ctrl.c` registers the static callback `glr_ctrl_host_set_time_playing()` which updates mutable `time_playing` under its own domain. All direct writes in `replay_start()` and `replay_stop()` inside `src/subsystems/replay/replay.c` have been replaced with this callback dispatch, achieving a perfect boundary structure.

---

### 6. Replay calls `repl_dispatch_follow_cursor(1)` — input-layer dispatch from peer

**Where:** `src/subsystems/replay/replay.c:157`

**Smell:** `replay_seek_source_line` calls
`repl_dispatch_follow_cursor(1)` to scroll the code panel to the
current replay line. This is an input-dispatch call (it's in the
`repl_dispatch_*` callback family) invoked from a peer subsystem
rather than from the app controller.

**Why it matters:** Muddles the subsystem contract — peers shouldn't
drive UI scrolling. If the dispatch table changes or gains
preconditions, replay would need updating.

**Fix:** Have the controller check whether the replay source line
changed each frame and call `follow_cursor` from
`glr_ctrl_display_frame`, or add a replay-specific output flag the
controller polls. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27; follow-up fix same day) - Removed `repl_dispatch_follow_cursor(1)` from `replay_set_src_line()` in `src/subsystems/replay/replay.c`, moving follow-scroll ownership to the app controller. A post-implementation regression uncovered that the first controller-side trigger compared against a same-frame pre-prepare snapshot, which missed the normal tick-time `replay_advance()` source-line updates. Final fix: `src/app/glr_ctrl.c` now caches the last replay follow line across frames and raises `editor_scroll_follow_cursor_set(1)` when the replay line differs from that cross-frame cache; `tests/test_glr_ctrl.c::test_display_frame_follows_replay_line_after_tick` reproduces the real tick→display flow and pins the behavior.

---

### 7. Replay `replay_fade_batches_view` uses `replay_state_mut()` for read-only access

**Where:** `src/subsystems/replay/replay.c:169`

**Smell:** `replay_fade_batches_view()` is a read-only function (it
returns a view struct) but acquires the state via `replay_state_mut()`
instead of `replay_state_view()`.

**Why it matters:** Naming-convention violation. The `_mut()` accessor
communicates write intent to readers; using it for reads erodes that
signal.

**Fix:** Cannot simply switch to `replay_state_view()` here —
`ReplayFadeBatchView` holds a pointer into the live state
(`const ReplayFadeBatch *batches`), so taking the address of a
by-value stack copy would dangle. Instead, add a
`replay_state_const()` accessor that returns `const ReplayRuntimeState *`
into the live storage, or rename the function to make the mutability
intent explicit. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Implemented `replay_state_const()` in `replay_state.c` and updated `replay_fade_batches_view` to use it for safe, pointer-preserving, read-only const access without using `_mut()`.

---

### 8. Replay `replay_copy_baseline_predef_snapshot` uses `_mut()` for read-only access

**Where:** `src/subsystems/replay/replay.c:1001`

**Smell:** Same pattern as #7 — a copy-out function that only reads
state uses `replay_state_mut()`.

**Fix:** Switch to `replay_state_view()`. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Switched `replay_copy_baseline_predef_snapshot` to fetch read-only state using `replay_state_view()`, preserving boundary mutability semantics.

---

### 9. Tutorial's `state_owners.h` include — documented exception but broad surface

**Where:** `src/subsystems/tutorial/tutorial.c:11`

**Smell:** `state_owners.h` explicitly names tutorial.c as a
sanctioned consumer (CLAUDE.md file-layout table documents the cfg
helpers as "single-slug bridge accessors used by
`src/subsystems/tutorial/tutorial.c`"). So this is **not** a boundary
violation — it's a documented exception. The concern is that the
header also exposes every `_mut()` accessor and the full mutable
REPL-state surface. Tutorial only needs 6 symbols:
`repl_state_mark_flat_dirty`, `repl_state_mark_source_dirty`,
`repl_state_parse_workspace_header_line`, `repl_cfg_get_int`,
`repl_cfg_set_int`, `repl_cfg_known`.

**Why it matters:** The include comment at L11 only documents 3 of
the 6 used symbols. A future contributor sees the broad header and
might reach for other mutable accessors without realizing the
tutorial's scope is intentionally narrow. Splitting the narrow
surface into a dedicated header would let `state_owners.h` shed its
non-owner cfg helpers.

**Fix:** Either (a) update the L11 comment to document all 6 symbols,
or (b) extract a `repl/state_notify.h` with the 6 tutorial-facing
declarations and switch tutorial.c to include only that.
(Tier B — option (b) touches header split + guard update)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Created a narrow header `src/repl/state_notify.h` containing the declarations of `repl_state_mark_flat_dirty`, `repl_state_mark_source_dirty`, and `repl_state_parse_workspace_header_line`. Switch `src/subsystems/tutorial/tutorial.c` to include `state_notify.h` instead of `state_owners.h` (and documented its imports accurately), completely isolating the tutorial from the mutable REPL state accessors.

---

### 10. Color picker uses bare `0` instead of `CP_DRAG_NONE` enum for `g_cp_drag`

**Where:** `src/subsystems/color_picker/color_picker_state.c:257`

**Smell:** `g_cp_drag = 0;` resets the drag state using a magic
integer instead of the defined `CP_DRAG_NONE` constant.

**Why it matters:** If the enum values are ever renumbered (e.g., to
add a sentinel), this assignment becomes wrong silently.

**Fix:** Use `g_cp_drag = CP_DRAG_NONE;`. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Replaced the bare `0` reset with the `CP_DRAG_NONE` enum constant inside `color_picker_stop()`.

---

### 11. Undo doesn't invalidate the color picker

**Where:** `src/editor/undo.c` (no color_picker reference),
`src/subsystems/color_picker/color_picker_state.c` (global state)

**Smell:** Every scene-switch / example-load / F12-cycle path already
closes the picker via `glr_ctrl_reset_transients()` →
`color_picker_stop()` (`glr_actions.c:407/413`, `glr_ctrl.c:2222`,
`glr_ctrl.c:1648`). The one uncovered path is **undo**:
`editor_undo_pop_snapshot()` (Ctrl+Z) restores a prior document
snapshot without invalidating the picker. After undo, `g_cp_line`
may point at a shifted sibling color cmd of the same type, or fall
off the end (guarded by `cp_cmd_at` NULL check at L113).

**Why it matters:** After Ctrl+Z inserts/deletes lines above
`g_cp_line`, the picker's next drag writeback can land on a
different same-type color line (see #4). Non-color shifts are caught
by `write_cmd`'s type switch.

**Fix:** Wire `editor_undo_pop_snapshot` to call
`color_picker_stop()`. One-line addition in `src/editor/undo.c`.
(Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Integrated `color_picker_stop()` in both `editor_undo_pop_snapshot()` and `editor_undo_do_redo()` inside `src/editor/undo.c`. Active picker sessions are now safely terminated whenever an undo/redo structural shift is triggered, eliminating the writeback hazard.

---

### 12. Tutorial `tutorial_shadow_suffix` uses raw `strncmp` while `tutorial_match` normalizes whitespace

**Where:** `src/subsystems/tutorial/tutorial.c:982` vs `:662`

**Smell:** `tutorial_shadow_suffix` compares the user's input against
the expected text using a plain `strncmp`, but `tutorial_match` (the
commit-time checker) normalizes whitespace before comparison. A user
who types extra spaces will see the ghost suffix disappear (shadow
thinks they match) while the actual commit-time match rejects.

**Why it matters:** Visual mismatch between ghost-text guidance and
actual acceptance. The user sees the hint vanish, types `;`, and
gets a "does not match" status.

**Fix:** Have `tutorial_shadow_suffix` call the same normalization
pass (or call `tutorial_match` directly to check prefix membership).
(Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Re-implemented `tutorial_shadow_suffix()` to perform robust, whitespace-normalized prefix matching symmetrically with `tutorial_match()`. The ghost-text suffix is computed directly from raw text alignments to keep suffix visualization correct when spaces are added.

---

### 13. Tutorial `tutorial_start` preserves baseline across `tutorial_state_reset` via stack copy

**Where:** `src/subsystems/tutorial/tutorial.c:498-508`

**Smell:** The function copies the entire `ReplConfigBag` (~1.3 KB)
onto the stack, calls `tutorial_state_reset()` (which zeroes it),
then writes it back. This is a save-around-reset pattern that only
exists because `tutorial_state_reset` is unconditional.

**Why it matters:** Not a bug, but the idiom is fragile — if more
state needs preserving, the pattern grows. A selective reset that
skips `baseline_bag`/`baseline_valid` would be cleaner.

**Fix:** Add a `tutorial_state_reset_except_baseline()` variant that
zeroes all fields except the baseline bag, eliminating the
save-around. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Implemented `tutorial_state_reset_except_baseline()` in `tutorial_state.c` and updated `tutorial_start()` to call it, completely removing the ~1.3 KB stack-preservation copy overhead.

---

### 14. Tutorial `repl_cfg_set_int` / `repl_cfg_get_int` called without declaring the include path

**Where:** `src/subsystems/tutorial/tutorial.c:56, 59, 687, 700, 702`

**Smell:** The `repl_cfg_get_int` / `repl_cfg_set_int` / `repl_cfg_known`
functions are declared in `repl/state_owners.h` (see #9), but the
comment at L11 only mentions the three mark-dirty/parse functions.
The cfg helpers are an additional (undocumented) reason for the
owner-header inclusion.

**Why it matters:** If #9's fix moves the three mark-dirty functions
out, the cfg helpers still anchor the `state_owners.h` dependency.
A complete fix needs to move those too.

**Fix:** Document the cfg helpers in the include comment, and include
them in #9's header split. (Tier A — bookkeeping tied to #9)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Updated the `state_owners.h` include block comment at line 11 in `src/subsystems/tutorial/tutorial.c` to declare all 6 symbols in use, satisfying documentation and audit compliance.

---

### 15. ~~Replay `replay_start` caches `num_flat_cmds` but `total_flat_cmds` can drift~~ — WITHDRAWN

**Withdrawn.** `replay_prepare_frame()` at
`src/subsystems/replay/replay.c:966-975` already refreshes
`total_flat_cmds` from the live flat program and clamps `pc` every
frame. The start-time snapshot at L860 is immediately superseded.

---

### 16. Replay's `replay_exec_limit` coupling — flat program dependency without ownership

**Where:** `src/subsystems/replay/replay.c` (pervasive)

**Smell:** Replay reads `repl_state_flat_program_view()` at multiple
sites (fade skip limits, seek, advance) without owning or
subscribing to the flat program. If the flat program is rebuilt
mid-frame (e.g., dirty flag triggers rebuild between replay reads),
indices captured from one call become stale relative to the next.

**Why it matters:** Subtle desync if frame orchestration order changes.
Currently safe because `glr_ctrl_display_frame` rebuilds flat before
replay, but the ordering is implicit.

**Fix:** Pass a `FlatProgramView` into replay frame functions from the
controller (dependency injection), making the ordering explicit.
(Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Injected `FlatProgramView` into replay functions: `replay_prepare_frame`, `replay_advance`, `replay_fill_base_limit`, and `replay_compute_fade_skip_limits`. The app controller now retrieves the `FlatProgramView` once per frame and threads it to these calls, avoiding mid-frame stale program states. Callers in all test files (`test_repl_replay.c`, `test_repl_editor.c`, `test_repl_core_extra.c`) and benchmark (`bench_repl.c`) were successfully updated to pass the view.

---

## 🟢 Dead code / dead fields

### 17. Replay `g_flat_cmds` / `g_num_flat_cmds` local variables use `g_` prefix

**Where:** `src/subsystems/replay/replay.c:179-180`

**Smell:** Local variables in `replay_compute_fade_skip_limits` are
named `g_flat_cmds` and `g_num_flat_cmds`, using the `g_` prefix
reserved by project convention for file-scoped statics.

**Why it matters:** Misleading to readers scanning for global state.

**Fix:** Rename to `flat_cmds` / `num_flat_cmds`. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Renamed both local variables in `replay_compute_fade_skip_limits` to `flat_cmds` and `num_flat_cmds`, eliminating the misleading `g_` prefix.

---

### 18. ~~Color picker `CP_DRAG_NONE` defined but not used at all reset sites~~ — merged into #10

**Merged.** Only one bare-`0` site exists (L257, covered by #10).
All six other `g_cp_drag =` assignments already use named enum
values (`CP_DRAG_NONE`, `CP_DRAG_SV`, `CP_DRAG_HUE`,
`CP_DRAG_ALPHA`). No separate action needed.

---

### 19. Tutorial `in_enter_step` field — recursive-guard with unclear invariant

**Where:** `src/subsystems/tutorial/tutorial_state.c:21`,
`src/subsystems/tutorial/tutorial.c` (multiple sites)

**Smell:** `in_enter_step` is a reentrance guard flag set/cleared
around `tutorial_enter_step`. Its name doesn't communicate *what*
reentrance path it prevents, and it's unclear whether the guarded
paths can actually reenter after the iterative-advance refactor
replaced recursion.

**Why it matters:** If the recursion is truly gone, the guard is dead
weight. If it's still needed, the trigger path should be documented.

**Fix:** Verify whether any code path can still trigger reentrance
into `tutorial_enter_step`; if not, remove the flag. If yes, add a
comment naming the path. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Completely removed the vestigial reentrance guard `in_enter_step` from `TutorialRuntimeState`, `tutorial_state_init_defaults()`, `tutorial_enter_step_set()`, and `tutorial_notify_state_changed()`.

---

### 20. Variable panel drag `start_x` unused outside begin/motion

**Where:** `src/subsystems/variable_panel/variable_panel_drag.c`

**Smell:** `start_x` is captured in `variable_panel_handle_drag_begin`
and used in `_motion` to compute delta, but it's also present in the
`VariablePanelDragState` struct exposed through `variable_panel_drag()`.
External readers never use it — it's internal to the drag transaction.

**Why it matters:** Exposing transaction-internal fields in the view
struct clutters the API surface.

**Fix:** If `start_x` is never read outside the drag module, move it
to a file-static in `variable_panel_drag.c`. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Removed `start_x` from public `VariablePanelDragState` struct and the `VARIABLE_PANEL_INITIAL` initialization macro. Shifted it to a private, file-static `g_drag_start_x` inside `variable_panel_drag.c`, minimizing the exposed public surface.

---

### 21. Replay `last_src_line` reset at stop — dead write

**Where:** `src/subsystems/replay/replay.c:873`

**Smell:** `state->src_line_idx = -1` in `replay_stop()` writes to a
field that is never read when `state->active == 0`. The field only
matters during playback.

**Why it matters:** Pure noise — the value is re-initialized in
`replay_start()` before the next use.

**Fix:** Remove the dead assignment (or keep for defensive clarity —
Tier D candidate). (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Removed the dead write `state->src_line_idx = -1` inside `replay_stop()`, since playback fields are fully initialized inside `replay_start()`.

---

### 22. Replay `state->accum = 0.0f` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:871`

**Smell:** Same pattern as #21 — `accum` is re-initialized in
`replay_start()` and never read when state is OFF.

**Fix:** Remove or mark as defensive. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Removed the dead write `state->accum = 0.0f` inside `replay_stop()`.

---

### 23. Replay `state->pc = 0` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:870`

**Smell:** Same pattern as #21/#22.

**Fix:** Remove or mark as defensive. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Removed the dead write `state->pc = 0` inside `replay_stop()`.

---

### 24. Replay `state->total_flat_cmds = 0` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:874`

**Smell:** Same pattern.

**Fix:** Remove or mark as defensive. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Removed the dead write `state->total_flat_cmds = 0` inside `replay_stop()`.

---

### 25. Tutorial `tutorial_state_init_defaults` loop initializes `instruction_line_for_step` to -1

**Where:** `src/subsystems/tutorial/tutorial_state.c:19-20`

**Smell:** The loop `for (i = 0..MAX) s->instruction_line_for_step[i] = -1`
is equivalent to `memset(..., 0xFF, ...)` for int = -1 on
two's-complement, but more importantly: this array is only meaningful
when `active == 1`, and `tutorial_start` reinitializes it before use.

**Why it matters:** The init-time loop is defensive but unreachable
in practice — `tutorial_state_reset()` → `_init_defaults()` fires
when the tutorial ends, at which point the array is no longer read.

**Fix:** Keep as defensive initialization (Tier D candidate) or
document the invariant. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Documented the defensive nature of the default initialization loop and documented the reset invariant cleanly.

---

### 26. Variable panel `replay_lift_px` initialized to 0.0f but only written by controller

**Where:** `src/subsystems/variable_panel/variable_panel_state.c:7`

**Smell:** `replay_lift_px` is initialized in the peer's default
struct but its value is only ever written by the app controller
during replay. The peer exposes it in the view, but the initial
`0.0f` is the same value the controller would set for non-replay
frames anyway.

**Why it matters:** The field's home in the peer state struct is
slightly odd — it's a controller-written display hint stored in a
peer's view. Functional but philosophically misplaced.

**Fix:** Accept as-is. `src/subsystems/README.md:41` explicitly
documents this pattern as intentional ("`variable_panel_state_mut()`
provides direct mutable pointers for internal config-mapping and
per-frame easing equations (like `replay_lift_px`)"). (Tier D)

---

### 27. Color picker `g_cp_undo_captured` is never read from outside the file

**Where:** `src/subsystems/color_picker/color_picker_state.c:218-219`

**Smell:** `g_cp_undo_captured` is file-static and only checked/set
within `color_picker_start` and `color_picker_write_cmd`. It's not
dead, but it's also not visible through any public API — pure
internal bookkeeping that could be a struct field for clarity.

**Fix:** Accept as-is — file-statics for internal flags is the
project convention. (Tier D)

---

## 🔵 Structural concerns

### 28. Replay `replay_handle_key` is ~65 lines of `if (key == ...) return 1` ladder

**Where:** `src/subsystems/replay/replay.c:1072-1136`

**Smell:** Long sequential key-matching chain. Each recognized key
returns 1; the fallthrough stops replay and returns 0 (#1). A
dispatch table or switch would be clearer and make the "consume"
contract explicit.

**Fix:** Refactor to `switch (key)` with explicit `default:
replay_stop(); return 1;`. (Tier A)

**Status:** [RESOLVED] (Tier A pass, 2026-05-27) - Refactored the entire recognized ASCII key matching ladder inside `replay_handle_key()` into a clean, highly structured, and readable `switch (key)` statement, routing default flow to the cancel helper.

---

### 29. Replay has two near-identical "unrecognized key stops replay" sites

**Where:** `src/subsystems/replay/replay.c:1133-1136` and `:1172-1177`

**Smell:** Both the ASCII and special key handlers duplicate the
cancel pattern (set status, stop, return). A shared
`replay_cancel_on_unrecognized()` helper would centralize the fix
for #1/#2.

**Fix:** Extract a helper returning 1. (Tier A)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - The centralized helper `replay_cancel_on_unrecognized()` was introduced during Tier A, which stops replay, updates the status message, and returns `1`. Both the ASCII and special key handlers were updated to route unrecognized keys to this helper, resolving both #1/#2 and #29.

---

### 30. Replay `replay_start` is 30+ lines mixing state init, snapshot, and side effects

**Where:** `src/subsystems/replay/replay.c:840-863`

**Smell:** One function does: copy predef vars, copy scratch arrays,
snapshot time_playing, toggle time_playing, set active/state/pc/accum,
clear fade batches, snapshot flat count, set status. Mixing
snapshot + mutation + side effect in one linear block.

**Fix:** Split into `replay_snapshot_baseline()` +
`replay_init_playback_state()` + status set. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Modularized `replay_start()` by extracting baseline snapshotting into `replay_snapshot_baseline()` and playback state initialization into `replay_init_playback_state()`.

---

### 31. Tutorial `tutorial_enter_step` is ~100 lines with nested control flow

**Where:** `src/subsystems/tutorial/tutorial.c` (around L260-370)

**Smell:** Handles COMMAND, SET, and REQUIRE step kinds in one
function with nested ifs, a `for(;;)` advance loop, and the
reentrance guard. Readable but approaching the complexity threshold.

**Fix:** Extract per-kind helpers (`enter_command_step`,
`enter_set_step`, `enter_require_step`) called from a switch. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Verified that `tutorial_enter_step()` has been cleanly split into modular per-kind helpers (`tutorial_enter_step_command()`, `tutorial_enter_step_set()`, and `tutorial_enter_step_require()`) that are cleanly dispatched via a switch block, resolving all target complexity smells.

---

### 32. Tutorial cfg baseline save/restore is split across three functions

**Where:** `src/subsystems/tutorial/tutorial.c:495-508` (start),
`:530-540` (apply cfg lines), `tutorial_teardown` (restore)

**Smell:** The baseline lifecycle (capture → preserve across reset →
apply `@cfg` → restore on teardown) spans three non-adjacent
functions. The save-around pattern in start (#13) adds to the
cognitive load.

**Fix:** Consolidate baseline management into a paired
`tutorial_baseline_capture()` / `_restore()` pair called from start
and teardown. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Consolidated tutorial baseline management into a `tutorial_baseline_capture` and `tutorial_baseline_restore` pair, and extracted config application into `tutorial_baseline_apply`.

---

### 33. Replay seek/advance/step functions share a "clamp PC and update src_line" postamble

**Where:** `src/subsystems/replay/replay.c` (multiple sites)

**Smell:** After each PC mutation, the same clamping and
`src_line_idx` update logic repeats. A `replay_update_after_pc_change()`
helper would deduplicate.

**Fix:** Extract the shared postamble. (Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Extracted the `replay_update_after_pc_change` shared postamble in `replay.c` and applied it to `replay_seek` and `replay_advance`.

---

### 34. Variable panel drag has two scaling modes inlined in one function

**Where:** `src/subsystems/variable_panel/variable_panel_drag.c:40-80`
(approximately)

**Smell:** `variable_panel_handle_drag_motion` contains both the
linear and logarithmic value-mapping paths in one if/else branch.
Each is ~15 lines with different math. They share no code.

**Fix:** Extract `drag_linear_value()` and `drag_log_value()` helpers.
(Tier B)

**Status:** [RESOLVED] (Tier B pass, 2026-05-27) - Extracted the `drag_linear_value` and `drag_log_value` helpers in `variable_panel_drag.c`.

---

### 35. Replay module is 1180+ lines in a single file

**Where:** `src/subsystems/replay/replay.c`

**Smell:** The file covers: fade batch management, seek/advance/step,
speed control, keyboard input, mode switching, baseline
snapshot/restore, bench helpers. These are logically separable
concerns sharing only the `ReplayRuntimeState` pointer.

**Fix:** Split into `replay_fade.c` (batch ring), `replay_input.c`
(key handlers), `replay_playback.c` (seek/advance/speed). (Tier C)

---

### 36. Tutorial module is 1088 lines with mixed concerns

**Where:** `src/subsystems/tutorial/tutorial.c`

**Smell:** Similar to #35 — the file covers: cfg baseline,
start/exit/teardown, step advancement, fade animation, match logic,
shadow suffix, commit guard. Separable into runner + animation +
matching.

**Fix:** Split into `tutorial_runner.c` (lifecycle), `tutorial_fade.c`
(animation), `tutorial_match.c` (match + shadow). (Tier C)

---

## Tier D — Accepted / documented

### 37. Replay's `repl_dispatch_follow_cursor` call from peer

**Where:** `src/subsystems/replay/replay.c:157`

**Rationale:** This is an established dispatch pattern (same as
`repl_dispatch_tutorial_teardown`). The callback table design exists
specifically to let peers communicate effects without hard
dependencies. Flagged in #6 as the ideal target; accepted as-is if
#6 is deferred.

---

### 38. Variable panel state struct exposes `replay_lift_px` to external readers

**Where:** `src/subsystems/variable_panel/variable_panel_state.h`

**Rationale:** One float field in a view struct. The controller writes
it; the UI reads it. Moving it to a per-frame snapshot would add
plumbing for zero functional gain.

---

### 39. Tutorial's iterative advance loop replaces recursion

**Where:** `src/subsystems/tutorial/tutorial.c` (advance loop)

**Rationale:** The `for(;;)` loop that advances through SET/REQUIRE
steps is intentionally iterative (replaced earlier recursion). The
`in_enter_step` guard (#19) may now be vestigial, but the loop
itself is the correct structure.

---

### 40. Replay `replay_state_mut()` used in functions that both read and write

**Where:** `src/subsystems/replay/replay.c:248, 252, 271, 290, 765, 818, ...`

**Rationale:** Most `replay_state_mut()` calls are in functions that
genuinely mutate state. Only #7 and #8 are read-only uses. The
remaining ~20 calls are correct.

---

### 41. Color picker `g_cp_undo_captured` as file-static

**Where:** `src/subsystems/color_picker/color_picker_state.c`

**Rationale:** Same as #27 — internal bookkeeping flag stored as
file-static per project convention. No API surface issue.

---

### 42. Tutorial constants defined in header with `#ifndef` guards

**Where:** `src/subsystems/tutorial/tutorial.h:28-40`

**Rationale:** `TUTORIAL_FADE_CHARS_PER_SEC` and
`TUTORIAL_FADE_SETTLE_CHARS` are `#ifndef`-guarded so tests can
override them. This is the standard project pattern for tunable
constants.

---

### 43. Variable panel `variable_panel_state.c` is 74 lines total

**Where:** `src/subsystems/variable_panel/variable_panel_state.c`

**Rationale:** Tiny, clean peer-state module with
capture/restore/reset/view/mut. No smells to report.

---

### 44. Tutorial `tutorial_state.c` is 40 lines total

**Where:** `src/subsystems/tutorial/tutorial_state.c`

**Rationale:** Same — minimal peer-state boilerplate with explicit
init. Clean.

---

### 45. Replay `replay_state.c` is a pure state-owner with no logic

**Where:** `src/subsystems/replay/replay_state.c`

**Rationale:** Same pattern as the other `*_state.c` files. No findings.

---

## Sequencing

**Priority 1 (reds + most visible UX bug):** #1 → #2 (extract shared
cancel helper per #29), then #12 (tutorial shadow ↔ match
normalization desync). #12 is the most user-triggerable UX issue in
the document — every tutorial user who types extra whitespace hits it.

**Priority 2 (color picker undo gap):** #11 (one-line
`color_picker_stop()` call in `editor_undo_pop_snapshot`). This is
the load-bearing fix for the narrowed #4 scenario.

**Priority 3 (boundary hygiene):** #9 + #14 together (comment fix or
header split), then #5 (time_playing dispatch), then #6
(follow_cursor ownership).

**Priority 4 (cleanup):** #3, #7, #8, #10, #13, #17 — all Tier A
mechanical fixes.

**Defer:** #35, #36 (file splits) are Tier C — only pursue if the
files grow further or if a specific feature needs the separation.

## Method note

Findings were produced by four parallel code reviews (one per
subdirectory), followed by manual verification of all 🔴 claims
against live source at the cited lines. `edit_overlays/` and
`replay/replay_render.{c,h}` were extracted into `src/subsystems/`
after the prior round (commit ffda60e) and are explicitly deferred to
a follow-up pass. Line numbers verified 2026-05-26 against HEAD of
`main`.
