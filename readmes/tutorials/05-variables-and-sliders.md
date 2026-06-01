# 5 · Variables & sliders

> **Goal:** declare your own variables and tune them live with the slider panel.
> **Time:** ~4 minutes. &nbsp; **Prereq:** [Tutorial 4](04-loops-and-functions.md).

---

## Declaring a variable

Beyond `t`, you can declare your own with `float name;`. Declarations are
hoisted to the top of the program automatically, so a value is always defined
before it's used.

```c
float n;
n = 5;
```

Assign with ordinary expressions. Then reference `n` anywhere a number goes:

```c
glTranslatef(0, 0, -6);
glColor3f(0.4, 0.8, 1);
glutSolidSphere(1, n, n);
```

Change `n` and re-commit — the sphere's tessellation (its slice/stack count)
follows. Low `n` is chunky and faceted; high `n` is smooth.

<!-- TODO: GIF — declaring float n, drawing the sphere with n slices/stacks, then editing n from say 4 -> 32 and re-committing; sphere goes from faceted to smooth. -->
<div align="center">
<img src="assets/05-tessellation.gif" alt="A sphere's facet count changing as n changes" width="80%">
<br><sub><i>Capture: <code>float n; n = 4;</code> → sphere → edit n up to 32 → smoother sphere.</i></sub>
</div>

---

## The variable slider panel

Declared variables show up in the **variable panel** as sliders. Drag a
slider and the value updates live — no re-typing, no commit. It's the fastest
way to *feel* how a parameter affects the scene.

```c
float wobble;
wobble = 0.5;

glTranslatef(0, 0, -5);
glRotatef(t * 30, 0, 1, 0);
glScalef(1, 1 + sin(t * 3) * wobble, 1);
glColor3f(1, 0.5, 0.3);
glutSolidTorus(0.3, 0.9, 24, 48);
```

Press `Ctrl+T` to start time, then drag the `wobble` slider from `0` upward and
watch the torus pulse harder. The slider is editing the same `wobble` your
source refers to.

<!-- TODO: GIF — Ctrl+T running, then grabbing the `wobble` slider in the variable panel and dragging it up; the torus's vertical pulse grows. Show the panel clearly. -->
<div align="center">
<img src="assets/05-slider-drag.gif" alt="Dragging the wobble slider to change a live animation" width="80%">
<br><sub><i>Capture: run the snippet → <code>Ctrl+T</code> → drag the <code>wobble</code> slider; pulse amplitude grows.</i></sub>
</div>

> [!NOTE]
> If the variable panel isn't showing, toggle it from the **Config** menu on the
> code-panel header (the `variable_panel` item).

---

## Scratch arrays

For loop/recursive algorithms there are three fixed global arrays — `A`, `B`,
`C` — each with indices `0`–`7`. Read and write them like normal variables:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;   // a step of a lerp
glVertex3f(A[0], 0, 0);
```

There are up to 23 user-declared `float` slots (plus `t`). Names like `A`/`B`/`C`,
`PI`, `TAU`, `t`, `float`, and `var` are reserved.

---

**Next →** [Lighting & material](06-lighting-and-material.md)
