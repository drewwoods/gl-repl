# Observe the Scene Background From Real Execution

## Status - IN REVIEW (2026-08-04)

This plan follows the clear-color source-order work in:

- `71eae8f6` - remove controller clear-color snooping and the export-time
  clear-color hoist;
- `ccbf7de7` / `62402be6` - normalize examples so `glClearColor` precedes the
  `glClear` it is intended to affect;
- `a0b89143` - resolve the presentation background in source order for chrome,
  grid/axes fog, and overlay contrast;
- `078914cc` - extend that resolver through multiple clears and `goto` control
  flow.

The first two decisions are sound: the user's program must issue its own clear,
and exported C and the live REPL must execute `glClearColor` / `glClear` in the
same order. The follow-up resolver repaired presentation consumers, but did so
by adding a second, partial interpreter beside `ReplExecCursor`. That approach
has already needed two semantic follow-ups and still cannot describe every
program the REPL accepts.

The target design keeps immediate-mode execution authoritative and makes the
background an observation produced by that execution. No controller or
renderer walks the flat program to predict what the executor will do.

## Goals

- Preserve real immediate-mode ordering for `glClearColor`, `glColorMask`,
  `glClear`, `glPushAttrib`, `glPopAttrib`, and `goto`.
- Give chrome, grid/axes fog, backdrop/helpers, and overlay contrast one honest
  per-pass background observation.
- Handle multiple and partially masked color clears without inventing a fully
  known RGBA value when framebuffer history is required.
- Resolve time-blur backgrounds from the flat program actually baked for each
  accumulation sample.
- Keep `src/render3d/` independent of the REPL program model. It may consume a
  generic execute result, but must not receive `GLCmd`, `FlatProgramView`, or
  `SourceTextView`.
- Delete the duplicate clear-color program walk and, if the audit confirms no
  remaining consumer, the runtime `ReplRenderState.clear_color` mirror.
- Retain the existing rule that deleting the source `glClear` leaves the scene
  rectangle uncleared; the host must not clear it on the program's behalf.

## Non-goals

- Inferring the visible color of arbitrary pixels after geometry, blending,
  texturing, fog, or post-processing.
- Sampling the framebuffer with `glReadPixels` to guess a background. A corner
  can contain geometry, and the synchronous readback would add a pipeline
  stall to every frame.
- Making replay prefixes behave like complete frames. Replay may continue to
  use an explicit presentation policy, but that policy must be named rather
  than hidden inside the executor's semantic result.
- Making hidden-line rendering identical to normal fill rendering. It is a
  synthetic visualization pass and already deliberately forces color writes
  on for its one clear. Its observed result must describe that synthetic pass.
- Changing export source order or reintroducing an export-time clear-color
  hoist.

## Problems in the current implementation

### 1. A clear color is not always a background color

`repl_flat_resolve_clear_color()` tracks clear color, attribute scopes, color
clears, and `goto`, but it does not track `CMD_COLOR_MASK`:

```c
glClearColor(0.05, 0.0, 0.0, 1.0);
glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
glClear(GL_COLOR_BUFFER_BIT);
```

The real clear writes no color pixels. The resolver nevertheless reports red,
so the controller clears the surrounding chrome red and configures fog and
overlay contrast for a background the scene never received.

With a partial mask there may be no source-only RGBA answer. Enabled channels
come from the current clear color; disabled channels retain their previous
framebuffer values unless an earlier clear in this frame made them known. The
result therefore needs a known-channel mask, not just `float rgba[4]`.

### 2. Time blur changes the program after resolution

`glr_ctrl_build_scene_config()` resolves `config->clear_color` once. Later,
`glr_ctrl_setup_subframe()` rebakes or fully re-flattens the program at each
sampled `t`. An animated `glClearColor` or a time-dependent branch selecting a
clear can therefore execute a different background in every sample while:

- grid/axes fog reads the one pre-resolved frame value;
- `alpha_scale` reads the same stale value;
- chrome is cleared once to that value rather than to the accumulated result.

The renderer already has a per-sample configuration and frame context. The
background must become per-sample data alongside them.

### 3. The resolver duplicates executor semantics

The resolver has its own program counter, `goto` target search, loop budget,
attribute depth, and selected state machine. Any new command that affects a
clear requires coordinated changes in both the resolver and the executor.

The shared label-text helper also bounds the supplied `SourceTextView` through
the global `repl_state_document_count()`. That makes temporary program/text
views depend on unrelated live document state. Tests currently hide the issue
by synchronizing both counts manually.

### 4. Clear-color runtime bookkeeping has no rendering consumer

`ReplRenderState.clear_color` is reset, snapshotted, restored, attribute-scoped,
and tested, but current comments and call sites agree that no renderer reads
it. Actual GL restores the state through `glPopAttrib`; suppressed passes have
no production consumer for the mirror. Keeping it only to make the mirror
internally self-consistent expands every render-state capture/restore boundary
without contributing a result.

Before deletion, repeat the production-use audit without tests and docs. If a
real consumer appears during implementation, narrow the mirror to that
consumer's explicit output contract rather than retaining it as ambient state.

## Design

### 1. Generic background observation

Add a generic result type at the render callback boundary. Names are
illustrative; settle exact placement while implementing:

```c
enum {
    RENDER3D_RGBA_R = 1u << 0,
    RENDER3D_RGBA_G = 1u << 1,
    RENDER3D_RGBA_B = 1u << 2,
    RENDER3D_RGBA_A = 1u << 3,
    RENDER3D_RGBA_ALL = 0x0fu
};

typedef struct Render3dExecuteResult {
    float    background_rgba[4];
    unsigned background_known_mask;
    int      color_clear_seen;
} Render3dExecuteResult;
```

`Render3dExecuteContext` supplies an output pointer for the current pass. The
type describes pixels and contains no REPL vocabulary. A caller that emits a
procedural scene can populate it without linking the REPL.

`background_known_mask == RENDER3D_RGBA_ALL` means the program established a
uniform conceptual clear background for every channel. A partial mask is
valid data, not an error. `color_clear_seen` distinguishes no color clear from
a color clear that could not make every channel known.

The initial framebuffer background is unknown. The bootstrap clear color is
the initial *GL clear-color state*, not evidence that pixels were cleared.
This preserves the current smear behavior when source contains no color clear.

### 2. Observe inside `ReplExecCursor`

Extend `ReplExecutionOptions` with:

```c
Render3dExecuteResult *result_out; /* or a REPL-neutral equivalent */
const float            baseline_clear_rgba[4];
```

Do not literally make `src/repl/executor.h` depend on a render3d header if that
would invert the module boundary. Prefer a neutral `ReplExecutionObservation`
with the same fields and copy it into `Render3dExecuteResult` in
`scene_execute_adapter`. The important constraint is one semantic walk, not
one shared typedef.

The cursor owns a small clear-state tracker:

```c
typedef struct ReplClearObservationState {
    float    clear_rgba[4];
    unsigned color_write_mask;
    float    background_rgba[4];
    unsigned background_known_mask;
    int      color_clear_seen;
} ReplClearObservationState;
```

Rules:

- Initialize `clear_rgba` from the frame bootstrap default.
- Initialize `color_write_mask` to all channels enabled.
- Initialize `background_known_mask` to zero.
- `CMD_CLEAR_COLOR` updates `clear_rgba`.
- `CMD_COLOR_MASK` updates `color_write_mask`.
- `CMD_PUSH_ATTRIB` whose mask includes `GL_COLOR_BUFFER_BIT` saves clear color
  and color-write mask; the matching pop restores them.
- `CMD_CLEAR` containing `GL_COLOR_BUFFER_BIT` copies each enabled channel from
  `clear_rgba` into `background_rgba` and marks that channel known. Disabled
  channels retain their previous value and knownness.
- Gotos require no special observation code: the existing cursor moves the PC,
  so only commands actually reached affect the result.
- A state filter affects the observation exactly as it affects emitted GL. Do
  not update the observation for a command the pass suppresses unless that pass
  explicitly defines a synthetic replacement.

This is an observation of the executor, not a second executor. It must live in
the same cases that emit or deliberately suppress the relevant GL calls.

Fix `execution_flat_text()` in the same stage: use
`SourceTextView.line_count` through `source_text_line()` and remove the global
document-count gate. Factor goto target lookup so the cursor has one target
implementation; do not add another analyzer-only copy.

### 3. Make the pass context mutable after user fill

`render_3d_scene_pass()` already has the required ordering:

1. pass setup;
2. user fill;
3. buffer read;
4. backdrop/grid/axes/lights helpers;
5. geometry-reporting overlays.

Change the fill path to publish its `Render3dExecuteResult` into the local
`Render3dFrameRenderContext` before helpers execute. The input config keeps a
bootstrap value used to establish GL state, but it is no longer named as if it
were the final frame background:

```c
float baseline_clear_color[4];
```

The pass context carries the observed presentation background separately:

```c
float    background_rgba[4];
unsigned background_known_mask;
float    alpha_scale;
```

If all RGB channels are known, derive luminance and `alpha_scale` from the
observed pass background. If RGB is incomplete, use the bootstrap/default
presentation color and its neutral contrast scale. The fallback is explicitly
presentation policy; it must never be described as the color the source
actually cleared.

Grid and axes fog read the pass context's background. Post-fill and overlay
callbacks must receive the pass presentation/result, or an equivalent immutable
view, so the app does not have to build `g_overlay_pack` before execution. Build
or refresh the guide/overlay snapshot after the main fill observation is known.

This removes the existing timing dependency where `g_overlay_pack` must be
built at the very end of `glr_ctrl_build_scene_config()` because
`alpha_scale` was computed there.

### 4. Accumulation result

Each accumulation pass reports its own background after its subframe callback
has rebaked the program.

For the frame result:

- A channel is known only if it is known in every accumulated pass.
- For a known channel, accumulate `pass.background_rgba[channel] * weight`
  with the same weights passed to `glAccum`.
- AA and camera blur normally collapse to the same value across passes.
- Time blur naturally produces the weighted background of the sampled shutter
  interval.
- If any pass leaves a channel unknown, the final frame leaves it unknown and
  chrome uses the explicit fallback for that channel or for the full color,
  according to the policy chosen below.

Prefer an all-or-fallback chrome policy in the first implementation: use the
observed RGBA only when all four channels are known; otherwise clear chrome to
the bootstrap default. Per-channel chrome composition would still require the
previous chrome framebuffer contents and adds no corresponding benefit to fog
or luminance consumers.

Return the aggregate through an explicit output parameter:

```c
int render3d_draw_scene(Render3dState *state,
                        const Render3dRenderConfig *config,
                        Render3dFrameResult *out);
```

Allow `out == NULL` so `render3d_demo` and callers uninterested in presentation
metadata stay simple. Do not store the result in a file-static or require a
post-call getter.

### 5. Clear chrome after the scene

Move `glr_ctrl_clear_chrome()` after `render3d_draw_scene()` and after the
controller pops the scene-rect scissor, but before 2D overlays render.

The four-strip clear still excludes the scene rectangle, so it cannot repair,
erase, or otherwise change user pixels. Moving it later lets it consume the
actual aggregate frame result rather than a prediction. It also remains
controller-owned because only the controller knows the window/chrome layout.

Use the aggregate background when fully known; otherwise use
`CFG_DEFAULT_CLEAR_*`. Continue to set `glClearDepth(1.0)` before clearing the
chrome depth strips.

Check profiler attribution after moving the work. It remains inside
`PROF_RENDER3D` only if that section is still defined as scene plus surrounding
chrome clear; otherwise add or reuse an explicit controller section so the
four clears do not become unattributed frame work.

### 6. Specialized execution purposes

Only the pass responsible for the displayed fill publishes the authoritative
background:

| View/path | Authoritative observation |
|---|---|
| Normal / plain wireframe | `RENDER3D_EXEC_MAIN_FILL` |
| Winding view | `RENDER3D_EXEC_WINDING` |
| Hidden-line view | depth-fill pass's one synthetic clear |
| Depth probe | none |
| Hidden/visible line redraws | none |
| Replay fade overlays | none; they deliberately suppress clear |

`hidden_lines_emit_clear()` forces all color channels writable for the clear,
irrespective of the user's `glColorMask`, because that visualization owns its
color/depth recipe. Report that forced full-mask clear as the hidden-line
background. Do not claim it is normal immediate-mode behavior; it is the
documented synthetic-pass behavior.

Auxiliary passes that snapshot and restore program side effects must also
snapshot/restore or discard observations. Only the selected authoritative pass
may publish into the frame result.

### 7. Remove duplicate and dead state

After all consumers use execution results:

- delete `repl_flat_resolve_clear_color()` and its tests;
- delete the resolver-specific goto target walk;
- remove the resolver call from `glr_ctrl_build_scene_config()`;
- rename render config's input clear value to make its bootstrap role clear;
- remove `ReplRenderState.clear_color`, its setter/reset code, and its
  `ReplAttribSave` restoration if the production-use audit remains empty;
- leave `ReplRenderState.light_enabled_mask` intact, because light indicators
  consume it;
- update state ownership docs and tests to describe the narrower render tail;
- update comments that still say the resolver stops at the first color clear.

The GL state inspector keeps its own source/attribute analysis and is not a
consumer of `ReplRenderState.clear_color`; verify that boundary rather than
folding inspector concerns into the frame result.

## Implementation sequence

### Phase 0 - Lock in the failures

Add regressions before changing architecture:

1. Full-disabled color mask followed by color clear does not produce a known
   background.
2. Partial mask after an earlier full clear updates only enabled channels and
   preserves known values for the rest.
3. Partial mask with no earlier known clear leaves disabled channels unknown.
4. `GL_COLOR_BUFFER_BIT` push/pop scopes both clear color and color mask.
5. Multiple clears retain the result of the final effective channel writes.
6. Forward/backward goto takes exactly the cursor's path and terminates under
   its existing loop budget.
7. A temporary `SourceTextView` resolves goto without synchronizing global
   document count.
8. Time-blur subframes with an animated clear color produce distinct pass
   observations and the expected weighted frame result.
9. A source file with no color clear leaves the scene result unknown and does
   not cause a host-side scene clear.

Prefer differential tests around `ReplExecCursor`: feed one flat program to
the actual cursor and assert the observation emitted by the commands it really
visits. Do not replace resolver unit tests with another standalone analyzer
test suite.

### Phase 1 - Cursor observation and source-view fix

- Add the REPL-neutral observation/result type.
- Add clear color, color mask, known-channel, and clear-seen fields to the
  cursor.
- Scope them through the cursor's existing attribute frames.
- Publish the observation from `repl_execute_program()`.
- Fix `execution_flat_text()` to honor only the supplied view.
- Consolidate goto target lookup for cursor use.
- Cover filtered and suppressed state-command behavior.

This phase may coexist temporarily with the static resolver, but no new
consumer should be added to the resolver.

### Phase 2 - Per-pass render result

- Extend the generic render execute context/result boundary.
- Update `scene_execute_adapter()` and specialized passes.
- Publish the authoritative result into `Render3dFrameRenderContext` after
  fill.
- Derive per-pass background-dependent fog and alpha there.
- Pass presentation data to post-fill/post-overlay hooks.
- Rebuild the overlay snapshot after the result is known.
- Keep `render3d_demo` compiling with `out == NULL` and no REPL dependency.

### Phase 3 - Accumulation and chrome ordering

- Aggregate per-pass known backgrounds with accumulation weights.
- Return `Render3dFrameResult` from `render3d_draw_scene()`.
- Move the chrome-strip clear after scene rendering.
- Apply the explicit default fallback when the aggregate is incomplete.
- Add time-blur and unknown-channel controller tests.
- Verify profile coverage after reordering.

### Phase 4 - Delete the parallel model

- Remove `repl_flat_resolve_clear_color()`.
- Remove `ReplRenderState.clear_color` if the final audit confirms it remains
  consumer-free.
- Simplify auxiliary-pass capture/restore structs and tests.
- Update architecture/module/contributing documentation.
- Remove stale tests that only validate the deleted predictor; retain source
  ordering, export parity, and execution-observation tests.

## Test plan

### Focused suites

- `test_repl_executor`
  - observation state transitions;
  - attrib scoping;
  - color masks;
  - multiple clears;
  - goto and loop-limit parity;
  - temporary source views.
- `test_glr_ctrl`
  - chrome clear happens after the scene result;
  - known/unknown fallback policy;
  - overlay `alpha_scale` comes from the pass result;
  - replay policy remains stable and explicit;
  - time-blur aggregation.
- render3d tests/demo
  - callback result is generic;
  - `out == NULL` is supported;
  - accumulation aggregation;
  - no REPL link dependency.
- `test_hidden_lines`
  - the synthetic depth-fill clear reports its forced full-mask background;
  - later hidden/visible passes cannot overwrite the result.
- `test_repl_export_clearcolor`
  - source order remains unchanged;
  - bootstrap default remains in `init()`;
  - no clear-color hoist returns.
- `test_repl_core_examples`
  - authored examples keep `glClearColor` before their intended clear.

### Pixel verification

Use a real OSMesa build for cases the no-op GL stubs cannot prove:

- normal fill with all color-mask channels disabled;
- partial channel masks after a known full clear;
- two differently colored clears;
- an animated clear color under time blur;
- no-clear smear behavior;
- hidden-line view's synthetic clear.

Compare a scene-background pixel, a chrome-strip pixel, and a fogged helper
pixel where applicable. Keep these fixtures minimal and deterministic.

### Full verification

```bash
make test-stubs
make gl-repl USE_GL_STUBS=1
make gl-repl
make render3d-demo
make check-c99
make check-state-ownership
make test-web
```

For portability-sensitive cursor/config changes, run the documented real-GCC
lane on `gracemont` after the branch is pushed to a reviewable ref.

## Documentation updates

- `docs/ARCHITECTURE.md`
  - frame order: user execution produces a pass result; helpers consume it;
    chrome clears afterward;
  - accumulation result aggregation;
  - explicit unknown-background fallback;
  - specialized-pass ownership table.
- `src/repl/ARCHITECTURE.md`
  - cursor observation is part of the real execution walk;
  - no static background resolver.
- `docs/MODULES.md`
  - executor owns command semantics and observations;
  - render3d owns generic per-pass/result plumbing;
  - controller owns chrome layout and fallback policy.
- `docs/CONTRIBUTING.md`
  - a new state command that affects clearing updates cursor execution once;
    it must not create an analyzer-side execution path.
- `src/render3d/README.md`
  - execute-result contract for standalone callers.

## Acceptance criteria

- Live REPL and exported C retain source-ordered `glClearColor` / `glClear`
  behavior.
- There is no flat-program clear-color prediction walk in the controller,
  render3d, or executor support code.
- `CMD_COLOR_MASK`, attrib scopes, multiple clears, and gotos affect the
  presentation observation through the same cursor that emits GL.
- Time-blur helpers use each rebaked subframe's result, and chrome uses the
  accumulated frame result.
- Unknown or partially known backgrounds are represented explicitly; no API
  silently turns them into a supposedly exact RGBA value.
- The host never clears the scene rectangle when the source did not do so.
- `src/render3d/` remains REPL-free and `render3d_demo` links.
- No production consumer remains for `ReplRenderState.clear_color`; the field
  is removed, or the discovered consumer and retained invariant are documented
  before implementation proceeds.
- Focused tests, stubbed full tests, C99/ownership guards, native build, and
  OSMesa pixel checks pass.

## Review questions

1. Is all-or-default the right chrome fallback for an incomplete known-channel
   mask, or should chrome expose a separate fixed theme color whenever the
   source does not establish all four channels?
2. Should replay always use the complete program's most recent known
   background as presentation policy, or should it use only the executed
   prefix and accept background changes as the replay PC advances? This is a
   replay product decision, not executor semantics, and should be named in the
   replay layer.
3. Should the generic render callback return its result through
   `Render3dExecuteContext`, or should `Render3dRenderConfig` carry a separate
   result-producing callback? Prefer the former unless a standalone caller
   demonstrates that execution and observation must be supplied separately.
