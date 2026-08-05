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
- one *read* format. There are still two emitted projections - `.c` carries the
  animation-hook line, `.glr` and the code panel do not - but they differ by the
  presence of one optional row, not by the spelling of a shared one, so every
  consumer parses the four pose lines identically;
- the camera bridge stops parsing text: it receives a resolved pose plus an
  explicit snap-or-ease intent;
- direct file load and catalog load of the same `.glr` produce identical
  documents, camera pose, predefs, and diagnostics, enforced by a differential
  test.

Nothing here is released, and the `.glr` corpus is edited in place. There is no
back-compatible reading of the old positional format: the legacy readers are
deleted, not kept as fallbacks.

## The bug this comes from

`tests/scenes/stress/matrix-stack-recursion-stress.glr` carries a
hand-authored header whose first transform folds a y offset into the distance
line and whose yaw is `20.0f * t`:

```c
// --- Camera -------------------------
glTranslatef(0.0f, -2.5f, -10.00f);
glRotatef(15.0f, 1.0f, 0.0f, 0.0f);
glRotatef(20.0f * t, 0.0f, 1.0f, 0.0f);
```

Loaded as a file it looks like it works; loaded from
`tests/scenes/stress/catalog.ini` the three transforms land in the document and
the tree renders 2.5 units low, spinning. The two loaders disagree because they
were written to opposite contracts.

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
without reference to position. Four pose roles - `dist`, `rx`, `ry`, `pan` -
plus one write-only role, `spin`, covered in the next section.

**Both comment syntaxes are accepted.** `.glr` files carry `// @camera dist`,
but every line of exported C goes through `export_write_c89_line` →
`export_format_c89_comment_line` (`export.c:175`), which rewrites `//` to
`/* … */` for C89. So the same block reads `/* @camera dist */` in a `.c` and
`// @camera dist` in a `.glr`. The reader accepts either form rather than
relying on a normalization pass, and both are covered by tests - a reader that
handled only `//` would silently drop the camera from every exported file, which
is this bug with new syntax.

**The `// camera` marker is the reader's job too, and it needs its own result.**
The marker stays deliberately untagged, so under a three-value result it comes
back `NOT_CAMERA` and every caller inserts it into the document as an ordinary
comment row. Keeping a separate marker handler beside the shared reader
contradicts the whole design. The result enum therefore gains a fourth value,
`REPL_CAMERA_LINE_MARKER`: consumed, not inserted, no role accumulated. The
reader also captures the marker's verbatim text, which is what feeds
`camera_comment_line` (below).

This closes a live bug rather than only a hypothetical one.
`repl_comment_alpha_payload_equals` (`text_helpers.c:18`) hard-requires
`p[0] == '/' && p[1] == '/'`, and an exported `.c` writes its marker through the
C89 rewrite as `/* camera */`. So **today an exported file already loses its
camera marker on re-import** - the transforms still parse, the section heading
does not. The marker matcher must learn the block-comment form along with the
tags, and `test_camera_header` covers `/* camera */` as well as `// camera`.

A one-line summary directive (`// @camera dist=10 rx=15 ry=20 pan=…`) was
considered and **rejected**: it makes the directive and the transforms two
sources of truth, and whichever loses means a hand edit to an exported `.c` is
silently discarded.

### Four pose roles plus a tagged animation hook

The tag lives on the transform line, always. The four pose roles carry literal
floats and nothing else; the exported C's animation hook gets **its own line and
its own role**, `spin`, so no pose role has to admit an expression.

Exported C, where the C89 rewrite applies:

```c
static float g_angle = 0.0f;                        /* untagged boilerplate */
...
void display(void) {
  glLoadIdentity();
  /* camera */
  glTranslatef(0.0000f, 0.0000f, -10.0000f);        /* @camera dist */
  glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);            /* @camera rx */
  glRotatef(20.0000f, 0.0f, 1.0f, 0.0f);            /* @camera ry */
  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);             /* @camera spin */
  glTranslatef(-0.0000f, 2.5000f, -0.0000f);        /* @camera pan */
```

and the same block in a `.glr`, at baseline depth 0 with no wrapper, no
`g_angle`, and therefore no `spin` line at all:

```c
// camera
glTranslatef(0.0000f, 0.0000f, -10.0000f);          // @camera dist
glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);              // @camera rx
glRotatef(20.0000f, 0.0f, 1.0f, 0.0f);              // @camera ry
glTranslatef(-0.0000f, 2.5000f, -0.0000f);          // @camera pan
```

**`spin` is write-only.** It carries no pose data: its sole argument must be the
`g_angle` identifier, it is accepted and discarded, and `finish()` never counts
it toward the seen-mask or the missing-role warning. A rejected pose role
changes what the scene looks like; a rejected `spin` is a diagnostic about a
line that was going to be thrown away regardless.

That argument check is **tokenized, not string-compared**, so `g_angle`,
`( g_angle )`, and any whitespace spelling all pass while `g_angle2` and
`g_angle + 1` do not. The same applies to the pose roles' float arguments: a
leading `-` is part of the literal, not garbage after it.

An earlier draft folded the hook into `ry` as `glRotatef(20.0000f + g_angle, …)`
and gave that one role a bespoke grammar - *a leading float literal, optionally
followed by the exact token `+ g_angle`*. Splitting it out is strictly simpler:

- **Every pose role parses identically.** Two call shapes, literal arguments
  only, axis check, end-of-call. The reader holds no expression sublanguage at
  all, and `ry` stops being the one role with a special case. Three rejection
  rules disappear with it (`ry` without a leading literal, `ry` with an unknown
  suffix identifier, the literal-plus-offset grammar itself).
- **The writer's variant flag stops leaking into the reader.**
  `cam_format_block_impl(block, int use_g_angle)` (`glr_camera_export.c:31`) is
  *already* one formatter with a boolean - `fill_save_block` is `impl(1)`,
  `fill_display_block` is `impl(0)`. Under the folded grammar that boolean
  survives as a suffix rule the reader must understand. As a separate role it
  stays purely a writer concern: emit the row or don't. An absent optional role
  needs no reader branching, which is what lets the `.c` and `.glr` projections
  share one parse path exactly.
- **A hand edit still survives re-import.** `ry`'s number sits alone on its line,
  so there is no `20.0000f + g_angle` for someone to edit the wrong half of.

Consequences either way:

- **The exported binary still animates.** `g_angle` starts at 0 and accumulates
  in the timer (`if (g_rotating) g_angle += 0.5f;`, `export_display.c:697`); it
  is a pure animation offset, not a value smuggled through file scope.
- **`fill_save_preamble` is deleted**, along with the preamble reader
  (`cam_try_parse_angle_preamble`) that only the importer ever had. The
  `static float g_angle = 0.0f;` line reverts to plain header boilerplate,
  absorbed on import by the existing predef-decl handler, whose
  `strcmp(name, "g_angle") != 0` guard (`import.c:567`) already stashes it
  without registering a variable.
- **`g_angle`'s initializer is non-semantic and must stay `0.0f`.** A hand-edited
  `static float g_angle = 45.0f;` would make the compiled C view and the imported
  REPL pose disagree. The shared reader cannot catch this - the declaration is
  untagged, and teaching the reader to inspect untagged lines would resurrect the
  preamble reader this section deletes. **The warning therefore lives in
  `import.c`, at the `g_angle` guard that already exists**, and is explicitly
  importer-local: the example, tutorial, and demo paths never see a `g_angle`
  line, so there is nothing there to miss.
- **`REPL_EXPORT_CAMERA_LINES` goes 4 → 5** (`export_state.h:28`). It dimensions
  `ReplExportCameraBlock.lines[]`, `g_cam_lines[]` (`state_views.h:158`) and
  about a dozen loop sites. Most are plain iteration and the `.glr` / code-panel
  projection simply leaves the fifth slot empty, but two need checking: the
  row-count arithmetic at `export_setup.c:975`, and the code panel, which counts
  and renders `cam_lines[]` unconditionally in two places
  (`repl_code_panel.c:240` and `:2027`) and would otherwise draw a blank row. The
  `.c` writers already skip empty slots (`export_setup.c:755`,
  `export_glr.c:95`). Note the comment at `example_loader.c:415-422` explaining
  that today's hook deliberately *substitutes for* the numeric rotate to hold the
  count at 4 - that constraint is an artifact of the positional parser and dies
  with it.
- **Writer-side ordering**: the canonical emit order is
  `dist, rx, ry, spin, pan`. `spin` must fall between `rx` and `pan` for the
  compiled C to compose the intended modelview; swapping it with `ry`
  specifically is harmless, since same-axis rotations compose additively, but it
  is still off-canonical and draws the order warning below.
- A tagged `ry` whose argument is not a literal - `glRotatef(g_angle, …)
  // @camera ry` - is a diagnostic, not a silently-zero yaw.

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
- **`camera_comment_line` moves to the reader.** Today the marker text has two
  independent writers: `import_handle_camera_comment` (`import.c:2153`) on the
  file path, and `repl_example_consume_camera_header` itself
  (`example_loader.c:437-444`) on the catalog and tutorial paths. Deleting the
  region function without reassigning that write would silently drop the
  author's section banner (`// --- Camera --- `) from the code panel and from
  `glr_scene_write_camera`'s `.glr` round-trip (`export_glr.c:85-88`). The
  reader returns `REPL_CAMERA_LINE_MARKER` and stashes the verbatim text, which
  `finish()` hands back as `marker_text` for the caller to publish - one place
  decides what the marker *is*, instead of two. One semantic
  change falls out, worth stating rather than discovering: today the banner is
  recorded only if the whole block validated, and it now records on sight. The
  parity test compares `camera_comment_line`, so neither the move nor the
  semantic change can regress unnoticed.

**Recognition order is free; the emitted format is still ordered.** Nothing
scans for a run, so the reader accepts the roles in any order, and blank lines,
comments, and the marker between them are irrelevant. That is a statement about
*recognition*, not a licence to shuffle the block - and the distinction matters
because the two halves of the format disagree about what order means:

- The REPL consumes the tagged lines and applies one resolved pose at
  `finish()`, so order genuinely cannot affect it.
- The **exported C executes those same lines in place**. `pan` before the
  rotations, or a tagged transform sitting after executable setup, composes a
  different modelview - so the compiled program and the REPL would render the
  same file differently. That is precisely the export-parity property this plan
  exists to protect, lost from the other end.

So: only comments and blank lines may interleave with the tagged block. A tagged
line separated from the others by an executable line, or a role out of canonical
`dist, rx, ry, spin, pan` order, is **accepted with a warning diagnostic** - the
pose is unambiguous, so refusing the load would be disproportionate, but the file
is not one the exporter would ever write and its compiled twin will not match.
`test_camera_header` covers both shapes.

"Executable line" needs a definition the reader can apply without parsing C, and
the cheap one is sufficient: a line is executable if it holds any non-whitespace
that is not a comment (`//` or `/* … */`, including a continuation line inside an
open block comment) and not a preprocessor directive (`#`). Declarations count as
executable for this purpose - they are not part of a camera block either.

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
  opened a brace: malformed, rejected with a diagnostic rather than silently
  accepted at baseline 0.
- **The region closes when depth falls below baseline** - the `}` of `display()`
  - and baseline reverts to 0. Without this a later function body at raw depth 1
  would match the stale display baseline and accept a camera tag sitting inside
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

**`code_brace_delta` must learn `/* … */` first.** It breaks out of the scan on
`//` only, so a brace inside a block comment - `/* reset {x,y} */`, or an
exporter prose block spanning several lines - moves the depth counter and
desynchronizes the region. That is harmless while the helper only serves
function-body tracking over `//`-commented REPL source; it stops being harmless
the moment the same counter gates camera tags in C89 files where *every* comment
is `/* … */`.

Block-comment state has to survive across lines, so the hoisted helper takes it
as an in/out parameter rather than hiding it in a static - two consumers with
independent state is the whole point:

```c
int code_brace_delta(const char *line, int *in_block_comment);
```

`ReplCameraHeader` owns one flag; `import.c:2023`, the only existing caller,
owns another beside `s->func_depth` in `ImportState`. Passing `NULL` is not
offered as a shortcut - a caller that opts out of block-comment tracking is a
caller that silently mis-scopes, which is the failure this is fixing.

**What it already gets right:** string and char literals, *including escaped
quotes* - `if (c == '\\' && p[i + 1]) { i++; continue; }` (`import.c:1942`), so
`"a brace \" { inside a string"` is already safe. Block comments are the only
gap; the escape handling does not need revisiting.

**The catalog loader must offer from the top of its array**, including the
`@cfg` header and the metadata blanks it currently skips before its body loop
(`example_loader.c:361`). Those lines contain no braces today, so skipping them
would happen to work - which is exactly the kind of accidental correctness this
plan is removing. Offering everything keeps one rule for all three callers and
survives a future header line that does contain a brace.

### Deferred application, so a partial pose is unrepresentable

The reader accumulates roles into a `ReplCameraPose` plus a seen-mask and
applies **nothing** until end of load. `repl_camera_header_finish()` resolves the
partial pose to a complete one *before* it reaches the bridge. The mask tracks
the four pose roles only - `spin` contributes nothing to a pose and is never
counted:

- **no pose role seen** → no bridge call at all; host defaults stand.
- **some seen** → `capture_pose()` reads the baseline pose, the seen roles
  overwrite it, and the merged - now complete - pose is applied.
- **all four pose roles seen** → apply directly, no capture needed.

**A partial header is a note, not a scolding.** A hand-authored `.glr` that sets
only `dist` and is content with the defaults for the rest is a legitimate file,
and the obvious reading of "one warning names each missing role" - three warnings
on screen for one deliberate choice - would make the common hand-authoring case
the noisiest one. So the two audiences are served differently: the accumulator
records **one diagnostic per missing role**, because that is what a test can
compare and what makes the parity tuple precise, while the caller emits **a
single message naming them together** ("camera header: rx, ry, pan not set;
keeping current"). Missing-role diagnostics carry the `note` severity, below the
warnings that indicate a genuinely malformed file.

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

The corollary is that **a test asserting anything about a pose must install a
bridge.** `test_camera_header_parity` compares resolved poses and scene
defaults, so it cannot run bridge-less; it uses the same recording stub as
`test_camera_apply_modes`, which therefore lands in `tests/support/` rather than
inside one test's TU.

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
| `spin` argument must be the bare `g_angle` token | new |
| Duplicate role in one file | undefined |
| Tagged line below baseline depth | falls through as geometry |
| Roles out of canonical order, or split by an executable line | undefined |
| Missing role | importer half-applies; example loader rejects the block |
| `/* camera */` marker (C89 form) | unreadable - `text_helpers.c:18` requires `//` |

Decisions: duplicate role is an error (first wins, second diagnosed); tagged
line in a function body is an error; missing pose roles warn at finish (`spin`
is exempt - it is write-only); out-of-order or interleaved tagged lines warn;
every rejection names file, line, and rule.

Atomicity stops being a policy question. The example loader's all-or-nothing
rule existed to stop a desynced parser from eating geometry; with independent
tagged lines and deferred application there is nothing to desync.

### Diagnostics API

The reader is neutral code in `src/repl/` and cannot know a file name or line
number, so the caller supplies them and receives a typed result:

```c
typedef enum {
    REPL_CAMERA_LINE_NOT_CAMERA = 0,  /* untagged - caller handles as usual */
    REPL_CAMERA_LINE_MARKER,          /* bare camera marker, either syntax */
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
`MARKER` is the same property applied to the one camera line that carries no
role - consumed, so no caller inserts it as a document row.

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
file cannot allocate.

**Severity is derived from the rule, not stored.** The reader emits three
strengths - a rejection (the line is malformed and discarded), a warning (the
pose is unambiguous but the file is not one the exporter would write: order,
interleaving, a non-zero hook initializer), and a note (a missing pose role,
which merges cleanly from the baseline). Rather than add a fourth field that
callers could set inconsistently, `ReplCameraRule` **fully determines** severity
through one table:

```c
ReplCameraSeverity repl_camera_rule_severity(ReplCameraRule rule);
```

That keeps `(role, rule, line_no)` as the whole comparable key - a severity field
would be a derived value in the parity tuple, which is how two paths end up
"differing" on something neither of them chose.

**The overflow counter is part of parity, not bookkeeping.** Comparing only the
stored eight would let two paths agree on the first eight diagnostics and differ
on everything after, which is a divergence the test was built to catch. The
accumulator is a bounded *inspection cache* over a complete diagnostic stream:
every rejection is reported to the caller's sink as it happens, and parity
compares the stored `(role, rule, line_no)` list **plus** the overflow count.

Line numbers are caller-supplied because the reader sees
lines, not files; the caller also owns the human-readable message (it has the
file name) and routes it to the sink it already uses - `repl_set_status` for the
app, stderr for the demo, the tutorial status line for setup scaffolds.

Tests read the accumulator directly rather than capturing a sink.

**`finish()` has explicit outputs.** The plan has been saying `finish()`
"publishes" the marker and "applies" a pose without showing how a caller gets at
either. It returns a summary and exposes the accumulator, so nothing is
communicated by side effect alone:

```c
typedef struct {
    int                  pose_applied;   /* did a bridge call happen */
    ReplCameraPose       pose;           /* the resolved pose, applied or not */
    unsigned             seen_mask;      /* which pose roles the file supplied */
    const char          *marker_text;    /* verbatim marker, NULL if none seen */
    int                  diag_count;     /* stored diagnostics */
    int                  diag_overflow;  /* dropped past REPL_CAMERA_MAX_DIAGS */
} ReplCameraFinish;

ReplCameraFinish       repl_camera_header_finish(ReplCameraHeader *hdr,
                                                 ReplCameraApplyMode mode);
const ReplCameraDiag  *repl_camera_header_diags(const ReplCameraHeader *hdr);
```

`marker_text` is what the caller writes into `camera_comment_line` - the reader
stashes, the caller publishes, so the reader still touches no state it does not
own. `pose` is populated whether or not a bridge was installed, which is what
lets a bridge-less test assert the resolved pose directly instead of inferring it
from a call that never happened.

**`line_no` is a parity key after all** - the offer-from-the-top rule bought it.
An earlier draft dropped it, on the grounds that the catalog path is handed a
line array whose body pointer has already advanced past the `@cfg` header and the
metadata blanks (`example_loader.c:370`) and so has no notion of the original
file line. That reasoning expired the moment the region rule required the catalog
loader to offer **from index 0**: for a `.glr` catalog entry the array *is* the
file's lines, so a 1-based array index and the importer's file line are the same
number. Both paths therefore supply a real line, parity compares it, and the goal
statement's promise that a diagnostic names file *and* line holds on every path
rather than on one.

The one case with no file behind it is a built-in example compiled into
`examples.c`, where `line_no` is the array index - stable, useful in a message,
and not something the parity test exercises, since it walks `.glr` corpus files.
`0` remains legal in the signature for a caller that genuinely has no numbering,
but no caller in this tree passes it.

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

| Mode | Transition | Scene default | Replaces | Caller |
|---|---|---|---|---|
| `REPL_CAMERA_APPLY_IMPORT` | snap | adopt | `try_consume_import_line` + `adopt_import_scene_default` | file / workspace load |
| `REPL_CAMERA_APPLY_RESTORE` | snap | leave | `apply_capture_block_snap` | `scene_snapshot`, workspace-save staging |
| `REPL_CAMERA_APPLY_EXAMPLE` | ease | adopt | `apply_example_block` | example, tutorial scaffold |

The fourth combination (ease without adopting) has no caller and is not
defined - adding it later is a deliberate act, not an accident of an
orthogonal flag pair. The transition difference is real and wanted - examples
animate (`glr_camera_ease_to`, `glr_camera_export.c:255`) while a file load
lands immediately - but today both it *and* the scene-default decision are side
effects of *which parser ran*. Making both explicit is what keeps them from
drifting again.

**`EXAMPLE` carries a third effect, and it has a timing contract the mode table
cannot express.** `cam_apply_example_block` also calls
`glr_ctrl_view_record_external_3d_pose` (`glr_camera_export.c:265`), which keeps
the controller's saved-3D snapshot aligned so a 2D→3D restoration lands on the
newly-loaded example's angle instead of a stale one. Its header states the
precondition plainly: *"must be called only from a specific point in the display
frame, after the camera modelview is loaded"* (`glr_camera_export.h:16`).

That precondition is **already violated on the path this plan preserves**:
`glr_scene_load_example` (`glr_actions.c:1053`) calls `repl_load_example`
synchronously off the action path, nowhere near a display frame. Moving the call
into `apply_pose(…, EXAMPLE)` unchanged would inherit the violation and hide it
behind a mode name.

**Decision: enqueue in the bridge implementation, drain in the controller.** The
neutral interface in `src/repl/export.h` stays exactly `apply_pose(pose, mode)` -
it never learns that a 3D pose record exists. The app-side implementation in
`glr_camera_export.c`, which is already app-layer and already calls
`glr_ctrl_view_record_external_3d_pose` today, sets a pending record instead of
calling it, and `glr_ctrl` drains that at its existing frame-safe point, where
other deferred view state is handled.

The alternative - hoisting the call out to the action layer, so the bridge is a
pure pose adapter - is architecturally tidier at the interface and worse
everywhere else: "EXAMPLE also means record the 3D pose" would then have to be
restated at each of the three call sites (example load, tutorial scaffold, and
whatever comes next), which is precisely the duplicated-knowledge failure this
plan exists to end. The neutral API is already clean because the effect lives
behind the app-side implementation, not because the effect was moved to a
caller. `test_camera_apply_modes` asserts the record is enqueued exactly once per
example load and drained exactly once.

`scene_snapshot` (`scene_snapshot.c:136`) stops carrying a
`ReplExportCameraBlock` and carries a `ReplCameraPose`, so an in-memory scene
switch no longer formats and re-parses text. `SceneSnapshotCameraMode` maps
directly onto `ReplCameraApplyMode` and can likely be replaced by it.

`repl_live_demo`'s bridge becomes an `apply_pose` implementation writing its
orbit variables - a few lines, no parser.

The formatting half changes less than it first appears. `fill_save_block` and
`fill_display_block` are *already* one function behind a boolean -
`cam_format_block_impl(block, int use_g_angle)` (`glr_camera_export.c:31`) - so
there is no split to dissolve, only two bridge entry points to collapse onto one:

```c
void (*fill_block)(ReplExportCameraBlock *block, int with_anim_hook);
```

What actually changes is the tags, the unconditional marker, and the hook moving
to its own `spin` row.

**Both projections stay, deliberately**, because `fill_display_block` has three
consumers and not one: `scene_snapshot.c:117` (which goes away, migrating to a
pose), and `repl_refresh_camera_lines` (`export_setup.c:786`) → `g_cam_lines` →
*both* the code-panel preview and `export_glr.c:76`. A blanket merge onto the
hook-carrying form would put a `g_angle` reference in the code panel - a symbol
the user cannot type and that is not a REPL identifier - and into every
app-saved `.glr`. So `with_anim_hook = 1` for exported C, `0` for the panel and
the `.glr` writer.

Calling that "one emitted format" would be wrong. It is **one formatter with two
output projections that differ by one optional row** - which is the whole point
of the `spin` split: a row the reader may simply not see is the one kind of
difference that costs the reader nothing. `repl_live_demo` is the evidence the
two entry points want to be one function; it already installs
`demo_cam_fill_block` for `fill_save_block` *and* `fill_display_block`
(`repl_live_demo.c:262`).

### Corpus

44 of 46 `.glr` files under `examples/scenes/` and `tests/scenes/stress/` carry
a camera header. 38 are in canonical numeric form and need only four appended
tags.

**Six need real edits** - every one of the stress scenes uses a dynamic yaw the
format cannot express:

| File | Yaw |
|---|---|
| `tests/scenes/stress/attrib-stack-push-pop-stress.glr` | `25.0f * t` |
| `tests/scenes/stress/color-mask-clear-stress.glr` | `25.0f * t` |
| `tests/scenes/stress/expression-eval-boundary-stress.glr` | `12.0f * t` |
| `tests/scenes/stress/function-order-dependency-stress.glr` | `30.0f * t` |
| `tests/scenes/stress/matrix-stack-recursion-stress.glr` | `20.0f * t` |
| `tests/scenes/stress/nested-if-branching-stress.glr` | `18.0f * t` |

`matrix-stack-recursion-stress.glr` additionally folds a y offset into its
`dist` line, which moves to `pan`. That relocation is **not** a behavior-
preserving rewrite and should not be treated as mechanical: the offset currently
sits *before* `glRotatef(15, 1,0,0)`, while `pan` applies *after* the rotations,
so the offset ends up rotated. It needs its own visual check alongside the yaw
decision.

**A seventh file diverges without a marker at all.**
`tests/scenes/stress/function-local-shadowing-stress.glr` heads its block
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
   - `make test` gates pushes, so it cannot simply land red. It ships with the
     seven known-divergent scenes in an explicit `k_known_divergent[]` list
     asserted as *expected* divergence - the bug as data, failing if a file
     silently stops diverging. **The list empties at phase 4, not phase 6** -
     see the sequencing note below.
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
5. **Export the tags** - one `fill_block(block, with_anim_hook)`, unconditional
   marker, `ry` numeric with the hook on its own `spin` row and `g_angle` left at
   `0.0f`, **`export_glr.c:76`** so app-saved `.glr` files reload their own
   camera, and **`glprobe_extract.c:261`** so extracted scenes do too.
6. **Corpus sweep** - 38 mechanical, 7 real edits, plus the four
   `repl_live_demo` fixtures, with an OSMesa capture per real edit. (38 + 7 = 45;
   the 46th, `pulse_bars_easing.glr`, has no camera content at all. The seventh
   real edit is markerless, so it was never among the 44 and does not come out of
   the 38.) **`whale-full-c.c` is not mechanical**: it is the one demo fixture
   carrying a `g_angle`, and it carries a *pose* in it -
   `static float g_angle = -114.3999f;` (`:98`) with a bare
   `glRotatef(g_angle, …)` (`:436`). Under the `spin` split that yaw has to be
   lifted out into a real `ry` literal of `-114.3999f`, with the bare rotate
   retagged `spin` and the initializer reset to `0.0f`.
7. **Goldens, tests and docs** - `--dump-code` and export fixtures gain the tag
   comments (regenerate with `BUILD=debug`); the documentation sites in the table
   above are updated in the same commit, so no released text describes a format
   the tree no longer reads; and two existing test TUs move off the deleted API
   in the same breath, since `make test` must end green:
   `tests/test_repl_state.c:879` asserts `repl_example_consume_camera_header`
   returns 5 and installs a legacy bridge with `reset_import` /
   `try_consume_import_line`, and `tests/test_repl_core_io.c:1318` writes
   synthetic workspace `.c` files carrying the `g_angle` preamble and untagged
   transforms.

### Sequencing: the window from 4 to 7 is not pushable

Phases 2-4 land together; a half-migrated tree has two readers, which is the
state this plan exists to end. But the commit boundary has to be drawn wider
than that, because **phase 4 is where the corpus goes dark**:

- At phase 4 the old readers are gone and *no file carries tags yet* - phases 5
  and 6 do that. Every one of the 44 headers is therefore untagged, comes back
  `NOT_CAMERA`, and lands in the document as geometry. The whole corpus renders
  at the default pose with stray transforms in the body.
- Parity is restored at that instant, uniformly wrong but equal on both paths.
  So `k_known_divergent[]` must empty at **phase 4**, not phase 6.
- Example goldens and `--dump-code` fixtures break at the same moment, for the
  same reason, and are not repaired until phase 7.

So `make test` is red from phase 4 through phase 7 and the pre-push hook will say
so. **Phases 2-7 are therefore one pushable unit.** Keep them as separate local
commits if that helps review - the boundaries are real work boundaries - but
nothing between 2 and 7 goes to `origin` on its own, and phase 1 is the only
piece that lands independently green. What is *not* available is treating 5, 6
and 7 as follow-up PRs. Goldens regenerate exactly once, at the end, rather than
churning at 4 and again at 6.

## Tests

- **`test_camera_header_parity`** (new, registered in `TEST_BINS`) - for every
  `.glr` in `examples/scenes/` and `tests/scenes/stress/`, load via the catalog
  and as a file, then compare:
  - normalized source text of every document row, and the row count;
  - command structure - `CmdType` sequence and per-command arg values;
  - resolved camera pose (dist / rx / ry / pan) and the scene default, observed
    through the shared recording bridge stub;
  - `camera_comment_line`, which the reader now owns on both paths;
  - predef variable names and values;
  - the diagnostic list as ordered `(role, rule, line_no)` triples plus the
    overflow count, per the accumulator section.

  Comparing row counts alone would pass a file whose camera lines became
  geometry as long as the count matched, which is precisely the bug. Both loads
  run against the shared recording stub bridge from `tests/support/`; without a
  bridge installed there is no pose to compare.
- **`test_camera_header`** (new, `TEST_BINS`) - per-role parse plus every
  rejection rule: non-literal argument, `dist` with non-zero x/y, wrong rotate
  axis, trailing garbage after the call, duplicate role, a tagged `ry` whose
  argument is not a literal, a `spin` whose argument is not the bare `g_angle`
  token, tagged line below baseline depth, tagged line inside a snippet, tagged
  line after the snippet ends - the last three asserting the line is consumed
  *and* not inserted - and partial-pose `finish`: the merged pose keeps the
  destination value for every unseen role and one `note`-severity diagnostic per
  missing role, with **`spin` absent producing no missing-role diagnostic at
  all** since it is write-only. Severity is asserted through
  `repl_camera_rule_severity()` rather than a stored field.

  Also covered, because each is a live bug or a fresh rule rather than a
  variation:
  - **Both comment syntaxes for every role** (`// @camera dist` and the C89
    `/* @camera dist */`), and both marker syntaxes - `// camera` *and*
    `/* camera */`, the form `text_helpers.c:18` cannot read today.
  - **The marker returns `MARKER`, not `NOT_CAMERA`**, and its text reaches
    `camera_comment_line` from the catalog path as well as the file path.
  - **Both baselines**: raw depth 0 for a `.glr`, depth 1 inside a `display()`
    wrapper - including the split-brace form where `{` sits on its own line, and
    a tag in a *later* function at raw depth 1 after `display()` closed, which
    must be rejected rather than accepted against a stale baseline.
  - **A tagged line inside a `funcN` body**, which the parity test covers only
    indirectly.
  - **Order warnings**: roles out of canonical order, and a tagged line split
    from the block by an executable line - accepted, pose correct, one warning
    each.
  - **Diagnostic overflow**: more than `REPL_CAMERA_MAX_DIAGS` rejections in one
    load, asserting the stored list truncates and the overflow count is exact.
- **`code_brace_delta` fixtures** - a brace inside a `/* … */` comment on one
  line, a block comment spanning lines with a brace inside it, and a brace in a
  string literal, each asserting the depth counter is unmoved. These are exactly
  the inputs that would silently break region tracking in C89 files, and they are
  worth their own fixtures because the failure is a *mis-scoped tag*, several
  steps removed from the brace that caused it.
- **`spin` argument fixtures** - `g_angle`, `( g_angle )` and assorted
  whitespace spellings accepted, with a trailing comment after the call;
  `g_angle2`, `g_angle + 1` and a bare float rejected. The pose roles get the
  mirror-image set, including a negative literal, so `-15.0000f` is one token
  and not a sign followed by garbage.
- **`test_camera_apply_modes`** (new, `TEST_BINS`) - the recording stub bridge
  (shared, `tests/support/`) asserting each caller passes the mode it means: file
  load → `IMPORT` (snap, scene default adopted), `scene_snapshot` restore →
  `RESTORE` (snap, scene default *unchanged*), example and tutorial scaffold →
  `EXAMPLE` (ease, adopted). The `RESTORE` case is the one a two-mode API would
  have silently broken. `EXAMPLE` additionally asserts the external-3D-pose
  record happens exactly once per load, wherever phase 3 ends up putting it.
- Existing export/import round-trip tests - extended so a file whose transform
  numbers are edited by hand between export and import reads back the edited
  pose. That is the property the rejected one-line-directive design would have
  broken, so it is worth an explicit assertion. A second hand-edit case covers
  `g_angle`: rewriting its initializer must not move the imported pose, and must
  warn from `import.c` (the shared reader never sees that line). A third asserts
  the exported `.c`'s `/* camera */` marker survives a round-trip - it does not
  today.
- **Mid-ease load** - start an example ease, load a partial-pose file before it
  settles, assert the merged pose used the destination rather than the
  interpolated live value.
- `make check-state-ownership` - the reader lives in `src/repl/` and must not
  reach for `glr_camera_*` directly; `check-repl-demo-stubs-shrinking` should
  register the demo bridge getting smaller, not larger.

## Risks

- **Phases 2-4 are one commit, and 4-7 is one push.** Splitting 2-4 leaves two
  live readers, which is exactly the bug. Splitting 4-7 leaves the corpus with no
  camera at all and the goldens red - see "Sequencing" above.
- **The external-3D-pose record has a frame-timing contract that is already
  being broken** (`glr_actions.c:1053` calls the example load off the action
  path, `glr_camera_export.h:16` says display-frame only). Phase 3 has to decide
  where it lives rather than carrying it along inside `apply_pose`; carrying it
  is how it stayed invisible in the first place.
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
