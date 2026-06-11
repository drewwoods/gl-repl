# Advanced Usage

The power-user reference: command-line flags, environment variables,
headless rendering, recording, mesh export, scene-file headers, music,
and diagnostics. For day-to-day features (editing, the language, the
panels), see the [User Guide](USER_GUIDE.md).

## Synopsis

```
gl-repl [file.c | workspace/] [--example name|n] [--time secs]
        [--export-ply out.ply [--export-ply-srgb]] [--noaccum]
        [--assets dir] [--no-audio] [--dump-code] [--detailed-prof]
        [--list-examples]
```

## Options

| Option | Effect |
|---|---|
| *file*.c | Reload a previously saved session from a single file. |
| *workspace*/ | Load every `*.c` under the directory as a separate scene. |
| `--example` *name*\|*n* | Start on a built-in example (case-insensitive name, or 0-based index). |
| `--list-examples` | Print the built-in examples and exit. |
| `--time` *secs* | Initial value of the animation variable `t` (applied after any `--example` load). |
| `--export-ply` *out*.ply | Capture the scene geometry to an ASCII PLY mesh on frame 1, then exit. |
| `--export-ply-srgb` | With the above, decode vertex colors sRGB → linear for color-managed viewers. |
| `--noaccum` | Disable the accumulation buffer (anti-aliasing + motion blur). |
| `--assets` *dir* | Scan *dir* for `*.mp3` instead of `./assets`. Beats `GLR_ASSETS_DIR`. |
| `--no-audio` | Skip audio initialization entirely (also isolates startup stalls). |
| `--dump-code` | Print the loaded buffer to stdout. |
| `--detailed-prof` | Add fine-grained init-trace phases (see [Diagnostics](#diagnostics)). |

## Environment variables

| Variable | Meaning |
|---|---|
| `GLR_ASSETS_DIR` | Music directory; the `--assets` flag overrides it. |
| `GLR_TIME` | Initial animation time `t` in seconds; `--time` wins. |
| `GLR_EDIT_LINE` | Park the cursor on source line *n* (0-based, clamped) after load — makes cursor-bound overlays render headlessly. |
| `GLR_ACCUM_PASSES` | Accumulation AA sample count (1/2/4/8/12/16); used by the capture pipeline. |
| `GLR_NO_POINT_PARAMETER` | Force the no-`glPointParameterfv` fallback path on capable hardware (keeps the fallback testable). |
| `GLR_AUDIO_HITCH_MS` | Audio-worker hitch-report threshold in ms (default 50; `0` disables). |
| `GLR_DETAILED_PROF` | Same as `--detailed-prof`, via env. |
| `FREEGLUT_CAPTURE_FILE` | *(OSMesa builds)* Filename prefix for `SIGUSR1` screenshots (default `freeglut`). |
| `FREEGLUT_CAPTURE_FRAMES` | *(OSMesa builds)* Record mode: capture N frames as numbered PPMs, then exit. |
| `USE_GL_STUBS=1` | *(build-time)* Compile against the bundled no-op GL headers — no system GL needed (non-rendering tests only). |

## Headless rendering (OSMesa)

For CI or any machine with no display, `make ... FREEGLUT_OSMESA=1` builds
gl-repl against a software, off-screen **OSMesa** backend — it renders into
memory with no window. The real-GL tests and `--export-ply` then run headless
(`make gl-tests FREEGLUT_OSMESA=1`), and you can grab a **screenshot of a
running headless process** by sending it `SIGUSR1`:

```bash
brew install mesa mesa-glu                       # macOS deps (Linux: apt-get install libosmesa6-dev)
make gl-repl FREEGLUT_OSMESA=1
FREEGLUT_CAPTURE_FILE=/tmp/shot ./build/release-osmesa/gl-repl --example 8 --no-audio &
kill -USR1 $!                                    # writes /tmp/shot-0000.ppm
magick /tmp/shot-0000.ppm shot.png               # PPM -> PNG to view
```

**Starting the animation later.** Animation plays by default, with `t`
advancing a fixed `1/60 s` per rendered frame from `0`. To capture from a
later point in the timeline, set the initial `t` with `--time <secs>` (or
`GLR_TIME`):

```bash
./build/release-osmesa/gl-repl --example 2 --time 5 --no-audio &
```

**Posing the cursor.** Cursor-bound overlays (transform guides, vertex
labels) need the cursor parked on the relevant line — `GLR_EDIT_LINE=<n>`
does that at startup, as if you had arrowed to source line *n*:

```bash
GLR_EDIT_LINE=4 ./build/release-osmesa/gl-repl scene.c --no-audio &
```

### Recording GIFs / MP4s

`FREEGLUT_CAPTURE_FRAMES=N` is the backend's record mode: it captures every
rendered frame to a numbered PPM and exits after N frames.
`scripts/record-gif.sh` wraps that and assembles the output via `ffmpeg`:

```bash
make gl-repl FREEGLUT_OSMESA=1
scripts/record-gif.sh --example 2 --duration 3 --out ring        # ring.gif + ring.mp4
scripts/record-gif.sh --example 8 --duration 4 --fps 30 --scale 600 --time 5 --out torus
```

`--duration <secs>` × `--fps` sets the frame count; `--scale <w>` downsizes;
`--time <t0>` starts later in the animation. The clock advances `1/60 s` per
frame, so playback is `~fps/60`× natural speed — use `--fps 60` for
real-time. Needs `ffmpeg`. (`scripts/record-gif.sh --help` for all flags.)

The screenshots and GIFs in the README and User Guide are themselves
generated headlessly — `scripts/docs-assets.sh` regenerates everything under
`docs/images/`, rendering scene shots at 2x via `--window 2400x1600` and
downscaling for 4x supersampling (the software rasterizer has no MSAA).

### Re-vendoring the OSMesa freeglut

The OSMesa backend needs a vendored freeglut that carries it. It lives in
the freeglut fork at <https://github.com/drewwoods/freeglut> (branch
`osmesa-backend`), which adds the OSMesa platform plus the `SIGUSR1`
screenshot and `FREEGLUT_CAPTURE_FRAMES` record-mode capture used above:

```bash
FREEGLUT_REPO=https://github.com/drewwoods/freeglut \
  scripts/vendor-freeglut.sh osmesa-backend
make freeglut-clean
```

The resolved SHA is pinned in `third_party/freeglut/VENDORED.txt`. See
*Headless Rendering & Screenshots (OSMesa)* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for the full design, and
`plans/external/freeglut-osmesa-backend.md` for the backend spec.

## Mesh export (PLY)

Press **F11** (or **File → Export .ply**) to capture the current scene as an
ASCII PLY mesh, named after the active scene (like Save Scene). The geometry
— your `glVertex` polygons, GLU-tessellated shapes, and the GLUT solids
(teapot/sphere/cube/cone/torus) — is captured through a single
`glRenderMode(GL_FEEDBACK)` pass, so everything on screen exports the same
way. Authored per-vertex normals are preserved; the rest are smoothly
synthesized.

Headless / scripted capture:

```bash
./gl-repl --example 8 --export-ply out.ply                     # capture on frame 1, then exit
./gl-repl --example 8 --export-ply out.ply --export-ply-srgb   # decode colors sRGB -> linear
```

Line primitives (`glBegin(GL_LINES/LINE_STRIP/LINE_LOOP)`) export as a PLY
`edge` element. See *Mesh Export (PLY via GL_FEEDBACK)* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for the capture/encode design.

## Scene-file headers

Saved scenes are standalone C files, but their leading comments carry REPL
metadata that round-trips on reload:

| Directive | Meaning |
|---|---|
| `// @scene-name <name>` | Names the scene slot the file loads into. |
| `// @workspace-dir <path>` | Re-binds the workspace directory. |
| `// @var name=value` | Restores a predefined variable's value. |
| `// @cfg <slug> = <value>` | Applies a scene-presentation setting (`wireframe`, `grid`, `axes`, `view_mode`, `backdrop`, …). |
| `// @declare name` | Reconstructs a `float name;` declaration on import. |
| `// @tune` | Marks a variable as a tunable knob in the exported program — see [User Guide → Tunable Variables](USER_GUIDE.md#tunable-variables--tune). |
| `// camera` block | A 5-line camera preset applied on load. |

Built-in examples use the same `@cfg` + `// camera` headers, so a saved
scene and an example are the same file format. Only *leading* directives
are metadata; the same text later in the file is an ordinary comment.

## Music & assets

gl-repl plays background music: any `*.mp3` it finds at startup, in filename
order, from three combined sources:

1. **`./assets`** next to where you run it — overridden by `--assets <dir>`
   or `GLR_ASSETS_DIR` (flag beats env).
2. **Bundled with the app** — the macOS `gl-repl.app` (from `make app`)
   ships a sample track inside the bundle.
3. **Your music folder** — `~/Library/Application Support/gl-repl/Music` on
   macOS, `$XDG_DATA_HOME/gl-repl/music` on Linux. Created on first run;
   drop `.mp3`s there and they join the playlist.

The repository ships only one sample track to stay lightweight. The optional
**music pack** is attached to the GitHub release tagged `assets-v1` — fetch
it into your per-user music folder with:

```bash
scripts/fetch-music.sh             # downloads the pack into the user music folder
scripts/fetch-music.sh --dir ./assets   # or anywhere else
```

`./gl-repl --no-audio` starts silent. See *Music Asset Resolution* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for precedence details.

## Diagnostics

Two always-on stderr diagnostics help locate startup stalls and audio
hitches:

- **Init trace** — `main()` logs a wall-clock line per startup phase
  (`[init +N.NNNs] <phase>`). A large gap names the slow phase;
  `--no-audio` isolates whether opening the OS audio device is the cause.
  `--detailed-prof` / `GLR_DETAILED_PROF=1` adds finer phases, including
  per-frame timing triples for the first two frames.
- **Audio-worker hitch detector** — any blocking audio lifecycle op (track
  load, stream teardown, advance) over the threshold logs
  `repl_audio: worker hitch: <op> took N ms`. Tune with
  `GLR_AUDIO_HITCH_MS` (default 50; `0` disables).

In-app, the CPU profiler overlay shows per-frame section timings and the
memory panel shows RSS history — see
[User Guide → Profiling & Diagnostics](USER_GUIDE.md#profiling--diagnostics).

## Files

```
output.c        default save target (a standalone, compilable C file)
<workspace>/    a directory of *.c scenes, round-tripped via @-headers
audio_state.ini persisted audio state (track, position, volume)
~/Library/Application Support/gl-repl/Music    per-user music folder (macOS)
$XDG_DATA_HOME/gl-repl/music                   per-user music folder (Linux)
```

---

<sub>See also: [User Guide](USER_GUIDE.md) · [Contributing](CONTRIBUTING.md) · [Architecture](ARCHITECTURE.md)</sub>
