# Vertex-Label Depth Readback: End-of-Frame Lagged Capture

## Status - IMPLEMENTED on `feat/vertex-label-lagged-depth-capture` (2026-08-09)

Version 1 is built and measured in-app: Frame Work with labels on went
16.3 -> 3.4 ms on the atoll stress scene, against a 2.1 ms labels-off baseline.
`vertex nums` now reads 1.25 ms (its layout + glyph cost) and the readback is
attributed separately as `depth snapshot` at ~1.5-2.1 ms, outside Frame Work.
Version 2 remains rejected. Kept in `in-review/` rather than moved to `done/`
because one review item is still open by choice - see **Viewport validation**.

Drafted 2026-08-08; review folded in 2026-08-09.

Review raised four issues. Three are folded into the design below - stale-
snapshot invalidation and failed-read/allocation behavior (both now specified
in **Lifecycle**), and controller-side lifecycle/ordering coverage (now in
**Tests**). The fourth, validating the snapshot's origin as well as its size,
is **declined**; the trade and the reachable case that argues against declining
it are recorded under **Viewport validation** so the decision is not silently
inherited.

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
  `float *` plus its viewport rect and a validity flag. `valid` means "these
  pixels describe the scene as of the end of last frame" - it is set only by a
  capture that completed, and cleared by everything in Lifecycle below.
- New entry point called from [`gl_repl.c`](../../../gl_repl.c) between the
  existing `glFinish()` and `glutSwapBuffers()` - the one place where the
  pipeline is known drained and the frame's depth is still intact.
- `OverlaySnapshotPack` grows a snapshot view (pointer + `w`/`h`/`x`/`y` +
  valid) replacing the current `int depth_readback_supported` capability flag;
  `edit_overlays_render_vertex_numbers()` takes the same view instead of the
  flag. The pass stops calling `glReadPixels` at all - it becomes a pure
  consumer, which also makes it testable without a GL trace.

**Gating - and the flag must be cleared before every early return.** The pass
has early exits (labels off, empty program) that run before the walk. Leaving
the feedback flag alone on those paths makes it sticky: an empty document with
labels enabled inherits the previous frame's "yes" and the controller keeps
capturing a depth buffer every frame for labels that cannot exist.

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

**Lifecycle.** A stale buffer that still reads as `valid` is the main hazard
this design introduces, so invalidation is specified rather than left to
"it self-refreshes":

- Allocate lazily at first capture; reallocate when the scene viewport changes.
- On a size change, discard rather than scale - a stale-size buffer must read
  as invalid, hitting the labels-stay-visible fallback for one frame.
- **Invalidate (clear `valid`, keep the allocation) on every event that can make
  the retained depth describe a different scene:** labels cycling to `OFF`,
  reshape, and the depth capability going false. Document replacement -
  example, user scene, workspace, tutorial teardown, F12 cycling,
  `glr_ctrl_reset_all` - is handled by comparing `editor_undo_generation()`
  rather than by invalidating at each load site. Enumerating those sites is
  what leaked the first time (only `reset_all` was covered), and the next load
  path added would have been missed too; every one of them is already required
  to call `editor_undo_note_wholesale_replacement()`, which bumps that counter.
  The comparison happens on read, not on write, because the replacement lands
  between the capture and the consumption a frame later. Without this, disabling labels and re-enabling them
  later publishes the *old* snapshot to the first pass - which both contradicts
  the "first frame after enabling has no snapshot" rule below and culls against
  a stale scene and camera. The gate deciding whether to *capture* is not the
  same question as whether the retained buffer is still *usable*; conflating
  them is what produces the stale-publish bug.
- **A failed capture must clear `valid`, never leave the previous buffer
  published as current.** That covers a failed `malloc`/`realloc` and a
  `glReadPixels` that raises an error. The startup capability probe reads a
  single pixel and does not promise every later full-viewport read succeeds, so
  the read is checked per capture, not once at init.
- Free on shutdown. Note that [`glr_shutdown()`](../../../src/app/glr_ctrl.c)
  currently frees only audio, hidden-line and executor resources - it has no
  controller-owned-storage free to extend, so this adds one.
- First frame after enabling labels has no snapshot: labels all visible for one
  frame, which is exactly today's unsupported-context behavior.

**Capture rect - the scene rect, asked for explicitly.** The capture runs after
the code panel, the HUDs and the compositor have each set the viewport to the
whole window for their own 2D passes, so ambient `GL_VIEWPORT` there is the
window, not the scene. Reading it produced a window-sized buffer that the pass
rejected next frame for disagreeing with its projection rect - a larger readback
and **no occlusion culling at all**, silently. Nothing about that is visible in
a profile (the stall is still gone, the labels still draw) which is why it
survived a round of review; `ui_layout_scene_rect()` is the same source
`render3d_x/y/w/h` comes from, so capture rect and projection rect now agree by
construction. Reading the smaller rect is also less data: ~2.1 ms -> ~1.0 ms
in-app.

**Viewport validation - decided: compare size only, not origin.** The consumer
checks the snapshot's `w`/`h` against the live scene rect and refuses a
mismatch. It deliberately does **not** compare `x`/`y`, though a reviewer asked
for it. Recording the trade because the failing case is real and reachable, not
hypothetical: `ui_layout_scene_rect()` gives CODE_PANEL_LAYOUT_TOP
`(0, 0, win_w, win_h - panel_h)` and LAYOUT_BOTTOM
`(0, panel_h, win_w, win_h - panel_h)` - **identical size, different origin** -
so toggling the code panel between top and bottom shifts the rect without
changing its dimensions, and the retained buffer then describes a region
`panel_h` away. The consequence is bounded at one frame of wrong occlusion
during a layout switch that is already a visible jump, which is why it is
accepted. Note for whoever revisits: `x`/`y` have to be stored anyway, because
the capture needs them to issue `glReadPixels(x, y, w, h)` - so if this ever
bites, the fix is two extra integer comparisons and costs nothing at runtime.

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
| [`src/app/glr_ctrl.c`](../../../src/app/glr_ctrl.c) | Snapshot storage, capture entry point, realloc/invalidate on viewport change, invalidate on label-off / reset / scene load / capability loss, clear `valid` on read or allocation failure, publish the view into `OverlaySnapshotPack` |
| [`src/app/glr_ctrl.c`](../../../src/app/glr_ctrl.c) `glr_shutdown()` | Free the snapshot - it currently frees only audio / hidden-line / executor resources |
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

New **consumer-side** coverage, all of which the refactor makes easy because
the pass becomes pure over a buffer - no GL trace needed:

- Occlusion culls against a supplied buffer.
- A `valid = 0` snapshot leaves every label visible (the fallback).
- A snapshot whose dimensions disagree with the current viewport is refused
  rather than indexed.

New **controller-side** coverage. This is the part that would otherwise go
uncovered: the expensive, stateful half of the feature moves out of
`test_edit_overlays.c`, and a plan that only relocates the three existing
assertions would leave the whole lifecycle untested. Belongs with the other
controller tests in [`tests/test_glr_ctrl.c`](../../../tests/test_glr_ctrl.c):

- Capture is skipped when labels are off, and when the capability is false.
- **First-frame latency**: enabling labels leaves the first pass with no
  snapshot; the second frame has one.
- **Stale-publish, the P1 case**: labels on -> off -> on must not hand the pass
  the pre-`off` buffer. Same for a scene load and for `glr_ctrl_reset_all`
  between the two captures.
- **Viewport change** invalidates rather than publishing a wrongly-sized buffer.
- **Failure paths** clear `valid`: a `glReadPixels` that raises an error, and an
  allocation that fails, must both leave the consumer with no snapshot rather
  than the previous one. Needs a seam to force each - the GL stubs can be made
  to fail the read; the allocation path may need a size that cannot be honoured.
- **Ordering**: the capture happens after the frame's `glFinish` and before
  `glutSwapBuffers`. This is the property the whole fix rests on, and it is
  invisible in any timing-free test unless asserted directly - assert call
  order from the stub trace.
- Shutdown frees the buffer (visible under ASan/LSan in the debug test build).

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

So on this driver the synchronous path is specific to reading
`GL_DEPTH_COMPONENT`, at any size, from any framebuffer, into a PBO or not,
fenced or not. Nothing on the client side defers it.

**Scope that claim carefully.** It is a measurement of one vendor's driver, not
a property of OpenGL: `glReadPixels` into a PBO is *specified* as asynchronous,
and the RGBA control shows this driver honours that for colour. Independently
reproduced on a second machine (`zen3.local`, RTX 5050, driver 610.43.02):
direct depth->PBO 16.442 ms kick, +fence 16.439 ms, 1x1 16.394 ms, RGBA control
0.007 ms, lagged-after-glFinish Frame Work 0.149 ms against a 0.147 ms baseline.
Two drivers a major version apart agree, so it is not a one-off - but another
vendor, or a later NVIDIA release, could differ, and the bench exists so that is
one command to check rather than an assumption to inherit.

There is a design that could plausibly make an async depth readback work here:
render depth encoded into a colour attachment and read *that* asynchronously,
since colour demonstrably goes async. That is a shader plus FBO change to the
scene pass, not a PBO follow-up, and it buys nothing over version 1 - which
already costs baseline - so it is noted rather than proposed.

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
