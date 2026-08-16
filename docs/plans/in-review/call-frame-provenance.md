## Call-Frame Provenance - debugging deep call chains and recursion

## Status - NOT STARTED (exploration)

Motivated by two scenes that are currently hard to reason about:

- [`tests/scenes/stress/deep-call-chain-shadowing-stress.glr`](../../../tests/scenes/stress/deep-call-chain-shadowing-stress.glr)
  - `func0 -> func1 -> func2 -> func3 -> func4`, five levels, driven from a
  5x5 nested loop. 754 flat commands, 125 dynamic invocations.
- `new_scene.glr` (untracked, Sierpinski subdivision) - `divide_triangle`
  recurses into itself from four distinct call sites and bottoms out in
  `triangle`. 525 flat commands, 37 dynamic invocations, recursion depth 4.

Both render fine. Neither can be *inspected*: the flat program cannot say
which invocation a given command came from.

### The problem, measured

`--dump-flat` prints the whole provenance record. Here are three of the
sixteen `glVertex2f` rows the Sierpinski scene emits from `triangle`:

```
  51 | CMD_VERTEX2F | src_idx=18 call_src_idx=59 root_call_src_idx=114 depth=4 func_scope=0x00000003 | args=2[0, 1]
  79 | CMD_VERTEX2F | src_idx=18 call_src_idx=59 root_call_src_idx=114 depth=4 func_scope=0x00000003 | args=2[-0.25, 0.75]
 107 | CMD_VERTEX2F | src_idx=18 call_src_idx=59 root_call_src_idx=114 depth=4 func_scope=0x00000003 | args=2[0.25, 0.75]
```

Every provenance field is identical across all sixteen. The same holds for
the deep-chain scene, where all 25 `glutSolidSphere` rows report
`call_src_idx=20 root_call_src_idx=67 depth=5 func_scope=0x1f`.

Why each field fails:

| Field | What it holds | Why it can't identify the invocation |
|---|---|---|
| `src_cmd_idx` | the body line | one line, N executions |
| `call_src_cmd_idx` | *immediate* call site | recursion re-enters through the same line; a loop calls the same line N times |
| `root_call_src_cmd_idx` | *outermost* call site | the whole point of a deep chain is that rungs 2..N-1 are the interesting ones, and they are not recorded |
| `func_scope_mask` | **set** of active func slots | a set cannot count repeated entries - `divide_triangle` at depth 1, 2 and 3 all contribute the same single bit |
| `call_depth` | frame count | counts, does not identify: four sibling invocations share a depth |

So the record keeps the first rung and the last rung of the ladder and the
number of rungs, and drops the ladder.

**There is a structural asymmetry worth naming.** Flatten already maintains
a *dynamic* loop stack that survives call boundaries and snapshots its
innermost entries into every flat command
([`FlatCmdActiveLoop`](../../../src/repl/flatten.h#L41), with an explicit
comment saying replay needs caller iterator values while showing commands
several calls deeper). It maintains a *dynamic* call stack too -
`ctx->call_depth` plus the C recursion of `flatten_call` - and throws all of
it away except the depth. Loop ancestry is recoverable per command; call
ancestry is not. The whole of this plan is closing that asymmetry.

A second consequence, easy to miss: **an ancestor frame's argument values are
unrecoverable.** Scope in flatten is deliberately lexical - `flatten_call`
builds a fresh `lvars[]` of the callee's params and locals and copies nothing
of the caller's - so a flat command's `FlatCmdLocalVars` describes exactly one
frame, its own. Standing on a `glVertex2f` inside `triangle`, there is no way
to ask "what `depth` and `type` was the `divide_triangle` two frames up
called with?", which is the first question anyone debugging this scene has.

### What the flat program already knows

One property does all the heavy lifting and is worth stating explicitly,
because nothing in the tree records it today:

> **A call frame's flat commands occupy a contiguous half-open interval.**
> Flatten is depth-first and append-only (`ctx->flat_count++` is the only
> way a command enters the stream), so a frame's own commands and its whole
> subtree of descendants form one run `[begin, end)`.

That means a frame is *addressable as a range*, which is the same shape the
replay clamp already consumes, and it means subtree queries ("how much of
the flat budget did this invocation cost?", "render only this call") are
O(1) once the range is recorded.

It also means the call tree is *almost* derivable from what is already
stored. A stack-machine scan over the flat stream that opens a frame when
`call_depth` rises and closes it when it falls reconstructs the tree
exactly - for both target scenes.

I prototyped this as an awk pass over `--dump-flat`. Sierpinski, verbatim
(trimmed to the first subtree):

```
call site line 114  (depth 1, first flat 7)          <- divide_triangle(0,1,-1,0,1,0,2,0)
  call site line 96  (depth 2, first flat 21)
    call site line 96  (depth 3, first flat 35)
      call site line 59  (depth 4, first flat 45)    <- triangle(...)
    call site line 97  (depth 3, first flat 63)
      call site line 59  (depth 4, first flat 73)
    call site line 98  (depth 3, first flat 91)
      call site line 59  (depth 4, first flat 101)
    call site line 99  (depth 3, first flat 119)
      call site line 59  (depth 4, first flat 129)
  call site line 97  (depth 2, first flat 147)
  ...
reconstructed frames: 37
```

37 frames, which is exactly right (1 + 4 + 16 `divide_triangle` invocations,
plus 16 `triangle`). The deep-chain scene reconstructs to 125, also exact.

**Its one failure mode is precise and worth stating**, because it is the
thing a real frame identity buys. Two sibling invocations from the *same*
call site with nothing emitted between them are indistinguishable. Minimal
repro:

```
func0(x) { glVertex3f(x, 0, 0); }
for(i, 0, 4) { func0(i); }
```

```
   2 | CMD_VERTEX3F | src_idx=1 call_src_idx=7 root_call_src_idx=7 depth=1 func_scope=0x1 | args=3[0, 0, 0]
   3 | CMD_VERTEX3F | src_idx=1 call_src_idx=7 root_call_src_idx=7 depth=1 func_scope=0x1 | args=3[1, 0, 0]
   4 | CMD_VERTEX3F | src_idx=1 call_src_idx=7 root_call_src_idx=7 depth=1 func_scope=0x1 | args=3[2, 0, 0]
   5 | CMD_VERTEX3F | src_idx=1 call_src_idx=7 root_call_src_idx=7 depth=1 func_scope=0x1 | args=3[3, 0, 0]
```

Four invocations, four byte-identical provenance records; the scan merges
them into one frame. (A human can separate them by the baked `args`, and the
loop iterator *is* recoverable from `active_loops[]` - but that is the loop,
not the frame. A function called twice in a row outside a loop has nothing
at all to separate it.) Neither target scene hits this, because
`divide_triangle`'s four recursive calls sit on four different lines and the
deep-chain loop body emits `glPushMatrix`/`glTranslatef` between calls.

### Options

#### A - Derive the tree from existing fields (zero storage)

The prototype above, moved into C as a `--call-tree` dump and a query helper
in `flatten_query.c`.

- **Cost**: ~150 lines, no new storage, no change to flatten, no change to
  `GLCmd`, no risk to the rebake/dep-routing model.
- **Buys**: an offline call tree for both target scenes today; per-invocation
  flat-budget accounting; enough to validate whether the *surfaces* below are
  worth building.
- **Cannot**: separate adjacent same-site siblings; recover ancestor
  argument values; recover call sites for a frame that emitted nothing of
  its own (a jump of more than one depth level leaves a `?` rung).
- **Verdict**: cannot back the PATH annotation - that surface is defined by
  ancestor arguments, which this option structurally cannot supply. Keep it
  as an offline `--call-tree` dump only, and only if the dump is wanted
  before option B lands.

#### B - Interned call-frame table (recommended core)

Give each dynamic invocation an identity. `flatten_call` already holds
everything needed at the moment it has it.

```c
/* One dynamic funcN invocation. Interned by flatten_call; a flat command
 * names its innermost frame and the parent chain reconstructs the stack. */
typedef struct {
    int   parent;               /* enclosing frame, REPL_CALL_FRAME_NONE at top */
    int   call_src_cmd_idx;     /* the funcN(...) row that opened this frame */
    int   func_slot;            /* 0..REPL_FUNC_SLOT_COUNT-1 */
    int   depth;                /* == parent->depth + 1 */
    int   flat_begin, flat_end; /* the frame's contiguous subtree range */
    int   arg_count;            /* params bound; > ARG_MAX means truncated */
    float args[REPL_CALL_FRAME_ARG_MAX];
} ReplCallFrame;
```

and one field on `GLCmd`: `int call_frame_idx`.

Sizing, against what the flat program already costs
(`sizeof(GLCmd)` = 208 B, `sizeof(FlatCmdLocalVars)` = 1032 B, so
`ReplFlatProgramState` is already ~9.7 MB):

| | bytes |
|---|---|
| `call_frame_idx` x `MAX_FLAT_COMMANDS` | 32 KB |
| frame table, 4096 frames x 92 B (`ARG_MAX` = 16) | 377 KB |
| **total** | **~0.4 MB, ~4% of the flat program** |

Design notes:

- **Parameter *names* are not stored.** `func_slot` locates the
  `CMD_FUNC_DEF` row and `parse_repl_func_signature()` re-derives them, the
  same way `flatten_call` gets them. Values must be stored, though: locating
  them positionally in a descendant's `FlatCmdLocalVars` does not work
  because `flatten_for_loop` *prepends* its iterator, and a name lookup is
  not bulletproof either because an inner loop iterator may legally shadow a
  param.
- **Capacity is a soft limit.** Past `MAX_CALL_FRAMES`, interning stops and
  further commands carry `REPL_CALL_FRAME_NONE`; consumers fall back to
  today's display. A scene must never fail to flatten because the debug
  table filled.
- **The in-place rebake is safe by an argument that already exists.**
  `flatten_call` classifies call-argument deps as *structural* ("call-argument
  values freeze into per-flat-command local snapshots"), so anything that
  could change a frame's `args[]` already forces a full flatten. The frame
  table stays correct for exactly the reason `FlatCmdLocalVars` does.
- **It is a net simplification, not just an addition.** `call_frame_idx`
  *subsumes* three existing fields: `call_src_cmd_idx` is `frame.call_src_cmd_idx`,
  `root_call_src_cmd_idx` is the chain's root frame's call site, `call_depth`
  is `frame.depth`, and `func_scope_mask` is the OR of `1 << func_slot` up
  the chain. Add the field first, migrate consumers
  (`gl_state_inspector.c`, `edit_overlays.c`, `flatten_query.c`,
  `replay_annotations.c`, `glr_ctrl.c`) behind inline accessors, then retire
  the three - which nets **-12 bytes** on `GLCmd`, i.e. -98 KB, paying for
  most of the frame table. A drift test asserting the derived values equal
  the stored ones is the safe way to do the migration, and mirrors the
  existing `test_replay_walk` predicate-agreement test.

#### C - On-demand re-flatten probe

No storage at all: to answer "what is the stack at flat index N", re-run
`repl_flatten_program` with a `stop_at_flat_idx` and read `ctx`'s live call
stack when it trips.

- **Buys**: exactness at zero steady-state cost, and it is the only option
  that could also report frames that emitted nothing.
- **Costs**: a full flatten per query, so it cannot back an always-on
  code-panel breadcrumb during playback; and the flatten walk *applies
  assignments to the live predef/scratch tables as it goes*, so a probe needs
  the same baseline save/restore replay's seek path already does. That is a
  real correctness hazard for a debug feature.
- **Verdict**: not the core mechanism, but a good oracle for a test that
  validates option B's table.

#### D - Rejected

- **Path hash / breadcrumb string per flat command.** A hash is not
  invertible and a string is expensive per command; the readable breadcrumb
  is something you *render from* B, not something you store.
- **Widening `func_scope_mask` to a counter per slot.** Fixes recursion
  depth only, still cannot order the chain or separate siblings, and burns
  the mask's cheap set semantics that four call sites already depend on.

### The surface: a replay PATH annotation on the emitted vertex

This is the target the whole plan exists to serve, and it is deliberately
narrow. **No standalone stack panel, no runtime executor stack.** The
breadcrumb is one replay annotation row attached to the source line the
program counter is on, answering exactly one question about exactly one
emitted vertex: *how did we get here*.

#### Shape

A third annotation kind, `REPL_REPLAY_ANNOTATION_KIND_PATH`, rendered
immediately below the focused source row and **before** the existing SUBST
and EVAL rows. Sierpinski, the first vertex of the first triangle (flat
index 51 - every value below is the real one, cross-checked against
`--dump-flat`):

```
  16 |       x = x0 * cos(twistDeg * d) - y0 * sin(twistDeg * d);
  17 |       y = x0 * sin(twistDeg * d) + y0 * cos(twistDeg * d);
  18 | >     glVertex2f(x, y);
     |  |-> divide_triangle(0, 1, -1, 0, 1, 0, 2, 0) @114
     |    > divide_triangle(0, 1, -0.5, 0.5, 0.5, 0.5, 1, 3) @96
     |    > divide_triangle(0, 1, -0.25, 0.75, 0.25, 0.75, 0, 3) @96
     |    > triangle(0, 1, -0.25, 0.75, 0.25, 0.75, 3) @59
     |  glVertex2f(0, 1)                                          <- SUBST
```

The two `@96` rungs are the payoff: they are the same source line, entered
twice at different depths with different arguments, and *nothing in the flat
record today can tell them apart*. Note also that the outermost frame's
arguments are literals from the source, but every inner frame's are computed
- `A[0]` resolved to 3, `v0_x` to -0.5 then -0.25 - and none of them appear
anywhere in the current annotation.

The deep-chain scene is the other shape: five distinct lines, no repeats,
and loop ancestry doing real work (first sphere, `u = v = -2`, values again
verified against the dump):

```
   9 | >     glutSolidSphere(0.18, 8, 8);
     |  |-> u=-2 > v=-2 > func0(-2, -0.378, -2) @67 > func1(-2.2, -0.416, -2.2) @53
     |      > func2(-2.2, -0.416, -2.2) @42 > func3(-2.2, -0.416, -2.2) @31
     |      > func4(-1.98, -0.375, -1.98) @20
```

**One logical row**, wrapped by the panel's existing row-wrapping, not N
separate annotations - so scroll-follow accounting stays a single entry.
Sierpinski is drawn stacked above only because each rung is long; the same
row model produces both, and the renderer decides.

#### The values must be historical, not re-derived

This is the load-bearing constraint and it is what forces option B over
anything cheaper. The existing SUBST annotation calls
`replay_load_runtime_state_for()` and re-evaluates the line against restored
predef/scratch state. **That cannot work for an ancestor frame**: scope in
flatten is lexical, the caller's bindings were never copied into the callee,
and by the time the PC reaches the vertex the caller's locals are gone. The
arguments have to be the values captured *at call entry, during flatten*, and
that means storing them in the frame record. So:

- `args[]` in `ReplCallFrame` is **required**, not an optimization. The
  `first_own_flat_idx` trick floated under option B is dead for this surface.
- Parameter *names* stay derived - `func_slot` locates the `CMD_FUNC_DEF`,
  `parse_repl_func_signature()` reads the signature. Rendering
  `divide_triangle(depth=1, type=3)` instead of positionally costs no
  storage.
- The same rebake argument still holds: call-argument deps are already
  classified structural, so a value-only rebake can never leave `args[]`
  stale.

Loop ancestry is the one part already solved: `FlatCmdActiveLoop
active_loops[]` is snapshotted per flat command *and* survives call
boundaries by design, so `u=-2 > v=-2` needs no new storage at all - just
prepending the existing innermost-last array to the frame chain.

#### Scope for the first cut

- **Vertex replay mode only.** In polygon mode one step can cover a whole
  primitive, and a primitive's vertices can come from different frames -
  `for(i,0,4){ func0(i); }` inside a `glBegin` block is exactly that, and is
  the same minimal repro that breaks the zero-storage reconstruction. Showing
  one path there would be a lie. **Suppress the row in polygon mode**; the
  obvious later refinement is to show the longest common prefix of the step's
  frames and mark where they diverge, but that is a separate decision.
- **Verbose expansion only.** `replay_annotations_refresh_output()` documents
  the invariant that virtual rows belong to Verbose alone and Expanded keeps
  every readout inline on the source row. PATH is a virtual row, so it is
  Verbose's. Do not add a fourth `e` mode for it.
- `REPL_REPLAY_ANNOTATION_MAX` goes 2 -> 3.
- The existing `HIGHLIGHT_REPLAY_CALL_SITE` / `_ROOT_CALL_SITE` gutter
  markers **stay as they are**. They are cheap and they point at real lines;
  the breadcrumb just becomes the authoritative account. Generalizing them to
  N rungs is a later, optional change and should not ride along.

#### Two traps worth writing down now

1. **`@114` is not `src_cmd_idx + 1`.** The code panel's derived-C chrome
   rows shift the visible gutter label, and
   `ui_repl_code_panel_gutter_labels_for_lines()` exists precisely because of
   it, with a comment saying any UI quoting a line number to the user must
   resolve it there. A breadcrumb printing `source_line_idx + 1` will be
   subtly wrong for every scene with chrome visible. The resolver is batched,
   which suits a whole chain resolved at once.
2. **Long recursion needs middle-elision, not truncation.** `MAX_VIRTUAL_LINE_TEXT`
   is 256 and `MAX_FLATTEN_CALL_DEPTH` is 64, so a deep chain does not fit.
   Keep the outermost rung and the innermost two or three, elide the middle
   with a count - the ends carry the information, the middle is the part a
   reader skims:

   ```
   |-> divide_triangle(depth=12) @124 > ... 9 frames ... > depth=1 @104 > triangle @65
   ```

#### One more corroboration that the gap is real

`replay_flat_cmd_context_matches()` in `replay_annotations.c` already tries
to identify an invocation and has to do it with a four-field conjunction -
`func_scope_mask && call_depth && call_src_cmd_idx && root_call_src_cmd_idx`
- with a comment conceding that repeated calls at one source site are
disambiguated *temporally*, by `find_replay_assignment_flat_cmd`'s backward
scan, because the provenance cannot do it. `call_frame_idx` replaces that
conjunction plus its temporal fallback with one integer comparison. The
codebase has already paid for this gap once.

### Later surfaces (explicitly not in the first cut)

Kept for sequencing only. None of these should ride along with the PATH row.

1. **Colour geometry by call depth.** An overlay mode that tints each draw by
   `call_depth`. Needs **no new provenance at all** - the field is already on
   `GLCmd` - and it makes recursion structure visible spatially. The cheapest
   genuinely useful thing on the list, and independent enough to ship first
   or never.
2. **Offline `--call-tree` dump.** Also turns `--flat-histogram` from
   per-*function* into per-*invocation* accounting, which is what you want
   when a scene approaches the 8192 flat budget.
3. **Debugger verbs on replay.** Frames-as-ranges makes *step out* (seek to
   `frame.flat_end`), *step over* and *run to frame* one-liners over replay's
   existing clamped execution.
4. **Frame isolate.** Render one invocation's subtree - a range clamp.
5. **Click geometry, get the stack.** The inverse-pick direction, using
   `repl_find_affecting_transforms_for_flat_vertex()`'s plumbing and the
   assign-plot right-click hit model.

### Adjacent work

- **`console(...)`** - a `label()`-shaped command that writes to a panel
  instead of the framebuffer, auto-indented by `call_depth`. Independent of
  everything here (it needs no new provenance) and useful on its own, so it
  has its own plan: [`console-command.md`](console-command.md). The two meet
  in one place worth knowing about: once frames exist, a console line can
  carry its `call_frame_idx`, which makes the console a clickable execution
  trace.
- **Export to C and use a real debugger.** Export writes `funcN` as genuine C
  functions with genuine recursion, so `Save Scene as C` + `gcc -g` + `lldb`
  gives real stack frames, watchpoints and conditional breakpoints today.
  Worth documenting as the escape hatch; it does not help debug *in* the
  REPL, which is the point of everything above.

### Recommendation

The PATH annotation is the goal, and it needs option B - there is no cheaper
route to it, because ancestor arguments only exist at flatten time. So:

- **Stage 1** - option B. Intern the frames in `flatten_call`, add
  `call_frame_idx` to `GLCmd`, thread the table through `FlatProgramView`,
  print it in `--dump-flat` / a new `--call-tree`. Migrate the three subsumed
  fields behind inline accessors under a drift test, then retire them.
  Verifiable entirely offline, with no UI.
- **Stage 2** - the PATH annotation: the new annotation kind, the chain
  formatter (loop prefix, named args, middle-elision, gutter-label
  resolution), vertex-mode and Verbose gating.
- **Stage 3** - anything from "Later surfaces", by demand, one at a time.

Two things may be worth doing out of band because they are independent of
all of the above and cheap: depth tinting (later surface 1) and the
`console()` command.

Do **not** start at Stage 3. Every one of those is a UI decision that is
cheaper to make after living with the PATH row for a while.

### Risks and invariants to hold

- Flatten is hot (rebuilt per frame). Interning is O(1) per call and adds one
  store per emitted command; it must not add work to the non-call path.
- `repl_flatten_rebake_program` must leave `call_frame_idx` and the table
  untouched - it is topology, like every other provenance field.
- `export_setup.c` synthesizes commands and sets `root_call_src_cmd_idx = -1`;
  those need `REPL_CALL_FRAME_NONE` for the same reason.
- `FlatProgramView` must carry the frame table so temporary-buffer callers
  (tests, `repl_demo`, the flatten differential) see the same thing the live
  path does.
- The flatten differential test compares two dumps byte for byte; frame
  indices must be deterministic (they are - interning order is the
  depth-first walk order).
- Export/import and trace parity are untouched: frames are provenance, never
  emitted GL.
- The PATH row must read the focused command through `replay_focus_flat_idx()`,
  not `replay_pc()` - the PC is the execution *limit* and points one past the
  step's last command.
- Annotation building is per-frame during playback and already caches on
  `s_replay_cache_pc`. Chain formatting must sit inside that cache, not run
  per render call.
