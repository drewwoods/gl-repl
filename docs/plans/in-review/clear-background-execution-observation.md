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
needed two semantic corrections within a day of landing (multiple clears, then
`goto`), neither caught by its own tests, and it still cannot describe
`glColorMask`.

The target design makes immediate-mode execution the only authority: the
executor **observes** the background as it emits the clear, and the controller
**retains** the last observed background as this session's presentation color.
Nothing walks the flat program to predict what the executor will do.

### What changed in this revision

An earlier draft of this plan delivered the observation to every consumer
*within the frame that produced it*. That requirement - not the observation
itself - was responsible for most of its machinery: a generic render execute
result, an output pointer on the execute context, per-pass mutation of the
frame render context, a `Render3dFrameResult` return, a
`render3d_draw_scene()` signature change with matching `render3d_demo` and
hot-reload updates, a REPL-to-render conversion helper with field-parity tests,
and a per-pass story for time blur.

This revision keeps the architectural win - one semantic walk - and drops the
same-frame requirement for everything except the chrome strips. See
"[The vintage decision](#1-the-vintage-decision)". `src/render3d/` gains three
plain input fields and loses nothing; the executor gains an observation; the
controller gains one retained `float[4]`.

## Goals

- Preserve real immediate-mode ordering for `glClearColor`, `glColorMask`,
  `glClear`, `glPushAttrib`, `glPopAttrib`, and `goto`, with **one**
  implementation of that ordering.
- Represent "the program did not establish a background" explicitly, instead of
  inventing an RGBA value the framebuffer never took.
- Give chrome, grid/axes recede fog, overlay contrast, and the cursor edit
  guides one coherent presentation background with a documented vintage.
- Keep `src/render3d/` independent of the REPL program model, and keep its
  public call surface (`render3d_draw_scene()`, `post_fill_fn`,
  `post_overlays_fn`, `post_resolve_overlays_fn`, `execute_fn`) unchanged.
- Delete the duplicate clear-color program walk and, if the audit confirms no
  remaining consumer, the runtime `ReplRenderState.clear_color` mirror.
- Retain the existing rule that deleting the source `glClear` leaves the scene
  rectangle uncleared; the host must not clear it on the program's behalf.
- Keep replay presentation stable before a replay prefix has executed a color
  clear, while letting later executed clears change it honestly.

## Non-goals

- Inferring the visible color of arbitrary pixels after geometry, blending,
  texturing, fog, or post-processing.
- Sampling the framebuffer with `glReadPixels`. A corner can contain geometry,
  and the synchronous readback would stall the pipeline every frame.
- Per-channel presentation composition. A partially masked clear leaves the
  disabled channels holding framebuffer history this plan refuses to read, so
  an incompletely known background is used as a whole or not at all.
- Making hidden-line rendering identical to normal fill rendering. It is a
  synthetic visualization pass that deliberately forces color writes on for its
  one clear; its observation describes that synthetic pass.
- Making recede fog backdrop-aware. Fading toward the clear color is already
  known to be wrong when a backdrop paints a different background; this plan
  rewires how the background reaches fog and does not solve that selection.
- Changing export source order or reintroducing an export-time clear-color
  hoist.
- Source or binary compatibility for internal executor APIs. Change signatures
  directly; add no shims.
- Keeping every implementation phase separately releasable. Phase 0 is a
  standalone precursor; phases 1-3 may land together on one branch.

## Problems in the current implementation

### 1. The resolver duplicates executor semantics

`repl_flat_resolve_clear_color()` (`src/repl/executor.c:551`) has its own
program counter, `goto` target search, loop budget, attribute-stack depth, and
selected-state machine. Any command that affects a clear needs coordinated
changes in it and in `ReplExecCursor`. That risk is demonstrated, not
hypothetical: multiple-clear handling and `goto` traversal each required a
follow-up immediately after the resolver landed, and review rather than its
tests found both gaps.

A related defect rides along: `execution_flat_text()`
(`src/repl/executor.c:518`) bounds the *caller-supplied* `SourceTextView`
through the global `repl_state_document_count()`, so a temporary program/text
view resolves `goto` correctly only if unrelated live document state happens to
agree. Tests currently paper over this by synchronizing both counts by hand.
This is an independent bug and lands first, on its own (see Phase 0).

### 2. A clear color is not always a background color

The resolver tracks clear color, attribute scopes, multiple color clears, and
`goto`, but not `CMD_COLOR_MASK`:

```c
glClearColor(0.05, 0.0, 0.0, 1.0);
glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
glClear(GL_COLOR_BUFFER_BIT);
```

The real clear writes no color pixels. The resolver reports red, so chrome
clears red and fog and overlay contrast are configured for a background the
scene never received. With a *partial* mask there may be no source-only answer
at all: enabled channels come from the current clear color, disabled channels
retain framebuffer history unless an earlier clear in the same frame made them
known. The observation therefore needs per-channel bookkeeping internally and a
single "did this establish all four channels" answer at its boundary.

### 3. Clear-color runtime bookkeeping has no rendering consumer

`ReplRenderState.clear_color` is reset, snapshotted, attribute-scoped, restored
(`src/repl/executor.c:302`, `:322`), and tested, but no renderer reads it -
`src/repl/state_views.h:118` and `src/repl/ARCHITECTURE.md:753` both say so.
Real GL restores the state through `glPopAttrib`; suppressed passes have no
production consumer for the mirror. It widens every render-state
capture/restore boundary without producing a result.

Repeat the production-use audit (excluding tests and docs) before deleting. If
a real consumer appears, narrow the mirror to that consumer's explicit output
contract rather than keeping it as ambient state.

## Design

### 1. The vintage decision

Three consumers need a background: the chrome strips, the grid/axes recede fog,
and the overlay contrast scale (`alpha_scale`, which also reaches the cursor
edit guides through the guide snapshot). The only real design question is
*which frame's* observation each one gets, because the answer decides how much
plumbing exists.

Same-frame delivery for grid/axes fog is expensive: fog is configured inside
the render pass, after user fill, so the observation must travel out of the
executor, across the render-callback boundary as a REPL-free type, into the
per-pass frame context, and - because time blur re-bakes the program per
accumulation sample - it must do so once per pass. That is the entire cost
listed under "What changed in this revision", and it buys a fog color that is
correct 16 ms sooner on faint distant grid lines.

**This plan takes the retained observation instead.** The controller keeps the
last fully-known background as `g_presentation_rgba`. Everything built before
the frame - `presentation_rgba`, `alpha_scale`, the guide snapshot - reads that
retained value, exactly where it reads the resolver's answer today. Only the
chrome strips, whose clear moves after the scene render, use the observation
produced by the frame they paint.

| Consumer | Vintage | Why that is enough |
|---|---|---|
| Chrome strips | this frame | Largest flat area and directly adjacent to the scene rect, so a mismatch is most visible here; the fix is moving one call. |
| Grid / axes recede fog | previous frame | Fog color on deliberately faint distant lines; one frame of lag is not resolvable. |
| Overlay contrast `alpha_scale` | previous frame | Same value, same source. |
| Cursor edit guides | previous frame | Mechanism unchanged: the snapshot copies `config->alpha_scale`. |

Two vintages, and the three previous-frame consumers share one number, so they
cannot disagree with each other. The earlier draft of this plan already
conceded a one-frame-late `alpha_scale` for the guides, on a decoration, to
avoid pushing presentation data through the overlay callbacks; this revision
applies that same tradeoff consistently instead of running two rules.

The visible consequence, stated plainly: on the frame a background *changes* -
edit, scene switch, replay progress, animation step - grid/axes fog and overlay
alpha use the previous background for one rendered frame while chrome and the
scene rect are already correct. There is no accumulating error and no
invalidation protocol.

### 2. Observe inside `ReplExecCursor`

The executor owns the type; nothing else needs to:

```c
/* The background the program's own clears established, as observed by the
 * walk that emitted them. `rgba` is meaningful only when `known` is set. */
typedef struct ReplBackgroundObservation {
    float rgba[4];
    int   known;    /* the program established all four channels */
} ReplBackgroundObservation;
```

`ReplExecutionOptions` gains:

```c
ReplBackgroundObservation *observation_out;   /* NULL = publish nothing */
float                      baseline_clear_rgba[4];
```

`baseline_clear_rgba` is the GL clear-color state the walk starts in - what a
`glClear` with no preceding `glClearColor` would use. A caller that publishes
must set it; a caller that passes `observation_out = NULL` need not.

Cursor state splits along its lifetime, deliberately:

```c
/* Scoped by GL_COLOR_BUFFER_BIT: saved by glPushAttrib, restored by the
 * matching pop. */
typedef struct ReplClearScopedState {
    float    clear_rgba[4];
    unsigned color_write_mask;   /* REPL_RGBA_* bits */
} ReplClearScopedState;
```

plus a running result on the cursor - the observed `rgba[4]` and its
per-channel `known_mask` - which **is not scoped** and must never be restored by
a pop. Keeping the scoped half in its own struct is what stops a future
wholesale save/restore from reverting the result; `ReplAttribSave` carries a
`ReplClearScopedState` beside its existing `ReplRenderState`, and
`repl_exec_restore_attrib_bookkeeping()` (`src/repl/executor.c:316`) restores it
under the same per-group gate it already uses. `CMD_COLOR_MASK` and
`CMD_CLEAR_COLOR` both map to `GL_COLOR_BUFFER_BIT`
(`src/repl/attrib_bits.c:161`), so one saved frame covers both.

Rules:

- Initialize `clear_rgba` from `baseline_clear_rgba`, `color_write_mask` to all
  channels enabled, `known_mask` to zero.
- `CMD_CLEAR_COLOR` updates `clear_rgba`; `CMD_COLOR_MASK` updates
  `color_write_mask`.
- `CMD_CLEAR` containing `GL_COLOR_BUFFER_BIT` copies each *enabled* channel
  from `clear_rgba` into the observed RGBA and marks that channel known.
  Disabled channels keep their previous value and knownness.
- `goto` needs no observation code at all: the cursor already moves the PC, so
  only commands actually reached affect the result.
- A state filter affects the observation exactly as it affects emitted GL. Do
  not update the observation for a command a pass suppresses, unless that pass
  defines a synthetic replacement (hidden-line rendering does).
- At `repl_exec_cursor_end()`, publish `{rgba, known = (known_mask ==
  REPL_RGBA_ALL)}` through `observation_out` when it is non-NULL.

The per-channel `known_mask` is a cursor-internal detail: it exists so that a
partial mask over an earlier full clear still yields a correct fully-known
result. Consumers get the collapsed boolean, so no per-channel consumer can
grow. There is no `color_clear_seen` field - the presentation rule is
all-or-retained and nothing would read it, which is the same rule that deletes
`ReplRenderState.clear_color`.

#### Emission-decision contract

Update the observation where a pass has decided that a clear will actually be
emitted. Neither existing broad helper site is sufficient:

- not `repl_apply_state_bookkeeping()` (`src/repl/executor.c:283`), because
  hidden-line rendering calls it even for clears it skips
  (`src/subsystems/hidden_lines/hidden_lines.c:322`);
- not only `repl_apply_state_cmd()`, because the hidden-line subsystem drives a
  cursor itself and emits `CMD_CLEAR_COLOR` / `CMD_CLEAR` outside that helper.

Expose narrow cursor operations that pair GL emission with observation:

```c
void repl_exec_cursor_emit_clear_color(ReplExecCursor *cursor,
                                       const GLCmd *cmd);
void repl_exec_cursor_emit_clear(ReplExecCursor *cursor,
                                 const GLCmd *cmd,
                                 unsigned write_mask_override);
```

The last parameter is an **override**, not the mask: the sentinel
`REPL_OBSERVE_TRACKED_MASK` means "use the mask the cursor already tracks", and
only a pass that overrides the program's `glColorMask` passes an explicit
value. The normal executor never hands back state the cursor is holding - two
copies of one mask that can disagree is the seam this plan closes.
`repl_exec_cursor_emit_clear()` emits the full mask carried by `CMD_CLEAR`
(depth and stencil bits included) and observes only the color effect.
`CMD_COLOR_MASK` stays inside the cursor step, where its `glColorMask` call and
tracker update are one case; it needs no public entry point.

`hidden_lines_emit_clear()` (`hidden_lines.c:196`) keeps its forced
`glColorMask(TRUE, TRUE, TRUE, TRUE)` bracket but delegates the `glClear` and
the observation to `repl_exec_cursor_emit_clear()` with an all-channel
override. It is reached only from the depth-fill pass, so the hidden and
visible line redraws cannot publish a clear. A forgotten observation is then
impossible without also losing the visible GL emission.

`repl_apply_state_bookkeeping()` is narrowed back to independently consumed
bookkeeping - after this plan, just the light-enable mask. It must not become
an implicit observation hook.

### 3. Controller retention and selection

The controller owns one piece of new state:

```c
/* The last background a program actually established, as observed by the
 * walk that cleared it. Seeded to the configured default; updated only by a
 * fully-known observation, so an unknown frame (no color clear, a masked
 * clear, a replay prefix that has not reached its clear yet) keeps showing
 * the last honest answer instead of snapping to the default. */
static float g_presentation_rgba[4] = { CFG_DEFAULT_CLEAR_R, ... };
```

`scene_execute_adapter()` (`src/app/glr_ctrl.c:1132`) already sits on every
program walk render3d performs, so it is where the frame's observation is
collected. For the authoritative purposes below it points `observation_out` at
a local, and folds the result into two frame-scoped values: the RGBA of the
most recent authoritative publish, and an AND of `known` across all of them.
Every other purpose passes `observation_out = NULL` explicitly.

After `render3d_draw_scene()` returns, the controller:

1. selects this frame's chrome color - the folded RGBA when every authoritative
   pass was fully known, otherwise `g_presentation_rgba`;
2. clears the chrome strips with it (see below);
3. stores it into `g_presentation_rgba` when it was fully known, and otherwise
   leaves the retained value untouched.

A rejected config needs no special contract: no callback runs, nothing
publishes, the fold stays unknown, and chrome uses the retained value.

`glr_ctrl_build_scene_config()` then does what it does today, one line
different: `config->presentation_rgba` is copied from `g_presentation_rgba`
instead of from `repl_flat_resolve_clear_color()`, and the existing Rec. 709
contrast derivation (`src/app/glr_ctrl.c:1556-1563` - luminance, the `+0.02`
black-background guard, the `1..3` clamp) runs on that. **The contrast formula
stays in the controller**; there is no per-pass derivation, so no ownership
moves and `docs/MODULES.md` keeps its current split.

Replay needs no branch of its own. Retention *is* the policy: a prefix that has
not executed an effective color clear publishes `known = 0`, so chrome, fog and
contrast hold their last honest value; when the PC crosses an effective clear,
the observation becomes known and the presentation follows it. Fade batches
suppress `CMD_CLEAR` and publish nothing.

This matters for shipped content. `replay_frame_setup_limit()`
(`src/subsystems/replay/replay_fade.c:163`) keeps a program's clear only when
the run leading up to it holds nothing but comments, empty rows, variable
declarations, and clear-color commands. That test walks the **flat** program,
where `flatten_range()` skips `CMD_FUNC_DEF` bodies outright
(`src/repl/flatten.c:1435`) and the `// camera` header never becomes a command
- so a scene may define functions before its clear and still keep it. Auditing
all 39 scenes against that rule, five have a first clear that an early replay
prefix can omit:

| Scene | First flat command before the clear |
|---|---|
| `aurora-observatory-dish-tracks-sky.glr` | `dishAz = 16*sin(0.22*t) - 12;` |
| `clip-planes-carve-solids.glr` | `glEnable(GL_DEPTH_TEST);` |
| `glr-logo.glr` | `glEnable(GL_DEPTH_TEST);` |
| `lantern_festival.glr` | `rise = clamp(rise, 0.1, 2.0);` |
| `planar-shadows-glmultmatrixf.glr` | `glEnable(GL_DEPTH_TEST);` |

Without retention those five snap chrome, recede fog, and contrast to the
default until replay crosses their clear, regressing the existing no-flicker
policy. Re-run this audit programmatically against `replay_frame_setup_limit()`
rather than by reading source text; an earlier hand audit over-counted by
treating `funcN(...) { }` definitions as flat commands.

### 4. Render config: split the two colors, keep the call surface

`Render3dRenderConfig.clear_color` (`src/render3d/render_types.h:214`) is doing
two jobs under one name. Split it:

```c
float baseline_clear_color[4];  /* GL clear-color state to establish before
                                 * the program walk - what a glClear with no
                                 * preceding glClearColor uses. */
float presentation_rgba[4];     /* the background helpers fade toward */
float alpha_scale;              /* unchanged; derived from presentation_rgba */
```

They must stay separate. `baseline_clear_color` is `CFG_DEFAULT_CLEAR_*` and
never varies: feeding the retained presentation into it would let host
presentation history change the pixels the *program's own* clear writes, on a
scene switch most visibly.

Readers move mechanically and nothing else in `src/render3d/` changes:

| Reader | New field |
|---|---|
| `render3d_apply_clear_color()` (`render.c:883`) | `baseline_clear_color` |
| `set_fog_to_clear_color()` callers (`grid.c:2318`, `:2338`) | `presentation_rgba` |
| `axes_xn_apply_transition_fog()` (`axes.c:746`) | `presentation_rgba` |
| Grid / axes / aurora backdrop alpha (`grid.c:2283`, `axes.c:750`, `backdrop.c:1311`) | `alpha_scale`, unchanged |
| Guide snapshot (`glr_ctrl.c:580`) | `alpha_scale`, unchanged |

`render3d_draw_scene()` keeps its signature, `Render3dExecuteContext` keeps its
single field, and the post-fill / post-overlays / post-resolve-overlays hooks
keep their `void *user_data` shape. `render3d_demo` needs only its three config
assignments updated (`tools/render3d_demo/render3d_demo.c:413-421`); its
`dlsym` hot-reload shim (`:63`, `:147`) is untouched, and the REPL-free link
proof stands unchanged.

### 5. Clear chrome after the scene

Move `glr_ctrl_clear_chrome()` from `src/app/glr_ctrl.c:2718` to immediately
after the `glPopAttrib()` that drops the scene-rect scissor (`:2746`), still
inside `PROF_RENDER3D` (`:2747`) and before `replay_ui_hud_render()`,
`tour_ui_hud_render()`, and the remaining 2D overlays.

The four strips still exclude the scene rectangle, so the move cannot repair,
erase, or change user pixels; it only lets them consume the frame's own
observation instead of a prediction. Chrome stays controller-owned because only
the controller knows the window layout. The intervening consumers are safe:
buffer visualization and the post filter read the scene rectangle only, and the
whole-window compositor runs after the 2D overlays. The load-bearing ordering
rule is just that chrome clears before those overlays paint it.

Keep `glClearDepth(1.0)` before clearing the chrome depth strips, and update
the `PROF_RENDER3D` section description to mention the chrome clear.

### 6. Which passes publish

| View / path | Publishes |
|---|---|
| Normal / plain wireframe | `RENDER3D_EXEC_MAIN_FILL` |
| Winding view | `RENDER3D_EXEC_WINDING` |
| Hidden-line view | depth-fill pass's one synthetic clear |
| Depth probe | no |
| Hidden / visible line redraws | no |
| Replay fade overlays | no; they suppress `CMD_CLEAR` |
| PLY feedback-normal export | no |
| Legacy / test `repl_execute_commands()` | no |
| `execute_fn == NULL` | no walk runs at all |

Every "no" row sets `observation_out = NULL` explicitly - do not rely on
zero-initialization or on nobody reading a discarded result.

Under accumulation, the last authoritative publish of the frame wins and
`known` is ANDed across passes. AA and camera blur observe the same color every
pass. Time blur re-bakes the program per sample, so an animated `glClearColor`
can produce a different color per pass while chrome takes the final one - and
the final blur sample is baked at exactly `t_end`, so that is the honest
frame-end color. Helpers use the retained previous-frame value, as everywhere
else. Weighted background averaging is not part of this plan; add it only if a
real time-blur scene demonstrates a visible need.

`execute_fn == NULL` remains supported: no program walk, no program-owned
clear, and render3d still draws grid, axes, lights, and any configured backdrop
against the caller-supplied presentation inputs. That preserves framebuffer
history rather than pretending a color was painted.

### 7. Remove duplicate and dead state

- delete `repl_flat_resolve_clear_color()` and the resolver-specific goto walk;
- remove its call from `glr_ctrl_build_scene_config()` (`glr_ctrl.c:1404`);
- delete `ReplRenderState.clear_color`, its setter, its reset, and its
  `ReplAttribSave` restoration, if the production-use audit remains empty;
- leave `ReplRenderState.light_enabled_mask` intact - light indicators read it;
- update comments that still describe a static resolver
  (`src/repl/executor.h:53`, `src/repl/state_views.h:118`).

The deletion must not drop the GL-stub-observable attrib coverage that
originally justified the mirror. The cursor's scoped clear state becomes the
observable: assert that a clear color and color mask changed inside
`glPushAttrib(GL_COLOR_BUFFER_BIT)` are restored at the matching pop. Move that
coverage *before* deleting the mirror, so a future audit need not re-litigate.

The GL state inspector (`src/repl/gl_state_inspector.c`) stays a separate
analysis: it answers what `GL_COLOR_CLEAR_VALUE` is *attributed to* at a source
position, not which color an emitted clear wrote into framebuffer channels. It
keeps its own source/attribute fold and clear-color cell, does not consume
`ReplRenderState.clear_color`, and must not publish a presentation background.
Be accurate about what this plan achieves: it removes the second walk that
answers the *framebuffer* question, leaving one. The inspector still models
`CMD_CLEAR_COLOR` (`:1263`), `CMD_COLOR_MASK` (`:1301`), and attribute groups
(`:845`) for its own question, so a new clear-affecting command still touches
it. Verify that boundary in tests and ownership docs.

## Implementation sequence

### Phase 0 - Precursor: source-view and goto lookup (lands standalone)

Independent of everything else and worth landing on its own:

- fix `execution_flat_text()` to honor `SourceTextView.line_count` through
  `source_text_line()`, dropping the global document-count gate;
- factor goto target lookup so there is one implementation for cursor use;
- add the regression: a temporary `SourceTextView` resolves `goto` without
  synchronizing global document count.

### Phase 1 - Cursor observation

Write these regressions first; land them with this phase.

1. Full-disabled color mask followed by a color clear does not produce a known
   background.
2. Partial mask after an earlier full clear updates only the enabled channels
   and preserves the known values of the rest.
3. Partial mask with no earlier known clear leaves the result unknown.
4. `GL_COLOR_BUFFER_BIT` push/pop scopes both clear color and color mask, and
   the running observation survives the pop unchanged.
5. Multiple clears retain the result of the final effective channel writes.
6. Forward and backward `goto` take exactly the cursor's path and terminate
   under its existing loop budget.
7. A suppressed state command updates neither GL nor the observation; the
   shared emit-and-observe helpers do both or neither.

Then:

- add `ReplBackgroundObservation`, the scoped/running cursor state, and the
  `observation_out` / `baseline_clear_rgba` options;
- scope the clear state through the existing attribute frames;
- add the emit-and-observe clear operations and route both the normal cursor
  and `hidden_lines_emit_clear()` through them;
- pass `observation_out = NULL` explicitly from PLY feedback export and the
  legacy/test `repl_execute_commands()` entry point.

Prefer differential tests around `ReplExecCursor`: feed one flat program to the
real cursor and assert the observation produced by the commands it visits. Do
not replace resolver unit tests with another standalone analyzer suite.

### Phase 2 - Controller retention, config split, chrome ordering

- collect the frame observation in `scene_execute_adapter()` for the three
  authoritative purposes; NULL everywhere else;
- add `g_presentation_rgba`, seeded to `CFG_DEFAULT_CLEAR_*`, updated only by a
  fully-known frame;
- split `clear_color` into `baseline_clear_color` + `presentation_rgba` and
  move the five readers;
- feed `presentation_rgba` and the existing contrast derivation from the
  retained value; delete the resolver call;
- move the chrome-strip clear after the scene-scissor pop and give it the
  frame's selected color;
- update `render3d_demo`'s three config assignments.

### Phase 3 - Delete the parallel model

- remove `repl_flat_resolve_clear_color()`, its goto walk, and its tests;
- remove `ReplRenderState.clear_color` if the final audit confirms it is
  consumer-free, after moving its attrib coverage onto the cursor;
- simplify the auxiliary-pass capture/restore struct and its tests;
- update architecture / module / contributing documentation.

## Test plan

### Focused suites

- `test_repl_executor` - the seven Phase 1 regressions, plus publication
  through `observation_out` and its absence when NULL.
- `test_glr_ctrl`
  - chrome clears immediately after the scene-scissor pop, within
    `PROF_RENDER3D` and before the replay/tour HUDs;
  - chrome uses this frame's observation; fog and `alpha_scale` use the
    retained one (assert the deliberate one-frame lag rather than working
    around it);
  - retention updates only on a fully-known frame and survives a rejected
    render unchanged;
  - a replay prefix shaped like each of the five audited scenes holds the
    retained presentation, then follows an effective clear once executed;
  - `test_display_frame_clear_color_before_clear_applies` and
    `..._after_clear_is_ignored` (`tests/test_glr_ctrl.c:505`, `:535`) migrate
    to two rendered frames, and the assertions at `:430`, `:557-560`, `:3849`
    move onto `presentation_rgba` / the retained scale;
  - the execute, post-fill, post-overlays and post-resolve-overlays callback
    signatures are unchanged.
- render3d tests / demo
  - `baseline_clear_color` establishes GL state; `presentation_rgba` drives
    grid/axes fog; the two may differ;
  - `execute_fn == NULL` renders helpers against the supplied presentation
    without clearing the scene;
  - no REPL link dependency; ordinary and hot-reload demo builds unchanged.
- `test_hidden_lines` - the synthetic depth-fill clear reports its forced
  full-mask background; later passes cannot overwrite it.
- `test_repl_export_clearcolor` - source order unchanged, bootstrap default
  still in `init()`, no clear-color hoist returns.
- `test_export_trace_parity` - the REPL execution trace stays aligned with
  exported C.
- `test_repl_core_examples` - authored examples keep `glClearColor` before
  their intended clear.

### Pixel verification

Two OSMesa cases carry the visual risk the no-op GL stubs cannot prove:

- a color clear with all four channels masked off (chrome must not take the
  clear color);
- no color clear at all (the scene rect smears; the host does not clear it).

Compare a scene-background pixel and a chrome-strip pixel. Keep the fixtures
minimal and deterministic; the remaining cases are covered by stub-level
observation tests.

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

For portability-sensitive cursor changes, run the documented real-GCC lane on
`gracemont` after the branch is pushed to a reviewable ref.

## Documentation updates

- `docs/ARCHITECTURE.md` - user execution observes the background it clears;
  the controller retains it; chrome clears after the scene with the current
  frame's observation while fog and contrast use the retained one; unknown
  backgrounds keep the last known presentation, which is also what makes replay
  prefixes stable.
- `src/repl/ARCHITECTURE.md` - cursor observation is part of the real execution
  walk; no static background resolver; drop the `ReplRenderState.clear_color`
  row (`:753`).
- `docs/MODULES.md` - executor owns command semantics and the observation;
  controller owns chrome layout, retention, and the contrast derivation
  (unchanged); render3d receives a baseline and a presentation color as plain
  inputs.
- `docs/CONTRIBUTING.md` - a new state command that affects clearing updates
  cursor execution once and must not create an analyzer-side execution path.
- `src/render3d/README.md` - the two-color config contract for standalone
  callers.
- `src/render3d/render.c` - update the accumulation comment that says chrome
  was cleared before the passes and accumulates at the same weight.
- `CLAUDE.md:659` - the `glClearColor` precedes `glClear` note still points at
  `repl_flat_resolve_clear_color()`.

## Acceptance criteria

- Live REPL and exported C retain source-ordered `glClearColor` / `glClear`
  behavior.
- No flat-program clear-color prediction walk remains in the controller,
  render3d, or executor support code.
- `CMD_COLOR_MASK`, attrib scopes, multiple clears, and `goto` affect the
  presentation observation through the same cursor that emits GL.
- Clear-color and clear emission cannot occur through a specialized cursor path
  without the paired observation update.
- An unknown or partially known background is never turned into a supposedly
  exact RGBA value; consumers fall back to the retained presentation as a
  whole.
- The host never clears the scene rectangle when the source did not.
- The five audited non-leading-clear scenes hold a stable presentation through
  an early replay prefix and follow a later effective clear when it executes.
- `src/render3d/` remains REPL-free and its public call surface -
  `render3d_draw_scene()`, `execute_fn`, and the three overlay hooks - is
  unchanged.
- No production consumer remains for `ReplRenderState.clear_color`; the field
  is removed, or the discovered consumer and its invariant are documented
  before implementation proceeds.
- Focused tests, stubbed full tests, C99/ownership guards, native build, and
  the two OSMesa pixel checks pass.

## Settled review decisions

1. **Vintage:** chrome uses the frame's own observation (the chrome clear
   moves after the scene); grid/axes fog, overlay contrast, and the cursor edit
   guides use the retained previous-frame presentation. Same-frame delivery to
   the in-pass helpers was rejected: it requires a render-boundary result type,
   per-pass context mutation, a frame-result return with a
   `render3d_draw_scene()` signature change, and a per-pass time-blur story, all
   to move a fog color 16 ms earlier.
2. **Unknown background:** all-or-retained. An observation is used only when
   all four channels are known; otherwise every consumer keeps the last known
   presentation. No per-channel composition, no `glReadPixels`.
3. **Replay:** no dedicated policy. Retention already holds the presentation
   steady through a prefix that has not cleared, and releases it when an
   effective clear executes. Fade batches publish nothing.
4. **Retention lifetime:** one global `float[4]`, seeded to the configured
   default, with no reset or scene-load invalidation - invalidating would snap
   to the default for exactly the frame the value is least trustworthy.
5. **Published shape:** RGBA plus one `known` boolean. The per-channel mask
   stays inside the cursor, and no `color_clear_seen` field is added: a
   consumer-free field is not made acceptable by labelling it reserved, which
   is the same rule that deletes `ReplRenderState.clear_color`.
6. **Accumulation:** the last authoritative pass's color, gated on every pass
   being fully known. No weighted background averaging in v1.
7. **Compatibility:** no internal API shims; intermediate phases 1-3 need not
   be separately releasable. Phase 0 is independent and lands first.
