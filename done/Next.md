# Rendering Replay Feature
## Context
The OpenGL immediate-mode REPL stores user commands in `g_cmds[]` (source) and expands them into `g_flat_cmds[]` (flattened: loops unrolled, functions inlined, expressions evaluated) for execution. The user wants a "replay" mode where geometry builds up incrementally -- polygon by polygon or vertex by vertex -- with fade-in, speed control, current-line highlighting in the code panel, and correct variable panel state.
## Architecture
```
                   timer_func (16ms tick)
                        |
          advance g_replay_pc (polygon or vertex step)
          set g_replay_fade_alpha = 0, ramp toward 1.0
          update g_replay_src_line from flat_cmds[pc].src_cmd_idx
                        |
                   display_func
                        |
          flatten_commands() if dirty
          clamp: saved = g_num_flat_cmds
                 g_num_flat_cmds = min(g_replay_pc, saved)
                        |
        +---------+----------+-----------+
        |         |          |           |
   render_3d   outlines  vtx_dots  vtx_nums/normals
   (execute_   (loop     (loop     (loop
    commands)   to g_num   to g_num   to g_num
                _flat)     _flat)     _flat)
        |
        +-- all bounded by clamped g_num_flat_cmds
        |
   restore: g_num_flat_cmds = saved
        |
   render_code_panel  -- highlight g_replay_src_line
   render_var_panel   -- shows vars at replay point (natural)
   render HUD overlay -- progress bar + controls hint
```
**Key insight**: Temporarily clamping `g_num_flat_cmds` before the render pass means ALL existing scene_render.c loops (execute_commands at :3555, outlines at :989, vertex dots at :1075, vertex numbers at :479, normals at :532) automatically respect the replay limit with zero changes to those loops. Restore after rendering.
## Files to modify
| File | Role |
|------|------|
| `sample.h` | Add ReplayState enum, replay extern globals, replay function decls |
| `repl_core.c` | Replay globals, advance logic, timer integration, keyboard bindings, config entries, fade alpha modulation in execute_commands |
| `scene_render.c` | Replay HUD overlay (progress bar + status) |
| `ui_panels.c` | Replay source-line highlight + auto-scroll |
All paths relative to `src/immediate-mode-repl/claude4.6-opus-thinking/`.
## Implementation steps
### Step 1: Types and globals in `sample.h`
After the `CmdType` enum (line 91), add:
```c
typedef enum {
    REPLAY_OFF = 0, REPLAY_PLAYING, REPLAY_PAUSED, REPLAY_DONE
} ReplayState;
```
In the extern globals section (after line 230, near other toggle globals), add:
```c
extern int    g_replay_active;     /* master on/off */
extern int    g_replay_state;      /* ReplayState */
extern int    g_replay_pc;         /* flat cmd program counter (exclusive upper bound) */
extern int    g_replay_mode;       /* 0=polygon, 1=vertex */
extern float  g_replay_speed;      /* steps per second */
extern float  g_replay_accum;      /* fractional step accumulator */
extern float  g_replay_fade_alpha; /* 0..1, current polygon fade-in progress */
extern float  g_replay_fade_speed; /* alpha units per second */
extern int    g_replay_fade_begin; /* flat cmd range of fading polygon */
extern int    g_replay_fade_end;
extern int    g_replay_src_line;   /* source cmd idx for code panel highlight */
```
Add function declarations (after `execute_commands` decl at line 311):
```c
void replay_start(void);
void replay_stop(void);
void replay_advance(void);
int  replay_exec_limit(void);
```
### Step 2: Global definitions and config in `repl_core.c`
Near other toggle globals (~line 559, after `g_ortho_mode`), add the replay global definitions with initial values:
- `g_replay_active = 0`, `g_replay_state = REPLAY_OFF`, `g_replay_pc = 0`
- `g_replay_mode = 0`, `g_replay_speed = 4.0f`, `g_replay_accum = 0.0f`
- `g_replay_fade_alpha = 1.0f`, `g_replay_fade_speed = 3.0f`
- `g_replay_fade_begin = -1`, `g_replay_fade_end = -1`, `g_replay_src_line = -1`
Add replay mode names array:
```c
static const char *replay_mode_names[] = { "Polygon", "Vertex" };
```
Add two entries to `g_cfg_items[]` (line 679, before the closing `}`):
```c
{ "Replay",      "Ctrl+g", &g_replay_active, 2, NULL },
{ "Replay mode", "m",      &g_replay_mode,   2, replay_mode_names },
```
Update `CFG_ITEM_COUNT` - it's auto-computed from sizeof so no change needed.
### Step 3: Replay control functions in `repl_core.c`
Place near `execute_commands()` (~line 3545). These are the core replay engine:
**`replay_start()`**: Set `g_replay_active = 1`, `g_replay_state = REPLAY_PLAYING`, `g_replay_pc = 0`, `g_replay_accum = 0`, `g_replay_fade_alpha = 1.0`, `g_replay_src_line = -1`.
**`replay_stop()`**: Set `g_replay_active = 0`, `g_replay_state = REPLAY_OFF`, `g_replay_src_line = -1`.
**`replay_advance()`**: The step logic, called when accumulator reaches 1.0:
- If `g_replay_mode == 0` (polygon mode): scan forward from `g_replay_pc` through `g_flat_cmds` looking for the next complete primitive:
  - For `CMD_BEGIN`..`CMD_END`: advance pc past the END, set fade_begin/end to the block range
  - For `CMD_GLU_SPHERE/CYLINDER/DISK/PARTIAL_DISK`, `CMD_GLUT_TORUS`: advance pc past the single command
  - For `CMD_TESS_BEGIN_POLYGON`..matching `CMD_TESS_END` (depth tracking): advance past the polygon
  - For non-primitive commands (transforms, colors, normals, enables, var assigns): include them and keep scanning for the next primitive
  - Set `g_replay_fade_alpha = 0.0` to trigger fade-in
- If `g_replay_mode == 1` (vertex mode): advance pc by 1 flat command, brief alpha pulse on vertex commands
- Update `g_replay_src_line = g_flat_cmds[g_replay_pc - 1].src_cmd_idx` (bounds-checked)
- If `g_replay_pc >= g_num_flat_cmds`: set `g_replay_state = REPLAY_DONE`
**`replay_exec_limit()`**: Returns `g_replay_pc` if replay active & not OFF, else returns `g_num_flat_cmds`. (Utility for any code that needs the limit without the clamp trick.)
### Step 4: Clamp `g_num_flat_cmds` during rendering in `display_func()` (repl_core.c:3837)
After `flatten_commands()` runs and `g_flat_dirty` is cleared (line 3839), add:
```c
/* Clamp flat cmd count for replay - all render loops respect this automatically */
int saved_flat_count = g_num_flat_cmds;
if (g_replay_active && g_replay_state != REPLAY_OFF) {
    if (g_replay_pc < g_num_flat_cmds)
        g_num_flat_cmds = g_replay_pc;
    else
        g_replay_pc = g_num_flat_cmds; /* clamp pc after re-flatten */
}
```
After all rendering (before `glutSwapBuffers()`, line 3876):
```c
g_num_flat_cmds = saved_flat_count;
```
This single change makes execute_commands(), outlines, vertex dots, vertex numbers, and normal vectors all respect the replay limit with no per-loop modifications.
### Step 5: Fade-in alpha modulation in `execute_commands()` (repl_core.c:3547)
Add a `float current_alpha = 1.0f` tracking variable at the top. When replay is active and `g_replay_fade_alpha < 1.0`:
- In `CMD_COLOR3F` case (line 3580): if `pc >= g_replay_fade_begin && pc <= g_replay_fade_end`, emit `glColor4f(r, g, b, g_replay_fade_alpha)` instead of `glColor3f`
- In `CMD_COLOR4F` case (line 3584): if in fade range, multiply existing alpha by `g_replay_fade_alpha`
- In `CMD_BEGIN` case (line 3563): if the begin is at `g_replay_fade_begin`, re-emit the default color `(0.7, 0.7, 0.8)` with `g_replay_fade_alpha` via `glColor4f` (since many polygons inherit color without explicit color commands)
### Step 6: Timer integration in `timer_func()` (repl_core.c:5730)
After the existing `g_t_playing` block (line 5739) and before the camera momentum code, add:
```c
if (g_replay_active && g_replay_state == REPLAY_PLAYING) {
    /* Fade-in: ramp alpha toward 1.0 */
    if (g_replay_fade_alpha < 1.0f) {
        g_replay_fade_alpha += g_replay_fade_speed * 0.016f;
        if (g_replay_fade_alpha > 1.0f) g_replay_fade_alpha = 1.0f;
    }
    /* Step accumulator */
    g_replay_accum += g_replay_speed * 0.016f;
    while (g_replay_accum >= 1.0f && g_replay_state == REPLAY_PLAYING) {
        g_replay_accum -= 1.0f;
        replay_advance();
    }
}
```
### Step 7: Keyboard bindings in `keyboard_func()` (repl_core.c:4585)
Add a replay input block early in keyboard_func, after blink reset (line 4590) but before the selection clear (line 4593):
```c
/* Replay controls - intercept keys when replay is active */
if (g_replay_active) {
    if (key == 7) { /* Ctrl+G: toggle off */
        replay_stop();
        return;
    }
    if (key == ' ') { /* Space: pause/resume/restart */
        if (g_replay_state == REPLAY_PLAYING)
            g_replay_state = REPLAY_PAUSED;
        else if (g_replay_state == REPLAY_PAUSED)
            g_replay_state = REPLAY_PLAYING;
        else if (g_replay_state == REPLAY_DONE) {
            g_replay_pc = 0; g_replay_accum = 0;
            g_replay_state = REPLAY_PLAYING;
        }
        return;
    }
    if (key == '+' || key == '=') { /* Faster */
        g_replay_speed *= 1.5f;
        if (g_replay_speed > 200.0f) g_replay_speed = 200.0f;
        char msg[64]; snprintf(msg, sizeof(msg), "Replay: %.1f cmd/s", g_replay_speed);
        set_status(msg);
        return;
    }
    if (key == '-') { /* Slower */
        g_replay_speed /= 1.5f;
        if (g_replay_speed < 0.5f) g_replay_speed = 0.5f;
        char msg[64]; snprintf(msg, sizeof(msg), "Replay: %.1f cmd/s", g_replay_speed);
        set_status(msg);
        return;
    }
    if (key == 'm') { /* Toggle polygon/vertex mode */
        g_replay_mode = !g_replay_mode;
        set_status(g_replay_mode ? "Replay: Vertex mode" : "Replay: Polygon mode");
        return;
    }
    if (key == 27) { /* Escape: stop replay */
        replay_stop();
        return;
    }
    /* Any other key: stop replay, fall through to normal handling */
    replay_stop();
}
```
Also add **Ctrl+G to start replay** (key == 7) in the normal key handling path (after Ctrl+F or nearby):
```c
if (key == 7) { /* Ctrl+G: start replay */
    replay_start();
    return;
}
```
In `special_func()` (line 5369), add at the top of the switch, before GLUT_KEY_LEFT:
```c
if (g_replay_active && g_replay_state == REPLAY_PAUSED && key == GLUT_KEY_RIGHT) {
    replay_advance();  /* single-step when paused */
    return;
}
if (g_replay_active) {
    replay_stop();  /* any other special key cancels replay */
}
```
### Step 8: Replay HUD overlay in `scene_render.c`
At the end of `render_3d_scene()` (before `glPopAttrib()` at line 1107), when `g_replay_active`, draw a 2D overlay at the bottom of the 3D viewport using the existing `begin_2d()/end_2d()` helpers:
- Progress bar: green filled quad proportional to `g_replay_pc / total_flat_cmds`
- Status text: `"REPLAY  42/256  4.0 cmd/s  [Space] pause  [+/-] speed  [m] mode  [Esc] stop"`
- Mode indicator: "Polygon" or "Vertex"
- State: "PLAYING" / "PAUSED" / "DONE"
Position at bottom of the 3D viewport (right of code panel). Use `g_panel_frac` to offset past the code panel. Keep it compact (1-2 lines of text + thin progress bar).
Need to add `extern int g_replay_active;` etc. - already in sample.h so no extra includes needed since scene_render.c includes sample.h.
Note: need to receive the `saved_flat_count` (total commands) for the progress display. Pass it via a new `extern int g_replay_total_flat` global set in display_func before clamping.
### Step 9: Source line highlight in `ui_panels.c`
In `render_code_panel()`, in the "existing command, not being edited" path (line 303-341), before the existing selection highlight check (line 307), add a replay highlight check:
```c
/* Replay position highlight */
if (g_replay_active && g_replay_src_line >= 0 && i == g_replay_src_line) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Green-tinted background */
    glColor4f(0.10f, 0.35f, 0.15f, 0.55f);
    draw_quad(0, (float)(line_y - 3), (float)panel_w, (float)LINE_H);
    /* Green left-gutter accent bar */
    glColor4f(0.20f, 0.90f, 0.30f, 0.85f);
    draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
    glDisable(GL_BLEND);
}
```
Add auto-scroll: when replay is active, treat `g_replay_src_line` as the "cursor" for scroll purposes. In the scroll-follow logic (line 144-152), when `g_replay_active`, use `n_header + g_replay_src_line` as the target line instead of `cursor_doc_line`. Set `g_scroll_follow_cursor = 1` each time `g_replay_src_line` changes (done in timer_func via replay_advance).
### Step 10: Edge cases
1. **Editing cancels replay**: Step 7 already handles this - any unrecognized key calls `replay_stop()` before falling through.
2. **Re-flatten during replay**: Step 4 clamps `g_replay_pc` to new `g_num_flat_cmds` after flatten.
3. **Goto loops**: The existing 100k safety guard in execute_commands works; the clamped `g_num_flat_cmds` also bounds execution.
4. **Empty command buffer**: `replay_start()` should check `g_num_flat_cmds > 0` before starting.
5. **Config menu toggle**: When replay is active and user opens config via backtick, cancel replay (backtick handling is before replay intercept, so it works naturally since the config toggle returns early).
## Verification
1. `make sample` in the claude4.6-opus-thinking directory - must compile clean
2. Run `./sample`, type a few glBegin/glEnd blocks with vertices
3. Press Ctrl+G: geometry should build up incrementally from empty
4. Press Space: pause/resume; when DONE, Space restarts
5. Press +/-: speed changes, status bar shows new rate
6. Press m: toggle polygon/vertex mode
7. Press Right arrow (while paused): single-step
8. Verify code panel highlights the source line corresponding to replay position
9. Verify variable panel shows values at replay position (vars set by CMD_VAR_ASSIGN up to replay_pc)
10. Verify fade-in alpha on newly completed polygons
11. Test with for-loops (expand to many flat commands)
12. Press Esc or Ctrl+G again: replay stops, full scene renders normally
13. Verify editing a line after replay works (replay cancelled, normal behavior resumes)
14. Run `make test` to verify no regressions in parsing/format/commit/IO tests
