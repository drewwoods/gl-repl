<div align="center">

<img src="docs/images/logo.svg" width="64">

# gl-repl

**Render as you type**

[![macOS](https://github.com/drewwoods/gl-repl/actions/workflows/macos.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/macos.yml)
[![Linux](https://github.com/drewwoods/gl-repl/actions/workflows/linux.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/linux.yml)

</div>

<table>
<tr>
<td width="50%">

```c
// type this —
glRotatef(t * 30, 0, 1, 0);
glColor3f(0.95, 0.44, 0.66);
glutSolidTorus(0.3, 0.9, 24, 48);

// press Ctrl+T —
// the torus spins. that's the loop.
```

</td>
<td width="50%">

<img src="docs/images/tour-editing-basics.gif" width="100%">

</td>
</tr>
</table>

<div align="center"><sub>the scene renders beside the source that made it — no build step</sub></div>

---

## The 30-second demo

```c
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_LIGHTING); glEnable(GL_LIGHT0);
glRotatef(t * 30, 0, 1, 0);
glutSolidTorus(0.3, 0.9, 24, 48);
```

Type these, watch each line land as you commit it. Anything you build exports to
a standalone C89 program — no REPL, no runtime of ours.

## Install

```bash
make gl-repl && ./gl-repl
```

macOS needs cmake, Linux needs `freeglut3-dev`. Docs images live in Git LFS —
`git lfs pull` after cloning.

---

## Why it's different

| | |
|---|---|
| **Time as a variable** | `Ctrl+T` toggles `t`. Reference it anywhere — animation with no boilerplate. |
| **Live values** | Every float gets a slider, every color a swatch. Drag, and the scene follows. |
| **A real little language** | Loops, functions, `if`, scratch arrays, a math library — expressions anywhere a number goes. |
| **Replay your draws** | `Ctrl+R` steps through the command stream call by call. |
| **Runs in the browser** | Compiles to wasm via gl4es — parity with native, blur included. |
| **Sketch here, ship as C** | `Ctrl+S` exports a standalone GLUT/OpenGL program that round-trips back in. |

---

## Showcase

<div align="center">
  <img src="docs/images/showcase/lantern-festival.gif" width="23%">
  <img src="docs/images/showcase/whale.gif" width="23%">
  <img src="docs/images/showcase/torus-knot.gif" width="23%">
  <img src="docs/images/showcase/aurora-observatory.gif" width="23%">
  <br>
  <sub>40 built-in scenes — <b><a href="docs/SHOWCASE.md">browse the showcase →</a></b></sub>
</div>

---

<div align="center">

`;` commit · `Ctrl+T` time · `Ctrl+R` replay · `Ctrl+S` save · `F1` full key reference

[User Guide](../docs/USER_GUIDE.md) ·
[Showcase](../docs/SHOWCASE.md) ·
[Advanced Usage](../docs/ADVANCED_USAGE.md) ·
Contributing ·
[Architecture](../docs/ARCHITECTURE.md)

<sub>C99 · OpenGL 1.1 · GLU · freeglut · MIT</sub>

</div>
