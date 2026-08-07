# glColorMaterial: retargeting the tracked face discards the previous face's tracked color

**Component:** Mesa core (`src/mesa/main`) — reproduces on `iris` and `llvmpipe`
**Severity:** correctness / conformance, fixed-function lighting
**Status:** not yet reported upstream

## Summary

While `GL_COLOR_MATERIAL` is enabled and `GL_FRONT` is the tracked face, a
`glColor*` correctly updates `GL_FRONT`'s material. Selecting a *different*
face afterwards then throws that value away: `GL_FRONT` reverts to the
untouched GL default instead of keeping the color it tracked.

The call that causes the loss names `GL_BACK` and changes no color.

## Reproducer

The whole bug is four calls:

```c
glEnable(GL_COLOR_MATERIAL);
glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
glColor3f(0.2, 0.6, 0.3);                          /* -> GL_FRONT */
glColorMaterial(GL_BACK,  GL_AMBIENT_AND_DIFFUSE); /* retarget only */

glGetMaterialfv(GL_FRONT, GL_AMBIENT, m);
/* expected: 0.20 0.60 0.30
   Mesa:     1.00 1.00 1.00  */
```

`mesa-colormaterial-face-switch.c` in this directory runs this in a fresh
context, plus three controls. Plain GLX, no GLUT:

```sh
cc -std=c99 -O0 -o mesa-colormaterial-face-switch \
   mesa-colormaterial-face-switch.c -lGL -lX11 -lm
./mesa-colormaterial-face-switch    # 0 = conformant, 1 = bug, 77 = no GL
```

## Observed

```
Case A: no face switch                  [control]
  [PASS] GL_FRONT holds the color it tracked   got 0.20 0.60 0.30

Case B: retarget to GL_BACK afterwards  [the bug]
  [FAIL] GL_FRONT holds the color it tracked   got 1.00 1.00 1.00
  [PASS] GL_BACK picked up the current color   got 0.20 0.60 0.30

Case C: as B, but query before the switch
  [PASS] GL_FRONT holds the color it tracked   got 0.20 0.60 0.30
  [PASS] GL_BACK picked up the current color   got 0.20 0.60 0.30

Case D: spec p.519 example, ColorMaterial sets the named face
  [PASS] GL_FRONT ambient = current color      got 0.20 0.60 0.30
```

Case A shows the color *does* reach `GL_FRONT`; only the retarget loses it.
`GL_BACK` is always correct — it is the face being left behind that breaks.

## Expected

`GL_FRONT = 0.20 0.60 0.30` in every case.

## Spec basis

Normative source: **OpenGL 4.6 Compatibility Profile (May 5, 2022), §12.2.3
"ColorMaterial", p. 519.**
<https://registry.khronos.org/OpenGL/specs/gl/glspec46.compatibility.pdf#page=544>
(PDF page 544 = printed page 519; both quotes below are on that one page.)

This text is unchanged from OpenGL 2.1 §2.14.3, p. 66
(<https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf#page=80>) — the
wording has carried through verbatim, so the older spec can be cited
interchangeably if preferred.

Two sentences decide this. First, the permanence rule, which is exhaustive
about what may overwrite a tracked value:

> "The replacements made to material properties are **permanent**; the replaced
> values remain until changed by either sending a new color or by setting a new
> material value when ColorMaterial is not currently enabled to override that
> particular value. When COLOR_MATERIAL is enabled, the indicated parameter or
> parameters always track the current color."

The spec lists exactly two ways a replaced material value may change: a new
`Color*`, or a `Material*` call made while `ColorMaterial` is **not** enabled.
Calling `ColorMaterial` to retarget the face is neither. `GL_FRONT`'s value is
therefore required to persist.

Second, the spec's own worked example on the same page, which is the same
call shape as the one that fails here:

> "calling
>
>     ColorMaterial(FRONT, AMBIENT)
>
> while COLOR_MATERIAL is enabled sets the front material a_cm to the value of
> the current color."

So `ColorMaterial` issued while enabled is explicitly specified to write the
material of the face it names, immediately. Mesa **passes** that example in
isolation (verified). What it gets wrong is the other half: when the call
retargets *away* from a face, the color that face was already tracking is
dropped instead of remaining, contradicting the permanence rule above.

Note also §12.2.5 "Lighting State" (p. 521): ColorMaterial state is just "a
five-valued variable indicating the current ColorMaterial mode, a bit
indicating whether or not COLOR_MATERIAL is enabled" — the face/mode selection
is plain state, with no wording that makes changing it retroactive.

The `glColorMaterial` reference page
(<https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/glColorMaterial.xml>)
agrees but is informative only; it advises "Call glColorMaterial before enabling
GL_COLOR_MATERIAL", which is authoring guidance and not a constraint — the
normative text above attaches no ordering precondition, and the failure here
does not depend on that ordering in any case (see the ablation below).

## What is and is not required to reproduce

Starting from a longer failing sequence and removing one element at a time,
only the retarget matters:

| element removed | result |
|---|---|
| the `glMaterialfv` sentinel resets | still fails |
| the preceding `glColor4f(1,1,1,1)` | still fails |
| the preceding `glDisable(GL_COLOR_MATERIAL)` | still fails |
| **the `glColorMaterial(GL_BACK, …)` retarget** | **passes** |

In particular the relative order of `glEnable(GL_COLOR_MATERIAL)` and the first
`glColorMaterial` does **not** matter: both orderings pass in a fresh context
when no retarget follows, and both fail when one does. Enabling before or after
selecting the face is not the trigger.

Also verified: the spec's own `ColorMaterial(FRONT, AMBIENT)` example (p. 519)
passes on Mesa. Writing the named face works; only the outgoing face is lost.

## Two traps when reducing this

Both are worth knowing before writing a test, and both cost time here:

1. **Use a distinctive color.** The unwritten material sits at GL's default
   white, so a test that tracks white passes for the wrong reason — it cannot
   distinguish "tracked correctly" from "never written".

2. **Do not query between the `glColor` and the face switch.** A
   `glGetMaterialfv` in that window makes the bug vanish (case C). That is the
   signature of pending material state which the query path realizes but the
   `glColorMaterial` path does not: the retarget appears not to flush the
   pending color into the outgoing face before switching.

Consequence of (2): the bug is invisible to any test that asserts state as it
goes, and only appears when an application does the natural thing and never
queries.

## Environment

Reproduced on two independent backends, which points at shared core code rather
than a hardware driver:

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS, kernel 6.8.0-137-generic |
| GPU | Intel Alder Lake-N UHD Graphics `[8086:46d2]` |
| Backend 1 | `Mesa Intel(R) Graphics (ADL-N)` (iris), GL 4.6 compat, Mesa **25.2.8** (`libgl1-mesa-dri` 25.2.8-0ubuntu0.24.04.2) |
| Backend 2 | `llvmpipe (LLVM 20.1.2, 256 bits)` via OSMesa, GL 4.5 compat, Mesa **25.1.7** (`libosmesa6` 25.1.7-1ubuntu2~24.04.2) |

Not reproducible on NVIDIA (proprietary) or Apple's OpenGL: both report
`0.20 0.60 0.30`.

## Possibly related

Mesa 25.0.6 release notes list:

> glPushAttrib/glPopAttrib broken with glColorMaterial and ligthing
> — *mesa: fix color material tracking* (Timothy Arceri, 3 commits)

Same subsystem, and the same shape of "color material tracking is not realized
at the right moment". That `glPushAttrib`/`glPopAttrib` regression is fixed in
the versions tested here — a repeated push/pop cycle around this sequence is
stable — so this looks like a remaining case in the same area rather than a
recurrence.

## Real-world impact

Found in a fixed-function scene that colors the two sides of an open cube
independently — the exterior once, then per-face accents on the interior:

```c
glEnable(GL_COLOR_MATERIAL);
glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
glColor3f(0.92, 0.95, 0.98);                  /* exterior: near-white */
glColorMaterial(GL_BACK, GL_AMBIENT_AND_DIFFUSE);
/* glBegin ... per-face glColor for the interior accents ... */
```

This is an idiomatic use of color material — it is the reason the *face*
parameter exists — and the exterior color is silently dropped. Outward-facing
polygons render with whatever the front material previously held, which in a
real frame loop is rarely the harmless default. The same source renders
correctly on NVIDIA and Apple GL, so it presents as a Mesa-only "geometry is
black" bug with no obvious cause in the application.

Workarounds: set the material explicitly with
`glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, ...)` rather than routing it
through color tracking, or avoid retargeting the tracked face while enabled.
