# glRasterPos latches GL_CURRENT_RASTER_COLOR unclamped when lighting is disabled

**Component:** Mesa core, fixed-function raster position path — reproduces on `iris`
**Severity:** correctness / conformance
**Status:** not yet reported upstream

## Summary

With lighting **disabled**, `glRasterPos*` stores the current colour into
`GL_CURRENT_RASTER_COLOR` verbatim, including components outside [0,1]. The
spec clamps the raster colour "after lighting (whether enabled or not)". With
lighting enabled Mesa does clamp, so it is specifically the unlit path — the
parenthetical half of the sentence — that is missed.

## Reproducer

The whole bug is three calls:

```c
glDisable(GL_LIGHTING);
glColor4f(1.5, -0.5, 0.25, 1.0);
glRasterPos3f(0, 0, 0);

glGetFloatv(GL_CURRENT_RASTER_COLOR, c);
/* expected: 1.00  0.00  0.25
   Mesa:     1.50 -0.50  0.25 */
```

```sh
cc -std=c99 -O0 -o mesa-rasterpos-color-unclamped \
   mesa-rasterpos-color-unclamped.c -lglut -lGL -lGLU -lm
./mesa-rasterpos-color-unclamped     # 0 = conformant, 1 = bug, 77 = no GL
```

The test colour puts one component above 1, one below 0 and one in range, so a
driver clamping only one side is still distinguishable.

## Observed

Mesa 25.2.8, `Mesa Intel(R) Graphics (ADL-N)`, GL 4.6 compat:

```
Case A: lighting disabled, out-of-range color   [the bug]
  [FAIL] GL_CURRENT_RASTER_COLOR clamped   got  1.50 -0.50  0.25   want 1.00 0.00 0.25
  [PASS] GL_CURRENT_COLOR stays raw        got  1.50 -0.50  0.25   want 1.50 -0.50 0.25

Case B: lit, emission past 1.0                 [control]
  [PASS] GL_CURRENT_RASTER_COLOR clamped   got  1.00  0.50  0.00   want 1.00 0.50 0.00
```

Two controls make the failure unambiguous:

- **Case B** reaches the same clamp through lighting (emission past 1.0, every
  other term zeroed). Mesa passes it, so the clamp exists and is only skipped
  on the unlit path.
- **`GL_CURRENT_COLOR`** must stay raw and does. The two cells are specified
  differently — the current colour is not a colour produced for a primitive —
  so a driver storing the same value in both is not being consistent, it is
  missing the clamp.

## Expected

`GL_CURRENT_RASTER_COLOR` = `1.00 0.00 0.25`.

## Spec basis

**OpenGL 2.1 (December 1, 2006), §2.14.6 "Clamping or Masking", p. 70.**
<https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf>

> "After lighting (**whether enabled or not**), all components of both primary
> and secondary colors are clamped to the range [0, 1]."

That this applies to the raster colour follows from §2.13 "Current Raster
Position", p. 54: the coordinates "are treated as if they were specified in a
Vertex command [...] used to generate primary and secondary colors [...] just
as is done for a vertex". The latched value is a primary colour, so the clamp
above covers it, and the parenthetical makes the unlit case normative rather
than an inference.

In the 4.6 compatibility profile the clamp is controllable via `ClampColor`,
but `CLAMP_VERTEX_COLOR` defaults to `TRUE` — the reproducer never changes it.

## Environment

| | |
|---|---|
| OS | Ubuntu 24.04 |
| GPU | Intel Alder Lake-N UHD Graphics `[8086:46d2]` |
| `GL_RENDERER` | `Mesa Intel(R) Graphics (ADL-N)` (iris) |
| `GL_VERSION` | `4.6 (Compatibility Profile) Mesa 25.2.8-0ubuntu0.24.04.2` |

Not reproducible on Apple GL (`2.1 Metal - 90.5`, Apple M2), where this
reproducer exits 0. NVIDIA 595.84 also clamps — measured with a differential
state test rather than with this program, which has not been run there yet.

## Note on impact

Lower than the other raster-position deviations: an application that keeps
colours in range never sees it, and the clamp is applied later in the pipeline
anyway, so drawn pixels are usually unaffected. It matters for code that
*reads* `GL_CURRENT_RASTER_COLOR` back — state inspectors, debuggers, and
anything asserting on GL state — where a driver-dependent answer to "what
colour was latched?" cannot be modelled portably.
