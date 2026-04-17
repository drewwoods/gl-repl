## Design Goals

 - Launch pad.  Make its easy to get something goaling quickly.
 - Indpendence.  Export/import is first class citizen.  The idea is to take
   what you build and use it in your own engine or tool.
 - Immediate mode.  The joy of immediate mode is the localized focus.  The
   geometry is in the code and not hidden behind a data file.  The user can see
   the geometry and color in the code and change it without having to open a
   separate tool.
 - Limited state.  Animation driven by time.  Particles drived by deterministic
   random number generator.
 - No textures, just geometry and color.  Not a hard design goal but the
   current idea is to expose the expressiveness of geometry and color to the
   user and not hide it behind textures.

## Transform Guides

When your cursor sits on a committed `glTranslatef`, `glRotatef`, or
`glScalef` line, an overlay arrow/arc shows what that line does. Guides
only appear when the line parsed cleanly and your current input matches
the committed source — partial or mid-edit lines are skipped.

- **Translate** — dashed orange arrow with a 4-fin arrowhead, from the
  "before" point to `before + (tx, ty, tz)`.
- **Rotate** — cyan axis stub through the rotation pivot plus a dashed
  arc sweeping from the "before" point by the command's angle. If the
  point lies on the rotation axis (degenerate case), a unit-radius
  reference point is synthesized so the sweep is still visible.
- **Scale** — magenta arrow from the "before" point to the
  component-wise scaled result. If the "before" point is at the
  origin, a 3-axis gizmo is drawn instead: a gray unit reference
  segment per axis and a magenta arrow to `(sx, 0, 0)`, `(0, sy, 0)`,
  `(0, 0, sz)`.

### What is the "before" point?

OpenGL applies transforms in reverse source order when computing a
vertex: `M_1 · M_2 · ... · M_n · v`. That means the cursor's command
`C_k` operates on the point that the later commands `C_{k+1..n}` have
already placed. The guide starts at that point.

Accumulation of the post-cursor transforms stops at the first draw
call (`glBegin`, `gluSphere`, `gluCylinder`, `gluDisk`,
`gluPartialDisk`, `glutSolidTorus`, tess polygon). Transforms that
come after an intervening draw don't factor into the guide.

### Guide mode

The config menu (Config button on the code panel header) has an
**Xform guide mode** toggle with two options:

- **World** *(default)* — guide is rendered in world axes at world
  origin. This is the strict OpenGL reverse-order reading: pre-cursor
  transforms wrap the sub-expression later and don't move the guide.
  Use this mode when you want to reason about what your line produces
  independent of its surroundings.

  Example — cursor on `glTranslatef(0, 0, -2);` at the bottom of:

  ```
  glTranslatef(0, 0, 2);
  glRotatef(45, 0, 1, 0);
  glTranslatef(0, 0, -2);   // cursor here
  ```

  Shows an arrow from `(0, 0, 0)` to `(0, 0, -2)` along world Z.

- **Frame** — guide is anchored at the scene-world position that
  pre-cursor **translations** have carried you to (pre-cursor
  rotations are ignored). Use this mode when you want the guide to
  line up visually with geometry drawn by earlier `func0()` /
  `glBegin` blocks.

  Example — cursor on the second translate in:

  ```
  glTranslatef(2, 0, 0);
  func0();
  glTranslatef(-4, 0, 0);   // cursor here
  func0();
  ```

  Frame mode anchors the guide at `x = 2` so the arrow runs from
  `(2, 0, 0)` to `(-2, 0, 0)`, visually matching the rendered
  triangles. World mode would instead draw from `(0, 0, 0)` to
  `(-4, 0, 0)`.

Toggle guides entirely with the **Vertex guides** config item (F8).
