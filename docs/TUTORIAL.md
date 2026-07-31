# Tutorial (rough draft)

Material moved out of [`USER_GUIDE.md`](USER_GUIDE.md), which documents what
gl-repl *is* - its commands, its syntax, its deviations from GL. What lands
here instead is the teaching layer: OpenGL techniques you can build with the
commands the REPL supports, and the parts of the expression language that are
worth a worked explanation rather than a reference entry.

Nothing below is unique to gl-repl. Each section is a GL feature the REPL
happens to expose; the user guide's [Supported GL
commands](USER_GUIDE.md#supported-gl-commands) list is the authority on exact
arguments and on the handful of places the REPL's spelling differs.

---

## Contents

- [Scoping state with glPushAttrib](#scoping-state-with-glpushattrib)
- [Clip planes](#clip-planes)
- [Clearing mid-scene](#clearing-mid-scene)
- [Stencil masks](#stencil-masks)
- [Wireframe & decals - glPolygonMode, glPolygonOffset](#wireframe--decals---glpolygonmode-glpolygonoffset)
- [Math expressions](#math-expressions)

---

## Scoping state with glPushAttrib

`glPushAttrib(mask)` saves a group of GL state; the matching `glPopAttrib()`
puts it back. Use them to make a local change without it leaking into the
rest of the scene:

```c
glColor3f(0.2, 0.6, 1);
glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT);
  glColor3f(1, 0.3, 0.3);   // red, and…
  glLineWidth(4);           // …fat lines, but only until the pop
  glBegin(GL_LINE_LOOP); glVertex3f(-1, 0, 0); glVertex3f(1, 0, 0); glEnd();
glPopAttrib();              // colour and line width snap back to blue / 1
```

`mask` names which *groups* of state to save, one or more of `GL_CURRENT_BIT`
(colour, normal, raster position, edge flag), `GL_POINT_BIT`, `GL_LINE_BIT`,
`GL_POLYGON_BIT` (cull + winding + polygon mode/offset), `GL_LIGHTING_BIT`
(materials, shade model, lights), `GL_FOG_BIT` (fog mode / density / start /
end / colour), `GL_DEPTH_BUFFER_BIT`, `GL_STENCIL_BUFFER_BIT` (stencil
func/ref/mask, the three `glStencilOp` slots, the write mask and the clear
value), `GL_TRANSFORM_BIT` (clip planes), `GL_ENABLE_BIT` (every
`glEnable`/`glDisable` toggle), and `GL_COLOR_BUFFER_BIT` (blend, clear colour,
colour mask). The `GL_FOG` and `GL_STENCIL_TEST` enable flags are saved by both
their group bit and `GL_ENABLE_BIT`, matching real GL. `GL_ALL_ATTRIB_BITS` is
a compact alias for the union of every group the REPL can currently change.

The editor draws the scope for you while the cursor sits on either half of the
pair - see [Attribute scope](USER_GUIDE.md#attribute-scope).

## Clip planes

```c
glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0.2, 1, 0.3, 0.4});
glEnable(GL_CLIP_PLANE0);
```

`glClipPlane` sets the plane equation `a*x + b*y + c*z + d >= 0` - GL keeps
the half-space the inequality selects and clips everything on the other
side. Six planes are available (`GL_CLIP_PLANE0..5`); each does nothing
until its cap is enabled. The equation is interpreted in the coordinate
frame active at the call, so transforms before the line position the plane
just like they position geometry.

Coefficients are full expressions, so a plane can animate - a `d` driven by
`t` sweeps a live cross-section through the scene:

![An animated clip plane sweeping a torus](images/clip-plane-sweep.gif)

A plane equation is hard to picture from four numbers, so the cursor draws
it for you - see [the clip-plane guide](USER_GUIDE.md#the-clip-plane-guide).

The *Clip planes carve solids (glClipPlane)* example walks the three core
moves - one plane (a sphere becomes a dome), two planes meeting at an angle
(a 120° wedge), and an animated `d` (a cutaway sweeping through a torus).

## Clearing mid-scene

```c
glClear(GL_DEPTH_BUFFER_BIT);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glClear(GL_STENCIL_BUFFER_BIT);
```

The REPL does not clear the scene rectangle on your program's behalf. Like the
exported C, a scene's own `glClear` is its frame setup; use another one in the
middle of a scene when a later pass needs a fresh buffer.

The useful one is `GL_DEPTH_BUFFER_BIT`. It throws away the depth of
everything drawn so far, so geometry below the line draws over what came
before it no matter how far away it is - the classic way to sit a HUD, a
gizmo, or an inset object on top of a scene without moving it:

```c
glutSolidTeapot(0.6);
glClear(GL_DEPTH_BUFFER_BIT);   // everything below wins the depth test
glColor3f(0.98, 0.45, 0.4);
glutSolidCube(0.3);             // ...so the cube is never hidden by the teapot
```

`GL_COLOR_BUFFER_BIT` repaints the scene with the current
[`glClearColor`](https://docs.gl/gl2/glClearColor), erasing geometry drawn
above the line. It is confined to the 3D viewport, so it cannot touch the
code panel or the menu bar - the rest of the window keeps the background the
frame started with.

The accumulation bit is not offered, because clearing it would fight the accum
effects under [Rendering quality](USER_GUIDE.md#rendering-quality).

## Stencil masks

Stencil commands let a first pass write a byte-sized mask and later geometry
draw only where that mask passes. Start a frame with
`glClear(GL_STENCIL_BUFFER_BIT)`, which writes 0 unless `glClearStencil(value)`
has set a different value (0..255).

`glStencilFunc(func, ref, mask)` compares the incoming reference with the
stored value while `GL_STENCIL_TEST` is enabled. `glStencilOp` chooses the
actions for stencil failure, depth failure, and a full pass; `glStencilMask`
limits which stencil bits can be written. Use `glStencilMask(0)` to protect a
completed mask while later geometry tests it.

All of it - comparison, ops, write mask and clear value - is scoped by
`GL_STENCIL_BUFFER_BIT` on the attribute stack, so a masked pass can be
wrapped in `glPushAttrib(GL_STENCIL_BUFFER_BIT)` / `glPopAttrib()` and leave
nothing behind. The `GL_STENCIL_TEST` enable flag rides that bit *and*
`GL_ENABLE_BIT`, the same dual membership real GL gives it.

The grid, axes, backdrop and light indicators are drawn with the stencil test
suspended, so a mask never clips the host's own chrome. Vertex outlines and
points *do* follow it - they report what your geometry did, and an outline
around masked-away geometry would be a lie - but they never write stencil, so
turning them on cannot disturb a mask a later pass depends on.

A mask is invisible in the rendered frame, so there is a viewer for it:
[Stencil view](USER_GUIDE.md#stencil-view).

## Wireframe & decals - glPolygonMode, glPolygonOffset

`glPolygonMode(face, mode)` picks how polygons rasterize: `GL_FILL` (the
default), `GL_LINE` for edges only, or `GL_POINT` for their corners. It is a
wireframe of the geometry you already submitted - no second set of line
primitives to build, and `face` (`GL_FRONT`, `GL_BACK`, `GL_FRONT_AND_BACK`)
can give the two sides different treatments:

```c
glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
glutSolidSphere(1, 24, 16);       // the same sphere, as wireframe
glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
```

`glPolygonOffset(factor, units)` exists for the pass you draw *on top of*
another. Two coplanar surfaces have mathematically equal depth, so the depth
test can't order them and the result speckles - the artefact known as
z-fighting. The offset shifts polygon depths a hair before the test, and
negative values pull toward the viewer:

```c
glEnable(GL_POLYGON_OFFSET_FILL);
glPolygonOffset(-1, -1);          // pull the decal in front of the wall
glColor3f(1, 0.4, 0.2);
glBegin(GL_QUADS); /* … the decal, drawn on the wall's plane … */ glEnd();
glDisable(GL_POLYGON_OFFSET_FILL);
```

`factor` scales with the polygon's depth slope (how steeply it recedes) and
`units` is a fixed multiple of the smallest resolvable depth difference;
`(-1, -1)` is the conventional starting pair for both. **The offset only
applies while the matching capability is enabled** - `GL_POLYGON_OFFSET_FILL`
for filled polygons, `GL_POLYGON_OFFSET_LINE` and `GL_POLYGON_OFFSET_POINT`
for the other two `glPolygonMode` modes. Setting an offset without enabling
one of those does nothing at all, which is the usual reason a decal still
flickers.

The two commands are made for each other: a wireframe drawn over its own solid
is the same z-fight, so `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)` plus
`glEnable(GL_POLYGON_OFFSET_LINE)` is how an outlined-solid pass stays clean.

## Math expressions

Every numeric argument is a full expression, evaluated when the line runs:

- **Operators:** `+ - * / %` and parentheses; comparisons
  `> < >= <= == !=`; logical `&& || !`.
- **Functions:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`,
  `sqrt`, `abs`, `pow`, `log` (base 10), `ln` (base e), `min`, `max`,
  `clamp(x, lo, hi)`, `lerp(a, b, s)`, `smoothstep(e0, e1, x)`, `sign`,
  `floor`, `ceil`, `round`, `fmod`, `rem`, `rand(seed[, iter])`,
  `rand2(seed[, iter])`. `fmod` is the C `fmodf` (result takes the sign of the
  dividend); `rem` is the IEEE remainder via `remainderf` (rounds the quotient
  to nearest, so the result can differ in sign).
- **Constants:** `PI`, `TAU`, `e`.

`rand` returns a deterministic value in `[0, 1]` for a given (seed, iter)
pair; `rand2` is the same hash mapped to `[-1, 1]` — useful for centered
jitter. Determinism means particle systems look the same every frame and
every run — it is the stateless substitute for storing random values, a
pattern covered in [Working without
state](USER_GUIDE.md#working-without-state).

`atan2(y, x)` is the inverse of the polar pair: it returns the angle in
`[-PI, PI]` from the +X axis to `(x, y)`, using both signs to pick the right
quadrant (unlike a plain `y/x` ratio). It is how a shape turns to face
something — `glRotatef(atan2(tz, tx) * 180 / PI, 0, 1, 0)` aims the local +X
axis at the target `(tx, tz)`, and `atan2` of a vertex's own coordinates
recovers the polar angle a ring was built from. Plain `atan(x)` takes a slope
instead of a pair, so it can only answer within `(-PI/2, PI/2)` — reach for it
when you already have a ratio (a tilt from `rise/run`), and for anything built
from two coordinates prefer `atan2`. `asin` and `acos` invert the other two:
both clamp their argument to `[-1, 1]` first, so a dot product that drifts a
hair past 1 returns `0` rather than a NaN that would erase the geometry
mid-edit.

`clamp`, `lerp`, `smoothstep`, and `sign` are the animation-shaping set — the
things most scenes end up spelling out by hand:

```c
clamp(x, lo, hi)         // x held inside [lo, hi]; replaces min(max(x, lo), hi)
lerp(a, b, s)            // a at s=0, b at s=1, straight line between
smoothstep(e0, e1, x)    // 0 below e0, 1 above e1, eased curve in between
sign(x)                  // -1, 0, or 1
```

`lerp` is the one to reach for whenever something moves *from* one value *to*
another: `lerp(1, 3, s)` grows a radius, and nesting a `sin` in the blend
factor gives an oscillation between two poses. It is deliberately **not**
clamped, so `s` past 1 (or below 0) overshoots the endpoints — that is how
springy easings are written, and `clamp(s, 0, 1)` is the hard stop when you
don't want it.

`smoothstep` is the fade whose start and stop you can't see: it leaves `e0` and
arrives at `e1` with zero slope, where a bare `lerp` visibly kinks at both
ends. Feed it a *distance* for a soft edge (`smoothstep(4, 2, dist)` fades a
glow in as geometry approaches) or a *time* for an entrance
(`smoothstep(0, 1, t)`). Its edges may run either direction: passing `e0 > e1`
ramps from 1 down to 0.

`sign` returns exactly `0` at `0` — it is not a rounding function, it answers
"which side". Multiplying by it mirrors a value about the origin
(`sign(x) * 0.5` snaps to one side or the other), and it turns a comparison
into arithmetic without an `if`.

The places this expression language is *not* C - float `%`, division by zero,
the `==` epsilon, the closed function set - are in [Where expressions differ
from C](USER_GUIDE.md#where-expressions-differ-from-c).
