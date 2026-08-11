# glRasterPos is lit with untransformed inputs (object-space position, unnormalized normal)

**Component:** Mesa core, fixed-function raster position path — reproduces on `iris`
**Severity:** correctness / conformance, fixed-function lighting
**Status:** not yet reported upstream

## Summary

When lighting is enabled, the colour latched into `GL_CURRENT_RASTER_COLOR` by
`glRasterPos*` is computed from **untransformed** inputs, in two independent
ways:

1. the vertex position fed to the lighting equation is the **object-space**
   position rather than the eye-space one, so the vector to a positional light
   is wrong whenever the modelview is not the identity;
2. **`GL_NORMALIZE` is ignored** on that path, so a modelview carrying a scale
   lights a non-unit normal.

A *vertex* at the same position under identical state is lit correctly, so this
is specific to the raster-position path. Both symptoms need a non-identity
modelview; with the identity they vanish, which is why they hide easily.

`GL_CURRENT_RASTER_COLOR` is the colour bitmaps (`glBitmap`, `glDrawPixels`)
are drawn with, so the visible symptom is lit text or bitmap sprites taking the
wrong shade.

## Reproducer

```sh
cc -std=c99 -O0 -o mesa-rasterpos-lighting-untransformed \
   mesa-rasterpos-lighting-untransformed.c -lglut -lGL -lGLU -lm
./mesa-rasterpos-lighting-untransformed   # 0 = conformant, 1 = bug, 77 = no GL
```

**No expected value is hardcoded.** Each case compares the raster colour with
the colour *this same driver* gives a vertex at the same position under
identical state, captured with `GL_FEEDBACK` / `GL_3D_COLOR`. The spec makes
those two the same computation, so the driver is disagreeing with itself and no
third-party model of the lighting equation is involved.

The light is **positional** (`w = 1`) and its position is set under the
identity modelview, so the eye-space light is fixed across cases and only the
vertex transform varies. With a directional light the position drops out of the
light vector and symptom 1 is invisible.

## Observed

Mesa 25.2.8, `Mesa Intel(R) Graphics (ADL-N)`, GL 4.6 compat:

```
Case A: identity modelview                    [control]
  [PASS] identity                 raster 0.781473 0.781473 0.781473
                                  vertex 0.781473 0.781473 0.781473

Case B: rotated + translated modelview        [the bug]
  [FAIL] transformed modelview    raster 0.812520 0.812520 0.812520
                                  vertex 0.752773 0.752773 0.752773

Case C: GL_NORMALIZE under glScalef(4)        [the bug]
  [FAIL] normalize + scale        raster 0.190413 0.190413 0.190413
                                  vertex 0.641653 0.641653 0.641653
```

Case A passing is what makes B and C readable: the setup, the feedback oracle
and the lighting configuration are identical across all three, and only the
modelview changes.

Case C is scaled **up** deliberately. The inverse transpose shrinks the normal
to a quarter length, so the normalized and unnormalized results both stay
inside [0,1] and differ by a clean factor; scaling *down* saturates the correct
answer at 1.0 and the two models become indistinguishable.

## Expected

Raster colour equal to the vertex colour in every case.

## Spec basis

**OpenGL 2.1 (July 30, 2006).**
<https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf>
(Wording carried through to the 4.6 compatibility profile unchanged.)

§2.13 "Current Raster Position", p. 54 — the raster position is a vertex for
this purpose:

> "The coordinates are treated as if they were specified in a Vertex command.
> [...] These coordinates, along with current values, are used to generate
> primary and secondary colors and texture coordinates **just as is done for a
> vertex**."

§2.14.1 "Lighting" — one sentence settles case B:

> "All computations are carried out in eye coordinates."

§2.12 "Normal Transformation" — and one settles case C:

> "Before use in lighting, normals are transformed to eye coordinates by a
> matrix derived from the model-view matrix. Rescaling and normalization
> operations are performed on the transformed normals to make them unit length
> prior to use in lighting."

Neither is qualified for `RasterPos`, and §2.13 explicitly routes the raster
position through the same computation as a vertex.

## Environment

| | |
|---|---|
| OS | Ubuntu 24.04 |
| GPU | Intel Alder Lake-N UHD Graphics `[8086:46d2]` |
| `GL_RENDERER` | `Mesa Intel(R) Graphics (ADL-N)` (iris) |
| `GL_VERSION` | `4.6 (Compatibility Profile) Mesa 25.2.8-0ubuntu0.24.04.2` |

Not reproducible on Apple GL (`2.1 Metal - 90.5`, Apple M2), where this
reproducer exits 0 with all three cases passing. NVIDIA 595.84 also agrees with
the vertex — measured with a differential state test rather than with this
program, which has not been run there yet.

## Workaround

Set the raster position under an identity modelview and place the text by
transforming the coordinates in the application, or set light positions so the
light is directional (`w = 0`), where the vertex position leaves the light
vector. For the normalize case, supply unit-length normals rather than relying
on `GL_NORMALIZE`.
