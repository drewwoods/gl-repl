# One scene loader

## Goal

Delete `load_example_lines()` from `src/repl/example_loader.c` and make
`src/repl/import.c` the single reader for both scene formats. After this, which
loader runs is decided by the *format* of the content, never by the route it
arrived on, and the catalog path and the file path cannot drift because there is
only one path.

The finished behavior is:

- one reader walks the lines, for `.glr` and exported `.c` alike;
- format is chosen from an explicit `ReplExampleSourceFormat`, never inferred
  from a filename suffix;
- atomicity (all-or-nothing vs. warn-and-continue) is an explicit caller
  choice, not an accident of which TU the caller reached;
- the presentation reset and the `@cfg` scene-subset filter apply on both
  routes, or on neither, by decision rather than by omission;
- `example_loader.c` keeps only the catalog choreography that is genuinely
  about *examples* - scene stashing, index bookkeeping, histogram reset - and
  owns no line-walking of its own.

## The bug this comes from

`shade-model-flat-smooth.glr` carries 24 lines with two statements on one row:

```c
glColor3f(1.00f, 0.35f, 0.20f); glVertex3f( 0.0f,  1.2f,  0.0f);
```

The REPL is one command per line, so `repl_parse_and_normalize_strict()` rejects
the tail of each. That is a scene bug and is not what this plan is about. What
this plan is about is that the *same bytes* produce two unrelated outcomes:

| Route | Result |
|---|---|
| `./gl-repl scene.glr` | 24 warnings, 33 commands loaded, scene renders (wrong) |
| `--examples-dir` then select | one error at body line 22, document wiped, nothing renders |

Same parse verdict, two error policies, because the route picked the loader.

## Where the split actually is today

It is **by format, not by source**, and it is already half-collapsed.
`load_example()` (`example_loader.c:288`) branches on
`repl_example_source_format()`:

- `REPL_EXAMPLE_SOURCE_C` → `load_example_c_source()` →
  `repl_export_load_from_lines()` - the catalog delegates to the importer;
- otherwise → `load_example_lines()` - the catalog's own `.glr` walk.

So the importer is *already* a catalog loader for one of the two formats, and
`repl_export_load_from_lines()` (`import.c:2891`) already takes exactly the
`const char *const *` the catalog stores. **Compiled-in example arrays are not
an obstacle to this change** - they are already fed to the importer today.

What each side does with a `.glr`:

| Concern | `example_loader.c` | `import.c` |
|---|---|---|
| per-line parse + apply | `repl_load_apply_line` | `repl_load_apply_line` (shared) |
| camera header | `camera_header.c` | `camera_header.c` (shared) |
| canonical doc order | always, via `repl_doc_order_offer` | same call, gated on `check_order` |
| `check_order` decided by | it is a `.glr` loader by construction | `.glr` filename-suffix sniff (`import.c:2581`) |
| `@cfg` header | filtered to the scene subset (`example_loader.c:102`) | whole pending bag applied |
| presentation reset | `repl_dispatch_example_presentation_reset(idx)` | none |
| tag-keyed cfg defaults | layered by example index | none |
| body-line cap | `EXAMPLE_BODY_LINES_MAX` = 512 | none (store capacity only) |
| parse failure | abort, `reset_example_load_state()` wipes | warn, skip line, continue |
| order failure | abort | abort (`import.c:2811`), caller cleans up |

The shared column is the work `done/camera-header-tags.md` already did. The
divergent column is what remains.

## The second bug: a failed load is a hard block in the F12 cycle

Independent of which loader runs, and worth fixing on its own.

`load_example()` (`example_loader.c:288`) stamps the catalog index *only on
success*:

```c
new_edit_line = load_example_lines(lines, idx);
if (new_edit_line <= 0)
    return 0;                                   /* early out */
repl_state_scenes_set_active_example_idx(idx);  /* not reached */
repl_scenes_detach_active_user_scene();         /* not reached */
```

`reset_example_load_state()` has already wiped the document by then. So a failed
load leaves `active_example_idx` pointing at the **last scene that loaded**,
while the live document is **empty** - the index and the document disagree.

`cycle_example_or_user_scene_dir()` (`glr_ctrl_router.c:559`) computes
`next = active_example_idx + direction`, so from a good scene N with a broken
scene N+1:

| Key | Computes | Result |
|---|---|---|
| F12 | N+1 | fails, index stays N, document wiped |
| F12 again | N+1 | fails again - **hard block, cannot advance** |
| Shift+F12 | N-1 | loads, i.e. two scenes back from where you appeared to be |

Which is exactly the reported symptom. The empty document also means no
`glClear`, so the previous frame smears - a cosmetic consequence of the same
disagreement, and acceptable as-is.

The standalone F12 fix deliberately uses **skip-on-failure**, not index
stamping. Each candidate load still emits its detailed loader diagnostic, but
the cycler continues through the remaining candidates in the same direction
and wraps without retrying entries it already examined. A failed load detaches
the active user-scene identity after the pre-load save, so a later candidate
cannot save the wiped document back over that slot. If every candidate fails,
the cycler restores the captured origin when possible and leaves an error
status; the last successful example index remains the scene identity.

This means a broken catalog entry is not a current F12 destination while it is
broken: from good scene N, F12 skips a bad N+1 and lands on the next loadable
scene; Shift+F12 applies the same rule in reverse. Direct Scene-menu selection
of the broken entry reports the loader error without selecting it. This is a
deliberate authoring/runtime trade-off for the standalone fix; a future
cycle-cursor design could instead make the errored entry current while keeping
`active_example_idx` reserved for successfully loaded scenes.

**The coverage gap is failure, not cycling.** Cycling itself is tested -
`tests/test_repl_editor.c:895` asserts F12 advances `active_example_idx` 0 → 1,
and `:3847` covers the example ↔ user-scene transitions in both directions.
(`test_glr_actions.c`'s `GLR_NEXT_EXAMPLE` hits are keymap string formatting,
not cycling.) What no test did was cycle across a catalog entry that fails to
load, especially from a user-scene origin. Every existing assertion was on the
success path.

That shape is why this survived: the tests confirmed the index advances when a
load works, but did not exercise the loader's document wipe or the active-slot
save that precedes a subsequent attempt.

So the test to write first is a cycle across a deliberately-broken entry,
asserting the behavior the standalone fix promises:

- a failed entry is skipped within the same F12/Shift+F12 action;
- the reverse direction skips the same entry without landing two scenes back;
- an all-fail cycle from a user scene preserves that scene's slot;
- transient all-fail scans do not retry the catalog and inflate the skip count.

It needs a catalog with a known-bad entry. `--examples-dir` + a fixture
directory under `tests/scenes/` is the cheapest route, and it doubles as
coverage for the runtime catalog path.

This is independent of the loader merge and can land first. It interacts with
one step: §2's `REPL_SCENE_LOAD_ATOMIC` must preserve the same failure-safe
scene identity and slot-save boundary, even though the standalone cycler does
not stamp failed entries.

## Design

### 1. Format becomes a parameter

`import_set_source()` sniffs `.glr` off the end of the source label to set
`check_order`. That works for a filesystem path and silently fails for anything
else: a catalog entry named `"Torus Knot"` gets `check_order = 0` and loses
canonical-order validation with no diagnostic. The catalog already stores the
answer explicitly as `entry->format` (`ReplExampleSourceFormat`,
`examples.c:933`).

Split the label from the format:

```c
void import_set_source(ImportState *s, const char *label,
                       ReplExampleSourceFormat format);
```

`check_order` becomes `format == REPL_EXAMPLE_SOURCE_GLR`. The file entry
points keep the suffix sniff, but *at the call site*, as the one place where a
path is all the caller has:

```c
int repl_export_load_from_file(const char *filename, ReplImportResult *result) {
    return import_load_path(filename, repl_scene_format_from_path(filename), result);
}
```

**This step is worth landing on its own**, independent of the rest: it removes a
silent validation dropout that exists today.

### 2. Load options replace the implicit policy

One options struct carries what currently differs by TU:

```c
typedef enum {
    REPL_SCENE_LOAD_TOLERANT,   /* warn per bad line, keep the rest */
    REPL_SCENE_LOAD_ATOMIC,     /* first bad line aborts, document restored */
} ReplSceneLoadPolicy;

typedef struct {
    ReplExampleSourceFormat format;
    ReplSceneLoadPolicy     policy;
    int  example_idx;      /* -1 = no catalog context */
    int  body_line_max;    /* 0 = store capacity only */
    int  cfg_scene_subset; /* filter @cfg to the scene subset */
} ReplSceneLoadOpts;
```

Both policies are already implemented, just in different files. `ATOMIC` is
`example_loader.c:247-251`'s abort-and-reset; `TOLERANT` is the importer's
existing warn counter. Neither is universally right, which is why this is an
option and not a merge:

- a shipped catalog entry that half-loads is a bug the author must see;
- a user's hand-edited file that loses 33 good rows to 24 bad ones is hostile.

`--examples-dir` is the awkward middle - authored content arriving by the
catalog route. It takes `ATOMIC`, matching today's behavior for that route.

Restoring the document on an `ATOMIC` abort is the one piece with no existing
implementation on the import side. The importer inserts rows as it walks, and
on `order_failed` it returns 0 leaving them in place for
`activate_new_scene_after_failed_import()` to clear. `example_loader.c` instead
calls `reset_example_load_state()` itself. Unify on the loader restoring, using
`SceneSnapshot` (`src/repl/scene_snapshot.h`) - the mechanism
`repl_document_rebuild()` already uses to make a failed find/replace leave no
trace.

### 3. Presentation reset and `@cfg` filtering move to the option

Both are currently `example_loader`-only, and both are load-bearing:

- CLAUDE.md's rule that *every* example load resets non-camera presentation to
  `CFG_DEFAULT_*` before applying the scene's own `@cfg`, plus tag-keyed
  defaults layered by example index;
- the scene-subset filter, which decides what a scene is *allowed* to change
  (`slug_is_scene_subset`, `glr_actions.c:972`).

The reset is straightforward: it is keyed on `example_idx`, so `-1` means no
reset and the option carries it.

**The `@cfg` filter needs a decision, not a mechanical move.** Today a catalog
scene may only touch the scene subset while a file may set anything in the
pending bag. One of those is wrong and I do not think it is settled which:
the permissive file behavior may be deliberate (opening a `.c` you exported
should restore what you exported) or may simply never have been considered.
Resolve this before writing code - it is the only step here that changes
user-visible semantics rather than relocating them.

### 4. `example_loader.c` keeps only the choreography

`load_example()` retains what is genuinely about examples and not about
reading: `repl_scenes_save_active_scene_if_any()`,
`repl_scenes_capture_pre_example_cfg_if_entering()`,
`repl_state_scenes_set_active_example_idx()`, the status line, and
`prof_histogram_reset()`. Its two format arms collapse into one call.

Public API is unchanged:

- `repl_load_example(int idx)` - 7 call sites in `src/`, ~30 in `tests/`;
- `repl_load_example_lines(const char *const *)` - kept as a thin wrapper over
  the new entry point with `example_idx = -1`, because
  `tests/test_repl_core_examples.c`, `test_camera_apply_modes.c`,
  `test_camera_header_parity.c`, `test_repl_flatten_differential.c`,
  `test_repl_state.c` and `tools/repl_live_demo` all call it. Rewriting those
  is churn that proves nothing.

`EXAMPLE_BODY_LINES_MAX` stays in `example_loader.h` and travels in the options
struct.

### 5. `bootstrap.c` stops being the odd one out

`repl_load_initial_commands()` (`bootstrap.c:60-86`) sends any non-directory
file argument to `repl_export_load_from_file()` regardless of extension - which
is how a `.glr` file gets the C importer's tolerant policy. With step 1 in
place it passes the resolved format, and the tolerant policy becomes a stated
choice for the CLI file route rather than a side effect.

## Test impact

`tests/test_camera_header_parity.c` is the casualty and it should be named
plainly: with one loader there is no second implementation to compare against,
so the test becomes tautological and is deleted at the end.

It is a good test - `done/camera-header-tags.md` records that it caught the
catalog loader eating the blank run between `@cfg` and the body on its first
run. So:

- **keep it green through every intermediate step.** It is the regression net
  for exactly this migration, and any step that makes it fail is a step that
  changed behavior on one route only;
- delete it only in the final commit, together with `load_example_lines()`;
- before deleting, port anything it asserts that no other test does. Its
  document-shape comparison (row text, `CmdType` sequence, arg counts) has no
  equivalent elsewhere; that becomes a single-load golden over the same corpus
  rather than a two-load diff.

### Coverage gaps this plan has to close

Both are gaps in **failure** behavior. The suite tests these paths thoroughly
when the load succeeds, which is why neither bug was caught.

1. **No test cycles across a failed catalog entry.** F12 cycling is covered
   (`test_repl_editor.c:895`, `:3847`) but only over scenes that load. The
   fixture-catalog coverage in §"The second bug" now checks skip-on-failure,
   user-scene slot preservation, all-fail diagnostics, and no duplicate scan.
2. **No test compares the two loaders' error policies.** `test_camera_header_parity.c`
   compares the loaders only on scenes that load cleanly - `parity_capture()`
   records a successful document. The whole tolerant-vs-atomic divergence sits
   outside it, which is how a scene could load one way and wipe the other with
   no test failing. Step 3 needs a direct test of each policy against the same
   deliberately-broken input.

Also affected:

- `tests/test_repl_core_examples.c` - the `bad_body` case (line 854) asserts the
  atomic abort. It should keep asserting it, now via
  `REPL_SCENE_LOAD_ATOMIC`, and gain a `TOLERANT` twin over the same input.
- `tests/test_glr_cli.c` - covers the CLI file route's warning output.
- `make test-scenes` (`REPL_SCENE_CORPUS=1`) - both corpora walk both loaders
  today via the parity test; after the merge they walk one.

## Steps

Each step is independently landable and leaves the parity test green.

0. **Failure-safe scene-cycle policy** (§"The second bug"), preceded by the
   fixture-catalog test. Keep the last successful scene identity, skip failed
   entries once per keypress, preserve user-scene slots, and keep all-fail
   diagnostics visible. Independent of everything below; land it first because
   it is the user-visible one.
1. **Explicit format.** Add `ReplExampleSourceFormat` to `import_set_source()`;
   the file entry points derive it from the path at the call site. No behavior
   change for files; fixes the silent `check_order` dropout for non-path
   labels. Ships alone.
2. **Options struct.** Introduce `ReplSceneLoadOpts` with `TOLERANT` +
   `example_idx = -1` + no body cap + no subset filter - i.e. today's importer
   behavior spelled out. Thread it through `import_begin_load`. Pure
   refactor.
3. **Implement `ATOMIC`** in the importer over `SceneSnapshot`, and add the
   body-line cap and the presentation reset. Cover each with a test against the
   importer directly, before any caller uses them.
4. **Resolve the `@cfg` subset question** (design decision, §3), then implement
   `cfg_scene_subset` to match.
5. **Reroute the catalog.** `load_example()` calls the importer for both
   formats with `ATOMIC` + `example_idx` + cap + subset filter.
   `load_example_lines()` becomes a wrapper. The parity test now compares the
   importer to itself and must be green trivially - if it is not, step 3 or 4
   is wrong.
6. **Delete** `load_example_lines()` and its helpers, port the parity test's
   document-shape assertions to a single-load golden, delete the parity test,
   update `docs/MODULES.md` and this plan's status.

## Risks

- **Step 3 is where the behavior lives.** Atomic restore over `SceneSnapshot`
  has to cover predefs, scratch arrays, func aliases and the source document,
  not just the command store. `repl_document_rebuild()` in
  `src/repl/replace.c` is the working reference for what a full restore needs.
- **The `@cfg` decision (step 4) is user-visible.** It is the only step that
  can change what an existing scene does. If it turns out both behaviors are
  wanted, the option stays and the plan's "or neither, by decision" goal is met
  by the option being explicit rather than by the behaviors converging.
- **Import is already 3036 lines** against `example_loader.c`'s 341. This adds
  to the larger file. The net is ~-250 lines and one fewer concept, but if
  `import.c` needs splitting afterwards that is a separate plan, not a reason
  to keep two loaders.
- **`repl_live_demo`** calls both entry points (`repl_live_demo.c:355,372`) and
  is the load-bearing proof that scene loading works without the controller.
  It must keep building and running at every step.

## Out of scope

- Fixing the scenes in the new general corpus that motivated this. Those are
  authoring bugs; they should be fixed and landed under
  `tests/scenes/general` so the corpus tests cover them, but that is
  independent work.
- Splitting `import.c`.
- Any change to the `.glr` or exported-`.c` formats themselves.

## Review feedback

The plan has a good diagnosis, but it is not implementation-ready without
resolving the following contract-level gaps:

1. **Failed-load index corrupts scene identity.** The proposed change to store
   the failed catalog index in `active_example_idx` conflicts with the rest of
   the app, which treats that field as a successfully active example. Scene
   tabs, window titles, recovery-save decisions, workspace promotion, and
   `repl_promote_transient_if_needed()` all rely on that meaning. A failed load
   would show an empty document as the failed example and could suppress
   recovery or promote edits under the wrong name. Add a separate cycle cursor
   or last-attempt field, or explicitly update every consumer.

2. **Camera behavior is missing from the options.** Catalog `.glr` loading
   currently finishes with `REPL_CAMERA_APPLY_EXAMPLE`, which eases the camera
   and records the 3D transition. The importer uses
   `REPL_CAMERA_APPLY_IMPORT`, which snaps instead. Rerouting catalog loads
   would silently change camera behavior. Add an explicit camera-apply mode to
   the load options and test it.

3. **`repl_load_example_lines()` behavior is contradicted.** The plan says
   `example_idx = -1` means no presentation reset and no subset filter, then
   makes the public wrapper pass `-1`. Today that wrapper resets the document,
   predefs, aliases, presentation settings, input state, and filters `@cfg`;
   existing tests depend on this behavior. Separate catalog context from
   example-load choreography, or preserve the wrapper's current setup
   explicitly.

4. **Explicit format is not propagated to all entry points.** Making only
   `import_set_source()` explicit leaves `repl_export_load_from_lines()` and
   `repl_export_load_from_stream()` without a format parameter. The importer
   also continues deriving raw-versus-exported behavior from snippet-marker
   scans. Route all three APIs through `ReplSceneLoadOpts`, with file suffix
   detection confined to the filesystem adapter.

5. **`ATOMIC` cannot currently abort at the first parse failure.**
   `import_feed_one_line()` converts failures into warnings and returns void,
   while `import_process_line()` discards handler results. Add an explicit
   failure status propagated through physical-line and staged-function paths.
   Also define the rollback boundary: `SceneSnapshot` only captures the
   scene-subset cfg, not unrestricted presentation cfg or full editor/input
   state.

6. **`.glr` metadata position will change unless constrained.** The current
   catalog loader only consumes a leading `@cfg` run; the importer accepts
   workspace directives through its general pre-snippet handler. A shared
   importer could therefore apply `// @cfg` appearing later in a raw `.glr`
   body. Preserve the leading-header rule or document and test the intentional
   semantic change.

7. **Body-cap semantics are underspecified.** The existing cap counts physical
   post-metadata `.glr` body lines; the importer processes logical accumulated
   statements and exported-C scaffolding. Specify whether `body_line_max`
   counts physical lines, logical commands, or translated scene rows, and
   whether it applies to `.c`.

The parity-test deletion also needs a fuller migration checklist. Its camera
diagnostics, canonical-order rejection, exported-C exemption, and failure-path
assertions should survive as direct tests, not only its document-shape
comparison.
