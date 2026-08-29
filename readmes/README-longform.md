<div align="center">

<br>

<img src="docs/images/glrepl-wordmark-dark.svg#gh-dark-mode-only" alt="gl-repl - immediate mode, immediately" width="70%">
<img src="docs/images/glrepl-wordmark-light.svg#gh-light-mode-only" alt="gl-repl - immediate mode, immediately" width="70%">

**gl-repl**

<sub>Render as you type</sub>

[![macOS CI](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-macos.yml)
[![Linux CI](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/drewwoods/gl-repl/actions/workflows/ci-linux.yml)

</div>

An interactive interpreter for fixed-function OpenGL. As you type GL, the
scene renders beside the source that made it. No build step, just immediate mode, immediately.

<sub>Strictly a Read-Eval-**Render** Loop, but RERL is unpronounceable.</sub>

```c
glBegin(GL_TRIANGLES);
glVertex3f(0, 1, 0);
glVertex3f(-1, -1, 0);
glVertex3f(sin(t), -1, 0);   // expressions everywhere; t animates
// glVertex4f(x, y, z, w) also accepts homogeneous coordinates
glEnd();
```

<sub>A triangle. One vertex rides `sin(t)`. Press `Ctrl+T` and it sways.</sub>

<br>

---

### What you get

| | |
|---|---|
| **Render as you type** | The geometry lives in your code. Edit a `glVertex3f` or `glVertex4f`, see the vertex move. |
| **Time as a variable** | `Ctrl+T` toggles `t`. Reference it anywhere - animation with no boilerplate. |
| **A real little language** | Loops, functions with parameters, `if`, variables, scratch arrays, and a math library - and an expression goes anywhere a number does. A `for` loop unrolls into geometry as you type it. |
| **Live values** | Every `float` gets a slider, every number an inline stepper, every color a swatch with a picker. Drag, and the scene follows. |
| **See what your code means** | Cursor on a `glRotatef` line? An arc guide shows the rotation in the scene. Vertex labels, normal arrows, and polygon highlights follow your cursor. |
| **Replay your draws** | `Ctrl+R` steps through the command stream; watch the scene assemble call by call, loop variables substituted live in the code panel. |
| **AA and motion blur** | Accumulation effects layer 2-16 samples into edge antialiasing, animation-time motion blur, or camera blur - and they work while paused. |
| **Runs in the browser** | The same app compiles to wasm, with gl4es translating the fixed-function GL to WebGL2 - at parity with native, accumulation blur included. |
| **Sketch here, ship as C** | `Ctrl+S` exports a standalone, compilable GLUT/OpenGL program that round-trips back into the REPL. `File → Export .ply` exports the geometry as a PLY mesh. |
| **Driver-differential checks** | Selected state-model behavior has been compared row-by-row on Apple, Mesa, and NVIDIA drivers. [How that's tested](../docs/CONTRIBUTING.md#fidelity-to-opengl) |

<br>

<div align="center">

<a href="docs/SHOWCASE.md"><img src="docs/images/animated-ring.png" alt="Animated ring example" width="31%"></a>
&nbsp;
<a href="docs/SHOWCASE.md"><img src="docs/images/transform-stress.png" alt="Transform guides" width="31%"></a>
&nbsp;
<a href="docs/SHOWCASE.md"><img src="docs/images/labels-orrery.png" alt="Orrery with tracking labels" width="31%"></a>

<sub>**[Read the User Guide →](../docs/USER_GUIDE.md)** - every feature, with screenshots. Start here.</sub>

<sub>These three are from the [showcase](../docs/SHOWCASE.md): 41 built-in scenes, each a screenful of typed GL.</sub>

</div>

<br>

---

### Quick start

```bash
# macOS: needs cmake (brew install cmake) - builds the vendored freeglut
# Linux: sudo apt install freeglut3-dev
make gl-repl

./gl-repl                  # fresh session - type GL commands, ; after each
./gl-repl --example "Torus knot"  # or start from a built-in (F12 cycles all 41)
./gl-repl output.c         # reload a saved session
printf 'glutSolidCube(1);\n' | ./gl-repl -  # load a snippet from stdin
```

**If you want the images**: the screenshots and GIFs under `docs/images/` live
in **Git LFS** - docs only, nothing in the build needs them. Install it
*before* cloning, or they arrive as ~130-byte pointer text:

```bash
# macOS: brew install git-lfs
# Linux: sudo apt install git-lfs
git lfs install             # one-time, per user
git lfs pull                # already cloned without it? fetch the real bytes
```

It runs in a browser too. With `emcc` on your `PATH`:

```bash
make web                   # Emscripten build - gl4es → WebGL2
make web-serve             # serve it at http://localhost:8000/
```

<sub>Details and the emsdk setup: [packaging/web/README.md](../packaging/web/README.md).</sub>

Press **F1** in-app for the full command and key reference, and read the
[**User Guide**](../docs/USER_GUIDE.md) for the rest - it is the manual, and the
best place to go after this page. There are guided tutorials under the
**Tutorials** menu too, and guided tours of the app itself under **Tours**.

### The 30-second demo

Type these. Watch each line land as you commit it.

```c
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glRotatef(t * 30, 0, 1, 0);
glColor3f(0.95, 0.44, 0.66);
glutSolidTorus(0.3, 0.9, 24, 48);
```

Now press `Ctrl+T`. The torus spins. That's the whole loop - the rotation
rate is just the scalar in front of `t`, and every line stays editable.

Anything you build here exports to a standalone C89 program (`Ctrl+S`, or
`--export-c` headlessly) that compiles and runs on its own against GL/GLUT -
no REPL, no runtime of ours.

<br>

---

### Keys

| | | | |
| :--- | :--- | :--- | :--- |
| `;` | commit | `Ctrl+T` | toggle time |
| `Tab` | autocomplete | `Ctrl+R` | replay |
| `↑ ↓` | navigate | `Ctrl+S` | save |
| `Ctrl+Z` | undo | `F1` | help |
| `Ctrl+F` | find | `F12` | next example |

<sub>On macOS, `Cmd`+letter works as `Ctrl`+letter. Full reference: [User Guide → Keyboard & Mouse](../docs/USER_GUIDE.md#keyboard-mouse).</sub>

<br>

---

### Design goals

- **Launch pad.** Make it easy to get something going quickly.
- **Independence.** Export/import is a first-class citizen - take what you
  build into your own engine or tool.
- **Immediate mode.** The joy is the locality: the geometry is in the code,
  not hidden behind a data file.
- **Limited state.** Animation is a pure function of time; particles come
  from a deterministic `rand`.
- **No textures, just geometry and color.** The expressiveness comes from
  composition

<br>

---

### Documentation

| | |
|---|---|
| [**User Guide**](../docs/USER_GUIDE.md) | The manual - every feature, with screenshots. |
| [**Showcase**](../docs/SHOWCASE.md) | The built-in scenes and the source that draws them. |
| [**Advanced Usage**](../docs/ADVANCED_USAGE.md) | CLI flags, env vars, headless rendering, recording GIFs, mesh export, music. |
| [**Contributing**](../docs/CONTRIBUTING.md) | Build, test, the guard suite, and how to extend the REPL. |
| [**Modules**](../docs/MODULES.md) | One-page map of the source tree and its ownership rules. |
| [**Architecture**](../docs/ARCHITECTURE.md) | The deep dive - command model, frame pipeline, boundaries. |

<br>

---

<div align="center">

<sub>C99 · OpenGL 1.1 · GLU · freeglut · [MIT](../LICENSE)</sub>

<sub>Bundles [freeglut and miniaudio](../docs/THIRD_PARTY_LICENSES.md). Background music: drop `.mp3`s in `./assets` or fetch the optional [music pack](../docs/ADVANCED_USAGE.md#music--assets).</sub>

</div>
