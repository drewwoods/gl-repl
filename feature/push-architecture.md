# scene_* Push-Model Architecture

> Superseded by
> [`feature/push-architecture-refinement.md`](push-architecture-refinement.md).
> The current direction is Option B controller extraction through
> `imrepl_ctrl.c`, not the generic scene-callback/scene-plugin boundary
> described below.

## Context

Today all `scene_*.c` files pull state at render time from `repl_state.h` and
call directly into `repl_executor.h`, `repl_core.h`, and `ui_panels.h`. This
means nothing in the scene layer can run without the full REPL + UI stack.

The goal is a clean **push model**:

- A REPL-owned adapter builds all per-frame state once and pushes it into
  `SceneRenderConfig`.
- The scene layer exposes a config-driven entry point:
  `scene_render_3d_scene_with_config(const SceneRenderConfig *config)`.
- The existing `scene_render_3d_scene()` public API remains available as the
  REPL compatibility wrapper. Existing callers keep working, but the standalone
  scene renderer no longer depends on that wrapper.
- The `repl_execute_program()` call becomes an **optional function callback**
  inside `SceneRenderConfig`. When `NULL`, the scene renders without user
  geometry — grid, backdrop, axes, lights, and non-user overlays still work;
  only user GL commands are suppressed.
- `scene_*.c` files depend only on **common code**: `sample.h`,
  `gl_includes.h`, `gl_2d.h`, and `repl_flatten.h` for `FlatProgramView` /
  `GLCmd`. No direct includes of `repl_state.h`, `repl_executor.h`,
  `repl_core.h`, `repl_replay.h`, or `ui_panels.h`.

`sample.h` is accepted as a transitional common header because it still owns
`GLCmd`, `CmdType`, `SceneLight`, grid enums, replay enums, and shared UI
constants today. Long term, those common pieces should be split out of
`sample.h` so the scene layer is structurally clean rather than merely
include-clean by convention.

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

## Entry Point Design

The scene layer gets a true push-model entry point:

```c
/* Pure scene entry point.  Does not read repl_state, does not call ui_panels,
 * and does not include repl_executor/repl_replay/repl_core. */
void scene_render_3d_scene_with_config(const SceneRenderConfig *config);
```

The old API becomes a REPL-owned compatibility wrapper:

```c
void scene_render_3d_scene(void) {
    SceneRenderConfig config;
    scene_render_config_build(&config);
    scene_render_3d_scene_with_config(&config);
}
```

That wrapper may live in `repl_core.c` or a small `repl_scene_adapter.c` rather
than in pure `scene_render.c`. The important rule is that standalone scene code
only needs `scene_render_3d_scene_with_config()`.

---

## Execute Callback Design

The callback typedef lives in `scene_render_types.h`. It uses only
`FlatProgramView` from `repl_flatten.h` — no `repl_executor.h` types:

```c
typedef struct SceneExecuteRequest {
    float alpha_scale;          /* 1.0 normal pass; 0.0-1.0 fade batches */
    int   skip_geom_before_pc;  /* prefix state walk; skip geometry before pc */
    int   flat_cmd_count;       /* number of commands to execute */
    FlatProgramView program;    /* flat command buffer + local vars */
} SceneExecuteRequest;

/* Called by scene_render.c to emit user geometry. May be NULL, in which case
 * user geometry is silently skipped while scene helpers still render. */
typedef void (*SceneExecuteProgramFn)(const SceneExecuteRequest *request,
                                      void *user_data);
```

`SceneRenderConfig` gains two execution fields:

```c
SceneExecuteProgramFn execute_fn;        /* NULL = skip user geometry */
void                 *execute_user_data;
```

There is intentionally **no reset callback**. The execution request carries the
fade alpha and prefix-skip state explicitly. The REPL adapter should leave no
persistent executor state behind.

Preferred REPL adapter shape:

```c
static void scene_execute_adapter(const SceneExecuteRequest *req, void *ud) {
    (void)ud;
    ReplExecutionOptions opts = {
        .flat_cmd_count = req->flat_cmd_count,
        .program = req->program,
        .alpha_scale = req->alpha_scale,
        .skip_geom_before_pc = req->skip_geom_before_pc,
    };
    repl_execute_program(&opts);
}
```

This implies a small executor API cleanup: move `alpha_scale` and
`skip_geom_before_pc` into `ReplExecutionOptions` and retire the temporal
`repl_execute_set_fade_context()` reset pattern. If that cleanup cannot happen
in the first pass, the adapter may temporarily keep the old set/reset behavior,
but the architecture target is stateless request-driven execution.

---

## New Fields Added to SceneRenderConfig

Grouped by domain. All filled by `scene_render_config_build()` in REPL-owned
code.

```c
/* -- Execute callback --------------------------------------------------- */
SceneExecuteProgramFn execute_fn;          /* NULL = no user geometry */
void                 *execute_user_data;

/* -- Flat program snapshot --------------------------------------------- */
FlatProgramView flat_program;              /* repl_state_flat_program_view() */

/* -- Animation ---------------------------------------------------------- */
float anim_time;                           /* *repl_state_variables()->anim_time */

/* -- Viewport ----------------------------------------------------------- */
int viewport_w;
int viewport_h;

/* -- Lighting ----------------------------------------------------------- */
int        user_lighting_enabled;          /* repl_state_flat_program_user_lighting_enabled() */
SceneLight lights[MAX_LIGHTS];             /* per-frame copy */
int        show_light_indicators;

/* -- Backdrop ----------------------------------------------------------- */
int backdrop_mode;

/* -- Outline overlay ---------------------------------------------------- */
int show_vertex_outlines;

/* -- Replay HUD / layout ----------------------------------------------- */
int   code_panel_layout;
int   replay_pc;
int   replay_total_cmds;
int   replay_state_val;     /* REPLAY_PLAYING / REPLAY_PAUSED / REPLAY_DONE */
float replay_speed;
int   replay_expand_args;

/* -- Replay fade batches ----------------------------------------------- */
typedef struct SceneReplayFadeBatch {
    float alpha_scale;
    int   skip_geom_before_pc;
    int   flat_cmd_count;
} SceneReplayFadeBatch;

SceneReplayFadeBatch replay_fade_batches[REPLAY_FADE_BATCH_MAX];
int                  replay_fade_batch_count;
int                  replay_base_limit;

/* -- Grid tables -------------------------------------------------------- */
float grid_major_steps[GRID_MAJOR_COUNT];
float grid_extents[GRID_EXTENT_COUNT];

/* -- Cursor block snapshot --------------------------------------------- */
int          cursor_block_begin_idx;  /* -1 = no active block */
int          cursor_block_end_idx;
int          cursor_block_source_line;
int          edit_line_idx;
unsigned int cursor_func_scope_mask;
int          cursor_call_src_cmd_idx; /* -1 = cursor not on a CMD_CALL */
```

`FrameRenderContext` is **unchanged** — the new fields live in the embedded
`SceneRenderConfig` and are reached via `frame_ctx->config.*`.

Note: `REPLAY_FADE_BATCH_MAX` is also transitional. If replay constants move out
of `sample.h`, either define a scene-side maximum or store an array view plus a
count in the config.

---

## Config Build: scene_render_config_build()

A REPL-owned function declared in a REPL-facing header and implemented in
`repl_core.c` or `repl_scene_adapter.c`:

```c
/* Populate *config from the current REPL runtime state for one frame.
 * Call once at frame start before scene_render_3d_scene_with_config(). */
void scene_render_config_build(SceneRenderConfig *config);
```

Key responsibilities:

- Reads `repl_state_camera()`, `repl_state_presentation()`,
  `repl_state_render()`, `repl_state_replay()`, `repl_state_viewport()`,
  `repl_state_variables()`.
- Calls `ui_panels_scene_rect()` to fill `scene_x/y/w/h`. This call moves out
  of `scene_render.c`.
- Calls `refresh_current_block_highlight()` or a wrapper before reading
  `current_block_begin/end_idx` so the cursor block is current.
- Computes replay fade batch alpha values and skip limits, then copies them
  into `config->replay_fade_batches[]`. `scene_render.c` should not call
  `repl_replay_*` helpers directly.
- Deep-copies `lights[MAX_LIGHTS]` and the two grid tables by value.
- Sets `execute_fn = scene_execute_adapter` and `execute_user_data = NULL`.

The pure scene entry point receives the already-built config and does no REPL
state sampling.

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

Do not duplicate the old `repl_flat_cmd_matches_cursor()` semantics inline in
`scene_overlays.c`. Move the logic to a pure helper that accepts a pushed
snapshot and a flat command:

```c
typedef struct SceneCursorMatchSnapshot {
    int          edit_line_idx;
    int          cursor_block_begin_idx;
    int          cursor_block_end_idx;
    unsigned int cursor_func_scope_mask;
    int          cursor_call_src_cmd_idx;
} SceneCursorMatchSnapshot;

int scene_flat_cmd_matches_cursor(const SceneCursorMatchSnapshot *cursor,
                                  const GLCmd *cmd,
                                  int flat_idx);
```

`scene_overlays.c` then calls the pure helper using fields already present in
`SceneRenderConfig`. The side effect of refreshing the current block stays in
`scene_render_config_build()`.

This avoids creating two independent implementations of the same highlight
rules.

---

## Replay Fade Pass

Replay orchestration belongs to REPL-owned config build code; replay fade
rendering belongs to the scene layer.

- `scene_render_config_build()` computes the fade batch list, alpha values,
  prefix skip limits, and `replay_base_limit`.
- `scene_render_3d_scene_with_config()` performs the normal fill pass up to
  `config->replay_base_limit` when fades are active.
- It then loops over `config->replay_fade_batches[]` and calls `execute_fn` for
  each batch.

Sketch:

```c
if (config->execute_fn) {
    SceneExecuteRequest fill = {
        .alpha_scale = 1.0f,
        .skip_geom_before_pc = 0,
        .flat_cmd_count = config->replay_fade_batch_count > 0
                        ? config->replay_base_limit
                        : config->flat_program.cmd_count,
        .program = config->flat_program,
    };
    config->execute_fn(&fill, config->execute_user_data);
}

for (int i = 0; config->execute_fn && i < config->replay_fade_batch_count; i++) {
    const SceneReplayFadeBatch *batch = &config->replay_fade_batches[i];
    SceneExecuteRequest fade = {
        .alpha_scale = batch->alpha_scale,
        .skip_geom_before_pc = batch->skip_geom_before_pc,
        .flat_cmd_count = batch->flat_cmd_count,
        .program = config->flat_program,
    };
    config->execute_fn(&fade, config->execute_user_data);
}
```

The scene layer still owns per-pass GL isolation such as `glPushAttrib()` and
per-batch `glPushMatrix()` / `glPopMatrix()`. Matrix isolation is correctness,
not merely cleanup: executor transforms are incremental and must not leak from
one fade batch into the next.

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
| `repl_core.c` / `repl_scene_adapter.c` | — | `scene_render_types.h` |

New files: `scene_transform_utils.h` and optionally `scene_cursor_match.h` /
`.c` for the pure cursor-match helper.

---

## Makefile Boundary Checks

**Remove the existing `ui_panels.h` exception:**

```makefile
# Old (check-layer-coupling):
grep -nE '#include\s+"ui_' scene_*.c scene_*.h | grep -vE 'scene_render\.c:.*ui_panels\.h'

# New — no exception needed:
grep -nE '#include\s+"ui_' scene_*.c scene_*.h
```

**Add two new guards wired into `make test`:**

```makefile
check-scene-boundaries: ## scene_*.c must not include REPL/UI owner headers
	@! grep -nE '#include\s+"repl_state\.h"' scene_*.c scene_*.h \
		|| (echo "ERROR: scene_*.c/h must not include repl_state.h" && exit 1)
	@! grep -nE '#include\s+"repl_(executor|core|replay)\.h"' scene_*.c scene_*.h \
		|| (echo "ERROR: scene_*.c/h must not include repl_executor/core/replay.h" && exit 1)
	@! grep -nE '#include\s+"ui_' scene_*.c scene_*.h \
		|| (echo "ERROR: scene_*.c/h must not include ui_*.h" && exit 1)
	@echo "scene/repl boundaries OK"
```

---

## Staged Commits

Each commit must compile and pass `make test-stubs TEST_JOBS=4`.

| # | Commit | Files Touched |
|---|--------|---------------|
| 1 | `refactor: add config-driven scene render entry point` | `scene_render.c`, `scene_render.h` |
| 2 | `feat: add SceneExecuteRequest callback model to SceneRenderConfig` | `scene_render_types.h` |
| 3 | `refactor: make executor options carry fade context` | `repl_executor.h`, `repl_executor.c`, callers |
| 4 | `refactor: add scene_render_config_build() in REPL adapter` | `repl_core.c` or `repl_scene_adapter.c`, headers |
| 5 | `refactor: remove repl_state.h from scene_render.c` | `scene_render.c`, adapter file |
| 6 | `refactor: remove repl_state.h from scene_grid.c and scene_axes.c` | `scene_grid.c`, `scene_axes.c` |
| 7 | `refactor: remove repl_state.h from scene_backdrop.c and scene_lights.c` | `scene_backdrop.c`, `scene_lights.c`, headers |
| 8 | `refactor: add scene_transform_utils.h for scene transform walking` | `scene_transform_utils.h`, `scene_transform_guides.c` |
| 9 | `refactor: move cursor highlight matching to pure scene helper` | `scene_overlays.c`, `scene_cursor_match.*` |
| 10 | `refactor: remove remaining repl/ui includes from scene_render.c` | `scene_render.c` |
| 11 | `refactor: tighten Makefile boundary checks for scene_* layer` | `Makefile` |

Commits 1 and 2 establish the API shape before the risky ownership moves.
Commit 3 removes the need for an executor reset callback. Commit 4 contains the
REPL pull-to-push adapter. Commit 9 carries the cursor-match semantic risk and
should include focused tests.

---

## Verification

- `make test-stubs TEST_JOBS=4` after every commit.
- `make test` after the Makefile boundary checks land.
- Manual smoke test after Commit 10: grid, axes, backdrop, lights, overlays,
  replay, and fade pass all exercise changed code paths.
- Required GL-stubs tests:
  - `scene_render_3d_scene_with_config()` with `execute_fn = NULL` does not
    crash.
  - `execute_fn = NULL` suppresses user flat-program execution.
  - Grid/axes/backdrop still issue GL-stub drawing calls with no executor.
  - Replay fade batches do not accumulate transforms across batches.
  - Vertex replay inside `glBegin`/`glEnd` still emits the incremental vertex.
  - Tess replay inside a GLU tess contour still emits the incremental tess
    vertex.
  - Cursor overlay matching returns the same results before and after the pure
    helper extraction.

---

## Required Design Constraints / Notes

- **`scene_lights_setup()` mutation**: currently writes `lights[i].enabled = 0`
  as a per-frame reset. After this refactor, scene code must not mutate
  persistent REPL light state. Either mutate only the per-frame copied
  `SceneLight lights[MAX_LIGHTS]` inside `SceneRenderConfig`, or reset the live
  REPL light state before building config via a helper such as
  `repl_lights_reset_all_enabled()`.
- **`repl_state_render_derived()` write-back**: `scene_prepare_focus_vertex()`
  currently writes the focus vertex back to global derived state. After the
  config-driven entry point lands, this write-back is no longer needed — the
  result stays local to `FrameRenderContext.focus`. The global
  `ReplRenderDerivedState` write is eliminated.
- **Matrix isolation around fade batches**: keep per-batch matrix isolation even
  if attribute push/pop is reduced. `repl_execute_program()` applies
  `glTranslatef` / `glRotatef` / `glScalef` incrementally, so later fade batches
  must not inherit the previous batch's final modelview matrix.
- **`sample.h` dependency**: accepted for this phase, but do not expand it. New
  common scene-facing types should go into focused headers rather than adding
  more REPL declarations to `sample.h`.

---

## Implementation Notes: Deviations From Plan

The plan above was written before implementation. This section documents
pragmatic adjustments made during execution.

### Execute Callback Signature

**Plan:** Proposed a `SceneExecuteRequest` struct wrapper:
```c
typedef struct SceneExecuteRequest {
    float alpha_scale;
    int   skip_geom_before_pc;
    int   flat_cmd_count;
    FlatProgramView program;
} SceneExecuteRequest;

typedef void (*SceneExecuteProgramFn)(const SceneExecuteRequest *request,
                                      void *user_data);
```

**Implementation:** Flattened to direct function parameters:
```c
typedef void (*SceneExecuteProgramFn)(float alpha_scale,
                                     int skip_geom_before_pc,
                                     int flat_cmd_count,
                                     FlatProgramView program,
                                     void *user_data);
```

**Rationale:** Simpler to use at call sites (no struct allocation), fewer
indirections, and the overhead of a struct wrapper added no clarity given the
small parameter set. Direct parameters map clearly to the semantic meaning at
the call site.

### Reset Callback Addition

**Plan:** Explicitly stated "intentionally no reset callback." Execution context
was intended to be stateless, with all state carried in the request parameters.

**Implementation:** Added `execute_reset_fn` callback:
```c
void (*execute_reset_fn)(void *user_data);
```

**Rationale:** In practice, the executor maintains some per-frame state (matrix
depth tracking, attribute state) that must be cleaned up between replay fade
batches. Without a reset hook, the first batch's transforms would leak into the
second batch's GL state. The reset callback provides a clean separation point
between fade passes. Called after all geometry in a batch is emitted, before the
next batch begins.

### Replay Fade Batch Representation

**Plan:** Pre-computed fade batch snapshots stored in config:
```c
typedef struct SceneReplayFadeBatch {
    float alpha_scale;
    int   skip_geom_before_pc;
    int   flat_cmd_count;
} SceneReplayFadeBatch;

SceneReplayFadeBatch replay_fade_batches[REPLAY_FADE_BATCH_MAX];
int                  replay_fade_batch_count;
```

**Implementation:** Config carries replay state variables instead; fade batches
computed dynamically during execution:
```c
int   replay_pc;
int   replay_total_cmds;
int   replay_state_val;     /* REPLAY_PLAYING / REPLAY_PAUSED / REPLAY_DONE */
float replay_speed;
int   replay_expand_args;
```

**Rationale:** Fade batch computation is tied to replay's internal state machine
(speed, pause state, total command count). Pre-snaphotting the batches would
require duplicating that logic in `scene_render_config_build()`. Instead, the
config carries just the replay state variables, and the executor callback logic
queries `repl_replay_*` helpers to determine which batches are active and their
alpha values. This keeps the replay state machine in one place (`repl_replay.c`)
and avoids storing stale batch snapshots that might diverge from the replay
state during frame-to-frame changes.

### Cursor Match Helper

**Plan:** Proposed extracting cursor matching to a pure helper function in a new
file:
```c
typedef struct SceneCursorMatchSnapshot {
    int          edit_line_idx;
    int          cursor_block_begin_idx;
    int          cursor_block_end_idx;
    unsigned int cursor_func_scope_mask;
    int          cursor_call_src_cmd_idx;
} SceneCursorMatchSnapshot;

int scene_flat_cmd_matches_cursor(const SceneCursorMatchSnapshot *cursor,
                                  const GLCmd *cmd,
                                  int flat_idx);
```

**Implementation:** Inlined as a static helper directly in `scene_overlays.c`:
```c
static int overlay_flat_cmd_matches_cursor(int begin_idx, int is_tess,
                                           const SceneRenderConfig *cfg);
```

**Rationale:** The cursor matching logic is only used in `scene_overlays.c` for
polygon outline highlighting. Creating a separate module (`scene_cursor_match.c`)
added no benefit since the function is tightly coupled to the overlay rendering
context. The static inline version in `scene_overlays.c` is clearer and avoids
unnecessary file proliferation. The logic still operates on a pushed snapshot
(via `SceneRenderConfig` fields), preserving the decoupling intent.

### Entry Point Strategy

**Plan:** Proposed a new pure entry point `scene_render_3d_scene_with_config()`
with the existing `scene_render_3d_scene()` as a REPL-owned wrapper.

**Implementation:** Kept `scene_render_3d_scene()` as the sole public entry point;
config building happens in `repl_core.c` via `scene_render_config_build()` called
from `display_func()`.

**Rationale:** The plan's dual-entry-point design was correct architecturally
but added naming complexity. In practice, `scene_render.c` doesn't need to expose
a separate config-driven entry point because the REPL always controls the
rendering context via `display_func()`. The decoupling is achieved by pushing
state into `SceneRenderConfig` and passing `FrameRenderContext` through all
scene helper functions. Users of the scene layer inside the REPL can already see
the push-model design through the `FrameRenderContext` parameter; adding a
second entry point would just be redundant ceremony.

### Commit Strategy Consolidation

**Plan:** 11 staged commits, including a separate executor refactor step
(Step 3: "make executor options carry fade context").

**Implementation:** 9 commits (Steps 1–9) plus a final verification, skipping the
separate executor refactor. The config-building and scene-module decoupling
stages were consolidated rather than split into separate steps.

**Rationale:** The executor refactor (moving `alpha_scale` and
`skip_geom_before_pc` into `ReplExecutionOptions`) was intended to remove the
need for a reset callback. In practice, we added the reset callback anyway
(see above), so the executor refactor became unnecessary. The direct callback
parameters work well. Consolidating the commits reduced the number of
intermediate states and made the refactor easier to review as a cohesive whole.

### Test Coverage

**Plan:** Mentioned required GL-stubs verification tests but did not propose a
dedicated test module for scene_* functions.

**Implementation:** Added `test_scene_render.c`, a comprehensive standalone test
suite that validates all scene modules operate independently of `repl_state.h`.
45 tests cover config initialization, renderer entry points, data propagation,
viewport handling, and render mode toggles.

**Rationale:** The refactor's core value is scene module independence. Having
explicit tests for the push-model decoupling ensures the contract is maintained
as the code evolves. The tests use GL stubs and require no REPL initialization,
proving the modules truly are independent components.
