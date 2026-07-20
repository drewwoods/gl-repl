# Guided-tour scripts

This directory contains the guided tours shown in gl-repl's **Tours** menu.
A tour is a `.pointer` script: it drives the same mouse and keyboard handlers
as a person, and renders a cursor, click ripples, spotlight rings, and caption
text while it runs. A real key press, click, or wheel event stops a running
tour and returns control to the user.

Tours are compiled into the application from [`catalog.ini`](catalog.ini). The
same scripts can drive an offline recording through
[`scripts/record-video.sh`](../scripts/record-video.sh).

## Add a tour

1. Create a top-level `.pointer` file here, using the untimed,
   completion-driven form described below.
2. Add a section to [`catalog.ini`](catalog.ini). Section order is the order
   in the Tours menu.
3. Validate the catalog:

   ```sh
   make check-tours-catalog
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
script, glides, clicks, paced typing, rings, captions, and pauses each finish
before the next step starts; immediate actions advance on the next rendered
frame.

```text
# Completion-driven tour
glide menu:file 0.6
click
glide item:new_scene 0.4
click
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

Labels use a case-insensitive normalized prefix match. In target labels, `_`
matches a space, so `sub:3d:torus_knot` can match **Torus knot (animated)**.
Use enough of the label to be unambiguous. Chrome rows such as dividers and
headers cannot match.

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
| Spotlight | `ring <point> <seconds>` | Shows a pulsing highlight ring for a positive duration. |
| Caption | `echo <point> <height-px> <seconds> <text>` | Shows text at a point for a positive duration. The requested height chooses the nearest fixed bitmap font. |
| Wait | `pause <seconds>` | Delays later events for a positive duration. |

`click`, `rightclick`, `down`, and `up` use the current pointer when their
point is omitted. A synthetic click releases roughly 0.1 seconds after its
press. While `down` is active, the engine sends motion rather than passive
motion, so camera and slider drags behave like real ones.

`skey` accepts `f1` through `f12`, `up`, `down`, `left`, `right`, `home`,
`end`, `pageup`, and `pagedown` (case-insensitive).

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

`catalog.ini` is an INI file. Each section has a stable identifier and exactly
these two keys:

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
