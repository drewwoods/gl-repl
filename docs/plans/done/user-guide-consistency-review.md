# `docs/USER_GUIDE.md` Consistency, Readability & Verbosity Review

## Status - landed, 2026-08-14

Implemented against [`docs/USER_GUIDE.md`](../../USER_GUIDE.md). Factual
fixes (display baseline + `glClear`, Ctrl+Shift+H, tour transport, common
CLI + `--tour` / `--list-tours`, accumulation ladder 1/2/4/6/8/10/12/14/16,
replay keys in the appendix and F1, PLY display/OSMesa) landed first; then
the cuts and moves.

Final review follow-ups removed the duplicate `glClear` instruction from the
triangle walkthrough while keeping the call visible, corrected the `+` / `-`
tour-speed direction, clarified menu-only config items, and removed editorial
notes that had leaked into reader-facing prose. F1 now splits controls into
**Editor** and **Scene** tabs; Scene follows the Config menu's section order.

Post-cut counts: **1,938 lines, ~13,200 words, 23 H2 sections** (was 2,694
lines / ~20,700 words). The REPL Language and Seeing sections together
dropped from ~7,400 words to ~3,800. Worked math, planar shadows, and
stateless animation moved to [`TUTORIAL.md`](../../TUTORIAL.md); driver-oracle
methodology moved to [`CONTRIBUTING.md`](../../CONTRIBUTING.md). The
40-example roster left the guide; `check-user-guide-examples` now checks
the count claim only. `check-user-guide-keymap` forbids the stale
Ctrl+Shift+Y spelling.

The original review text follows as the design record.

---

## Verdict

The guide is thorough and mostly accurate, but it is doing too many jobs at
once. It reads like a merged user guide, language reference, overlay manual,
and contributor note.

The opening already names a cleaner split — this file for what gl-repl *is*,
[`TUTORIAL.md`](../../TUTORIAL.md) for techniques, [`ADVANCED_USAGE.md`](../../ADVANCED_USAGE.md)
for power-user flags — and then the body often ignores it.

The title still says **(Draft)**. That undercuts a document this long. Drop
the label or keep it only until the cuts below land.

Keep the session-shaped spine (start → write → move → see → save). Cut or
move the essays that sit on that spine. A reader who wants to type a
triangle and export it should not have to pass driver-oracle methodology,
NaN propagation, and a 2,600-word overlay treatise to get there.

A realistic target is **roughly half the length** in the main flow, with the
rest becoming pointers or a short appendix.

---

## What is already working

- The opening thesis (“the code is the interface”) is the right frame.
- The session roadmap and the H2 list match.
- Cross-doc pointers at the top are the right contract.
- Examples, tutorials, and tours are the right on-ramp, in the right order.
- Images are well-captioned and usually earn their space.
- Cross-links to `TUTORIAL.md` for clip planes, stencil, and mid-scene clear
  are the model to copy.

---

## Consistency

### Factual

**Display defaults are wrong.** The guide says File → New Scene and the
trash button reset to **five** lighting lines (`USER_GUIDE.md` ~99, ~301,
~320–331). The source seeds **six**, and the missing one is `glClear`:

```c
/* src/repl/load.c — k_default_display_baseline[] */
"glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
"glEnable(GL_COLOR_MATERIAL);",
...
```

That is the most important error. The first-triangle walkthrough never
mentions `glClear` either, so a new user can miss the smear-if-deleted rule
that the language section later treats as load-bearing. The source
`glMaterialfv` alpha is also `1.0`, not the guide's `1`.

The first-triangle code block should visibly start with `glClear`, while the
walkthrough tells the reader to keep the line File → New Scene already seeds.
That advertises that this is real GL and that the program, not hidden frame
machinery, owns the clear without inserting a duplicate clear.

**Syntax-highlight shortcut disagrees with itself.** Writing Code says
**Ctrl+Shift+Y** twice (`USER_GUIDE.md` ~515, ~522); the keyboard table
(`~2632`) and `keymap.h` (`GLR_SYNTAX_HL` = Shift+Ctrl+H) say
**Ctrl+Shift+H**. The table is correct.

**Guided-tour controls are stale.** The guide says any key stops a tour. Tours
now have replay-style transport: Space plays/pauses, Left/Right step back and
forward, `+`/`-` change speed, and Esc exits. Other physical input cancels the
tour. `src/app/glr_tours.h` and `glr_pointer_script_handle_tour_*()` are the
authority.

**The CLI section's contract disagrees with itself.** The introduction promises
the **full CLI**, while the section later calls itself the **day-to-day set**.
It omits `--tour` / `--list-tours`, despite Guided Tours being a first-class
section, and also omits less-common flags such as `--examples-dir`,
`--lint-scenes`, and `--dump-flat`. Make the guide explicitly the **common CLI**,
add the two tour flags beside the tutorial flags, and leave the complete flag
reference in `ADVANCED_USAGE.md`.

**Accumulation passes omit three valid steps.** Rendering quality lists
`1/2/4/8/12/16`; `GLR_ACCUM_PASS_LADDER` also contains **6, 10, and 14**. The
F1 help text has the same stale list, so update both surfaces together.

**`;` vs Enter.** The body says either key “applies it and moves to the next
line.” The keyboard table distinguishes them: `;` commits, Enter commits
**and** inserts. `Agents.md` matches the table. The first-triangle sample
also omits trailing `;` while later samples include them.

**Keyboard reference is not a reference for this guide.** Replay's own table
documents Space, `+`/`-`, arrows, `m`, `e`, `n`, `v`. The end-of-doc table
keeps only Ctrl+R (with Ctrl+K parenthetical). A reader who trusts the
appendix will miss most of replay. The F1 Scene tab also omits replay's `e`,
`n`, and `v` controls, despite the guide calling it the "always-current"
version of the appendix. Complete both lists together, or weaken that claim.

**PLY's "headless" label needs a condition.** `--export-ply` renders a frame
and the normal build needs a display; it is genuinely headless only with the
OSMesa build. Call the examples **scripted export**, with a sentence pointing
to the OSMesa instructions for headless operation.

### Repeated topics, different wording

These are the same fact told two or three times, not always identically:

| Topic | Where it repeats | Problem |
|---|---|---|
| Code-is-the-interface | Intro, Camera, Scope & Limitations | Same thesis, three essays |
| Auto-promotion + 8-slot cap | Built-in Examples, Scenes & Workspaces | Slot-full behavior is only complete in the first |
| `Ctrl+/` block comment | Keeping the buffer tidy, Disabling a block, Comments | Disabling a block is a subset of Comments |
| macOS black `label()` text | `label()`, state inspector, Fidelity | Workaround belongs once, under `label()` |
| Grass / 8192 / export | Export, Performance, Tune | Same story three times |
| Function locals vs variable panel | Window tour, Variables, Variable Panel, `@tune` | Fine as a one-liner; not as a refrain |
| Music sources | Music, `ADVANCED_USAGE.md` | Near-duplicate |
| PLY export | Mesh export, `ADVANCED_USAGE.md` | Near-duplicate |
| Math function list | Math expressions, `TUTORIAL.md` | TUTORIAL already owns the worked explanations |

`File -> New Scene` (ASCII arrow) appears once next to the usual
`File → New Scene`.

**Config OVERLAYS list** omits Vertex label placement, then later calls it
menu-only. Either list it or don’t.

**Same image, two captions.** `images/variable-panel.png` is reused for the
ordinary variable-panel explanation and the `@tune` accent. Both captions are
accurate: the image shows `t` plus `amp`, `freq`, and `spread`, with tune marks
on `amp` and `freq`. The reuse is only a verbosity question; it is not a
consistency error.

### Naming collisions

- **Diagnostic Views** (winding / depth / stencil) vs **Profiling &
  Diagnostics** (CPU/GPU panels). Easy to mix up. Rename the second to
  **Profiling**.
- **Scope & Current Limitations** vs **Performance & Scope**. The first is
  product bounds; the second is the interpreter-vs-export story. The word
  “scope” is doing two jobs.

---

## Verbosity

Word counts by section at review time, largest first:

| Section | Words | Note |
|---|---:|---|
| The REPL Language | 4,772 | A reference manual inside a guide |
| Seeing What You're Doing | 2,658 | Overlay encyclopedia |
| Writing Code | 2,148 | Plot + inspector are most of it |
| Exporting & Importing | 1,150 | Grass-blade economics + `.glr` authoring |
| Making It Move | 1,003 | Stateless-physics essay is half of it |
| Diagnostic Views | 963 | Stencil viewer caveats over-taught |
| Scene Appearance | 820 | Grid-brightness paragraph is a blog post |
| Scenes & Workspaces | 748 | Workspace-binding is careful but long |
| Profiling & Diagnostics | 724 | Histogram hover protocol is more than users need |
| Tunable Variables | 672 | Solid, slightly padded |
| Fidelity to OpenGL | 631 | Contributor material |

The language and overlay sections together are **~7,400 words** — more than
a third of the file.

### Cut or move

**Move to `TUTORIAL.md` (already claimed there)**

- Worked `fmod` vs GLSL `mod`, `rem`, `lerp` unclamped, NaN-propagation
  detective work. The guide’s own intro says worked math lives in the
  tutorial. Keep an 8–10 bullet “not C” list here.
- Planar-shadow matrix walkthrough. Point at the *Planar shadows* example.
- Stateless animation patterns (closed-form integrate, replay-the-sort).
  One paragraph + three example names is enough; the rest is teaching.

**Move out of the main guide or drop**

- Fidelity to OpenGL (`make gl-tests`, driver table, `third_party/bugs/`).
  Users need the one-line macOS `label()` workaround, already under
  `label()`. Test/oracle methodology belongs in `ARCHITECTURE.md` or
  `CONTRIBUTING.md`, not in the power-user CLI reference.
- `.glr` phase-order / `@cfg` subset / catalog authoring. The section
  already points at Advanced Usage.
- PLY color-management and feedback-pass detail. Keep one sentence + link.
- Music search-path list. Keep the keys; link Advanced Usage.
- Compute-profile histogram isolation protocol.

**Collapse in place**

- **Supported GL commands**: signatures + one clause each. Policy novels
  (`glStencilFunc` literal-vs-animated, `glClear` smear, `glPushAttrib`
  eleven bits, `glClearColor` 0.15 clamp) become one sentence or a TUTORIAL
  link. The enum/mask subsection is good; keep it short. Preserve the
  expensive user-facing semantics somewhere obvious: in particular, the
  program owns `glClear`, and stencil reference/mask slots accept different
  kinds of input.
- **Assignment plot**: table of controls + X/Y/replay/`@plot`. Drop the
  chip-grammar manifesto and the “rate greys out during replay” paragraph.
- **State inspector**: what it shows, setup fold, Shift-compare. Drop
  raster-color driver caveats (they belong under `label()` / Fidelity).
- **Vertex label placement & numbering**: one short rule — decluttered is
  global, at-vertex is per-primitive. The looped-quad walkthrough is a
  figure caption, not a subsection.
- **Transform World vs Frame**: keep the figure and two bullets. Cut the
  reverse-order matrix lecture.
- **Disabling a block**: delete the section; Comments already covers it.
- **Scope & Current Limitations**: keep textures / shaders / no gizmos /
  light presets as a tight list. Delete the rest — it restates the intro
  and Performance.
- **What the export is free of** + **Performance & Scope**: one section.
  The 135→9600 grass story once.

**The 40-example roster** is too much catalog inside the guide.
`--list-examples` is authoritative, and `make check-user-guide-examples`
currently prevents silent drift, so rot is not the reason to remove it.
Remove or shorten it because it interrupts the on-ramp and duplicates a
queryable catalog.

---

## Readability

The prose is careful and often good, but it argues with itself in the same
sentence:

> The rule has to sit over the values the replay is actually stepping
> through, so while replay is active the plot captures every frame
> regardless of the rate - the rate chip greys out to show it is not in
> force.

That is one idea (replay forces per-frame capture) wearing three clauses.
The guide does this constantly: claim, because-clause, parenthetical,
em-dash, second because-clause.

A user guide scans better as:

1. What it is.
2. How to do it.
3. One gotcha, if the gotcha is expensive.

Examples of the current shape vs that:

- Grid brightness spends a paragraph on *why* Bold uses a dark casing.
  Users need Dim / Normal / Bright / Bold and when to reach for Bold.
- Stencil view spends four caveats plus compositing-order philosophy before
  “web can’t read stencil.” Lead with what the modes show; park the rest.
- Function-scoped locals is accurate and also a language-lawyer note. The
  Orrery `px/py/pz` anecdote is nice once, not as the closer of a 40-line
  bullet list.

The **session roadmap** sells a short path (start → examples → write →
move → see → ship) and then the TOC is 23 flat H2s, including Guided Tours,
Camera, Config, Appearance, Replay, Tune, Fidelity, Profiling, Music. Group
the TOC to match the roadmap:

- Start here
- Write and animate
- See and debug
- Keep and export
- Reference

Or the roadmap is lying about the shape of the file.

Long lines in “Editing what’s there” (clipboard paragraph) and the
status-bar / scrollbar description in **The window** bury the useful facts.
The window tour does not need the thumb-drag-from-click-on-track algorithm.

---

## Suggested end-state

If the goal is “not too verbose,” treat these as the whole main guide:

1. Getting Started (window + first triangle, **with `glClear`**)
2. Examples / Tutorials / Tours (no full roster)
3. Writing Code (commit, edit, stepper, picker; plot/inspector cut down)
4. The REPL Language (command list as a compact table; structure; short
   “not C”)
5. Making It Move (`t`, panel, one paragraph on stateless)
6. Camera
7. Seeing + Diagnostics (guides and views, not numbering theory)
8. Config / Appearance / Replay (keep; trim)
9. Scenes, Export, Tune, Performance (one export story)
10. Limitations (short)
11. CLI + keyboard (complete, and consistent with the body)

Everything else becomes a link.

---

## Implementation order

Highest-value edits, in order. Each item is independently shippable; do not
wait for the full cut pass to land the factual fixes.

1. Fix the six-line display baseline and put `glClear` visibly at the start of
   the first-triangle code block. Its presence there is intentional even
   though New Scene seeds it: the example should advertise real GL frame
   ownership.
2. Fix Ctrl+Shift+H.
3. Document tour transport, and add `--tour` / `--list-tours` next to the
   tutorial flags. Rename the CLI section/preamble claim so it promises common,
   not complete, options.
4. Fix the accumulation-pass ladder in the guide and F1 help: 1/2/4/6/8/10/
   12/14/16.
5. Complete the appendix and F1 replay-key listings together.
6. Qualify PLY export: normal builds need a display; OSMesa is the headless
   path.
7. Delete **Disabling a block**; fold **Fidelity** and most of **Scope &
   Current Limitations**.
8. Halve **The REPL Language** and **Seeing What You're Doing**, while keeping
   a complete compact command roster and the costly semantic gotchas.
9. Deduplicate promotion, grass/8192, `label()`/macOS, Music, and PLY.
10. Drop "(Draft)" when that pass is done.

Related files to consult or update while executing the plan:

- [`docs/TUTORIAL.md`](../../TUTORIAL.md) — receives worked math / matrix /
  stateless essays if they leave the guide.
- [`docs/ADVANCED_USAGE.md`](../../ADVANCED_USAGE.md) — already owns CLI
  completeness, music paths, PLY detail; the guide should point, not copy.
- [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) / [`docs/CONTRIBUTING.md`](../../CONTRIBUTING.md)
  — better homes for GL differential-oracle and driver-test methodology.
- [`src/repl/load.c`](../../../src/repl/load.c) — authority for the display
  baseline (read, do not rewrite, unless the product changes).
- [`keymap.h`](../../../keymap.h) — authority for Ctrl+Shift+H.
- [`src/app/glr_tours.h`](../../../src/app/glr_tours.h) and
  [`src/app/glr_pointer_script.c`](../../../src/app/glr_pointer_script.c) —
  authority for controlled-tour transport.
- [`src/app/glr_config.h`](../../../src/app/glr_config.h) — authority for the
  accumulation-pass ladder.
- [`src/repl/help_text.c`](../../../src/repl/help_text.c) — F1 help must move
  with the accumulation and replay-key corrections.
- [`src/app/boot/glr_cli.c`](../../../src/app/boot/glr_cli.c) — authority for
  the full option set and the display requirement of `--export-ply`.

The current `check-user-guide-keymap` guard passes despite the stale
Ctrl+Shift+Y prose because it proves that required correct text exists; it
does not reject every obsolete shortcut mentioned elsewhere. Add a forbidden
pattern for this known spelling, or broaden the guard when making the fix.
