# 4 · Loops & functions

> **Goal:** build a lot of geometry from a little source using `for` and `funcN`.
> **Time:** ~5 minutes. &nbsp; **Prereq:** [Tutorial 3](03-animating-with-time.md).

---

## Loops

`for(var, start, end)` repeats its body, with `var` running from `start` to
`end`. An optional fourth argument is the step.

```c
for(i, 0, 8) {
  glPushMatrix();
  glRotatef(i * 45, 0, 1, 0);
  glTranslatef(2, 0, 0);
  glColor3f(i / 8, 0.5, 1 - i / 8);
  glutSolidCube(0.4);
  glPopMatrix();
}
```

Eight cubes in a ring — each rotated `45°` further around the center. The loop
variable `i` is just another value you can use in expressions.

<!-- TODO: GIF — committing the for-loop block; the ring of 8 colored cubes appears at once on the closing brace. -->
<div align="center">
<img src="assets/04-cube-ring.gif" alt="A ring of eight cubes drawn by a for-loop" width="80%">
<br><sub><i>Capture: type the <code>for</code> block; the whole ring renders when you commit the <code>}</code>.</i></sub>
</div>

> [!TIP]
> Combine the loop variable with `t`: change `glRotatef(i * 45, ...)` to
> `glRotatef(i * 45 + t * 20, ...)` and press `Ctrl+T` — the whole ring turns.

---

## Functions

Define reusable geometry with `func0` through `func9`. Parentheses are always
required, even with no arguments. You can also alias a name to the next free
slot: `petal(angle) { ... }`.

```c
func0(size) {
  glColor3f(0.9, 0.7, 0.2);
  glutSolidCone(size, size * 2, 16, 4);
}

glTranslatef(0, 0, -7);
for(k, 0, 6) {
  glPushMatrix();
  glRotatef(k * 60, 0, 0, 1);
  glTranslatef(0, 1.2, 0);
  func0(0.4);
  glPopMatrix();
}
```

A function defines the geometry once; the loop stamps it six times into a
flower. Edit the body of `func0` and **every** instance updates together.

<!-- TODO: GIF — define func0, then the loop; show editing the func0 body (e.g. color) and all six cones updating at once. -->
<div align="center">
<img src="assets/04-flower-function.gif" alt="Six cones stamped by a function in a loop, edited together" width="80%">
<br><sub><i>Capture: build the flower, then edit <code>func0</code>'s color and re-commit — all petals change.</i></sub>
</div>

---

## How this works under the hood

What you type is the **source**. Before each frame the REPL expands it into a
**flat** program — loops unrolled, functions inlined, `if`-blocks resolved —
and walks that flat list to emit real GL calls.

```
source  ── for/func/if ──▶  flat (fully expanded)  ──every frame──▶  GL calls
```

You only ever edit the source; the flat program is a cache, rebuilt
automatically. Loops are capped (visit budget 200,000, call depth 64) so a
runaway expansion can't hang the frame.

---

**Next →** [Variables & sliders](05-variables-and-sliders.md)
