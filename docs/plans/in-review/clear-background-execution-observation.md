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

The primary justification is architectural, not the frequency of any one
pixel mismatch: the parallel resolver needed two semantic corrections within
one day of landing (multiple clears, then `goto`), and neither omission was
caught by its original tests. Color masks and time blur are further examples
of the same failure mode. One semantic walk makes that class of drift
impossible.

## Goals

- Preserve real immediate-mode ordering for `glClearColor`, `glColorMask`,
  `glClear`, `glPushAttrib`, `glPopAttrib`, and `goto`.
- Give grid/axes fog, backdrop/helpers, and overlay contrast the observation
  from their render pass, and give chrome one explicitly selected frame
  presentation color.
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
- Making recede fog backdrop-aware. Existing architecture notes already record
  that fading toward the clear color is wrong when a backdrop paints a
  different background. This plan rewires how the clear background reaches
  fog; it does not solve backdrop/color selection.
- Changing export source order or reintroducing an export-time clear-color
  hoist.
- Preserving source or binary compatibility for the internal executor and
  render callback APIs. This is an unreleased internal refactor; change call
  signatures directly and do not add compatibility shims or parallel APIs.
- Keeping every implementation phase releasable on its own. The phases are an
  implementation sequence and may be completed on one branch before landing.

## Problems in the current implementation

### 1. The resolver duplicates executor semantics

The resolver has its own program counter, `goto` target search, loop budget,
attribute depth, and selected state machine. Any new command that affects a
clear requires coordinated changes in both the resolver and the executor.

That risk is demonstrated, not hypothetical: multiple-clear behavior and
`goto` traversal each required a follow-up immediately after the resolver was
introduced, and review rather than the shipped tests found both gaps. A second
partial interpreter beside `ReplExecCursor` is the coupling this plan removes.

The shared label-text helper also bounds the supplied `SourceTextView` through
the global `repl_state_document_count()`. That makes temporary program/text
views depend on unrelated live document state. Tests currently hide the issue
by synchronizing both counts manually.

### 2. A clear color is not always a background color

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

### 3. Time blur changes the program after resolution

`glr_ctrl_build_scene_config()` resolves `config->clear_color` once. Later,
`glr_ctrl_setup_subframe()` rebakes or fully re-flattens the program at each
sampled `t`. An animated `glClearColor` or a time-dependent branch selecting a
clear can therefore execute a different background in every sample while:

- grid/axes fog reads the one pre-resolved frame value;
- `alpha_scale` reads the same stale value;
- chrome is cleared once to that value rather than to the accumulated result.

The renderer already has a per-sample configuration and frame context. The
background must become per-sample data alongside them. The first version does
not need to average those colors for chrome; it needs the observation to match
the program each helper pass actually executes and one defined final-frame
presentation color.

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

```c
typedef struct Render3dExecuteContext {
    Render3dExecutePurpose purpose;
    Render3dExecuteResult *result_out;
} Render3dExecuteContext;
```

The callback still receives `const Render3dExecuteContext *`: the context and
pointer binding are immutable, while the object referenced by `result_out` is
the explicit output. Do not add a separate observation callback.

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
ReplExecutionObservation *observation_out;
float                     baseline_clear_rgba[4];
```

Do not literally make `src/repl/executor.h` depend on a render3d header if that
would invert the module boundary. `ReplExecutionObservation` is the neutral
executor-owned type; copy its fields into `Render3dExecuteResult` in
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

#### Emission-decision contract

Update the observation at the point where a pass has decided that a state
change or clear will actually be emitted. Neither of the existing broad helper
sites is sufficient:

- do not put clear observation in `repl_apply_state_bookkeeping()`, because
  hidden-line rendering calls that helper even for clears it deliberately
  skips;
- do not put it only in `repl_apply_state_cmd()`, because the hidden-line
  subsystem drives a cursor itself and emits `CMD_CLEAR_COLOR` / `CMD_CLEAR`
  outside that helper.

Expose narrow cursor operations for specialized emitters, for example:

```c
void repl_exec_cursor_observe_clear_color(ReplExecCursor *cursor,
                                          const GLCmd *cmd);
void repl_exec_cursor_observe_color_mask(ReplExecCursor *cursor,
                                         const GLCmd *cmd);
void repl_exec_cursor_observe_color_clear(ReplExecCursor *cursor,
                                          const GLCmd *cmd,
                                          unsigned effective_write_mask);
```

The normal executor calls these immediately beside the corresponding GL
emission, after state filters and other suppression decisions. Hidden-line
rendering calls the clear-color operation when it really emits
`CMD_CLEAR_COLOR`, and `hidden_lines_emit_clear()` gets the paired explicit
color-clear operation with all four channel bits because that pass forces
`glColorMask(TRUE, TRUE, TRUE, TRUE)`. Its hidden/visible redraws skip both the
GL clear and the observation. Winding rendering uses the normal cursor path
after its filter, so it needs no special observation branch.

`repl_apply_state_bookkeeping()` is narrowed back to independently consumed
bookkeeping such as the light-enable mask. It must not become an implicit
execute-result hook.

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
float overlay_design_luma;
```

The pass context carries the observed presentation background separately:

```c
float    background_rgba[4];
unsigned background_known_mask;
float    alpha_scale;
```

Use one fallback rule for every presentation consumer. An observed background
is usable only when all four channels are known. Otherwise chrome, grid/axes
recede fog, and overlay luminance/`alpha_scale` all use the caller-supplied
bootstrap presentation color and neutral `alpha_scale = 1.0`. The full app
populates those inputs from `CFG_DEFAULT_CLEAR_*` /
`CFG_DEFAULT_CLEAR_LUMA`; render3d does not include app defaults. When the
background is known, `overlay_design_luma` supplies the reference for the
existing contrast formula. The fallback is explicitly presentation policy and
must never be described as the color the source actually cleared.
All-or-default is deliberate: per-channel chrome composition would require
the previous chrome contents and a readback this plan rules out.

Grid and axes read the pass context's selected presentation background and
`alpha_scale` at draw time. Post-fill and overlay callbacks receive the same
small pass presentation/result view.

Do not rebuild `g_overlay_pack` or rerun guide computation after user fill.
`glr_ctrl_build_guide_snapshot()` captures only one background-dependent value:
`snapshot.alpha_scale`. Keep the existing snapshot build, pass the current
pass presentation to the overlay hook, copy the snapshot locally at draw time,
and override that one field before rendering. This avoids repeating
`repl_flatten_cursor_polygon()` and the rest of guide preparation every frame.

### 4. One frame presentation color under accumulation

Each accumulation pass reports its own background after its subframe callback
has rebaked the program. That pass's helpers use that pass's selected
background, so time-blurred fog and overlays follow the commands actually
executed for the sample.

Do not average pass background colors in the first implementation. The frame
publishes one presentation color:

- retain the final pass's RGBA (time blur deliberately bakes that pass at
  exactly `t_end`);
- mark the frame result usable only if every accumulation pass produced a
  fully known background;
- otherwise publish the all-or-default fallback.

AA and camera blur normally observe the same color in every pass. Time blur may
accumulate different scene colors while chrome uses the single `t_end`
presentation color. That is an accepted v1 approximation. Add weighted
background averaging only if a real time-blur scene demonstrates a visible
need; it is not part of this plan.

Return the frame result through an explicit output parameter:

```c
int render3d_draw_scene(Render3dState *state,
                        const Render3dRenderConfig *config,
                        Render3dFrameResult *out);
```

Allow `out == NULL` for callers uninterested in presentation metadata. Update
both standalone proof paths: the ordinary `render3d_demo` call and the
hot-reload shim's two-argument function-like macro plus its `dlsym` function
pointer. Do not store the result in a file-static or require a post-call
getter.

### 5. Clear chrome after the scene

Move `glr_ctrl_clear_chrome()` after `render3d_draw_scene()` and after the
controller pops the scene-rect scissor, but before 2D overlays render.

The four-strip clear still excludes the scene rectangle, so it cannot repair,
erase, or otherwise change user pixels. Moving it later lets it consume the
actual frame presentation result rather than a prediction. It also remains
controller-owned because only the controller knows the window/chrome layout.
The intervening consumers are safe: buffer visualization reads only the scene
rectangle, and the whole-window compositor postprocess runs after the 2D
overlays. The load-bearing ordering rule is simply that chrome clears before
those overlays paint it.

Use the frame background when fully known; otherwise use
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

Replay uses the observation from the actually executed main-fill prefix. Do
not freeze a complete-program background at replay start. The existing
`replay_frame_setup_limit()` keeps the leading clear in the executable prefix,
so every shipped single-clear example remains stable. In a multi-clear program
the presentation background changes when the PC crosses a later clear, which
honestly represents partial execution and needs no snapshot invalidation
protocol. Fade batches publish no observation.

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

The deletion does not drop the GL-stub-observable attrib restoration coverage
that originally justified keeping the mirror. The cursor's scoped clear
observation becomes the observable: tests assert that a clear color and color
mask changed inside `glPushAttrib(GL_COLOR_BUFFER_BIT)` are restored in the
cursor state at the matching pop. Move the coverage there before deleting the
runtime mirror so a future audit does not need to re-litigate the decision.

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
   observations; helpers use the per-pass value and the frame publishes the
   final `t_end` value when every pass is fully known.
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
- Add the explicit emission-decision operations used by the normal executor
  and hidden-line renderer.

This phase is sequencing inside the implementation branch, not a separately
releasable compatibility state. No bridge or dual-answer contract is required.

### Phase 2 - Per-pass render result

- Extend the generic render execute context/result boundary.
- Update `scene_execute_adapter()` and specialized passes.
- Publish the authoritative result into `Render3dFrameRenderContext` after
  fill.
- Derive per-pass background-dependent fog and alpha there.
- Pass presentation data to post-fill/post-overlay hooks.
- Override only the copied guide snapshot's `alpha_scale` at overlay draw time;
  do not rebuild the guide/overlay pack.
- Update the ordinary and hot-reload `render3d_demo` call surfaces while
  preserving the REPL-free link proof.

### Phase 3 - Accumulation validity and chrome ordering

- Retain the final pass's presentation color and require every pass to be fully
  known before publishing it as the frame color.
- Return `Render3dFrameResult` from `render3d_draw_scene()`.
- Move the chrome-strip clear after scene rendering.
- Apply the one explicit chrome/fog/alpha fallback when the frame is incomplete.
- Add per-pass time-blur and unknown-channel controller tests.
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
  - time-blur per-pass helpers and final-pass frame color.
- render3d tests/demo
  - callback result is generic;
  - `out == NULL` is supported;
  - all-passes-known validity and final-pass color selection;
  - ordinary and hot-reload call signatures;
  - no REPL link dependency.
- `test_hidden_lines`
  - the synthetic depth-fill clear reports its forced full-mask background;
  - later hidden/visible passes cannot overwrite the result.
- `test_repl_export_clearcolor`
  - source order remains unchanged;
  - bootstrap default remains in `init()`;
  - no clear-color hoist returns.
- `test_export_trace_parity`
  - the actual REPL execution trace remains aligned with exported C; this is
    the behavioral symmetry established by `71eae8f6`, beyond merely checking
    emitted source text.
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
pixel where applicable. For animated time blur, assert the documented final
`t_end` chrome policy rather than equality with the accumulated scene pixel.
Keep these fixtures minimal and deterministic.

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
  - per-pass results plus final-pass frame presentation selection;
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
- `src/render3d/render.c`
  - update the accumulation comment near the per-pass loop that currently
    says chrome was cleared before the passes and is accumulated at the same
    weight; chrome will instead be cleared after scene rendering.

## Acceptance criteria

- Live REPL and exported C retain source-ordered `glClearColor` / `glClear`
  behavior.
- There is no flat-program clear-color prediction walk in the controller,
  render3d, or executor support code.
- `CMD_COLOR_MASK`, attrib scopes, multiple clears, and gotos affect the
  presentation observation through the same cursor that emits GL.
- Time-blur helpers use each rebaked subframe's result; the frame uses the
  final `t_end` pass's single presentation color only when every pass was
  fully known.
- Unknown or partially known backgrounds are represented explicitly; no API
  silently turns them into a supposedly exact RGBA value.
- The host never clears the scene rectangle when the source did not do so.
- `src/render3d/` remains REPL-free; ordinary and hot-reload `render3d_demo`
  variants link with the new callback signature.
- No production consumer remains for `ReplRenderState.clear_color`; the field
  is removed, or the discovered consumer and retained invariant are documented
  before implementation proceeds.
- Focused tests, stubbed full tests, C99/ownership guards, native build, and
  OSMesa pixel checks pass.

## Settled review decisions

1. **Unknown background:** all-or-default for chrome, grid/axes fog, and
   overlay contrast. No per-channel chrome composition or readback.
2. **Replay:** follow the executed prefix. A later clear changes the background
   when the replay PC reaches it; fade batches publish nothing.
3. **Callback shape:** return through the non-const `result_out` referenced by
   the otherwise-const `Render3dExecuteContext`. A separate result-producing
   callback is rejected because it would allow execution and observation to
   diverge into two walks again.
4. **Accumulation:** one final-pass frame presentation color, gated on every
   pass being fully known. No weighted background averaging in v1.
5. **Compatibility:** no internal API backward-compatibility shims and no
   requirement that intermediate implementation phases be releasable.
