# Tagged camera header + one shared reader

## Goal

Make a scene's `// camera` header mean the same thing no matter which loader
reads it, and make a malformed header a reportable error instead of geometry
that silently offsets the scene.

The finished behavior is:

- one camera-header reader, in `src/repl/`, called by every consumer;
- each camera line self-identifies by role tag, so recognition never depends on
  arrival order, line position, or a parser state variable;
- a line the reader rejects produces a diagnostic naming the rule it broke -
  plus the file and line where the caller has them, which the catalog path does
  not (see the accumulator section) - and never falls through into the document
  as geometry;
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

**Both comment syntaxes are accepted.** `.glr` files carry `// @camera dist`,
but every line of exported C goes through `export_write_c89_line` →
`export_format_c89_comment_line` (`export.c:175`), which rewrites `//` to
`/* … */` for C89. So the same block reads `/* @camera dist */` in a `.c` and
`// @camera dist` in a `.glr`. The reader accepts either form rather than
relying on a normalization pass, and both are covered by tests - a reader that
handled only `//` would silently drop the camera from every exported file, which
is this bug with new syntax.

A one-line summary directive (`// @camera dist=10 rx=15 ry=20 pan=…`) was
considered and **rejected**: it makes the directive and the transforms two
sources of truth, and whichever loses means a hand edit to an exported `.c` is
silently discarded.

### One canonical `ry` source; the `g_angle` preamble stops being camera data

The tag lives on the transform line, always, and yaw is emitted as a literal
plus the animation hook:

Exported C, where the C89 rewrite applies:

```c
static float g_angle = 0.0f;                        /* untagged, never read back */
...
void display(void) {
  glLoadIdentity();
  /* camera */
  glTranslatef(0.0000f, 0.0000f, -10.0000f);        /* @camera dist */
  glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);            /* @camera rx */
  glRotatef(20.0000f + g_angle, 0.0f, 1.0f, 0.0f);  /* @camera ry */
  glTranslatef(-0.0000f, 2.5000f, -0.0000f);        /* @camera pan */
```

and the same block in a `.glr`, at baseline depth 0 with no wrapper and no
`g_angle`:

```c
// camera
glTranslatef(0.0000f, 0.0000f, -10.0000f);          // @camera dist
glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);              // @camera rx
glRotatef(20.0000f, 0.0f, 1.0f, 0.0f);              // @camera ry
glTranslatef(-0.0000f, 2.5000f, -0.0000f);          // @camera pan
```

The `ry` grammar is *a leading float literal, optionally followed by the exact
identifier `g_angle` added to it*. No other identifier is accepted:
`20.0f + tweak` is a diagnostic, not a silently-ignored suffix, because the
reader cannot know whether `tweak` is zero at load.

Parse it by tokenizing, not by string comparison: `strtof` for the literal (which
takes `-15.0f` for free), skip the optional `f`/`F` suffix and whitespace,
require `+`, skip whitespace and any wrapping parens, then match the identifier
`g_angle` and require nothing but `,` after it. That accepts `+g_angle`,
`+  g_angle`, `+ (g_angle)`, and a negative literal without a table of literal
spellings, and it keeps a trailing comment out of the value - the tag itself
lives in that comment. Consequences:

- **Exactly one tagged `ry` source per file**, so the duplicate-role rule below
  has no exception to carve out.
- **`g_angle`'s initializer is non-semantic and must stay `0.0f`.** The reader
  ignores it, so a hand-edited `static float g_angle = 45.0f;` would make the
  compiled C view and the imported REPL pose disagree - the exact
  two-sources-of-truth failure this design rejects elsewhere. Export always
  writes `0.0f`; import warns on a non-zero initializer rather than silently
  tolerating the divergence, and a hand-edit test covers it.

  **Where that warning lives, and what already eats the line.** The camera
  reader never sees it - `static float g_angle = …;` carries no tag, so it is
  `NOT_CAMERA` by construction. Deleting `cam_try_parse_angle_preamble` does not
  leave the line loose: it falls through to `import_parse_predef_decl_common`,
  which stashes the value and then explicitly declines to register it
  (`strcmp(name, "g_angle") != 0`, `import.c:567`). That guard is load-bearing
  and easy to miss, so it is named here: it is the reason the line is absorbed
  rather than reappearing as a document row. The non-zero warning is a two-line
  addition *at that guard* in phase 4, and phase 4 must also confirm the stash's
  later apply pass no-ops on the unregistered name.
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

**Where tags are honored** - "wherever it appears" needs bounding, because the
importer already refuses camera handling inside a snippet
(`s->in_snippet`, `import.c:2171`). The region rule, identical for all callers:

| Position | Result |
|---|---|
| Pre-snippet region or `display()` body, at the region's **baseline depth** | accepted |
| Deeper than baseline (inside any nested `{ }`) | `REJECTED` + diagnostic |
| Inside a snippet region | `REJECTED` + diagnostic |
| After the snippet region ends | `REJECTED` + diagnostic |

**Baseline depth, not absolute depth zero.** A `.glr` file's camera lines sit at
raw brace depth 0, but an exported `.c` puts them inside the `display()` wrapper
(`REPL_EXPORT_DISPLAY_OPEN_LINE` is `"void display(void) {"`,
`export.h:199`), so they are at raw depth 1. A literal depth-0 rule would reject
every exported camera block. The reader therefore records the depth in force
when a region begins and accepts tags only at that depth:

- file start → baseline 0, which is what a `.glr` uses;
- `repl_camera_header_set_region(hdr, REPL_CAMERA_REGION_DISPLAY)` on the
  display open line → baseline capture is **deferred**, not taken from that
  line. A hand-formatted file may put the brace on its own line:

  ```c
  void display(void)
  {
    glTranslatef(0.0f, 0.0f, -10.0f);   /* @camera dist */
  ```

  so capturing at the opener would record 0 and reject every line inside the
  body. Instead the reader arms a pending-region flag and takes
  `baseline := depth` at the **first offered line whose depth is > 0**. A tagged
  line arriving while the flag is still armed at depth 0 means the body never
  opened a brace: that is malformed, and is rejected with a diagnostic rather
  than silently accepted at baseline 0.
- **The region closes when depth falls below baseline** - the `}` of `display()`
  - and baseline reverts to 0. Without this a later function body at raw depth 1
  would match the stale display baseline and accept a camera tag that is inside
  someone else's function. Region exit is evaluated *after* the line's brace
  delta, so the closing brace itself is outside the region.
- deeper than baseline is a nested `{ }` and rejected.

Only the importer signals a display region; the example and tutorial loaders are
always at baseline 0. Note this is *stricter* than today's gate in a useful way:
`func_depth` counts only `CMD_FUNC_DEF` bodies, so a literal-argument transform
inside a top-level `for` body currently reaches the camera parser and can be
eaten as camera. Brace depth catches that; `func_depth` never did.

Every case outside the accepted one is *consumed with a diagnostic*, never
passed through as geometry - the tag is reserved syntax, so a line carrying it
is a camera line that is in the wrong place, not a document row. Exported files
put the camera block before the snippet start (`export_display.c:386`), so the
accepted region is where the format already writes.

**The reader tracks its own depth.** Requiring callers to supply `func_depth`
does not work: only `ImportState` has it, and `example_loader.c` /
`tutorial_runner.c` have no nesting tracking at all - they stream lines straight
through `repl_load_apply_line`. Making them compute it would put three copies of
a brace counter in the tree, which is this plan's own failure mode. Instead
`ReplCameraHeader` counts braces itself from the lines it is offered, reusing
`code_brace_delta` (`import.c:1937`, hoisted to a shared TU - it already skips
braces inside string/char literals and `//` comments). The contract that
buys this: **callers must offer every line, in order**, not only the ones they
suspect. Snippet-region entry/exit is signalled by the caller through
`repl_camera_header_set_region()`, since only the importer knows the snippet
markers and the other two callers are always pre-snippet.

**`code_brace_delta` must learn `/* … */` first.** It currently breaks out of
the scan on `//` only, so a brace inside a block comment - `/* reset {x,y} */`,
or an exporter prose block spanning lines - moves the depth counter and
desynchronizes the region. That was harmless while the helper only served
function-body tracking on `//`-commented REPL source; it is not harmless once
the same counter gates camera tags in C89 files whose every comment is
`/* … */`. Hoisting it therefore includes adding block-comment handling, with
open-across-lines state, and that state lives in `ReplCameraHeader` rather than
in a static.

**The catalog loader must offer from the top of its array**, including the
`@cfg` header and the metadata blanks it currently skips before its body loop
(`example_loader.c:361`). Those lines contain no braces today, so skipping them
would happen to work - which is exactly the kind of accidental correctness this
plan is removing. Offering everything keeps one rule for all three callers and
survives a future header line that does contain a brace.

### The marker becomes presentation, and needs a new owner

The `// camera` comment stays, but as a section heading for the expanded code
panel only - stashed in `ReplImportExportState::camera_comment_line`. Nothing
gates on it, so the two functions that currently decide whether a comment is
"the camera marker" collapse to one.

**That stash loses its writer on the example path if this is not explicit.**
`repl_example_consume_camera_header` does two jobs and only one is parsing: it
also writes the author's marker text into `io->camera_comment_line`
(`example_loader.c:437`). `import.c` has its own writer
(`import_handle_camera_comment`, `import.c:2153`); the example loader does not.
Deleting the region function would silently drop the heading on catalog and
tutorial-scaffold loads, and with it the `// --- Camera ---` banner that
survives a `.glr` round-trip through `glr_scene_write_camera`
(`export_glr.c:85`). The marker writer therefore moves **into the shared
reader**, which all three callers already offer every line to.

One semantic change rides along, worth stating rather than discovering: today
the marker is recorded only if the block validated; per-line consumption makes
it **record-on-sight**. The parity test compares `camera_comment_line` so
neither the move nor the semantic change can regress unnoticed.

### Deferred application, so a partial pose is unrepresentable

The reader accumulates roles into a `ReplCameraPose` plus a seen-mask and
applies **nothing** until end of load. `repl_camera_header_finish()` resolves the
partial pose to a complete one *before* it reaches the bridge:

- **none seen** → no bridge call at all; host defaults stand.
- **some seen** → `capture_pose()` reads the baseline pose, the seen roles
  overwrite it, and the merged - now complete - pose is applied. One warning
  names each missing role.
- **all four seen** → apply directly, no capture needed.

**The baseline is the camera's *destination*, not its live pose.** A load can
land mid-ease (example switches ease over several frames), and merging against a
transient interpolated pose would bake an arbitrary frame of an animation into
the result - the same class of bug as reading live state during a transition
that `cam_format_block_impl` already avoids by formatting from
`glr_camera_destination()`. `capture_pose()` is specified as the destination
reader, making it the exact inverse of the formatter.

**No bridge installed** (the standalone demo, most tests): `finish()` validates
and diagnoses as usual but applies nothing and captures nothing. That matches
today's documented no-header behavior - the scene inherits the live camera
(`export_glr.c:76`) - and keeps the reader linkable without a camera at all.

Merging lives in the reader, not the bridge, so `apply_pose()` never receives a
partial pose and no bridge implementation has to reason about a mask. That
requires the bridge to gain the symmetric reader:

```c
void (*capture_pose)(ReplCameraPose *out);   /* inverse of apply_pose */
```

which `glr_camera_capture` already provides in all but name. This subsumes
`cam_adopt_import_scene_default`'s state-4 gate and makes the half-applied
camera that started this investigation structurally impossible.

### Tag detection precedes both gates

Today `import_handle_camera` (`import.c:2168`) returns 0 when
`s->in_snippet || s->func_depth != 0`, so a tagged line in either position falls
through to the function-body or snippet handler and becomes geometry. Tag
detection must therefore run **before both gates**, not just the depth one:

1. detect the `@camera` tag;
2. tagged, but in a snippet or below the region baseline depth: **consume** the line (do not
   insert it) and record a diagnostic;
3. tagged in the accepted region: parse, validate, accumulate;
4. untagged: existing handling, entirely unchanged - the existing gates still
   run, they are simply no longer the first thing a tagged line meets.

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
| Tagged line below baseline depth | falls through as geometry |
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
                                              int line_no);
```

There is deliberately **no `func_depth` parameter**: the reader counts braces
itself, per the region rule above, so no caller has to own a notion of depth.
`line_no` is the caller's own numbering (0 when it has none) and exists solely
to stamp the diagnostic accumulator - the reader never interprets it.

The critical property is that `REJECTED` and `NOT_CAMERA` are distinct: rejected
lines are consumed, which is what stops a malformed header becoming geometry.

**Diagnostics accumulate; they are not only routed.** A fire-and-forget sink
cannot be asserted on, and the parity test compares an ordered diagnostic list.
`ReplCameraHeader` therefore owns a per-load accumulator - a fixed array of

```c
typedef struct {
    ReplCameraRole role;       /* which role, or NONE for whole-load warnings */
    ReplCameraRule rule;       /* which rule failed - the comparable key */
    int            line_no;    /* caller-supplied; 0 when unknown */
} ReplCameraDiag;
```

capped at `REPL_CAMERA_MAX_DIAGS` = 8 with an overflow counter, so a pathological
file cannot allocate. Line numbers are caller-supplied because the reader sees
lines, not files; the caller also owns the human-readable message (it has the
file name) and routes it to the sink it already uses - `repl_set_status` for the
app, stderr for the demo, the tutorial status line for setup scaffolds.

Tests read the accumulator directly rather than capturing a sink.

**`line_no` is not a parity key.** The two paths cannot agree on it as things
stand: the importer reads the file directly, while the catalog path is handed a
line array whose body pointer has already advanced past the `@cfg` header and
the metadata blanks (`example_loader.c:370`), so it has no notion of the
original file line. Parity therefore compares `(role, rule)` in order and
ignores `line_no`; `test_camera_header`, where one caller controls the
numbering, is where `line_no` is asserted. Making the example path carry true
file line numbers is real wiring work and is deliberately **not** in scope here -
if it is wanted later it is a phase of its own, not a free rider on this one.

### The bridge stops parsing text

With parsing in `src/repl/`, `ReplExportCameraBridge` returns to being a camera
adapter. `try_consume_import_line`, `reset_import`, `adopt_import_scene_default`,
`apply_example_block`, and `apply_capture_block_snap` collapse into one entry
point taking a resolved pose and an explicit intent:

```c
void (*apply_pose)(const ReplCameraPose *pose, ReplCameraApplyMode mode);
```

**Three modes, not two.** A snap/ease flag alone loses a second, independent
axis that the current bridge encodes structurally: whether the applied pose
becomes the scene default (what "Reset camera" returns to). Collapsing to
`SNAP` + `EASE` would make a snapshot restore adopt a scene default it must
not - the bug this plan is meant to prevent, reintroduced by its own API.

| Mode | Transition | Scene default | External 3D pose | Replaces | Caller |
|---|---|---|---|---|---|
| `REPL_CAMERA_APPLY_IMPORT` | snap | adopt | no | `try_consume_import_line` + `adopt_import_scene_default` | file / workspace load |
| `REPL_CAMERA_APPLY_RESTORE` | snap | leave | no | `apply_capture_block_snap` | `scene_snapshot`, workspace-save staging |
| `REPL_CAMERA_APPLY_EXAMPLE` | ease | adopt | **yes** | `apply_example_block` | example, tutorial scaffold |

**Three axes, not two.** `cam_apply_example_block` also calls
`glr_ctrl_view_record_external_3d_pose` (`glr_camera_export.c:265`), so that a
load while dwelling in 2D does not restore a stale pose on the 2D→3D trip. That
is a third behavior riding on which parser ran - the same failure shape as the
other two - so the mode table names it explicitly rather than leaving it as an
implementation detail of one bridge function. It also carries a documented
precondition (must run after the camera modelview is loaded in the display
frame), which is a property of `EXAMPLE`, not of `apply_pose` in general.

The fourth combination (ease without adopting) has no caller and is not
defined - adding it later is a deliberate act, not an accident of an
orthogonal flag pair. The transition difference is real and wanted - examples
animate (`glr_camera_ease_to`, `glr_camera_export.c:255`) while a file load
lands immediately - but today both it *and* the scene-default decision are side
effects of *which parser ran*. Making both explicit is what keeps them from
drifting again.

`scene_snapshot` (`scene_snapshot.c:136`) stops carrying a
`ReplExportCameraBlock` and carries a `ReplCameraPose`, so an in-memory scene
switch no longer formats and re-parses text. `SceneSnapshotCameraMode` maps
directly onto `ReplCameraApplyMode` and can likely be replaced by it.

`repl_live_demo`'s bridge becomes an `apply_pose` implementation writing its
orbit variables - a few lines, no parser.

**The formatter keeps the variant as a parameter, not as two functions.**
`fill_display_block` has three consumers, not one: `scene_snapshot.c:117` (which
goes away, migrating to a pose), and `repl_refresh_camera_lines`
(`export_setup.c:786`) → `g_cam_lines` → *both* the code-panel preview and
`export_glr.c:76`. A blanket merge onto the `g_angle` form would put
`glRotatef(20.0000f + g_angle, …)` in the code panel - a symbol the user cannot
type and that is not a REPL identifier - and add the suffix to every app-saved
`.glr`. So:

```c
void (*fill_block)(ReplExportCameraBlock *block, int with_anim_hook);
```

`with_anim_hook = 1` for exported C, `0` for the panel and `.glr` writer. The
*parser* is still one code path - the `ry` grammar accepts both spellings - so
the divergence this plan exists to end is still ended; what survives is a
one-bit output choice, which is what the split always should have been.
`repl_live_demo` is the evidence that it wants to be one function: it already
installs `demo_cam_fill_block` for `fill_save_block` *and* `fill_display_block`
(`repl_live_demo.c:262`).

### Corpus

44 of 46 `.glr` files under `examples/scenes/` and `stress/` carry a camera
marker, and 37 of those are in canonical numeric form needing only four appended
tags. But **marker presence is the wrong way to classify the corpus** under this
design: the marker stops meaning anything, so the question per file is "which
transforms should be tagged", and a markerless file whose opening transforms are
camera-shaped is exactly as much of an open decision as a marked one. Counting
that way gives **seven real edits**, not six.

Six of the seven are marked, and every one is a stress scene using a dynamic yaw
the format cannot express:

| File | Yaw |
|---|---|
| `stress/attrib-stack-push-pop-stress.glr` | `25.0f * t` |
| `stress/color-mask-clear-stress.glr` | `25.0f * t` |
| `stress/expression-eval-boundary-stress.glr` | `12.0f * t` |
| `stress/function-order-dependency-stress.glr` | `30.0f * t` |
| `stress/matrix-stack-recursion-stress.glr` | `20.0f * t` |
| `stress/nested-if-branching-stress.glr` | `18.0f * t` |

`matrix-stack-recursion-stress.glr` additionally folds a y offset into its
`dist` line. Moving it to `pan` is **not** a lossless relocation: the `-2.5f`
currently sits before `glRotatef(15, 1,0,0)` and is therefore unrotated, while
`pan` is applied after the rotations and gets rotated. That is an independent
framing change on top of the yaw decision, and phase 6's per-file capture is
what verifies it - the implementer should not treat that one as mechanical.

**A seventh file diverges without a marker at all.**
`stress/function-local-shadowing-stress.glr` heads its block
`// --- Global Declarations ---` and then opens with camera-shaped transforms at
depth 0:

```c
glTranslatef(0.0f, 0.0f, -8.00f);
glRotatef(12.0f, 1.0f, 0.0f, 0.0f);
glRotatef(20.0f * t, 0.0f, 1.0f, 0.0f);
```

Because the importer is marker-independent, file load eats the first two as
camera while catalog load keeps all three as geometry - the same divergence,
reached with no marker. The fix resolves it automatically (untagged → geometry
on both paths), but its file-load framing changes visibly: the camera reverts to
defaults and the transforms become body geometry. It needs the same before/after
capture and per-file decision as the other six. A scan of all 46 files found
exactly one other markerless file with top-level transforms
(`examples/scenes/pulse_bars_easing.glr`), and its transforms are inside a loop
body with non-literal arguments, so it never reached the parser and does not
diverge.

All seven render *differently on the two paths today*, so there is no "current
behavior" to preserve - the choice per file is between a static camera angle and
moving the rotation into the scene body as geometry.

### Two more emitters outside the corpus

Neither writes tags today, and with no back-compat reader both silently lose
their camera the day phase 5 lands:

- **`tools/glprobe/glprobe_extract.c:261`** emits a `// camera` marker plus four
  untagged transforms into extracted `.glr` scenes. Migrate with the exporters
  in phase 5 - it is the same four lines and one of the few places whose comment
  already explains *why* the block matters ("without it an extracted scene opens
  at gl-repl's default pose").
- **`tools/repl_live_demo/scenes/*.c`** - four fixtures (`torus.c`, `ring.c`,
  `triangle.c`, `whale-full-c.c`) in the old positional format. These are demo
  inputs, so they can be migrated mechanically with the corpus in phase 6, but
  they must be *in* the sweep: the demo is the load-bearing proof that the REPL
  pipeline links without the app, and a demo whose fixtures no longer carry a
  readable camera stops proving anything about the new reader.

**Personal `.glr` / `.c` saves predating the tags lose their camera on
re-import.** Nothing is released, so this is acceptable by the plan's own
premise, but anyone carrying a workspace from before the switch re-poses those
scenes by hand once. Worth a line in the phase-6 commit message rather than a
silent surprise.

### Documentation and the fifth writer

The five-line positional format is described in prose in several places, all of
which become wrong on the day phase 5 lands:

| Location | Change |
|---|---|
| `src/repl/example_loader.h:38` | dies with `repl_example_consume_camera_header` |
| `docs/ADVANCED_USAGE.md:522` | directive table row "a 5-line camera preset" → tagged lines; the surrounding metadata prose describes leading-directives-only, which the tags do not follow |
| `src/repl/tutorials.h:234` | setup-scaffold comment naming the 5-line block |
| `src/subsystems/tutorial/tutorial_runner.c:690` | ditto, plus the `pos +=` contract |
| `src/repl/export.h:58` | `ReplExportCameraBlock` doc comment and the whole bridge-callback block (lines 70-125) |
| `src/repl/examples.h:18` | "an optional `// camera` block specifying initial camera position/rotation" |
| `src/repl/export_glr.c:13` | the `.glr` format diagram, which draws the marker plus a literal 4-line block, and its "symmetric with example_loader.c" claim naming `repl_example_consume_camera_header` |
| `src/app/glr_camera_export.c:5` | file header describing the two variants and the preamble |
| `docs/ARCHITECTURE.md`, `docs/USER_GUIDE.md` | `// camera` header mentions |
| `CLAUDE.md` | "Example metadata & presentation reset" camera sentence |

`src/repl/export_glr.c:76` is a **fifth** camera-block writer - the `.glr`
exporter, emitting the numeric form independently of the bridge formatter. It
does not parse, so it is not a fifth reader, but it must grow the tags in phase
5 or `.glr` files saved from the app will not reload their own camera.

## Implementation phases

1. **Differential test first** - `tests/test_camera_header_parity.c` (below).
   Two constraints shape what phase 1 can actually contain:
   - `make test` gates pushes, so it cannot land red. It ships with the seven
     known-divergent scenes in an explicit `k_known_divergent[]` list asserted
     as *expected* divergence - the bug as data, failing if a file silently
     stops diverging. Phase 6 empties the list and the test asserts parity for
     all 46.
   - The diagnostic accumulator does not exist yet, so phase 1 compares
     **documents, pose, `camera_comment_line`, and predefs only**; the
     diagnostic comparison arrives with phases 2-4.

   Observing a pose at all needs a bridge: a `src/repl`-only test binary has
   none installed, so `apply_pose` is never called and the pose is invisible.
   The recording stub therefore lands in `tests/support/` in this phase, shared
   with `test_camera_apply_modes` rather than private to it.
2. **`src/repl/camera_header.{c,h}`** - tag recognition, brace-depth tracking,
   per-role parse, validation, accumulation, diagnostics, marker stash, `finish`
   merge. `code_brace_delta` hoists out of `import.c` to a shared TU **and gains
   `/* … */` handling, including the open-across-lines case**. Unit-tested
   against line fixtures with no loader involvement. Add both sources to
   `REPL_DEMO_DEP_SRCS` (`Makefile:563`).
3. **Bridge collapse** - `ReplCameraPose`, `capture_pose()`,
   `apply_pose(pose, IMPORT|RESTORE|EXAMPLE)`; delete the five line-oriented
   bridge entry points, the import state machine, and `fill_save_preamble`.
   Migrate `scene_snapshot` to carry a pose and pass `RESTORE`.
4. **Wire all consumers** - `import.c` (tag check before *both* gates, snippet
   and display regions signalled to the reader, plus the non-zero `g_angle`
   warning at the `import.c:567` guard), `example_loader.c` and
   `tutorial_runner.c` (offer from the top of the array, `pos += n` contract
   deleted), `repl_live_demo` (parser deleted, `apply_pose` kept).
5. **Export the tags** - `fill_block(block, with_anim_hook)`, unconditional
   `// camera` marker, `ry` as literal-plus-`g_angle` with a `0.0f` initializer,
   **`export_glr.c:76`** so app-saved `.glr` files reload their own camera, and
   **`glprobe_extract.c:261`** so extracted scenes do too.
6. **Corpus sweep** - 37 mechanical, 7 real edits, plus the four
   `repl_live_demo` fixtures, with an OSMesa capture per real edit. Empties the
   parity test's `k_known_divergent[]`. `whale-full-c.c` is **not** mechanical:
   it carries `static float g_angle = -114.3999f;` with a bare
   `glRotatef(g_angle, …)`, so its yaw has to be folded back into the literal
   (`-114.3999f + g_angle` with a `0.0f` initializer) rather than tagged in
   place.
7. **Goldens, tests and docs** - `--dump-code` and export fixtures gain the tag
   comments (regenerate with `BUILD=debug`); the documentation sites in the
   table above are updated in the same commit; and two existing test TUs move
   off the deleted API in the same breath, since `make test` must stay green:
   `tests/test_repl_state.c:879` asserts `repl_example_consume_camera_header`
   returns 5 and installs a legacy bridge with `reset_import` /
   `try_consume_import_line`, and `tests/test_repl_core_io.c:1318` writes
   synthetic workspace `.c` files carrying the `g_angle` preamble and untagged
   transforms.

Phases 2-4 land together; a half-migrated tree has two readers, which is the
state this plan exists to end.

## Tests

- **`test_camera_header_parity`** (new, registered in `TEST_BINS`) - for every
  `.glr` in `examples/scenes/` and `stress/`, load via the catalog and as a
  file, then compare:
  - normalized source text of every document row, and the row count;
  - command structure - `CmdType` sequence and per-command arg values;
  - resolved camera pose (dist / rx / ry / pan) and the scene default, observed
    through the shared recording bridge stub;
  - `camera_comment_line`, so the marker stash cannot lose its writer;
  - predef variable names and values;
  - the diagnostic list as ordered `(role, rule)` pairs - **not** `line_no`, per
    the accumulator section.

  Comparing row counts alone would pass a file whose camera lines became
  geometry as long as the count matched, which is precisely the bug.
- **`test_camera_header`** (new, `TEST_BINS`) - per-role parse plus every
  rejection rule: non-literal argument, `dist` with non-zero x/y, wrong rotate
  axis, trailing garbage after the call, duplicate role, `ry` without a leading
  literal, **`ry` with an unknown suffix identifier (`20.0f + typo`)**, tagged
  line below baseline depth, tagged line inside a snippet, tagged line after the
  snippet ends - the last three asserting the line is consumed *and* not
  inserted - and partial-pose `finish`: the merged pose keeps the destination
  value for every unseen role and one diagnostic names each. `line_no` is
  asserted here, where one caller owns the numbering. Both comment syntaxes
  (`// @camera dist` and the C89 `/* @camera dist */`) are covered for every
  role, and both baselines: raw depth 0 for a `.glr`, depth 1 inside a
  `display()` wrapper - including the split-brace form where `{` is on its own
  line, and a tag in a *later* function at raw depth 1 after `display()` closed
  (which must be rejected, not accepted against a stale baseline).
- **`code_brace_delta` fixtures** - a brace inside a `/* … */` comment on one
  line, a block comment spanning lines with a brace inside it, and a brace in a
  string literal, each asserting the depth counter is unmoved. These are the
  inputs that would silently break region tracking in C89 files.
- **`ry` grammar fixtures** - `+g_angle`, `+  g_angle`, `+ (g_angle)`,
  `-15.0f + g_angle`, and a trailing comment after the suffix, all accepted;
  `+ tweak` and a bare `g_angle` rejected.
- **`test_camera_apply_modes`** (new, `TEST_BINS`) - a recording stub bridge
  asserting each caller passes the mode it means: file load → `IMPORT` (snap,
  scene default adopted), `scene_snapshot` restore → `RESTORE` (snap, scene
  default *unchanged*), example and tutorial scaffold → `EXAMPLE` (ease, adopted).
  The `RESTORE` case is the one a two-mode API would have silently broken.
- Existing export/import round-trip tests - extended so a file whose transform
  numbers are edited by hand between export and import reads back the edited
  pose. That is the property the rejected one-line-directive design would have
  broken, so it is worth an explicit assertion. A second hand-edit case covers
  `g_angle`: rewriting its initializer must not move the imported pose, and must
  warn.
- **Mid-ease load** - start an example ease, load a partial-pose file before it
  settles, assert the merged pose used the destination rather than the
  interpolated live value.
- `make check-state-ownership` - the reader lives in `src/repl/` and must not
  reach for `glr_camera_*` directly; `check-repl-demo-stubs-shrinking` should
  register the demo bridge getting smaller, not larger.

## Risks

- **Phases 2-4 are one commit.** Splitting them leaves two live readers, and the
  intermediate state is exactly the bug.
- **Seven behavior changes, not one.** Every stress scene's framing moves
  visibly; each needs a before/after capture and a per-file call on static angle
  vs. body rotation. The markerless one
  (`function-local-shadowing-stress.glr`) is the easiest to miss because it has
  no camera header to notice.
- **`scene_snapshot` migration is on the hot path** for F12 cycling and scene
  restore, and its mode mapping is the easiest thing here to get wrong:
  `SCENE_SNAPSHOT_CAMERA_EASE` → `EXAMPLE`, everything else → `RESTORE`, and
  **never `IMPORT`** - a snapshot restore that adopts a scene default silently
  redefines what "Reset camera" returns to. `test_camera_apply_modes` exists
  for this line specifically.
- **Preprocessor conditionals are not evaluated.** The reader counts braces on
  the text it is offered, so a hand-written file with unbalanced braces split
  across `#if` / `#else` arms desynchronizes the counter and can reject a valid
  tag. Nothing this tree emits does that - exported C's only directives are
  includes and a balanced `#ifndef M_PI` guard - so this is a documented limit
  rather than a case to handle. If it ever bites, the symptom is a
  rejected-with-diagnostic camera line, not a silent misparse, which is the
  right failure direction.
- **The reader's depth tracking depends on callers offering every line.** A
  caller that filters before offering (an easy optimization to reach for)
  desynchronizes the brace counter and can accept a tagged line inside a
  function body. The header contract must say so, and the parity test covers it
  only indirectly - a fixture with a tagged line inside a `funcN` body belongs
  in `test_camera_header` proper.
- **Golden churn** is broad but mechanical; it lands with phase 7 rather than
  spread across the sweep.
