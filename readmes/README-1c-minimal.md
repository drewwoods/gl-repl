<img src="../docs/images/logo.svg" width="32" align="left">

# gl-repl

[![macOS](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml)
[![Linux](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml)

An interactive interpreter for fixed-function OpenGL — the scene renders beside
the source that made it. No build step.

```bash
make gl-repl && ./gl-repl                     # macOS: brew install cmake
./gl-repl --example "Torus knot"              # F12 cycles all 41
```

Linux: `sudo apt install freeglut3-dev`. Also runs in the browser — `make web`.

<img src="../docs/images/tour-editing-basics.gif" width="100%">

<sub>the whole app, in one animation — the built-in **Editing Basics** tour</sub>

- **Render as you type** — edit a `glVertex3f`, see it move.
- **`Ctrl+T` for time** — reference `t` anywhere, animation with no boilerplate.
- **Live sliders** on every float, number, and color.
- **`Ctrl+S` exports standalone C** — no REPL, no runtime of ours.
- **Runs in the browser** too, at parity with native.

---

[User Guide](../docs/USER_GUIDE.md) ·
[Showcase](../docs/SHOWCASE.md) ·
[Advanced Usage](../docs/ADVANCED_USAGE.md) ·
[Contributing](../docs/CONTRIBUTING.md) ·
[Modules](../docs/MODULES.md) ·
[Architecture](../docs/ARCHITECTURE.md)

Press `F1` in-app for the full key reference.

<sub>C99 · OpenGL 1.1 · GLU · freeglut · MIT</sub>
