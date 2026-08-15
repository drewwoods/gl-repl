# BYOE — bring your own editor

## Goal

Let an external editor (vim, VS Code, …) be a peer author of the live scene,
in four escalating stages:

1. **Write-triggered sync.** The external editor saves the file → gl-repl
   reloads it, provided it imports cleanly. gl-repl writes back on explicit
   save only.
2. **One incomplete line.** The file may carry one incomplete final row;
   gl-repl parks it in the live input row with the cursor in it, so the user
   continues typing there *and gets the edit-guide overlays* for the
   half-typed command.
3. **2.5 — live WIP buffer.** vim publishes a plain-text sidecar on every
   keystroke and cursor move; gl-repl follows along with no save required and
   no plugin, protocol, or IPC. The incomplete row becomes *any* row, named by
   cursor metadata.
4. **3 — real-time IPC.** The same thing over a socket instead of the disk.

Premises:

- **Outbound is explicit-save only.** Inbound is automatic; Ctrl+S stays the
  only writer. Halves the conflict surface and avoids fighting vim's own
  "file changed on disk" prompt on every `;`.
- **The loader stays strictly atomic.** Zero tolerance: any rejected line
  fails the whole import, live document untouched. The incomplete-row
  allowance of stages 2/2.5 is *not* a loader concession — the controller
  removes that row before handing text to the loader. See "Stage 2".
- **An inbound reload is undoable, always.** See "Divergence".

## Verdict up front

- **Stage 1 is mostly assembly**, over two design problems that are not:
  path binding and undoable divergence. A working prototype of the inbound
  half already ships in the tree.
- **Stage 2 is a small delta**, once the row-removal framing above replaces
  the first draft's "allow one rejected line" framing, which was
  unimplementable (see "Why the loader cannot report the cursor row").
- **Stage 2.5 is the highest-value step and is smaller than stage 3.** Vim's
  `.swp` cannot serve it — evidence below, recorded so it is not re-litigated.
- **Stage 3 may not be worth building.** Once 2.5 lands, IPC only removes the
  disk round-trip. Speculative until 2.5's reflatten cost is measured.

The friction that bites at every stage and has no cheap fix: **gl-repl
rewrites the text.** A UX cost to state clearly, not a bug to solve.

## Prior art already in the tree

`tools/repl_live_demo/repl_live_demo.c` is a working file-watching REPL host
built for exactly this workflow — its header opens with "The editor is
external (vim, or anything). This demo never edits text."

- `file_mtime()` (`:286`) — plain `stat` + `st_mtime`.
- `poll_active_file()` (`:461`), driven by `watch_timer` (`:706`) on a
  `poll_ms` (default 250) GLUT timer.
- `import_active_scene()` (`:334`) — resets live state, picks the loader by
  extension, reflattens, rebuilds the variable panel.
- `:382-383` — after gl-repl writes the file itself it re-stamps the mtime so
  the watcher does not self-trigger. Stage 1 needs that trick verbatim.

Its header (`:24-33`) is candid that reload is **not** transactional and that
a malformed save can replace a good scene. That is the gap between the demo
and a shippable feature.

## Stage 1

### The watched path must be defined before anything else

**This is the first thing to resolve; the rest of stage 1 is meaningless
without it.** There is no "current file" in gl-repl today. What exists:

- CLI bootstrap retains the loaded *document* but **not its source path** —
  `opts.input_file` aliases argv and `GlrCliOptions` is a `main()` local
  (`gl_repl.c:257`), dropped after `glr_ctrl_bootstrap_repl()`.
- `repl_active_scene_export_path(ext)` (`scenes.h:153`) *synthesizes* an
  export destination from the scene slug. It is a save target, not a record
  of where the document came from.
- `glr_origin_path` (`scenes.c:80`) exists **only** for runtime-catalog `.glr`
  entries under `--examples-dir`, and is documented as bound once at promotion.

So the plan owes an explicit table, and it must cover the Ctrl+S target too,
since inbound and outbound have to name the same file or the loop is broken:

| Origin | Watched path | Ctrl+S target |
|---|---|---|
| `./gl-repl foo.glr` (CLI file) | needs a **new retained path** on the scene slot | ? |
| Managed workspace slot | manifest scene file | already `repl_save_active_scene` |
| `--examples-dir` entry | `glr_origin_path` | `repl_active_scene_glr_write_back_path()` |
| Built-in example (compiled in) | none — no file exists | promotion, then as a workspace slot |
| After F12 / scene switch | rebind to the new active scene, or refuse | follows the binding |

Recommendation: add a retained per-slot source path (generalizing
`glr_origin_path` beyond the `--examples-dir` case) and make BYOE bind to it,
refusing to watch when a slot has none. A scene switch rebinds; a scene with
no file unbinds and says so in the status bar.

**Unresolved, and it blocks the format recommendation:** managed workspaces
are `.c`-only (`workspace_io_has_c_ext`, and unlisted `.c` files are ignored),
but `.glr` is the format that survives canonicalization best, and the stage-2.5
vimrc snippet is written against `*.glr`. Either BYOE binds `.c` in workspaces
and accepts more churn, or workspaces learn `.glr`, or BYOE is scoped to
non-workspace files first. **Decide this before writing code**; it changes
which stage-1 path is even reachable.

### The prerequisite runs deeper than the first draft said

`one-scene-loader.md` steps 1-2 are **not** enough. That plan's own step list:
step 1 is explicit format, step 2 is the `ReplSceneLoadOpts` struct carrying
**`TOLERANT`** — "today's importer behavior spelled out. Pure refactor."
**`ATOMIC` is step 3.** BYOE depends through step 3.

And step 3 is not self-contained. Landing it requires resolving:

- **Parse-failure propagation** — today `import.c` warns and continues
  (`import_state_warn_parse_line`, `:268-291`) and only hard-fails on read
  error, over-long line, canonical-order violation (`import_finish_load`,
  `:2941-2977`) or zero commands loaded (`:3045`) — and on that last one the
  `@cfg` and camera side effects are *already committed*.
- **Camera-apply mode** on rollback — `SCENE_SNAPSHOT_CAMERA_SNAP` vs `EASE`
  (`scene_snapshot.h:39-42`); a rollback must use `SNAP` or "Reset camera" is
  silently redefined (`scene_snapshot.c:135-148`).
- **The cfg rollback boundary** — `SceneSnapshot` carries only the scene-subset
  `ReplConfigBag`, and `one-scene-loader.md` step 4 flags the "may a file set
  anything, or only the scene subset?" question as *explicitly unresolved*.
  BYOE inherits that question directly (see "What Ctrl+Z cannot restore").

### Divergence: an inbound reload must not destroy local edits

A *valid* external save can overwrite gl-repl-side edits made since the last
sync; atomic parsing only protects against malformed input. And
`editor_undo_note_wholesale_replacement()` (`src/editor/undo.h:138`) **is not
undo** — it clears *both* rings and bumps the generation precisely so nothing
survives. Using it here would make the clobber unrecoverable.

**Do not try to detect divergence with an edit tick.** The first draft
proposed one at `editor_undo_push_snapshot()`, on the strength of its "called
before any mutation" header comment. That comment is about *document*
mutations: there are only 18 call sites in `src/`, all in editor/repl/app
document paths, and the header states outright that input-buffer text,
selection, and scroll are **not** snapshotted. In-progress typing, undo/redo
navigation, camera movement and config changes all produce state an inbound
reload would affect without moving that tick.

**Policy instead: every inbound reload conservatively captures complete state
and is undoable.** No divergence heuristic to get wrong. The common case —
external editor is the sole author — is handled by the content token
(below): a reload whose content is unchanged is skipped entirely, so the
undo ring is not polluted by no-op saves.

#### What Ctrl+Z cannot restore, and what to do about it

`EditorUndoSnapshot` (`src/editor/undo.h:60-71`) carries commands, line text,
edit line, predef values/names, scratch arrays, func aliases, generation.
It does **not** carry cfg, camera, or the in-progress input buffer — the
header says the exclusions are deliberate.

So a naive undoable reload restores the old document **under the new file's
camera and presentation state**. Options:

1. **Strip `@cfg` / `@camera` on the watch path** (recommended). An external
   *text* edit rarely intends to change presentation, and this makes the
   undo unit exactly what the ring already stores. Costs: a file you exported
   with `@cfg` will not restore it via the watcher.
2. Keep a separate one-deep pre-reload `SceneSnapshot` (which *does* carry cfg
   + camera pose) restorable by a dedicated action.
3. Extend the ring's snapshot type — largest blast radius, affects all undo.

This interacts with `one-scene-loader.md` step 4; resolve them together.

#### Ring rollback must be lossless

If the reload itself then fails, the first draft rewound with
`editor_undo_ring_state_restore()`. `EditorUndoRingState`
(`src/editor/undo.h:79-85`) is **head/count/generation only** — indices, not
entries. With a full ring, the push already overwrote the oldest entry, and
restoring indices does not bring it back.

Use `editor_undo_history_capture()` / `_restore()` / `_destroy()`
(`src/editor/undo.h:100-102`) instead — heap-backed, copies the actual ring
entries, already used by the tour baseline for exactly this reason. Or
validate the load before pushing.

#### A successful reload needs a transient-state policy

A row-reshaping reload invalidates state keyed by row identity or execution
shape: tutorial progress, replay, `@plot` series, color picker, depth
snapshot, selection, scripted input. The plan must say, per feature, whether
it is **stopped, reset, preserved, or causes the reload to be refused**, and
route through the controller-owned replacement seams rather than each
subsystem discovering the change. `@plot` already has a precedent worth
copying: `glr_assign_plot_sync_tags()` re-resolves from tags on wholesale
replacement, keyed on `editor_undo_generation()`. An inbound reload needs an
explicit successful-reload notification that such consumers can hang off.

### Change detection

Use **nanosecond mtime (`st_mtimespec` / `st_mtim`) plus inode and size** from
stage 1. Not `time_t` seconds.

*(This reverses the first draft, which proposed shipping one-second `st_mtime`
and documenting the limit. It does not survive contact with the later stages:
a same-second final save can be missed **indefinitely** rather than briefly;
the sidecar "newer than the scene file" rule in 2.5 becomes undecidable at
one-second resolution; and live WIP updates are impossible. The prototype's
`time_t` is a demo simplification, not a design to inherit.)*

Inode and size also cover the editor-style safe write — write sibling temp,
`rename` over the target — which changes the inode while the timestamp may
not distinguish it.

Keep both tests regardless: same-second double write, and a safe-write rename.

Separately, track a **content token** (hash of the bytes read). A sidecar
whose content is unchanged must not trigger a parse + reflatten — this is what
keeps cursor-only updates cheap in 2.5, and what keeps no-op saves out of the
undo ring in stage 1.

### The rest of stage 1

1. **Reload into the *active* slot.** `repl_load_scene_via_loader()`
   (`scenes.c:976-1025`) allocates a *fresh* slot and fails `ERR_NO_SLOT` when
   all eight are full — right for "open a file as a new scene", wrong for
   "this scene's file changed". Needs a sibling reusing the active slot and
   keeping its name / `file_name` / source-path binding.
2. **A watcher.** New controller-band TU (`src/app/glr_extedit.c`) holding the
   bound path, change token, content token, polled once per frame from the
   `PROF_SCRIPTED_INPUT` slot region (`gl_repl.c:41-51`), before
   `glr_ctrl_display_frame()`. Not `src/app/boot/` — `check-app-boot-band`
   forbids the controller including boot headers. Needs its own `ProfSection`
   (`prof_sections.h`, rows in `src/app/glr_prof.c:106`/`:192`): the `stat` is
   cheap, the *reload* is not.
3. **Self-write suppression.** Every gl-repl write to the bound path re-stamps
   the stored token (`repl_live_demo.c:382`).
4. **Enablement.** A `GlrConfigKey` toggle (skill `gl-repl-config-toggle`;
   forces a `@cfg` line into all example goldens) plus a `--watch` CLI flag
   (`src/app/boot/glr_cli.h:34`, positional capture at `glr_cli.c:383`).
   Default off.
5. **Web build inert.** `#ifdef` the TU as `src/app/glr_web_io.c:8` does; keep
   it non-empty for the C99 rule.

### The friction that cannot be designed away

Canonical text is **regenerated from the parsed command** via the spec `fmt`
(`src/repl/parser.c:472-580`), with indentation re-derived from block scope
rather than the source line (`repl_source_scope_cmd_indent`,
`src/repl/source_scope.h:134-148`), and `repl_load_apply_line()` strips C
float suffixes on the way in (`src/repl/load.c:98-101`). So the first Ctrl+S
after an external edit rewrites the user's spacing, indentation and `1.0f`
literals.

Mitigations, stated honestly:

- **Fixed-point-after-one-pass is asserted for exactly one construct, not in
  general.** `tests/test_repl_core_io.c:851-859` asserts a byte-stable
  re-export — inside the if/else-if/else round-trip block. That is a narrow
  case, not a document-wide guarantee. *(The first draft cited `:765-800` and
  generalized it; both were wrong.)* If BYOE is going to promise the file
  stops churning, **that promise needs a new fixed-point test over the scene
  corpus**, and it belongs in this plan's test list.
- **`.glr` survives best** (`src/repl/export_glr.c:36-53` — indent is
  presentation-only and round-trips exactly) — subject to the unresolved
  workspace-format question above.
- Expressions with visible variables keep verbatim text (`preserve_expr` /
  `has_vars`, `src/repl/normalize.h:17-23`).

`repl_document_rebuild()` (`src/repl/replace.c:116`) looks like the right
primitive and **is not**: it deliberately preserves cfg, camera, scene name
and workspace binding (`rebuild_reset_live`, `replace.c:38-48`), so it drops
the `@cfg` / `@camera` headers an external file carries. Model for the
transaction shape, not the entry point.

## Stage 2 — one incomplete final row

### Why the loader cannot report the cursor row

The first draft said: allow exactly one rejected line, and require it to be
the cursor row. **That is unimplementable**, and `import.c` says so in its own
comment.

The importer accumulates *physical* lines into a *logical* statement while
brackets remain open (`import.c:2900-2934`): `complete` requires
`depth <= 0 && is_stmt_terminator(last)`. An incomplete `glVertex3f(1,` leaves
`depth > 0` and **absorbs the following physical lines** until something
closes at depth 0. The comment at `import.c:2685-2692` describes the exact
consequence: an unfinished statement "glues the next physical line onto it,
and reports one joined parse error — losing the following row from the
document."

So what an ATOMIC loader would report is a *joined logical statement*, not a
physical row, and "the one rejected line is the cursor row" cannot be
evaluated.

### The framing that works

**The controller removes the designated physical row before the import, and
the loader stays strictly atomic.**

- Controller takes the file text, extracts row *N*, hands `lines minus row N`
  to a zero-tolerance ATOMIC load.
- On success, it puts row *N*'s text into the live input row with the cursor
  in it.
- On failure, nothing changes; status-bar error.

This removes the need for any `partial_tail` or one-reject allowance in the
load options, keeps the loader's contract clean, and is the same mechanism at
both stages — only the choice of *N* differs.

### Which row, and why stage 2 stops at the last one

**Stage 2 designates the last non-empty row. Stage 2.5 designates the
`@cursor` row.**

This is forced: a normal external-editor save carries no cursor metadata, and
`@cursor` only exists once 2.5's sidecar does. Stage 2 must therefore be
editor-independent, and "the row being typed is the last one" is the only
defensible guess for a plain save. Arbitrary-row support arrives with the
metadata that makes it well-defined. *(The first draft put arbitrary-row
support in stage 2 while introducing `@cursor` only in 2.5 — a circular
dependency.)*

Guard against swallowing typos: only treat the final row as incomplete if it
*looks* incomplete — unbalanced brackets, or a prefix matching a known command
name. A well-formed-but-wrong final row (`glVertx3f(1,2,3);`) must still be an
error.

### When the removed row is structurally significant

Removing a `for(...) {` or a `}` unbalances the rest of the document, so the
ATOMIC import of the remainder fails, so the live document is left alone and
the status bar says so. That falls out of the design rather than needing a
special case, and it is the right outcome: live sync pauses while a block head
is half-typed and resumes when it balances. Document it as a known limitation;
cover it with a test.

### Boundary

The incomplete row is **not in the document**, so an outbound write drops it.
Consistent with outbound being explicit-save only. Placement must live in
`src/app/`, not a `repl_*` TU — `check-no-load-line-to-input-in-pipeline`
forbids pipeline TUs from touching the input row (`src/editor/input.h:160`).

### What already works

- Input buffer is programmatically settable: `editor_input_set_text()`
  (`src/editor/state.c:364`), `editor_cursor_pos_set(col)` (`:404`, clamped),
  `editor_state_edit_line_set()` (`src/editor/state.h:390`).
- Overlays read the **live input text**: `glr_ctrl_build_guide_snapshot()`
  (`src/app/glr_ctrl.c:560`) pulls `editor_state_input()`;
  `fill_guide_arg_slots()` (`:449-552`) prefix-matches with `strncmp` and
  requires no closing paren; `parse_arg_slots()` (`:402-431`) splits with
  `repl_scan_next_arg_delim()` and records a per-slot `filled[]` bitmask.
- **Correction to the first draft:** the "renderer fills unset slots with the
  identity" contract is the **transform** guide's (`xform_filled`, identity 0
  for translate/rotate and 1 for scale, `guides_shared.h:104-111`), *not* the
  vertex guide's. For vertex args the documented contract is only that
  `vertex_n_filled = 0` means "not a vertex call"
  (`guides_shared.h:88-96`); what a partially-filled vertex renders is not
  stated there. **Verify by hand before relying on it** — this is why the
  verification step below is not a formality.
- Autocomplete follows for free: `ac_try_enum_slot_completion()`
  (`src/app/glr_completion.c:529`) resolves the active slot from top-level
  commas before the cursor and already fires mid-line.

## Stage 2.5 — live WIP buffer, no plugin, no IPC

### Why not the swap file

Checked against the installed vim 9.2's own documentation.

| Question | Answer |
|---|---|
| Can the format be changed? | **No.** The only swap knobs are `'swapfile'` (on/off, `options.txt:8483`), `'directory'` (location, `:3212`), `'swapsync'` (fsync, `:8505`) and `'updatecount'`/`'updatetime'` (timing, `:9460`). No plaintext option. |
| Is it documented? | Not as an API. Vim's internal block-paged memline structure. Third-party parsers exist but track vim versions. |
| Is it written live? | **No.** "updated after typing 200 characters or when you have not typed anything for four seconds" (`recover.txt:102-104`). Tunable via `updatecount`, but at `updatecount=1` every keystroke costs a write **plus an `fsync`** (`'swapsync'` defaults to `"fsync"`). |
| Does it carry the cursor? | **No** — the disqualifier. `recover.txt` never mentions cursor; block 0 holds file identity, mtime, inode, dirty flag. Cursor lives in shada/viminfo, written at exit. |

Also fatal: the swap updates "only if the buffer was changed, **not when you
only moved around**" (`recover.txt:104`), so cursor-only motion produces no
event.

### The sidecar instead

All four events confirmed present (`autocmd.txt:1334-1352`, `:771-793`); the
`I` variants fire while still in insert mode.

```vim
autocmd TextChanged,TextChangedI *.glr call s:GlrWip()
autocmd CursorMoved,CursorMovedI  *.glr call s:GlrWip()
autocmd VimLeave,BufUnload       *.glr call delete(expand('%') . '.wip')
```

**Publication must be atomic.** Writing `<file>.wip` in place lets the frame
watcher observe it after truncation and before completion — spurious rejected
reloads, or a momentarily empty file. `s:GlrWip()` writes a **sibling temp in
the same directory** and `rename()`s it over the sidecar: a same-directory
`rename(2)`, atomic, and vim's `rename()` overwrites an existing target
without warning (`builtin.txt:9045-9051`). `writefile()`, `rename()` and
`delete()` are all builtins (`builtin.txt:788`, `:518`, `:154`).

Cursor rides as a trailing directive: `// @cursor 42 13`.

Stale-sidecar policy: the `VimLeave` / `BufUnload` autocmd removes it on clean
exit; on a crash it survives, so gl-repl **ignores a `.wip` whose change token
is not newer than the scene file's** (decidable only with the nanosecond
token — another reason stage 1 must not ship seconds), and never treats a
`.wip` alone as a scene.

### Cursor-only updates must not reimport

`CursorMoved` republishes unchanged text with new cursor metadata. Without a
content comparison every cursor movement costs a full parse + reflatten.
Compare the **content token** from stage 1 against the previous read: if the
text is identical, update cursor / input state only and skip the import
entirely. This is the single most important performance property of 2.5.

### File rows require an explicit document-row mapping

Vim reports **physical file rows**. The document is not the file: exported C
carries header directives, wrappers, staged function definitions and consumed
metadata, and even `.glr` can carry non-document lines. Physical row *N* is
not document row *N*, and **the controller cannot derive `edit_line` by
subtracting a constant.**

The loader must **return the document insertion row corresponding to the
physical cursor row** — it is the only component that knows which physical
lines produced document rows and which were consumed as metadata. Add that to
the load result alongside the existing metadata.

### Where `@cursor` lives — not in the document

`@cursor` looks like `@plot` and **must not be treated like it**:

- `import.c` must not call `editor_state_edit_line_set()` or
  `editor_cursor_pos_set()` — the same `src/app/` boundary stage 2 respects.
- Unlike `@plot`, cursor state must **not** survive canonical export. `@plot`
  rides a row's text deliberately, because a plot target must survive reformat
  and export. A cursor position is transport metadata for one snapshot;
  exporting it would write editor state into the user's scene file.

So the importer **consumes** `@cursor` into `ReplImportResult` — which already
carries exactly this kind of parsed-header metadata (`scene_name`,
`workspace_dir`, `src/repl/export.h:304-307`) — and the controller applies it.
Never retained on a row, never exported.

Semantics to pin explicitly: **row is 1-based** (vim's `line('.')`), **column
is a 1-based byte offset** (vim's `col('.')`), both converted at the
`src/app/` boundary to gl-repl's 0-based `cursor_pos` and to the document row
the loader returned. An off-by-one here is invisible until it is infuriating.

## Stage 3 — real-time IPC

**Correction to the first draft: `src/` is not thread-free.**
`src/app/glr_audio.c` runs a pthread worker with a mutex and condvar
(`:1050`, `:1120-1123`, `:1184-1187`). So there is in-tree precedent for a
background thread and its conventions — which makes a transport *more*
plausible than the first draft implied, not less.

What genuinely does not exist: sockets, kqueue, inotify, FSEvents, mkfifo,
fork/exec. The only external-process code is `popen("/usr/bin/pbpaste")`
(`src/app/glr_clipboard.c:101`). GLUT's loop is single-threaded, so either the
transport is non-blocking and polled from the stage-1 frame slot, or it
follows the audio worker's threading model and hands frames across a lock.

Stage 3 = transport + line protocol + vim plugin + a second consumer of the
stage-2 row-removal path.

**Do not build a shared-document / character-granular model.** gl-repl's
document is a parsed `GLCmd[]` with real structural invariants — declarations
hoist to the end of the declaration prologue, canonical order is validated,
indentation derives from block scope, `float x;` scope depends on the
enclosing `CMD_FUNC_DEF`. Per-keystroke synchronization of arbitrary
intermediate states is the opposite of the strict one-command-per-line
pipeline the REPL is built on. The tractable version is 2.5's: push whole
buffer + cursor, handle as a stage-2 load.

Open questions to record rather than answer: debounce policy; whether
gl-repl→vim ever pushes (canonicalization would move vim's cursor — recommend
one-way vim→gl-repl); transport choice; whether the vim plugin ships in-tree.

## Steps

0. **Decide the two blocking design questions**: the path-binding table
   (incl. workspace `.c` vs `.glr`), and the Ctrl+Z-cannot-restore-cfg/camera
   option. Neither is code.
1. `one-scene-loader.md` **steps 1-3** — explicit format, options struct,
   and `ATOMIC` over `SceneSnapshot` — plus its step 4 `@cfg` subset
   resolution where it touches the rollback boundary. Prerequisite.
2. Retained per-slot source path; active-slot reload sibling to
   `repl_load_scene_via_loader()`.
3. `src/app/glr_extedit.c`: binding, nanosecond+inode+size change token,
   content token, per-frame poll, `ProfSection`, self-write suppression.
   Conservative full-state capture; lossless ring rollback via
   `editor_undo_history_capture()`.
4. Successful-reload notification + the per-feature transient-state policy.
5. Enablement: config toggle + `--watch` (+ example-golden regen).
6. Stage 2: controller-side row removal, incomplete-row heuristic, `src/app/`
   input-row placement.
7. Stage 2.5: loader returns the document row for a physical row; `@cursor`
   into `ReplImportResult`; `.wip` overlay binding + stale rule; content-token
   short-circuit; the vimrc snippet, shipped only after it has been run.
8. Measure the reflatten cost. Then decide on stage 3.

## Test impact

All of it in failure and edge behavior, where the suite is thinnest:

- **Undoable reload.** Local edit, then a valid external write: reload
  happened *and* Ctrl+Z restores the pre-reload document. Assert what Ctrl+Z
  does *not* restore (cfg/camera) matches whichever option step 0 picked.
- **Lossless ring rollback at capacity.** Fill the undo ring, then fail a
  reload; assert the oldest entry survives. This is the case
  `EditorUndoRingState` cannot cover.
- **Failed reload leaves no trace.** Live document byte-identical, undo
  history unchanged.
- **Change token.** Same-second double write; safe-write rename changing the
  inode; size-only change. Assert each is detected.
- **Content token.** Cursor-only sidecar update performs no import (assert via
  a parse/reflatten counter, not by timing).
- **Stage 2 shapes.** Clean file; incomplete final row; well-formed-but-wrong
  final row (must fail); removed row is a block head, cascading (must fail,
  live doc intact).
- **Physical→document row mapping.** A file with headers, `@declare` rows and
  staged functions: assert the returned document row is right, not
  off-by-header-count.
- **`@cursor` boundary.** Import a document carrying `@cursor`, export it,
  assert the directive does not appear.
- **Sidecar atomicity.** Repeated writes while the watcher reads; no
  empty/truncated reload ever observed.
- **New: document-wide export fixed point** over the scene corpus, to back the
  "the file stops churning" claim that `test_repl_core_io.c:851-859` only
  supports for the if-chain.

## Verification

1. `make check-state-ownership` (includes `check-c99`, `check-include-style`,
   `check-app-boot-band`, `check-palette`, …) and
   `make check-trailing-whitespace`.
2. `make test`, `make test-stubs`; `make test-web` must still link with the
   watcher TU inert.
3. Real GCC: `ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && make check-c99 && make test-stubs'`.
4. End-to-end: `./gl-repl --watch scene.glr`, edit in vim, save, confirm the
   scene updates; edit in gl-repl too, save from vim, confirm Ctrl+Z gets the
   gl-repl version back.
5. **Hand-verify the partial-vertex guide claim** before relying on it (see
   the correction in Stage 2): type `glVertex3f(1,` without committing and
   observe what the guide actually draws. If it draws nothing, stage 2's
   user-visible payoff shrinks to autocomplete and the plan needs revisiting.
6. Run the vimrc snippet before shipping it — confirm `<file>.wip` updates per
   keystroke and per cursor motion, with a correct `@cursor` line and no
   observable partial write.
7. Re-read every `file:line` citation before landing.
