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
