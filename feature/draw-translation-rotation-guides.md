# Transform Guides for glTranslatef / glRotatef

## Context

Today the REPL draws live guide overlays (`draw_guide_xy_plane`, etc.) only while
the user is *typing* a `glVertex3f(` line — `draw_vertex_guides()` inspects
`g_input` mid-keystroke and renders planes/dots to help the user place a point
in 3D.

There is no equivalent feedback for transform commands. When the cursor sits on
a committed `glTranslatef` or `glRotatef` line, the user has no visual cue for
what that line moves/rotates *in context of the surrounding modelview*.

We want a guide that appears when the cursor is on a **complete, valid**
`glTranslatef` or `glRotatef` line, rendered in the local frame of transforms
that come *before* the cursor line, and reflecting where subsequent transforms
have already carried the origin. Examples:

```
glTranslatef(0,1,0);     // cursor here
glTranslatef(0,0,-1);
```

Shows an arrow from `(0,0,-1)` → `(0,1,-1)`.

```
glRotatef(90, 0,1,0);    // cursor here
glTranslatef(0,0,-1);
```

Shows a 90° arc from `(0,0,-1)` swept around the Y axis through the local origin.

## Approach

All work lives in `scene_render.c`. Reuse the existing inline replay loop
(`scene_render.c:2071-2106`) that already walks `g_flat_cmds[]`, tracks
`matrix_depth`, and draws `draw_vertex_guides()` when `is_cursor` is true.

### 1. Compute the "after-cursor" local origin

Key insight: to draw the guide in the correct frame, render with modelview =
product of transforms **strictly before** the cursor line, and express both
endpoints in *that* local frame:

- `P_before_local` = `M_after · origin`
- `P_after_local`  = `M_cursor · M_after · origin`

where `M_after` is the product of transforms that come after the cursor line in
source order.

Add a helper in `scene_render.c`:

```c
static void compute_after_cursor_origin(int first_after_idx, float out[3]);
```

Implementation:

- `glPushMatrix(); glLoadIdentity();`
- Walk `g_flat_cmds[first_after_idx .. g_num_flat_cmds)`.
- For each flat cmd whose type is a transform (`is_transform_cmd`), call
  `apply_tracked_transform_cmd()` (handles push/pop correctly, already used
  at `scene_render.c:2082`).
- Read current matrix via `glGetFloatv(GL_MODELVIEW_MATRIX, m)`; `out = m · (0,0,0,1)`
  → `(m[12], m[13], m[14])`.
- `glPopMatrix();`

Best-effort across nested for-loops / functions: we follow the same flat-cmd
linearization the renderer already uses.

### 2. Detect cursor on a transform line

In the existing loop (`scene_render.c:2071-2106`), once per frame locate the
**first** flat cmd `i` with `src_cmd_idx == g_edit_line`. Pull the owning
source cmd via `g_cmds[g_edit_line]`.

Gate the new guides on:

- `g_cmds[g_edit_line].valid` is true (committed, parsed cleanly).
- `type` is `CMD_TRANSLATE3F` or `CMD_ROTATEF`.
- `!replaying` (match existing vertex-guide gating).
- The cursor line is **not** currently being edited mid-keystroke. Enforced
  by comparing `g_input` against the normalized `g_cmds[g_edit_line].source`
  (ignoring leading whitespace and the trailing `;`) using the same
  `unmodified` pattern as `repl_editor.c:2067-2080`. An empty input buffer
  (`g_input_len == 0`) is also treated as unmodified.

### 3. Draw the translate guide

New helper `draw_translate_guide(const GLCmd *cmd, const float p_after[3])`:

- `tx,ty,tz = cmd->args[0..2]`; `p_before = p_after`; `p_result = p_after + (tx,ty,tz)`.
- Dashed line (`glLineStipple(1, 0xAAAA)`, same style as
  `draw_normal_guides`, `scene_render.c:~1203`) from `p_before` to `p_result`.
- Endpoint dots: small dot at `p_before`, larger at `p_result`
  (`glPointSize(8.0f)` — reuse normal-guide style).
- Color: axis-tinted (e.g. translate = yellow/orange) to distinguish from
  vertex/normal guides.

### 4. Draw the rotate guide

New helper `draw_rotate_guide(const GLCmd *cmd, const float p_start[3])`:

- `angle = cmd->args[0]`, axis `(ax, ay, az) = cmd->args[1..3]`.
- Normalize axis; skip draw if `|axis| < eps` or `angle == 0`.
- Arc as `GL_LINE_STRIP` of ~48 segments from angle `0` to `angle`:
  at step `i`, build quaternion/axis-angle rotation of `theta_i` about the
  axis, apply to `p_start`, emit vertex. (Straightforward Rodrigues formula —
  no new math libs needed.)
- Draw a short axis stub through the local origin along `(ax,ay,az)` so the
  rotation axis is visible.
- Small dot at `p_start` (pre-rotation), larger dot at the final point.
  No tangent stub at the end — the larger dot is sufficient.
- Color derived from the rotation axis via `xform_axis_color(ax,ay,az)`.
  The shaft/arc uses the axes-pulse visual style (dim base + traveling
  pulse dot).

### 5. Integrate into the guide pass

Inside the existing loop at `scene_render.c:2071-2106`, when the first flat cmd
matching `src_cmd_idx == g_edit_line` is about to be applied and the source cmd
is a transform:

- Compute `p_after` via `compute_after_cursor_origin(i_after)`, where
  `i_after` is found by first locating the first flat cmd with
  `src_cmd_idx == g_edit_line`, then scanning forward in flat
  execution order for the next valid flat cmd whose `src_cmd_idx`
  differs. `src_cmd_idx` is **not** monotonic — function-call
  expansions carry the callee's body line — so a numeric `>` compare
  would skip past them.
- Before applying the cursor transform (so modelview = `M_before`), call
  `draw_translate_guide` or `draw_rotate_guide`.
- Continue the existing loop unchanged.

The existing `draw_vertex_guides()` call stays as-is for non-transform cursor
lines.

## Critical files

| File | Change |
|------|--------|
| `scene_render.c` | Add `compute_before_cursor_origin`, `compute_after_cursor_origin`, `is_geometry_emit_cmd`, `xform_axis_color`, `draw_pulse_segment`, `draw_translate_guide`, `draw_rotate_guide`, `draw_scale_guide`; extend the cursor branch in the flat-cmd replay pass |
| `scene_render.h` | No external API change needed |
| `sample.h` | `extern int g_xform_guide_mode;` + `CFG_DEFAULT_XFORM_GUIDE_MODE` |
| `repl_core.c` | Backing storage for `g_xform_guide_mode` (initialized to `CFG_DEFAULT_XFORM_GUIDE_MODE`) and reset entry in `reset_example_presentation_defaults()` |
| `repl_editor.c` | CfgItem entry for the "Xform guide mode" toggle (World/Frame) |

## Verification

1. `make sample && ./sample` — manual smoke test.
2. Type the two example programs from the task description; navigate cursor
   onto each transform line; confirm arrow/arc positions match expected
   `(0,0,-1) → (0,1,-1)` and 90° arc from `(0,0,-1)` around Y.
3. Edge cases to verify interactively:
   - Cursor on invalid/partial transform line — no guide (respects "complete
     and correct" rule).
   - `glRotatef(0, …)` or zero-length axis — no arc drawn.
   - Transform inside a `glPushMatrix/glPopMatrix` block — guide still renders
     in the correct local frame.
   - Transform inside a for-loop — renders against the first unrolled instance
     (best-effort; acceptable per the existing flat-cmd model).
4. `make test` — ensure no regressions in existing parse/format/commit tests
   (rendering-only change, expected to pass unchanged).
