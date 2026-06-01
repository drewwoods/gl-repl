# 1 · First triangle

> **Goal:** draw your first piece of geometry and learn the type-and-commit loop.
> **Time:** ~2 minutes. &nbsp; **Prereq:** none.

---

The whole REPL is one habit: type a line, press `;`, see it render. Let's draw
the simplest thing there is — a single triangle.

Start a fresh session:

```bash
./sample
```

Type these lines, pressing `;` after **each one**:

```c
glBegin(GL_TRIANGLES);
glVertex2f(-0.5, -0.5);
glVertex2f(0.5, -0.5);
glVertex2f(0, 0.5);
glEnd();
```

As soon as you commit `glEnd()`, a triangle fills the viewport.

<!-- TODO: GIF — record typing all five lines one at a time, each ; landing, the triangle building up vertex by vertex and filling on glEnd(). -->
<div align="center">
<img src="assets/01-first-triangle.gif" alt="Typing five lines to draw a triangle" width="80%">
<br><sub><i>Capture: fresh session → type the five lines above, one <code>;</code> at a time.</i></sub>
</div>

---

## What just happened

- `glBegin(GL_TRIANGLES)` opens a block that interprets the next vertices as
  triangle corners.
- Each `glVertex2f(x, y)` places a corner. (`2f` = 2D; `z` is assumed `0`.)
- `glEnd()` closes the block — and the triangle appears.

The code panel on the left always shows exactly what you've committed. That
panel **is** the program: there's no hidden state, no data file. Edit a number
and the geometry follows.

---

## Try it

1. Navigate up to a `glVertex2f` line with `↑`, change a coordinate, and
   re-commit with `;`. The corner moves.
2. Add a color *before* `glBegin` and re-run:
   ```c
   glColor3f(1, 0.4, 0.2);
   ```
   The triangle turns orange. (More on color in [Tutorial 2](02-color-and-transforms.md).)

<!-- TODO: GIF — dragging one vertex's coordinate by re-editing the line; corner moves live. -->
<div align="center">
<img src="assets/01-edit-vertex.gif" alt="Editing a vertex coordinate and re-committing" width="80%">
<br><sub><i>Capture: select a <code>glVertex2f</code> line, edit a number, <code>;</code> — the corner jumps.</i></sub>
</div>

---

> [!TIP]
> Start typing `glVe` and press **Tab** — autocomplete fills in `glVertex3f(`
> and shows the parameter hint. It's the fastest way to learn the command names.

---

**Next →** [Color & transforms](02-color-and-transforms.md)
