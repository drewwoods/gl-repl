<div align="center">

# gl-repl — Tutorials

<sub>Short, hands-on walkthroughs. Type the lines, press `;`, watch it happen.</sub>

<br>

<!-- TODO: GIF — a 5-8s loop of the REPL: type a glRotatef + glutSolidTeapot, press Ctrl+T, teapot spins. This is the "hook" GIF for the whole tutorials section. -->
<img src="assets/00-overview.gif" alt="gl-repl in action: type a command, geometry renders" width="80%">

<sub><i>Capture: a fresh session, type the teapot snippet from Tutorial 1, then <code>Ctrl+T</code>.</i></sub>

</div>

---

## Before you start

```bash
make sample        # build (see the top-level README for prerequisites)
./sample           # launch a fresh session
```

Every tutorial is self-contained — start a fresh session, type the lines in
order, and press `;` after each. Nothing here needs a previous tutorial's
state. If you make a mess, `Ctrl+Z` undoes; `F12` cycles to a clean example.

> [!TIP]
> Press **F1** at any time for the in-app help overlay (commands + key bindings).

---

## The path

| # | Tutorial | You'll learn |
|---|---|---|
| 1 | [**First triangle**](01-first-triangle.md) | `glBegin`/`glVertex`/`glEnd`, the commit loop |
| 2 | [**Color & transforms**](02-color-and-transforms.md) | `glColor3f`, `glTranslatef`, `glRotatef`, `glScalef` |
| 3 | [**Animating with `t`**](03-animating-with-time.md) | the time variable, `Ctrl+T`, motion as a function of `t` |
| 4 | [**Loops & functions**](04-loops-and-functions.md) | `for(...)`, `func0(...)`, building a lot from a little |
| 5 | [**Variables & sliders**](05-variables-and-sliders.md) | `float name;`, live editing via the variable panel |
| 6 | [**Lighting & material**](06-lighting-and-material.md) | `glEnable(GL_LIGHTING)`, normals, `glMaterialfv` |
| 7 | [**Replay & export**](07-replay-and-export.md) | `Ctrl+R` step-through, save to `output.c`, `F11` → `.ply` |

---

## Key bindings used throughout

| Key | Action | | Key | Action |
|---|---|---|---|---|
| `;` | commit current line | | `Ctrl+T` | toggle time variable `t` |
| `Enter` | new line | | `Ctrl+R` | start / stop replay |
| `Tab` | autocomplete | | `Ctrl+\` | reformat all lines |
| `↑ ↓` | navigate lines | | `Ctrl+G` | toggle wireframe |
| `Ctrl+Z / Y` | undo / redo | | `Ctrl+S` | save to `output.c` |
| `Ctrl+F` | find | | `F11` | export geometry to `.ply` |
| `Ctrl+C/X/V` | copy / cut / paste | | `F12` | next example / scene |

<sub>On macOS, <b>Cmd</b>+letter is normalized to its <b>Ctrl</b> equivalent.</sub>

---

<div align="center">

<sub>GIFs live in <a href="assets/"><code>assets/</code></a> · see each tutorial for the exact capture note.</sub>

</div>
