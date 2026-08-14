# `examples/scenes` Consistency Review

## Status - CONSOLIDATED REVIEW (2026-08-14)

This is a read-only review of the 40 shipped `.glr` scenes in
`examples/scenes/`, both example catalogs, the user-facing example lists,
showcase links, capture selectors, exact-name test lookups, and the generated
example goldens. No scene or catalog change has been implemented yet.

The collection is already coherent. Its useful variation follows the catalog
groups: Basics and Functions are compact, GL-state examples are more didactic,
and Showcase scenes allow longer narrative and implementation comments. A
cleanup pass is warranted, but it should concentrate on names and source text
that visibly disagree with the collection's own conventions. It should not
rewrite scene personalities or force every file into one comment template.

All 40 scenes currently pass `scripts/format_scenes.py --check`; both catalogs
pass `scripts/gen_examples.py --check`. The findings below therefore concern
collection-level naming and canonical source spelling, not a broken formatter
or catalog.

---

## Decisions after comparing the prior reviews

The prior reviews correctly identified the three underscore filenames, the
stale snowfall section ID, and the places where a permissive input spelling is
rewritten before appearing in the code panel.

This assessment changes four conclusions:

1. **Drop generic `animated` labels from the spirograph, wave-surface, and
   torus-knot examples.** Keep `animated-ring-for-t.glr`: it is the Basics
   example whose subject is the first-animation mechanism (`for + t`). In the
   other three conceptual examples, animation is neither a useful family name
   nor a discriminator; many neighboring scenes animate without saying so.
2. **Drop `(glFog)` from the catalog name, but do not add `-glfog` to the
   filename.** The scene calls `glFogi`, `glFogf`, and `glFogfv`; there is no
   exact `glFog()` command in the REPL. “Fog ring tunnel” already names the
   effect, while the scene comments and Showcase caption retain API
   discoverability.
3. **If the recursive-tree name is cleaned up, use
   `3d-tree-func-recursion`, not `tree-3d-func-recursion`.** “3D tree” is the
   natural subject and the proposed filename remains the exact slug of the
   catalog name. The stronger collection rule is filename/section/name
   agreement, not forcing the dimension behind every possible subject.
4. **Do not add `glClearColor` to teapot carousel or whale merely for
   uniformity.** The renderer explicitly establishes
   `CFG_DEFAULT_CLEAR_{R,G,B,A}` before a program walk, and the exporter has a
   matching default setup. Their bare `glClear` is deterministic and
   supported. They are structural exceptions, not broken scenes.

---

## Findings

The first five findings form a worthwhile cleanup pass. Findings 6-8 are
lower-priority and should only join that pass if the additional name/reference
churn is acceptable.

### 1. Three filenames use underscores while every other `.glr` file uses hyphens

**Files**

- `lantern_festival.glr`
- `pulse_bars_easing.glr`
- `stencil_mask_window_glstencilop.glr`

**Inconsistency.** The other 37 built-in scenes use kebab-case. These three
catalog entries already use the intended hyphenated section IDs:
`[lantern-festival]`, `[pulse-bars-easing]`, and
`[stencil-mask-window-glstencilop]`. They are also the only `.glr` filenames
anywhere in the repository that contain underscores; the test-scene corpora
use hyphens too.

**Why it matters.** This is the clearest different-era marker in the folder.
It affects tab completion, path prediction, and the otherwise strong
filename/section-ID relationship.

**Smallest change.** Rename only the files to the existing section-ID spelling,
then update `file =` in both catalogs and the affected Showcase links. Do not
lengthen `lantern-festival` with its long catalog-only technique list.

**Precedent.** The other 37 filenames and these entries' own section IDs.

### 2. `animated` is applied inconsistently to three visual subjects

**Files and catalog names**

| Current | Recommended |
|---|---|
| `animated-spirograph-curve.glr` / “Animated spirograph curve” | `spirograph-curve.glr` / “Spirograph curve” |
| `animated-wave-surface.glr` / “Animated wave surface” | `wave-surface.glr` / “Wave surface” |
| `animated-wave-surface-analytic-normals.glr` / “Animated wave surface (analytic normals)” | `wave-surface-analytic-normals.glr` / “Wave surface (analytic normals)” |
| `torus-knot-animated.glr` / “Torus knot (animated)” | `torus-knot.glr` / “Torus knot” |

This is three conceptual examples; the wave surface has a base and an
analytic-normal variant, so four files move.

**Inconsistency.** Animation appears as a prefix for spirograph and wave, as a
suffix for torus knot, and nowhere in the names of many other animated scenes
(`traveling-ripple-ring`, `snowfall-particles`, `fog-ring-tunnel`,
`planar-shadows-glmultmatrixf`, and most Showcase scenes). It therefore does
not form a reliable browsing family or distinguish animated from static
content.

**Why it matters.** The generic adjective pushes the actual visual subject
later in alphabetical/path scanning, and the prefix/suffix split makes the
three related naming decisions look unrelated. Dropping it also makes the
existing base/variant pair read more cleanly:
`wave-surface` / `wave-surface-analytic-normals`.

**Smallest change.** Apply the four file/section/name changes above and update
exact-name consumers. Keep `animated-ring-for-t.glr` and “Animated ring
(for + t)” unchanged: unlike the others, it sits in Basics and explicitly
introduces animation through the predefined `t` variable. Its adjective is
the lesson, not a generic property of the rendering.

**Precedent.** Subject-first names throughout the catalog, the Showcase tiles
already titled “Torus knot,” and the bare-base + descriptive-suffix structure
of the GLU-arrow family.

### 3. “Fog ring tunnel (glFog)” is redundant and names no exact REPL command

**Files.** `examples/catalog.ini`, `examples/catalog-emscripten.ini`, and
user-facing references to the exact display name.

**Inconsistency.** The filename is already the concise, accurate
`fog-ring-tunnel.glr`, but the catalog appends `(glFog)`. The scene actually
demonstrates the `glFogi`, `glFogf`, and `glFogfv` family. By contrast,
parentheticals such as `(glClipPlane)`, `(glStencilOp)`, and
`(glMultMatrixf)` name exact calls, while `(glDepthMask translucency)` names
both an exact call and the technique it enables.

**Why it matters.** The qualifier does not improve discovery—the subject
already says fog—and it looks like an exact command name even though no
`glFog()` spelling exists. The Showcase already uses the cleaner visible
title “Fog ring tunnel” and explains the `glFog*` mechanism in its subcaption.

**Smallest change.** Change only the catalog display name to “Fog ring tunnel”
in both catalogs and update exact-name selectors, the User Guide list, and
goldens. Keep the filename and section ID as `fog-ring-tunnel`. Do not add
`-glfog` or `-glclipplane` filename suffixes.

**Precedent.** Catalog-only mechanism detail is already allowed when the
filename's subject is sufficient (`lantern-festival` is the clearest example),
and Showcase titles put the visual subject first while retaining API detail
below it.

### 4. Snowfall's section ID contains stale, mutable detail

**Files.** The `[snowfall-demo-550-particles]` entry in both catalogs.

**Inconsistency.** The file and display name are `snowfall-particles.glr` and
“Snowfall particles,” but the section ID adds both `demo` and the current
`flakeCount` value. After the underscore renames, it is the only section ID
that does not match its filename stem for accidental rather than abbreviated
or catalog-only reasons.

**Why it matters.** `flakeCount` is a `@tune` knob, so `550` can become false
without touching the catalog. Stable identifiers should describe the example,
not freeze a mutable setting.

**Smallest change.** Use `[snowfall-particles]` in both catalogs. The generated
C symbol changes, but section IDs are not exposed in the runtime example
entry; catalog order and index-keyed fixture positions stay unchanged. Align
the stale “Snowfall demo (550 particles)” wording in
`docs/images/showcase/README.md` if it is intended as a scene title rather than
a capture description.

**Precedent.** The filename stem and display name already agree on “Snowfall
particles.”

### 5. Several source spellings are rewritten before users see the scene

**Files and inconsistencies**

- `lantern_festival.glr`, `snowfall-particles.glr`, and
  `bezier-curve-with-guides.glr` contain nine total `for (` / `if (` sites;
  loaded code uses `for(` / `if(`.
- Bezier's `0.01f` loop step is shown as `0.01`; canonical REPL expression text
  does not retain the suffix.
- `glow-sprites-blend-point-attenuation.glr` and `lantern_festival.glr` use the
  accepted flat shorthand for `glPointParameterfv`; the loader presents the
  argument as `(GLfloat[]){...}`.
- The two `gluColor` sites in `dusk-lighthouse-atoll-stress-test.glr` omit
  alpha; the loader presents them with an explicit fourth argument of `1`.

**Why it matters.** These are not requests to prefer one valid style over
another. The repository source and the code panel show different text for the
same built-in example, which complicates copying, review, and golden diffs.
Built-in sources should normally use their own loader's canonical spelling.

**Smallest change.** Delete the keyword spaces and the one `f`; wrap the two
point-attenuation vectors in `(GLfloat[]){...}`; add `, 1` to the two
three-component `gluColor` calls. No rendered behavior should change, and the
existing goldens should already contain these canonical forms.

**Precedent.** The code-panel goldens, the 26 scenes already using `for(`, the
four-component `gluColor` calls in the GLU-arrow family, and the compound
literals used for other vector-valued GL commands.

### 6. The recursive-tree sibling repeats its construct and orders the name awkwardly

**Files.** The three consecutive Recursion entries:

- `sierpinski-carpet-2d-recursion.glr`
- `sierpinski-sponge-3d-recursion.glr`
- `recursive-3d-tree-func-recursion.glr`

**Inconsistency.** The first two use one occurrence of `recursion` as the
mechanism qualifier. The third says both `recursive` and `recursion` and is
the longest, least scannable member of the group.

**Why it matters.** Differences inside a three-entry catalog family are more
visible than differences between unrelated scenes. Here the repeated construct
does not add meaning; `func` is the useful discriminator because this scene
specifically demonstrates a recursive named function with arguments.

**Smallest change.** If this rename joins the cleanup, use
`3d-tree-func-recursion.glr` and “3D tree (func + recursion).” This removes one
word while keeping the exact filename/section/name slug relationship. Do not
use `tree-3d-func-recursion`: it mimics the Sierpinski filename order at the
cost of the less natural subject “tree 3D” and a new catalog/filename mismatch.

**Precedent.** The catalog's dominant `<subject> (<mechanism>)` presentation
and exact slug coupling. This is lower priority than findings 1-5 because the
current name is still accurate.

### 7. Aurora mixes camelCase and snake_case in one small state block

**File.** `aurora-observatory-dish-tracks-sky.glr`.

**Inconsistency.** `dishAz` and `cycle_length` are declared three lines apart.
Cross-file variation exists and can be intentional—the wave pair, for example,
uses internally consistent mathematical names such as `z_phase` and
`slope_x0`—but Aurora mixes the forms for two ordinary scene-state values.

**Why it matters.** Within-file consistency helps readers distinguish naming
semantics from historical drift. There is no apparent semantic distinction
between these two names that the casing communicates.

**Smallest change.** Rename `cycle_length` to `cycleLength` in its declaration,
three uses/comment references, and nowhere else.

**Precedent.** `dishAz` in the same block and the broader Showcase use of
camelCase (`flyRadius`, `marineSnow`, `spinRate`, `orbitRate`). This is safe but
opportunistic, not a reason for a standalone pass.

### 8. Orrery's tunables are the only variable-panel names in SCREAMING_CASE

**File.** `orrery-labels-track-3d-orbits.glr`.

**Inconsistency.** `ASTEROIDS`, `EARTH_R`, `EARTH_RATE`, `ORB_BASE`, and
`ORB_SCALE` are tagged `@tune`, so they appear in the variable panel. No other
panel-visible tunable is all-caps. Most use camelCase or short lowercase names;
the wave pair's `z_phase` shows that snake_case also exists, but not constant
style.

**Why it matters.** This difference is user-visible, not confined to a
scientific helper. On the other hand, the uppercase notation makes Orrery's
formula-heavy comments resemble conventional orbital tables, so it also
supports that scene's personality.

**Smallest change.** If panel consistency is judged more important, rename the
five to `asteroids`, `earthR`, `earthRate`, `orbBase`, and `orbScale`, including
formula comments. Otherwise leave them deliberately; do not touch internal
orbital-element parameters such as `Drel`, `Yrel`, or `Ldot`.

**Precedent.** All other `@tune` rows support the lowercase/camelCase option;
Orrery's scientific voice supports leaving it alone. This review does not put
the rename in the recommended cleanup batch.

---

## Differences examined and intentionally left alone

- **No `-glclipplane` / `-glfog` filename suffixes.** A command suffix is useful
  when the visual subject would not reveal the API (`planar-shadows`,
  `stencil-mask-window`, `jellyfish`). `clip-planes` and `fog` already name the
  concept. Content and Showcase captions provide command-level searchability.
- **`function-demo-named-func.glr`.** “Demo” is generic, but the primary concept
  is the named function, not the incidental triangles. Renaming it to
  `function-triangles-*` would trade pedagogical accuracy for visual naming and
  does not clear the recommendation bar.
- **Teapot carousel and whale omit `glClearColor`.** This is supported by the
  explicit neutral baseline in `src/app/glr_defaults.h`, applied before each
  program walk. The test deliberately requires ordering only when a scene sets
  its own clear color. A clarifying source comment is reasonable if either file
  is already open, but adding a color is not a consistency fix.
- **`glClearColor` is not adjacent to `glClear` in `glr-logo`, clip planes, and
  planar shadows.** Those scenes place material/lighting setup between the two
  intentionally. Thirty-five scenes use adjacent calls; adjacency is a common
  skeleton, not a semantic rule.
- **Heading spellings.** `// ===== ... =====` is used for scene/major-phase
  banners, while `// --- ... ---` is used for setup/helper sections. Aurora's
  four-dash pass labels and the logo's blank-comment title block are isolated
  but readable. Do not restyle them solely to make the punctuation uniform.
- **Trailing-comment alignment.** Several files align comments in the source;
  the code panel collapses the extra spaces. The alignment is harmless on disk
  and does not warrant a cleanup pass.
- **Second-person instructions.** “Tip,” “Things to try,” and draggable-knob
  guidance cluster in GL-state scenes, where direct instruction is useful.
  Basics remain terse; Showcase scenes carry more narrative. That variation
  follows purpose rather than era.
- **Numeric spelling in colors and camera metadata.** Trailing zeroes are often
  useful when scanning color triples, and camera blocks are machine-written.
  Normalizing them would impose a preference without improving discovery.
- **Scene length and function naming.** The 26-to-458-line range tracks the
  catalog groups. Noun functions (`triangle`, `carpet`, `limb`) and action
  functions (`drawSeat`, `computeLifespan`) both read naturally in context.

---

## Dominant conventions today

### Naming

- Kebab-case filenames.
- The usual relationship is
  `filename stem == catalog section ID == kebab-slug(display name)`.
- Display names are normally `<Subject> (<mechanism>)`: the visual or language
  subject comes first; a construct, technique, or exact GL entry point follows
  only when it improves identification.
- Variant families keep a bare base and add a descriptive suffix:
  `glu-concave-arrow` / `-cutout` / `-extrusion`, and after the recommended
  cleanup, `wave-surface` / `-analytic-normals`.
- `-2d`, `-3d`, `-func`, `-recursion`, and GL-command suffixes are
  disambiguators, not mandatory taxonomy.
- Generic properties shared by much of the catalog—especially “animated”—are
  omitted unless they are the actual teaching concept, as in Animated ring.

### Comment tone and presentation

The dominant voice is neutral and explanatory. Full prose usually uses normal
sentence capitalization; short inline annotations and helper descriptions
often use lowercase fragments. Comments prefer purpose, mechanism, or a useful
constraint over narrating the next obvious call. Basics and early language
examples are sparse, GL-state scenes talk directly to the reader, and long
Showcase scenes explain algorithms and performance-sensitive structure.

That is a coherent tiered style. The earlier claim that the collection's
comments are simply “lowercase” is not supported: comment lines are split
roughly evenly between sentence-initial uppercase prose and lowercase
fragments.

### Structure and code style

Comparable scenes have a recognizable order:

1. `@cfg` and declarations,
2. helper functions,
3. camera block,
4. clear/background setup,
5. GL state,
6. drawing/composition.

Functions precede the camera/body in every scene that defines them. Exceptions
to adjacent clear calls and the two baseline-clear scenes are intentional.
Indentation is already formatter-clean. Variable naming is mostly internally
consistent, with Aurora as the clearest accidental mix and Orrery as a
deliberate scientific-style outlier.

---

## Recommended implementation scope

### Batch A - warranted

1. Hyphenate the three underscore filenames.
2. Remove generic `animated` from the spirograph, wave pair, and torus knot;
   retain Animated ring.
3. Drop `(glFog)` from the Fog ring tunnel display name; do not add command
   suffixes to the fog or clip-plane filenames.
4. Rename the snowfall section ID.
5. Apply the canonical source spellings in finding 5.

### Batch B - optional while names are already moving

6. Shorten the recursive-tree entry to `3d-tree-func-recursion`.
7. Rename Aurora's `cycle_length` to `cycleLength`.

Leave Orrery's all-caps tunables unless there is a separate decision to align
variable-panel labels; that edit is larger and the current notation has a
scene-specific rationale.

### Files that must move together

For file or display-name changes, inspect and update:

- `examples/scenes/*.glr`
- `examples/catalog.ini`
- `examples/catalog-emscripten.ini`
- `docs/USER_GUIDE.md`
- `docs/SHOWCASE.md`
- `docs/images/showcase/README.md`
- `scripts/docs-assets.sh` exact `--example` selectors
- root `README.md` where it uses an exact example name
- exact-name lookups in `tests/test_repl_core_examples.c` and
  `tests/test_repl_flatten_rebake.c`
- current-name comments/lookups in `bench/bench_repl.c`, benchmark baselines,
  tours documentation, and pointer-script documentation
- index-keyed example UI goldens via the supported regeneration target

Do **not** rename `tests/testdata/camera-order/torus-knot-animated.glr`: it is a
frozen pre-migration rejection fixture, not the built-in scene. Historical
plans may retain the old name when describing the state at that time; update
only links or statements intended to point at the current built-in file.

### Verification

```bash
python3 scripts/format_scenes.py --check examples/scenes/*.glr
make check-examples-catalog
make check-user-guide-examples
make rebuild-golden
make test-stubs
make test-scenes
rg -n "lantern_festival|pulse_bars_easing|stencil_mask_window|snowfall-demo-550|Animated spirograph|Animated wave surface|Torus knot \(animated\)|Fog ring tunnel \(glFog\)" \
  examples docs scripts tests bench README.md
```

The final `rg` should leave only deliberate historical or frozen-fixture
references. Catalog order should not change, so numeric example positions stay
stable even though display-name goldens must be regenerated.

---

## Final assessment

The directory already feels like one example collection. The clear outliers
are the three underscore paths, the inconsistent generic `animated` qualifier,
the stale snowfall ID, and the handful of non-canonical source spellings.
`(glFog)` is worth dropping because it is redundant and technically imprecise,
not because every catalog parenthetical must be removed.

A targeted cleanup is warranted. A broad tone, comment, whitespace, or scene
structure rewrite is not.
