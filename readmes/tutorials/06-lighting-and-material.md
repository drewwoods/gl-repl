# 6 · Lighting & material

> **Goal:** turn flat silhouettes into shaded surfaces with light and material.
> **Time:** ~5 minutes. &nbsp; **Prereq:** [Tutorial 5](05-variables-and-sliders.md).

---

## Turning on the lights

Without lighting, a solid is a flat color blob. Three enables wake up the
fixed-function lighting model:

```c
glEnable(GL_DEPTH_TEST);   // so nearer surfaces win
glEnable(GL_LIGHTING);     // shade by light + normal
glEnable(GL_LIGHT0);       // a default light

glTranslatef(0, 0, -5);
glColor3f(0.7, 0.7, 0.75);
glutSolidSphere(1.2, 32, 32);
```

The sphere now has a highlight and a shaded side. The GLUT solids carry their
own surface normals, so they light correctly out of the box.

<!-- TODO: GIF — committing the three enables one at a time over a sphere; the sphere goes from flat disk to shaded ball as GL_LIGHTING + GL_LIGHT0 land. -->
<div align="center">
<img src="assets/06-lighting-on.gif" alt="A sphere gaining shading as lighting is enabled" width="80%">
<br><sub><i>Capture: draw the sphere first, then enable LIGHTING and LIGHT0 — watch it gain depth.</i></sub>
</div>

> [!IMPORTANT]
> Up to four lights — `GL_LIGHT0` through `GL_LIGHT3`. Toggle **light indicators**
> (`Ctrl+Shift+L`) to see markers for where each enabled light sits in the scene.

---

## Color material — colors that respond to light

By default, lit surfaces ignore `glColor3f`. Enable `GL_COLOR_MATERIAL` and
your colors feed the material instead, so a lit object keeps its hue:

```c
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_COLOR_MATERIAL);

glTranslatef(0, 0, -5);
glRotatef(t * 30, 0, 1, 0);
glColor3f(0.9, 0.3, 0.2);   // now this lights correctly
glutSolidTeapot(1.0);
```

Press `Ctrl+T`. A red, shaded, spinning teapot.

---

## Explicit material

For finer control, set material properties directly. `glMaterialfv` takes a
compound literal; `GL_SHININESS` takes a single value:

```c
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);

glTranslatef(0, 0, -5);
glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.1, 0.4, 0.9, 1});
glMaterialfv(GL_FRONT, GL_SPECULAR, (GLfloat[]){1, 1, 1, 1});
glMaterialfv(GL_FRONT, GL_SHININESS, (GLfloat[]){64});
glutSolidSphere(1.2, 32, 32);
```

A glossy blue sphere with a tight highlight.

<!-- TODO: GIF — editing GL_SHININESS from a low value to a high one and re-committing; the specular highlight tightens. -->
<div align="center">
<img src="assets/06-shininess.gif" alt="A sphere's specular highlight tightening as shininess increases" width="80%">
<br><sub><i>Capture: edit the <code>GL_SHININESS</code> value (e.g. 8 → 96); the highlight sharpens.</i></sub>
</div>

---

## See the normals

Shading is driven by surface **normals**. Toggle the normal-vector overlay with
`Ctrl+Shift+N` to draw an arrow at each vertex — handy when your own
`glBegin`/`glVertex` geometry looks wrongly lit (usually a missing or flipped
`glNormal3f`).

```c
glBegin(GL_TRIANGLES);
glNormal3f(0, 0, 1);
glVertex3f(-0.5, -0.5, 0);
glVertex3f(0.5, -0.5, 0);
glVertex3f(0, 0.5, 0);
glEnd();
```

<!-- TODO: GIF — toggling Ctrl+Shift+N to show normal arrows on a hand-built triangle/shape. -->
<div align="center">
<img src="assets/06-normals.gif" alt="Normal-vector arrows shown on geometry" width="80%">
<br><sub><i>Capture: draw a shape, press <code>Ctrl+Shift+N</code>; per-vertex normal arrows appear.</i></sub>
</div>

---

**Next →** [Replay & export](07-replay-and-export.md)
