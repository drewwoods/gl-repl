# 3 · Animating with `t`

> **Goal:** make things move using the predefined time variable.
> **Time:** ~3 minutes. &nbsp; **Prereq:** [Tutorial 2](02-color-and-transforms.md).

---

There is exactly one predefined variable: **`t`**, the time. It's frozen at the
value it had when you stopped it, and **`Ctrl+T`** toggles it advancing. Reset
it to `0` with **`Ctrl+Shift+T`**.

Anything written as a function of `t` animates. Build a lit, spinning teapot:

```c
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glTranslatef(0, 0, -5);
glRotatef(t * 30, 0, 1, 0);
glColor3f(0.25, 0.79, 1.0);
glutSolidTeapot(1.0);
```

Now press **`Ctrl+T`**. The teapot turns. The number in front of `t` is the
rate — degrees per unit of time. Change `30` to `120` and re-commit for a
faster spin.

<!-- TODO: GIF — the teapot snippet committed, then Ctrl+T pressed, teapot begins rotating; ideally show editing 30 -> 120 and the speed-up. -->
<div align="center">
<img src="assets/03-spinning-teapot.gif" alt="A lit teapot rotating after Ctrl+T" width="80%">
<br><sub><i>Capture: type the snippet → <code>Ctrl+T</code> → spin. Bonus: edit the rate live.</i></sub>
</div>

---

## Motion is recomputed, not accumulated

Each frame, the scene is **re-evaluated** from the current `t` — nothing is
nudged or stored between frames. That's why `Ctrl+Shift+T` snaps it cleanly
back to the start, and why a paused scene is exactly reproducible.

Combine more than one `t`-driven transform for richer motion:

```c
glTranslatef(0, sin(t * 2) * 0.5, -5);   // bob up and down
glRotatef(t * 45, 0, 1, 0);              // and spin
glColor3f(1, 0.4, 0.7);
glutSolidTorus(0.3, 0.9, 24, 48);
```

<!-- TODO: GIF — a torus bobbing on a sine while spinning; emphasizes combining two t-functions. -->
<div align="center">
<img src="assets/03-bobbing-torus.gif" alt="A torus bobbing on a sine wave while rotating" width="80%">
<br><sub><i>Capture: type the bob+spin snippet → <code>Ctrl+T</code>.</i></sub>
</div>

---

## Deterministic randomness

`rand(seed)` returns a value in `[0, 1]`; `rand2(seed)` maps to `[-1, 1]`. Both
are **deterministic hashes** — same seed, same output, every frame. That's how
you get a scatter of "particles" that looks alive but replays identically:

```c
for(i, 0, 60) {
  glPushMatrix();
  glTranslatef(rand2(i) * 3, rand2(i + 99) * 3, -8);
  glColor3f(rand(i), rand(i + 1), rand(i + 2));
  glutSolidCube(0.15);
  glPopMatrix();
}
```

(That `for(...)` is covered next, in [Tutorial 4](04-loops-and-functions.md).)

> [!NOTE]
> Because the field is a pure function of its seeds, scrub `t` back and forth
> and the scatter lands in exactly the same places — no drift.

---

**Next →** [Loops & functions](04-loops-and-functions.md)
