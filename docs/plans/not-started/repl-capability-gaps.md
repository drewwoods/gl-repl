# REPL Capability Gaps - Roadmap

## Status - NOT STARTED (2026-08-12)

Nothing here has been implemented. This document is a **prioritized index**,
not a fourth copy of designs that already exist: two of the five items below
already have a design of record elsewhere, and this plan's job is to say what
each unlocks, what it costs, and which order they are worth doing in.

## Why this list exists

Writing `tests/scenes/general/stencil-shadow-volume.glr` (landed
`d9462d47`) surfaced the gaps concretely rather than abstractly. That scene
implements a real z-pass stencil shadow volume - the cube's silhouette as seen
from a moving point light, extruded into a closed hull, counted per-pixel with
`GL_INCR`/`GL_DECR`. It works, and its shape is a direct fingerprint of what
the REPL cannot express:

- `volume()` is **96 lines of unrolled per-face blocks** because a mesh cannot
  be stored as data.
- `caps()` takes **twelve scalar parameters** and repeats the extrusion
  arithmetic twelve times because a helper cannot return a value.
- The hull is drawn **twice, with `glCullFace` flipped between passes**,
  because there is no two-sided stencil.
- The scene fakes illumination with a hand-written ambient/lit pass pair
  because the program cannot move a light.

Each bullet maps to one item below. The scene is the acceptance test: when an
item lands, a specific part of that file should get shorter or disappear.

## Summary

| # | Feature | Unlocks | Effort | Design of record |
|---|---|---|---|---|
| 1 | Float-returning functions (`return expr;`) | Helpers that compute instead of only draw; removes the global-slot tax on every scene | **7-10 dev-days**, ~825 LOC | `not-started/float-returning-repl-functions.md` (needs a refresh - see §1) |
| 2 | `glStencilOpSeparate` / `glStencilFuncSeparate` + `GL_INCR_WRAP` / `GL_DECR_WRAP` | One-pass shadow volumes; correct counting under deep overlap | **2-3 dev-days**, ~250 LOC | `done/stencil-buffer-support.md` §"Phase 3" |
| 3 | `glLightfv` / `glLightf` (program-movable lights) | Real lighting response; unbreaks one corpus scene, unbends two others | **3-5 dev-days**, ~350 LOC - *ownership decision required first* | none - §3 below |
| 4 | Larger scratch storage (`REPL_SCRATCH_ARRAY_*`) | Data-driven geometry, per-particle state, meshes above 16 floats | **1-2 dev-days** for the knob; more if `D`-`H` are added | `done/add-fixed-array-support.md`, `done/bounded-global-arrays.md` |
| 5 | Defect fixes (`--time` on the dump path; two broken corpus scenes) | Makes the documented authoring-verification recipe actually work | **0.5 dev-days** | §5 below |

Recommended order is **5 → 2 → 1 → 3 → 4**, argued in §6. It is deliberately
not the order of the table, which is ranked by impact rather than by sequence.

---

## 1. Float-returning functions

### What is missing

`CMD_CALL` is a command type in the statement stream. Functions are void; there
is no `return`, and a call cannot appear in an expression. Every helper that
computes something must deposit the result in a global, which costs one of the
31 user predef slots (`MAX_PREDEF_VARS` = 32, one reserved for `t`) and one
flat command per call.

### What it unlocks

This is the constraint that shapes the **existing** catalog, not a hypothetical
one. The scene-authoring skill already documents the workaround as an authoring
technique:

- *Orrery* went from **29 globals to 16** by moving temporaries to
  function-scoped locals.
- `planetKepler()` sat at **16 params + 15 locals = 31 of 32**
  (`MAX_EXPR_VARS`) until the sphere draw was split into `drawBody()`. The
  documented remedy - "splitting a func's *positioning* args from its
  *appearance* args" - exists only because `planetKepler` could not return a
  position and let the caller draw it.

In the shadow-volume scene the same tax appears as `caps(x0, y0, z0, … z3)` -
twelve parameters and twelve copies of `p + ext*(p - o)` - where
`e0 = extrude(x0, y0, z0)` would do. Expect most non-trivial scenes to shed
globals and parameters.

### Effort

**7-10 dev-days, ~825 LOC.** Not re-derived here - the existing plan carries a
file-by-file table and a day-by-day breakdown. The high-complexity pieces are
`src/repl/eval.c` (+140), a new `src/repl/scalar_eval.c` (+260), and
`src/repl/flatten.c` (+120, propagating a return-reached signal through nested
`for` / `if` / call frames).

### What needs refreshing before it is picked up

The existing plan predates two shipped features and must be reconciled with
them before coding starts:

- Its **"Design Decisions & Assumptions"** section states *"No local variable
  declaration feature is added in this work… Functions will only use parameter
  bindings as local variables."* Function-scoped locals **have since shipped**
  (`CMD_VAR_DECLARE` with `var_idx == REPL_VAR_IDX_LOCAL`; see CLAUDE.md "Float
  declarations"). The scalar evaluator must therefore bind locals as well as
  params, and its side-effect rejection list - which currently rejects
  `CMD_VAR_DECLARE` outright - needs to distinguish a *local* declaration
  (legal inside a scalar function) from a *global* one (still rejected).
- It interacts with `not-started/local-aware-rebake.md`. CLAUDE.md's rule that
  "any dep feeding a local assignment is reported **structural**" exists because
  frozen `FlatCmdLocalVars` snapshots cannot propagate a local forward on a
  value-only rebake. A scalar evaluator re-evaluating return expressions per
  flatten has the same hazard. Read both plans together.

---

## 2. Separate-face stencil + wrapping increment ops

### What is missing

`k_enum_command_specs` has `glStencilFunc`, `glStencilOp` and `glStencilMask`
and no separate-face variants; `k_stencil_ops` has `GL_KEEP`, `GL_ZERO`,
`GL_REPLACE`, `GL_INCR`, `GL_DECR`, `GL_INVERT` and no `_WRAP` forms. Verified
absent from `src/repl/command_spec.c`.

### What it unlocks

**One-pass shadow volumes.** The scene currently draws the hull twice:

```
glCullFace(GL_BACK);  glStencilOp(GL_KEEP, GL_KEEP, GL_INCR); volume();
glCullFace(GL_FRONT); glStencilOp(GL_KEEP, GL_KEEP, GL_DECR); volume();
```

With `glStencilOpSeparate` that is one `volume()` call with culling off - half
the geometry cost and, more importantly, half the source. The same shape
applies to any two-sided stencil technique (CSG, mirrors, portal counting).

`GL_INCR_WRAP` / `GL_DECR_WRAP` are a correctness fix rather than a
convenience: `GL_INCR`/`GL_DECR` **saturate**, so a pixel behind many nested
shadow casters clamps at 255 or 0 and stops cancelling. Fine for one caster,
silently wrong for many.

### Why the existing deferral no longer holds

`done/stencil-buffer-support.md` scoped this as Phase 3 and left it, for a
stated reason:

> It was scoped last on purpose (GL 2.0 procs, `glutExtensionSupported`
> gating, uncertain gl4es support), **and nothing since has needed per-face
> stencil**.

The shadow-volume scene is the first thing that needs it. The technical
concerns in that sentence are still real; the "nothing needs it" half is now
falsified, and this plan is the record of that.

The plan also states the design constraint that decides how `_WRAP` ships:

> The spec tables are `static const` data driving parse and autocomplete;
> making an entry conditional on a probe means a scene that parses on one
> machine is **rejected on another**, which breaks scene-file round-trip and
> export parity… The only clean options are "always present" or "absent"… If
> they are wanted later, add them **unconditionally** (both are core since
> GL 1.4).

So: `_WRAP` goes in **unconditionally** as two rows in `k_stencil_ops`. The
`*Separate` *entry points* are GL 2.0 and do need a runtime gate, but the gate
belongs on the **executor**, not the spec table - exactly the
`glPointParameterfv` precedent (`GLR_NO_POINT_PARAMETER`,
`glr_ctrl_load_point_parameter_proc` at `src/app/glr_ctrl.c:212-239`): the
command always parses and always exports, and a context without the entry point
degrades at execute time.

### Effort

**2-3 dev-days, ~250 LOC.** Larger than a plain `gl-repl-new-command` job for
one specific reason the Phase 3 section already calls out: `glStencilFuncSeparate`
is an *enum-then-ints* shape, and

> `glStencilFunc`'s enum+int shape is a genuine gap in the slot-kind model.
> Custom branches are the established answer (six precedents in
> `try_parse_custom_arg_command`) but add to the parser. **If Phase 3 adds a
> fourth stencil branch, build a proper `ENUM_THEN_INTS` slot kind instead.**

and, in the Phase 3 section itself, that this is the *first* step: migrate
`glStencilFunc` / `glStencilMask` onto the new slot kind rather than adding a
third hand-written parser branch.

Breakdown:

| Piece | Est. LOC | Notes |
|---|---:|---|
| `ENUM_THEN_INTS` slot kind + migrate `glStencilFunc`/`glStencilMask` | ~90 | The actual work; refactor before feature |
| `CmdType` + parser + executor + `flatten_range()` + spec tables ×2 | ~80 | Standard five-edit checklist |
| `GL_INCR_WRAP` / `GL_DECR_WRAP` rows | ~5 | Unconditional |
| Runtime proc gate for the `*Separate` entry points | ~40 | Mirror the `glPointParameterfv` loader |
| GL stub headers | ~15 | `GL_INCR_WRAP`, `GL_DECR_WRAP`, `glStencilOpSeparate`, `glStencilFuncSeparate` - none present in `tests/gl-stubs/include/GL/gl.h` today; add to `gl_stub_counts.h` too |
| Tests + export round-trip + docs | ~20 | |

**Web risk:** gl4es→WebGL2 support for the separate entry points is unverified.
`make test-web` links no GL, so it will not answer the question - this needs a
browser check, and the runtime gate is what makes an unsupported context
degrade rather than break.

---

## 3. `glLightfv` / `glLightf` - program-movable lights

### What is missing

Neither symbol exists in `command_spec.c`. Lights are **app state**, not
program state: `Render3dLight lights[MAX_LIGHTS]` (`MAX_LIGHTS` = 4) lives in
`GlrRenderState`, is seeded from the active theme by
`render3d_lights_apply_theme()`, and is pushed to GL every frame by
`render3d_lights_setup()` (`src/render3d/render.c:636`). Programs can
`glEnable(GL_LIGHT0..3)` but cannot say where a light is or what colour it is.

### What it unlocks

Genuine lighting response - and it is the gap with the most existing evidence
of demand in the tree:

- `tests/scenes/general/multi-light-rig.glr` **does not load**. It was written
  against `glLightfv` and fails with a parse error at body line 21; it is one
  of the pre-existing `make test-scenes` failures. Someone already wrote the
  scene this feature is for.
- `tests/scenes/general/multiple-planar-shadow-projections.glr` carries a
  header comment explaining that it renders unlit because *"The REPL's
  configured GL light position is fixed, so the bright pass below is unlit."*
- `stencil-shadow-volume.glr` fakes the same way, for the same reason.

Three scenes, two of them contorted and one of them broken.

### The ownership decision that must come first

This is **not** a mechanical `gl-repl-new-command` job, which is why it is
costed above the stencil work despite sounding smaller. Three collisions:

1. **Frame ordering.** `render3d_lights_setup()` runs at `render.c:636`; user
   geometry executes at `:770`; both sit inside the outer
   `glPushAttrib(GL_ALL_ATTRIB_BITS)` / `glPopAttrib()` pair at `:625` / `:826`.
   So a user `glLightfv` would naturally override for the remainder of the
   frame's user geometry and be restored at frame end - favourable, but it must
   be *decided* and asserted, not inherited by accident. Note `backdrop.c:1234`
   sets its own lights inside the same span.
2. **The export light bridge.** `ReplExportLightBridge` /
   `glr_ctrl_export_fill_light()` (`src/app/glr_ctrl.c:3524-3541`) copies
   app-owned theme light data into the exported C so it emits `glLightfv`
   init/display blocks. If the *program* can also emit `glLightfv`, exported C
   gets two writers for one piece of GL state and the ordering between them
   becomes load-bearing. Decide whether a program-set light **replaces** the
   bridge's emission for that slot or **precedes** it.
3. **Theme interaction.** Lights are theme-seeded. A program-set light either
   wins until the next theme change, or is re-stomped every frame by
   `render3d_lights_setup()`. Pick one and write it down; the failure mode of
   guessing is a scene that looks right until the user switches theme.

The narrow-and-safe v1: accept `glLightfv(GL_LIGHTn, GL_POSITION|GL_DIFFUSE|
GL_AMBIENT|GL_SPECULAR, (GLfloat[]){…})` with the existing flat-arg shorthand
(same canonicalization as `glMaterialfv` / `glFogfv`), applied **after**
`render3d_lights_setup()`, scoped by the existing attrib pair, with the export
bridge emitting first and the program's own calls overriding in `display()`.

### Effort

**3-5 dev-days, ~350 LOC**, of which roughly a day is the ownership decision
and its documentation in `docs/ARCHITECTURE.md`. The command itself rides the
`glMaterialfv` precedent almost exactly (`GL_LIGHTn` enum slot, pname enum slot,
4-float compound literal, flat shorthand accepted). Add: spec tables, executor,
`flatten_range()`, the export/import path, a `GL_LIGHTING_BIT` interaction check,
and fixing `multi-light-rig.glr` as the acceptance test.

---

## 4. Larger scratch storage

### What is missing

`REPL_SCRATCH_ARRAY_COUNT` = 3 and `REPL_SCRATCH_ARRAY_LEN` = 16
(`src/repl/eval.h:95-100`) - 48 floats total, under three fixed global names,
with no local arrays and no array parameters.

### What it unlocks

Data-driven geometry: meshes, per-particle state, anything the program wants to
*store* rather than recompute. Today a cube's eight vertices consume half the
budget before normals or edge adjacency, which is the sole reason
`volume()` in the shadow-volume scene is unrolled per face instead of looping
over a face table. The language already supports computed indexing -
`bubble-sort-scratch-arrays.glr` runs `A[j] > A[j+1]` against a loop variable -
so a stride convention (vertex *j* at base `3*j`) would work today if there were
room for it. This is a capacity problem, not an expressiveness problem.

### Why it is ranked last despite being the hardest ceiling

Only **6 of 101** scenes in `examples/scenes` + `tests/scenes` touch `A`/`B`/`C`
at all. Nearly every scene is a pure function of `(t, loop index)`, and ships
fine. So this is an absolute wall that is rarely walked into - the opposite
profile from item 1, which everybody pays a little.

### Effort

**1-2 dev-days for the capacity knob.** Both constants are `#ifndef`-guarded,
so widening is a compile-time change, not a rewrite. The cost is *memory in the
copy sites*, and there are more than expected - every one of these copies the
whole block and must be re-measured:

| Site | File |
|---|---|
| `SceneSnapshot.scratch_arrays` | `src/repl/scene_snapshot.h:24` |
| Replay fade baseline | `src/app/glr_ctrl.c:619` |
| Frame-level save/restore | `src/app/glr_ctrl.c:1245`, `:3077`, `:3353` |
| Accum sub-frame baseline | `src/app/glr_ctrl.c:2660`, `:2742` |
| Flatten baseline | `src/repl/flatten.c:2021` |

`SceneSnapshot` is the one to watch: up to `MAX_USER_SCENES` = 8 slots plus a
transient stash, so the block is multiplied.

**Adding array *names* (`D`-`H`) is a separate, larger decision.** It newly
reserves those identifiers - a future `float E;` would be rejected. I checked
the current corpus: `D`, `E`, `F`, `G` appear only inside comment prose
(`R/E`, `D/E`, `E - e*sin(E)`), never as identifiers, so nothing in the tree
breaks today. But the reservation is permanent and the error message
("reserved name") is only clear if the docs list them.

`done/bounded-global-arrays.md` (user-declared `array name(size);`, 16 arrays ×
4096 floats) is the maximal version. It was audited **NOT STARTED** and the
smaller fixed-scratch design shipped instead, deliberately. Do not revive it to
solve a capacity problem that a constant bump solves.

---

## 5. Defects found alongside

Small, unrelated to each other, and worth clearing first because two of them
make verification lie to you.

**`--time` / `GLR_TIME` is not applied on the `--dump-*` boot path.** The time
override is handled in `glr_capture_env.c:312-317`, which runs only on the
windowed/capture path; `glr_boot_run_dumps()` (`src/app/boot/glr_boot_dumps.c`)
loads the session and dumps without it. The scene-authoring skill instructs
authors to "diff `--dump-flat` at several `--time` values to see the real blast
radius" - that recipe silently dumps `t = 0` every time. This cost real
verification effort on the shadow-volume scene, which had to be checked with
time-shifted scene variants instead. **~30 LOC**; the fix is to apply the time
override inside `glr_boot_load_session()`.

**Two `tests/scenes/general` scenes do not load**, and are the 6 failing
assertions in `make test-scenes` / `test_camera_header_parity`:

- `multi-light-rig.glr` - uses `glLightfv` (see §3). Either fix by landing §3,
  or rewrite the scene to the fixed-light idiom in the meantime.
- `shade-model-flat-smooth.glr` - puts two commands on one line
  (`glColor3f(…); glVertex3f(…);`). Mechanical split.

**~0.5 dev-days total.**

---

## 6. Recommended sequencing

1. **§5 defects first.** Half a day, and one of them is actively misleading
   anyone verifying scene work.
2. **§2 separate stencil.** Smallest real feature, has a written design of
   record, and its deferral rationale is now explicitly falsified. Its first
   step (`ENUM_THEN_INTS`) is a refactor that pays down debt the stencil plan
   already flagged as coming due.
3. **§1 float-returning functions.** Biggest payoff, and the only item that
   improves scenes that are already written. Needs the refresh in §1 first.
4. **§3 `glLightfv`.** Gated on an architecture decision, so it wants a design
   pass before an implementation pass. Unblocks a broken scene.
5. **§4 scratch capacity.** Do it when a scene actually wants it. Bump the
   constant; do not build `bounded-global-arrays.md`.

## 7. Explicitly out of scope

**`glMatrixMode` / `glLoadMatrixf`.** Both absent, and they should stay absent.
The controller owns camera and projection (`glr_ctrl_display_frame()` loads the
camera; `render3d` sets projection; the export projection bridge at
`glr_ctrl.c:3509` mirrors it into exported C). Handing the program the
projection matrix punches straight through that ownership boundary.

The one technique that wants it is z-fail shadow volumes ("Carmack's reverse"),
which conventionally uses an infinite far plane so the far cap cannot be
clipped. That is not a good enough reason: `RENDER3D_DEFAULT_FAR_Z` is 200, a
bounded extrusion stays well inside it, and z-fail is otherwise reachable today
by swapping which stencil slot updates. If z-fail is wanted, it is a **scene**
follow-up on `stencil-shadow-volume.glr` (~10 lines, lifting that scene's
"keep the light in front of the cube" restriction), not a language feature.

## 8. Acceptance

Each item should be able to point at a diff in
`tests/scenes/general/stencil-shadow-volume.glr`:

| Item | Expected effect on that scene |
|---|---|
| §1 returns | `caps()` loses ~8 of its 12 params; the twelve `p + ext*(p - o)` lines collapse to a reusable helper |
| §2 separate stencil | Pass 2 draws `volume()` once instead of twice; `glCullFace` flipping disappears |
| §3 `glLightfv` | The ambient/lit two-pass fake can become real lighting; the light marker becomes an actual light |
| §4 scratch capacity | `volume()`'s 96 unrolled lines become a loop over a face table |
