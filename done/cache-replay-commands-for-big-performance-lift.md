## Question

running replays becomes slower when replaying scenes with many commands deeper
into the replay.  Two profile sections blow up
`PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE` goes from 0.01ms when replaying the
first command to 4ms when replaying the 2000 th command.  Similarly
`PROF_CODE_PANEL_LINES_BODY_CMDS` goes from about 2ms at the first command to
7ms at the 2000th.

Why does replay become more expensive to render the code panel as it
progresses? Are we replaying all the commands until we hit the 2000th command?

The scene overhead with a lot more geometry only goes from 2.5ms to 4ms

## Response

The slowdown is caused by **O(n²) scaling** in the replay annotation code path,
not by replaying more commands. Here's the breakdown:

### Replay doesn't change `g_num_cmds`

`replay_exec_limit()` only clamps `g_num_flat_cmds` — the code panel still
iterates **all source commands** every frame. The scene rendering correctly
limits work to `g_replay_pc`, but the code panel does not.

### PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE

`code_panel_precompute_layout_rows` (ui_panels.c) loops all `g_num_cmds`. For
each command, it calls `code_panel_command_main_rows` →
`code_panel_get_command_display_text`. During replay, for every
`CMD_VAR_ASSIGN` with `has_vars`, this calls:

1. **`find_replay_assignment_flat_cmd`** → `find_replay_flat_cmd` (ui_panels.c)
   — scans **backwards from `g_replay_pc` to 0** to find the matching flat cmd.
   This is O(replay_pc) per call.

2. **`build_replay_assignment_inline_comment`** →
   `replay_copy_predef_values_before_flat_cmd` (ui_panels.c) — walks **forward
   from 0 to `flat_idx`**, re-evaluating every assignment. Also O(replay_pc).

So for N variable-assignment commands: **O(N × replay_pc)** per frame. At
command 2000, that's millions of iterations just for layout.

### PROF_CODE_PANEL_LINES_BODY_CMDS

The rendering loop calls `code_panel_get_command_display_text` **again** for
every non-edit command — duplicating all the same O(replay_pc) scans from
precompute. Then for `g_replay_src_line`, it additionally calls
`find_replay_flat_cmd`, `build_replay_subst_annotation`, and
`build_replay_eval_annotation` — each triggering another O(replay_pc) scan.

### Summary

| Function | Complexity per frame | Why it grows |
|---|---|---|
| `find_replay_flat_cmd` | O(replay_pc) per call | Linear scan backwards |
| `replay_copy_predef_values_before_flat_cmd` | O(replay_pc) per call | Forward walk replaying all assignments |
| Both called N times across precompute + render | O(N × replay_pc) total | Called for every var-assign command |

The scene only going from 2.5ms→4ms makes sense — it's bounded by
`replay_exec_limit()` which is O(replay_pc) once. The code panel is O(N ×
replay_pc) due to repeated linear scans.

Want me to fix this? The main approaches would be: (1) cache the flat-cmd index
mapping instead of scanning each frame, (2) cache predef values at
`g_replay_pc` instead of replaying from scratch per-command, and (3) avoid
calling `code_panel_get_command_display_text` twice (once in precompute, once
in render).

## Implementation

Three per-frame caches are built once in `render_code_panel` when replay is
active (inside `rebuild_replay_annotation_cache()`), reducing the hot path from
O(N × replay_pc) to O(replay_pc) + O(N):

### 1. Flat-cmd index map (`s_replay_flat_map[MAX_COMMANDS]`)

A single backward pass from `g_replay_pc` builds a src_cmd_idx → flat_idx
lookup table. `find_replay_flat_cmd` and `find_replay_assignment_flat_cmd` now
do O(1) map lookups instead of O(replay_pc) backward scans per call. Fallback
to full scan is preserved for the rare context-mismatch case in function bodies.

### 2. Per-source predef snapshots (`s_replay_predef_snap[MAX_COMMANDS][MAX_PREDEF_VARS]`)

A single O(replay_pc) forward simulation (`replay_build_predef_snapshots()`)
replicates the full `replay_copy_predef_values_before_flat_cmd` control-flow
logic (var-assign updates, if-block skipping, goto handling) and snapshots
predef values at each flat position referenced by `s_replay_flat_map`. Each
snapshot captures the state **before** that flat command executes — matching
the exact semantics of the original per-call function.

`build_replay_assignment_inline_comment`, `build_replay_subst_annotation`, and
`build_replay_eval_annotation` use `memcpy` from the per-source snapshot when
the cache covers the exact cmd_idx/flat_idx pair, falling back to the full
forward simulation only for the rare context-mismatch fallback path.

An earlier version used a single snapshot at `g_replay_pc` for all commands,
but PR review correctly identified that as semantically wrong: each annotation
needs values "before flat_idx", which varies per source command.

### 3. Cached `s_replay_current_flat_idx`

Caches `replay_current_flat_cmd()` so `find_replay_assignment_flat_cmd`
doesn't trigger an additional backward scan on every call.

### Complexity

**Before**: For K var-assign commands at replay position P, cost was
~K × 3 × O(P) per frame. At command 2000 with 20 var-assigns, that's ~120,000
iterations per frame in precompute alone, then repeated in the render loop.

**After**: One O(P) backward pass + one O(P) forward simulation, then O(1) per
annotation. Total per frame: O(P + N) instead of O(N × P).
