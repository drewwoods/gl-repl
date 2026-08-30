<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/glrepl-wordmark-dark.svg">
  <img src="docs/images/glrepl-wordmark-light.svg" alt="gl-repl — immediate mode, immediately" width="340">
</picture>

[![macOS](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml)
[![Linux](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml)

An interactive interpreter for fixed-function OpenGL — the scene renders beside
the source that made it. No rebuild while editing scenes.

```bash
# macOS: brew install cmake
# Ubuntu/Debian: sudo apt install freeglut3-dev
make gl-repl && ./gl-repl
./gl-repl --example "Torus knot"              # F12 cycles all 41
```

Also runs in the browser: with Emscripten's `emcc` on `PATH`, run `make web`.
[Web setup →](packaging/web/README.md)

**If you want the images**: the screenshots and GIFs under `docs/images/` live in
**Git LFS** — docs only, nothing in the build needs them. Install it *before*
cloning, or they arrive as pointer text; `git lfs install && git lfs pull` fixes
a clone that already missed them.

Type this into the code panel:

```c
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glRotatef(t * 30, 0, 1, 0);
glutSolidTorus(0.3, 0.9, 24, 48);
```

Now press `Ctrl+T`. The torus spins — the rate is just the scalar in front of
`t`, and every line stays editable.

<img src="docs/images/tour-highlights.gif" alt="Highlights guided tour" width="100%">

<sub>the built-in **Highlights** tour</sub>

- **Render as you type** — edit a `glVertex3f`, see it move.
- **`Ctrl+T` for time** — reference `t` anywhere, animation with no boilerplate.
- **A C-like language** — loops, functions, `if`; expressions wherever a number goes.
- **Runs in the browser** — the same app as wasm via gl4es.
- **`Ctrl+R` replays your draws** — the scene assembles call by call.
- **Export and import** — standalone C89, compiles on its own, round-trips back in.

<a href="docs/SHOWCASE.md"><img src="docs/images/animated-ring.png" alt="Animated ring example" width="32%"></a>
&nbsp;
<a href="docs/SHOWCASE.md"><img src="docs/images/transform-stress.png" alt="Transform guides" width="32%"></a>
&nbsp;
<a href="docs/SHOWCASE.md"><img src="docs/images/labels-orrery.png" alt="Orrery with tracking labels" width="32%"></a>

<sub>three of 41 built-in scenes, each a screenful of typed GL — **[browse the showcase →](docs/SHOWCASE.md)**</sub>

---

[**User Guide**](docs/USER_GUIDE.md) ·
[Showcase](docs/SHOWCASE.md) ·
[Advanced Usage](docs/ADVANCED_USAGE.md) ·
[Contributing](docs/CONTRIBUTING.md) ·
[Modules](docs/MODULES.md) ·
[Architecture](docs/ARCHITECTURE.md)

Press `F1` in-app for the full key reference.

<sub>C99 source · C89 export · OpenGL 1.1 · GLU · freeglut · [MIT](LICENSE)</sub>
