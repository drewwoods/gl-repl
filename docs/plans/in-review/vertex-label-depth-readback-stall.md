# Vertex-Label Depth Readback: End-of-Frame Lagged Capture

## Status - IN REVIEW (drafted 2026-08-08)

Two staged versions, in the order they should be considered:

1. **Lagged capture after the frame's existing `glFinish`** - the proposed fix.
   Restores Frame Work to baseline exactly, needs no new GL feature.
2. **PBO round-trip**, *if* pixel buffer objects are supported - drafted here
   because it is the textbook answer, but **measured to buy nothing on the
   driver that has the problem**, and recommended against on current evidence.

## Context

Turning vertex numbering on costs ~14 ms/frame on NVIDIA (RTX 5050, driver
595.84, Linux). The profile panel's `vertex nums` row goes from absent to
14.3 ms, and Frame Work goes 2.2 -> 16.3 ms against a 16.67 ms budget - every
bit of the headroom, at a 1200x440 scene viewport.

The cost is one call: the full-viewport
`glReadPixels(GL_DEPTH_COMPONENT, GL_FLOAT)` in
[`vertex_label_depth_snapshot()`](../../../src/subsystems/edit_overlays/edit_overlays.c)
that feeds the label occlusion cull. It is issued mid-frame, and a synchronous
read blocks until the driver's entire outstanding queue retires. On a driver
that renders ahead - which NVIDIA does by default - that queue is deep and
vsync-throttled, so the read absorbs most of a refresh interval.

**The pixels are not the problem.** Reproduce with `make bench-vertex-labels`;
the same read costs ~0.7 ms (no MSAA) / ~2.5 ms (4x MSAA) once the queue is
drained. Two driver knobs remove the entire penalty without touching the
readback, which is what identifies the mechanism:

| control | in-app readback |
|---|---|
| default | 12.8 ms |
| `__GL_SYNC_TO_VBLANK=0` | 1.12 ms |
| `__GL_MaxFramesAllowed=1` | 1.08 ms |

### What the profiler shows, and why it misleads

With labels **off**, Frame Time 12.4 ms = Frame Work 2.2 + Present 10.2. With
labels **on**, Frame Time 16.6 = Frame Work 16.3 + Present 0.3. The wait did
not appear - it **moved**, out of the end-of-frame drain and into the middle of
Frame Work, where it is charged to whichever section holds the frame's first
synchronous GL call.

That relocation is the whole bug. The app already drains at end of frame
([`gl_repl.c:131`](../../../gl_repl.c), `glFinish()` before `glutSwapBuffers()`),
*outside* `glr_frame_work_end()`. Waiting there is free - it is time the frame
spends waiting for the refresh anyway. Waiting mid-frame is not: nothing can
overlap it, and the headroom is gone.

### Three fixes that do not work

Measured, each ruling out an otherwise-reasonable approach:

- **Read a different buffer.** `GL_FRONT` - already presented, nothing pending
  against it - blocks 16.475 ms, identical to `GL_BACK`'s 16.474 ms. A
  synchronous read is a whole-pipeline sync point regardless of target. (And
  `glReadBuffer` selects a *color* buffer, so it never applied to a depth read;
  the pass has no front/back choice to make.)
- **Move the read within the frame, still synchronous.** Reading post-swap, so
  it takes the *previous* frame's depth, still costs 16.466 ms. Staleness alone
  buys nothing.
- **Read fewer pixels.** The drained floor is 0.7-2.5 ms; the stall is 16.5 ms.
  Even a perfect bounding-box read optimizes the wrong term.

## Version 1 - lagged capture after the frame's existing `glFinish`

### The idea

Capture the depth **at the end of the frame, immediately after the `glFinish`
the app already performs**, and let the next frame's label pass consume it. The
queue is drained at that point and the drain is already paid for and already
attributed to Present, so the read costs only its transfer, in a span that has
slack.

Measured on the same harness, modelling gl_repl.c's real frame shape
(`work ms` is the app's Frame Work span - the headroom):

| case | read ms | **work ms** | frame ms |
|---|---|---|---|
| no readback (baseline) | 0.000 | **0.148** | 16.665 |
| depth mid-frame (today) | 16.411 | **16.563** | 16.664 |
| **depth after frame's `glFinish` (lagged)** | 2.463 | **0.151** | 16.661 |
| depth into PBO, mapped next frame | 0.001 | **16.613** | 16.664 |

(The PBO row measures only the *map*. Its `glReadPixels` kick blocks a further
16.47 ms inside Frame Work - see version 2.)

Frame Work returns to baseline **exactly**. The 2.46 ms transfer lands in the
span the app already spends waiting for the refresh (Present was 10.2 ms with
labels off, so there is ample slack).

### Design

**Ownership.** Capture is frame-lifecycle policy, so it belongs to the
controller, not to the subsystem: `src/app/` owns when the frame drains, and
`src/subsystems/edit_overlays/` stays a consumer. The overlay must not reach
back into frame timing to trigger its own read.

- New controller-owned snapshot in `src/app/glr_ctrl.c`: a persistent
  `float *` plus its viewport rect and a validity flag.
- New entry point called from [`gl_repl.c`](../../../gl_repl.c) between the
  existing `glFinish()` and `glutSwapBuffers()` - the one place where the
  pipeline is known drained and the frame's depth is still intact.
- `OverlaySnapshotPack` grows a snapshot view (pointer + `w`/`h`/`x`/`y` +
  valid) replacing the current `int depth_readback_supported` capability flag;
  `edit_overlays_render_vertex_numbers()` takes the same view instead of the
  flag. The pass stops calling `glReadPixels` at all - it becomes a pure
  consumer, which also makes it testable without a GL trace.

**Gating - the lazy read is the thing being traded away.** Today the read is
taken lazily, at the first vertex that actually reaches the cull, so a label
mode that is on but has nothing in scope pays nothing. That is a deliberate
existing optimization with a test pinning it. An end-of-frame capture cannot
know what the *next* frame will label, so the gate has to become "labels were
on this frame". Proposed: capture when `vertex_label_mode != OFF` **and** the
previous frame's pass actually reached the cull (a one-bit feedback the pass
already knows, exposed as a getter). That preserves the empty-scope case at the
cost of one frame of latency when labels first come into scope - during which
the existing "no snapshot -> labels stay visible" fallback applies.

**Profiling.** Per `docs/ARCHITECTURE.md`, per-frame work added to the host
callback needs its own `ProfSection` or it shows up only as unattributed
remainder. Add `PROF_DEPTH_SNAPSHOT` to [`prof_sections.h`](../../../prof_sections.h)
and bracket the capture, so the ~2.5 ms is visible and attributed rather than
silently inflating Present.

**Lifecycle.**
- Allocate lazily at first capture; reallocate when the scene viewport changes.
- On a size change, discard rather than scale - a stale-size buffer must read
  as invalid, hitting the existing labels-stay-visible fallback for one frame.
- Free on shutdown; `glr_ctrl_reset_all` need not clear it (it self-refreshes).
- First frame after enabling labels has no snapshot: labels all visible for one
  frame, which is exactly today's unsupported-context behavior.

**Staleness semantics.** The occlusion cull becomes one frame late. Under camera
motion a label can persist one frame after its vertex goes behind geometry, or
appear one frame late. At 60 fps this is not perceptible, and the failure mode
is symmetric with the existing sticky-placement easing, which already lags. This
must be stated in the pass's header comment - the cull is no longer "this
frame's depth", and a future reader will otherwise treat that as a bug.

**Capability probe.** `g_depth_readback_supported` keeps its current meaning and
now gates the *capture* rather than the pass. Web/gl4es stays hard-disabled and
the pass keeps drawing uncelled labels, unchanged.

### Changes

| File | Change |
|---|---|
| [`prof_sections.h`](../../../prof_sections.h) | New `PROF_DEPTH_SNAPSHOT` section |
| [`src/app/glr_prof.c`](../../../src/app/glr_prof.c) | Its `ProfSectionInfo` label/depth |
| [`src/app/glr_ctrl.c`](../../../src/app/glr_ctrl.c) | Snapshot storage, capture entry point, realloc/invalidate on viewport change, publish the view into `OverlaySnapshotPack` |
| [`src/app/glr_ctrl.h`](../../../src/app/glr_ctrl.h) | Declare the capture entry point |
| [`gl_repl.c`](../../../gl_repl.c) | Call it between `glFinish()` and `glutSwapBuffers()` |
| [`src/subsystems/edit_overlays/edit_overlays.h`](../../../src/subsystems/edit_overlays/edit_overlays.h) | Snapshot view in the pack; `edit_overlays_render_vertex_numbers()` signature |
| [`src/subsystems/edit_overlays/edit_overlays.c`](../../../src/subsystems/edit_overlays/edit_overlays.c) | Delete `vertex_label_depth_snapshot()`; consume the passed-in buffer; expose the "reached the cull" bit; rewrite the header comment on staleness |

### Tests

[`tests/test_edit_overlays.c`](../../../tests/test_edit_overlays.c) pins the
current contract in three places and all three change meaning - they assert
`glReadPixels` **call counts from the label pass**, which will become zero
because the pass no longer reads:

- "empty label scope skips the depth readback" (expects 0) - becomes an
  assertion about the *capture gate*, not the pass.
- "supported label pass snapshots scene depth" (expects 1) - the pass no longer
  reads; move to the controller-side capture.
- "unsupported label pass skips depth readback" (expects 0) - same relocation.

New coverage worth adding, all of which the refactor makes easy because the
pass becomes pure over a buffer:

- Occlusion culls against a supplied buffer (no GL trace needed).
- A `valid = 0` snapshot leaves every label visible (the fallback).
- A snapshot whose dimensions disagree with the current viewport is refused
  rather than indexed.
- Capture is skipped when labels are off.

`make test-web` must still pass: the web build never captures, so the pass sees
a permanently invalid snapshot - the same path as today's unsupported context.

### Verification

- `make bench-vertex-labels` - the lagged row's `work ms` must sit at baseline.
- `GLR_PROF_DUMP=400 ./gl-repl --no-audio <scene>` with labels on: `vertex nums`
  should fall to roughly the layout+glyph cost (~1.3 ms), Frame Work should
  return to ~2-3 ms, and `Present` should reabsorb the wait.
- A/B the same scene with `__GL_MaxFramesAllowed=1`; after the fix the two runs
  should agree, because there is no longer a queue-depth-sensitive stall.
- Confirm on macOS and on Mesa that nothing regresses - both drain more shallowly
  and never had the symptom.

## Version 2 - PBO round-trip, with or without a fence (**does not work here**)

### The idea

`glReadPixels` into a pixel buffer object returns without a CPU sync; map the
buffer a frame later, when the GPU has finished with it. Better still, insert a
`glFenceSync` after the read and poll it non-blocking
(`glClientWaitSync(..., GL_SYNC_FLUSH_COMMANDS_BIT, 0)`) so a buffer the GPU has
not finished with is skipped rather than waited on - no call ever blocks. This
is the textbook answer to a readback stall and is why it was drafted.

### Why it does not work

**The block is inside `glReadPixels` at issue time, before there is a fence to
insert.** Timing the issuing call (the "kick") separately from taking delivery
(the "consume") is what makes this visible - measuring only the consume is
exactly what makes a PBO look like it solved something:

| case | kick | consume | Frame Work | mapped/missed |
|---|---|---|---|---|
| depth into PBO, map next frame | **16.47** | 0.001 | 16.62 | 80/0 |
| depth into PBO **+ fence, polled** | **16.46** | 0.001 | 16.62 | 80/0 |
| control: **1x1** depth into PBO | **16.33** | 0.000 | 16.48 | 80/0 |
| control: `glFlush` at the same point | 0.002 | 0.000 | 0.15 | - |
| control: full **RGBA** into PBO | **0.008** | 0.001 | **0.16** | 80/0 |

The fence works exactly as designed - 80/80 mapped, 0 missed, consume 0.001 ms -
and it is irrelevant, because the consume was already free. Four controls pin
down what the constraint actually is:

- **Not the queue-full throttle landing on that call.** A `glFlush` in the same
  position costs 0.002 ms, so the position is fine for a non-readback call.
- **Not the transfer, and not the size.** A **1x1** depth read into a PBO -
  one pixel, nothing to move - still blocks 16.33 ms.
- **Not the default framebuffer.** Rendering the scene into an FBO and reading
  its `DEPTH_COMPONENT24` **texture** instead still blocks (16.45 ms), with or
  without a fence.
- **Not PBOs, which work fine on this driver.** The identical full-size read in
  **color** goes fully async: kick 0.008 ms, Frame Work 0.16 ms against a
  0.15 ms baseline.

So the synchronous path is specific to reading `GL_DEPTH_COMPONENT`, at any
size, from any framebuffer, into a PBO or not, fenced or not. Nothing on the
client side can defer it.

### Consequence for the design

Version 2 is **not an alternative to version 1 and not a follow-up to it**. Even
if a driver were found where the depth kick goes async, the read still wants to
be issued at version 1's drained end-of-frame point - at which the queue is
already empty and the asynchrony has nothing left to hide. Version 1 subsumes
it, at zero GL-version cost.

Worth revisiting only if a driver is found where the **kick** is genuinely async
for depth (measure the kick, never just the map). Requirements if so: GL 2.1 /
`GL_ARB_pixel_buffer_object` and `GL_ARB_sync` probed the way
`GL_ARB_timer_query` already is in `glr_ctrl_init_gl`; a ring of buffers plus
fences; a `glMapBuffer`-returns-NULL fallback; a policy for a poll that is not
yet signalled (reuse the previous snapshot, or skip the cull for a frame); and
web/gl4es excluded as it is today.

All of the above is reproducible with `make bench-vertex-labels`, which carries
every one of these rows as permanent controls.

## Open questions for review

1. **Capture gate.** Is "labels on **and** last frame's pass reached the cull"
   the right condition, or is the simpler "labels on" acceptable - accepting a
   ~2.5 ms Present-side read on frames whose scope is empty?
2. **Staleness under replay.** Replay clamps the flat program and re-renders;
   confirm a one-frame-old depth buffer is acceptable there, or gate the cull
   off during playback as the accum path already does for its own reasons.
3. **Capture-mode determinism.** `GLR_TICK_PER_FRAME` / `FREEGLUT_CAPTURE_FRAMES`
   docs-media runs are frame-deterministic; a one-frame lag shifts label
   occlusion by one frame in generated media. Check whether any `docs/images/`
   asset shows occluded labels before regenerating.
4. **`glFinish` dependency.** Version 1 assumes `gl_repl.c:131` keeps its
   `glFinish`. If that is ever removed as an optimization, the capture point
   silently becomes a stall again. Worth a comment at both sites, or an
   explicit `glFinish` owned by the capture itself.
