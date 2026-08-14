# BYOE (bring your own editor) — feasibility write-up

## Context

gl-repl owns its text. The editor buffer, the canonical line text, the commit
chain and the cursor are all internal, and the only way source enters or
leaves is a whole-file import/export. The ask is to let an external editor
(vim, VS Code, …) be a peer author of the same scene, in three escalating
stages:

1. **Write-triggered sync.** External editor saves the file → gl-repl reloads
   it, provided it imports cleanly through the existing path. gl-repl writes
   back on explicit save.
2. **Partial trailing line.** The external file may end with one incomplete
   line (`glVertex3f(1,`); the importer parks it in gl-repl's live input row
   with the cursor at end-of-line, so the user continues typing there *and
   gets the edit-guide overlays* for the half-typed command.
3. **Real-time editor integration** (vim). Bidirectional sync with no file
   round-trip.

**This session lands one document and no code**: a feasibility + staged plan
at `docs/plans/not-started/byoe-external-editor.md`, in the style of the
existing plans in that directory (goal / what exists / what has to change /
steps / test impact).

Decisions already taken (they belong in the doc as stated premises):

- **Binding = the active scene's file.** A managed `.glr-workspace` already
  stores each scene as its own file with a stable name; BYOE is bidirectional
  sync of that directory, not a new file-identity concept.
- **Outbound is explicit-save only.** Inbound is automatic; Ctrl+S stays the
  only writer. Halves the conflict surface and avoids fighting vim's own
  "file changed on disk" prompt on every `;`.
- **Inbound is atomic.** A file with a line the loader rejects is refused
  whole, with a status-bar error naming the failing line. Today's importer is
  tolerant (warn + skip), which would silently delete rows of live work.

## Verdict up front

- **Stage 1 is cheap and mostly assembly.** Every mechanism exists; the only
  genuinely missing piece is an *atomic* import policy, and that is already
  designed in `docs/plans/not-started/one-scene-loader.md` §2. A working
  prototype of the inbound half already ships in the tree.
- **Stage 2 is a small delta on stage 1, not a new subsystem.** Partial-line
  parsing and the overlays it feeds already work — on the live input buffer,
  which is programmatically settable.
- **Stage 2.5 — a live vim WIP buffer with no plugin and no IPC — is the
  highest-value step and is smaller than stage 3.** Two lines of vimrc write a
  plain-text sidecar with the cursor as an `@cursor` comment directive; gl-repl
  reuses the stage-1 watcher and the stage-2 partial-line handling unchanged.
  Vim's `.swp` cannot serve this (it carries no cursor, flushes on a 200-char /
  4-second heuristic, and has no alternate format) — details below.
- **Stage 3 is the expensive one, and if 2.5 lands it may not be worth
  building.** Its cost is transport + a vim plugin, and its only advantage over
  2.5 is removing the disk round-trip — a performance refinement, not a new
  capability. It is only tractable at all if scoped as "vim pushes buffer +
  cursor, gl-repl treats it like a stage-2 reload"; a character-granular
  shared-document model is a different and much worse project (see below).

The one thing that bites at every stage and has no cheap fix: **gl-repl
rewrites the text.** That is a UX cost to state clearly, not a bug to solve.

## Prior art already in the tree

`tools/repl_live_demo/repl_live_demo.c` is a working file-watching REPL host
built for exactly this workflow — its header comment opens with "The editor is
external (vim, or anything). This demo never edits text."

- `file_mtime()` (`:286`) — plain `stat` + `st_mtime`.
- `poll_active_file()` (`:461`), driven by `watch_timer` (`:706`) on a
  `poll_ms` (default 250) GLUT timer.
- `import_active_scene()` (`:334`) — resets live state, picks the loader by
  extension, reflattens, rebuilds the variable panel.
- `:382-383` — after gl-repl writes the file itself it re-stamps the mtime so
  the watcher does not self-trigger. That is the loop-breaking trick stage 1
  needs verbatim.

Its header (`:24-33`) is also candid that reload is **not** transactional and
that a malformed save can replace a good scene. That is precisely the gap
between the demo and a shippable feature, and it is the same gap
`one-scene-loader.md` exists to close.

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
| Undo after replacement | `editor_undo_note_wholesale_replacement()` (`src/editor/undo.c:214`) — called only *after* a confirmed success (contract at `src/editor/inline_file_prompt.c:141-147`) |
| Report failure | `repl_set_status_error()` (`src/repl/host_effects.h:17`) |

### What has to change

1. **Atomic load policy.** `import.c` warns per bad line and continues
   (`import_state_warn_parse_line`, `import.c:268-291`); it only fails the
   whole file on read error, over-long line, canonical-order violation
   (`import_finish_load`, `import.c:2941-2977`) or zero commands loaded
   (`:3045`). Worse, on that last failure the `@cfg` and camera side effects
   have already been committed.
   → This is `one-scene-loader.md` §1-2: `ReplSceneLoadOpts` carrying an
   explicit `ReplSceneLoadPolicy { TOLERANT, ATOMIC }`, with `ATOMIC`
   restoring through `SceneSnapshot`. **That plan is a prerequisite, not an
   optional cleanup** — BYOE cannot be built on the tolerant loader without
   silently eating the user's scene. Land steps 1-2 of it first.

2. **Reload into the *active* slot.** `repl_load_scene_via_loader()`
   allocates a *fresh* slot and fails with `ERR_NO_SLOT` when all eight are
   full — correct for "open a file as a new scene", wrong for "this scene's
   file changed". Needs a sibling that reuses the active slot and keeps its
   name / `file_name` / `glr_origin_path` binding.

3. **A watcher.** New controller-band TU (`src/app/glr_extedit.c`) holding
   the bound path + last-known mtime, polled once per frame. Not
   `src/app/boot/` — it runs inside the frame loop, and
   `check-app-boot-band` forbids the controller including boot headers.
   Per CLAUDE.md's frame rule and the note at `gl_repl.c:36-40`, it needs its
   own `ProfSection` (enum in `prof_sections.h`, descriptor rows in
   `src/app/glr_prof.c:106`/`:192`) or it becomes unattributed remainder.
   The poll is a `stat` — cheap — but the *reload* is not, and that is the
   spike worth attributing.

4. **Self-write suppression.** Every gl-repl write to the bound path must
   re-stamp the stored mtime (`repl_live_demo.c:382`), or Ctrl+S triggers an
   immediate reload of what was just saved.

5. **Enablement.** A `GlrConfigKey` toggle via skill `gl-repl-config-toggle`
   (one `g_cfg_items[]` row + storage in the two default-less switches in
   `glr_config.c`) — note this forces a `@cfg` line into all example goldens.
   Plus a CLI flag in `GlrCliOptions` (`src/app/boot/glr_cli.h:34`, positional
   capture at `glr_cli.c:383`) so `./gl-repl --watch scene.c` works from
   launch. Default off.

6. **Web build is inert.** No filesystem, no watcher — `#ifdef` the TU the way
   `src/app/glr_web_io.c:8` does in reverse, and keep the TU non-empty for the
   C99 rule.

### The friction that cannot be designed away

gl-repl does not store the user's bytes. Canonical text is **regenerated from
the parsed command** via the spec `fmt` (`src/repl/parser.c:472-580`) with
indentation re-derived from block scope, not from the source line
(`repl_source_scope_cmd_indent`, `src/repl/source_scope.h:134-148`), and
`repl_load_apply_line()` strips C float suffixes on the way in
(`src/repl/load.c:98-101`). So the first Ctrl+S after an external edit
rewrites the user's spacing, indentation and `1.0f` literals.

Three honest mitigations for the doc:

- It is a **fixed point after one pass** — `tests/test_repl_core_io.c:765-800`
  already asserts `export∘import∘export == export`. So the file churns once and
  then stays stable, which is tolerable; it is not an every-save diff storm.
- **`.glr` survives best** (`src/repl/export_glr.c:36-53` — indent is
  presentation-only and round-trips exactly). Recommend `.glr` as the BYOE
  format and say why.
- Expressions with visible variables keep their verbatim text
  (`preserve_expr` / `has_vars`, `src/repl/normalize.h:17-23`), so the parts a
  user most cares about formatting are the parts that survive.

Also worth stating: `repl_document_rebuild()` (`src/repl/replace.c:116`) looks
like the right primitive and **is not** — it deliberately preserves cfg,
camera, scene name and workspace binding (`rebuild_reset_live`,
`replace.c:38-48`), so it would drop the `@cfg` / `@camera` headers an external
file carries. It is the model for the transaction shape, not the entry point.

## Stage 2 — one incomplete trailing line

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
  renderer supplies identity for the unset slots
  (`src/render3d/guides/guides_shared.h:78-116`). **The requested behavior
  already happens today** for any text placed in the input buffer.
- Autocomplete follows for free: `ac_try_enum_slot_completion()`
  (`src/app/glr_completion.c:529`) resolves the active slot from top-level
  commas before the cursor and already fires mid-line.

So stage 2 is: on an atomic reload, if the *only* rejected line is the final
non-empty one, do not fail — feed the preceding lines, then park that line in
the input row with the cursor at its end. Concretely a `partial_tail`
allowance on the load options, plus the editor-side placement (which must live
in `src/app/`, not a `repl_*` TU — `check-no-load-line-to-input-in-pipeline`
forbids pipeline TUs from touching the input row, `src/editor/input.h:160`).

Two things to nail down in the doc:

- **"Rejected" is not "incomplete".** `glVertex3f(1,` and `glVertx3f(1,2,3)`
  both fail `repl_parse_and_normalize_strict`. Accepting *any* failing final
  line as a partial silently swallows typos. Options: require an unbalanced
  open paren / brace, or require the prefix to match a known command name.
  Recommend the latter — it reuses the same prefix test the guides already do
  and gives a real error for a misspelled command.
- **The partial line is not in the document**, so the outbound write drops it.
  That is consistent with outbound being explicit-save only, but say so.

## Stage 2.5 — live vim WIP buffer, no plugin, no IPC

The user asked whether vim's `.swp` file could carry the work-in-progress
buffer, and whether vim can be told to write it in an easier format.
**Checked against the installed vim 9.2's own docs: no, and don't.** But the
underlying idea is right, and there is a much better route to it.

### Why not the swap file

| Question | Answer (verified) |
|---|---|
| Can the format be changed? | **No.** The only swap knobs are `'swapfile'` (on/off, `options.txt:8483`), `'directory'` (location, `:3212`), `'swapsync'` (fsync, `:8505`) and `'updatecount'`/`'updatetime'` (timing, `:9460`). There is no plaintext option. |
| Is it documented? | Not as an API. It is vim's internal block-paged memline structure (block 0 header, pointer blocks, data blocks). Third-party parsers exist but track vim versions. |
| Is it written live? | **No.** "updated after typing 200 characters or when you have not typed anything for four seconds" (`recover.txt:102-104`). Tunable via `updatecount`, but at `updatecount=1` every keystroke costs a write **plus an `fsync`** (`'swapsync'` defaults to `"fsync"`). |
| Does it carry the cursor? | **No** — and this is the disqualifier. `recover.txt` never mentions cursor. Block 0 holds file identity, mtime, inode, dirty flag. Cursor position lives in shada/viminfo, written at exit. Stage 2.5 exists *for* the cursor. |

Also fatal for this purpose: the swap is updated "only if the buffer was
changed, **not when you only moved around**" (`recover.txt:104`), so
cursor-only motion would never produce an event.

### What to do instead — a sidecar WIP file written by autocmd

Two lines of vimrc, no plugin, no protocol, plain text:

```vim
autocmd TextChanged,TextChangedI *.glr call s:GlrWip()
autocmd CursorMoved,CursorMovedI  *.glr call s:GlrWip()
```

…where `s:GlrWip()` writes the buffer to `<file>.wip` with the cursor appended
as a trailing directive:

```c
// @cursor 42 13
```

All four events are confirmed present (`autocmd.txt:1334-1352`, `:771-793`),
and `TextChangedI`/`CursorMovedI` mean it fires while still in insert mode —
exactly the "half-typed line" case stage 2 is built for.

This is strictly better than swap parsing on every axis: it fires on cursor
motion, it carries the cursor, it is plain text, it has no version coupling,
and gl-repl already has the mtime watcher from stage 1.

### Why this makes stage 2.5 nearly free

`@cursor` fits the existing comment-directive vocabulary — `@cfg`,
`@scene-name`, `@workspace-dir`, `@declare`, `@camera`, `@plot`. CLAUDE.md
already records *why* `@plot` is a comment tag and not a config slug: the tag
rides the row's canonical text, so it survives commit/reformat/export/import
with no index bookkeeping. The same argument applies here, which means
stage 2.5 is **stage 2 plus "honor an `@cursor` directive"** — one directive
handler alongside the existing ones in `import.c`, feeding the
`editor_state_edit_line_set()` + `editor_cursor_pos_set()` calls stage 2
already needs.

One binding change: gl-repl watches `<scene>.wip` **for reading** and keeps
writing `<scene>` on Ctrl+S. Strictly one-way, so the self-write suppression
from stage 1 is not even needed on this path, and vim never sees gl-repl
reformat the file it is editing.

The real cost moves to **debounce**: a keystroke-rate WIP file means a full
reparse + reflatten per change. The once-per-frame mtime poll naturally caps
this at 60 Hz, but a reflatten every frame while typing is the thing to
measure. This is the one place stage 2.5 needs a number before it is called
done.

### What this does to stage 3

If 2.5 works, stage 3's only remaining advantage is removing the disk
round-trip and moving debounce into the protocol — a **performance
refinement, not a new capability**. The doc should say so plainly and mark
stage 3 as speculative-until-2.5-is-measured, rather than as the planned
destination.

## Stage 3 — real-time vim integration

### The cost

Nothing in `src/` does IPC. Grep confirms **no** sockets, kqueue, inotify,
FSEvents, mkfifo, fork/exec or threads; the only external-process code is
`popen("/usr/bin/pbpaste")` in `src/app/glr_clipboard.c:101`. GLUT's loop is
single-threaded, so the transport must be non-blocking and polled from the
same per-frame slot stage 1 adds.

So stage 3 = a transport (unix domain socket or FIFO — both new to the tree),
a line protocol, a vim plugin, and a second consumer of the stage-2 loader.

### The scoping decision that decides whether this is tractable

**Do not build a shared-document / character-granular model.** gl-repl's
document is a parsed `GLCmd[]` with real structural invariants — declarations
hoist to the end of the declaration prologue, canonical order is validated,
indentation is derived from block scope, `float x;` scope depends on the
enclosing `CMD_FUNC_DEF`. Vim's document is bytes. Synchronizing per-keystroke
means parsing arbitrary intermediate states, which is the exact opposite of
the strict one-command-per-line pipeline the whole REPL is built on.

The tractable version: **vim pushes the whole buffer plus cursor row/col on
`TextChanged` / `CursorMoved`; gl-repl handles it exactly as a stage-2
reload, minus the disk round-trip.** That reuses 100% of stages 1-2 and
reduces stage 3 to transport plus debounce. It also makes the stage-2 partial
line the *normal* case rather than an edge case — mid-typing, the cursor line
is nearly always incomplete, which is what makes live overlays worth having.

Open questions the doc should record rather than answer:

- Debounce policy — a reflatten per keystroke at 60 fps is the real cost, not
  the socket.
- Whether gl-repl→vim ever pushes (canonicalization would move vim's cursor);
  recommend one-way vim→gl-repl for stage 3 and keep Ctrl+S as the only
  gl-repl→file writer, consistent with stage 1.
- Transport choice, and whether the vim plugin ships in-tree
  (`packaging/`?) or out.

## Deliverable

Write `docs/plans/not-started/byoe-external-editor.md` with the sections
above, matching the house style of `docs/plans/not-started/one-scene-loader.md`
(Goal / the bug or need this comes from / what exists today as a table /
Design / Steps / Test impact / coverage gaps).

It must state explicitly:

- `one-scene-loader.md` steps 1-2 are a **prerequisite** for stage 1, and BYOE
  is the second consumer that justifies the `ReplSceneLoadPolicy` split.
- `tools/repl_live_demo/` is the existing prototype and what it does **not**
  do (transactionality) is exactly the delta.
- Canonicalization rewrites the user's file, is a fixed point after one pass,
  and `.glr` is the recommended format.
- The `.swp` route was evaluated and rejected on evidence (no cursor, coarse
  flush heuristic, no alternate format) — record the finding so it is not
  re-litigated, and cite `recover.txt` / `options.txt` line numbers.
- Stage 2.5 supersedes stage 3 as the target; stage 3 is speculative until
  2.5's reflatten cost is measured.
- Native-only; the web build is inert.

Add the one-line pointer to `docs/plans/README.md` if that file indexes the
directory (check its format first).

## Verification

No code, so no build to run. Checks for the document:

1. `make check-trailing-whitespace` — hard-failing guard over commits since
   `origin/main`; a markdown file trips it like any other.
2. Re-read every `file.c:line` citation in the finished doc against the tree —
   this plan's references were taken from a live read, but a doc that names
   drifted line numbers is worse than one that names none.
3. Confirm the `docs/plans/not-started/README.md` convention (whether entries
   are indexed) and follow it.
4. Sanity-check the stage-2 claim by hand before writing it as fact: run
   `./gl-repl`, type `glVertex3f(1,` without committing, and confirm the
   guide overlays render with the two unset slots at identity. If they do not,
   the stage-2 section is wrong and needs re-derivation from
   `fill_guide_arg_slots()`.
5. Sanity-check the stage-2.5 vimrc snippet by actually running it: open a
   `.glr` in vim with those autocmds, type into it, and confirm `<file>.wip`
   updates per keystroke and per cursor motion with a correct `@cursor` line.
   The doc should ship a snippet that has been run, not one that has been
   reasoned about.
