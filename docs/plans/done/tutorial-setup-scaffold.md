# Tutorial Composition: Building On Prior Tutorials Without Boilerplate

## Status — LANDED (2026-07-08)

Option A shipped in commit `0a15b86d`: catalog setup scaffolds, locked setup
rows, header/camera consumption, setup-label placement, validation, and the
Color Interpolation demonstration. The historical design below records the
contract that shipped.

## Problem

Tutorials should be able to build on ideas an earlier tutorial taught —
e.g. *draw a triangle* → *color interpolation* → *depth testing* —
without every later tutorial making the learner re-type (and the author
re-narrate) the shared preamble. Before this plan, every tutorial
started from an empty transient scene, so "builds on the triangle"
meant repeating the five triangle steps in each dependent tutorial:
boilerplate for the author and repeated ground for the learner.

This doc records the design options considered, their trade-offs, and
the decision.

## Options Considered

### Option A — Preloaded setup lines on `TutorialEntry` (`.setup`)

A NULL-terminated `const char *const *setup` array of REPL source lines
on the catalog entry, loaded at `tutorial_start` through
`repl_load_apply_line` (the same non-editor path instruction comments
use), each loaded row locked. Steps then run against that scaffold.

- **Complexity: low.** Mirrors the existing `.cfg` field pattern and
  reuses the programmatic loader; append-placement math is untouched
  (insertion point = document count, wherever that starts). Load
  failure follows the existing pattern: status + teardown.
- **Flexibility: moderate.** Each tutorial ships self-contained with
  its starting code visible, commented, and locked. Scaffold text is
  duplicated between tutorials in the catalog *source*, but it is
  compact data with no cross-tutorial coupling: no ordering
  constraints, and a tutorial can start from a state no other tutorial
  produces.
- **Anchor extension:** `STEP_AT` target labels may also resolve to
  `:name` goto-label rows (`CMD_GOTO_LABEL` — existing REPL syntax)
  in the preloaded scaffold, so label-targeted steps can splice
  commands *into* the scaffold, not just above earlier steps' rows.

### Option B — Authoring-side sharing with step-block macros

Shared step runs as C macros pasted into multiple tutorials' arrays
(`#define STEPS_DRAW_TRIANGLE STEP_APPEND(...), ...`).

- **Complexity: zero.** Works today, no runtime change.
- **Flexibility: low for the actual goal.** Removes *author*
  boilerplate but the *learner* still re-types the shared steps in
  every tutorial — it doesn't stop covering the same ground. Still
  useful for genuinely-interactive shared runs, and it composes with
  any of the other options.

### Option C — Prologue by reference (`.prologue = "First Triangle"`)

At start, replay another tutorial's COMMAND steps programmatically
(feed each `expected` — and its comment, as a locked comment — through
the loader), then begin this tutorial's interactive steps. Could take a
step range to "start at any point" in the referenced tutorial.

- **Complexity: medium.** The replay loop is small — it is Option A
  deriving its setup from another entry's steps. The cost is semantics
  and validation: what to do with non-COMMAND prologue steps (skip
  REQUIRE/NOTE? apply SET's cfg? declare REQUIRE_VAR's variable?),
  carrying the referenced tutorial's label table into this run so
  `STEP_AT` can target prologue rows, cycle detection if prologues
  chain, and catalog validation of the composed program.
- **Flexibility: high, with a coupling hazard.** Zero duplication, and
  editing the base tutorial automatically updates every dependent —
  which is also the risk: a pedagogical edit to A silently changes B's
  starting state and row numbering. The validator catches structural
  breakage but not "B's narration no longer matches what's on screen."

### Option D — Start from a scene/example (`.scene = <example name>`)

Preload a named built-in example (or snippet file) into the transient
scene using the existing example-loading path, headers included.

- **Complexity: low-to-medium.** The loader exists; the work is wiring
  it into `tutorial_start` and deciding the locking policy.
- **Flexibility: high in one dimension, awkward in another.** Scenes
  can contain things COMMAND steps can't express (loops, funcN bodies,
  animation with `t`) — a richer scaffold than any replayed tutorial.
  But authoring splits across two catalogs (examples + tutorials) and
  the "builds on the previous tutorial" relationship becomes implicit.

## Decision

**Implement Option A now** (with the `:name` goto-label anchor
extension). It is a small, self-contained change that fully unlocks the
triangle → color → depth progression: each tutorial declares its
starting code once, locked and commented, and the learner only types
what's new.

**Option C is a possible future follow-up**, to be implemented as sugar
over A: `.prologue` would *derive* the setup lines from another entry's
COMMAND steps at start (restricted initially to COMMAND-only prologues,
no chaining), lowering into the same preload path so the runtime keeps
one mechanism. Adopt only once the catalog is large enough that setup
duplication actually hurts.

Options B and D remain available/possible but are not planned: B costs
nothing and can be used ad hoc; D only becomes attractive for tutorials
that must start from animated or loop-heavy scenes that steps cannot
build.

Policy decisions (both settled):

1. **Preloaded rows are locked** — consistent with instruction rows
   ("Tutorial line is read-only" on navigation), and it keeps the
   tutorial line-tracking bookkeeping safe from user edits.
2. **Setup honors the example header vocabulary** — leading
   `// @cfg slug = value` lines and an optional 5-line `// camera`
   block are consumed the way examples consume them, so an existing
   example/scene body can be pasted verbatim as a tutorial scaffold.
   Setup `@cfg` slugs join the teardown-restore baseline exactly like
   entry-level `.cfg` slugs.

## Implementation Notes (Option A)

- `TutorialEntry.setup` + `repl_tutorial_setup_lines()` accessor
  (`src/repl/tutorials.{h,c}`).
- Loaded in `tutorial_start` after `tutorial_baseline_apply`, before
  step 0: consume `@cfg` header (via
  `repl_state_parse_workspace_header_line` +
  `repl_export_apply_pending_cfg`), consume the optional camera block,
  then feed body lines through `repl_load_apply_line`; lock every
  loaded row; mark flat/source dirty. Failure → status + teardown.
- `tutorial_baseline_capture` also records slugs named by setup `@cfg`
  lines so teardown restores them.
- `tutorial_step_instruction_line` (LABEL placement): when
  `target_label` doesn't match an earlier step's label, search the live
  document for a `CMD_GOTO_LABEL` row whose extracted name matches
  (resolved at step-entry time, so prior splices/shifts are handled by
  construction).
- Validator: `target_label` may resolve to a setup goto label; step
  labels must not collide with setup goto labels; setup line count +
  step count must fit the locked-line table
  (`TUTORIAL_LOCKED_LINE_MAX`, raised 64 → 128 since setup rows are
  locked too).
- Catalog demonstration: **Color Interpolation** tutorial — setup
  preloads the flat triangle (one red vertex color), steps splice
  per-vertex `glColor3f` calls at setup goto-label anchors so the
  gradient appears without re-typing the triangle.
