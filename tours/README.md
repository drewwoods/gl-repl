# Guided-tour scripts

This directory contains the guided tours shown in gl-repl's **Tours** menu.
A tour is a `.pointer` script: it drives the same mouse and keyboard handlers
as a person, and renders a cursor, click ripples, spotlight rings, and caption
text while it runs. A menu-started tour is a **controlled tour**: it shows a
transport HUD at the top of the scene and responds to replay-style controls
(see below). Clicking the HUD expands/collapses its transport details. A mouse
click outside the HUD, wheel event, or any unrecognized key stops the tour and
returns control to the user; the transport keys instead drive playback.
Starting a tour also stops any active REPL replay before the tour captures its
rewind baseline, so the tour always begins from the full scene.

Tours are compiled into the application from [`catalog.ini`](catalog.ini) for
native builds or [`catalog-emscripten.ini`](catalog-emscripten.ini) for the web
build. The same scripts can drive an offline recording through
[`scripts/record-video.sh`](../scripts/record-video.sh).

## Add a tour

1. Create a top-level `.pointer` file here, using the untimed,
   completion-driven form described below.
2. Add a section to [`catalog.ini`](catalog.ini). If the tour also works in the
   browser, add it to [`catalog-emscripten.ini`](catalog-emscripten.ini), using
   a web-specific script when the two platforms need different targets.
   Section order is the order in the Tours menu.
3. Validate the catalog:

   ```sh
   make check-tours-catalog
   make WEB=1 check-tours-catalog
   ```

4. Run `make test-stubs` to exercise every compiled-in tour through the
   parser. To preview an interaction as a video, build the app and run, for
   example:

   ```sh
   make gl-repl
   scripts/record-video.sh --script tours/menus-and-examples.pointer \
       --duration 30 --out /tmp/menus-and-examples
   ```

Use symbolic targets in built-in tours. They resolve against the live layout
when an event fires, so the tour works at every window size and continues to
work when menu positions change.

## Transport controls

While a menu-started tour runs, a small strip at the top of the scene defaults
to `Tour | <name> | [+]`, leaving the authored narration and scene unobstructed.
Click anywhere on that strip to expand the playback state, speed, current step
(`Step n / total`), active `.pointer` source line, progress, and keyboard
controls. Click anywhere on the expanded HUD to collapse it again. The HUD
choice survives Back reconstruction; every newly started tour begins compact.

One executable event line is one step; comments and blank lines are not steps.
These keys drive the tour instead of stopping it:

| Key | Action |
| --- | --- |
| `Space` | Pause / resume. From the end (Done), restart from the beginning. |
| `→` | Execute one event immediately, then stay paused. |
| `←` | Backstep: return to the previous step boundary (before the in-flight event, or one completed event back), then stay paused. |
| `+` / `=` / `-` | Change speed along `0.25×, 0.5×, 1×, 2×, 4×, 8×, 16×`. |
| `Esc` | Exit the tour, keeping whatever it has done so far. |

Backstep works by restoring one whole-app baseline captured when the tour
started, then fast-replaying the events up to the target boundary — there is no
per-event snapshot. Reversible authored actions reconstruct faithfully: discrete
edits, config toggles, menu navigation, and scene changes land in the same
state. Camera and slider **drags** reconstruct their end *position* (the glide's
smoothstep samples are dispatched synchronously), but not their dynamics — the
fast prefix replay skips the per-frame camera tick, so any leftover orbit
momentum and its subsequent settling can differ from the live run. Scene-camera
presets are deterministic during Back: their normally eased camera target is
applied immediately while the prefix reconstructs, so the pre-tour baseline
angle does not flash or ease through the target again. **Irreversible
side effects cannot be undone by backstep:** filesystem writes, process exit,
and audio-position changes. Keep rewindable tours free of those actions.
Speed affects only pointer-script timing, never animation `t`, camera easing,
REPL replay, status message lifetimes, or audio.

A tour that reaches its last event enters a persistent **Done** state (it does
not auto-stop): the HUD stays up so you can backstep, restart with `Space`, or
exit with `Esc`. Environment-driven recording scripts
(`GLR_POINTER_SCRIPT=`) get no HUD or transport and are never canceled by input.

## File structure and timing

Each nonblank, non-comment line is one event. A line beginning with `#` is a
comment. Inline comments work after most events, but **not** after `key` or
`echo`: everything after those verbs' fixed arguments is payload text.

There are two mutually exclusive timing styles:

- **Untimed** (`verb ...`) is the required style for Tours-menu content. The
  next event begins only after the current event has completed. This makes a
  tour robust to frame-rate variation and menu-opening delays.
- **Timestamped** (`seconds verb ...`) is intended for offline recordings.
  Seconds are absolute on a 60 Hz rendered-frame clock and must be
  nondecreasing. Events can overlap unless a `pause` blocks dispatch.

Do not mix the two forms in a file. `pause` works in both forms. In an untimed
script, glides, clicks, paced typing, rings, and pauses each finish before the
next step starts; immediate actions advance on the next rendered frame. An
`echo` is deliberately non-blocking: its seconds parameter is its on-screen
lifetime, and following events run while the caption remains visible. Put a
`pause` after it when the caption needs an exclusive reading beat.

```text
# Completion-driven tour
glide menu:file 0.6
click
glide item:new_scene 0.4
click
echo scene:0.25,0.76 18 2.5 A caption can label the next action.
glide scene:0.55,0.30 0.8
pause 0.5

# Absolute-time recording script -- use this style for every event in its file
0.0 move scene:0.5,0.5
0.4 glide menu:scene 0.6
1.2 click
```

## Points and symbolic targets

An event point is either literal window pixels, `x y` (origin at the
top-left), or one of these symbolic tokens:

| Target | Resolves to |
| --- | --- |
| `menu:<label>` | A top-level menu button, such as `menu:scene`. |
| `item:<label>` | A row in the currently open dropdown, such as `item:new_scene`. |
| `subenter:<parent>` | The safe horizontal entry point into a dropdown row's flyout. |
| `sub:<parent>:<label>` | A row in that parent's flyout. |
| `pin:<label>` | A pinned menu-bar control: `search`, `view`, or `replay`. |
| `scene:<x>,<y>` | A fraction of the live scene viewport, measured from its top-left. For example, `scene:0.5,0.5` is its center. |
| `shell:<label>` | An Emscripten browser-shell control outside the canvas. Currently `shell:new` targets the top **New** button. |

Labels use a case-insensitive normalized prefix match. In target labels, `_`
matches a space, so `sub:3d:torus_knot` can match **Torus knot (animated)**.
Use enough of the label to be unambiguous. Chrome rows such as dividers and
headers cannot match.

`shell:` targets are web-only: the Emscripten build resolves them against the
live browser DOM rather than the GLUT canvas. Name the target on both the move
and the click so the click activates the shell control instead of being routed
through GLUT at the current pointer position:

```text
glide shell:new 0.8
click shell:new
glide scene:0.55,0.30 0.6
```

Native builds cannot resolve `shell:` targets, so keep them in script variants
selected only by `catalog-emscripten.ini`.

Row targets require their parent dropdown to be open. For a hover-opened
flyout, use this sequence rather than gliding diagonally into it:

```text
glide menu:scene 0.6
click
glide item:3d 0.4
glide subenter:3d 0.3
glide sub:3d:torus_knot 0.5
click
```

`subenter:` crosses the flyout boundary on the parent row's y-coordinate. A
diagonal path can cross another parent row and switch the flyout before the
pointer reaches its intended target.

If a symbolic target cannot resolve, a built-in tour stops with a status
message. An environment-driven recording fails instead, rather than capturing
an unintended interaction.

## Event reference

| Event | Syntax | Effect |
| --- | --- | --- |
| Move | `move <point>` | Moves the pointer immediately. |
| Glide | `glide <point> <seconds>` | Smoothly moves the pointer over a positive duration. |
| Left click | `click [point]` | Optionally moves, then presses and releases the left button. |
| Right click | `rightclick [point]` | Optionally moves, then presses and releases the right button. |
| Drag press | `down [point]` | Optionally moves, then holds the left button. Subsequent moves and glides use the normal drag path. |
| Drag release | `up [point]` | Optionally moves, then releases the left button. |
| Wheel | `wheel <nonzero-integer>` | Sends a wheel event at the current pointer. `1` and `-1` are the usual directions. |
| Text | `key <text>` | Sends every character through the normal keyboard handler immediately. |
| Paced text | `key@<chars-per-second> <text>` | Types the payload on the frame clock, one character at a time. The next untimed step waits for it. |
| Special key | `skey <name>` | Sends a special GLUT key. See the names below. |
| Modified key | `chord <mods> <key>` | Sends one key press with a declared modifier mask, reaching shortcuts `key`/`skey` cannot (Shift is not carried in a plain key byte). See the rules below. |
| Spotlight | `ring <point> <seconds>` | Shows a pulsing highlight ring for a positive duration. |
| Caption | `echo <point> <height-px> <seconds> <text>` | Shows text for the positive on-screen duration without delaying the next event. A later caption replaces it. The requested height chooses the nearest fixed bitmap font. |
| Wait | `pause <seconds>` | Delays later events for a positive duration. |

To show a caption without concurrent actions, follow it with a matching (or
longer) pause:

```text
echo scene:0.25,0.76 18 2.5 Take a moment to notice the grid.
pause 2.5
```

`click`, `rightclick`, `down`, and `up` use the current pointer when their
point is omitted. A synthetic click releases roughly 0.1 seconds after its
press. While `down` is active, the engine sends motion rather than passive
motion, so camera and slider drags behave like real ones.

`skey` accepts `f1` through `f12`, `up`, `down`, `left`, `right`, `home`,
`end`, `pageup`, and `pagedown` (case-insensitive).

### Modified key chords

`chord <mods> <key>` presses a single key with modifiers held, so a tour can
trigger shortcuts that key off Shift — which `key`/`skey` cannot reach, because
a plain key byte carries no Shift bit.

- `<mods>` is a `+`-joined subset of `ctrl`, `shift`, and `alt` — order-free and
  case-insensitive (`ctrl+shift`, `shift`, `alt+ctrl`).
- `<key>` is either a special-key name from the `skey` list above, or a single
  printable character. For a printable character, `ctrl` folds it to its control
  byte exactly as `\cX` does, so `chord ctrl+shift c` sends the same byte as
  Ctrl+C plus a held Shift — i.e. Ctrl+Shift+C.

```text
chord ctrl+shift c    # Ctrl+Shift+C  (reset camera)
chord shift f12       # Shift+F12      (previous example)
chord shift left      # Shift+Left     (extend selection)
```

A line is rejected at load (failing a recording, stopping a tour) when: the
`<key>` is a single printable char but no `ctrl` is present (a shift-only glyph
has no shortcut meaning — type it with `key`); a modifier name is unknown,
empty, or repeated (`ctrl++shift`, `ctrl+ctrl`); or extra tokens follow the key
(only a trailing `#` comment is allowed).

### Keyboard text and escapes

The text after `key` or `key@...` is literal script payload. It may contain
spaces and punctuation. Use these escapes where a control character is
needed:

| Escape | Sends |
| --- | --- |
| `\n` | Enter (carriage return in the input handler) |
| `\e` | Escape |
| `\t` | Tab |
| `\cX` | Ctrl+X; for example, `\cT` toggles animation |
| `\\` | A backslash |

For example, a tour can enter and commit a command like this:

```text
key@20 glRotatef(t * 40, 0, 1, 0)
key ;
```

The parser currently allows up to 256 events per script and up to 127 bytes
of text or caption payload per event. Keep captions short enough to fit the
scene area at the tour's smallest supported window size.

## Catalog format

`catalog.ini` and `catalog-emscripten.ini` are INI files with the same shape.
The Makefile selects the latter when `WEB=1`. Each section has a stable
identifier and exactly these two keys:

```ini
[camera-and-views]
file = camera-and-views.pointer
name = Camera & Views
```

- The section identifier must start with a letter or number; after that it may
  contain letters, numbers, `.`, `_`, and `-`.
- `file` must name a unique `.pointer` file inside `tours/`.
- `name` is the case-insensitively unique, user-visible Tours-menu label.
- Section order determines menu order.

The native and web catalogs may use the same section identifier and display
name while pointing at different `.pointer` files. This lets a tour preserve
the same menu entry while substituting browser-shell targets or examples that
exist in the web catalog.

[`scripts/gen_tours.py`](../scripts/gen_tours.py) embeds the catalog's scripts
into the built binary. Do not edit the generated include in `build/`.

## Recording-only music directives

For `scripts/record-video.sh`, a script may include these header comments:

```text
# music: assets/sample.mp3
# music-seek: 12.5
```

They select recording audio only; they do not affect an in-app tour. Explicit
`record-video.sh --music` and `--music-seek` options take precedence.
