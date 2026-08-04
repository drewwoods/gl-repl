# Tagged camera header + one shared reader

## Goal

Make a scene's `// camera` header mean the same thing no matter which loader
reads it, and make a malformed header a reportable error instead of geometry
that silently offsets the scene.

The finished behavior is:

- one camera-header reader, in `src/repl/`, called by both the file importer
  and the example/catalog loader;
- each camera line self-identifies by role tag, so recognition never depends on
  arrival order, line position, or a parser state variable;
- a line the reader rejects produces a diagnostic naming the line and the rule
  it broke, and never falls through into the document as geometry;
- the `g_angle` save variant and the numeric display variant stop being two
  dialects - they differ only in which line carries the `ry` tag;
- the camera bridge stops parsing text: it receives a resolved pose plus an
  explicit snap-or-ease intent;
- direct file load and catalog load of the same `.glr` produce identical camera
  state and identical document rows, enforced by a differential test.

Nothing here is released, and the `.glr` corpus is edited in place. There is no
back-compatible reading of the old positional format: the legacy reader is
deleted, not kept as a fallback.

## The bug this comes from

`stress/matrix-stack-recursion-stress.glr` carries a hand-authored header whose
first transform folds a y offset into the distance line and whose yaw is
`20.0f * t`:

```c
// --- Camera -------------------------
glTranslatef(0.0f, -2.5f, -10.00f);
glRotatef(15.0f, 1.0f, 0.0f, 0.0f);
glRotatef(20.0f * t, 0.0f, 1.0f, 0.0f);
```

Loaded as a file it looks like it works; loaded from `stress/catalog.ini` the
three transforms land in the document and the tree renders 2.5 units low,
spinning. The two loaders disagree because they were written to opposite
contracts.

**File load** - `src/repl/import.c` delegates to the bridge state machine
`cam_try_consume_block_line` (`src/app/glr_camera_export.c:117`):

- marker-independent: `import_handle_camera` (`import.c:2168`) offers *every*
  line at `func_depth == 0` to the consumer, marker or no marker;
- greedy and per-line: a matching line applies immediately, a non-matching line
  parks the state and falls through to geometry;
- lossy: state 0 keeps only `-v[2]`, so the `-2.5f` is discarded in silence.

Result: distance and `rx` apply, the `20.0f * t` line stays as geometry, parse
state stops at 2, and `cam_adopt_import_scene_default` declines to record a
scene default.

**Catalog load** - `try_apply_example_camera_header`
(`src/repl/example_loader.c:125`):

- marker-anchored and positional (`lines[1..4]` after the marker);
- all-or-nothing by design (`example_loader.c:381`), so a malformed header does
  not swallow the geometry that follows it;
- stricter: distance line must have x ≈ y ≈ 0 (`example_loader.c:143`), angles
  must be literal floats, and `example_cam_finish_call` requires the line to end
  after its arguments.

That header fails three independent checks, so the whole block is rejected and
every line becomes geometry.

The marker itself is not the difference: `repl_comment_alpha_payload_equals`
ignores punctuation and case, so `// --- Camera ------` reads as `camera` on
both paths.

### The same split, reached from export

`emit_export_cam_lines` (`src/repl/export_setup.c:743`) writes the save variant,
whose yaw line is the literal `glRotatef(g_angle, 0.0f, 1.0f, 0.0f)` with the
value carried by a file-scope `static float g_angle = N.NNNNf;`
(`export_setup.c:727`). The `// camera` marker is emitted only when
`g_camera_comment_line` is non-empty, i.e. only when the document already
carried an authored marker.

The importer handles that fine - `cam_try_parse_angle_preamble` reads the
preamble, and the marker is optional. The example loader cannot read it at all:
it has no preamble reader, and `example_cam_read_floats` fails on the `g_angle`
token. So an exported `.c` used as a catalog entry loses its camera the same way
the stress file does. The shipped examples avoid this only because they are
hand-authored in the numeric display variant.

## Design decisions

### Role tags on the camera lines

Each camera line carries a trailing tag naming its role. The transform lines
remain the source of truth - a hand edit to a number in an exported `.c` must
survive re-import - and the tag exists solely to answer "camera or geometry?"
without reference to position.

```c
static float g_angle = 20.0000f;                 // @camera ry
...
void display(void) {
  glLoadIdentity();
  // camera
  glTranslatef(0.0000f, 0.0000f, -10.0000f);     // @camera dist
  glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);         // @camera rx
  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);          // @camera ry
  glTranslatef(-0.0000f, 2.5000f, -0.0000f);     // @camera pan
```

Four roles: `dist`, `rx`, `ry`, `pan`. Consequences:

- **No state machine.** Each line parses independently. A missing line does not
  desync the ones after it; an extra untagged transform is geometry, full stop.
- **The preamble special case collapses.** `static float g_angle` carrying
  `// @camera ry` gives yaw exactly one tagged source in the file, so the
  file-scope reader that only the importer had disappears.
- **Per-line diagnostics.** `glTranslatef(0, -2.5, -10) // @camera dist` becomes
  a specific error - *dist line must have x = y = 0; put the offset on the pan
  line* - rather than a silent drop or a wholesale reinterpretation.
- **Both variants are one dialect.** Numeric-`ry` and `g_angle`-`ry` differ only
  in which line the `ry` tag sits on.

A one-line summary directive (`// @camera dist=10 rx=15 ry=20 pan=…`) was
considered and **rejected**: it makes the directive and the transforms two
sources of truth, and whichever loses means a hand edit to an exported `.c` is
silently discarded.

The `// camera` marker comment stays, but as *presentation only* - a section
heading for the expanded code panel, stashed into
`ReplImportExportState::camera_comment_line`. It is no longer syntax, so nothing
gates on its presence and the two functions that currently decide whether a
comment is "the camera marker" collapse to one.

### One reader, in `src/repl/`

A new `src/repl/camera_header.{c,h}` owns tag recognition, per-role parsing, and
validation. Both callers use it:

- `import.c` offers lines to it while `func_depth == 0`;
- `example_loader.c` offers the header region to it after the `@cfg` block.

Deleted: `example_cam_parse_translate` / `_parse_rotate` / `_read_floats` /
`_finish_call` / `_skip_sep` / `_skip_ws` in `example_loader.c`, and
`cam_try_consume_block_line` / `cam_try_parse_angle_preamble` /
`cam_line_read_floats` / `cam_line_skip_sep` / `g_cam_parse_state` in
`glr_camera_export.c`.

Rules the shared reader owns, so they cannot diverge again:

| Rule | Today |
|---|---|
| Tagged line at `func_depth > 0` | not expressible; importer gates, example loader never sees one |
| `dist` line must have x ≈ y ≈ 0 | example loader only (`example_loader.c:143`) |
| Rotate axis must match the role | example loader only |
| Line must end after its arguments | example loader only (`example_cam_finish_call`) |
| Non-literal argument (`20.0f * t`) | rejected on both, silently |
| Duplicate tag in one file | undefined |
| Missing role | importer half-applies; example loader rejects the block |

Decisions: a tagged line inside a `funcN` body is an error; a duplicate tag is
an error; a missing role keeps the current camera value and warns; every
rejection names the file line and the rule.

Atomicity stops being a policy question. The example loader's all-or-nothing
rule existed to stop a desynced parser from eating geometry; with independent
tagged lines there is nothing to desync, so both paths apply per line and
collect diagnostics.

### The bridge stops parsing text

With parsing in `src/repl/`, `ReplExportCameraBridge` returns to being a camera
adapter. `try_consume_import_line`, `reset_import`, `adopt_scene_default`, and
`apply_example_block` collapse into one entry point taking a resolved pose and
an explicit intent:

```c
void (*apply_pose)(const ReplCameraPose *pose, ReplCameraApplyMode mode);
```

`REPL_CAMERA_APPLY_SNAP` for file load, `REPL_CAMERA_APPLY_EASE` for example
load. That difference is real and wanted - examples animate the transition
(`glr_camera_ease_to`, `glr_camera_export.c:255`) while a file load should land
immediately - but today it is a side effect of *which parser ran*. Making it an
argument is what keeps it from drifting again. Scene-default adoption moves into
`apply_pose`, unconditional, since a rejected header now never produces a
partial pose.

The formatting half of the bridge (`fill_save_block`, `fill_display_block`,
`fill_save_preamble`) is unchanged apart from appending the tags.

### Corpus

44 of 46 `.glr` files under `examples/scenes/` and `stress/` carry a camera
header. The sweep is mechanical for the 43 already in canonical numeric form -
append four tags. `stress/matrix-stack-recursion-stress.glr` is the one real
edit: its y offset moves to a `pan` line, and its `20.0f * t` yaw is not
expressible as a camera (the only non-literal the format accepts is the
`g_angle` token, which the REPL does not animate) - the spin belongs in the
scene body.

## Implementation phases

1. **Differential test first.** `tests/test_camera_header_parity.c`: for every
   `.glr` in `examples/scenes/` and `stress/`, load via the catalog and as a
   file, assert identical camera pose and identical document row count. Expected
   red on `matrix-stack-recursion-stress.glr` before any other change - that is
   the check that the diagnosis is right.
2. **`src/repl/camera_header.{c,h}`** - tag recognition, per-role parse,
   validation, diagnostics. Unit tests against line fixtures, no loader
   involvement.
3. **Bridge collapse** - `ReplCameraPose` + `apply_pose(mode)`; delete the four
   line-oriented bridge entry points and the state machine behind them.
4. **Wire both loaders** to the shared reader; delete the example loader's
   private parsers and the positional block reader.
5. **Export the tags** - `cam_format_block_impl` and `cam_format_save_preamble`
   append them; the `// camera` marker becomes unconditional so an exported file
   always reads back with its heading.
6. **Corpus sweep** - all 44 headers, plus the real edit to the stress file.
7. **Goldens** - `--dump-code` output and any export fixtures gain the tag
   comments. Regenerate with `BUILD=debug` (see the golden-regen note in
   `docs/CONTRIBUTING.md`).

Phases 2-4 land together; a half-migrated tree has two readers, which is the
state this plan exists to end.

## Tests

- `test_camera_header_parity` (new) - the catalog-vs-file differential over the
  whole corpus. This is the regression lock.
- `test_camera_header` (new) - per-role parse and every rejection rule:
  non-literal argument, `dist` with a non-zero x/y, wrong rotate axis, trailing
  garbage after the call, duplicate tag, tagged line inside a `funcN` body,
  missing role.
- Existing export/import round-trip tests - extended so a file whose transform
  numbers are edited by hand between export and import reads back the edited
  pose, which is the property the rejected one-line-directive design would have
  broken.
- `make check-state-ownership` - the reader lives in `src/repl/` and must not
  reach for `glr_camera_*` directly.

## Risks

- **Phases 2-4 are one commit.** Splitting them leaves two live readers, and the
  intermediate state is exactly the bug.
- **Golden churn** is broad but mechanical; it lands with phase 7 rather than
  being spread across the sweep.
- **The `20.0f * t` case is a behavior change, not a port.** That header never
  worked as a camera on either path; the fix is to move the rotation into the
  scene body, and the stress test's framing changes visibly. Worth an OSMesa
  capture before/after.
