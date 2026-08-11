# glRasterPos: components tracked by GL_COLOR_MATERIAL are lit as zero

**Component:** Apple OpenGL (legacy compatibility GL, `2.1 Metal - 90.5`)
**Severity:** correctness / conformance, fixed-function lighting
**Status:** not yet reported upstream

## Summary

When `GL_COLOR_MATERIAL` is enabled at a `glRasterPos*` call, the material
components it tracks contribute **zero** to `GL_CURRENT_RASTER_COLOR`. The
material itself holds the tracked colour correctly, and an ordinary *vertex*
under identical state is lit correctly - only the raster-position path is
affected.

Since `GL_CURRENT_RASTER_COLOR` is the colour bitmaps (`glBitmap`,
`glDrawPixels`) are drawn with, the visible symptom is **black text**.

## Reproducer

```c
glEnable(GL_LIGHTING); glEnable(GL_LIGHT0);
glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
glColor3f(0.2, 0.5, 0.7);
glEnable(GL_COLOR_MATERIAL);
glRasterPos3f(0, 0, 0);

glGetFloatv(GL_CURRENT_RASTER_COLOR, c);
/* expected: 0.24 0.60 0.84   (same material, written with glMaterialfv)
   Apple:    0.00 0.00 0.00 */
```

`apple-colormaterial-rasterpos.c` in this directory runs this plus controls.
GLUT, no project dependencies:

```sh
cc -std=c99 -O0 -Wno-deprecated-declarations \
   -o apple-colormaterial-rasterpos apple-colormaterial-rasterpos.c \
   -framework OpenGL -framework GLUT
./apple-colormaterial-rasterpos    # 0 = conformant, 1 = bug, 77 = no GL
```

**No expected value is hardcoded.** Each case is compared against two oracles
the same driver produces under state the spec requires to be equivalent: the
same material written explicitly with `glMaterialfv` and the cap off, and the
colour GL assigns a vertex at the same position under identical state, read
back with `GL_FEEDBACK` / `GL_3D_COLOR`. The driver disagrees with itself.

## Observed

```
Reference (glMaterialfv, GL_COLOR_MATERIAL off)
  raster color = 0.240 0.600 0.840   <- every case below must match

Case A: GL_COLOR_MATERIAL tracking the color   [the bug]
  [FAIL] raster color == explicit material   got 0.000 0.000 0.000
  [PASS] GL_FRONT ambient tracked the color  got 0.200 0.500 0.700
  [PASS] lit vertex color (feedback)         got 0.240 0.600 0.840

Case B: tracking GL_DIFFUSE only
  [FAIL] raster color == explicit material   got 0.040 0.040 0.040
                                            want 0.240 0.540 0.740

Case C: cap disabled just before glRasterPos  [workaround]
  [PASS] raster color == explicit material   got 0.240 0.600 0.840
```

Case B localizes it exactly. With only `GL_DIFFUSE` tracked, what comes back is
`0.04` per channel - precisely the ambient term (material ambient 0.2 × the
default light-model ambient 0.2) with the diffuse term missing. So it is not
that the material is ignored wholesale: **the tracked component, and only the
tracked component, is lit as zero.**

It is also face-independent (`GL_FRONT` and `GL_FRONT_AND_BACK` behave alike)
and does not depend on a `glColor` ever being issued - setting the material
with `glMaterialfv` while the cap is enabled zeroes it just the same. It is the
cap's state *at the `glRasterPos` call* that decides.

## Expected

`GL_CURRENT_RASTER_COLOR` = `0.24 0.60 0.84`, matching both the explicit
material and the lit vertex.

## Spec basis

**OpenGL 2.1 (July 30, 2006), §2.13 "Current Raster Position", p. 54.**
<https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf>
(Wording carried through to the 4.6 compatibility profile unchanged.)

> "The coordinates are treated as if they were specified in a Vertex command.
> [...] These coordinates, along with current values, are used to generate
> primary and secondary colors and texture coordinates **just as is done for a
> vertex**. The colors and texture coordinates so produced replace the colors
> and texture coordinates stored in the current raster position's associated
> data."

So the feedback control in the reproducer is not an approximation of the right
answer - it is the same computation the spec names, produced by the same
driver, and the two disagree.

That colour material is in force is settled by §2.14.3 "ColorMaterial": while
`COLOR_MATERIAL` is enabled the indicated parameters "always track the current
color". The reproducer's `glGetMaterialfv` control confirms this driver does
track it - the value is present in the material and simply not used when
lighting the raster position.

## Environment

| | |
|---|---|
| Machine | Apple M2 (arm64), macOS |
| `GL_VENDOR` | `Apple` |
| `GL_RENDERER` | `Apple M2` |
| `GL_VERSION` | `2.1 Metal - 90.5` |

Not reproducible on Mesa 25.2.8 (iris, Intel ADL-N) or NVIDIA 595.84 - both
light the tracked components normally.

## Real-world impact

Any application that enables `GL_COLOR_MATERIAL` before placing lit bitmap text
gets black text on macOS and correct text everywhere else, with nothing in the
application to point at:

```c
glEnable(GL_COLOR_MATERIAL);          /* set once, early, for the geometry */
...
glColor3f(0.25, 0.7, 1.0);
glRasterPos3f(x, y, z);
glutBitmapCharacter(...);             /* black on Apple GL */
```

## Workaround

`glDisable(GL_COLOR_MATERIAL)` immediately before the `glRasterPos*`, and
re-enable after (case C). The material keeps its tracked value, and the latched
colour then matches the lit vertex exactly.
