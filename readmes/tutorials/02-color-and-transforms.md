# 2 · Color & transforms

> **Goal:** color your geometry and move it around with the transform stack.
> **Time:** ~4 minutes. &nbsp; **Prereq:** [Tutorial 1](01-first-triangle.md).

---

## Color

Color is **state**: `glColor3f(r, g, b)` sets the current color, and every
vertex committed afterward uses it until you change it. Channels run `0`–`1`.

```c
glColor3f(0.2, 0.8, 1.0);
glBegin(GL_TRIANGLES);
glVertex3f(-0.5, -0.5, 0);
glVertex3f(0.5, -0.5, 0);
glVertex3f(0, 0.5, 0);
glEnd();
```

Set a *different* color before each vertex and OpenGL blends across the face:

```c
glBegin(GL_TRIANGLES);
glColor3f(1, 0, 0); glVertex3f(-0.5, -0.5, 0);
glColor3f(0, 1, 0); glVertex3f(0.5, -0.5, 0);
glColor3f(0, 0, 1); glVertex3f(0, 0.5, 0);
glEnd();
```

<!-- TODO: GIF — the classic RGB gradient triangle building up as each colored vertex commits. -->
<div align="center">
<img src="assets/02-gradient-triangle.gif" alt="A triangle with red, green, blue corners blending" width="80%">
<br><sub><i>Capture: type the per-vertex color snippet; the gradient fills on <code>glEnd()</code>.</i></sub>
</div>

---

## Transforms

Transforms reposition everything drawn *after* them. The three you'll use most:

```c
glTranslatef(x, y, z);     // move
glRotatef(deg, x, y, z);   // rotate `deg` degrees about axis (x,y,z)
glScalef(sx, sy, sz);      // scale per-axis
```

Try a rotated, shifted cube (this one's 3D, so push it back from the camera):

```c
glTranslatef(0, 0, -5);
glRotatef(30, 1, 1, 0);
glColor3f(0.9, 0.6, 0.2);
glutSolidCube(1.5);
```

<!-- TODO: GIF — committing the translate, then the rotate, then the cube; show the cube appearing already tilted. -->
<div align="center">
<img src="assets/02-transformed-cube.gif" alt="A translated, rotated solid cube" width="80%">
<br><sub><i>Capture: type the four lines; the cube lands shifted back and tilted.</i></sub>
</div>

---

## The push/pop stack

`glPushMatrix()` / `glPopMatrix()` save and restore the transform so one
object's movement doesn't leak into the next:

```c
glTranslatef(0, 0, -6);

glPushMatrix();
glTranslatef(-1.5, 0, 0);
glColor3f(1, 0.3, 0.3);
glutSolidSphere(0.6, 24, 24);
glPopMatrix();        // back to where we were

glPushMatrix();
glTranslatef(1.5, 0, 0);
glColor3f(0.3, 0.6, 1);
glutSolidSphere(0.6, 24, 24);
glPopMatrix();
```

Two spheres, side by side — each `Translatef` measured from the same origin
because the stack reset in between.

> [!IMPORTANT]
> OpenGL applies transforms in **reverse source order** to each vertex. Park
> your cursor on a `glTranslatef`/`glRotatef`/`glScalef` line and an on-screen
> **transform guide** (arrow or arc) shows exactly what that line does. Toggle
> guides with **F8**.

<!-- TODO: GIF — cursor resting on a glRotatef line, the arc guide sweeping in the viewport. -->
<div align="center">
<img src="assets/02-transform-guide.gif" alt="The on-screen transform guide for a rotate line" width="80%">
<br><sub><i>Capture: place the cursor on the <code>glRotatef</code> line; the rotation-arc guide draws in 3D.</i></sub>
</div>

---

**Next →** [Animating with `t`](03-animating-with-time.md)
