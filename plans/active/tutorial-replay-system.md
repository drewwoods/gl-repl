# Tutorial replay system design

## Question

> in src/immediate-mode-repl/claude4.6-opus-thinking i would like to design a global state capture system to implement tutorials by replaying state to demonstrate certain features.  Assume repl-state-phase2-sketch.md is completed, how would you do this and how much effort would be involved? 

## Response

Yes — once Phase 2 is actually in place, this becomes pretty doable.

The clean way is **not** to dump all globals wholesale. I would build a **typed snapshot/apply layer** on top of the Phase 2 state boundary, then a tutorial player on top of that.

Why I’d do it that way:

* Your Phase 2 contract already turns runtime state into named domains behind `repl_state.h`, instead of raw `extern g_*` access, which is exactly the seam a capture system wants: [`feature/repl-state-phase2-sketch.md`](https://github.com/drewwoods/OpenGL-vibe/blob/main/src/immediate-mode-repl/claude4.6-opus-thinking/feature/repl-state-phase2-sketch.md), [`repl_state.h`](https://github.com/drewwoods/OpenGL-vibe/blob/main/src/immediate-mode-repl/claude4.6-opus-thinking/repl_state.h)
* The current replay system is already good at **geometry/program replay** from the flat program, with PC, mode, source-line mapping, fade batches, and baseline predef restoration — but it is not a full app/tutorial state system by itself: [`repl_replay.c`](https://github.com/drewwoods/OpenGL-vibe/blob/main/src/immediate-mode-repl/claude4.6-opus-thinking/repl_replay.c)
* The architecture is already split into the right ownership buckets: state, editor, replay, render, code panel, export/import, examples, actions. That means you can add a tutorial layer without shoving more policy back into `repl_core.c`: [`ARCHITECTURE.md`](https://github.com/drewwoods/OpenGL-vibe/blob/main/src/immediate-mode-repl/claude4.6-opus-thinking/ARCHITECTURE.md)

What I would build

### 1. A serializable tutorial snapshot type

Not `memcpy(ReplRuntimeState)`. Too brittle.

I’d make a separate DTO-like type, something like:

```c
typedef struct ReplTutorialSnapshot {
    uint32_t version;
    uint32_t present_mask;

    ReplTutorialDocumentSnapshot    document;
    ReplTutorialVariablesSnapshot   variables;
    ReplTutorialEditorSnapshot      editor;
    ReplTutorialUiSnapshot          ui;
    ReplTutorialCameraSnapshot      camera;
    ReplTutorialPresentationSnapshot presentation;
    ReplTutorialRenderSnapshot      render;
    ReplTutorialReplaySnapshot      replay;
} ReplTutorialSnapshot;
```

The key point is: **capture stable values, not runtime internals**.

### 2. Be strict about what is captured vs derived

I would capture:

* document source state
* predefined variables / anim time / t-playing
* edit line, input text, cursor position, insert mode
* code panel scroll / help visible / var panel visible when needed
* camera
* presentation toggles
* render settings that are user-visible
* replay position/mode only if a step needs to resume mid-replay

I would **not** capture as persistent data:

* raw GL resources (`GLUquadric*`, `GLUtesselator*`)
* flat program buffers
* current block derived markers
* focus vertex derived state
* fade batch internals
* mouse drag momentum / pointer-down transient state
* hover/open-menu/transient popup geometry unless a tutorial explicitly needs it
* undo/redo rings

Those should be rebuilt or reset during apply.

That matches the direction in the Phase 2 design: `repl_state.h` is for runtime contexts and focused resets, while flat/executor data and many ephemeral/module-private things are not supposed to become long-term persistence boundaries: [`feature/repl-state-phase2-sketch.md`](https://github.com/drewwoods/OpenGL-vibe/blob/main/src/immediate-mode-repl/claude4.6-opus-thinking/feature/repl-state-phase2-sketch.md)

### 3. For the document, store canonical source lines, not raw `GLCmd` blobs

This is the most important design choice.

I would store the tutorial’s code as canonical source text lines, not serialized `GLCmd` memory. Then apply it through the same parser/commit/store path you already trust.

Something like:

```c
typedef struct ReplTutorialDocumentSnapshot {
    int   line_count;
    char  lines[MAX_COMMANDS][MAX_LINE_LEN];
    int   edit_line_idx;
} ReplTutorialDocumentSnapshot;
```

Why:

* much less fragile than raw binary structs
* easier to diff/review in git
* matches how your import/export and examples already think
* keeps parser/normalization as the authority

I would then rebuild flattened state from source after apply, rather than persisting `g_flat_cmds`.

### 4. Add a capture/apply API per domain

I’d make a new module pair:

* `repl_tutorial_state.h`
* `repl_tutorial_state.c`

With APIs like:

```c
void repl_tutorial_capture_snapshot(ReplTutorialSnapshot *out,
                                    const ReplTutorialCaptureOptions *opts);

int repl_tutorial_apply_snapshot(const ReplTutorialSnapshot *snap,
                                 const ReplTutorialApplyOptions *opts);
```

Internally that fans out into domain helpers:

```c
static void capture_document(...);
static void apply_document(...);
static void capture_camera(...);
static void apply_camera(...);
...
```

This keeps tutorial policy out of `repl_state.c`, while still consuming `repl_state_*()` accessors.

### 5. Apply in a fixed reset order

Order matters. I’d do:

1. stop replay
2. clear transient UI/drag/search/autocomplete state
3. reset document + variables + editor
4. load source commands through command-store / parser path
5. restore variable values and time
6. restore editor selection/cursor/input
7. restore camera/presentation/render settings
8. restore optional UI state
9. mark flat/normals dirty
10. flatten/rebuild derived state
11. if requested, restore replay PC/mode against the rebuilt flat program

That lines up with the explicit reset-oriented Phase 2 contract.

### 6. Put a tutorial script layer above snapshots

A snapshot system alone is not enough. For tutorials, I’d make **steps**.

```c
typedef enum {
    TUTORIAL_STEP_APPLY_SNAPSHOT,
    TUTORIAL_STEP_REPLAY_TO_LINE,
    TUTORIAL_STEP_REPLAY_TO_PC,
    TUTORIAL_STEP_SHOW_CALLOUT,
    TUTORIAL_STEP_WAIT_FOR_KEY,
    TUTORIAL_STEP_TWEEN_CAMERA,
    TUTORIAL_STEP_SET_CURSOR_LINE,
    TUTORIAL_STEP_TOGGLE_CFG,
} ReplTutorialStepType;
```

Then a tutorial is:

```c
typedef struct ReplTutorialStep {
    ReplTutorialStepType type;
    ...
} ReplTutorialStep;

typedef struct ReplTutorial {
    const char *id;
    const char *title;
    ReplTutorialSnapshot base;
    const ReplTutorialStep *steps;
    int step_count;
} ReplTutorial;
```

That gives you two big wins:

* sparse checkpoints instead of recording every frame
* semantic playback instead of brittle global-state movie recording

So a tutorial can say:

* load base snapshot
* put cursor on line 12
* show callout “this `glBegin` opens the primitive”
* start replay in vertex mode
* advance to next vertex
* tween camera slightly
* toggle outlines
* continue replay

That composes with your existing replay instead of replacing it.

### 7. Reuse existing replay, do not replace it

Your replay system already handles:

* flat-program stepping
* vertex vs polygon mode
* source-line linkage
* fade batches
* baseline predefined-variable restore

That is already a good “geometry explainer” engine. I’d keep it.

The tutorial system should mostly do two things around it:

* load/apply the right snapshot before the replay segment
* drive replay commands in a scripted way

So tutorial playback becomes a thin orchestrator around `repl_replay.c`, not a second replay engine.

### 8. Keep authoring simple: full checkpoints first, deltas later

Do not start with fancy snapshot-delta compression.

First version:

* full base snapshot
* full snapshot at major chapter boundaries
* semantic steps between them

Later, if needed, add delta snapshots or shared interned strings.

That will get you shipping much faster.

Where I would draw the boundaries

I’d split it like this:

* `repl_tutorial_state.*`

  * capture/apply typed snapshots
* `repl_tutorial_format.*`

  * save/load tutorial assets
* `repl_tutorial_player.*`

  * state machine for current step, next/back, autoplay, wait states
* `ui_tutorial_overlay.*`

  * callouts, progress, captions, highlighted line/region
* `repl_tutorial_authoring.*` later, only if you want recording tools

How much effort

Assuming the Phase 2 state boundary is really finished and reasonably clean:

### MVP

Capture/apply + one hardcoded tutorial player + reuse existing replay

About **4 to 7 working days**

That would give you:

* typed snapshot capture/apply
* a small built-in tutorial format in C data
* next/back tutorial steps
* snapshot load + replay orchestration
* minimal tests

### Good production version

File-backed tutorials, step types, callouts, robust resets, tests

About **2 to 3 weeks**

That would include:

* stable snapshot schema with versioning
* tutorial step interpreter
* overlay UI
* good reset/apply semantics
* tests around capture/apply determinism
* a few example tutorials

### Polished system

Authoring workflow, recorder helpers, more UI automation, migration/versioning

About **4 to 6 weeks**

That is where it starts to feel like a real feature instead of an internal facility.

What will cost you the most

The hard part is not the snapshot struct. It is deciding **what counts as authoritative state**.

The likely traps are:

* serializing too much transient state
* trying to persist GL/resource pointers
* depending on module-private caches/fade arrays/menu hover state
* using raw `GLCmd` memory instead of stable textual or typed value snapshots
* letting tutorial playback mutate state through ad hoc paths instead of one apply boundary

My recommendation

I’d do this in three passes:

1. **Snapshot/apply only**

   * document, variables, editor, camera, presentation, replay-lite
2. **Tutorial player**

   * step list + overlay + replay integration
3. **Authoring helpers**

   * capture current state into a tutorial checkpoint

If you want the safest version, I’d make the first tutorial asset format entirely code-defined, not external text. Once it feels right, then add disk format/import/export.

My confidence is medium-high. The architecture you already have is close to the shape needed for this, and the Phase 2 state split is the key enabling move. The main thing I would avoid is turning “global state capture” into “serialize every global.” That would work briefly and then punish you.
