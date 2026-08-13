# `examples/scenes` Consistency Review

## Status - NOT STARTED (2026-08-13)

A read-only consistency pass over the 40 shipped `.glr` scenes in
`examples/scenes/`, plus `examples/catalog.ini` and
`examples/catalog-emscripten.ini`, against filename conventions, comment
tone, code style, scene structure, and naming accuracy.

No files were changed. Every claim below was checked mechanically (catalog
slug comparison, golden-vs-source diff, per-convention greps, independent audit
across all 40 built-in scenes and 88 test scenes in `tests/scenes/`) and cites the
files it came from.

## Independent Assessment & Peer Review

An independent re-assessment was conducted across the entire example collection
and test corpora to evaluate the initial review findings:

1. **Underscore vs Hyphen filenames (Finding 1):** Verified. Across all 128 `.glr`
   scenes in the repository (40 in `examples/scenes/`, 60 in `tests/scenes/general/`,
   28 in `tests/scenes/stress/`), exactly three files use underscores:
   `lantern_festival.glr`, `pulse_bars_easing.glr`, and
   `stencil_mask_window_glstencilop.glr`. All 88 test scenes and 37 other example
   scenes use kebab-case. Renaming these three brings the entire repository to 100%
   kebab-case uniformity.
2. **Whitespace / Keyword syntax (Finding 2):** Verified. REPL's parser/serializer
   strictly normalizes `for(` and `if(` without spaces and strips `f` suffixes from
   float literals in geometry code. Aligning source to disk eliminates confusing
   visual diffs between source files and the interactive REPL code panel.
3. **GL state & effects naming (Finding 3):** Verified. In `examples/catalog.ini`,
   all 5 scenes in `group = GL state & effects` display parenthesized GL entry
   points (`(glClipPlane)`, `(glFog)`, `(glStencilOp)`, `(glMultMatrixf)`,
   `(glDepthMask translucency)`), but only 3 filenames reflect the command.
   Renaming `clip-planes-carve-solids.glr` → `clip-planes-carve-solids-glclipplane.glr`
   and `fog-ring-tunnel.glr` → `fog-ring-tunnel-glfog.glr` completes the 1:1
   `filename == slug(catalog name)` mapping and maximizes `grep` discoverability.
4. **Stale Section ID (Finding 4):** Verified. `[snowfall-demo-550-particles]` is the
   sole section ID embedding a mutable particle count and a redundant "demo" tag.
5. **Casing Inconsistencies (Findings 5 & 6):** Verified. `aurora-observatory` mixes
   `dishAz` and `cycle_length` within lines 11–13. `orrery` is the only scene using
   `SCREAMING_CASE` for `@tune` knobs (`ASTEROIDS`, `EARTH_R`, `EARTH_RATE`,
   `ORB_BASE`, `ORB_SCALE`), which directly leaks uppercase identifiers into the
   user-facing REPL variable panel.
6. **Compound Array Literals (Finding 7):** Verified. `glPointParameterfv` in
   `glow-sprites` and `lantern_festival` are the only 2 calls passing flat floats
   rather than `(GLfloat[]){...}`, causing silent REPL load rewriting.
7. **Clear Color Invariant (Finding 11):** Verified. `test_example_clear_color_precedes_clear`
   in `tests/test_repl_core_examples.c` guards with `if (last_clear_color >= 0)`,
   allowing `teapot-carousel` and `whale` to bypass the check entirely. Adding an
   explicit clear color or documented canvas ownership comment resolves this gap.

**Verdict on Peer Review:** All 14 findings are confirmed accurate, high-signal,
and conservative. No unjustified homogenizing changes are proposed, preserving the
personality and pedagogical voice of individual scenes.

## Verdict first

**The collection is in good shape.** It reads as one curated set, not a
grab-bag, and the structural conventions are unusually well kept:

- **Scene skeleton is near-universal.** 38 of 40 scenes run
  `func defs → @camera block → glClearColor → glClear → GL state → geometry`
  with `glClearColor` and `glClear` on adjacent lines. Every scene that
  defines functions puts all of them above the camera block. There is no
  ordering cleanup to do.
- **Filename ↔ catalog coupling is real and mostly honoured.** For 32 of 40
  entries, `filename stem == section id == kebab-slug(catalog name)` exactly.
  That is a genuine, derived convention, not one worth inventing.
- **The variant-suffix families are exemplary.** `animated-wave-surface` /
  `-analytic-normals` and `glu-concave-arrow` / `-cutout` / `-extrusion` both
  use bare-base + descriptive-suffix, and both sets read in an obvious order.
  These are the precedent other groups should be measured against.
- **The palette rollout is already tracked.** Off-palette scenes are on the
  remove-only ratchet in `scripts/baselines/palette-coverage.txt`; that
  process needs no help from this review.
- **Deliberate variation is genuinely deliberate.** The stencil scene's
  `glStencilMask(255)` / `glColorMask(0, 0, 0, 0)` are documented on the line
  itself as teaching the reverse-mapping; the second-person voice clusters in
  the "GL state & effects" group where the scenes are explicitly didactic;
  `jellyfish` shadows globals with parameters and says so. None of that is
  drift.

What follows is the residue: places where the set has two conventions for one
thing, or where a name points at the wrong feature.

---


## Findings

Ranked by (cost of leaving it) × (cheapness of fixing it). 1-4 are worth
doing as a batch; 5-9 are worth doing when the file is already open; 10-14 are
recorded for the record and may well be left alone.

---

### 1. Three scene files use underscores; 37 use hyphens

**Files.** `lantern_festival.glr`, `pulse_bars_easing.glr`,
`stencil_mask_window_glstencilop.glr`

**What.** Every other scene file in the directory is kebab-case. These three
are snake_case — and, tellingly, **their own catalog section IDs are already
hyphenated**:

| section id | file on disk |
|---|---|
| `[lantern-festival]` | `scenes/lantern_festival.glr` |
| `[pulse-bars-easing]` | `scenes/pulse_bars_easing.glr` |
| `[stencil-mask-window-glstencilop]` | `scenes/stencil_mask_window_glstencilop.glr` |

The catalog already states the intended name; only the filename disagrees.
The `tests/scenes/general/` corpus independently uses
`stencil-mask-window.glr` for its own stencil scene, so the hyphen form is the
house convention on both sides of the tree.

**Why it matters.** These are the clearest "different era" marker in the
directory — a tab-completing or `ls`-scanning reader hits two shell-word
conventions in one folder. It is also the one inconsistency where the fix is
purely mechanical and provably intent-preserving, because the desired string
is already written down three lines away.

**Smallest change.** `git mv` each to the hyphenated form, update the `file =`
line in both `catalog.ini` and `catalog-emscripten.ini`, and fix the three
links in `docs/SHOWCASE.md:436,488,540`. Section IDs already match, so catalog
order and the index-keyed goldens are untouched. Re-run
`make check-examples-catalog`.

**Precedent.** The 37 hyphenated filenames, and these entries' own section IDs.

---

### 2. `for (` / `if (` spacing in three files is rewritten before it ships

**Files.** `lantern_festival.glr` (7 sites), `snowfall-particles.glr` (1),
`bezier-curve-with-guides.glr` (1)

**What.** 26 scenes write `for(i, ...)`. These three write `for (i, ...)`, and
`lantern_festival.glr:144` also writes `if (mirror > 0.5)`. The REPL
canonicalizes the spacing on load, so the shipped text differs from the file:

```
disk  (lantern_festival.glr:183)          ships (32.golden.txt:272)
for (i, 0, lanterns) {                 →  for(i, 0, lanterns) {
if (mirror > 0.5) {                    →  if(mirror > 0.5) {
```

`bezier-curve-with-guides.glr:49` carries the same problem twice over —
`for (u, 0, 1, 0.01f)` ships as `for(u, 0, 1, 0.01)`. Canonical REPL text
carries no `f` suffix (`repl_load_apply_line` strips it), so the suffix is
also non-canonical; it is the only stray `f` suffix in scene geometry.

**Why it matters.** This is not a style preference — it is the file on disk
disagreeing with the code panel the user actually reads in the app. Anyone
copying a line out of the source and comparing it to the running scene sees a
mismatch, and the divergence is invisible until you diff against a golden.

**Smallest change.** Delete 9 spaces and one `f`. No behavior change, no
golden churn (the goldens already record the canonical form).

**Precedent.** The 26 scenes using `for(`, and the goldens themselves.

---

### 3. The "GL state & effects" group names its GL command in 3 of 5 filenames

**Files.** `clip-planes-carve-solids.glr`, `fog-ring-tunnel.glr` (no command)
vs `planar-shadows-glmultmatrixf.glr`,
`jellyfish-gldepthmask-translucency.glr`, `stencil_mask_window_glstencilop.glr`
(command present)

**What.** This group has a visible sub-convention — `<effect>-<glcommand>` —
and the catalog names apply it to all five:

| catalog name | filename carries the command? |
|---|---|
| Clip planes carve solids **(glClipPlane)** | no |
| Fog ring tunnel **(glFog)** | no |
| Stencil mask window (glStencilOp) | yes |
| Planar shadows (glMultMatrixf) | yes |
| Jellyfish (glDepthMask translucency) | yes |

So the intent is already recorded in the catalog for all five; two filenames
just do not carry it.

**Why it matters.** This is the group where the GL command *is* the reason the
scene exists, and it is the thing someone greps for. `grep -l glclipplane
examples/scenes/` finds nothing by name today. Within a five-scene group, a
3/2 split is the difference between "a family" and "some files that happen to
be adjacent."

**Smallest change.** Rename two files to `clip-planes-carve-solids-glclipplane.glr`
and `fog-ring-tunnel-glfog.glr`, matching the slug of their existing catalog
names; update section IDs and both catalogs to match. If the longer names are
judged unwieldy, the equally-consistent alternative is to drop `(glClipPlane)`
/ `(glFog)` from the two catalog names — but pick one direction for the group
rather than leaving the split.

**Precedent.** The three siblings that already do it, and these two entries'
own catalog names.

---

### 4. `snowfall` carries a stale section ID naming a count and a "demo"

**File.** `examples/catalog.ini:154`, `catalog-emscripten.ini:158`

**What.** The section ID is `[snowfall-demo-550-particles]`; the file is
`snowfall-particles.glr` and the name is `Snowfall particles`. It is the only
section ID in the catalog that does not equal its filename stem apart from the
three underscore cases in finding 1, and the only identifier anywhere in the
set that hardcodes a magic number.

**Why it matters.** Section IDs are stable IDs, so a stale one is cheap to
leave — but it is also the one place a reader looks to confirm a file's
identity, and "550" invites a future edit to `flakeCount` to silently falsify
it. `docs/images/showcase/README.md:23` already repeats the stale
"Snowfall demo (550 particles)" phrasing.

**Smallest change.** Rename the section to `[snowfall-particles]` in both
catalogs. Nothing keys off section IDs — the goldens are index-keyed and the
grep confirms no test or source file references this string — so this is a
two-line edit. Optionally align `docs/images/showcase/README.md:23`.

**Precedent.** The other 36 section IDs, which equal their filename stems.

---

### 5. `aurora-observatory` mixes camelCase and snake_case in one declaration block

**File.** `aurora-observatory-dish-tracks-sky.glr:11-13`

**What.** Multi-word scene variables are camelCase in 11 files (`fogDensity`,
`flakeCount`, `bladeCount`, `blowHoleX`, `orbitRate`, `marineSnow`, `tentLen`,
`ringZ`, `spinRate`, `flyRadius`, …) and snake_case in 4
(`step_size`, `z_phase`, `slope_x0`, `x_a`). Aurora uses **both, three lines
apart**:

```c
static float dishAz;                // dish azimuth, degrees
static float cycle_length = 30;     // seconds per dome shutter cycle
```

**Why it matters.** Cross-file variation is defensible; within one declaration
block it just reads as an oversight. Aurora is also a Showcase scene, so it is
one of the first files a reader opens.

**Smallest change.** Rename `cycle_length` → `cycleLength` in that one file
(4 occurrences). Leave the two wave-surface scenes and `bezier` alone —
`step_size` / `slope_x0` / `x_a` are internally consistent per file and
`slope_x0`/`slope_z1` genuinely read better with the separator.

**Precedent.** camelCase, on an 11-to-4 file split, and `dishAz` in the same block.

---

### 6. `orrery` is the only scene using SCREAMING_CASE for panel-visible knobs

**File.** `orrery-labels-track-3d-orbits.glr:10,17-20`

**What.** `ASTEROIDS`, `EARTH_R`, `EARTH_RATE`, `ORB_BASE`, `ORB_SCALE` — all
tagged `// @tune`. Every other `@tune` knob in the collection is camelCase or
a short lowercase word: `fogDensity`, `bladeCount`, `waterDroplets`,
`marineSnow`, `tentLen`, `flakeCount`, `depth`, `len`, `rad`, `rise`,
`spread`, `bright`.

**Why it matters.** `@tune` variables are surfaced in the **variable panel**,
so this is user-visible, not internal. A user cycling scenes with the panel
open reads `fogDensity`, then `bladeCount`, then `EARTH_RATE` — the naming
style changes under them for no reason they can see.

**Smallest change.** Lowercase the five to `asteroids`, `earthR`, `earthRate`,
`orbBase`, `orbScale`. Note this touches ~25 reference sites plus four prose
comment blocks that name `ORB_BASE`/`ORB_SCALE`/`EARTH_RATE` in formulas, so
it is a bigger edit than its rank suggests — reasonable to defer until the
file is open for another reason.

**Precedent.** The 13 other scenes carrying `@tune` knobs.

---

### 7. `glPointParameterfv` is the one array-argument call spelled without the cast

**Files.** `glow-sprites-blend-point-attenuation.glr:31`,
`lantern_festival.glr:176`

**What.** Array-argument GL calls in the collection use a bracketed cast —
`glMaterialfv` 25 times, `glClipPlane` 9, `glFogfv` 1, all
`(GLfloat[]){…}` / `(GLdouble[]){…}`. The two `glPointParameterfv` calls pass
bare arguments, and the REPL rewrites them on load:

```
disk : glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 0.2, 0, 0.15);
ships: glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){0.2, 0, 0.15});
```

**Why it matters.** Same class as finding 2 — the disk text is not what the
app shows — plus these two lines are the only place a reader learns the
command's shape, and they teach the non-canonical spelling of it.

**Smallest change.** Wrap both argument lists in `(GLfloat[]){…}`.

**Precedent.** The 35 other array-argument calls.

---

### 8. "Animated" is a prefix three times and a suffix once

**File.** `torus-knot-animated.glr`

**What.** `animated-ring-for-t`, `animated-spirograph-curve`,
`animated-wave-surface` — then `torus-knot-animated`. Catalog names follow the
files: three `Animated X`, one `Torus knot (animated)`.

**Why it matters.** All four sit in the two adjacent catalog groups
("Curves & plots", "Surfaces & tessellation"), so they appear near each other
in the Scene menu, where alphabetical-ish scanning makes the odd one out
visible. It also breaks the qualifier convention: parenthesised qualifiers
elsewhere name a *mechanism* (`(for + t)`, `(nested for)`,
`(analytic normals)`, `(scratch arrays)`), not an adjective already available
as a prefix.

**Smallest change.** Rename to `animated-torus-knot.glr` / `Animated torus
knot`, with matching section ID.

**Precedent.** The three `animated-*` files.

---

### 9. The recursion trio orders its concepts three different ways

**Files.** `sierpinski-carpet-2d-recursion.glr`,
`sierpinski-sponge-3d-recursion.glr`, `recursive-3d-tree-func-recursion.glr`

**What.** Two siblings are `<subject>-<dim>-recursion`:

- `sierpinski-carpet-2d-recursion` → "Sierpinski carpet (2D recursion)"
- `sierpinski-sponge-3d-recursion` → "Sierpinski sponge (3D recursion)"

The third fronts the construct and then repeats it: `recursive-3d-tree-func-recursion`
→ "Recursive 3D tree (func + recursion)". "Recursive" and "recursion" both
appear; the dimension tag has moved from the qualifier into the subject.

**Why it matters.** This is a three-member group in its own catalog `group =
Recursion`, listed consecutively in the Scene menu. It is exactly the case
where an ordering difference reads as accidental rather than expressive, and
the redundancy makes the longest name in the group the least informative.

**Smallest change.** `3d-tree-func-recursion.glr` / "3D tree (func +
recursion)" — drops the duplicated word, restores `<subject>-<qualifier>`
order, and keeps `func +` which is the one thing genuinely distinguishing it
from the two Sierpinski scenes (it is the only one using a recursive
user-defined function with arguments).

**Precedent.** Its two sibling scenes.

---

### 10. `function-demo-named-func` is the only filename whose subject names nothing visual

**File.** `function-demo-named-func.glr`

**What.** Its two group siblings name what is drawn or done —
`function-polygons-args-for`, `function-branching-args-if`. This one names
"demo". The scene draws three triangles via a named `triangle()` func. "demo"
is also the only occurrence of that word in any current filename.

**Why it matters.** In a menu group of three consecutive entries
("Function demo", "Function polygons", "Function branching") the first tells
the reader least, and it is the group's introductory scene — the one a new
user opens first.

**Smallest change.** `function-triangles-named-func.glr` / "Function triangles
(named func)".

**Precedent.** Its two sibling scenes, which name the geometry.

---

### 11. Two Showcase scenes never set a clear color

**Files.** `teapot-carousel-transform-stacks-glow-points.glr`,
`whale-particle-system-lit-model.glr`

**What.** 38 of 40 scenes open with `glClearColor(...)` immediately before
`glClear(...)`. These two call `glClear` with no `glClearColor` at all. Both
enable a backdrop, which is presumably the reason — but `ringed-planet`,
`dusk-lighthouse` and `lantern` also enable backdrops and all three still set
a clear color.

**Why it matters.** `glClearColor` preceding `glClear` is a documented,
guarded project invariant (`test_example_clear_color_precedes_clear`), and the
guard only fires when a clear color exists — so these two sit in the gap
rather than being blessed by it. A reader learning the rule from the catalog
meets two counterexamples with no comment explaining them.

**Smallest change.** Either add the canonical
`glClearColor(0.05, 0.06, 0.08, 1);` above the existing `glClear`, or add a
one-line comment saying the backdrop owns the canvas. Verify the framing is
unchanged before committing — this is the one finding with a visible outcome.

**Precedent.** The 38 scenes that set it, including the three backdrop scenes.

---

### 12. `dusk-lighthouse` is the only scene calling `gluColor` with three arguments

**File.** `dusk-lighthouse-atoll-stress-test.glr:67,77`

**What.** 23 `gluColor` calls across the GLU scenes pass four arguments; these
two pass three, and the REPL appends the alpha (`gluColor(0.08, 0.14, 0.20)`
ships as `gluColor(0.08, 0.14, 0.2, 1)`).

**Smallest change.** Add the explicit `, 1` to both.

**Precedent.** The 23 four-argument calls, all in `glu-concave-arrow*`.

---

### 13. Two section-heading comment styles, both used inside the same files

**Files.** `// --- X ---` in `glr-logo`, `rotating-cube`, `clip-planes`,
`pulse_bars`, `stencil`, `aurora`, `dusk-lighthouse`; `// ===== X =====` in
`aurora`, `dusk-lighthouse`, `ringed-planet`

**What.** The dash form (7 files) marks *setup sections* — "Render State",
"Lighting", "Material Colors", "Pass 1: stamp". The banner form (3 files)
marks *scene titles* and top-level phases — "Aurora observatory: a lonely dish
tracking the sky", "Scene composition". `aurora` and `dusk-lighthouse` use
both.

**Why it matters.** Read that way the two forms are not actually in conflict —
they encode two different levels. That is worth **writing down** rather than
changing, because right now it is a pattern a new scene has to reverse-engineer
and is as likely to get backwards as right.

**Smallest change.** One sentence in the `gl-repl-scene-authoring` skill:
banner `=====` for the scene title/major phase, dashes `---` for setup
sections. No file edits.

---

### 14. Column-aligned trailing comments are collapsed before they ship

**Files.** `torus-knot-animated`, `glow-sprites-blend-point-attenuation`,
`jellyfish`, `lantern_festival`, `orrery`, `parametric-torus-nested-for`

**What.** These six align trailing comments into a column; the REPL collapses
the run of spaces to one:

```
disk : static float count = 160;       // number of glow points
ships: static float count = 160; // number of glow points
```

**Why it matters.** The alignment is invisible to every user of the app, so it
is effort spent on a view nobody sees — and it makes the *source* look like it
has a second comment style that the shipped scenes do not have.

**Smallest change.** Do **not** reformat the six files; the alignment is
harmless and arguably nicer on disk. Add one line to the
`gl-repl-scene-authoring` skill noting the collapse so nobody invests in
column alignment on the assumption it survives.

---

## Examined and deliberately not raised

- **Numeric trailing zeros** (`glColor3f(0.40, 0.82, 0.50)` shipping as
  `0.4, 0.82, 0.5`). This affects most of the collection and is the single
  largest source of source-vs-shipped divergence — but the aligned decimal
  form is *more* readable in a column of color triples, and normalizing it
  would be imposing a preference for no reader benefit. Leave it.
- **`glClearColor(..., 1)` vs `(..., 1.0)`** — an 18/14 split with no dominant
  form and no visible effect. No precedent to appeal to; not worth a rule.
- **`glr-logo.glr` vs name "gl-repl logo"**, and
  `aurora-observatory-dish-tracks-sky` vs "…dish tracks **the** sky". Slug
  mismatches caused by an established abbreviation and a dropped article. Both
  benign; renaming would cost more than it returns.
- **Camera-block literal formats** (`-6.00f` vs `-6.5000f` vs bare `-6`,
  and the lone `-115f` at `whale-particle-system-lit-model.glr:159`). These are
  machine-written metadata, hidden in Code Focus mode and rewritten on save.
  `-115f` was checked and parses correctly — it resolves to `-115.0000f` — so
  there is no defect here, only three export eras.
- **Second-person voice** ("Drag `fogDensity`…", "Tip: park the cursor…",
  "Things to try:"). Concentrated in the GL state & effects group, which is
  the didactic group. That is a tone matching a purpose, not drift. The
  "Things to try:" exercise footer in `stencil` is unique in the collection;
  extending it to `clip-planes` and `fog-ring` would be a content decision,
  not a consistency fix.
- **`// leave shared GL state the way we found it`**, present in 3 of the ~6
  scenes that restore blend/depth-mask state. Worth copying into the others
  if they are touched; not worth a pass of its own.
- **Off-palette canvas colors** in `annotated-orbit-plot-labels` (`0.06, 0.07,
  0.09`) and `clip-planes` (`0.04, 0.05, 0.08`), which look like near-misses of
  the canonical `0.05, 0.06, 0.08` rather than deliberate moods. Both files are
  already on the `scripts/baselines/palette-coverage.txt` ratchet, so this is
  tracked work with an owner; no separate action.
- **Scene length spread** (26 to 458 lines). Correlates with catalog group —
  Basics is short, Showcase is long — which is the intended shape.

---

## Summary

### Dominant naming convention

Derived from the 32 of 40 entries that already agree:

> **`filename stem` == `section id` == kebab-slug of the catalog `name`**,
> where the name is `<Subject> (<mechanism>)` — subject first, mechanism in
> parentheses, dimension tags (`2d`/`3d`) inside the mechanism qualifier.

Mechanism qualifiers name a construct (`args + for`, `nested for`,
`vars only`, `func + recursion`), a GL entry point (`glMultMatrixf`,
`glStencilOp`, `glDepthMask`), or a technique (`analytic normals`,
`scratch arrays`, `blend + point attenuation`). Variants of an existing scene
extend the base name with a suffix (`-cutout`, `-extrusion`,
`-analytic-normals`) rather than being renamed.

### Dominant comment/tone style

Lowercase, explanatory, mechanism-first prose in `//` line comments, placed
directly above the code it explains, with the *why* preferred over the *what*
("Emit the high-x edge first so the upward-facing triangles are CCW"). Longer
scenes open with a 3-8 line description block. Em-dash asides use `--`.
Section headings use `// --- Title ---` for setup groups and
`// ===== Title =====` for the scene title. The didactic group additionally
addresses the reader directly and names draggable knobs in backticks.

### Clear outliers

1. The three underscore filenames — `lantern_festival`, `pulse_bars_easing`,
   `stencil_mask_window_glstencilop`. These are also the two Showcase files
   lacking a mechanism qualifier and the three files whose section IDs already
   disagree with them; they read as a different era of the collection.
2. `lantern_festival` again, as the only file using `for (` / `if (` throughout.
3. `orrery`'s SCREAMING_CASE tunables, the only ones in the variable panel.
4. `recursive-3d-tree-func-recursion`, the only filename naming its construct twice.

### Is a cleanup pass warranted?

**Yes, but a small and mostly mechanical one.** Findings 1-4 are worth doing
as a single batch: they are ~15 lines of catalog edits, five `git mv`s, three
doc-link fixes and nine deleted spaces, they need no golden regeneration
(section IDs and catalog order are preserved), and they remove every outlier
that makes the directory look like it came from two projects. Finding 11 is
worth checking visually at the same time.

Findings 5-10 and 12 are real but small; fold them in opportunistically as
those files are edited. Findings 13-14 are documentation-only — two sentences
in the `gl-repl-scene-authoring` skill — and are arguably the highest
value-per-keystroke items here, because they stop the next scene from
re-introducing the drift.

Nothing in this review calls for rewriting comments or homogenizing scene
personality. The individual voices are working.

## Verification after any of the above

```bash
make check-examples-catalog      # catalog schema + file paths
make check-palette               # scene literals vs accent_palette.h
make test-scenes                 # opt-in corpora, includes export/trace parity
grep -rn "lantern_festival\|pulse_bars_easing\|stencil_mask_window" docs/ examples/
```
