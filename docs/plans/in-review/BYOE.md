# BYOE — bring your own editor

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

Stage 1 is mostly assembly over two design problems — path binding and
undo/divergence — both now decided below. Stage 2 is a small delta. Stage 2.5
is the highest-value step and is gated on a measurement, not on new
architecture.

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

`editor_input_has_uncommitted_change()` does not exist today. Define it:

- `edit_line < count` → dirty iff the input differs from the canonical text of
  that document row. Arrowing onto a row *loads* it and is therefore not
  dirty.
- `edit_line == count` (trailing row) → dirty iff `input_len > 0`, except a
  parked WIP row still owned by the watcher (below).

**Commit and cancel resolve the pending version differently**, and conflating
them is a bug:

| Event | Pending |
|---|---|
| Cancel / escape the input row | **Apply** — the row was abandoned |
| Commit `;` | **Dismiss**, and advance the content token to the dismissed hash so the same bytes do not re-trigger next frame |

Applying on commit would destroy the line just committed: both sides hold *A*,
vim saves *B* while the user is typing *L*, the user commits so the document
is *A+L*, deferral lifts, *B* lands and *L* is gone. Ctrl+Z could recover it
only on the user-scene path, and the user still watches their commit vanish a
frame later. Dismiss instead — the local document wins until vim saves again.

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
row. "Allow one rejected line and require it to be the cursor row" was
unimplementable.

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

Require **lexical evidence of incompleteness** on the last non-empty,
non-directive row, after trimming. Treat it as incomplete iff:

- bracket depth ≠ 0, **or**
- the last code character is **not a statement terminator** — reuse
  `is_stmt_terminator` (`; { } :`, `import.c:2693`).

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

`is_stmt_terminator` (`import.c:2693`) is the right predicate to reuse but is
a file-static in a `repl_*` TU while the heuristic runs in `src/app/`; it
needs promoting to a shared header rather than duplicating.

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
| Ctrl+Z out of WIP | Restore the base version, clear WIP-active, and **ignore the sidecar until its change token moves again** — otherwise the next poll re-enters WIP and the undo does not stick |
| Sidecar deleted (`VimLeave`) | Clear WIP-active; keep the current document |
| Next WIP session | Captures a fresh entry snapshot, subject to the identity split above |

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
   heuristic (`is_stmt_terminator` promoted to a shared header).
7. Stage 2.5 only after stage 1 has been used and the total content-update
   latency is measured: `@cursor-hole` staging + nonce, bind-time recovery vs
   live follow, WIP undo/exit policy, full `s:GlrWip()`.

## Test impact

All in failure and edge behavior, where the suite is thinnest:

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
- **Tutorial / tour.** Inbound defers, and the pending version is **dismissed**
  at lesson end rather than landing on the leftover document.
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
  Include a hole **inside a staged function body**, and a marker-collision
  case where a genuine user comment spells `// @cursor-hole`.
- **WIP entry/exit.** Entry snapshot on a user scene but **not** on an
  unedited catalog scene (assert no promotion); Ctrl+Z out of WIP sticks and
  does not re-enter on the next poll; sidecar deletion clears WIP-active.
- **`@cursor` never exported.**
- **Sidecar atomicity.** Repeated writes while the watcher reads; no
  empty/truncated reload ever observed.
- **D7 transients**, per row.
- **New: document-wide export fixed point** over the scene corpus, backing the
  "the file stops churning" claim that `test_repl_core_io.c:851-859` only
  supports for the if-chain.

## Verification

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
   scenes (2.5 gate; ~8 ms total main-thread budget).
8. Re-read every `file:line` citation before landing.
