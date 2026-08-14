# Built-in Tutorial Catalog Review

## Status - landed, 2026-08-14

Findings 1-12, Tier 1 (A-D), and Tier 2 F shipped. The catalog is 29
tutorials. Function Scope & Locals (E) is blocked: COMMAND steps cannot
carry a `float` declaration. G, H, and Tier 3 were not scheduled.

| Finding | What shipped |
|---|---|
| 1 | Lighting run: Normals → Flat & Smooth → Materials → Culling → Two-Sided |
| 2 | Feature Tour retargeted; Depth Test Triangle retired |
| 3 | Closing takeaway NOTE on First Triangle, Color & Transform, GLUT Solids, Lighting Basics, Color Interpolation, If & Conditionals |
| 4 | Two-Sided Lighting rewritten in the catalog voice |
| 5 | Scaffold comments and Functions takeaway no longer say "scaffold" / "funcN" |
| 6 | Variable Slider / Bitmap Text → REPL Language; GLUT Solids and Materials drop the wrong extra tag |
| 7 | Learner-facing "example" → "tutorial" |
| 8 | Line stipple masks taught as `0x00FF` / `0xAAAA` |
| 9 | First Triangle names vertex labels; Scene Chrome states chrome vs program |
| 10 | Depth Mask draws the mistake, then splices `glDepthMask(GL_FALSE)` |
| 11 | First Animation draws a stood-up cone |
| 12 | Color & Transform is 2D + STEP_CMD corners; `@config` explained |
| A-D | Transform Stacks, Expressions & Motion, Watching a Program Run, Keeping Your Work |

The original review text is unchanged below.

This is a review of the **25 shipped tutorials**
in [`src/repl/tutorials.c`](../../../src/repl/tutorials.c) as a *set* - their
progression, voice, terminology, and coverage - plus the catalog metadata
(tags, subheadings, order) that decides how a learner encounters them. The
runner (`src/subsystems/tutorial/`) and the step-kind machinery are out of
scope except where the machinery leaks into learner-facing text.

Catalog order, as `./gl-repl --list-tutorials` prints it:

| # | Tutorial | Band | Tags | Steps |
|---|---|---|---|---|
| 1 | First Triangle | Beginner | Geometry | 5 |
| 2 | Color & Transform | Beginner | Color & Transforms | 11 |
| 3 | Feature Tour | Beginner | Geometry | 9 |
| 4 | Variable Slider | Beginner | Color & Transforms | 7 |
| 5 | First Animation | Beginner | Animation | 5 |
| 6 | Points & Lines | Beginner | Geometry | 13 |
| 7 | GLUT Solids Tour | Beginner | Geometry, Depth & Lighting | 12 |
| 8 | First Loop | Beginner | REPL Language | 9 |
| 9 | Depth Test Triangle | Intermediate | Geometry, Depth & Lighting | 6 |
| 10 | Lighting Basics | Intermediate | Depth & Lighting | 6 |
| 11 | Color Interpolation | Intermediate | Color & Transforms | 3 |
| 12 | Line Stipple | Intermediate | Effects | 10 |
| 13 | Blending & Transparency | Intermediate | Effects | 17 |
| 14 | Depth Mask & Draw Order | Intermediate | Effects, Depth & Lighting | 10 |
| 15 | Fog | Intermediate | Effects | 5 |
| 16 | Clip Planes | Intermediate | Effects | 4 |
| 17 | Materials & Shininess | Intermediate | Depth & Lighting, Effects | 5 |
| 18 | Flat & Smooth Shading | Intermediate | Depth & Lighting | 5 |
| 19 | Normals | Intermediate | Depth & Lighting | 20 |
| 20 | Two-Sided Lighting | Intermediate | Depth & Lighting, Geometry | 22 |
| 21 | Culling & Winding | Intermediate | Geometry | 16 |
| 22 | Bitmap Text | Intermediate | Geometry | 4 |
| 23 | Functions | Advanced | REPL Language | 12 |
| 24 | If & Conditionals | Advanced | REPL Language, Animation | 7 |
| 25 | Scratch Arrays | Advanced | REPL Language | 11 |

---

## What already works, and should not be disturbed

The catalog's best lessons are genuinely good and give the rest a template
worth copying:

- **Flat & Smooth Shading** (`tutorials.c:864`) is the model entry: draw the
  geometry, observe it, splice the state change in *above* it with `STEP_AT`,
  observe the same geometry again. Before/after on identical geometry is the
  strongest teaching shape the runner offers, and this is the cleanest use.
- **Normals** (`tutorials.c:921`) makes a point that no single lit surface can
  make - two identical quads, different normals - and stages the overlay it
  needs with `STEP_SET_QUIET` instead of a narrated detour.
- **Two-Sided Lighting** (`tutorials.c:1018`) is the only entry that teaches
  *diagnosis* rather than a feature, and the ordering of its evidence
  (normal → light → winding) is right.
- The **staging conventions block** (`tutorials.c:174-206`) and the per-entry
  camera/size rationale comments are excellent maintenance documentation. The
  reasoning recorded at `tutorials.c:419` (why `n` targets 2.5, not 10),
  `tutorials.c:538` (why point size is 12), and `tutorials.c:756` (why fog
  density and camera distance are one setting) is exactly what keeps a later
  editor from "simplifying" a tuned value back into a broken lesson.

The findings below are about the collection, not about rewriting these.

---

## Findings

Ranked roughly by how much a learner is affected. Findings 1-6 are the ones
worth a pass; 7-12 are smaller and only worth doing alongside them.

### 1. The lighting/shading run is ordered so each lesson depends on the next one

**Where.** Catalog entries 18-21 (`tutorials.c:1410-1439`).

Three dependencies run backwards:

- **Two-Sided Lighting (20) before Culling & Winding (21).** Two-Sided's whole
  payoff is that clockwise vertices make the quad back-facing, its evidence
  step is the winding overlay, and its fix is `glFrontFace(GL_CW)`
  (`tutorials.c:1046-1062`). Culling & Winding is the entry that *introduces*
  winding order, front/back classification, `glFrontFace`, and that same
  overlay (`tutorials.c:1085-1116`). A learner walking the catalog in order
  meets the hardest consequence of winding before the concept.
- **Flat & Smooth Shading (18) before Normals (19).** Flat & Smooth explains
  GL_SMOOTH as interpolating "the per-vertex lighting across each facet"
  (`tutorials.c:868`) - per-*vertex* lighting is meaningful only once a normal
  is understood as supplied per-vertex data, which is precisely what Normals
  teaches at `tutorials.c:933`.
- **Color Interpolation (11)** shows a gradient across a triangle without ever
  saying the mechanism is GL_SMOOTH; the entry that names it is seven lessons
  later.

**Why it matters.** These are the four entries where the catalog most looks
like a curriculum rather than a pile of demos, and the dependency order is the
one thing a curriculum has to get right.

**Recommendation.** Reorder the Depth & Lighting run to
Lighting Basics → **Normals** → **Flat & Smooth Shading** → Materials &
Shininess → **Culling & Winding** → **Two-Sided Lighting**, and add one
sentence to Color Interpolation's closing (it currently has none - see finding
4) pointing at the shade model. No step text changes; only `g_tutorials[]`
order, which is cheap here because subheadings stay Intermediate throughout.

**Constraint for whoever implements it.** `test_catalog_subheading_metadata`
requires each subheading to form a single contiguous run *per tag*, and
`test_tutorial_runner.c:246-249` pins entries 0 and 1 by name. Both survive
this reorder (it moves only Intermediate entries among themselves), but they
constrain anything more ambitious.

### 2. Two entries exist to exercise the runner, not to teach GL - and they sit early

**Where.** *Feature Tour* (entry 3, `tutorials.c:341`) and *Depth Test
Triangle* (entry 9, `tutorials.c:289`).

Their own header comments say so. Feature Tour "exercises the non-COMMAND step
kinds and the relaxed step shapes" (`tutorials.c:330`); Depth Test Triangle is
described as "This label-targeted tutorial…" (`tutorials.c:284`) - a
description of the placement mechanism, not of a lesson.

The costs are concrete:

- **Feature Tour re-draws the same triangle a learner just drew twice** (it is
  entry 3, after First Triangle and Color & Transform), then showcases two
  grid backdrop themes. Its actual subject - scene chrome, overlays, and the
  fact that the app's presentation settings are separate from the program - is
  a real and useful subject, but it is neither named by the title nor claimed
  by the tag (`Geometry`, which it does not teach).
- **Depth Test Triangle demonstrates nothing observable.** It draws one
  triangle and then splices `glEnable(GL_DEPTH_TEST)` above it. With a single
  primitive there is nothing to occlude, so the before and after images are
  identical - the lesson asserts an effect the screen never shows. Compare
  entry 14 (Depth Mask & Draw Order), which owns the same subject with two
  objects.

**Why it matters.** These are positions 3 and 9 in a 25-entry path, and both
are entries where a learner does work and sees no new idea. Depth Test Triangle
in particular is the catalog's only lesson whose promised visual result cannot
occur.

**Recommendation.**

- Retarget **Feature Tour** as what it already is: rename it to something like
  *Scene Chrome & Overlays*, retag it (drop `Geometry`), and replace the
  triangle build with the shortest possible geometry, so the steps spend
  themselves on the overlay/theme material. It is genuinely useful as an
  orientation lesson - it is the only place the app's own view settings are
  introduced at all (see finding 9).
- Either give **Depth Test Triangle** a second overlapping primitive at a
  different Z (so disabling/enabling depth test visibly changes which one
  wins), or fold it into Depth Mask & Draw Order and drop the entry. The
  `STEP_AT` mechanism it demonstrates is already exercised by Fog, Flat &
  Smooth Shading, Color Interpolation, and Two-Sided Lighting, so nothing is
  lost by retiring it.

### 3. Beginner lessons end without a takeaway; almost every later lesson has one

**Where.** Entries 1, 2, 4, 7, 9, 10, 11, 24 end on their last command or
`STEP_AT`; entries 5, 6, 8, 12-23, 25 end on a summarizing `STEP_NOTE`.

Concretely: First Triangle ends on "Close the batch and the filled triangle
appears in the scene." (`tutorials.c:237`); Color & Transform ends on
`glPopMatrix()`; Lighting Basics ends on the sphere draw; GLUT Solids Tour ends
mid-row on a comment-less `glutSolidCone` step; If & Conditionals ends on the
cube it draws.

**Why it matters.** The closing NOTE is where the catalog states the
transferable rule ("Translucent objects usually render last, with depth testing
on and depth writes off", `tutorials.c:744`). The lessons that skip it are
disproportionately the beginner ones, where the learner is least able to infer
the rule themselves. It also reads as two different eras of authoring within
one menu.

**Recommendation.** Add one closing `STEP_NOTE` to entries 1, 2, 7, 10, 11 and
24. One sentence each, stating the rule rather than recapping the steps -
e.g. for Color & Transform, that transforms accumulate and push/pop is what
bounds them; for Lighting Basics, that the four `glEnable` calls are the
minimum lit pipeline and the shape's normals come from the GLUT solid. Skip
Variable Slider (ends on a REQUIRE_VAR, which is its own payoff) and
Depth Test Triangle (see finding 2).

### 4. Two-Sided Lighting speaks in a voice no other tutorial uses

**Where.** `tutorials.c:1036-1057`: "Weird, the +Z normal faces the key
light…", "First suspect is the normal. Let's inspect…", "Okay, the normal is
right; let's verify…", "Ah, red means back-facing.", "Let's turn the winding
view off."

Every other entry in the catalog is written in neutral imperative or
descriptive voice ("Enable blending so alpha can mix…", "Draw a dense sphere so
its specular highlight reads smoothly").

**Why it matters.** The narrative voice is genuinely effective *for this
lesson* - it is a debugging story and the false-start structure is the point.
But it is unique, so a reader who arrives here from Normals hears the catalog
change personality, and a future author has two incompatible models to copy.

**Recommendation.** Keep the diagnostic structure; drop the interjections
("Weird,", "Okay,", "Ah,") and the first-person plural, so the steps read as
the same voice doing an investigation: "The +Z normal faces the key light, yet
the sheet receives no diffuse light." / "The normal is the first suspect -
place the cursor on the glNormal3f row to read the focused normal guide."
Alternatively, decide the narrative voice is the house style for diagnostic
lessons and say so in the authoring comment - but then it needs a second
adopter, and today it has none.

### 5. Tutorial-system vocabulary leaks into learner-facing text

**Where.**

- `tutorials.c:291` - "Start the triangle batch; **the label here anchors a
  later insert**." A learner has no concept of a step label or a placement
  anchor; these are catalog-authoring terms from `tutorials.h`. The sentence
  describes the tutorial engine, not OpenGL.
- Every preloaded scaffold comment says **"scaffold"**: "Lighting scaffold
  shared by every solid in this tour." (`tutorials.c:587`), "Minimal lighting
  scaffold for explicit material properties." (`tutorials.c:821`), "Lighting
  scaffold for the shade-model comparison." (`tutorials.c:847`), "Lighting
  scaffold for the surface-normal demonstration." (`tutorials.c:881`),
  "Lighting scaffold for the two-sided lighting lesson." (`tutorials.c:976`).
  `setup` scaffold is the name of the `TutorialEntry` field, not a word the
  learner has met.
- `tutorials.c:1169` - "Named calls compile to the REPL's **funcN slots** while
  the source keeps the readable name." `funcN` appears in no earlier tutorial.

**Why it matters.** These rows are locked into the learner's document and
survive into the scene they keep afterwards. The scaffold comments are the
first thing five different lessons show, and they currently describe the
lesson's construction rather than the code.

**Recommendation.** Rewrite the five scaffold comments to say what the locked
lines *do* and where the learner will learn them - e.g. "These locked lines are
the minimal lit pipeline from the Lighting Basics tutorial." Drop the label
clause at `tutorials.c:291` (or drop the entry per finding 2). Either introduce
`funcN` in the Functions lesson body or drop the mention.

### 6. Three tags are wrong, and one of them hides the catalog's language track

**Where.** `g_tutorials[]`, `tutorials.c:1271-1469`.

- **Variable Slider is tagged `Color & Transforms`** (`tutorials.c:1300`). It
  teaches declaring a `float`, referencing it from geometry, and driving it
  from the variable panel. Nothing in it is a color or a transform. It belongs
  in `REPL Language`, where it would be the *first* language lesson - the
  declaration lesson that Functions, If, and Scratch Arrays all assume. The
  subheading-contiguity test permits this: Variable Slider (4) and First Loop
  (8) are both Beginner, so the tag's Beginner run stays contiguous.
- **GLUT Solids Tour is tagged `Depth & Lighting`** (`tutorials.c:1326`) but
  teaches neither; its lighting comes preloaded and locked, and it never
  mentions it. Because it is entry 7, it is the *first* thing in the Depth &
  Lighting flyout, ahead of Lighting Basics. Dropping the tag leaves that
  flyout all-Intermediate and contiguous.
- **Bitmap Text is tagged `Geometry`** (`tutorials.c:1444`). `label()` is a
  REPL primitive that draws no geometry. `REPL Language` fits; so would a text
  tag if one is ever added.
- Lower confidence: **Materials & Shininess carries `Effects`**
  (`tutorials.c:1407`) alongside Depth & Lighting. Materials are the lighting
  model, not an effect. Worth dropping if the tag pass happens anyway.

**Why it matters.** The tag flyouts are how the Tutorials menu is browsed. A
learner who picks "REPL Language" today gets one beginner loop lesson and three
Advanced entries, with the declaration lesson they need filed under color.

### 7. Terminology drifts across the catalog for the same things

- The unit of instruction is called an **example** ("so this example can clean
  up after itself", `tutorials.c:250`), a **tour** ("shared by every solid in
  this tour", `tutorials.c:587`), a **demonstration** (`tutorials.c:881`), and
  a **lesson** (`tutorials.c:976`). "Example" is the worst of these: `Examples`
  is a distinct, user-visible catalog in the Scene menu, so the word names
  something else in this app.
- **"REPL"** means the language and command model everywhere in the project's
  docs, but Feature Tour's opening note says "A quick tour of the REPL's scene
  features" (`tutorials.c:343`), where it means the application.

**Recommendation.** Settle on "tutorial" in learner-facing text and reserve
"example" for the Scene-menu catalog. Say "the app" or name the feature
directly instead of "the REPL" when the subject is chrome.

### 8. Line Stipple teaches a bit mask in decimal, contradicting the app's own help

**Where.** `tutorials.c:678` - "The mask is 16 bits: 255 reads as dashes, while
43690 produces dots."

The command's own inline help says the opposite spelling:
`command_spec.c:347` - "expression or as a 0..65535 decimal or 0xNNNN literal
(**0xAAAA = dots**)". The parser accepts hex (`command_spec.c:702`), and the
evaluator preserves `0xNNNN` literals through export (`eval.c:1781`).

**Why it matters.** The lesson's stated subject is that the pattern *is* 16
bits; `43690` is exactly the spelling that hides the bit pattern, and a learner
who right-clicks `glLineStipple` for help immediately reads a different
convention from the one they were just taught.

**Recommendation.** Use `0x00FF` and `0xAAAA` in the step and the note, and let
the note say why: alternating bits give dots, a run of eight gives dashes.

### 9. Presentation settings change under the learner with no explanation

Five tutorials silently switch the app into 2D (`view_mode =
RENDER3D_VIEW_2D`: entries 1, 6, 11, 12, and 13; entry 11's override lives in
the Color Interpolation scaffold), several change the grid theme, backdrop, or
overlay set, and six preload a camera pose. Nothing in any step text mentions
that the *view* is an app setting rather than something the program did.

Related, and the reason this is one finding rather than a nitpick: **First
Triangle turns on `vertex_labels = OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE`**
(`tutorials.c:317`) - so the learner's very first screen has index and
world-coordinate labels next to each vertex, exactly matching the numbers they
typed - and no step says a word about them. That is the single best teaching
aid in the catalog, switched on and left unremarked.

**Recommendation.** One `STEP_NOTE` in First Triangle naming the vertex labels
and pointing out they show the coordinates just typed; and make the retargeted
Feature Tour (finding 2) the place where "these are app view settings, not
program state" is stated once for the whole catalog.

### 10. Depth Mask & Draw Order tells rather than shows

**Where.** `tutorials.c:722-746`. The lesson enables depth test, draws an
opaque cube, enables blending, sets `glDepthMask(GL_FALSE)`, draws a
translucent sphere, restores the mask, and closes with the rule.

The learner only ever sees the *correct* result. The mistake the lesson exists
to prevent - translucent fragments writing depth and occluding the rest of the
translucent surface - never appears on screen. The catalog already has the
right shape for this (Flat & Smooth Shading, finding "what already works"):
draw it wrong, look, then splice the fix in above with `STEP_AT`.

Secondary: the cube is drawn before any color is set and no lighting is
enabled, so both objects are flat silhouettes - and this entry ships
`g_tutorial_dense_solid_cfg`, which turns the vertex points and outlines off,
removing the remaining shape cues.

**Recommendation.** Restructure as draw-wrong-then-fix: label the
`glutSolidSphere` step, let it render with depth writes on, add a NOTE on what
looks wrong, then `STEP_AT` the `glDepthMask(GL_FALSE)` above it. Add a
lighting scaffold (`setup`) so the two solids read as solids, matching what
Materials, Flat & Smooth, Normals and Two-Sided already do.

### 11. First Animation's cube is unlit and depth-test-free

**Where.** `tutorials.c:467-483`. The program is `glRotatef(t * 45, 0, 1, 0)`
followed by `glutSolidCube(2)`, with no lighting, no material color, and no
`glEnable(GL_DEPTH_TEST)`.

With no lighting, every face takes the same default color, so the spinning cube
is a single-color silhouette whose only visible change is its width. What
actually reads as motion is the tutorial chrome's vertex points and outlines
(`CFG_DEFAULT_TUTORIAL_VERTEX_*`) - host overlays that are not part of the
program the learner is writing. Compare `examples/scenes/rotating-cube.glr`,
which is the same idea and enables depth test, materials, and four lights.

**Why it matters.** This is the lesson whose entire payoff is "press Ctrl+T and
watch it spin". It should be the most legible frame in the catalog. It is also
the first solid a learner draws, and the impression it leaves - that a GLUT
solid looks like a white blob - is corrected only three lessons later.

**Recommendation.** Keep this lesson self-contained and switch the geometry to
something whose silhouette carries the rotation (a cone or asymmetric assembly
would work). Do not preload the lighting pipeline here: First Animation precedes
Lighting Basics, so unexplained locked lighting state - especially a scaffold
comment pointing forward to that later lesson - would create the same backward
dependency finding 1 argues against. If a lit cube is strongly preferred, move
Lighting Basics ahead of First Animation and then reuse its scaffold explicitly.

### 12. Smaller items

- **`// @config` is used before it is explained.** Variable Slider's first step
  makes the learner commit `float n = 1; // @config the triangle's size…`
  (`tutorials.c:402`). `@config` is a real tag with a specific meaning (keeps
  the variable-panel row bright - `docs/USER_GUIDE.md:1198`) and this is its
  only appearance in any tutorial. Either explain it in the same step's closing
  note or drop the tag from the lesson.
- **`glVertex2f` appears exactly once, unremarked.** First Triangle uses
  `glVertex2f` (`tutorials.c:230-236`); every other flat lesson uses
  `glVertex3f` with an explicit `0`. Nothing says the two are related. One
  clause in First Triangle's new closing note fixes this.
- **Color & Transform runs in 3D while every other flat-figure lesson forces
  2D**, and its step text says the rotation is "around Z, the screen-facing
  axis" (`tutorials.c:258`). Under the shared tutorial pose (yaw 30, pitch 20)
  Z is not screen-facing, and the square is viewed obliquely. Either add
  `view_mode = RENDER3D_VIEW_2D` to make the sentence true, or reword it.
- **Color & Transform narrates all four corners of the quad** with near-
  identical sentences (`tutorials.c:264-274`) immediately after First Triangle
  narrated three vertices the same way. The catalog's own convention for
  repeated geometry is `STEP_CMD` (comment-less), used from entry 3 onward.
  Converting the last three corner steps trims the repetition without losing
  anything.
- **`glClearColor` is never demonstrated.** Every tutorial gets a locked
  `glClear` prelude with the comment "Clear the color and depth buffers so each
  frame starts fresh." (`tutorial_runner.c:708`), but no lesson shows that the
  background is settable, and none mentions the ordering rule that
  `glClearColor` must precede `glClear` - a rule the project flags as a trip
  wire and guards with a test for the example catalog. A learner writing their
  first scene from scratch hits it immediately.

---

## Coverage gaps

The catalog covers the GL state surface well - fog, clip planes, blending,
depth mask, stipple, materials, shading, normals, winding, culling all have
entries. The gaps are in three other places: the REPL language's harder half,
the app's own workflows, and the transition from beginner geometry to composed
scenes.

### Tier 1 - the gaps a learner actually walks into

**A. Transform Stacks & Hierarchy** *(Beginner→Intermediate bridge, after
First Loop)*
Teaches: nested `glPushMatrix`/`glPopMatrix`, that transforms accumulate and
compose in source order, and that a child transform is relative to its parent.
Shape: a body, an arm rotating off it, a hand rotating off the arm - each level
one push/pop deeper. Why it is needed: Color & Transform introduces push/pop in
one line as bookkeeping ("clean up after itself"), First Loop uses it as
per-iteration hygiene, and GLUT Solids Tour deliberately omits it so its
`glTranslatef(3, 0, 0)` steps accumulate - three different mental models across
three lessons and no lesson that owns the concept. This is also the single
biggest step between "draw one shape" and every showcase example in
`examples/scenes/`.

**B. Expressions & Motion** *(Beginner, after First Animation)*
Teaches: that any numeric argument is an expression, and the math built-ins -
`sin`/`cos` for oscillation, phase offsets, `lerp`/`smoothstep` for easing,
`rand(i)` for repeatable per-index variation. Why it is needed: the catalog
currently teaches the built-ins only in throwaway closing lines - "Loop
expressions can also use sin(i), cos(i), and the other math built-ins"
(`tutorials.c:648`) and "rand(i) can generate a repeatable value for each
index" (`tutorials.c:1229`) - which is a list, not a lesson. Meanwhile
`If & Conditionals` opens with `if(sin(t) > 0)` and assumes it reads.

**C. Watching a Program Run (Replay)** *(Intermediate, app workflow)*
Teaches: Ctrl+R replay, the execution clamp, stepping through a program's draw
calls, and reading the assignment annotations. Why it is needed: replay is one
of the app's signature capabilities, is the fastest way to answer "why did that
draw call not do what I expected", and has **no tutorial at all** - only a
scripted Tour that drives it *for* the user. A tutorial where the learner
replays a program they just typed and finds a specific command would teach the
debugging loop the app is built around.

**D. Keeping Your Work** *(short, Intermediate)*
Teaches: that the session *is* a C program, that a tutorial's result becomes a
user scene, Ctrl+S, and what the exported `.c` looks like. Why it is needed:
25 tutorials end with "Tutorial complete - press … to advance or edit to
continue" and none of them tells the learner that what they made can be kept,
exported, and compiled outside the app. This is the project's central idea and
the tutorial catalog never states it.

### Tier 2 - natural next lessons for material the catalog half-covers

**E. Function Scope & Locals** *(Advanced, immediately after Functions)*
Teaches: `float x;` inside a function body is a local, `static float x;` is a
global from anywhere, parameters and loop iterators are read-only, and locals
are bound to 0 per call. Why it is needed: Functions teaches parameters only;
scope is the part that surprises people, is documented at length in `CLAUDE.md`
because it is subtle, and has no lesson.

**F. Loops Beyond the Ring** *(Advanced, after First Loop's ideas are used)*
Teaches: nested `for`, a loop whose bounds are expressions, `break` and
`continue`. Why it is needed: `break`/`continue` are supported language
features with zero tutorial coverage, and every interesting scene in
`examples/scenes/` (parametric torus, wave surface, sierpinski) is a nested
loop.

**G. Wireframe & Polygon Offset** *(Intermediate, near Culling & Winding)*
Teaches: `glPolygonMode` for wireframe, and `glPolygonOffset` for the classic
z-fighting fix when overlaying wireframe on fill. Why it is needed: both are
supported, both are things people reach for early, and neither appears in any
tutorial.

**H. Stencil Masks** *(Advanced, Effects)*
Teaches: `glStencilFunc`/`glStencilOp`/`glStencilMask` as a mask-then-draw
pair. Why it is needed: there is a shipped example
(`stencil-mask-window-glstencilop.glr`) but no lesson, and stencil is the one
remaining major fixed-function subsystem with no entry.

### Tier 3 - worth listing, not worth scheduling yet

- **Scratch block assignment** (`A[0] = {…}`) - a two-step addition to the
  existing Scratch Arrays lesson rather than an entry of its own.
- **`glPushAttrib`/`glPopAttrib`** - naturally a closing note on the transform
  stack lesson (A), since the concept is the same and the commands are rarer.
- **The assignment value plot** (right-click a row, `// @plot`) - a real
  subsystem with no tutorial; probably better served as a section of the
  Replay lesson (C) than as its own entry.
- **Multiple lights / light themes** - Two-Sided Lighting already leans on the
  light indicators overlay; a dedicated lesson is optional.

---

## Suggested resulting shape

If findings 1, 2 and 6 and Tier 1 were all done, the catalog would read:

- **Beginner** - First Triangle, Color & Transform, Points & Lines,
  GLUT Solids Tour, *Scene Chrome & Overlays* (ex-Feature Tour), First
  Animation, **Expressions & Motion** (new), Variable Slider, First Loop
- **Intermediate** - Lighting Basics, Normals, Flat & Smooth Shading,
  Materials & Shininess, Culling & Winding, Two-Sided Lighting, Color
  Interpolation, Blending & Transparency, Depth Mask & Draw Order, Fog, Clip
  Planes, Line Stipple, Bitmap Text, **Transform Stacks & Hierarchy** (new),
  **Watching a Program Run** (new), **Keeping Your Work** (new)
- **Advanced** - Functions, **Function Scope & Locals** (new), If &
  Conditionals, Scratch Arrays, **Loops Beyond the Ring** (new)

That is 24 existing entries (Depth Test Triangle retired into Depth Mask) plus
six new ones, and it turns three isolated language entries at the tail into an
actual language track that starts at Variable Slider.

Note that the "Advanced" band today means *language*, not *difficulty* -
Two-Sided Lighting is the hardest thing in the catalog and is Intermediate,
while Scratch Arrays is Advanced. If the bands are meant to be difficulty, move
Two-Sided Lighting to Advanced (it must then sit in the tail Advanced run to
keep the contiguity test happy). If they are meant to be tracks, rename them.
Either is fine; the current mixture is what makes the tail look thin.

---

## Churn each change costs

- **Reordering entries** - edit `g_tutorials[]` and keep one contiguous run per
  subheading *per tag* (`test_catalog_subheading_metadata`). Entries 0/1 are
  pinned by name in `test_tutorial_runner.c:246-249` and `:1105`, and the
  ordered band lists in `docs/USER_GUIDE.md:223-231` must follow the new
  catalog order.
- **Renaming an entry** - update `docs/USER_GUIDE.md:223-231` and every
  exact-name lookup in tests. The proposed Feature Tour rename reaches the
  cfg allowlist at `test_tutorial_runner.c:1183`, its walkthrough helper and
  runner-mechanism tests from `:2730` onward, and the SET/REQUIRE tests around
  `:3919-4023`; this is not just the First Triangle lookups in
  `test_glr_actions.c:787` and `test_tutorial_runner.c:1142`.
- **Retagging** - `test_catalog_tag_metadata` fails on a tag name that matches
  no label; dropping a tag can also change which tags are *visible*
  (`repl_tutorial_visible_tag_count`).
- **Step-text edits** - validated at start by `repl_tutorial_validate`; no
  goldens are keyed to tutorial prose, so rewording is cheap unless it changes
  the expected command or step shape.
- **Adding or removing entries** - update the explicit 25-entry assertion and
  catalog walk in `test_tutorial_runner.c:2435-2459`, plus the band lists in
  `docs/USER_GUIDE.md:223-231`. Retiring Depth Test Triangle also requires
  replacing or relocating its dedicated multi-tag, subheading, placement, and
  full-walk coverage (`test_tutorial_runner.c:1370`, `:1480`, `:1728`, and
  `:2021-2075`); its runner-mechanism coverage cannot simply disappear because
  other shipped entries happen to use `STEP_AT`.
- **New `setup` scaffolds** - interact with the func-open restriction
  (`tutorials.c:1142` records why Functions has no camera scaffold), locked-line
  tracking, and the setup validation/walkthrough coverage. Treat them as a
  runner-sensitive change rather than ordinary catalog prose.
