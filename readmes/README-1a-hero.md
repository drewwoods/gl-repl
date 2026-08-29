<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/glrepl-wordmark-dark.svg">
  <img src="docs/images/glrepl-wordmark-light.svg" alt="gl-repl — immediate mode, immediately" width="420">
</picture>

**Render as you type**

[![macOS](https://github.com/drewwoods/gl-repl/actions/workflows/macos.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/macos.yml)
[![Linux](https://github.com/drewwoods/gl-repl/actions/workflows/linux.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/linux.yml)

</div>

An interactive interpreter for fixed-function OpenGL. As you type GL, the scene
renders beside the source that made it. No build step, just immediate mode,
immediately.

<sub>Strictly a Read-Eval-**Render** Loop, but RERL is unpronounceable.</sub>

```c
glBegin(GL_TRIANGLES);
  glVertex3f(0, 1, 0);
  glVertex3f(-1, -1, 0);
  glVertex3f(sin(t), -1, 0);   // expressions everywhere; t animates
glEnd();
```

A triangle. One vertex rides `sin(t)`. Press `Ctrl+T` and it sways.

---

## What you get

| | |
|---|---|
| **Render as you type** | The geometry lives in your code. Edit a `glVertex3f`, see the vertex move. |
| **Time as a variable** | `Ctrl+T` toggles `t`. Reference it anywhere — animation with no boilerplate. |
| **A real little language** | Loops, functions, `if`, variables, scratch arrays, a math library — an expression goes anywhere a number does. |
| **Live values** | Every float gets a slider, every number a stepper, every color a swatch. Drag, and the scene follows. |
| **See what your code means** | Cursor on a `glRotatef` line? An arc guide shows it in the scene. Vertex labels and normals follow your cursor. |
| **Replay your draws** | `Ctrl+R` steps through the command stream call by call, loop variables substituted live. |
| **Runs in the browser** | Compiles to wasm via gl4es, at parity with native — accumulation blur included. |
| **Sketch here, ship as C** | `Ctrl+S` exports a standalone, compilable GLUT/OpenGL program that round-trips back into the REPL. |

---

<div align="center">
  <img src="docs/images/showcase/ripple-ring.gif" width="30%">
  <img src="docs/images/xform-guide.gif" width="30%">
  <img src="docs/images/showcase/ringed-planet.gif" width="30%">
  <br>
  <sub><b><a href="docs/USER_GUIDE.md">Read the User Guide →</a></b> every feature, with screenshots.
  Three of 40 built-in scenes, from the <a href="docs/SHOWCASE.md">showcase</a>.</sub>
</div>

---

## Quick start

```bash
# macOS: needs cmake — builds the vendored freeglut
# Linux: sudo apt install freeglut3-dev
make gl-repl

./gl-repl                                     # fresh session
./gl-repl --example "Torus knot (animated)"   # F12 cycles all 40
./gl-repl output.c                            # reload a saved session
```

Screenshots and GIFs live in Git LFS — `git lfs install && git lfs pull` before
cloning, or docs images arrive as text pointers.

---

## Keys

| | | | |
|---|---|---|---|
| `;` | commit | `Ctrl+R` | replay |
| `Ctrl+T` | toggle time | `Ctrl+S` | save / export C |
| `Tab` | autocomplete | `F1` | help |

---

## Documentation

| | |
|---|---|
| [User Guide](../docs/USER_GUIDE.md) | The manual — every feature, with screenshots. |
| [Showcase](../docs/SHOWCASE.md) | The built-in scenes and the source that draws them. |
| [Architecture](../docs/ARCHITECTURE.md) | The deep dive — command model, frame pipeline, boundaries. |
| Contributing | Build, test, the guard suite, and how to extend the REPL. |

<div align="center"><sub>C99 · OpenGL 1.1 · GLU · freeglut · MIT</sub></div>
