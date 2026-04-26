# scene_* Push-Model Architecture

## Context

Today all `scene_*.c` files pull state at render time from `repl_state.h` and
call directly into `repl_executor.h`, `repl_core.h`, and `ui_panels.h`. This
means nothing in the scene layer can run without the full REPL + UI stack.

The goal is a clean **push model**:

- All state is built once per frame by the REPL caller and pushed into
  `SceneRenderConfig` before calling any `scene_*` entry point.
- The `repl_execute_program()` call in scene_render.c becomes an **optional
  function callback pointer** inside `SceneRenderConfig`. When `NULL`, the
  scene renders without user geometry — grid, backdrop, axes, lights, and
  overlays all work; only user GL commands are suppressed.
- `scene_*.c` files depend only on **common code**: `sample.h`,
  `gl_includes.h`, `gl_2d.h`, `repl_flatten.h` (for `FlatProgramView` /
  `GLCmd`). No direct includes of `repl_state.h`, `repl_executor.h`,
  `repl_core.h`, `repl_replay.h`, or `ui_panels.h`.

The existing `scene_render_3d_scene()` public signature is **unchanged**. All
callers are unaffected. The internal wiring changes; the external interface
stays the same.

---

## Current Pull-Model Problems

| File | What it pulls directly |
|------|------------------------|
| `scene_render.c` | `repl_state_camera()`, `repl_state_presentation()`, `repl_state_replay()`, `repl_state_viewport()`, `repl_state_flat_program_view()`, `repl_execute_program()`, `ui_panels_scene_rect()`, and replay helpers |
| `scene_grid.c` | `anim_time`, `viewport_w/h`, `presentation->grid_extents[]`, `presentation->grid_major_steps[]`, `flat_program_user_lighting_enabled()` |
| `scene_axes.c` | `anim_time`, `flat_program_user_lighting_enabled()` |
| `scene_backdrop.c` | `anim_time`, `presentation->backdrop_mode`, `flat_program_user_lighting_enabled()` |
| `scene_lights.c` | `render->lights[]`, `anim_time`, `presentation->show_light_indicators`, `flat_program_user_lighting_enabled()` |
| `scene_overlays.c` | `flat_program_view()`, `render->multisample_enabled/line_smooth_enabled`, `presentation->show_vertex_outlines`, `repl_flat_cmd_matches_cursor()` from `repl_core.h`, `repl_execute_program()` from `repl_executor.h` |
| `scene_transform_guides.c` | `repl_executor_apply_tracked_transform_cmd()`, `repl_executor_unwind_tracked_transform_stack()` from `repl_executor.h` |

---

## Execute Callback Design

The callback typedef lives in `scene_render_types.h`. It uses only
`FlatProgramView` from `repl_flatten.h` — no `repl_executor.h` types:

```c
/* Called by scene_render.c to emit user geometry. May be NULL (geometry
 * is silently skipped; scene background/grid/axes/lights still render).
 *
 * alpha_scale:          1.0 for normal pass; 0.0–1.0 for replay fade batches
 * skip_geom_before_pc:  first flat cmd index to emit geometry for (0 = all;
 *                       used by fade passes to suppress already-faded prefix)
 * flat_cmd_count:       number of commands to execute from program.cmds[]
 * program:              the flat command buffer + local vars
 * user_data:            opaque pointer supplied at config build time
 */
typedef void (*SceneExecuteProgramFn)(float alpha_scale,
                                     int skip_geom_before_pc,
                                     int flat_cmd_count,
                                     FlatProgramView program,
                                     void *user_data);
```

`SceneRenderConfig` gains three fields:

```c
SceneExecuteProgramFn execute_fn;          /* NULL = skip user geometry */
void                 *execute_user_data;
void (*execute_reset_fn)(void *user_data); /* called after last fade batch;
                                              resets executor fade state */
```

`repl_core.c` provides the adapters (static, not exported):

```c
static void scene_execute_adapter(float alpha, int skip, int count,
                                  FlatProgramView prog, void *ud) {
    (void)ud;
    repl_execute_set_fade_context(alpha, skip);
    repl_execute_program(&(ReplExecutionOptions){ .flat_cmd_count = count,
                                                  .program = prog });
}
static void scene_execute_reset_adapter(void *ud) {
    (void)ud;
    repl_execute_set_fade_context(1.0f, 0);
}
```

These are set in `scene_render_config_build()` (see below) so that the live
rendering path is wired transparently.

---

## New Fields Added to SceneRenderConfig

Grouped by domain. All filled by `scene_render_config_build()` in `repl_core.c`.

```c
/* ── Execute callback ─────────────────────────────────────────────────── */
SceneExecuteProgramFn execute_fn;          /* NULL = no geometry */
void                 *execute_user_data;
void (*execute_reset_fn)(void *user_data);

/* ── Flat program (snapshot for overlays / outline pass) ─────────────── */
FlatProgramView flat_program;              /* repl_state_flat_program_view() */

/* ── Animation ────────────────────────────────────────────────────────── */
float anim_time;                           /* *repl_state_variables()->anim_time */

/* ── Viewport (for 2D overlays and ocean fill rect) ──────────────────── */
int viewport_w;
int viewport_h;

/* ── Lighting ─────────────────────────────────────────────────────────── */
int        user_lighting_enabled;          /* repl_state_flat_program_user_lighting_enabled() */
SceneLight lights[MAX_LIGHTS];             /* deep copy of render->lights[] */
int        show_light_indicators;

/* ── Backdrop ─────────────────────────────────────────────────────────── */
int backdrop_mode;

/* ── Outline overlay ──────────────────────────────────────────────────── */
int show_vertex_outlines;

/* ── Replay HUD / layout ──────────────────────────────────────────────── */
int   code_panel_layout;
int   replay_pc;
int   replay_total_cmds;
int   replay_state_val;     /* REPLAY_PLAYING / REPLAY_PAUSED / REPLAY_DONE */
float replay_speed;
int   replay_expand_args;

/* ── Grid tables (GRID_MAJOR_COUNT and GRID_EXTENT_COUNT are small enums) */
float grid_major_steps[GRID_MAJOR_COUNT]; /* memcpy from presentation->grid_major_steps */
float grid_extents[GRID_EXTENT_COUNT];    /* memcpy from presentation->grid_extents */

/* ── Cursor block bounds (replaces repl_flat_cmd_matches_cursor()) ─────── */
int          cursor_block_begin_idx;  /* -1 = no active block */
int          cursor_block_end_idx;
int          cursor_block_source_line;
int          edit_line_idx;
unsigned int cursor_func_scope_mask;
int          cursor_call_src_cmd_idx; /* -1 = cursor not on a CMD_CALL */
```

`FrameRenderContext` is **unchanged** — the new fields live in the embedded
`SceneRenderConfig` and are reached via `frame_ctx->config.*`.

---

## Config Build: scene_render_config_build()

A new function declared in `scene_render.h`, implemented in `repl_core.c`:

```c
/* Populate *config from the current REPL runtime state for one frame.
 * Call once at frame start before any scene_* entry point. */
void scene_render_config_build(SceneRenderConfig *config);
```

`scene_render.c` calls this at the top of `scene_render_3d_scene()` instead of
the current static `scene_render_config_init()`.

Key responsibilities of this function:
- Reads `repl_state_camera()`, `repl_state_presentation()`,
  `repl_state_render()`, `repl_state_replay()`, `repl_state_viewport()`,
  `repl_state_variables()` — **these calls stay in `repl_core.c`** where they
  already belong.
- Calls `ui_panels_scene_rect()` to fill `scene_x/y/w/h` — this call moves
  from `scene_render.c` to `repl_core.c`.
- Calls `refresh_current_block_highlight()` (or a wrapper) before reading
  `current_block_begin/end_idx` so the cursor block is current.
- Deep-copies `lights[MAX_LIGHTS]` and the two grid tables by value.
- Sets `execute_fn = scene_execute_adapter`, `execute_reset_fn =
  scene_execute_reset_adapter`, `execute_user_data = NULL`.

---

## New File: scene_transform_utils.h

Inline helpers that eliminate `repl_executor.h` from `scene_transform_guides.c`
and `scene_overlays.c`. No `.c` companion — the bodies are small enough to inline.

```c
/*
 * scene_transform_utils.h - GL matrix transform helpers for scene modules.
 *
 * Mirrors repl_executor_apply_tracked_transform_cmd /
 * repl_executor_unwind_tracked_transform_stack without requiring
 * repl_executor.h.  Depends only on sample.h (GLCmd, CmdType).
 */
#ifndef SCENE_TRANSFORM_UTILS_H
#define SCENE_TRANSFORM_UTILS_H
#include "sample.h"
#include <gl_includes.h>

/* Apply a single transform command to the GL matrix stack.
 * Increments *depth on glPushMatrix, decrements on glPopMatrix. */
static inline void scene_apply_tracked_transform(const GLCmd *cmd,
                                                  int *depth) { ... }

/* Pop the GL matrix stack until *depth reaches zero. */
static inline void scene_unwind_transform_stack(int *depth) { ... }

#endif /* SCENE_TRANSFORM_UTILS_H */
```

---

## Eliminating repl_flat_cmd_matches_cursor() From scene_overlays.c

`repl_flat_cmd_matches_cursor()` in `repl_core.c` currently encapsulates the
cursor-block highlight logic. After this refactor, `scene_overlays.c` reimplements
it locally using only fields available from `SceneRenderConfig` plus the flat
command's own metadata fields (`src_cmd_idx`, `call_src_cmd_idx`,
`root_call_src_cmd_idx`, `func_scope_mask` — all already in `GLCmd`):

```c
/* Returns non-zero if the flat command at flat_idx should be highlighted
 * as part of the cursor's current block. Uses only pushed config state — no
 * repl_core.h call needed. */
static int overlay_flat_cmd_matches_cursor(int flat_idx,
                                           const SceneRenderConfig *cfg) {
    const GLCmd *cmd = &cfg->flat_program.cmds[flat_idx];

    /* CMD_CALL: highlight commands inlined from the called function */
    if (cmd->call_src_cmd_idx == cfg->edit_line_idx ||
        cmd->root_call_src_cmd_idx == cfg->edit_line_idx)
        return 1;

    /* Function scope: highlight commands sharing the cursor's func scope */
    if (cfg->cursor_func_scope_mask &&
        (cmd->func_scope_mask & cfg->cursor_func_scope_mask))
        return 1;

    /* Block range: highlight commands inside the cursor's begin..end block */
    if (cfg->cursor_block_begin_idx >= 0 &&
        flat_idx >= cfg->cursor_block_begin_idx &&
        flat_idx <= cfg->cursor_block_end_idx)
        return 1;

    /* Direct: flat cmd originates from the cursor source line */
    return cmd->src_cmd_idx == cfg->edit_line_idx;
}
```

The side-effect of refreshing the current block is moved to
`scene_render_config_build()` in `repl_core.c`.

---

## Replay Fade Pass: Moving to repl_core.c

`scene_render_replay_fade_pass()` calls `repl_replay_*` orchestration
functions and the executor — it belongs in REPL territory. Move it from
`scene_render.c` to a static function in `repl_core.c`. The public declaration
is removed from `scene_render.h`. The call site inside `render_3d_scene_pass()`
is replaced with `config.execute_fn` calls per batch:

```c
if (config.replay_has_fades && config.execute_fn) {
    /* For each fade batch: */
    config.execute_fn(batch_alpha, skip_limit, batch_pc,
                      config.flat_program, config.execute_user_data);
}
if (config.execute_reset_fn)
    config.execute_reset_fn(config.execute_user_data);
```

---

## Updated Includes Per File

| File | Removed | Added |
|------|---------|-------|
| `scene_render.c` | `repl_core_internal.h`, `repl_executor.h`, `repl_replay.h`, `repl_state.h`, `ui_panels.h` | `scene_transform_utils.h` |
| `scene_grid.c` | `repl_state.h` | — |
| `scene_axes.c` | `repl_state.h` | — |
| `scene_backdrop.c` | `repl_state.h` | — |
| `scene_lights.c` | `repl_state.h` | — |
| `scene_overlays.c` | `repl_executor.h`, `repl_core.h`, `repl_state.h` | `scene_transform_utils.h`, `scene_render_types.h` |
| `scene_transform_guides.c` | `repl_executor.h`, `repl_state.h` | `scene_transform_utils.h` |
| `scene_render_types.h` | — | `repl_flatten.h` |
| `repl_core.c` | — | `scene_render_types.h` (for SceneRenderConfig) |

New files: `scene_transform_utils.h` (inline-only, no `.c` companion).

---

## Makefile Boundary Checks

**Remove the existing `ui_panels.h` exception:**

```makefile
# Old (check-layer-coupling):
grep -nE '#include\s+"ui_' scene_*.c scene_*.h | grep -vE 'scene_render\.c:.*ui_panels\.h'

# New — no exception needed:
grep -nE '#include\s+"ui_' scene_*.c scene_*.h
```

**Add two new guards (wired into `make test`):**

```makefile
check-scene-boundaries: ## scene_*.c must not include repl_state.h, repl_executor.h, or repl_core.h
    @! grep -nE '#include\s+"repl_state\.h"' scene_*.c \
        || (echo "ERROR: scene_*.c must not include repl_state.h" && exit 1)
    @! grep -nE '#include\s+"repl_(executor|core)\.h"' scene_*.c \
        || (echo "ERROR: scene_*.c must not include repl_executor.h / repl_core.h" && exit 1)
    @echo "scene/repl boundaries OK"
```

---

## Staged Commits

Each commit must compile and pass `make test-stubs TEST_JOBS=4`.

| # | Commit | Files Touched |
|---|--------|---------------|
| 1 | `feat: add SceneExecuteProgramFn callback typedef and new fields to SceneRenderConfig` | `scene_render_types.h` |
| 2 | `refactor: add scene_render_config_build() in repl_core.c, remove repl_state.h from scene_render.c, move fade pass` | `repl_core.c`, `scene_render.c`, `scene_render.h` |
| 3 | `refactor: remove repl_state.h from scene_grid.c and scene_axes.c` | `scene_grid.c`, `scene_axes.c` |
| 4 | `refactor: remove repl_state.h from scene_backdrop.c and scene_lights.c` | `scene_backdrop.c`, `scene_lights.c`, `scene_lights.h`, `scene_backdrop.h` |
| 5 | `refactor: add scene_transform_utils.h, remove repl_executor.h from scene_transform_guides.c` | `scene_transform_utils.h` (new), `scene_transform_guides.c` |
| 6 | `refactor: remove repl_executor.h, repl_core.h, repl_state.h from scene_overlays.c` | `scene_overlays.c`, `scene_overlays.h` |
| 7 | `refactor: remove remaining repl/ui includes from scene_render.c` | `scene_render.c` |
| 8 | `refactor: tighten Makefile boundary checks for scene_* layer` | `Makefile` |

Commit 2 is the largest and highest-risk. Commits 3–5 are small and mechanical.
Commit 6 carries the cursor-match logic rewrite. Commit 7 is purely cleanup
once 2–6 have done the work.

---

## Verification

- `make test-stubs TEST_JOBS=4` after every commit
- `make test` after Commit 8 (validates new Makefile guards)
- Manual smoke test of live rendering after Commit 7: grid, axes, backdrop,
  lights, overlays, replay, fade pass all exercise the changed code paths
- Optionally: write a `test_scene_config.c` that calls `scene_render_3d_scene()`
  with `config.execute_fn = NULL` in GL-stubs mode and verifies no crash and
  `GL_STUB_glBegin` count is non-zero (grid/backdrop drew) but no user-geometry
  calls are issued

---

## Open Questions / Notes

- **`scene_lights_setup()` mutation**: currently writes `lights[i].enabled = 0`
  as a per-frame reset. After this refactor the lights array is a const copy in
  `SceneRenderConfig`. The reset must move to `repl_core.c` (before building
  config). A helper `repl_lights_reset_all_enabled()` encapsulates this.
- **`repl_state_render_derived()` write-back**: `scene_prepare_focus_vertex()`
  currently writes the focus vertex back to global derived state. After Commit 2,
  this write-back is no longer needed — the result stays local to
  `FrameRenderContext.focus`. The global `ReplRenderDerivedState` write is
  eliminated.
- **GL-stubs test feasibility**: `render_3d_scene_pass()` is currently a static
  function. A thin `#ifdef OPENGL_VIBE_USE_GL_STUBS` wrapper in `scene_render.c`
  could expose it for testing.
