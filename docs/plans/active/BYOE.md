# BYOE — bring your own editor

## Status

**Active.** D1-D8 below are **accepted decisions**, not open options: read the
whole "Decisions" section as instructions. Implementation follows the numbered
list in "Steps"; the per-step state is tracked here.

| Step | State |
|---|---|
| 0 — write D1-D8 in as decisions | done |
| 1 — `one-scene-loader.md` steps 1-3 | done |
| 2 — `source_path` + `repl_reload_active_scene_from_path()` | done |
| 3 — D3 metadata policy on `ReplSceneLoadOpts` | done |
| 4 — `glr_extedit.c`, `--watch` | done |
| 5 — `glr_extedit_notify_reloaded()` + D7 table | done |
| 6 — stage 2, one incomplete final row | done |
| 6.5 — stage 2.25, one recoverable failed row anywhere | **skipped — do not implement** |
| 7 — stage 2.5, live WIP buffer | done |

Both halves of step 7's precondition were satisfied before it was built. Stage
1 was used against a real editor, and the latency gate was measured rather than
estimated. The first pass timed the saved-file proxy; `bench_extedit` now
publishes `.wip` updates and asserts `wip_updates`. Native release, 200
content updates: p95 **5.1 ms** on the largest catalog scene against the 8 ms
budget, well under 0.5 ms on small and typical ones. The numbers and what
they imply for a per-keystroke path are under "Gate" below.

Stage 2.25 stayed skipped, and the reason held up: 2.5 receives an explicit
cursor row and needs a complete physical-to-document map anyway, so the
automatic first-failure recovery would have been throwaway work.

Where stage 2.5 landed, and the three places the implementation says something
the plan did not:

- **`ReplSceneRowMap`** (`src/repl/scene_load.h`) is the map plus the
  nonce-scoped `@cursor-hole` channel, requested through `ReplSceneLoadOpts`
  and off on every other load path. It needed **no fixup pass** over rows
  already recorded: the importer only ever appends, so feed order and document
  order cannot diverge even though read order and feed order do (staged
  function bodies, pending comments). Making the staged path record honestly
  meant giving `ImportStagedFuncLine` its physical row - which also fixed a
  warning about a rejected body line naming the flush point.
- **The removed row is the cursor row only when it is incomplete**, and stage
  2's trailing-row heuristic remains as the fallback when it is not. The plan
  says "the row to remove" as though it were unconditional; parking a *finished*
  command would take its geometry out of the scene while the user looks at it,
  and a row half-typed and then navigated away from would otherwise freeze the
  whole scene behind a row nobody is on.
- **Declining the recovery offer does not delete the `.wip`.** The plan's table
  says delete. Three reasons not to, in order of weight: the file is unsaved
  work from an editor that died, so it may be the only copy, and deleting it
  on a *dismissal* destroys it on the least deliberate keystroke available;
  gl-repl did not create it, and a watcher that deletes files it merely reads
  is a surprising thing to have on; and Esc has no commit callback, so
  implementing it would mean adding a cancel hook to the shared modal for one
  caller - with `glr_modal_cancel()` also being what runs after a *successful*
  commit, so the naive version would fire on accept too.

  Both load-bearing halves of that row survive without deleting anything:
  never auto-apply (the hold is set before the prompt opens, so declining is
  the default) and ignore it until its change token moves again. The cost is
  that a `.wip` nobody wants lingers until its editor next opens and closes
  that file - the plugin's `BufUnload`/`VimLeave` hooks are what clear it in
  the normal case, and they only failed to run here because the editor died.

  What the plan's version *would* have bought is not re-asking, and that has
  to be paid for separately: a decline is now remembered per path, keyed by
  the sidecar's bytes, so switching away and back does not re-ask. Keying on
  content rather than on the path alone is what keeps "ignore it until its
  change token moves again" true across bindings - the editor waking up and
  publishing changes the bytes, and that is a new question.

One gap the plan carried into the implementation, found by auditing stage 2.5
against stage 1 rather than against the plan: **returning to a scene whose
editor is still open re-offered the sidecar as a recovery.** It is the same bug
"Returning to a watched scene converges" fixed for the scene file, and it wants
the same answer - the per-path memory now records that this session has
followed a path's sidecar, and a bind to a path so marked resumes following
instead of prompting. Without it every F12 round trip during live editing put a
modal on screen.

One defect the build surfaced that no amount of reading would have: placing the
caret on a document row **must load that row into the input buffer**. An empty
input buffer sitting on an occupied row reads as a pending deletion to
`editor_input_has_uncommitted_change()`, which shuts the deferral gate - so
following the cursor stopped the following. Arrowing onto a row loads it; the
mirror has to do the same thing for the same reason.

Corrections made after review, kept here because each was a real defect the
first pass shipped. The behavioral cases have regression coverage; the
allocation-failure guard is explicit at the mutation boundary:

- **The applied stamp must come from the bytes the reload read**, not from the
  parked token. D8 says a deferred version is re-read when the gate opens, so
  the two can differ - and stamping the stale one made the watcher permanently
  deaf to a later, genuine save of exactly those bytes. A revert to
  already-known content also clears anything parked.
- **The parked row is removed, not truncated at.** It is the last row with
  *code*, so trailing comments and blanks sit after it and were being thrown
  away with it.
- **An over-long physical line is refused, not split.** `fgets` into
  `MAX_LINE_LEN` turned one long line into two short ones, atomically loading
  a document the path reader rejects outright.
- **`repl_scan_code_line` carries block-comment state.** `import.c` strips
  `/* ... */` upstream, so the scanner never needed it; the watcher scans *raw*
  physical lines and has no upstream, and a trailing C-style comment therefore
  had no terminator and was parked as a half-typed command. The two callers
  disagreeing about a comment is exactly what the shared header exists to
  prevent.
- **`--watch FILE --tutorial N` never bound.** The lesson had already parked
  the document in a slot-less transient before the watcher armed, and pinning
  an *empty* binding protected nothing. The pin now applies only once something
  is bound, and `--watch` seeds the CLI path outright.
- **The lesson-end token restamp is dropped, and D7 is amended to say so.**
  The rule assumed a design that ignores activity *without reading*; this poll
  reads and stamps on every change, lesson included, so the token is already
  current and restamping only swallowed saves arriving just after the lesson.
  The amendment is written into D7 itself rather than left as a note here, so
  the accepted contract and the code cannot be read as disagreeing.
- **A file that is *only* the half-typed row must still park.** Stage 2's
  actual first use - new `.glr`, one command typed, saved unfinished - left
  the loader nothing after the removal, and "no commands loaded" is an ATOMIC
  failure, so the save was refused. `ReplSceneLoadOpts.allow_empty` says the
  emptiness was a removal this code performed; the watcher sets it only when it
  actually removed a row, and a rejected line still fails the import. Every
  stage-2 fixture had complete commands before the tail, which is why the first
  suite missed it.
- **Returning to a watched scene converges.** Binding does not reload - the
  document usually already is the file, and reloading would clobber unsaved
  slot edits - but that reasoning fails on the way back: switch away, let vim
  save, switch back, and stamping the new bytes as applied buried the edit
  permanently. A four-entry per-path memory of what the document last came from
  separates "unchanged since we left it" from "moved while we were elsewhere".
- **A named file stays bound even when its import fails.** Startup has no
  stage-2 parking, so a new `.glr` holding only a half-typed command lands on a
  seeded New Scene - which used to have no path, so `--watch` lost the file it
  was told to watch. The empty scene is now bound to it, which also makes
  Ctrl+S write there rather than to a name-derived `<slug>.c`.
- **The per-path content memory must not evict.** A fixed ring sized to the
  user-scene catalog still lost history when a runtime `--examples-dir`
  catalog exposed more file-backed scenes than the ring could hold. Eviction
  reads as "never seen" - which stamps and does not reload, silently losing the
  next external save to that path. The watcher now keeps a growable,
  session-scoped map of every path it has seen, and the regression cycles past
  the old cap through a runtime catalog before saving the first file away from
  it.
- **A not-yet-created file is watchable.** `--watch new.glr` before the editor
  has written it: the bootstrap import fails, the seeded New Scene carries the
  path, and the first poll after the file appears picks it up.
- **One read per reload.** Hashing the file and then loading it left a window
  where a save between the two stamped an applied content describing bytes
  never loaded. The reader returns the bytes, their hash and the line pointers
  together; the path loader survives only as a fallback for a file too large to
  buffer, and re-hashes there.
- **A dismissal is remembered per file, not per binding.** `rebind()` wipes the
  live binding state, so switching away and back re-offered a version the
  user's own commit had already beaten. The suppression rides the per-path
  history now.
- **A missing relative path is resolved through its parent.** `realpath()`
  fails on a leaf that does not exist - the supported `--watch new.glr` case -
  and keeping the relative path left the binding at the mercy of the working
  directory.
- **The 2048-physical-line cap was unreachable for `.glr`, not a silent stage-2
  cutoff.** `MAX_EDITOR_COMMANDS` is 1024, so a `.glr` with more rows than that
  cannot load at all. The cap bit exported `.c` only, where it forced the
  fallback path and its separate hash. It is gone for that reason; no test
  accompanies it because no discriminating one exists, and a large-file test
  written first passed for the wrong reason (document capacity) and was removed
  rather than kept.
- **`REPL_DEMO_DEP_SRCS` needs a row for every new `src/repl/*.c`.** It is an
  explicit list; the binary and the tests use `$(wildcard)`, so a missing row
  is invisible to `make test` and to `check-c99`. `repl_demo` and
  `repl_live_demo` are gated by `test-full`'s `HEADLESS_DEMO_TARGETS` - run it.
- **An existing row edited down to empty is still dirty.** Empty input is clean
  only at an insertion/trailing row; on a document row it differs from the
  canonical text and must keep the reload gate shut.
- **File Open is later than the workspace binding.** A slot's `source_path`
  wins even when a managed workspace is already active. An explicit successful
  workspace save adopts all serialized slots and clears those older per-file
  homes at its commit point.
- **A reload settles and cancels camera control state.** Settle the ease first,
  then clear the held mouse button and momentum without changing the pose or
  scene default.
- **Undo-history capture failure refuses the reload.** Do not push when the
  heap-backed history copy cannot be allocated; otherwise a failed import can
  still clear redo or evict the oldest undo entry.

Post-review correctness on the 2.5 range (the feature was complete; these
were the remaining state/undo/mapping holes):

- **A dismissed WIP payload stays dismissed until its hash changes.** After
  a local commit or Ctrl+Z the poll used to drop one publication and clear
  `g_wip_payload_valid`, so a later `CursorMoved` of the same vim buffer
  looked like new content and overwrote the local document. A separate
  suppressed WIP hash now ignores both content and cursor updates for those
  bytes. Commit and undo are not the same pause: undo also drops the
  publication that first observed it; commit falls through so a genuinely
  new payload is followed immediately (D5).
- **A failed first WIP import must not mutate undo history.** The session
  snapshot was pushed before the atomic load; failure restored the document
  but left the push, clearing redo and evicting the oldest ring entry. The
  path now captures and restores the heap-backed history the way a failed
  saved-file reload already did.
- **Continuation rows belong on the row map.** A multi-line statement reset
  `line_no` to its first physical row, so every later code-bearing row
  stayed `REPL_ROW_MAP_NONE` and a cursor-only move onto it did nothing.
- **Parked WIP input is canonical.** An indented `    glVertex3f(1,` kept
  its leading whitespace, and live guides prefix-match at byte zero, so a
  typical partial vertex never grew a plane/line/point. Park through
  `repl_canonical_input_view` and subtract the stripped lead from vim's
  column.
- **The vim sidecar must resolve symlinks.** gl-repl binds through
  `realpath()`; the plugin only made the buffer name absolute, so a
  symlink scene published `<link>.wip` while gl-repl watched
  `<target>.wip`.
- **The latency gate times the shipped `.wip` path.** The first bench
  rewrote the saved file. It now publishes atomically and asserts
  `wip_updates`; `--saved` keeps the proxy.
- **A same-scene watched reload keeps the visible `t` binding.** The
  import initializer recreates `t` at 0; restoring through
  `repl_state_time_set()` would also reset the free-running `anim_time`.
  Play/pause already survived. Restore `t` only.
- **Sidecar cursor placement requests follow-scroll.** Moving the row
  and column without raising `editor_scroll_follow_cursor` left an
  offscreen caret invisible. Mapped rows and the parked hole request
  follow; an unmapped physical row still leaves the viewport alone.

Scope notes taken while implementing, so a later reader does not have to
re-derive them:

- **`one-scene-loader.md` steps 1-3 means exactly the three things the
  Prerequisite section names** - explicit format, the options struct, and
  `ATOMIC` over `SceneSnapshot`. That plan's step 3 also mentions the body-line
  cap and the presentation reset; those belong to its *catalog reroute* (its
  steps 4-6), have no caller until the reroute lands, and are not built here.
  `example_loader.c` keeps its own `.glr` walk and
  `test_camera_header_parity.c` stays green.
- **`REPL_CAMERA_APPLY_NONE` is honoured inside `repl_camera_header_finish()`**,
  not at the `import_finish_load()` call site. D3 warned that a `NONE` reaching
  `cam_apply_pose` would snap the camera; making the reader itself refuse the
  bridge call for `NONE` means no bridge implementation - present or future -
  can be handed the mode at all, while the header's own diagnostics
  (missing-role notes) still run.
- **D5's commit-vs-cancel distinction is derived, not hooked.** The plan
  assumed two router hooks (apply on cancel, dismiss on commit). The watcher
  instead records a document fingerprint when the gate shuts and compares it
  when the gate opens: unchanged means the row was abandoned, changed means the
  user committed. Same semantics, no ordering to get wrong and no call site to
  forget - and it handles "the user committed three lines while it waited",
  which a keystroke hook would have to enumerate. The lesson-end edge is
  detected the same way, from `tutorial_active()` going false during a poll.
- **Stage 2 parks only a single trailing row.** An unfinished statement absorbs
  every physical line after it, so one stray unclosed paren mid-file would make
  the whole tail "the incomplete row". The multi-row case is refused rather
  than stripped; ATOMIC then rejects the file and names the line, which is what
  tells the user something is wrong. This matches the stage's own title.
- **The cascade in stage 2's "Boundary" cannot occur** with the terminator
  rule: `{` and `}` *are* statement terminators, so a block-delimiter row is
  complete by construction and is never the selected row. Kept as an assertion
  (`test_block_rows_are_never_parked`) rather than a documented limitation.
- **The fixed-point test found nothing, which is the useful result.** The
  "Test impact" list asks for a document-wide export fixed point over the
  scene corpus, backing the claim that a watched file stops churning.
  `test_export_glr_fixed_point` (in `test_repl_core_examples.c`) exports each
  of the 40 catalog scenes to `.glr`, re-reads it through
  `repl_reload_active_scene_from_path` - the path a watched save takes - and
  re-exports; all 40 are byte-identical. So the friction is exactly one
  reformat, not per-save churn. (Its first run reported 15 "failures" that
  were the harness's own `declare_test_vars()` pre-declaring `x`/`y`/`n` and
  colliding with the scenes' declarations; the test now skips it, and that
  collision is worth remembering before adding any other round-trip walk.)
- **Verification 5 was resolved by reading, not by hand.** The partial-vertex
  guide draws: `draw_vertex_guides()` renders a plane at one filled slot and a
  line at two (`src/render3d/guides/geometry_guides.c`), and
  `tests/test_render3d_guides.c` already asserts both, labels included. The
  concern was that the "renderer fills unset slots with the identity" contract
  belonged to the *transform* guide - it does, and the vertex guide has its own
  DOF-based rendering instead. Stage 2's payoff is real.

## Goal

Let an external editor (vim, VS Code, …) be a peer author of the live scene,
in three planned stages:

1. **Write-triggered sync.** The external editor saves the bound file →
   gl-repl reloads it atomically. gl-repl writes back on explicit save only.
2. **One incomplete final row.** A `.glr` file may end with a half-typed
   command; gl-repl parks it in the live input row so the user continues
   typing there *and gets the edit-guide overlays*.
3. **2.5 — live WIP buffer.** vim publishes a plain-text sidecar on every
   keystroke and cursor move; gl-repl follows with no save required and no
   plugin, protocol, or IPC. The incomplete row becomes *any* row, named by
   cursor metadata, and `.c` becomes supportable.

**Real-time IPC (the former "stage 3") is not planned and is out of scope.**
See "Not this plan".

Premises:

- **Outbound is explicit-save only.** Ctrl+S stays the only writer.
- **The loader stays strictly atomic.** Any rejected line fails the whole
  import, live document untouched. The incomplete-row allowance is *not* a
  loader concession — the controller removes that row first.
- **Watching is a session mode, not scene config.** `--watch` only.

## Verdict

Stages 1, 2 and 2.5 have landed. Stage 2.25 stayed skipped: only its
hole-placement idea is reusable, while failure discovery and strict retry
are not needed once the sidecar names the active row. Post-review
correctness on the 2.5 range - dismissed-payload suppression, failed-WIP
undo restoration, continuation-row mapping, indented parked guides,
symlink-resolved sidecars, and a bench of the shipped `.wip` path - is
folded into the implementation notes below rather than opened as another
stage.

The friction that bites at every stage and has no cheap fix: **gl-repl
rewrites the text.** A UX cost to state clearly, not a bug to solve.

## Decisions

These were open questions in the previous draft. They are decided here so the
stages below can be read as instructions rather than options.

### D1 — Binding record

A slot gains a **binding record**: `{ origin_kind, format, resolved_path }`.
Watch and Ctrl+S use the *same* resolved path, with the writer selected by the
binding's `format`. A slot with no path is not watched.

| Origin | Resolved path | Ctrl+S |
|---|---|---|
| `./gl-repl --watch foo.glr` | new retained absolute `source_path`, set at bootstrap | **that same path** — not `repl_active_scene_export_path()` |
| Managed workspace slot | `join(workspace_dir, file_name)` | already `repl_save_active_scene` |
| `--examples-dir` `.glr` | existing `glr_origin_path` | existing write-back |
| Built-in example (compiled in) | none | unbind; status bar says so |
| F12 / scene tab | rebind to the new scene's path, else unbind | follows the bind |
| File → Open / Save As | rebind to the path just used | same |

`source_path` is a later per-slot choice than an already-active managed
workspace. It wins for watch and Ctrl+S until an explicit successful workspace
save adopts the slot; the workspace save clears `source_path` only after its
files and manifest commit.

Do **not** overload `glr_origin_path` for the CLI case: it is documented as
catalog `.glr` write-back, bound once and never rewritten (`scenes.h:155-165`).
Add `source_path` alongside it.

`--watch` is a boolean; the file stays the existing positional argument.

**Closing the CLI Ctrl+S gap is part of stage 1**, not a follow-up: today a
CLI-loaded document saves to `<slug>.c` / `output.c`, so without this the
self-write stamp would suppress the wrong file and the loop would break.

### D2 — Workspaces stay `.c`-only; stage 2 is `.glr`-only

Do not teach managed workspaces `.glr` in this plan — that is a separate
format/product change (manifest, prune rules, `workspace_io_has_c_ext`).

- **Stage 1** watches whatever file is bound, `.c` or `.glr`.
- **Stage 2** is **`.glr` only.** Its last-row heuristic is meaningless on
  exported C: the incomplete authored command sits inside `display()` and is
  followed by generated wrapper rows, so it is never the final non-empty
  physical row.
- **Stage 2.5 supports both**, because `@cursor` plus the physical→document
  mapping makes the row well-defined regardless of scaffolding.

### D3 — The watch path does not apply `@cfg` or `@camera`

Watched reloads replace **program text and variables only**; live cfg and
camera are preserved. Initial load and explicit Load Scene keep applying
headers as they do today.

Two payoffs: the undo unit becomes exactly what `EditorUndoSnapshot` already
stores, and BYOE stops depending on `one-scene-loader.md` step 4 (the
unresolved "may a file set anything, or only the scene subset?" question).
An external *text* edit is geometry, not a presentation reset.

**This needs a mechanism, not an omission.** Ignoring `ReplImportResult` is
not enough: `import_finish_load` applies the pending cfg bag and camera
*internally* before returning (`import_workspace_cfg_apply_and_reset`,
`REPL_CAMERA_APPLY_IMPORT`). Add an explicit metadata policy to
`ReplSceneLoadOpts` — `apply_cfg = 0`, `camera_apply = NONE` — as **option
fields, not a third load policy**. Declarations, function aliases and other
*program* metadata are still consumed normally.

Two traps in wiring those two fields:

- **`camera_apply = NONE` must skip the apply, not fall through it.**
  `ReplCameraApplyMode` today has only `IMPORT` / `RESTORE` / `EXAMPLE`
  (`camera_header.h:56-60`), and `cam_apply_pose` **snaps for anything that is
  not `EXAMPLE`** (`glr_camera_export.c:116-124`). A new `NONE` handed to that
  function would snap the camera and violate D3 outright. Either
  `import_finish_load` skips `repl_camera_header_finish` entirely, or `NONE`
  becomes an explicit no-op arm in the bridge *and* in every default-less
  switch over the enum.
- **`apply_cfg = 0` must still clear the pending bag.**
  `import_workspace_cfg_apply_and_reset` (`import.c:168-176`) applies *and*
  clears. Skipping the whole call leaves a stale accumulator for the next
  non-watch load to inherit. Skip the apply; keep the reset.

Also state it plainly: **`@scene-name` and `@workspace-dir` cannot rebind or
rename an already-bound watched slot.** A watched reload changes the program,
never the slot's identity or its binding record (D1). Otherwise an external
file could silently retarget the very path being watched.

### D4 — Undo policy, keyed on live identity

`editor_undo_note_wholesale_replacement()` (`src/editor/undo.h:138`) is **not
undo** — it clears both rings and bumps the generation so nothing survives.
Never use it here.

And **"always push" is wrong**, because `editor_undo_push_snapshot()` is the
auto-promotion hook: it calls `repl_promote_transient_if_needed()` before
saving (`src/editor/undo.c:241-249`). Pushing unconditionally would promote an
unedited `--examples-dir` scene into a user slot on the first vim save.

| Live identity | Inbound policy |
|---|---|
| User scene (`repl_active_user_scene() >= 0`) | `editor_undo_history_capture()`, push, ATOMIC reload. Failure → history restore. Success → one undoable clobber. |
| Unedited example / transient | Reload **without** `editor_undo_push_snapshot()`. The file is the source of truth and vim's undo is the undo. No promotion. |

Rollback must be lossless: `EditorUndoRingState` (`src/editor/undo.h:79-85`)
is head/count/generation only, so with a full ring a push has already
overwritten the oldest entry and restoring indices cannot bring it back. Use
`editor_undo_history_capture()` / `_restore()` / `_destroy()`
(`src/editor/undo.h:100-102`) — heap-backed, copies real entries, already used
by the tour baseline.

### D5 — A dirty input row defers the reload

`EditorUndoSnapshot` does not carry the in-progress input buffer (the header
lists the exclusion as deliberate), so a reload landing mid-edit destroys
typing that Ctrl+Z cannot recover.

**Policy: expose `editor_input_has_uncommitted_change()`; while it is true,
mark the inbound version *pending* and defer automatic application.** Status
bar shows that a newer external version is waiting. This preserves the
existing document-only undo contract without widening every snapshot.

`editor_input_has_uncommitted_change()` is now the single gate, with this
contract:

- `edit_line < count` → dirty iff the input differs from the canonical text of
  that document row, compared **through `repl_canonical_input_view()`**
  (`src/repl/text_helpers.h:19`). Not a raw `editor_buffer_line()` compare:
  document rows are stored with the trailing `;` while
  `editor_load_line_to_input()` strips it (`src/editor/input.c:414` uses
  exactly this view), so a raw compare would mark every normally loaded
  command dirty and deferral would never lift. Arrowing onto a row *loads*
  it and is therefore not dirty.
- `edit_line == count` (trailing row) → dirty iff `input_len > 0`, except a
  parked WIP row still owned by the watcher (below).

**Commit and cancel resolve the pending version differently**, and conflating
them is a bug:

| Event | Pending |
|---|---|
| Cancel / escape the input row | **Apply** — the row was abandoned |
| Commit `;` | **Dismiss** into a separate *suppressed-content* hash (see below) so the same bytes do not re-trigger |

Applying on commit would destroy the line just committed: both sides hold *A*,
vim saves *B* while the user is typing *L*, the user commits so the document
is *A+L*, deferral lifts, *B* lands and *L* is gone. Ctrl+Z could recover it
only on the user-scene path, and the user still watches their commit vanish a
frame later. Dismiss instead — the local document wins until vim saves again.

**Dismissal is a third state, not "successfully applied."** Advancing the
*content* token to the dismissed payload *B* would be wrong: the live document
is *A+L*, and the next `CursorMoved` would then look like a cursor-only update
(payload unchanged) and apply cursor/row mapping derived from *B* — which was
never loaded — onto *A+L*. Keep a distinct **suppressed-content hash**: while
the sidecar payload still equals dismissed *B*, ignore **both** content and
cursor updates. Resume only when vim changes the scene payload. *B* is never
recorded as applied.

**The parked WIP-row exclusion is conditional, not blanket.** Record the input
text (or an input revision) when the watcher parks the row; exclude it from
"dirty" **only while it is unchanged locally**. The first gl-repl keystroke in
that row transfers ownership back to the user and re-arms normal deferral —
otherwise the next sidecar update would overwrite local typing that undo
cannot restore.

*(The two reviews split on the base policy: the alternative was "file wins,
state it." That is simpler, but the point of this revision is not silently
destroying work, and an unrecoverable loss argues for care rather than
against it.)*

### D6 — Enablement is `--watch` only

No `GlrConfigKey`. Watching is a session/file mode like `--examples-dir`, not
a scene property. A config key would rewrite every example golden (via
`glr_export_cfg_fill_all`, `src/app/glr_actions.c:853`) and could travel in an
`@cfg` row, letting imported content enable its own watcher. Default off; the
web build ignores the flag and the TU stays inert.

A later non-exported session bit behind a File-menu action is fine, but not in
this plan.

### D7 — Transient state on successful reload

One controller notification, `glr_extedit_notify_reloaded()`. **Not** a
generation bump — that would make the undo snapshot just pushed unrestorable.

| Feature | Policy |
|---|---|
| Active tutorial or guided tour | **Defer** the reload (D5 mechanism), and **dismiss the pending version when the lesson/tour ends** — never auto-apply it |
| Replay | Stop — flat identity and `replay_exec_limit` do not survive a reflatten |
| `// @plot` | Force tag rescan through this notification, not `editor_undo_generation()` |
| Depth snapshot | Invalidate |
| Color picker, selection, autocomplete, active drag | Clear / cancel |
| Search | **Preserve the query**, rescan against the new document |
| Variable panel | Rebuild (what `repl_live_demo` already does) |
| Camera ease / drag | Settle, then leave the pose — D3 means no `@camera` is applied |
| Assign-plot series | Follow the `@plot` re-resolve; close if no tags |

**Do not call `glr_ctrl_reset_transients()` (`src/app/glr_ctrl.c:986`)
blindly** — it calls `glr_camera_settle_target()`,
`glr_camera_controls_reset()` and `glr_camera_clear_scene_default()`, which
clobbers exactly the camera state D3 preserves. Use a narrower cousin.

*(Reviews split on the tutorial row: the alternative was a hard refuse that
never queues. Deferral is kept because it still shows the user that an
external change is waiting, but the dismiss-on-end rule is mandatory — a
pending version computed against the pre-lesson document must never land on
the document `tutorial_end_keep_view` leaves behind. During a tutorial or tour
the sidecar is ignored outright; a tour and the watcher must not both mutate
one document.)*

**Ignoring is not enough — the observed tokens must not lag.** If sidecar and
base activity are ignored *without* advancing the observed tokens, the first
poll after the lesson sees that old movement as new and applies it, defeating
the dismiss-on-end rule.

**AMENDED after implementation — this is the accepted contract, and the code
matches it.** The original rule was "at tutorial/tour end, clear pending state
and stamp the current base and sidecar change tokens as observed". That is
right for a design which ignores activity *without reading it*. The
implementation does not: during a lesson the poll still stats, reads and stamps
`observed` on every change, and merely **defers** rather than ignoring — so the
token is already current at lesson end and the requirement is met
structurally. Re-stamping from disk on top of that is not a no-op, it is
harmful: it also swallows a save that landed between the lesson ending and the
next poll, discarding a real edit for having arrived at the wrong millisecond.

So: **at lesson end, clear the pending version and stamp nothing.** Live
following resumes on the next token movement, which now correctly includes one
that happened moments before the resume. Guarded by
`test_save_between_lesson_end_and_poll_is_not_swallowed` alongside the
dismiss-on-end test, so neither half can regress into the other.

**Stay bound across the lesson.** `tutorial_start` →
`repl_scenes_enter_transient_scene()` sets the active user scene to -1, so the
slot has no path. The binding must nonetheless remain the pre-lesson file —
otherwise the "external change waiting" status has nothing to point at. Do not
unbind. After the lesson the next genuine save of that file replaces whatever
the lesson left behind; that is ordinary watch behavior, not a leak.

### D8 — Two-level change gate

- **Change token** = nanosecond mtime (`st_mtimespec` / `st_mtim`) + inode +
  size. Not `time_t` seconds: a same-second final save would be missed
  *indefinitely*, the 2.5 "sidecar newer than scene file" rule becomes
  undecidable, and live WIP is impossible. Inode and size also catch the
  editor-style safe write (temp + `rename`) that may not move the timestamp.
- **Content token** = hash of the scene payload **with transport metadata
  removed**. Hashing the raw `.wip` bytes would defeat its own purpose: every
  `CursorMoved` rewrites the trailing `@cursor` line and changes the hash, so
  the cursor-only fast path would never fire. Keep a separate cursor token for
  row/column.

**One token is not enough state.** The watcher tracks three things:

- **last observed** — updated after a successful *read*, even when the parse
  then fails. Otherwise a malformed file is re-read and re-parsed every frame
  until the user fixes it.
- **last successfully applied content** — what the live document came from.
- **pending** — set while deferred (D5/D7). Retain only the newest token, and
  **re-read the file when the gate opens** rather than replaying stale bytes.

A successful same-path Ctrl+S clears any pending version and restamps all
three, so gl-repl's own write can never come back at it as an inbound change.

Missing or unreadable file: preserve the last good scene, report **once**, and
resume when the path reappears. Do not spam the status bar per frame.

The two levels are what keep the per-frame cost honest: the poll is a bare
`stat`; the file is only *read and hashed* once the change token moves; the
document is only *parsed* once the content token moves.

*(The prototype's `time_t` at `repl_live_demo.c:286` is a demo simplification,
not a design to inherit. This reverses an earlier call to ship seconds and
document the limit — it does not survive contact with 2.5.)*

## Prior art

`tools/repl_live_demo/repl_live_demo.c` is a working file-watching REPL host
built for this workflow — "The editor is external (vim, or anything). This
demo never edits text."

- `file_mtime()` (`:286`), `poll_active_file()` (`:461`), `watch_timer`
  (`:706`, 250 ms), `import_active_scene()` (`:334`).
- Its header (`:24-33`) is candid that reload is **not** transactional and a
  malformed save can replace a good scene. That is the gap this plan closes.

**Self-write suppression is a new requirement, not a trick to copy from the
demo.** `:382-383` stamps the mtime after `import_active_scene()` *reads* the
file, and the demo's writer targets a separate `.roundtrip.<ext>` file — it
never writes the watched path. Stage 1 must stamp after **every** successful
gl-repl write to the bound path: `glr_action_save_active_scene`, Save As, a
workspace save touching that leaf, and `--export-glr` to that path.

## Stage 1 — write-triggered sync

### Prerequisite

`one-scene-loader.md` **steps 1-3** — explicit format, the `ReplSceneLoadOpts`
struct, and `ATOMIC` over `SceneSnapshot`. Step 2 only carries `TOLERANT`
("today's importer behavior spelled out. Pure refactor"); **`ATOMIC` is step
3**, and that is what BYOE needs.

Step 3 must resolve: first-failure abort through the line feed (today
`import.c` warns and continues, `:268-291`, hard-failing only on read error,
over-long line, canonical-order violation `:2941-2977`, or zero commands
`:3045` — and on that last one `@cfg` and camera side effects are *already*
committed); `SCENE_SNAPSHOT_CAMERA_SNAP` on rollback, never `EASE`
(`scene_snapshot.c:135-148`); and restore of document, predefs and aliases.

**Step 4 is not needed** — D3 means the watch path never applies the pending
cfg bag. Do not add a third load policy or `@cursor` fields to that options
struct.

### Work

1. `source_path` on the slot (D1) + `repl_reload_active_scene_from_path()`:
   a sibling to `repl_load_scene_via_loader()` (`scenes.c:976-1025`) that
   reuses the **active** slot instead of allocating a fresh one and failing
   `ERR_NO_SLOT` at capacity.
2. CLI Ctrl+S writes `source_path` (D1).
3. `src/app/glr_extedit.c` — a thin poller: binding, change token, content
   token, post-write stamp, per-frame poll dispatching to the reload. Not
   `src/app/boot/`; `check-app-boot-band` forbids the controller including
   boot headers.
4. `glr_extedit_notify_reloaded()` + the D7 table.
5. `--watch` flag (`src/app/boot/glr_cli.h:34`, positional capture at
   `glr_cli.c:383`). Web build inert, TU non-empty for the C99 rule.

The poll goes in the `PROF_SCRIPTED_INPUT` region of `gl_repl.c:41-51`, before
`glr_ctrl_display_frame()`, and gets **its own `ProfSection`**
(`prof_sections.h`, rows in `src/app/glr_prof.c:106`/`:192`). CLAUDE.md states
that rule unconditionally for per-frame work in the host callback; one review
argued a bare `stat` cannot vanish into unattributed remainder and the section
is unnecessary, but the invariant is a project rule and the section costs an
enum plus two rows. In steady state it reads ~0, which is itself the useful
signal.

### The friction that cannot be designed away

Canonical text is regenerated from the parsed command via the spec `fmt`
(`src/repl/parser.c:472-580`), indentation re-derived from block scope rather
than the source line (`repl_source_scope_cmd_indent`,
`src/repl/source_scope.h:134-148`), and `repl_load_apply_line()` strips C float
suffixes (`src/repl/load.c:98-101`). The first Ctrl+S after an external edit
rewrites spacing, indentation and `1.0f` literals.

- **Fixed-point-after-one-pass is asserted for exactly one construct.**
  `tests/test_repl_core_io.c:851-859` asserts a byte-stable re-export inside
  the if/else-if/else round-trip block. That is not a document-wide guarantee.
  If BYOE promises the file stops churning, **that needs a new fixed-point
  test over the scene corpus** — it is in the test list below.
- `.glr` survives best (`src/repl/export_glr.c:36-53`; indent is
  presentation-only and round-trips exactly).
- Expressions with visible variables keep verbatim text (`preserve_expr` /
  `has_vars`, `src/repl/normalize.h:17-23`).

`repl_document_rebuild()` (`src/repl/replace.c:116`) is the model for the
transaction shape, **not** the entry point: it deliberately preserves cfg,
camera, scene name and workspace binding (`rebuild_reset_live`, `:38-48`).

## Stage 2 — one incomplete final row (`.glr` only)

### Why the loader cannot report the cursor row

`import.c` accumulates *physical* lines into a *logical* statement while
brackets are open (`:2900-2934`): `complete` requires
`depth <= 0 && is_stmt_terminator(last)`. An incomplete `glVertex3f(1,` leaves
`depth > 0` and **absorbs following physical lines**. The comment at
`:2685-2692` states the consequence outright — an unfinished statement "glues
the next physical line onto it, and reports one joined parse error — losing
the following row from the document."

So an ATOMIC loader reports a *joined logical statement*, never a physical
row. Stage 2 deliberately does not try to recover that case. Stage 2.5 avoids
guessing which row failed: the sidecar supplies the physical cursor row, and
the importer builds the complete mapping needed to place it.

### The framing that works

**The controller removes the designated physical row before the import; the
loader stays zero-tolerance.** On success the removed text goes into the live
input row; on failure nothing changes and the status bar says why.

Stage 2 needs **no row mapping at all**: after loading "lines minus last",
park at `edit_line = document_count` so the input row appends — already how
the code panel renders the trailing live row
(`repl_code_panel_add_trailing_document_row`). Do not insert a placeholder
document row.

Order matters: `editor_input_set_text()` ends by snapping the cursor to
end-of-text (`src/editor/state.c:364-368`), so set text **first**, then
`editor_cursor_pos_set()` if a column is wanted. Stage 2 has no column, so
end-of-text is the answer. `editor_state_edit_line_set` is `state.h:391`.

### Incomplete-row heuristic

Require **lexical evidence of incompleteness** on the last non-empty **code**
row, after trimming. Treat it as incomplete iff:

- bracket depth ≠ 0, **or**
- the last code character is **not a statement terminator** (`; { } :`).

**Promote `scan_code_line` (`import.c:2663-2682`), not just
`is_stmt_terminator`.** The heuristic needs *both* the running bracket depth
and the last code character, and depth is not a naive paren count:
`scan_code_line` skips string and char literals, stops at an unquoted `//`,
and counts `()` and `[]` but **not** `{}` (which delimit blocks). A `src/app/`
reimplementation would drift from the importer that has to agree with it.
Promote one helper returning `{depth, last, code_len}` — or an
`is_complete()` over them — and use it on both sides.

That same helper also settles "which row": a pure comment or directive line
yields `code_len == 0`, so **selecting the last row with code skips trailing
comments for free**. Without that, an ordinary trailing `// note` has no
terminator and would be parked as a half-typed command. Valid trailing
comments stay in the atomic document.

**The terminator is the test, not "a trailing comma or operator."** That
weaker rule misses the most common half-typed line of all:
`glVertex3f(1, 2, 3)` with the `;` not yet typed has depth 0 and ends in `)`,
so it would not be stripped — and it does not fail either.
`import_finish_load` flushes the accumulator at EOF (`import.c:2946-2947`),
and the parser *adds* the trailing `;` when rebuilding canonical text
(`parser.c:2432-2434`; the interactive `;` key never reaches the buffer, so
no-semicolon lines are normal input). The almost-finished command would
silently commit as a document row and stage 2's overlay payoff would never
fire for the case it exists to serve.

**A recognized command prefix alone is not sufficient** — too permissive. A
well-formed-but-wrong row (`glVertx3f(1,2,3);`) has a terminator, so it is not
stripped, ATOMIC rejects it, and the whole file is refused — which is correct.
Any *other* row failing ATOMIC refuses the file too.

Both `scan_code_line` and `is_stmt_terminator` (`import.c:2693`) are
file-statics in a `repl_*` TU while the heuristic runs in `src/app/`, so the
promotion above is a shared header, not a copy.

If the removed row is a block delimiter, the remainder unbalances and the
ATOMIC import fails, leaving the document alone. That falls out of the design;
document it as a known limitation and test it.

### Boundary

The incomplete row is not in the document, so an outbound write drops it.
Placement lives in `src/app/` — `check-no-load-line-to-input-in-pipeline`
forbids pipeline TUs from touching the input row (`src/editor/input.h:160`).

### What already works

- `editor_input_set_text()` (`state.c:364`), `editor_cursor_pos_set()`
  (`:404`, clamped), `editor_state_edit_line_set()` (`state.h:391`).
- Overlays read the **live input text**: `glr_ctrl_build_guide_snapshot()`
  (`glr_ctrl.c:560`) pulls `editor_state_input()`; `fill_guide_arg_slots()`
  (`:449-552`) prefix-matches with `strncmp` and needs no closing paren;
  `parse_arg_slots()` (`:402-431`) records a per-slot `filled[]` bitmask.
- **Unconfirmed:** the "renderer fills unset slots with the identity" contract
  is the **transform** guide's (`xform_filled`, `guides_shared.h:104-111`),
  *not* the vertex guide's. For vertex args the documented contract is only
  that `vertex_n_filled = 0` means "not a vertex call"
  (`guides_shared.h:88-96`). **Hand-verify before relying on it** — see
  Verification 5.
- Autocomplete follows for free (`ac_try_enum_slot_completion()`,
  `glr_completion.c:529`).

## Stage 2.25 — skipped; do not implement

The proposed intermediate stage would have guessed the first failed physical
row, removed it, retried the complete import, and parked it at a nonce-bearing
hole. It is not worth shipping as a separate recovery mode:

- failure discovery and the two-pass strict retry are substantial importer
  machinery that Stage 2.5 does not reuse—the sidecar names the row directly;
- proving one hole does not remove Stage 2.5's need for a complete
  physical-to-document map for later cursor-only moves; and
- an explicit save while a row is parked would rewrite the file without that
  row, adding another destructive edge to explain or block.

Keep only the useful design constraint: Stage 2.5 owns one typed, nonce-scoped
hole/mapping mechanism in the scene-import boundary, while `src/app/` owns the
sidecar policy, parked text and cursor placement. Genuine user comments that
look like transport markers remain ordinary text. Do not add automatic
first-parse-failure recovery or a second mapping path.

## Stage 2.5 — live WIP buffer

### Why not the swap file

Checked against the installed vim 9.2 documentation.

| Question | Answer |
|---|---|
| Can the format be changed? | **No.** Only `'swapfile'` (on/off, `options.txt:8483`), `'directory'` (location, `:3212`), `'swapsync'` (fsync, `:8505`), `'updatecount'`/`'updatetime'` (timing, `:9460`). |
| Documented as an API? | No — vim's internal block-paged memline structure. |
| Written live? | **No.** "updated after typing 200 characters or when you have not typed anything for four seconds" (`recover.txt:102-104`); at `updatecount=1` every keystroke costs a write **plus an `fsync`**. |
| Carries the cursor? | **No** — the disqualifier. Block 0 holds file identity, mtime, inode, dirty flag; cursor lives in shada/viminfo, written at exit. |

Also fatal: the swap updates "only if the buffer was changed, **not when you
only moved around**" (`recover.txt:104`).

### The sidecar

All four events confirmed (`autocmd.txt:1334-1352`, `:771-793`); the `I`
variants fire while still in insert mode.

**Publication must be atomic** — writing `<file>.wip` in place lets the
watcher observe it truncated. Write a sibling temp in the same directory and
`rename()` over it: a same-directory `rename(2)`, atomic, overwriting without
warning (`builtin.txt:9045-9051`). `writefile()`, `rename()` and `delete()`
are builtins (`builtin.txt:788`, `:518`, `:154`).

**Ship the full `s:GlrWip()`, not a stub**: read the whole buffer with
`getline(1,'$')`, append `// @cursor <line> <col>`, `writefile()` to
`<file>.wip.<pid>.tmp`, `rename()` onto `<file>.wip`, `delete()` the temp on
failure. Autocmds on `TextChanged,TextChangedI` and `CursorMoved,CursorMovedI`
for the buffer's file, plus `VimLeave,BufUnload` to delete the sidecar. Run it
before calling the snippet specified.

**Recovery is a bind-time question only.** "A `.wip` newer than the scene file
means recover, don't adopt" cannot be the steady-state rule: during live 2.5
the sidecar is *always* newer, because vim is typing and has not `:w`. As a
standing condition it would mean live WIP never runs at all. Split it:

| Moment | Behavior |
|---|---|
| Watch bind / startup | A `.wip` already present → "External WIP recovered", accept or discard. **Never auto-apply.** |
| Discard at that prompt | Delete the `.wip` and ignore it until its change token moves again (i.e. until vim starts publishing) |
| After bind | Any change-token movement on the sidecar is ordinary live follow |

A `.wip` alone is never a scene.

### Physical→document row mapping

Vim reports **physical file rows**. Exported C carries headers, wrappers,
staged functions and consumed directives; even `.glr` has non-document
metadata. Physical row *N* is not document row *N*, and the controller cannot
derive `edit_line` by subtracting a constant.

**But the controller has already removed row *N*, so the loader sees a
compacted stream with no marker there** and cannot map a row it never saw.

Mechanism: **substitute a transient consumed marker — `// @cursor-hole` — for
the removed row rather than deleting it outright.** The controller keeps sole
ownership of the user-facing `@cursor` directive (it must parse it *before*
the load, to know which row to remove); the marker is purely the loader's
channel for reporting where the hole landed. One owner each, no duplication.

Three constraints on the marker, all of which bite:

- **It must travel the staging path.** Recording the document index at the
  moment the marker is *encountered* is wrong for function rows, which the
  importer stages and emits later. The marker has to move through the same
  staging as the rows around it and record its index when that staged
  position is committed to final document order.
- **Consumption is scoped to the watch load path**, requested through a
  load option. Otherwise a genuine user comment reading `// @cursor-hole`
  would silently punch a hole in an ordinary File → Open.
- **Carry a per-load nonce** in the marker, so even on the watch path a
  user comment cannot be mistaken for transport metadata.

`// @cursor-hole` is a `//` line, which today becomes a `CMD_COMMENT` via the
raw-scene feed path. The importer must consume it *before* the feed, record
the row, and emit nothing.

**The hole alone is not enough — cache a complete row map.** The marker
resolves the row that was active during a *content* import. But the whole
point of the content token is that a cursor-only update **skips the import**,
and by then the cursor may name a *different* physical row for which no
mapping was ever produced. Without more, the fast path and `.c` support cannot
coexist: either every cursor move forces a reimport, or `.c` cursor moves land
on the wrong row.

So **each successful content import caches a physical→document map for every
physical row**, including an explicit classification for rows that have no
editable REPL row (headers, wrappers, staged-function scaffolding, consumed
directives). The hole marker supplies the entry for the removed row; a
cursor-only update just indexes the cached map. The map is invalidated by the
next content import.

`@cursor` itself must **not** go on `ReplImportResult` as a parsed directive,
and must never survive canonical export — unlike `@plot`, which rides a row's
text deliberately, cursor position is transport metadata for one snapshot and
exporting it would write editor state into the user's scene file.

Pin the semantics: **row is 1-based** (`line('.')`), **column is a 1-based
byte offset** (`col('.')`), both converted at the `src/app/` boundary to
gl-repl's 0-based `cursor_pos` and to the document row the loader returned.

### Undo at keystroke rate

**Do not reuse stage 1's undo policy.** The content token skips cursor-only
updates, but every content keystroke is still an inbound reload, and pushing
per reload would fill the 32-slot ring with vim keystrokes.

Push **at most one "entered live-WIP" snapshot** when the `.wip` first
appears, then update in place. Ctrl+Z exits WIP back to the last committed
gl-repl document — not backwards through vim's typing, which vim's own undo
already owns.

**D4's identity split applies here too.** "Push when the `.wip` first appears"
must not mean a bare `editor_undo_push_snapshot()`: that is the promotion
hook, so on an unedited `--examples-dir` scene the first vim keystroke would
promote it into a user slot — exactly what D4 exists to prevent. So: user
scene gets the one entry snapshot; transient/example gets none, unless a
deliberate ring-only, no-promotion helper is added for the purpose.

**WIP-session exit must be defined, or the state machine leaks:**

| Exit | Behavior |
|---|---|
| Ctrl+Z out of WIP | Restore the base version, clear WIP-active, **drop the publication that first observed the undo** (the sidecar may have been written before or after the key), and **suppress the dismissed payload hash** so a later `CursorMoved` of those bytes cannot undo the undo. Resume when the payload changes. |
| Local commit during WIP | Same suppressed-payload hash (D5). The observing publication is *not* dropped if it is already a new payload - that one is followed immediately. |
| Sidecar deleted (`VimLeave`) | Clear WIP-active, then **split on identity** (below) |
| Next WIP session | Captures a fresh entry snapshot, subject to the identity split above |

**Sidecar deletion cannot unconditionally retain the WIP text.** Retaining is
safe for an established user scene, but not for an unedited catalog/transient
scene: D4 deliberately created no slot and no undo entry, so a forced `:q!`
would silently convert discarded editor text into the active catalog scene
under an example identity, with its external source gone.

| Identity at deletion | Behavior |
|---|---|
| User scene | Retain the WIP text as local unsaved work, with a clear status |
| Transient / unedited example | **Reload the bound base file**, or require an explicit promotion — never silently adopt |

### Gate

**Measure the extedit section's total content-update latency** — not reflatten
alone. Reflatten excludes the file read and hash, the atomic import, the
snapshots, row mapping, variable-panel rebuild and the D7 notifications, which
together are most of the work.

Sample **only on actual content updates**, or steady-state zero-cost frames
dominate the distribution and p95 becomes meaningless. Measure representative
small, typical and large catalog scenes. Keep **~8 ms as the total main-thread
budget**; over that, do not ship 2.5. This is the one stage gated on a number
rather than on design.

#### Result — the gate passes

`bench/bench_extedit.c` (`make bench-extedit`; default is the shipped
`.wip` path, `--saved` keeps the stage-1 file-reload proxy). 2026-08-15,
macOS native release, 200 atomic sidecar publications per case. Cases are
the smallest, median and largest shipped catalog scenes by authored line
count, so the three rows keep meaning small / typical / large as the
catalog grows. `poll` is the extedit section alone; `frame` adds the
reflatten the reload forces onto the same frame, and is the number the
8 ms budget is about. The harness asserts `wip_updates` so a deferred or
failed publication cannot flatten p95.

| Case | Scene | Doc rows | p50 | **p95** | max |
|---|---|---|---|---|---|
| small | 2D assignment sketch (vars only) | 19 | 0.15 | **0.20** | 0.26 |
| typical | Annotated orbit plot (labels) | 54 | 0.39 | **0.44** | 0.51 |
| large | Orrery (labels track 3D orbits) | 447 | 4.81 | **5.09** | 7.6 |

An earlier measurement of the saved-file proxy sat at 5.54 ms p95 on the
same large scene; the shipped path is the same order of magnitude and
still inside the budget. `--saved` is there to re-take that proxy.

Two things the number does not say on its own:

- **Cost is dominated by document size, and the largest scene has ~1.5x
  headroom, not 8x.** Stage 2.5 pays this per keystroke rather than per `:w`,
  so on a 447-row scene a fast typist spends a third of every frame in the
  reload. Within budget, and worth knowing before anyone adds work to the
  path. The two-level gate is what keeps it survivable: a cursor-only sidecar
  update must stay off this path entirely, which is why the content token
  ignoring the `@cursor` line is a correctness requirement and not an
  optimization.
- **The import is the bulk of it** (`poll` is ~75% of `frame` on the large
  case). That is the ATOMIC re-parse of every row, which is inherent to
  "replace the document from the file" and is not something the sidecar can
  skip.

**Do not move the poll to a worker thread.** It is the obvious reading of a
5.5 ms main-thread cost and the split says otherwise: on the large case, open
+ read + FNV hash + newline split of the whole 19 KB file measures **0.046 ms
p50 / 0.049 ms p95** - under **1%** of the update. Everything else is
`repl_load_apply_line` writing the live command store, source document, predef
table, scratch arrays and func aliases, plus the reflatten - the same state the
frame is reading, so it cannot leave the main thread without a lock spanning
the whole REPL. Double-buffering the document into a shadow copy and swapping
would work in principle and means making every one of those stores swappable;
that is a far larger change than the 0.9% on offer, and this path is inside
budget. If large-scene keystroke cost ever does become the problem, the lever
is re-applying only the rows that changed, not concurrency.

One thing the measurement exposed that stage 2.5 has to fix: a successful
import writes `Loaded N commands from …` to stderr, which is right for a save
and wrong for a keystroke.

## Not this plan

Real-time IPC (socket transport, length-prefixed frames, a vim plugin) is
**not planned**. Recorded only so a later reader knows it was considered and
what was suggested: one-way vim→gl-repl; debounce of one import per frame
(the poll already is that); a `0600` Unix-domain socket; plugin location
undecided until 2.5 has users. Nothing in stages 1-2.5 should be shaped around
it.

Note for anyone who revisits: `src/` is **not** thread-free —
`src/app/glr_audio.c` runs a pthread worker with mutex and condvar (`:1050`,
`:1120-1123`, `:1184-1187`), so in-tree threading precedent exists. What does
not exist: sockets, kqueue, inotify, FSEvents, mkfifo, fork/exec.

## Steps

0. Write D1-D8 in as decisions. No code.
1. `one-scene-loader.md` steps **1-3** only.
2. `source_path` + `repl_reload_active_scene_from_path()`; CLI Ctrl+S writes
   `source_path`.
3. D3 metadata policy on `ReplSceneLoadOpts` (`apply_cfg = 0`,
   `camera_apply = NONE`) + the no-rebind rule.
4. Thin `glr_extedit.c`: two-level gate, three-state tracking, post-write
   stamp, per-frame poll, `ProfSection`. Undo per D4; defer per D5.
   `--watch` only.
5. `glr_extedit_notify_reloaded()` + the D7 table.
6. Stage 2: last-row strip, `.glr` only, append placement, terminator
   heuristic (`scan_code_line` + `is_stmt_terminator` promoted to a shared
   header; last **code** row, so trailing comments are skipped).
6.5. **Skipped — do not implement Stage 2.25.** Its automatic failure
   discovery and retry are not reused by 2.5. Carry only the typed hole and
   mapping constraints into step 7.
7. Stage 2.5 only after stage 1 has been used and the total content-update
   latency is measured: `@cursor-hole` staging + nonce, bind-time recovery vs
   live follow, WIP undo/exit policy, full `s:GlrWip()`.

## Test impact

All in failure and edge behavior, where the suite is thinnest.

**Where they landed.** `tests/test_scene_load.c` (132 assertions) covers the
loader options, the source-file binding and the row map / cursor hole,
including continuation-row mapping; `tests/test_glr_extedit.c` (456 native /
5 wasm) covers the watcher and the sidecar, including dismissed-payload
suppression, failed-WIP undo restoration, indented parked guides, and
cursor-only movement onto a continuation row; `test_export_glr_fixed_point`
in `tests/test_repl_core_examples.c` covers the last bullet. Everything in
the list below is covered.

- **Undo per identity.** User scene: reload is undoable. Unedited example:
  reload pushes nothing and **does not promote** — assert the slot count.
- **Lossless ring rollback at capacity.** Fill the ring, fail a reload, assert
  the oldest entry survives. The case `EditorUndoRingState` cannot cover.
- **Deferral.** Dirty input row → reload pends; **cancel applies it, commit
  dismisses it** and the same bytes do not re-trigger. A pending version is
  superseded by a same-path Ctrl+S. An untouched parked WIP row does not
  defer; a **locally edited** parked WIP row does.
- **D3 metadata preservation** across both successful and failed watched
  reloads: cfg, camera, scene name, workspace binding and `source_path` all
  unchanged.
- **Malformed file is not retried every frame** — assert the read/parse
  counters settle after one attempt.
- **Tutorial / tour.** Inbound defers; the pending version is **dismissed** at
  lesson end rather than landing on the leftover document; and **no reload
  occurs after the lesson without a further external edit** (the token-stamping
  case). The binding still names the pre-lesson file throughout.
- **Dismissed content suppresses cursor follow.** After a commit dismisses
  *B*, a subsequent cursor-only sidecar update must not apply *B*-derived row
  mapping to the live document; following resumes only when the payload
  changes.
- **Cursor-only move to an unmapped row.** With `.c` scaffolding, move the
  cursor to a row not active during the last content import and assert it
  resolves through the cached map — no reimport, no wrong row.
- **Failed reload leaves no trace** — document, history and input row
  untouched.
- **Change token.** Same-second double write; safe-write rename changing the
  inode; size-only change.
- **Content token.** Cursor-only sidecar update performs no import (assert a
  parse/reflatten counter, not timing). Assert the token ignores the `@cursor`
  line — this is the regression that would silently kill the fast path.
- **Stage 2 shapes.** Clean file; incomplete final row; **balanced final row
  missing only its `;`** (must be treated as incomplete — the case the weaker
  heuristic missed); well-formed-but-wrong final row (must fail); removed row
  is a block head, cascading (must fail).
- **`@cursor-hole` mapping.** A file with headers, `@declare` rows and staged
  functions: the returned document row is right, not off-by-header-count.
  Include a hole **inside a staged function body**. A genuine user comment
  spelling `// @cursor-hole` remains an ordinary comment; only the active,
  nonce-bearing transport marker is consumed. The staged-function case is
  driven off a *real* export rather than a hand-written fixture, so it cannot
  drift from what the writer emits.
- **WIP entry/exit.** Entry snapshot on a user scene but **not** on an
  unedited catalog scene (assert no promotion); Ctrl+Z out of WIP sticks and
  does not re-enter on the next poll; sidecar deletion clears WIP-active and
  **splits on identity** — a user scene retains the text, a transient reloads
  the base file rather than adopting it as the catalog scene. Plus the row of
  that table the first pass left untested: a *second* session takes its own
  entry snapshot, so its Ctrl+Z lands where **it** started rather than where
  the first session did.
- **D3 wiring.** `camera_apply = NONE` does not snap the camera (the
  `cam_apply_pose` fall-through), and `apply_cfg = 0` still leaves the pending
  cfg bag cleared for the next non-watch load.
- **Trailing comment is not parked.** A file ending in `// note` after a
  complete command loads whole, with the comment as a document row.
- **`@cursor` never exported.** Asserted at both ends - no document row
  mentions it, and Ctrl+S during a live session writes a file that does not
  either. True by construction (the line is stripped before the load), which
  is exactly why it is worth pinning: `@plot` deliberately rides a row's text
  through export, so "this one must not" is a rule nothing else enforces.
- **Sidecar atomicity**, and the honest reading of what a test can show here.
  A test drives its own publisher, so it goes on passing after the plugin
  regresses to an in-place write - which means the property cannot be tested,
  only guarded. Three pieces instead of one:
  `make check-wip-plugin-atomic` holds the plugin to a sibling temp plus
  `rename()`; a forty-publication run asserts every complete publication lands
  and the document never shrinks (named for that, not for atomicity); and a
  deliberately half-written sidecar asserts the backstop below.

  Writing that last one is what showed the rename is not
  belt-and-braces but the *only* protection: a torn file loaded **cleanly**,
  two rows adopted and the severed `glBegin(` parked as an ordinary half-typed
  row, because a prefix of a valid program is usually a valid program. The
  watcher now refuses a publication that has lost its `// @cursor` trailer
  after previously having one - the plugin writes it last, so its absence is
  the one cheap signal that a read landed mid-write. Conditional on having
  seen one, so an integration that never publishes a trailer is not refused
  outright; and still a heuristic, since a cut just after a complete trailer
  is indistinguishable.
- **D7 transients**, per row. D7 also reaches the *sidecar*, by a different
  route than it reaches a save: there is no parked version to dismiss, so the
  assertion is that a lesson observes-and-drops each publication, that nothing
  lands after the lesson either, and that following resumes only on a
  publication written afterwards.
- **A scene switch moves the sidecar with the binding** (D1). Switching to
  another *file-backed* scene rather than to a built-in example, because that
  is the case where a binding still exists afterwards and so actually tests
  re-derivation rather than teardown - the old sidecar goes unfollowed and the
  new scene's own is picked up.
- **Coming back to an open editor is not a recovery.** Switch away and back
  with the editor still running and the `.wip` still on disk: no prompt, and
  the document converges on the buffer. Asserted through a scene load with the
  watcher left armed - toggling it clears the per-path memory and would prove
  nothing.
- **New: document-wide export fixed point** over the scene corpus, backing the
  "the file stops churning" claim that `test_repl_core_io.c:851-859` only
  supports for the if-chain.

## Verification

Results for stages 1-2.5, with Stage 2.25 skipped, in the numbering below:

| # | Result |
|---|---|
| 1 | `make check-state-ownership` and `make check-trailing-whitespace` green. `--watch` needed rows in `scripts/completions/` too (`check-completions`). |
| 2 | `make test` / `make test-stubs` green (28,883 assertions), and every `HEADLESS_DEMO_TARGETS` demo builds - `repl_demo` / `repl_live_demo` are the load-bearing no-controller proofs and a new `src/repl/*.c` needs a `REPL_DEMO_DEP_SRCS` row they alone catch. `make test-web` links the watcher TU and `test_glr_extedit` passes there on its `__EMSCRIPTEN__` arm - it asserts the *inert* form rather than joining `WEB_TEST_EXCLUDE`. The lane's one remaining failure, `test_glr_init_trace`, fails identically at the pre-BYOE commit and is unrelated. |
| 3 | gracemont (gcc 13.3, Ubuntu 24.04): `make check-c99`, `make check-state-ownership` and `make test-stubs` green, including the `st_mtim` arm of the change token that macOS never compiles. |
| 4 | End-to-end, stage 2.5: real vim 9.2 driving the shipped plugin against the real binary - an unsaved buffer replaced the scene (green triangle from bytes never written to disk), the caret followed a cursor-only publication to `Ln 5:8`, and a round trip back onto the parked row restored `glColor3f(1, 1,` at `Ln 9:12`. Stage 1: `./gl-repl --watch scene.glr`, appended a `/* C-style note */`, four complete rows, a trailing `glVertex3f(3,` and a `// still typing` comment under it, all from outside; the session reloaded 7 -> 13 commands, kept both comments as document rows, and parked the incomplete row. Also `--watch new.glr` on a file holding nothing but `glVertex3f(1,`: the startup import fails (bootstrap has no parking), the binding survives, and the next save loads. `--watch` with no positional file exits 1 with a usage error. |
| 5 | Resolved by reading rather than by hand - see the scope note above. |
| 6 | Done - see Verification 6 above. |
| 7 | Done - `make bench-extedit && build/release/bench_extedit --iters 200`; results and caveats under "Gate". |

1. `make check-state-ownership` (includes `check-c99`, `check-include-style`,
   `check-app-boot-band`, …) and `make check-trailing-whitespace`.
2. `make test`, `make test-stubs`; `make test-web` must still link with the
   watcher TU inert.
3. Real GCC: `ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && make check-c99 && make test-stubs'`.
4. End-to-end: `./gl-repl --watch scene.glr`, edit in vim, save, confirm the
   scene updates; edit in gl-repl too, save from vim, confirm Ctrl+Z gets the
   gl-repl version back and that an unedited example was not promoted.
5. **Hand-verify the partial-vertex guide claim** before relying on it: type
   `glVertex3f(1,` without committing and observe what the guide draws. If it
   draws nothing, stage 2's user-visible payoff shrinks to autocomplete and
   the stage needs revisiting.
6. Run the full `s:GlrWip()` before shipping it — confirm per-keystroke and
   per-cursor-motion updates, a correct `@cursor` line, and no observable
   partial write.
7. Measure p95 **total content-update latency** in the extedit section,
   sampled only on real content updates, across small / typical / large
   scenes (2.5 gate; ~8 ms total main-thread budget). **Done** on the
   shipped `.wip` path - `make bench-extedit &&
   build/release/bench_extedit --iters 200` (the binary publishes the sidecar
   and asserts `wip_updates`; `--saved` is the file-reload proxy). Results and
   caveats under "Gate".
8. Re-read every `file:line` citation before landing.
