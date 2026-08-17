# Antialiased polygon-mode lines vanish when the viewport is smaller than the drawable

**Component:** Apple OpenGL (legacy compatibility GL, `2.1 Metal - 90.5`)
**Severity:** correctness / conformance, polygon rasterization
**Status:** not yet reported upstream

## Summary

With `GL_LINE_SMOOTH` and blending enabled, a polygon rasterized in `GL_LINE`
polygon mode draws **nothing at all** when the viewport is smaller than the
drawable. The same draw renders normally when the viewport covers the whole
window, when `GL_LINE_SMOOTH` is off, when the outline is submitted as a real
`GL_LINE_LOOP`, or in `GL_FILL` mode.

The visible symptom is a wireframe scene that is simply **empty** - in a window
whose GL viewport is a sub-rectangle (a 3D view under a code panel, a
side-by-side layout, a HUD strip), with no error raised and nothing in the
application to point at.

## Reproducer

```c
glutInitWindowSize(1200, 800);
glViewport(0, 0, 1200, 440);            /* viewport smaller than drawable */
glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
glEnable(GL_LINE_SMOOTH);
glEnable(GL_BLEND);                     /* smoothing needs it to show */
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

glBegin(GL_POLYGON);                    /* Apple: zero fragments */
  glVertex2f(0, 1); glVertex2f(-1, -1); glVertex2f(1, -1);
glEnd();
```

`apple-line-smooth-polygon-viewport.c` in this directory runs this plus its
oracles and controls. GLUT, no project dependencies:

```sh
cc -std=c99 -O0 -Wno-deprecated-declarations \
   -o apple-line-smooth-polygon-viewport apple-line-smooth-polygon-viewport.c \
   -framework OpenGL -framework GLUT
./apple-line-smooth-polygon-viewport   # 0 = conformant, 1 = bug, 77 = no GL
```

**No expected pixel value is hardcoded.** Each case is compared against ink the
same driver produced from the same geometry under state that cannot decide
whether the triangle is on screen: the same draw with smoothing off, and the
same draw with the viewport covering the window. Both draw the outline; the
failing case draws nothing. The driver disagrees with itself.

## Observed

```
Window 1200x800, viewport 1200x440 unless stated.

  [PASS] A oracle: GL_LINE_SMOOTH off, viewport < window          928 lit pixels
  [PASS] B oracle: GL_LINE_SMOOTH on, viewport = window          2252 lit pixels
  [FAIL] C  bug:   GL_LINE_SMOOTH on, viewport < window             0 lit pixels
  [PASS] D narrow: same state, GL_LINE_LOOP not GL_POLYGON       1856 lit pixels
  [PASS] E narrow: same state, GL_FILL polygon mode             48050 lit pixels
```

Cases D and E localize it: line *primitives* antialias correctly under the same
viewport mismatch, and filled polygons rasterize correctly. Only polygon edges
rasterized as antialiased lines are lost.

It is **not** a clean on/off - it is a clip that tightens as the mismatch
grows. Sweeping the window height against a fixed 440-tall viewport, the
outline is eaten progressively from one side:

| window | lit pixels |
|---|---|
| 1200x440 (viewport = window) | 1701 |
| 1200x441 | 1018 |
| 1200x450 | 310 |
| 1200x500 and taller | 0 |

A width mismatch does it too (window 1400 wide, viewport 1200), so it is the
viewport-vs-drawable relation and not one axis.

It is not multisampling: the reproducer requests no multisample visual, and
adding one (`GLUT_MULTISAMPLE` + `glEnable(GL_MULTISAMPLE)`) changes nothing.
Blending must be on, which is expected - with blending off, coverage alpha has
no effect and the aliased line survives.

The driver names the suspect path itself. Running the failing case logs:

```
FALLBACK (log once): Fallback to SW vertex processing
    (lineMode && (localGroupInfo & GLRGroupInfoPrimitiveEmulatedFill))
```

Apple's GL emulates `GL_LINE` polygon mode rather than rasterizing it natively,
and it is that emulation - not line antialiasing in general - that mishandles a
viewport smaller than its drawable.

## Expected

Case C draws the same triangle outline as cases A and B, antialiased.

## Spec basis

**OpenGL 2.1 (December 1, 2006), §3.5.4 "Polygon Rasterization and Depth
Offset", p. 118**, and §3.4.2 "Other Line Segment Features", p. 108.
<https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf>
(Wording carried through to the 4.6 compatibility profile unchanged.)

> "If PolygonMode is called with [...] LINE, [...] the polygon is rasterized by
> [...] drawing the boundary edges of the polygon as line segments. [...] the
> rasterization of each of these line segments is controlled by the line width,
> stipple, and antialiasing state."

Line antialiasing (§3.4.2) is specified to change the *coverage* a fragment
carries, which blending then applies. It is nowhere permitted to discard
fragments. The viewport (§2.11.1) only maps normalized device coordinates to
window coordinates; nothing couples the outcome to the size of the drawable the
viewport sits in.

## Environment

| | |
|---|---|
| Machine | Apple M2 (arm64), macOS 15 |
| `GL_VENDOR` | `Apple` |
| `GL_RENDERER` | `Apple M2` |
| `GL_VERSION` | `2.1 Metal - 90.5` |

Not reproducible on Mesa 25.2.8 (iris, Intel ADL-N), which draws the outline in
every case.

## Real-world impact

This is why a gl-repl scene built on `glPolygonMode(GL_LINE)` renders nothing
with **Line smooth** on while its exported C twin renders correctly: the app
puts the 3D scene in a viewport below the code panel, and the exported
program's viewport is the whole window. The same applies to the app's own
**Wireframe** view (`WIREFRAME_PLAIN` / `WIREFRAME_HIDDEN`), whose passes are
polygon-mode `GL_LINE` with blending enabled when line smoothing is on
(`render3d_pass_hidden_line_wireframe` in `src/render3d/render.c`).

## Workaround

Any of, in decreasing order of scope:

- Render with the viewport covering the whole drawable and confine the scene
  with `glScissor` plus a projection that accounts for the offset - the
  structural fix, and the only one that keeps both smoothing and polygon mode.
- Submit outlines as real line primitives (`GL_LINE_LOOP` / `GL_LINES`) instead
  of relying on `GL_LINE` polygon mode; those antialias correctly (case D).
- Turn `GL_LINE_SMOOTH` off while polygon mode is `GL_LINE` (case A). In
  gl-repl that is **Ctrl+Shift+L** (Config → Line smooth).
