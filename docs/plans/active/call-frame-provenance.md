## Call-Frame Provenance - debugging deep call chains and recursion

## Status - Stage 3 complete (PATH validated; Stage 4 is demand-driven)

Revision note, implementation reviews. Two post-Stage-2 reviews of the
landed intern + PATH row. The P1s were a mismatched 16384/65536 cap
(8-arg scenes bound the arena at 8192 frames), empty frames exhausting
the table, an uninitialized PATH length on long shallow paths, and a
Unicode em dash that blocked `make test-stubs`. The P2s were a join
budget that still used the caller's `MAX_LINE_LEN * 2` buffer, mid-number
`%g` truncation, overflow fallback rendering `func()` for missing
arguments, quadratic `--call-tree`, a dead 32 KB memset whose bench
attribution was wrong, and drift coverage that never left the hand-built
fixtures. Addressed below: arena is `MAX_CALL_FRAMES * 8`, empty frames
are LIFO-reclaimed, the formatter has a fallback for every over-budget
shape, overflow rungs render `func(...)` plus `[incomplete]`
only on the unindexed selected command,
`--call-tree` is one preorder pass, hard flatten failure zeros the
table, overflow status is one-shot on the 0->1 edge, and
`assert_indexed_drift` walks the built-in catalog (and
`tests/scenes` under `REPL_SCENE_CORPUS`).

Revision note, performance. After two design-review rounds the core
contract is settled; this pass adds an explicit `make bench` gate so the
flatten-hot intern does not ship on a hunch. Interning is O(1) per call
plus an arena append and must not add work to the non-call path - the
benchmark is how that claim is checked, not a comment.

Revision note, two review rounds. Round 1 rejected retiring the existing
provenance fields, storing the frame index on `GLCmd`, a fixed 16-argument
frame record, and formatting the breadcrumb inside `replay_annotations.c`.
Round 2 rejected the *repairs*: frame-index equality is not safe at the
overflow boundary, the code-panel row builder cannot resolve gutter labels
while it is building rows, "structured payload" was not a transport, and the
annotation cache key is missing a flat-program generation. All of it is
corrected below, and the reasoning is kept inline rather than in a
changelog, because in every one of these cases the *wrong* answer is the one
a reader would otherwise reinvent.

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

The record is **split in two**, because the two halves have different
loss tolerances (design-review findings 1 and 6):

```c
/* Topology - the identity half. Must be lossless for every flat command
 * that carries a frame index, because consumers navigate by it. */
typedef struct {
    int parent;             /* enclosing frame, REPL_CALL_FRAME_NONE at top */
    int call_src_cmd_idx;   /* the funcN(...) row that opened this frame */
    int func_slot;          /* 0..REPL_FUNC_SLOT_COUNT-1 */
    int depth;              /* == parent->depth + 1 */
    int flat_begin, flat_end;   /* the frame's contiguous subtree range */
    int arg_offset, arg_count;  /* window into the argument arena below */
} ReplCallFrame;            /* 32 B */
```

Arguments live in a separate **variable-length arena** - one `float` run per
frame, addressed by `arg_offset`/`arg_count` - rather than a fixed
`args[N]` inline. Two reasons:

- A fixed 16 would silently truncate: `MAX_EXPR_VARS` is 32, so a function
  may legally bind more parameters than the record holds, and "historically
  exact" is the whole premise of the PATH row. The arena is lossless to 32.
- It is also *smaller* in practice. Real functions take 3-8 parameters
  (`func4(x,y,z)`, `divide_triangle(...)` at 8), so a per-frame fixed 32
  would waste ~100 B on every frame to serve a case that essentially never
  occurs.

Storage location: **a parallel flat-only array, not a `GLCmd` field**
(finding 5). `GLCmd` is shared by the source document, both 32-slot
undo/redo rings, the eight user-scene snapshots and the flat program, so an
`int` there costs ~324 KB across copies that have no use for it, to buy
32 KB of flat-program data. `FlatCmdLocalVars local_vars[MAX_FLAT_COMMANDS]`
in `ReplFlatProgramState` is the existing precedent for exactly this - a
flat-only parallel array - and the frame index belongs beside it, in the same
struct or folded into a small `FlatCmdProvenance`.

Sizing, against what the flat program already costs
(`sizeof(GLCmd)` = 208 B, `sizeof(FlatCmdLocalVars)` = 1032 B, so
`ReplFlatProgramState` is already ~9.7 MB):

| | bytes |
|---|---|
| frame index x `MAX_FLAT_COMMANDS` (parallel array) | 32 KB |
| topology table, 16384 frames x 32 B | 512 KB |
| argument arena, `MAX_CALL_FRAMES * 8` floats | 512 KB |
| **total** | **~1.05 MB, ~11% of the flat program** |

The arena used to be 65536 floats. That bound the 8-arg motivating scene
(`divide_triangle`) at 8192 frames and left half the topology table
unreachable. Sizing the arena for 8 args makes the two caps meet at the
same invocation count. Empty-frame LIFO reclaim at the end of
`flatten_call` then keeps wrapper/predicate calls from occupying a slot
nothing can name (`flat_end == flat_begin`; children reclaim first, so
the parent is last-interned in turn).

Design notes:

- **The existing provenance fields stay.** An earlier draft proposed
  retiring `call_src_cmd_idx` / `root_call_src_cmd_idx` / `call_depth` /
  `func_scope_mask` as derivable from the chain (all four - the draft said
  three and then listed four). That is arithmetically true and
  operationally wrong: it collides head-on with the soft capacity limit
  below. Retiring them means an overflow no longer degrades *the PATH row*,
  it degrades call-site highlighting, `gl_state_inspector`, `edit_overlays`
  and `flatten_query` too - silently. Keep all four. They are also the
  migration oracle: a drift test asserting the chain-derived values equal
  the stored ones is how you gain confidence in the table, and it needs both
  sides to exist. The `-12 bytes` claim is withdrawn.
- **Capacity is a soft limit, and now that is safe.** Past
  `MAX_CALL_FRAMES`, interning stops and later commands carry
  `REPL_CALL_FRAME_NONE`; the PATH row falls back to the two-rung
  immediate/root display the legacy fields still provide. A scene must never
  fail to flatten because a debug table filled. The 16384 / (`16384 * 8`) figures
  above are chosen to make overflow practically unreachable rather than
  merely survivable - a program can exceed any fixed bound while staying
  inside the 200000-visit budget, so the bound must be generous *and* the
  degradation must be harmless. Empty frames are reclaimed, so a scene
  of wrapper calls does not spend the table on ranges no consumer can
  reach.
- **Parameter *names* are not stored.** `func_slot` locates the
  `CMD_FUNC_DEF` row and `parse_repl_func_signature()` re-derives them, the
  same way `flatten_call` gets them. Values must be stored, though: locating
  them positionally in a descendant's `FlatCmdLocalVars` does not work
  because `flatten_for_loop` *prepends* its iterator, and a name lookup is
  not bulletproof either because an inner loop iterator may legally shadow a
  param.
- **The in-place rebake is safe by an argument that already exists.**
  `flatten_call` classifies call-argument deps as *structural* ("call-argument
  values freeze into per-flat-command local snapshots"), so anything that
  could change a frame's arguments already forces a full flatten. The frame
  table stays correct for exactly the reason `FlatCmdLocalVars` does.

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

**The `@nnn` line labels in these mockups are the eventual form, not v1.**
They cannot be resolved from where the row is built; see "Transport" below.
v1 drops them and keeps everything else - the identity of each rung is the
function name and its arguments, which is the part that carries the
information.

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

- The **argument arena** is required, not an optimization. The
  `first_own_flat_idx` trick floated under option B is dead for this surface.
- Parameter *names* stay derived at capture time, not at render time -
  `func_slot` locates the `CMD_FUNC_DEF` and `parse_repl_func_signature()`
  reads the signature. Note *where* that resolution happens: it is REPL
  parsing, so it belongs in the annotation builder, and the names travel to
  the UI as data. See the transport section below.
- The same rebake argument still holds: call-argument deps are already
  classified structural, so a value-only rebake can never leave the arena
  stale.

Loop ancestry is the one part already solved: `FlatCmdActiveLoop
active_loops[]` is snapshotted per flat command *and* survives call
boundaries by design, so `u=-2 > v=-2` needs no new storage at all - just
prepending the existing innermost-last array to the frame chain.

#### Transport: how the chain reaches the UI

The naive placement - format the whole breadcrumb string, `@line` labels and
all, inside `replay_annotations.c` - does not typecheck against the layering:

- `replay_annotations_prepare()` runs during snapshot *construction*
  (`PROF_SNAPSHOT_VIRTUAL_LINES` in `glr_ctrl_display_frame`), before the
  `UiRenderSnapshot` exists.
- `ui_repl_code_panel_gutter_labels_for_lines()` **takes** a
  `const UiRenderSnapshot *` and lives in `src/ui/app/`. A
  `src/subsystems/replay/` TU calling it is both a cycle in time and the
  wrong direction across the module boundary.

So the annotation must carry **structure, not prose**. But the obvious
correction - "let the code-panel row builder resolve the labels" - is also
wrong, and for a subtler reason (finding 2): on a cache miss
`ui_repl_code_panel_gutter_labels_for_lines()` calls
`repl_code_panel_init_builder()` + `repl_code_panel_build_rows()`. Calling it
*from inside* `build_rows` re-enters the builder. There is no legal place in
the current design to ask for a gutter label while rows are being built.

**Therefore v1 omits `@line` labels.** Not "alternatively" - definitively.
The breadcrumb is fully readable without them (function name, arguments and
order are the content; the line number is a convenience), and adding them
later needs its own small design: either a two-pass gutter map published
alongside the snapshot, or a non-recursive label-only walk that does not go
through the row builder. Neither is hard; both are out of scope for the row
that answers "how did we get here".

One earlier claim withdrawn: a previous draft argued that late-patching the
row text would invalidate layout because the text drives its own wrapped row
count. **That is not true.**
`repl_code_panel_virtual_row_count_for_line()` counts *logical* virtual
entries matching `after_line_idx`; it does not measure wrapping. And virtual
rows never increment `builder->file_line` - only STATIC / INPUT /
PLACEHOLDER / TEXT rows do - so a PATH row cannot shift any other row's
gutter label no matter how long it is. The re-entrancy above is the real
constraint; wrapping is not.

This limitation is specific to **printing** a gutter number in the PATH
prose. It does not block a caller-site gutter tint. A normal editor highlight
already targets a document source-line index and the code panel resolves that
index while it builds the real source row; it does not need to know the row's
visible gutter number. The controller already uses this path for the immediate
and root caller markers. A later full-chain tint can therefore walk the frame
chain, publish one highlight per frame's `call_src_cmd_idx`, and let the UI
render those markers without a `UiRenderSnapshot`-to-gutter-label lookup or a
row-builder re-entry.

The two surfaces should stay separate: `@line` is optional text inside the
breadcrumb, while caller tint is an editor overlay keyed by source index. The
former remains out of v1; the latter is a reasonable Stage 4 surface once the
PATH row has established the chain semantics.

##### The payload

Do **not** widen `UiVirtualLine` with a union big enough for a depth-64 chain
and its arguments: that struct is instantiated `MAX_VIRTUAL_LINES` = 512
times, so the union's size is paid 512 times to serve at most one row
(finding 3).

Instead, one **`UiReplayPathSnapshot` per frame**, published beside the
virtual-line list, with the PATH virtual row carrying only a small index into
it. The snapshot must be self-sufficient for a *pure* UI formatter - the UI
layer must never call `parse_repl_func_signature()` or otherwise reconstruct
REPL semantics - so it carries, per rung:

- function display name (the funcN alias when set, `funcN` otherwise);
- parameter names, if named arguments are to be rendered;
- argument values;
- the call-site source line index (kept even in v1, so the later `@line`
  work needs no change to the transport);

plus, ahead of the frames, the loop prefix as name/value pairs
(`u = -2`, `v = -2`) read from the focused command's `active_loops[]`.

That list is the actual interface contract between the two plans' halves, and
it is worth writing into the header before any of it is built.

#### Scope for the first cut

- **Vertex replay mode only, and the accessor enforces it.** Use
  `replay_focus_anchor_flat_idx()`, not `replay_focus_flat_idx()`: it is
  documented as the flat index of *the draw the current step emitted*, and it
  already returns -1 when replay is inactive, when the mode is not vertex, or
  when the step emitted no draw. That -1 **is** the polygon-mode suppression
  - no separate gate is needed. The suppression is not squeamishness: in
  polygon mode one step covers a whole primitive, and a primitive's vertices
  can come from different frames (`for(i,0,4){ func0(i); }` inside a
  `glBegin` block - the same minimal repro that breaks the zero-storage
  reconstruction), so a single path would be a lie. The later refinement is
  the longest common prefix of the step's frames with a divergence marker,
  but that is a separate decision.
- **Verbose expansion only.** `replay_annotations_refresh_output()` documents
  the invariant that virtual rows belong to Verbose alone and Expanded keeps
  every readout inline on the source row. PATH is a virtual row, so it is
  Verbose's. Do not add a fourth `e` mode for it.
- `REPL_REPLAY_ANNOTATION_MAX` goes 2 -> 3.
- The existing `HIGHLIGHT_REPLAY_CALL_SITE` / `_ROOT_CALL_SITE` gutter
  markers **stay as they are** in the first cut. They are cheap, they point at
  real lines, and with the fields retained they are also the frame-table-
  overflow fallback. The breadcrumb just becomes the authoritative account.
  Generalizing those markers to N rungs is a later, optional change and should
  not ride along with the PATH row.

#### Long recursion needs middle-elision, not truncation

`MAX_VIRTUAL_LINE_TEXT` is 256 and `MAX_FLATTEN_CALL_DEPTH` is 64, so a deep
chain does not fit. Keep the outermost rung and the innermost two or three,
elide the middle with a count - the ends carry the information, the middle is
the part a reader skims:

```
|-> divide_triangle(depth=12) @124 > ... 9 frames ... > depth=1 @104 > triangle @65
```

Elision is a *display* concern and must not reach back into storage: the
frame's arguments stay lossless (see the arena in option B) even when the row
shows three of them. The formatter's join budget is
`min(out_sz - 1, MAX_VIRTUAL_LINE_TEXT - 1)`, and every over-budget shape
has a fallback: middle frame elision, loop elision, in-rung argument
elision (never a mid-number cut), then whole-part truncation ending in
`...`. A long shallow path (one 32-arg rung, three long rungs, many
loops) must not return an empty row.

#### One more corroboration that the gap is real

`replay_flat_cmd_context_matches()` in `replay_annotations.c` already tries
to identify an invocation and has to do it with a four-field conjunction -
`func_scope_mask && call_depth && call_src_cmd_idx && root_call_src_cmd_idx`
- with a comment conceding that repeated calls at one source site are
disambiguated *temporally*, by `find_replay_assignment_flat_cmd`'s backward
scan, because the provenance cannot do it. The frame index collapses that
conjunction to an identity test - but **not unconditionally**, and the
exception is exactly the overflow case (finding 1). `REPL_CALL_FRAME_NONE`
is not an identity: comparing two unindexed commands for equality would make
every unrelated invocation past the overflow point match. The contract is
identity-when-indexed, legacy-otherwise:

```c
if (candidate_frame != REPL_CALL_FRAME_NONE &&
    current_frame   != REPL_CALL_FRAME_NONE)
    return candidate_frame == current_frame;
return legacy_context_matches(candidate, current);   /* today's four fields */
```

which is the second reason the legacy fields are permanent, and the reason
the drift test must compare derived-vs-stored **only for indexed commands**,
with overflow behaviour asserted as its own case rather than folded in. The
codebase has already paid for this gap once; the fix must not introduce a
worse one at the boundary.

### Caller-frame expansion (Part A - LANDED)

Found by looking at a running replay: with the PC inside `func4`, `func4`'s
body carried inline value readouts but `func3`'s - the invocation that called
it - carried none, though its values are just as live.

Not a design decision, an expressiveness limit.
`find_replay_assignment_flat_cmd()` accepted a candidate row only if
`replay_flat_cmd_context_matches()` said it came from the *same frame* as the
PC, and "same frame" was the only relation the old four-field provenance
could express. Every ancestor differs in all four fields
(`func3` at `depth=4 mask=0x0f` vs `func4` at `depth=5 mask=0x1f`), so the
whole caller chain was rejected wholesale.

The value machinery was already frame-agnostic and needed no change:
`build_visible_vars_from_predef_snapshot(flat_idx, ...)` reads
`local_vars[flat_idx]` - that row's own frozen bindings - and the readout
takes `args[0]` / `payload.assign.prev_local_value` from the row itself. The
cache needed no change either: `replay_annotations_rebuild_cache()`'s
backward pass stores the most recent execution per source line with **no**
context filter, so an ancestor's row was already sitting in
`s_replay_flat_map[]`. Only the acceptance test was frame-restrictive.

So Part A is one new predicate, `replay_flat_cmd_on_current_chain()`:
identity first, else test whether the candidate's frame appears in
`repl_call_frame_walk_chain()` of the PC's frame. Both call sites in
`find_replay_assignment_flat_cmd` use it; `replay_flat_cmd_context_matches`
is left alone so "same invocation" stays available as its own concept.

Two properties worth keeping:

- **A completed sibling call is still rejected.** Its frame is not on the
  chain, so its stale values never reach the panel. That rejection is why
  this is a chain test rather than a relaxed gate, and it is asserted
  directly.
- **Overflow degrades to today's behaviour.** `repl_call_frame_identity()`
  returns -1 when either frame is `REPL_CALL_FRAME_NONE`, and that value
  cannot separate "genuinely top-level" from "past the latch", so neither is
  admitted as an ancestor. Top-level rows are therefore excluded as a
  consequence, not by intent; lifting that needs the `call_depth == 0`
  discriminator and was deliberately not done here.

Measured: `replay_examples` 118.7-120.0 ms before, 118.7-119.6 ms after
(`min_iter_ms`, `--iters 15`, three runs each) - the chain walk on the
reject path is not visible.

**Part B - function-def headers - is NOT done and must not be done by
deleting the gate.** `build_replay_funcdef_inline_comment()` reads
`local_vars[cur_flat]`, the *PC's* snapshot, and looks parameters up by
name. Every function in the deep-call scene takes `(x, y, z)`, so a relaxed
gate would find `func4`'s `x` and print it on `func3`'s header - wrong
values that look right. It has to read the frame's argument arena instead
(`f->arg_offset`), the way `replay_path_add_rung_from_frame()` already does.
Recursion also needs a policy there: one source row, several chain frames -
innermost wins.

### Later surfaces (explicitly not in the first cut)

Kept for sequencing only. None of these should ride along with the PATH row.

1. **Colour geometry by call depth.** **DONE** - Config -> GEOMETRY -> "Call
   depth". An overlay mode that tints each draw by `call_depth`. Needed **no
   new provenance at all** - the field was already on `GLCmd` - and it makes
   recursion structure visible spatially. The cheapest genuinely useful thing
   on the list, and independent enough to ship first or never; it shipped
   first. See "Call-depth tint, as built" below.
2. **Replay caller-chain gutter tint.** Generalize the existing immediate/root
   caller-site markers to the complete frame chain for the selected replay
   draw. The controller walks `repl_call_frame_walk_chain()` and publishes one
   source-line highlight per frame, using the frame's `call_src_cmd_idx` and
   depth; no displayed gutter-number lookup is involved. This is deliberately
   a gutter overlay, not another PATH row and not a replacement for the
   breadcrumb.

   The visual language should borrow `call_depth_viz`: depth is ordered, so
   use its cool-to-warm ramp (`call_depth_viz_ramp_rgb()`), normalized to the
   observed depth range for the active chain/frame. The recommended first
   rendering contract is two-role: the innermost, last rung gets the warmest
   ramp colour, while ancestor rungs use neutral off-palette white/grey bands.
   That makes the active caller pop without competing with the PATH row; a
   later pass can put every band on the full ramp if that proves more useful.
   The palette choice is presentation, not provenance. If several frames land
   on one recursive source line, keep one band per distinct frame/depth, using
   the existing segmented gutter mechanism already used for `glPushAttrib`
   bits. The existing marker-band cap is a display cap only; it must not
   truncate the frame chain data.

   This needs a multi-entry replay highlight kind (or equivalent depth payload)
   and a code-panel aggregation path analogous to
   `repl_code_panel_line_attrib_bits()`. The controller must resolve the ramp
   colours (or publish the chain depth range and a neutral colour payload) so
   the UI continues to consume a frozen snapshot rather than reaching into
   live REPL/call-depth state. Existing immediate/root markers remain the
   overflow fallback and can remain the scalar high-priority markers until the
   chain form is validated.
3. **Offline `--call-tree` dump.** Also turns `--flat-histogram` from
   per-*function* into per-*invocation* accounting, which is what you want
   when a scene approaches the 8192 flat budget.
4. **Debugger verbs on replay.** Frames-as-ranges makes *step out* (seek to
   `frame.flat_end`), *step over* and *run to frame* one-liners over replay's
   existing clamped execution.
5. **Frame isolate.** Render one invocation's subtree - a range clamp.
6. **Click geometry, get the stack.** The inverse-pick direction, using
   `repl_find_affecting_transforms_for_flat_vertex()`'s plumbing and the
   assign-plot right-click hit model.

### Adjacent work

- **`console(...)`** - a `label()`-shaped command that writes to a panel
  instead of the framebuffer, auto-indented by `call_depth`. **Follow-up
  work, sequenced after this plan**: it needs no new provenance and does not
  block anything here, and keeping it out avoids two features negotiating one
  review. Its own plan: [`console-command.md`](console-command.md). The two
  meet in one place worth knowing about - once frames exist, a console line
  can record the frame it was emitted from, which makes the console a
  clickable execution trace.
- **Export to C and use a real debugger.** Export writes `funcN` as genuine C
  functions with genuine recursion, so `Save Scene as C` + `gcc -g` + `lldb`
  gives real stack frames, watchpoints and conditional breakpoints today.
  Worth documenting as the escape hatch; it does not help debug *in* the
  REPL, which is the point of everything above.

### Recommendation

The PATH annotation is the goal, and it needs option B - there is no cheaper
route to it, because ancestor arguments only exist at flatten time. Staged so
that each step is verifiable before the next depends on it:

- **Stage 1 - lossless flat-only frame provenance.** Intern topology + the
  argument arena in `flatten_call`; index it from a parallel flat array;
  thread the table through `FlatProgramView`; print it in `--dump-flat` and a
  new `--call-tree`. **Retain every existing provenance field** and add a
  drift test asserting the chain-derived `call_src_cmd_idx` /
  `root_call_src_cmd_idx` / `call_depth` / `func_scope_mask` equal the stored
  ones - the legacy fields are the oracle, and later the overflow fallback.
  Entirely offline, no UI.
- **Stage 2 - the structured PATH virtual row.** New annotation kind carrying
  the chain as data via a per-frame `UiReplayPathSnapshot`, anchored on
  `replay_focus_anchor_flat_idx()`; formatting and elision in the code-panel
  row builder; Verbose gating; **no `@line` labels**.
- **Stage 3 - validation before anything else lands on top.** Same-site
  recursion (both `@96` rungs distinct), calls in loops (the four-invocation
  repro), frame-table overflow degrading to the two-rung fallback *and* the
  identity test correctly declining to match unindexed commands, a function
  with 17+ parameters round-tripping through the arena, a full flatten at an
  unchanged replay PC, and middle-elision at depth > 8.
  Done: `repl_call_frame_identity` + `replay_test_flat_cmd_context_matches`
  (`NONE` vs `NONE` is not an identity; unindexed different sites do not
  match), PATH coverage for same-site recursion, four loop siblings, the
  17-arg arena, a full flatten at an unchanged replay PC, and depth-9
  elision. Overflow fallback and pre-overflow indexed commands were
  already covered.
- **Stage 3b - `make bench`.** After Stage 1 (and again after Stage 2 if it
  touches the flatten or annotation-prepare path), run `make bench` and
  compare the flatten / rebake / replay rows against the pre-change
  baseline. See "Performance verification" below. A silent skip is a miss.
  Done: Stage 2/3 table under that heading.
- **Stage 4 - anything from "Later surfaces", by demand, one at a time.**

Depth tinting (later surface 1) was independent of all of this and shipped on
its own; see below. `console()` is follow-up work with its own plan; it needs
nothing here.

Do **not** start at Stage 4. Every one of those is a UI decision that is
cheaper to make after living with the PATH row for a while.

### Call-depth tint, as built

Shipped as a session-inspection config toggle beside Depth view / Stencil
view, not as a replay or overlay mode. Four decisions worth keeping, because
each had a plausible alternative that is worse:

- **A ramp, never a categorical palette.** The obvious move was to copy
  `buffer_viz_stencil`'s fixed 16-entry palette for shallow depths and fall
  back to a ramp past 8. But a stencil value is a *tag* - value 7 has no
  "more than" relationship to value 3, which is exactly why a lookup table
  buys view-independence there. Call depth is an *ordering*, and the only
  question the view answers is which geometry is deeper; a palette answers it
  with magenta-versus-yellow, which is no answer. The palette/ramp split had a
  second failure mode too: a scene whose recursion deepens with `t` would
  change colour *scheme* mid-animation at the crossover.
- **Normalized over the observed max depth, recomputed per frame.** A ramp
  over a fixed 0..`MAX_FLATTEN_CALL_DEPTH` would give a three-deep scene four
  adjacent blues. Normalizing adapts the contrast, and re-deriving it each
  frame (rather than EMA-smoothing it, as stencil RAMP does) is right because
  depth is an integer and a change in it is information - the stencil EMA
  exists to damp per-pixel jitter that has no analogue here.
- **The colours reach the executor as a table, not a hook.** They depend only
  on the frame's depth range, which the controller resolved before the pass,
  so `ReplExecutionOptions` carries a borrowed `const float (*)[3]` and
  `src/repl/` gains no knowledge of the viz module. The emit point is keyed on
  the existing `repl_cmd_consumes_current_color()` predicate and fires on
  depth *change*, so a 3000-vertex run costs one `glColor4f`.
- **Suppression is `state_filter`, not new executor policy.** The pass owns
  the colour, so the program's own colour/material commands must not paint
  over it - the same defence the winding view already runs for its two-sided
  lighting, through the same hook. Lighting itself is deliberately left alone:
  the tint replaces hue, and lit geometry stays lit so shape still reads.

Known scope boundaries, all deliberate: the wireframe views run through
`hidden_lines_execute` and are not tinted; replay fade batches already force
their own single colour; and the legend corner is single-tenant, so Stencil
view wins it when both are on.

### Performance verification

Flatten is hot: the live path rebuilds the flat program every frame when
`t` or a structural root moves, and accumulation time-blur multiplies that
by the sample count. The intern is specified as O(1) per `flatten_call`
plus an arena `memcpy` of the argument run, and as **zero extra work on
the non-call path** beyond one `int` write next to the existing local-var
snapshot.

That is a measurable claim. After the frame table lands, and again after
the PATH row if `replay_annotations_prepare()` grows:

```
make bench
```

Compare, at minimum:

| Bench row | What it answers |
|---|---|
| flatten / full-flatten of a no-`funcN` example | Non-call path must stay inside between-build noise (sub-2% on this rig). A super-linear jump or a several-percent move that survives rebuild medians is a bug. |
| flatten of a call-heavy example (or the deep-chain / Sierpinski scenes under `--examples-dir`) | Call-path overhead. A small constant per invocation is expected; a super-linear jump is not. |
| rebake | Must be unchanged: rebake does not walk or rewrite the frame table. |
| replay / replay-annotation rows | Stage 2 only. Re-resolving a ≤64-rung chain per `prepare()` should be invisible next to the existing snapshot walk. |

Record the before/after numbers in the Stage 1 (and Stage 3) commit
message, or as a short note under this heading once the numbers exist.
Do not treat "it should be cheap" as the verification.

A scene with no `funcN` calls is the load-bearing control. Sub-2%
moves on this rig are not measurable: identical HEAD source, rebuilt
in separate sessions, produced non-overlapping `flatten_grass` ranges
~2% apart (code-layout / alignment). Do not attribute a cause to a
delta that sits inside that between-build noise. A real regression is
a super-linear jump or a several-percent move that survives medians
across repeated rebuilds, not repeated runs of one binary.

#### Stage 1 numbers (2026-08-16, drew-macbook-air Darwin 25.6.0 arm64)

Taken after `bd34e303` (`repl: intern call-frame provenance during flatten`)
against its parent `4d2d2769`. Same binary shape as `make bench`
(`BUILD=release USE_GL_STUBS=1`, default `--iters 5`), run sequentially
on one machine so the two processes do not steal cycles from each other.
Subset is the flatten / rebake-refresh / replay rows the table above
names; parse/feed/slider rows cannot see the intern and were skipped.

Unit is `per_op_us` from `bench_repl --csv` (microseconds per flatten,
refresh, or replay step). Delta is `(after - before) / before`.

| Row | What it is | before | after | Δ |
|---|---|---|---|---|
| `flatten_examples` | no-`funcN` wave-surface control | 750.68 | 756.27 | +0.7% |
| `flatten_grass` | no-`funcN`, 8113 flats (near cap) | 1743.99 | 1772.38 | +1.6% |
| `flatten_grass_cold` | same, cache rebuilt inside the timer | 1766.29 | 1796.33 | +1.7% |
| `flatten_orrery` | call-heavy named funcs, 5651 flats | 961.59 | 961.21 | −0.0% |
| `flatten_orrery_cold` | same, cold cache | 1074.38 | 1079.24 | +0.5% |
| `flatten_corpus_14` | Sierpinski carpet (recursion) | 85.93 | 87.58 | +1.9% |
| `flatten_corpus_15` | Sierpinski sponge (recursion) | 284.28 | 287.98 | +1.3% |
| `flatten_corpus_16` | 3D tree (func + recursion) | 752.58 | 769.45 | +2.2% |
| `refresh_grass` | production `t` refresh (rebake path) | 1755.63 | 1763.27 | +0.4% |
| `refresh_orrery` | production `t` refresh | 974.13 | 975.78 | +0.2% |
| `refresh_wave` | production `t` refresh | 283.76 | 289.72 | +2.1% |
| `replay_examples` | step replay through every example | 1.792 | 1.791 | −0.0% |

Tiny corpus rows (`logo`, `cube`, `function demo`) sit at 1–3 µs and
jumped by tenths of a microsecond; that is timer noise, not a flatten
cost. Grass/wave/tree are the ones that can lie.

**Reading.** These deltas are below the noise floor. The same
unchanged `flatten_grass` binary shape, rebuilt on different days,
moved by ~2% with non-overlapping ranges; the recorded +0.7–1.7%
no-call shift and the memset A/B (fully overlapping, later removed as
dead) are not separable from that between-build variance. Rebake and
replay not jumping is the only claim this table can support: a walk
that rewrote the frame table would have shown up as more than layout
noise. Do not treat the leftover +1% as evidence for the per-command
`int` store or the extra cache footprint - those are guesses this
harness cannot confirm. Future notes under this heading should either
publish medians across repeated rebuilds or decline to name a cause
for a sub-2% move.

#### Stage 2/3 numbers (2026-08-17, drew-macbook-air Darwin 25.6.0 arm64)

Taken after Stage 2 PATH + the Stage 3 validation tests, same flags as
the Stage 1 table (`BUILD=release USE_GL_STUBS=1`, `bench_repl --csv`,
default `--iters 5`). Unit is `per_op_us`.

| Row | Stage 1 after | Stage 2/3 | Δ vs Stage 1 |
|---|---|---|---|
| `flatten_examples` | 756.27 | 773.91 | +2.3% |
| `flatten_grass` | 1772.38 | 1815.75 | +2.4% |
| `flatten_grass_cold` | 1796.33 | 1858.03 | +3.4% |
| `flatten_orrery` | 961.21 | 1014.28 | +5.5% |
| `flatten_orrery_cold` | 1079.24 | 1142.43 | +5.9% |
| `flatten_corpus_14` | 87.58 | 89.90 | +2.6% |
| `flatten_corpus_15` | 287.98 | 288.75 | +0.3% |
| `flatten_corpus_16` | 769.45 | 771.80 | +0.3% |
| `refresh_grass` | 1763.27 | 1811.51 | +2.7% |
| `refresh_orrery` | 975.78 | 991.57 | +1.6% |
| `refresh_wave` | 289.72 | 305.98 | +5.6% |
| `replay_examples` | 1.791 | 1.911 | +6.7% |

**Reading.** These are a different day's rebuild, not a paired A/B, and
they sit in the same band as the ~2% between-build scatter already
measured for unchanged `flatten_grass`. They do not support a claim
that PATH re-resolve made replay more expensive, or that empty-frame
reclaim made flatten more expensive. No super-linear jump. The
rebake-refresh rows still move with flatten, which is the "rebake does
not walk the table" check this harness can actually make.

### Risks and invariants to hold

- Flatten is hot (rebuilt per frame). Interning is O(1) per call plus an
  arena append; it must not add work to the non-call path.
- `repl_flatten_rebake_program` must leave the frame index and the table
  untouched - it is topology, like every other provenance field.
- Initialization of the frame index belongs to the **parallel provenance
  buffer**, not to synthesized commands. `export_setup.c` sets
  `root_call_src_cmd_idx = -1` on the `GLCmd`s it fabricates; with a flat-only
  side array there is no field on those commands to set.
  `flatten_append_cmd` stamps every slot in `[0, flat_count)`. Consumers
  must not read past `cmd_count`; there is no memset of the unused tail.
  A hard flatten failure (depth / visit budget / flat capacity) zeros
  the published frame counts so `--call-tree` cannot print ranges into
  an empty stream.
- **Overflow admission is atomic.** A frame needs a topology slot *and*
  `arg_count` floats of arena. If either is short, publish no partial frame:
  latch the overflow and hand out `REPL_CALL_FRAME_NONE` from that invocation
  onward, so a chain is never half-recorded and `parent` never points at a
  frame whose arguments were truncated. Surface the latch (`--call-tree`
  note, and a one-shot status on the 0->1 edge so an animated overflowing
  scene does not pin the status bar). Unindexed fallback rungs render as
  `func(...)` plus `[incomplete]`, not `func()`. The `[incomplete]`
  marker is per selected command: an indexed vertex before the overflow
  point keeps a complete chain even after the program latch is set.
- `FlatProgramView` must carry the frame table and the arena so
  temporary-buffer callers (tests, `repl_demo`, the flatten differential) see
  the same thing the live path does.
- The flatten differential test compares two dumps byte for byte; frame
  indices and arena offsets must be deterministic (they are - interning order
  is the depth-first walk order).
- Export/import and trace parity are untouched: frames are provenance, never
  emitted GL.
- **The PATH cache cannot key on the replay PC alone.**
  `replay_annotations_prepare()` rebuilds only when `s_replay_cache_pc !=
  replay.pc`, but `repl_refresh_flat_program()` runs earlier in the same
  frame and a full flatten can replace frame indices, topology and arena
  contents *at an unchanged PC* - a paused replay under a structural `t`
  dependency does exactly that. Stale indices would then point into a
  rebuilt table. Either add a flat-program generation to the cache key, or
  invalidate explicitly after a full flatten, or - simplest and
  recommended - just re-resolve the chain on every `prepare()` call: it is
  at most `MAX_FLATTEN_CALL_DEPTH` = 64 pointer hops, which is nothing
  against the work `prepare()` already does. Only *formatting* belongs in
  the row builder, which runs per layout.
