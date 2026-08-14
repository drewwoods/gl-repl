# BYOE — bring your own editor

## Goal

Let an external editor (vim, VS Code, …) be a peer author of the live scene,
in four escalating stages:

1. **Write-triggered sync.** The external editor saves the file → gl-repl
   reloads it, provided it imports cleanly. gl-repl writes back on explicit
   save only.
2. **One incomplete line.** The file may carry one line the parser rejects;
   gl-repl parks it in the live input row with the cursor in it, so the user
   continues typing there *and gets the edit-guide overlays* for the
   half-typed command.
3. **2.5 — live WIP buffer.** vim publishes a plain-text sidecar on every
   keystroke and cursor move; gl-repl follows along with no save required and
   no plugin, protocol, or IPC.
4. **3 — real-time IPC.** The same thing over a socket instead of the disk.

Premises, decided up front:

- **Binding = the active scene's file.** A managed `.glr-workspace` already
  stores each scene as its own file with a stable name; BYOE is sync of that
  directory, not a new file-identity concept.
- **Outbound is explicit-save only.** Inbound is automatic; Ctrl+S stays the
  only writer. That halves the conflict surface and avoids fighting vim's own
  "file changed on disk" prompt on every `;`.
- **Inbound is atomic.** A file the loader rejects is refused whole, with a
  status-bar error naming the failing line — apart from the single allowed
  incomplete line of stage 2. Today's importer is tolerant (warn + skip),
  which would silently delete rows of live work.
- **An inbound reload is undoable.** See "Divergence" below; this is the
  correction to the first draft's biggest error.

## Verdict up front

- **Stage 1 is cheap and mostly assembly**, with two real design pieces that
  are not assembly: an *atomic* import policy (already designed in
  `docs/plans/not-started/one-scene-loader.md` §2) and divergence handling. A
  working prototype of the inbound half already ships in the tree.
- **Stage 2 is a small delta on stage 1**, not a new subsystem. Partial-line
  parsing and the overlays it feeds already work — on the live input buffer,
  which is programmatically settable.
- **Stage 2.5 is the highest-value step and is smaller than stage 3.** Two
  lines of vimrc plus an atomic sidecar write; gl-repl reuses the stage-1
  watcher and stage-2 partial-line handling. Vim's `.swp` cannot serve this —
  evidence below, recorded so it is not re-litigated.
- **Stage 3 may not be worth building at all.** Once 2.5 lands, IPC only
  removes the disk round-trip: a performance refinement, not a new capability.
  Speculative until 2.5's reflatten cost is measured.

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
a malformed save can replace a good scene. That is precisely the gap between
the demo and a shippable feature, and it is the same gap `one-scene-loader.md`
exists to close.

## Stage 1 — write-triggered sync

### What already exists

| Need | Existing mechanism |
|---|---|
| Detect external write | `stat`/`st_mtime`, already used (`src/app/glr_paths.c:20`, `src/repl/scenes.c:1042`); pattern proven in `repl_live_demo.c:286` |
| Per-frame hook | the `PROF_SCRIPTED_INPUT` slot in `gl_repl.c:41-51`, right after `glr_frame_begin()` and before `glr_ctrl_display_frame()` |
| Read a file into the document | `repl_export_load_from_file()` (`src/repl/export.h:315`, impl `import.c:3124`) — applies `@cfg`, `@camera`, `@scene-name` headers too |
| Rollback on failure | `repl_load_scene_via_loader()` (`src/repl/scenes.c:976-1025`) — `SceneSnapshot` stash + `restore_live_from_stash`, also restoring `g_example_idx` / `g_active_user_scene` |
| Which file | `repl_active_scene_export_path(ext)` (`scenes.h:153`); `repl_active_scene_glr_write_back_path()` (`scenes.h:166`) for `--examples-dir` catalog scenes; `repl_workspace_dir()` (`scenes.h:197`) |
| Write it back | `glr_action_save_active_scene()` (`src/app/glr_actions.c:1405`), already the Ctrl+S path |
| Make the replacement undoable | `editor_undo_push_snapshot()` (`src/editor/undo.h:126`) — the ring push; `editor_undo_ring_state_capture/_restore` (`:118-119`) to rewind it if the load then fails |
| Report failure | `repl_set_status_error()` (`src/repl/host_effects.h:17`) |

### Divergence: an inbound reload must not destroy local edits

**This is the correction to the first draft.** A *valid* external save can
overwrite gl-repl-side edits made since the last sync; atomic parsing only
protects against malformed input, not against a well-formed file clobbering
good work. And the call the draft cited for this — `editor_undo_note_
wholesale_replacement()` (`src/editor/undo.h:138`) — **is not undo**: it
clears *both* rings and bumps the generation counter precisely so nothing
survives. Using it here would make the clobber unrecoverable.

Design:

- **Track local edits.** No document revision counter exists today
  (`src/repl/state_notify.h` has only dirty marks). Add a monotonic tick
  bumped at the existing every-mutation chokepoint,
  `editor_undo_push_snapshot()` — already documented as "called before any
  mutation" and already the auto-promotion hook
  (`repl_promote_transient_if_needed`). The watcher records the tick at each
  sync, in or out.
- **Diverged (tick moved since last sync) → reload is undoable.** Push one
  undo snapshot *before* the reload and do **not** call
  `note_wholesale_replacement()`, so Ctrl+Z restores the pre-reload document.
  This is exactly the find/replace model: one snapshot per whole-document
  transaction, ring rewound via `editor_undo_ring_state_restore()` if the
  rebuild fails (`src/repl/replace.c:162-171`), leaving no trace.
- **Not diverged → cheap path.** gl-repl's document is already the file's
  content; reload without a snapshot.
- CLAUDE.md's "wholesale replacement must clear undo first" rule does not
  apply: that exists so Ctrl+Z cannot restore *a different scene* into the
  current one. Here it is the same scene and the same file — restoring the
  prior document is the desired behavior, same as replace.

A status message should say which happened, so an undoable clobber is
visible rather than silent.

### Change detection resolution — a known, accepted limit

`struct stat.st_mtime` is `time_t`, i.e. **one-second** granularity, so two
writes inside the same second can be indistinguishable and the second one
missed until a later write. `st_mtimespec` / `st_mtim` (nanoseconds) exist on
both macOS and Linux and are the available upgrade.

**Decision: ship the one-second token, test and document the limit.** It is
not a showstopper for stage 1 (human save cadence) and it is a bounded,
well-understood behavior. It *does* mean the first draft's claim that polling
"caps WIP reloads at 60 Hz" was false — at stage 2.5 the effective inbound
rate can be as low as one update per second. Delete that claim; measure the
real rate as part of 2.5 and revisit the token then.

The test to write: two writes to the bound file within the same second, and
an editor-style safe-write (write sibling temp, `rename` over the target,
changing the inode) — assert what is detected and what is not, so the limit
is pinned rather than discovered.

### What else has to change

1. **Atomic load policy.** `import.c` warns per bad line and continues
   (`import_state_warn_parse_line`, `import.c:268-291`); it fails the whole
   file only on read error, over-long line, canonical-order violation
   (`import_finish_load`, `import.c:2941-2977`) or zero commands loaded
   (`:3045`) — and on that last one the `@cfg` and camera side effects have
   already been committed.
   → This is `one-scene-loader.md` §1-2: `ReplSceneLoadOpts` carrying an
   explicit `ReplSceneLoadPolicy { TOLERANT, ATOMIC }`, `ATOMIC` restoring
   through `SceneSnapshot`. **A prerequisite, not an optional cleanup** — BYOE
   on the tolerant loader silently eats the user's scene. Land its steps 1-2
   first; BYOE is the second consumer that justifies the split.

2. **Reload into the *active* slot.** `repl_load_scene_via_loader()` allocates
   a *fresh* slot and fails `ERR_NO_SLOT` when all eight are full — right for
   "open a file as a new scene", wrong for "this scene's file changed". Needs
   a sibling that reuses the active slot and keeps its name / `file_name` /
   `glr_origin_path` binding.

3. **A watcher.** New controller-band TU (`src/app/glr_extedit.c`) holding the
   bound path, the last change token and the last-sync edit tick, polled once
   per frame. Not `src/app/boot/` — it runs inside the frame loop and
   `check-app-boot-band` forbids the controller including boot headers. Per
   CLAUDE.md's frame rule and the note at `gl_repl.c:36-40` it needs its own
   `ProfSection` (enum in `prof_sections.h`, descriptor rows in
   `src/app/glr_prof.c:106`/`:192`). The `stat` is cheap; the *reload* is not,
   and that is the spike worth attributing.

4. **Self-write suppression.** Every gl-repl write to the bound path re-stamps
   the stored token (`repl_live_demo.c:382`), or Ctrl+S triggers an immediate
   reload of what was just saved.

5. **Enablement.** A `GlrConfigKey` toggle via skill `gl-repl-config-toggle`
   (one `g_cfg_items[]` row + storage in the two default-less switches in
   `glr_config.c`) — note this forces a `@cfg` line into all example goldens.
   Plus a CLI flag in `GlrCliOptions` (`src/app/boot/glr_cli.h:34`, positional
   capture at `glr_cli.c:383`) so `./gl-repl --watch scene.glr` works from
   launch. Default off.

6. **Web build is inert.** No filesystem, no watcher — `#ifdef` the TU the way
   `src/app/glr_web_io.c:8` does, and keep it non-empty for the C99 rule.

### The friction that cannot be designed away

gl-repl does not store the user's bytes. Canonical text is **regenerated from
the parsed command** via the spec `fmt` (`src/repl/parser.c:472-580`) with
indentation re-derived from block scope, not from the source line
(`repl_source_scope_cmd_indent`, `src/repl/source_scope.h:134-148`), and
`repl_load_apply_line()` strips C float suffixes on the way in
(`src/repl/load.c:98-101`). So the first Ctrl+S after an external edit
rewrites the user's spacing, indentation and `1.0f` literals.

Three honest mitigations:

- It is a **fixed point after one pass** — `tests/test_repl_core_io.c:765-800`
  already asserts `export∘import∘export == export`. The file churns once and
  then stays stable; not an every-save diff storm.
- **`.glr` survives best** (`src/repl/export_glr.c:36-53` — indent is
  presentation-only and round-trips exactly). Recommend `.glr` as the BYOE
  format.
- Expressions with visible variables keep verbatim text (`preserve_expr` /
  `has_vars`, `src/repl/normalize.h:17-23`), so the parts a user most cares
  about formatting are the parts that survive.

Also worth stating: `repl_document_rebuild()` (`src/repl/replace.c:116`) looks
like the right primitive and **is not** — it deliberately preserves cfg,
camera, scene name and workspace binding (`rebuild_reset_live`,
`replace.c:38-48`), so it would drop the `@cfg` / `@camera` headers an
external file carries. It is the model for the transaction shape, not the
entry point.

## Stage 2 — one incomplete line, at the cursor

**Cheaper than it sounds; the hard part is already built.**

- The input buffer is programmatically settable: `editor_input_set_text()`
  (`src/editor/state.c:364`) then `editor_cursor_pos_set(col)` (`:404`,
  clamped). Row index is separate: `editor_state_edit_line_set()`
  (`src/editor/state.h:390`).
- The overlays already read the **live input text**, not committed commands.
  `glr_ctrl_build_guide_snapshot()` (`src/app/glr_ctrl.c:560`) pulls
  `editor_state_input()`; `fill_guide_arg_slots()` (`:449-552`) prefix-matches
  with `strncmp` and **requires no closing paren**; `parse_arg_slots()`
  (`:402-431`) splits with `repl_scan_next_arg_delim()` and records a per-slot
  `filled[]` bitmask. `glVertex3f(1,` yields `vertex_filled = {1,0,0}` and the
  renderer supplies identity for unset slots
  (`src/render3d/guides/guides_shared.h:78-116`). **The requested behavior
  already happens today** for any text placed in the input buffer.
- Autocomplete follows for free: `ac_try_enum_slot_completion()`
  (`src/app/glr_completion.c:529`) resolves the active slot from top-level
  commas before the cursor and already fires mid-line.

### The rule

**The import allows exactly one rejected line, and it must be the cursor
row.** Not "the final line" — the first draft's trailing-line restriction was
wrong, because vim's cursor sits on any row of a longer file, so the
incomplete row is normally in the *middle* and an atomic loader would reject
the whole snapshot. That would have made stages 2.5 and 3 unable to reuse
stage 2 at all.

So:

- Zero rejected lines → ordinary atomic load.
- Exactly one rejected line, and it is the row named by `@cursor` → feed every
  other line, then park that row's text in the live input row with the cursor
  at the given column.
- Any other shape (two or more rejected lines, or one rejected line that is
  *not* the cursor row) → the whole import fails, live document untouched,
  status-bar error naming the first failing line.

That last rule is what keeps typos honest: `glVertx3f(1,2,3)` on a row the
cursor is not on is an error, exactly as today.

### When the cursor row is structurally significant

If the incomplete row is a block delimiter or a declaration — `for(i, 0,` or
`}` mid-retype — removing it unbalances the rest of the document, so the
following lines also fail to parse, so there is more than one rejected line,
so the import fails and the live document is left alone.

That falls out of the rule rather than needing a special case, and it is the
right outcome: live sync pauses while a block head is half-typed and resumes
on the next keystroke that balances it. **Document it as a known limitation**
— it is the one place where 2.5 visibly stalls — and cover it with a test so
it stays a deliberate behavior rather than a surprise.

### Boundary

The partial line is **not in the document**, so an outbound write drops it.
Consistent with outbound being explicit-save only. The placement itself must
live in `src/app/`, not a `repl_*` TU —
`check-no-load-line-to-input-in-pipeline` forbids pipeline TUs from touching
the input row (`src/editor/input.h:160`).

## Stage 2.5 — live WIP buffer, no plugin, no IPC

### Why not the swap file

Checked against the installed vim 9.2's own documentation.

| Question | Answer |
|---|---|
| Can the format be changed? | **No.** The only swap knobs are `'swapfile'` (on/off, `options.txt:8483`), `'directory'` (location, `:3212`), `'swapsync'` (fsync, `:8505`) and `'updatecount'`/`'updatetime'` (timing, `:9460`). There is no plaintext option. |
| Is it documented? | Not as an API. It is vim's internal block-paged memline structure. Third-party parsers exist but track vim versions. |
| Is it written live? | **No.** "updated after typing 200 characters or when you have not typed anything for four seconds" (`recover.txt:102-104`). Tunable via `updatecount`, but at `updatecount=1` every keystroke costs a write **plus an `fsync`** (`'swapsync'` defaults to `"fsync"`). |
| Does it carry the cursor? | **No** — the disqualifier. `recover.txt` never mentions cursor; block 0 holds file identity, mtime, inode, dirty flag. Cursor lives in shada/viminfo, written at exit. Stage 2.5 exists *for* the cursor. |

Also fatal: the swap updates "only if the buffer was changed, **not when you
only moved around**" (`recover.txt:104`), so cursor-only motion produces no
event.

### The sidecar instead

Two autocmds, no plugin, no protocol, plain text. All four events confirmed
present (`autocmd.txt:1334-1352`, `:771-793`), and the `I` variants fire while
still in insert mode — exactly the half-typed-line case.

```vim
autocmd TextChanged,TextChangedI *.glr call s:GlrWip()
autocmd CursorMoved,CursorMovedI  *.glr call s:GlrWip()
autocmd VimLeave,BufUnload       *.glr call delete(expand('%') . '.wip')
```

**Publication must be atomic.** Writing `<file>.wip` in place lets the frame
watcher observe it after truncation and before the write completes, producing
spurious rejected reloads or a momentarily empty file. `s:GlrWip()` writes a
**sibling temp in the same directory** and `rename()`s it over the sidecar —
a same-directory `rename(2)`, atomic, and vim's `rename()` overwrites an
existing target without warning (`builtin.txt:9045-9051`). `writefile()`,
`rename()` and `delete()` are all builtins (`builtin.txt:788`, `:518`,
`:154`).

The cursor rides as a trailing directive:

```c
// @cursor 42 13
```

Stale-sidecar policy, to be stated in the doc rather than left implicit: the
`VimLeave` / `BufUnload` autocmd removes it on a clean exit; on a crash it
survives, so **gl-repl ignores a `.wip` whose change token is not newer than
the scene file's**, and never treats a `.wip` alone as a scene. The watcher
binds `<scene>` and treats `<scene>.wip` as an optional overlay.

### Where `@cursor` lives — not in the document

`@cursor` looks like `@plot` and **must not be treated like it**. Two
differences decide the boundary:

- `import.c` must not call `editor_state_edit_line_set()` or
  `editor_cursor_pos_set()` — that is the same `src/app/` boundary stage 2
  already respects.
- Unlike `@plot`, cursor state must **not** survive canonical export as
  document text. `@plot` rides a row's text deliberately, because a plot
  target must survive reformat and export. A cursor position is transport
  metadata for one snapshot; exporting it would write editor state into the
  user's scene file.

So: the importer **consumes** `@cursor` into `ReplImportResult` (which already
carries exactly this kind of parsed-header metadata — `scene_name`,
`workspace_dir`, `src/repl/export.h:304-307`), and the controller applies it.
The directive is never retained on a row and never exported.

Nail down in the doc: **row is 1-based** (vim's `line('.')`) and **column is a
1-based byte offset** (vim's `col('.')`), converted at the `src/app/` boundary
to gl-repl's 0-based `cursor_pos`. Say it explicitly; an off-by-one here is
invisible until it is infuriating.

### The real cost

A keystroke-rate sidecar means a full reparse + reflatten per change.
Debounce is gl-repl's problem, and the inbound rate is bounded by the
change-token resolution, not by the frame rate — with one-second `st_mtime`
that is a *floor* on latency, not a cap on cost. **Measure before calling 2.5
done**; the number decides both whether the token needs upgrading and whether
stage 3 has any remaining value.

### What this does to stage 3

If 2.5 works, stage 3's only advantage is removing the disk round-trip and
moving debounce into the protocol — a performance refinement, not a new
capability. Mark stage 3 speculative-until-2.5-is-measured.

## Stage 3 — real-time IPC

Nothing in `src/` does IPC: no sockets, kqueue, inotify, FSEvents, mkfifo,
fork/exec or threads; the only external-process code is
`popen("/usr/bin/pbpaste")` (`src/app/glr_clipboard.c:101`). GLUT's loop is
single-threaded, so the transport must be non-blocking and polled from the
same per-frame slot stage 1 adds.

So stage 3 = a transport (unix socket or FIFO, both new to the tree), a line
protocol, a vim plugin, and a second consumer of the stage-2 loader.

**Do not build a shared-document / character-granular model.** gl-repl's
document is a parsed `GLCmd[]` with real structural invariants — declarations
hoist to the end of the declaration prologue, canonical order is validated,
indentation derives from block scope, `float x;` scope depends on the
enclosing `CMD_FUNC_DEF`. Vim's document is bytes. Per-keystroke
synchronization of arbitrary intermediate states is the opposite of the strict
one-command-per-line pipeline the REPL is built on. The tractable version is
the same one 2.5 uses: push whole buffer + cursor, handle as a stage-2 load.

Open questions to record rather than answer: debounce policy; whether
gl-repl→vim ever pushes (canonicalization would move vim's cursor — recommend
one-way vim→gl-repl); transport choice; whether the vim plugin ships in-tree.

## Steps

Each step is independently landable.

0. `one-scene-loader.md` steps 1-2 — explicit format, `ReplSceneLoadOpts` +
   `ReplSceneLoadPolicy` with `ATOMIC` restoring through `SceneSnapshot`.
   Prerequisite.
1. Local-edit tick at `editor_undo_push_snapshot()`, plus the active-slot
   reload sibling to `repl_load_scene_via_loader()`.
2. `src/app/glr_extedit.c`: bound path, change token, last-sync tick,
   per-frame poll, `ProfSection`, self-write suppression. Divergence →
   undo snapshot; not diverged → cheap reload.
3. Enablement: config toggle + `--watch` CLI flag (+ example-golden regen).
4. Stage 2: one-rejected-line-at-the-cursor allowance on the load options,
   and the `src/app/` input-row placement.
5. Stage 2.5: `@cursor` into `ReplImportResult`, controller applies; `.wip`
   overlay binding and stale-sidecar rule; the vimrc snippet, shipped only
   after it has been run.
6. Measure the reflatten cost. Then, and only then, decide on stage 3.

## Test impact

New coverage this needs — all of it in **failure and edge** behavior, which is
where the existing suite is thinnest:

- **Divergence.** Local edit, then a valid external write: assert the reload
  happened *and* Ctrl+Z restores the pre-reload document. Assert the
  not-diverged path pushes no snapshot.
- **Failed reload leaves no trace.** A rejected external file: live document
  byte-identical, undo ring state unchanged (the `replace.c` model, testable
  the same way).
- **Change-token limits** (§"Change detection resolution"): same-second double
  write, and a safe-write rename that changes the inode. Pins the accepted
  limitation.
- **Stage 2 shapes.** Zero rejected lines; one at the cursor row; one *not* at
  the cursor row (must fail); two or more (must fail); cursor row is a block
  head, cascading to multiple rejects (must fail, live doc intact).
- **`@cursor` boundary.** Round-trip a document that arrived with `@cursor`
  and assert the directive does **not** appear in the export.
- **Sidecar atomicity.** Repeated writes while the watcher reads; assert no
  empty-file or truncated-file reload is ever observed.
- Stage 2's overlay claim is already covered by existing guide tests once the
  input row is set; add a case that sets the input row programmatically rather
  than via keystrokes.

## Verification

1. `make check-state-ownership` (includes `check-c99`,
   `check-include-style`, `check-app-boot-band`, `check-palette`, …) and
   `make check-trailing-whitespace`.
2. `make test` plus `make test-stubs`; `make test-web` must still link with
   the watcher TU compiled inert.
3. Cross-check under real GCC: `ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && make check-c99 && make test-stubs'`.
4. End-to-end by hand: `./gl-repl --watch scene.glr`, edit in vim, save,
   confirm the scene updates; edit in gl-repl too, save from vim, confirm
   Ctrl+Z gets the gl-repl version back.
5. Sanity-check the stage-2 overlay claim by hand before relying on it: run
   `./gl-repl`, type `glVertex3f(1,` without committing, confirm the guides
   render with the two unset slots at identity.
6. Sanity-check the vimrc snippet by running it — confirm `<file>.wip` updates
   per keystroke and per cursor motion, with a correct `@cursor` line and no
   observable partial write. Ship a snippet that has been run.
7. Re-read every `file:line` citation in this doc against the tree before
   landing; a doc naming drifted line numbers is worse than one naming none.
