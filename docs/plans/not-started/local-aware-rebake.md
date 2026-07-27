## Local-Aware Rebake — carry function-scoped locals through a value-only walk

## Status — NOT STARTED

Split out of [`docs/plans/done/scoped-local-variables.md`](../done/scoped-local-variables.md),
where it is "fix 3". That plan shipped function-scoped locals end to end and
landed its own two performance fixes (the pay-for-use assignment-target
resolution); it deliberately did **not** attempt this one, because it changes
the flat snapshot layout and its documented immutability, flatten's emit path,
the whole rebake walk, and a dependency classification three of its phases
reasoned about.

Read that plan's "Which cost actually hurts, and what would fix it" and "Known
liabilities and revisit triggers" sections first: they are the measurements
that motivate this work, and they are not repeated here in full.

### The problem

Today `flatten_var_assign` reports **every** dependency feeding a local
assignment as **structural**, so any predefined variable that can reach a local
— transitively counts — forces a full reflatten instead of a value-only rebake.

That is not a statement about the program's shape. It is a statement about what
rebake can carry. `rebake_one_cmd` evaluates each flat command against a
*frozen* `FlatCmdLocalVars` snapshot and writes back only through predef slots,
so nothing would propagate a local's new value into later commands' snapshots.
Reporting the RHS structurally is the conservative answer to that gap.

The converted grass scene is the case that shows the cost is real and the
classification is wrong. *None* of its locals affect flat topology:
`wind`/`wave`/`bend`/`u`/`cx`/… are pure value dataflow. They are structural
only because rebake cannot carry a local forward through a frozen snapshot.
Grass consequently lost its `t` rebake route on conversion:

| grass, gracemont | pre-Phase-1 machinery, old scene | Phase 5 machinery, old scene | Phase 5, converted |
|---|---|---|---|
| `refresh_grass` route | rebake | rebake | **full** |
| `refresh_grass` | 2007 µs | 1947 µs | **3468 µs (1.73×)** |

Native worst case is 1.6 ms of a 16.7 ms frame, which is why the parent plan
did not escalate. **The web build is the trigger:** the maintainer measures
grass at ~1.2 ms → ~5 ms (≈4×) under Emscripten, consistent with rebake being
mostly compiled-program evaluation while a full flatten does text parsing, and
wasm penalising the latter harder. `make web` / `packaging/web/` is a supported
target, so this is a *when*, not an *if*.

### Outcome

`flatten_var_assign` reports local RHS deps as **value** instead of structural,
and grass rebakes again on `t`.

### Design

#### Carry locals by a stable function-local ordinal, never by scope-array slot

This is the part a first sketch gets wrong. `flatten_for_loop` prepends its
iterator at index 0 and shifts every parameter and local up one *per nesting
level*, so the slot a given local occupies differs between a command outside
the loop and one inside it. An index-for-index overlay would corrupt exactly
the converted grass case, whose locals are read both before and inside
`for(s, 0, 6)`.

The identity has to be the local's ordinal in its function's declaration
prologue — the order `flatten_bind_func_locals` appends them in, which is
stable across loop depth — with a per-command sidecar recording, for each
snapshot slot, which function-local ordinal (if any) it holds.

#### The assignment target needs its own recorded ordinal

Every local assignment carries the same `REPL_VAR_IDX_LOCAL` sentinel, so
rebake cannot tell *which* carried local a row writes. Either the sidecar
records the resolved target ordinal per flat command, or rebake recovers the
LHS by name. Recording the ordinal is the better trade: the full flatten that
produced the row computes it anyway.

(The parent plan's fix 2 — the per-row LHS memo on `ReplExprCache` — has
already landed, so the by-name route is available as a fallback rather than a
prerequisite. It also survives this change rather than being absorbed by it:
the full flatten still resolves every assignment target on every visit to
*produce* these ordinals.)

#### The sidecar must become mutable, and rebake must write it

Today `FlatCmdLocalVars` is exposed `const`, and `flatten.h`'s rebake contract
states outright that "local snapshots … are never touched" — which is what
makes the current skip safe. Local-aware rebake has to break that promise
deliberately: `replay_annotations.c` reads those values directly to render
per-instance bindings, so a rebake that fixed only `args[]` would render
correctly while the replay HUD and code panel still showed locals from the
previous full flatten.

Each command's stored snapshot must end up holding its *effective* locals —
post-write for a local assignment, matching what a full flatten leaves there.

#### Frame entry, and the off-by-one in the depth array

Resetting locals to 0 for the next call cannot key off `call_depth`: 135
successive `blade()` calls share both `call_depth` and `call_src_cmd_idx`, and
while grass happens to return to depth 0 between them, `for(i, 0, 10) {
blade(i); }` with nothing else in the body does not. `root_call_src_cmd_idx`
does not separate them either.

Hence a `frame_seq` bumped on call entry and stamped on every command emitted
in that frame — which means **flatten needs a per-depth active-sequence table
too**, not one scalar: `flatten_range` emits the caller's remaining commands
after a nested call returns, and those must carry the caller's seq, not the
callee's. Rebake keeps `carried_seq[depth]` beside the carried frames and
zeroes a depth's locals only when the seq *for that depth* changes, which
resets sibling calls while leaving a caller's locals intact across a nested
return.

Size the depth-indexed arrays `MAX_FLATTEN_CALL_DEPTH + 1`, or index them
`depth - 1`. `flatten_call` checks `call_depth >= max_call_depth` *before*
incrementing, so a command emitted at the limit carries
`call_depth == MAX_FLATTEN_CALL_DEPTH`; a literal `[MAX_FLATTEN_CALL_DEPTH]`
array indexed by that value overruns.

#### `rand`/`rand2` in a local's RHS needs no new rule — but say so explicitly

Value-routing those assignments means rebake re-evaluates them, and grass is
literally the `rand + t` scene (`cx = x0 + 0.08*rand2(seed, 11)*u + …`). It is
safe: `expr_rand01` is a pure hash of `(seed, iter)` with no stream state, so
re-evaluation is idempotent. Nor is there an existing rule to match — nothing
is pinned or forced structural for `rand` today, and a *global* assignment with
`rand` in its RHS already rebakes, which is exactly what pre-conversion grass
did on every `t` change. Locals inherit that unchanged.

The one requirement is one this design already carries: when a `rand` seed is
itself a local, the carried frame must be updated in stream order before the
row that reads it.

#### Replaces the Phase 5 skip

`rebake_one_cmd` currently skips local-target assignments outright (parent
plan, "Three defects found by the conversion"), because the row's frozen
snapshot is post-write and re-evaluating a self-referential RHS against it
applies the write twice. That skip is lossless only while every such dep is
structural. Value-routing them makes the skip wrong, so this change must
replace it — re-evaluate the assignment against the *carried* frame, and write
the result into both the carried frame and the row's snapshot.

The pre-write value the parent plan records in
`payload.assign.prev_local_value` (for replay's inline expansion) is the same
quantity this walk needs on entry to a self-referential row; check whether it
can be reused before adding a second one.

### Regression matrix

Cover the frame shapes the metadata exists to distinguish, not only the
production grass scene:

- a local read and written both outside and inside nested loops, where iterator
  prepending shifts its scope-array slot;
- repeated calls from one call site inside a loop, proving each sibling frame
  starts at zero;
- a nested call followed by another caller-local read/write, proving return
  restores the caller's active sequence;
- recursion, proving equal function-local ordinals at different call depths do
  not alias;
- the maximum permitted call depth, exercising the depth-array boundary;
- a `rand`/`rand2` seed that is itself a local, proving the carried frame is
  updated in stream order before the row that reads it; and
- **two successive value-only rebakes** followed by a full-flatten differential,
  comparing commands, local values and all sidecar metadata after each walk.
  The *second* rebake is the load-bearing one — a single pass hides latent
  metadata damage that only shows when the next walk consumes it.

`tests/test_repl_flatten_rebake.c` already compares `FlatCmdLocalVars` via
`flat_locals_equal`, so the value differential that would catch stale replay
snapshots exists. Extend that helper to compare every new sidecar field too —
slot-to-local ordinals, local-assignment target ordinal, and `frame_seq`.

### Verification

The parent plan's `bench_repl --only refresh_slider` is the acceptance
instrument: it carries four pinned (scene, variable, route) cases, and this
change is expected to flip **grass / `field`** and **grass / `t`** back to
REBAKE while leaving wave / `amp` REBAKE and the orrery's genuinely structural
cases FULL. Update the pinned expectations in the same commit as the routing
change, never before it.

`test_real_scene_time_routes` in `tests/test_repl_flatten_rebake.c` records
grass's current route (with the measurement in a comment) and will need the
same treatment.

Take the Emscripten measurement as well as the native one: the ≈4× web figure
is the number that motivated this plan, so it is the number that should show
the improvement.

### Related plans

- [`docs/plans/done/scoped-local-variables.md`](../done/scoped-local-variables.md)
  — parent; shipped locals, measured this regression, and landed the two
  pay-for-use fixes that precede this one.
- [`docs/plans/done/flatten-performance-without-vm.md`](../done/flatten-performance-without-vm.md)
  — the dependency-mask / compiled-expression design this walk is built on.
