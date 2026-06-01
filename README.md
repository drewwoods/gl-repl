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

## Supported Material Commands

The REPL accepts fixed-function material state in the display body:

```c
glColorMaterial(face, mode);
glMaterialfv(face, pname, (GLfloat[]){r, g, b, a});  // or {value} for GL_SHININESS
```

`glColorMaterial` supports `GL_FRONT`, `GL_BACK`, and
`GL_FRONT_AND_BACK` for `face`, and `GL_AMBIENT`, `GL_DIFFUSE`,
`GL_SPECULAR`, `GL_EMISSION`, or `GL_AMBIENT_AND_DIFFUSE` for `mode`.

## Transform Guides

When your cursor sits on a committed `glTranslatef`, `glRotatef`, or
`glScalef` line, an overlay arrow/arc shows what that line does. Guides
only appear when the line parsed cleanly and your current input matches
the committed source — partial or mid-edit lines are skipped.

All guides share an "axes pulse" visual language: a dim solid base
line or arc with a bright dot traveling along it and a short fading
trail behind. The color is derived from the command's vector so the
shape of the motion reads at a glance:

- **Translate** — shaft color is `(|tx|, |ty|, |tz|)` normalized by
  its max component, mapped to `RGB`. A pure-axis translation reads
  as a pure axis color (`glTranslatef(2, 0, 0)` → red,
  `glTranslatef(0, 0, -3)` → blue); diagonals blend. The 4-fin
  pyramid arrowhead at the tip is kept, tinted from the same color.
- **Rotate** — shaft color is `(|ax|, |ay|, |az|)` normalized, so a
  Y-axis rotation reads green, an X-axis rotation reads red, etc.
  The axis stub through the rotation pivot shares the color. The
  arc is sampled by Rodrigues rotation; the pulse dot sweeps along
  the curve so arc direction is unambiguous. (When `p_after` lies
  on the rotation axis the arc collapses to a point — hover over
  a rotate line where the pivot isn't on the axis to see the sweep.)
- **Scale** — shaft color is `(|sx-1|, |sy-1|, |sz-1|)` normalized,
  so the color highlights which axes deviate from identity
  (`glScalef(2, 1, 1)` reads red). Arrow runs from the "before"
  point to the component-wise scaled result. If the "before" point
  is at the origin, a 3-axis gizmo is drawn instead: a gray unit
  reference segment per axis and a pulsing arrow per axis in that
  axis's color (X=red, Y=green, Z=blue).

### What is the "before" point?

OpenGL applies transforms in reverse source order when computing a
vertex: `M_1 · M_2 · ... · M_n · v`. That means the cursor's command
`C_k` operates on the point that the later commands `C_{k+1..n}` have
already placed. The guide starts at that point.

Accumulation of the post-cursor transforms stops at the first draw
call (`glBegin`, `glutSolidCube`, `glutSolidSphere`, `glutSolidCone`,
`glutSolidTeapot`, `glutSolidTorus`, tess polygon). Transforms that
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
  the full pre-cursor modelview (translations, rotations, and
  scales) has carried the origin to. Use this mode when you want
  the guide to line up visually with geometry drawn by earlier
  `func0()` / `glBegin` blocks: the anchor tracks where that
  geometry actually rendered, even when pre-cursor rotations are
  in play. Only the anchor position is taken from the pre-cursor
  matrix — the guide is still drawn with world-axis orientation,
  so a translate arrow still reads along world axes at the anchor.

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

## Mesh Export (PLY)

Press **F11** (or **File → Export .ply**) to capture the current scene as
an ASCII PLY mesh, named after the active scene (like Save Scene). The
geometry — your `glVertex` polygons, GLU-tessellated shapes, and the GLUT
solids (teapot/sphere/cube/cone/torus) — is captured through a single
`glRenderMode(GL_FEEDBACK)` pass, so everything on screen exports the same
way. Authored per-vertex normals are preserved; the rest are smoothly
synthesized.

Headless / scripted capture and an optional conversion:

```bash
./gl-repl --example 8 --export-ply out.ply               # capture on frame 1, then exit
./gl-repl --example 8 --export-ply out.ply --export-ply-srgb   # decode colors sRGB -> linear (color-managed viewers)
```

Line primitives you draw (`glBegin(GL_LINES/LINE_STRIP/LINE_LOOP)`) are
exported as a PLY `edge` element. See *Mesh Export (PLY via GL_FEEDBACK)*
in [`ARCHITECTURE.md`](ARCHITECTURE.md) for the capture/encode design.

## Headless Rendering (OSMesa)

For CI or any machine with no display, `make ... FREEGLUT_OSMESA=1` builds
gl-repl against a software, off-screen **OSMesa** backend — it renders into
memory with no window. The real-GL tests and `--export-ply` then run headless
(`make gl-tests FREEGLUT_OSMESA=1`), and you can grab a **screenshot of a
running headless process** by sending it `SIGUSR1`:

```bash
brew install mesa mesa-glu                                  # macOS deps (Linux: apt-get install libosmesa6-dev)
make gl-repl FREEGLUT_OSMESA=1
FREEGLUT_CAPTURE_FILE=/tmp/shot ./build/release-osmesa/gl-repl --example 8 --no-audio &
kill -USR1 $!                                               # writes /tmp/shot-0000.ppm
magick /tmp/shot-0000.ppm shot.png                         # PPM -> PNG to view
```

This needs a vendored freeglut that carries the OSMesa backend (re-vendor with
`FREEGLUT_REPO=<path-or-url> scripts/vendor-freeglut.sh <ref>`). See *Headless
Rendering & Screenshots (OSMesa)* in [`ARCHITECTURE.md`](ARCHITECTURE.md) for the
full design (build-mode swap, the teardown fix, the signal-driven capture).

## Music

gl-repl plays background music: any `*.mp3` files it finds at startup,
in filename order. It looks in three places (all combined):

- **`./assets`** next to where you run it — the default. Point it
  somewhere else with `--assets <dir>` (or the `GLR_ASSETS_DIR` env var):

  ```bash
  ./gl-repl --assets ~/Music/glr       # scan this folder instead of ./assets
  GLR_ASSETS_DIR=~/Music/glr ./gl-repl # same, via env (--assets wins)
  ./gl-repl --no-audio                 # start with no music at all
  ```

- **Bundled with the app** — the macOS `gl-repl.app` (built by `make app`)
  ships a sample track inside it, so a Finder launch has music.
- **Your own music folder** — `~/Library/Application Support/gl-repl/Music`
  on macOS (the XDG data dir on Linux). It's created on first run; drop
  `.mp3`s in there and they join the playlist.

See *Music Asset Resolution* in [`ARCHITECTURE.md`](ARCHITECTURE.md) for
the resolution order and precedence.

## Third-Party Licenses

This project bundles freeglut (vendored under `third_party/freeglut/`, built as
a static library with the macOS Cocoa backend) and miniaudio
(`include/miniaudio.h`). See [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)
for attribution and license texts.
