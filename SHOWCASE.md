<div align="center">

# Showcase

<sub>What a few dozen lines of fixed-function OpenGL look like when the geometry lives in the code.</sub>

<br>

<sub>Every scene below ships in the binary. Cycle them with <b>F12</b>, or load one directly:</sub>

```bash
./gl-repl --example "Torus knot (animated)"    # by name (case-insensitive)
./gl-repl --example 15                         # or by 0-based index
./gl-repl --list-examples                      # canonical names + indices
```

</div>

<!--
  ASSET NOTES — how the media on this page is produced.

  Existing images come from the docs pipeline (scripts/docs-assets.sh ->
  docs/images/). New captures referenced below live under
  docs/images/showcase/ and are recorded headlessly:

      make gl-repl FREEGLUT_OSMESA=1
      scripts/record-gif.sh --example "<name>" --duration 6 --out <slug>

  Each PLACEHOLDER comment names the example, what the shot should show,
  and a starting command. Camera angle, fps, resolution, and duration are
  all up for grabs — the blurb describes intent, not pixels.
-->

---

## ✦ Featured

A few scenes worth seeing with their source — note how short each one is.
The whole program is what you'd type into the REPL; there is no other file.

### Torus knot

A `(p, q)` knot traced as a single `GL_LINE_LOOP`, hue cycling along the curve.

<!-- PLACEHOLDER docs/images/showcase/torus-knot.gif
     Shot: the (2,3) knot rotating with color cycling along the curve.
     Generate: scripts/record-gif.sh --example "Torus knot (animated)" --duration 6 --out torus-knot
     Intent: show that one for-loop + line loop reads as a complex object. -->
<div align="center">
<img src="docs/images/showcase/torus-knot.gif" alt="An animated (2,3) torus knot in cycling color" width="70%">
</div>

```c
n = 400;  p = 2;  q = 3;            // samples, winds around axis / through hole
glBegin(GL_LINE_LOOP);
  for(i, 0, n) {
    ang = TAU * i/n;
    rr = 2.0 + cos(q*ang);          // distance from the axis
    glColor3f(0.5 + 0.5*sin(ang + t), 0.5 + 0.5*sin(ang + t + 2), 0.5 + 0.5*sin(ang + t + 4));
    glVertex3f(rr*cos(p*ang), rr*sin(p*ang), sin(q*ang));
  }
glEnd();
```

---

### Snowfall — 550 deterministic particles

No particle system, no stored state. Each flake is a pure function of its
index and `t`, wrapped with `rem(...)`, so the whole field replays identically.

<!-- PLACEHOLDER docs/images/showcase/snowfall.gif
     Shot: the full flake field drifting and falling; a few seconds is plenty.
     Generate: scripts/record-gif.sh --example "Snowfall demo (550 particles)" --duration 5 --out snowfall
     Intent: density + motion — sell "550 particles, zero stored state". -->
<div align="center">
<img src="docs/images/showcase/snowfall.gif" alt="550 snow particles drifting and falling" width="70%">
</div>

```c
glPointSize(10);
glBegin(GL_POINTS);
  for (p, 0, 550) {
    drift  = driftMax * rand2(p, 1);
    flakeX = rem(wrapX * rand2(p,4) + t * drift,  2 * wrapX);
    fall   = fallBase - rand(p, 2);
    flakeY = rem(wrapY * rand2(p,3) + t * fall,   2 * wrapY);
    // depth -> shade; nearer flakes brighter
  }
glEnd();
```

<sub>*(abridged — load the example for the full source)*</sub>

---

### Parametric torus

Two nested loops sweep a `GL_QUAD_STRIP` around the tube and the ring. The
same example exports cleanly to `.ply` — it's the one in the
[`--export-ply` docs](ADVANCED_USAGE.md#mesh-export-ply).

<!-- PLACEHOLDER docs/images/showcase/parametric-torus.gif
     Shot: the shaded torus, ideally with a slow camera orbit (camera
     auto-rotate via @cfg camera_rotate, or a still PNG is fine too).
     Generate: scripts/record-gif.sh --example "Parametric torus (nested for)" --duration 6 --out parametric-torus
     Intent: smooth shaded surface from two nested for-loops. -->
<div align="center">
<img src="docs/images/showcase/parametric-torus.gif" alt="A shaded parametric torus built from quad strips" width="70%">
</div>

```c
for(i, 0, n) {
  glBegin(GL_QUAD_STRIP);
  for(j, 0, n+1) {
    v = j*TAU/n;  u = i*TAU/n;
    glColor3f(sin(u)*0.4+0.6, cos(v)*0.4+0.6, 0.5);
    glVertex3f((R + r*cos(v))*cos(u), (R + r*cos(v))*sin(u), r*sin(v));
    u = (i+1)*TAU/n;
    glVertex3f((R + r*cos(v))*cos(u), (R + r*cos(v))*sin(u), r*sin(v));
  }
  glEnd();
}
```

---

### Recursive triangle tree

A `branch(depth, size, spin)` function that calls itself — recursion,
inlined by the flattener, capped at depth 64.

<!-- PLACEHOLDER docs/images/showcase/recursive-tree.gif
     Shot: the branching triangle tree; animate t so the spin parameter sways.
     Generate: scripts/record-gif.sh --example "Recursive triangle tree (func + recursion)" --duration 6 --out recursive-tree
     Intent: "the REPL does recursion" in one image. -->
<div align="center">
<img src="docs/images/showcase/recursive-tree.gif" alt="A recursive triangle tree" width="70%">
</div>

```c
branch(depth, size, spin) {
  glColor3f(0.25 + depth*0.14, 0.45 + 0.2*sin(spin), 1 - depth*0.12);
  glBegin(GL_LINE_LOOP);  /* the triangle */  glEnd();
  if(depth > 0) {
    /* recurse: shrink + rotate two children */
  }
}
```

---

## ✦ The full gallery

Grouped by what they show off. Load any with `./gl-repl --example "<name>"`.

### Curves & line art

<table>
<tr>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/spirograph.gif
     scripts/record-gif.sh --example "Animated spirograph curve" --duration 6 --out spirograph -->
<img src="docs/images/showcase/spirograph.gif" alt="Animated spirograph curve" width="100%">

**Animated spirograph**
<br><sub>epitrochoid swept by `t`</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/ripple-ring.gif
     scripts/record-gif.sh --example "Traveling ripple ring" --duration 5 --out ripple-ring -->
<img src="docs/images/showcase/ripple-ring.gif" alt="Traveling ripple ring" width="100%">

**Traveling ripple ring**
<br><sub>a wave running around a loop</sub>

</td>
<td width="33%" align="center">

<img src="docs/images/animated-ring.gif" alt="Animated ring" width="100%">

**Animated ring**
<br><sub>`for` + `t`, line loop + fan</sub>

</td>
</tr>
<tr>
<td align="center">

<!-- PLACEHOLDER docs/images/showcase/bezier.png
     Still is fine. Cursor parked on a control-point line so the guides show:
     GLR_EDIT_LINE=<n> + SIGUSR1 capture, or record a short gif.
     ./gl-repl --example "Bezier curve with guides" -->
<img src="docs/images/showcase/bezier.png" alt="Bezier curve with control-point guides" width="100%">

**Bézier curve**
<br><sub>with control-point guides</sub>

</td>
<td align="center">

<!-- PLACEHOLDER docs/images/showcase/de-casteljau.png
     Still is fine. ./gl-repl --example "Scratch arrays (de Casteljau curve)" -->
<img src="docs/images/showcase/de-casteljau.png" alt="de Casteljau curve via scratch arrays" width="100%">

**de Casteljau curve**
<br><sub>built with scratch arrays `A/B/C`</sub>

</td>
<td align="center">

<!-- PLACEHOLDER docs/images/showcase/orbit-plot.png
     Still is fine — the point is the bitmap label() text annotating the plot.
     ./gl-repl --example "Annotated orbit plot (labels)" -->
<img src="docs/images/showcase/orbit-plot.png" alt="Annotated orbit plot with bitmap labels" width="100%">

**Annotated orbit plot**
<br><sub>bitmap `label(...)` text</sub>

</td>
</tr>
</table>

### Surfaces

<table>
<tr>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/wave-surface.gif
     scripts/record-gif.sh --example "Animated wave surface (analytic normals)" --duration 5 --out wave-surface
     Intent: the lighting rolling across the wave — normals are the star. -->
<img src="docs/images/showcase/wave-surface.gif" alt="Animated wave surface" width="100%">

**Animated wave surface**
<br><sub>analytic per-vertex normals</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/terrain.gif
     scripts/record-gif.sh --example "Procedural terrain (rand grid + sin ripple)" --duration 5 --out terrain -->
<img src="docs/images/showcase/terrain.gif" alt="Procedural terrain grid" width="100%">

**Procedural terrain**
<br><sub>`rand` grid + `sin` ripple</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/lit-cube.png
     Still. The default example 0 — lighting + material basics.
     ./gl-repl --example "Lit cube" -->
<img src="docs/images/showcase/lit-cube.png" alt="A lit cube" width="100%">

**Lit cube**
<br><sub>lighting + material basics</sub>

</td>
</tr>
</table>

### Particles & effects

<table>
<tr>
<td width="33%" align="center">

<img src="docs/images/glow-sprites.png" alt="Additive glow sprites" width="100%">

**Glow sprites**
<br><sub>additive blend + point attenuation</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/grass.gif
     scripts/record-gif.sh --example "Swaying grass field (rand + t)" --duration 5 --out grass -->
<img src="docs/images/showcase/grass.gif" alt="Swaying grass field" width="100%">

**Swaying grass field**
<br><sub>`rand` placement, `t` sway</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/jellyfish.gif
     scripts/record-gif.sh --example "Jellyfish (glDepthMask translucency)" --duration 6 --out jellyfish
     Intent: the translucent bell — shows glDepthMask-style ordering tricks. -->
<img src="docs/images/showcase/jellyfish.gif" alt="Translucent jellyfish" width="100%">

**Jellyfish**
<br><sub>`glDepthMask` translucency</sub>

</td>
</tr>
</table>

### Functions, branching & recursion

<table>
<tr>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/function-demo.png
     Still. ./gl-repl --example "Function demo (named func)" -->
<img src="docs/images/showcase/function-demo.png" alt="Named function demo" width="100%">

**Named function**
<br><sub>define once, reuse</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/function-polygons.png
     Still. ./gl-repl --example "Function polygons (args + for)" -->
<img src="docs/images/showcase/function-polygons.png" alt="Function polygons" width="100%">

**Function polygons**
<br><sub>args + `for`</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/conditional-colors.gif
     scripts/record-gif.sh --example "Conditional colors (if + t)" --duration 5 --out conditional-colors -->
<img src="docs/images/showcase/conditional-colors.gif" alt="Conditional colors" width="100%">

**Conditional colors**
<br><sub>`if` + `t`</sub>

</td>
</tr>
</table>

### Big scenes

<table>
<tr>
<td width="33%" align="center">

<img src="docs/images/labels-orrery.png" alt="Orrery with labels tracking 3D orbits" width="100%">

**Orrery**
<br><sub>`label()` text tracking 3D orbits</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/whale.gif
     scripts/record-gif.sh --example "Whale (particle system + lit model)" --duration 6 --out whale
     Intent: the flagship scene — lit model + particles together. -->
<img src="docs/images/showcase/whale.gif" alt="Whale: particle system + lit model" width="100%">

**Whale**
<br><sub>particle system + lit model</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/stress-test.gif
     scripts/record-gif.sh --example "Stress test (all features)" --duration 6 --out stress-test -->
<img src="docs/images/showcase/stress-test.gif" alt="All-features stress test" width="100%">

**Stress test**
<br><sub>everything at once</sub>

</td>
</tr>
</table>

### Tessellation & guides

<table>
<tr>
<td width="50%" align="center">

<img src="docs/images/glu-tess.png" alt="GLU tessellated concave arrow" width="100%">

**GLU tessellator**
<br><sub>concave polygon (+ cutout variant)</sub>

</td>
<td width="50%" align="center">

<img src="docs/images/transform-stress.png" alt="Transform stress with guides" width="100%">

**Transform stress**
<br><sub>translate / rotate / scale guides</sub>

</td>
</tr>
</table>

---

## ✦ Beyond the still image

The REPL isn't only a renderer — these are worth a GIF of the *interaction*:

<table>
<tr>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/feature-time.gif
     Shot: any animated example frozen, then Ctrl+T pressed — everything
     written in t starts moving. Needs an interactive recording (the
     headless pipeline can fake it with two clips: --time 0 paused vs playing). -->
<img src="docs/images/showcase/feature-time.gif" alt="Toggling the time variable" width="100%">

**`Ctrl+T` — time**
<br><sub>everything written in `t` moves</sub>

</td>
<td width="33%" align="center">

<img src="docs/images/replay.gif" alt="Replay stepping through draws" width="100%">

**`Ctrl+R` — replay**
<br><sub>watch the scene draw itself</sub>

</td>
<td width="33%" align="center">

<!-- PLACEHOLDER docs/images/showcase/feature-sliders.gif
     Shot: dragging a variable-panel slider while the scene responds live.
     Interactive recording; docs/images/variable-panel.png is the still
     stand-in until then. -->
<img src="docs/images/variable-panel.png" alt="Variable slider panel" width="100%">

**Live sliders**
<br><sub>drag a `float`, scene responds</sub>

</td>
</tr>
<tr>
<td align="center">

<img src="docs/images/xform-guide.gif" alt="On-screen transform guide" width="100%">

**Transform guides**
<br><sub>see what a `glRotatef` line does</sub>

</td>
<td align="center">

<!-- PLACEHOLDER docs/images/showcase/feature-ply.png
     Shot: an exported .ply (parametric torus is the canonical one) opened
     in MeshLab/Blender. One still is plenty.
     ./gl-repl --example "Parametric torus (nested for)" --export-ply torus.ply -->
<img src="docs/images/showcase/feature-ply.png" alt="Exported PLY mesh in an external tool" width="100%">

**`F11` — `.ply` export**
<br><sub>take the geometry anywhere</sub>

</td>
<td align="center">

<!-- PLACEHOLDER docs/images/showcase/feature-export-c.png
     Shot: the exported output.c open in an editor beside the running
     standalone binary — "it round-trips" in one image. -->
<img src="docs/images/showcase/feature-export-c.png" alt="Saving to standalone C" width="100%">

**`Ctrl+S` — standalone C**
<br><sub>round-trips as `output.c`</sub>

</td>
</tr>
</table>

---

<div align="center">

<sub>· · ·</sub>

<sub>back to the <a href="README.md">README</a> · the <a href="USER_GUIDE.md">User Guide</a> covers every feature shown here</sub>

</div>
