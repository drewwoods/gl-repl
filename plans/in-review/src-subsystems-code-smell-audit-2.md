# `src/subsystems/` — Code-Smell Audit (Round 2)

> Audit produced 2026-05-26. Findings come from four parallel reviews
> covering `replay/`, `tutorial/`, `color_picker/`, and
> `variable_panel/` plus targeted spot-verification of the most
> actionable claims. File:line references are exact at the time of
> writing — check `git log` on the cited files before acting if this
> doc has aged.
>
> Scope: every `.c`/`.h` file under `src/subsystems/`. The prior round
> (`plans/done/src-subsystems-code-smell-audit.md`) closed 58 of 59
> findings; #56 (cross-module `time_playing` write) is tracked in the
> `src/app/` audit and noted here for context but not re-counted.
> `edit_overlays/` was extracted out of this directory between rounds
> and no longer exists — it is out of scope.
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

45 findings across four subdirectories. **4 reds** — two in replay
(key-leak-after-stop for both ASCII and special handlers) and two in
color_picker (half-init on early return, stale-index writeback after
deletion). The 🟡 band is dominated by replay reaching across module
boundaries (`time_playing` writes, `repl_dispatch_follow_cursor`) and
the tutorial including `state_owners.h`. The 🟢/🔵 tail is light —
one `g_` naming violation, a few dead enum literals, a handful of
structural observations. This directory is largely healthy after the
prior round; the reds are the priority.

**Counts:** 4 🔴, 12 🟡, 11 🟢, 18 🔵 = 45 total.

## Tier classification

| Tier | Criteria | Findings |
|------|----------|----------|
| **A** | Small, safe, afternoon pass | #1, #2, #3, #4, #7, #8, #10, #11, #12, #13, #14, #15, #17, #18, #20, #21, #22, #23, #24, #25, #26, #27, #28 |
| **B** | Moderate, week-long pass with tests | #5, #6, #9, #16, #19, #29, #30, #31, #32, #33, #34 |
| **C** | High cost or cross-cutting | #35, #36 |
| **D** | Accepted / kept | #37, #38, #39, #40, #41, #42, #43, #44, #45 |

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
consumed by the "cancel replay" action. (Tier A)

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
block. (Tier A)

---

### 3. Color picker `color_picker_open` sets `g_cp_line` before null check — half-init on early return

**Where:** `src/subsystems/color_picker/color_picker_state.c:222-225`

**Smell:** `g_cp_line = cmd_idx` is written at L222 before `cmd = cp_cmd_at(cmd_idx)` at L223. If `cp_cmd_at` returns NULL (line was deleted between click and open), the function returns early at L225, leaving the picker in a half-initialized "open" state: `g_cp_line` points at a nonexistent command, `g_cp_active` was set to 1 earlier in the function, but color channels were never populated.

**Why it matters:** Subsequent `color_picker_write_cmd()` calls use
`g_cp_line` to look up the target command. With a stale index, the
write could land on the wrong command or on a NULL (which is safely
guarded at L113, but the picker stays visually open with garbage).

**Fix:** Move `g_cp_line = cmd_idx` below the null check, or clear
`g_cp_active = 0` in the early-return path. (Tier A)

---

### 4. Color picker `color_picker_write_cmd` writes to stale index after deletion

**Where:** `src/subsystems/color_picker/color_picker_state.c:107-108`

**Smell:** `color_picker_write_cmd()` fetches `cp_cmd_at(g_cp_line)`.
If lines were deleted while the picker was open (e.g., undo, cut,
line delete), `g_cp_line` may point past the array end or at a
different command type. The type switch at L117-138 then synthesizes
a line for the wrong command, and `editor_commit_apply_external_change`
overwrites it.

**Why it matters:** Silent data corruption — the wrong source line
gets its content replaced with a color command string. The NULL guard
at L113 catches the past-end case, but an index shifted to a
non-color command still passes and writes garbage.

**Fix:** Before writing, validate that `cp_cmd_at(g_cp_line)` still
has the same `CmdType` as when the picker was opened (save the
original type in a `g_cp_opened_type` static). If it doesn't match,
close the picker with a status message. (Tier B)

---

## 🟡 Drift / boundary hazards

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

---

### 7. Replay `replay_fade_batches_view` uses `replay_state_mut()` for read-only access

**Where:** `src/subsystems/replay/replay.c:169`

**Smell:** `replay_fade_batches_view()` is a read-only function (it
returns a view struct) but acquires the state via `replay_state_mut()`
instead of `replay_state_view()`.

**Why it matters:** Naming-convention violation. The `_mut()` accessor
communicates write intent to readers; using it for reads erodes that
signal.

**Fix:** Use `replay_state_view()` and take pointers via address-of on
the local copy, or make the view struct hold copies directly. (Tier A)

---

### 8. Replay `replay_copy_baseline_predef_snapshot` uses `_mut()` for read-only access

**Where:** `src/subsystems/replay/replay.c:1001`

**Smell:** Same pattern as #7 — a copy-out function that only reads
state uses `replay_state_mut()`.

**Fix:** Switch to `replay_state_view()`. (Tier A)

---

### 9. Tutorial includes `repl/state_owners.h` — owner-only header from a peer

**Where:** `src/subsystems/tutorial/tutorial.c:11`

**Smell:** `state_owners.h` is the "owner modules and controller only"
header per the project docs. The tutorial subsystem includes it for
`repl_state_mark_flat_dirty`, `repl_state_mark_source_dirty`, and
`repl_state_parse_workspace_header_line`. This violates the stated
ownership boundary.

**Why it matters:** If the ownership guard (`check-state-ownership`)
is ever tightened to whitelist includers, the tutorial would break.
It also makes it easy to accidentally reach other mutable state
through the header.

**Fix:** Move the three needed declarations into a narrow
`repl/state_notify.h` header (or `repl/state_views.h` if they're
logically read-like), keeping the mutable-accessor surface in
`state_owners.h`. (Tier B — touches header split + guard update)

---

### 10. Color picker uses bare `0` instead of `CP_DRAG_NONE` enum for `g_cp_drag`

**Where:** `src/subsystems/color_picker/color_picker_state.c:257`

**Smell:** `g_cp_drag = 0;` resets the drag state using a magic
integer instead of the defined `CP_DRAG_NONE` constant.

**Why it matters:** If the enum values are ever renumbered (e.g., to
add a sentinel), this assignment becomes wrong silently.

**Fix:** Use `g_cp_drag = CP_DRAG_NONE;`. (Tier A)

---

### 11. Color picker `g_cp_active` has no "close on scene/example switch" invalidation

**Where:** `src/subsystems/color_picker/color_picker_state.c` (global state)

**Smell:** When a scene switch, example load, or undo replaces the
command array, the picker's `g_cp_line` index and cached color
channels become stale. There is no call to `color_picker_close()`
from the scene-switch or undo paths.

**Why it matters:** The picker could be open after a scene switch with
`g_cp_line` pointing into the old scene's index space. The first
drag would write into the wrong line of the new scene (see #4).

**Fix:** Call `color_picker_close()` from `glr_ctrl_reset_transients()`
or from the undo-restore path that already clears other transient UI
state. (Tier A)

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

---

### 15. Replay `replay_start` caches `num_flat_cmds` but `total_flat_cmds` can drift

**Where:** `src/subsystems/replay/replay.c:860`

**Smell:** `state->total_flat_cmds = num_flat_cmds` snapshots the flat
program count at start time. If a workspace/scene switch occurs
during replay (possible via menu), the flat array is rebuilt but
`total_flat_cmds` stays at the old value. PC bounds checks use this
stale count.

**Why it matters:** If the new scene has fewer flat cmds than
`total_flat_cmds`, the PC could overshoot the array, reading
uninitialized memory. (The current UI likely prevents mid-replay
scene switches, but the code doesn't assert it.)

**Fix:** Re-snapshot `total_flat_cmds` from the live flat program each
frame in `replay_advance_frame`, or add a guard that stops replay on
scene switch (cleaner). (Tier A)

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

---

## 🟢 Dead code / dead fields

### 17. Replay `g_flat_cmds` / `g_num_flat_cmds` local variables use `g_` prefix

**Where:** `src/subsystems/replay/replay.c:179-180`

**Smell:** Local variables in `replay_compute_fade_skip_limits` are
named `g_flat_cmds` and `g_num_flat_cmds`, using the `g_` prefix
reserved by project convention for file-scoped statics.

**Why it matters:** Misleading to readers scanning for global state.

**Fix:** Rename to `flat_cmds` / `num_flat_cmds`. (Tier A)

---

### 18. Color picker `CP_DRAG_NONE` defined but not used at all reset sites

**Where:** `src/subsystems/color_picker/color_picker_state.c`

**Smell:** `CP_DRAG_NONE` exists as a named constant but at least one
reset site uses `0` directly (see #10). Other sites may also use
bare integers.

**Fix:** Audit all `g_cp_drag =` sites and replace with the enum. (Tier A)

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

---

### 22. Replay `state->accum = 0.0f` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:871`

**Smell:** Same pattern as #21 — `accum` is re-initialized in
`replay_start()` and never read when state is OFF.

**Fix:** Remove or mark as defensive. (Tier A)

---

### 23. Replay `state->pc = 0` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:870`

**Smell:** Same pattern as #21/#22.

**Fix:** Remove or mark as defensive. (Tier A)

---

### 24. Replay `state->total_flat_cmds = 0` in `replay_stop` — dead write

**Where:** `src/subsystems/replay/replay.c:874`

**Smell:** Same pattern.

**Fix:** Remove or mark as defensive. (Tier A)

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

**Fix:** Accept as-is (it's one float and works) or move to the
per-frame snapshot. (Tier A)

---

### 27. Color picker `g_cp_undo_captured` is never read from outside the file

**Where:** `src/subsystems/color_picker/color_picker_state.c:218-219`

**Smell:** `g_cp_undo_captured` is file-static and only checked/set
within `color_picker_open` and `color_picker_write_cmd`. It's not
dead, but it's also not visible through any public API — pure
internal bookkeeping that could be a struct field for clarity.

**Fix:** Accept as-is — file-statics for internal flags is the
project convention. (Tier D)

---

## 🔵 Structural concerns

### 28. Replay `replay_handle_key` is 48 lines of `if (key == ...) return 1` ladder

**Where:** `src/subsystems/replay/replay.c:1072-1136`

**Smell:** Long sequential key-matching chain. Each recognized key
returns 1; the fallthrough stops replay and returns 0 (#1). A
dispatch table or switch would be clearer and make the "consume"
contract explicit.

**Fix:** Refactor to `switch (key)` with explicit `default:
replay_stop(); return 1;`. (Tier A)

---

### 29. Replay has two near-identical "unrecognized key stops replay" sites

**Where:** `src/subsystems/replay/replay.c:1133-1136` and `:1172-1177`

**Smell:** Both the ASCII and special key handlers duplicate the
cancel pattern (set status, stop, return). A shared
`replay_cancel_on_unrecognized()` helper would centralize the fix
for #1/#2.

**Fix:** Extract a helper returning 1. (Tier A)

---

### 30. Replay `replay_start` is 30+ lines mixing state init, snapshot, and side effects

**Where:** `src/subsystems/replay/replay.c:840-863`

**Smell:** One function does: copy predef vars, copy scratch arrays,
snapshot time_playing, toggle time_playing, set active/state/pc/accum,
clear fade batches, snapshot flat count, set status. Mixing
snapshot + mutation + side effect in one linear block.

**Fix:** Split into `replay_snapshot_baseline()` +
`replay_init_playback_state()` + status set. (Tier B)

---

### 31. Tutorial `tutorial_enter_step` is ~100 lines with nested control flow

**Where:** `src/subsystems/tutorial/tutorial.c` (around L260-370)

**Smell:** Handles COMMAND, SET, and REQUIRE step kinds in one
function with nested ifs, a `for(;;)` advance loop, and the
reentrance guard. Readable but approaching the complexity threshold.

**Fix:** Extract per-kind helpers (`enter_command_step`,
`enter_set_step`, `enter_require_step`) called from a switch. (Tier B)

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

---

### 33. Replay seek/advance/step functions share a "clamp PC and update src_line" postamble

**Where:** `src/subsystems/replay/replay.c` (multiple sites)

**Smell:** After each PC mutation, the same clamping and
`src_line_idx` update logic repeats. A `replay_update_after_pc_change()`
helper would deduplicate.

**Fix:** Extract the shared postamble. (Tier B)

---

### 34. Variable panel drag has two scaling modes inlined in one function

**Where:** `src/subsystems/variable_panel/variable_panel_drag.c:40-80`
(approximately)

**Smell:** `variable_panel_handle_drag_motion` contains both the
linear and logarithmic value-mapping paths in one if/else branch.
Each is ~15 lines with different math. They share no code.

**Fix:** Extract `drag_linear_value()` and `drag_log_value()` helpers.
(Tier B)

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

## 🔵 / Tier D — Accepted / documented

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

**Priority 1 (reds, afternoon):** #1 → #2 → #3 → #11 → #4.
Fix #1/#2 together (extract shared cancel helper per #29).
Fix #3 before #4 — the half-init leaves `g_cp_line` stale which is
what makes #4's writeback dangerous. #11 (close-on-scene-switch)
prevents the scenario that triggers #4 in practice.

**Priority 2 (boundary hygiene):** #9 + #14 together (header split),
then #5 (time_playing dispatch), then #6 (follow_cursor ownership).

**Priority 3 (cleanup):** #7, #8, #10, #12, #13, #17 — all Tier A
mechanical fixes.

**Defer:** #35, #36 (file splits) are Tier C — only pursue if the
files grow further or if a specific feature needs the separation.

## Method note

Findings were produced by four parallel code reviews (one per
subdirectory), followed by manual verification of all 🔴 claims
against live source at the cited lines. The `edit_overlays/`
subdirectory no longer exists in `src/subsystems/` (only stale build
artifacts remain) and was excluded. Line numbers verified
2026-05-26 against HEAD of `main`.
