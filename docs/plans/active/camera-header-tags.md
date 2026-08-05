# Tagged camera header + one shared reader

## Goal

Make a scene's `// camera` header mean the same thing no matter which loader
reads it, and make a malformed header a reportable error instead of geometry
that silently offsets the scene.

The finished behavior is:

- one camera-header reader, in `src/repl/`, called by every consumer;
- each camera line self-identifies by role tag, so recognition never depends on
  arrival order, line position, or a parser state variable;
- a line the reader rejects produces a diagnostic naming the file, the line, and
  the rule it broke, and never falls through into the document as geometry;
- one emitted format - the save/display variant split disappears rather than
  being unified;
- the camera bridge stops parsing text: it receives a resolved pose plus an
  explicit snap-or-ease intent;
- direct file load and catalog load of the same `.glr` produce identical
  documents, camera pose, predefs, and diagnostics, enforced by a differential
  test.

Nothing here is released, and the `.glr` corpus is edited in place. There is no
back-compatible reading of the old positional format: the legacy readers are
deleted, not kept as fallbacks.

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

### Four parsers, not two

The split is wider than the two loaders:

| Parser | Location | Reads |
|---|---|---|
| import state machine | `glr_camera_export.c:117` | save + display variant, markerless, greedy |
| example/tutorial validator | `example_loader.c:125` | display variant only, marker-anchored, atomic |
| demo bridge | `tools/repl_live_demo/repl_live_demo.c:139` | hand-copied clone of the import state machine |
| scene snapshot | `src/repl/scene_snapshot.c:136` | round-trips live camera state *through formatted text lines* |

The demo's own comment says it "mirrors the import-side parser in
`src/app/glr_camera_export.c` (which the demo cannot link -- it is app-layer)",
so it is a copy that drifts by construction. `scene_snapshot_apply_camera`
never touches a file at all: it formats live state into a `ReplExportCameraBlock`
and re-parses it, so an in-memory scene switch pays for the text format and
inherits its failure modes.

### The same split, reached from export

`emit_export_cam_lines` (`src/repl/export_setup.c:743`) writes the save variant,
whose yaw line is the literal `glRotatef(g_angle, 0.0f, 1.0f, 0.0f)` with the
value carried by a file-scope `static float g_angle = N.NNNNf;`
(`export_setup.c:727`). The `// camera` marker is emitted only when
`g_camera_comment_line` is non-empty.

The importer handles that fine - `cam_try_parse_angle_preamble` reads the
preamble, and the marker is optional. The example loader cannot read it at all:
it has no preamble reader, and `example_cam_read_floats` fails on the `g_angle`
token. So an exported `.c` used as a catalog entry loses its camera the same way
the stress file does.

## Design decisions

### Role tags on the camera lines

Each camera line carries a trailing tag naming its role. The transform lines
remain the source of truth - a hand edit to a number in an exported `.c` must
survive re-import - and the tag exists solely to answer "camera or geometry?"
without reference to position. Four roles: `dist`, `rx`, `ry`, `pan`.

A one-line summary directive (`// @camera dist=10 rx=15 ry=20 pan=…`) was
considered and **rejected**: it makes the directive and the transforms two
sources of truth, and whichever loses means a hand edit to an exported `.c` is
silently discarded.

### One canonical `ry` source; the `g_angle` preamble stops being camera data

The tag lives on the transform line, always, and yaw is emitted as a literal
plus the animation hook:

```c
static float g_angle = 0.0f;                        /* untagged, never read back */
...
void display(void) {
  glLoadIdentity();
  // camera
  glTranslatef(0.0000f, 0.0000f, -10.0000f);        // @camera dist
  glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);            // @camera rx
  glRotatef(20.0000f + g_angle, 0.0f, 1.0f, 0.0f);  // @camera ry
  glTranslatef(-0.0000f, 2.5000f, -0.0000f);        // @camera pan
```

The `ry` grammar is *leading float literal, optional `+ <identifier>` suffix
which the reader ignores*. Consequences:

- **Exactly one tagged `ry` source per file**, so the duplicate-role rule below
  has no exception to carve out.
- **The exported binary still animates.** `g_angle` starts at 0 and accumulates
  in the timer (`if (g_rotating) g_angle += 0.5f;`, `export_display.c:697`); it
  is now a pure animation offset, not a value smuggled through file scope.
- **The save/display variant split disappears.** `fill_save_block` and
  `fill_display_block` collapse to one formatter, and `fill_save_preamble` is
  deleted along with the preamble reader that only the importer ever had.
- A hand-written `glRotatef(g_angle, …) // @camera ry` - no leading literal - is
  a diagnostic, not a silently-zero yaw.

### No header region: per-line consumption

With self-identifying lines there is no reason for a positional region, and
keeping one would re-create the divergence in a new place. The region concept is
deleted:

- `repl_example_consume_camera_header()` and its "returns 5 lines consumed"
  contract go away. Its two callers - `example_loader.c:378` and
  `tutorial_runner.c:730`, both of which do `pos += n` - switch to offering each
  line to the reader inside their existing body loop and skipping it when
  consumed. That is already the importer's shape, so all three consumers run the
  same code path.
- Tag order is free, and tagged lines need not be contiguous. Blank lines,
  comments, and the `// camera` marker between them are irrelevant because
  nothing is scanning for a run.
- **Scanning does not stop.** A tagged line is consumed wherever it appears in
  the file or display body.

### Deferred application, so a partial pose is unrepresentable

The reader accumulates roles into a `ReplCameraPose` with a seen-mask and
applies **nothing** until end of load. `repl_camera_header_finish()` then either
applies the pose once, via one bridge call, or reports what was missing:

- all four roles seen → apply with the caller's snap/ease intent, and adopt as
  the scene default;
- some roles seen → apply the seen roles over the current camera, warn once
  naming each missing role;
- none seen → no camera call at all, host defaults stand.

This subsumes `cam_adopt_import_scene_default`'s state-4 gate and makes the
half-applied camera that started this investigation structurally impossible.

### Tag detection precedes depth gating

Today a tagged line inside a `funcN` body would never reach the camera handler -
`import_handle_camera` (`import.c:2168`) returns 0 at `func_depth != 0` and the
line falls through to the function-body handler as geometry. The new order is:

1. detect the `@camera` tag;
2. if tagged and `func_depth > 0`: **consume** the line (do not insert it) and
   emit a diagnostic - a camera role inside a function body is meaningless, and
   silently rendering it as geometry is the failure mode this plan exists to
   remove;
3. if tagged at depth 0: parse, validate, accumulate;
4. untagged: existing handling, unchanged.

### One reader, in `src/repl/`

`src/repl/camera_header.{c,h}` owns tag recognition, per-role parsing,
validation, accumulation, and diagnostics. Deleted:

- `example_cam_parse_translate` / `_parse_rotate` / `_read_floats` /
  `_finish_call` / `_skip_sep` / `_skip_ws` (`example_loader.c`);
- `cam_try_consume_block_line` / `cam_try_parse_angle_preamble` /
  `cam_line_read_floats` / `cam_line_skip_sep` / `g_cam_parse_state`
  (`glr_camera_export.c`);
- the entire hand-copied parser in `tools/repl_live_demo/repl_live_demo.c`.

Rules the shared reader owns, so they cannot diverge again:

| Rule | Today |
|---|---|
| `dist` line must have x ≈ y ≈ 0 | example loader only (`example_loader.c:143`) |
| Rotate axis must match the role | example loader only |
| Line must end after its arguments | example loader only (`example_cam_finish_call`) |
| Non-literal argument (`20.0f * t`) | rejected on both, silently |
| `ry` literal-plus-offset grammar | new |
| Duplicate role in one file | undefined |
| Tagged line at `func_depth > 0` | falls through as geometry |
| Missing role | importer half-applies; example loader rejects the block |

Decisions: duplicate role is an error (first wins, second diagnosed); tagged
line in a function body is an error; missing roles warn at finish; every
rejection names file, line, and rule.

Atomicity stops being a policy question. The example loader's all-or-nothing
rule existed to stop a desynced parser from eating geometry; with independent
tagged lines and deferred application there is nothing to desync.

### Diagnostics API

The reader is neutral code in `src/repl/` and cannot know a file name or line
number, so the caller supplies them and receives a typed result:

```c
typedef enum {
    REPL_CAMERA_LINE_NOT_CAMERA = 0,  /* untagged - caller handles as usual */
    REPL_CAMERA_LINE_ACCEPTED,        /* consumed, role accumulated */
    REPL_CAMERA_LINE_REJECTED         /* consumed, do NOT insert; see diag */
} ReplCameraLineResult;

ReplCameraLineResult repl_camera_header_offer(ReplCameraHeader *hdr,
                                              const char *line,
                                              int func_depth,
                                              ReplCameraDiag *diag_out);
```

`ReplCameraDiag` carries the rule that failed and the role, as an enum plus a
short message; the caller prefixes its own `file:line` and routes to the sink it
already uses (`repl_set_status` for the app, stderr for the demo, the tutorial
status line for setup scaffolds). The critical property is that
`REJECTED` and `NOT_CAMERA` are distinct: rejected lines are consumed, which is
what stops a malformed header becoming geometry.

### The bridge stops parsing text

With parsing in `src/repl/`, `ReplExportCameraBridge` returns to being a camera
adapter. `try_consume_import_line`, `reset_import`, `adopt_import_scene_default`,
`apply_example_block`, and `apply_capture_block_snap` collapse into one entry
point taking a resolved pose and an explicit intent:

```c
void (*apply_pose)(const ReplCameraPose *pose, ReplCameraApplyMode mode);
```

`SNAP` for file load and snapshot restore, `EASE` for example load. That
difference is real and wanted - examples animate the transition
(`glr_camera_ease_to`, `glr_camera_export.c:255`) while a file load should land
immediately - but today it is a side effect of *which parser ran*. Making it an
argument is what keeps it from drifting again.

`scene_snapshot` (`scene_snapshot.c:136`) stops carrying a
`ReplExportCameraBlock` and carries a `ReplCameraPose`, so an in-memory scene
switch no longer formats and re-parses text. `SceneSnapshotCameraMode` maps
directly onto `ReplCameraApplyMode` and can likely be replaced by it.

`repl_live_demo`'s bridge becomes an `apply_pose` implementation writing its
orbit variables - a few lines, no parser.

The formatting half (`fill_save_block` → one `fill_block`) is unchanged apart
from appending the tags and merging the two variants.

### Corpus

44 of 46 `.glr` files under `examples/scenes/` and `stress/` carry a camera
header. 38 are in canonical numeric form and need only four appended tags.

**Six need real edits** - every one of the stress scenes uses a dynamic yaw the
format cannot express:

| File | Yaw |
|---|---|
| `stress/attrib-stack-push-pop-stress.glr` | `25.0f * t` |
| `stress/color-mask-clear-stress.glr` | `25.0f * t` |
| `stress/expression-eval-boundary-stress.glr` | `12.0f * t` |
| `stress/function-order-dependency-stress.glr` | `30.0f * t` |
| `stress/matrix-stack-recursion-stress.glr` | `20.0f * t` |
| `stress/nested-if-branching-stress.glr` | `18.0f * t` |

`matrix-stack-recursion-stress.glr` additionally folds a y offset into its
`dist` line, which moves to `pan`.

All six render *differently on the two paths today*, so there is no "current
behavior" to preserve - the choice per file is between a static camera angle and
moving the rotation into the scene body as geometry. Note the two files already
carrying uncommitted edits (`attrib-stack-push-pop`, `function-order-dependency`)
before touching them.

## Implementation phases

1. **Differential test first** - `tests/test_camera_header_parity.c` (below).
   Expected red on all six dynamic-yaw stress scenes before any other change;
   that is the check that the diagnosis is right.
2. **`src/repl/camera_header.{c,h}`** - tag recognition, per-role parse,
   validation, accumulation, `finish`, diagnostics. Unit-tested against line
   fixtures with no loader involvement. Add to `REPL_DEMO_DEP_SRCS`
   (`Makefile:563`).
3. **Bridge collapse** - `ReplCameraPose` + `apply_pose(mode)`; delete the five
   line-oriented bridge entry points, the import state machine, and
   `fill_save_preamble`. Migrate `scene_snapshot` to carry a pose.
4. **Wire all consumers** - `import.c` (tag check before the depth gate),
   `example_loader.c` and `tutorial_runner.c` (per-line offer, `pos += n`
   contract deleted), `repl_live_demo` (parser deleted).
5. **Export the tags** - one `fill_block`, merged variants, unconditional
   `// camera` marker, `ry` as literal-plus-`g_angle`.
6. **Corpus sweep** - 38 mechanical, 6 real edits, with an OSMesa capture per
   real edit.
7. **Goldens** - `--dump-code` and export fixtures gain the tag comments.
   Regenerate with `BUILD=debug`.

Phases 2-4 land together; a half-migrated tree has two readers, which is the
state this plan exists to end.

## Tests

- **`test_camera_header_parity`** (new, registered in `TEST_BINS`) - for every
  `.glr` in `examples/scenes/` and `stress/`, load via the catalog and as a
  file, then compare:
  - normalized source text of every document row, and the row count;
  - command structure - `CmdType` sequence and per-command arg values;
  - resolved camera pose (dist / rx / ry / pan) and the scene default;
  - predef variable names and values;
  - the diagnostic list, including order.

  Comparing row counts alone would pass a file whose camera lines became
  geometry as long as the count matched, which is precisely the bug.
- **`test_camera_header`** (new, `TEST_BINS`) - per-role parse plus every
  rejection rule: non-literal argument, `dist` with non-zero x/y, wrong rotate
  axis, trailing garbage after the call, duplicate role, tagged line at
  `func_depth > 0` (asserting the line is consumed *and* not inserted),
  `ry` without a leading literal, missing roles at `finish`.
- Existing export/import round-trip tests - extended so a file whose transform
  numbers are edited by hand between export and import reads back the edited
  pose. That is the property the rejected one-line-directive design would have
  broken, so it is worth an explicit assertion.
- `make check-state-ownership` - the reader lives in `src/repl/` and must not
  reach for `glr_camera_*` directly; `check-repl-demo-stubs-shrinking` should
  register the demo bridge getting smaller, not larger.

## Risks

- **Phases 2-4 are one commit.** Splitting them leaves two live readers, and the
  intermediate state is exactly the bug.
- **Six behavior changes, not one.** Every stress scene's framing moves
  visibly; each needs a before/after capture and a per-file call on static angle
  vs. body rotation.
- **`scene_snapshot` migration is on the hot path** for F12 cycling and scene
  restore. The pose swap should be behavior-identical, but SNAP/EASE mapping is
  easy to get backwards - `SCENE_SNAPSHOT_CAMERA_EASE` → `EASE`,
  everything else → `SNAP`, and the snapshot path must not adopt a scene
  default the way a file load does.
- **Golden churn** is broad but mechanical; it lands with phase 7 rather than
  spread across the sweep.
