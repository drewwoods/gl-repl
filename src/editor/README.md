# `src/editor` — the text-document model + controller (Draft)

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
> module-local view: what a text-editor component *is*, how the standalone
> demo exercises it, and what it does inside this app.

## What this is, in general

Every code editor, IDE pane, or text widget has a **document model** sitting
behind the glyphs: a buffer of lines, a cursor, a selection, an undo/redo
history, a clipboard, search state, and autocomplete state. On top of that
model sits a **controller** that turns keystrokes and mouse events into
edits ("Backspace deletes the char before the cursor", "Tab accepts a
completion", "scroll follows the cursor").

`src/editor` is that model + controller, split along a deliberate seam:

- **Generic, application-free core.** [`state.c`](state.c) owns the document
  ([`EditorState`](state.h#L199): buffer, cursor, selection, scroll, undo, …) and
  [`edit_ops.c`](edit_ops.c) provides the primitive operations (insert a char, delete at
  cursor, consume a selection) that *any* plain-text editor needs. These
  know nothing about OpenGL, REPL grammar, or this app.
- **Application-flavored controllers.** [`input.c`](input.c) (key/mouse dispatch with
  this app's bindings), [`commit.c`](commit.c) (the transaction that turns a text edit
  into a validated program change), [`clipboard.c`](clipboard.c), [`undo.c`](undo.c), [`search.c`](search.c),
  [`completion.c`](completion.c), and the inline overlays. These encode *policy* — what a
  given keystroke should mean in this app.

The general lesson the layout encodes: the **data model and edit primitives
are reusable; the key bindings and commit semantics are app-specific.**

## The demo: `editor_demo`

[`tools/editor_demo/`](../../tools/editor_demo/) is a **generic plain-text
editor** built from the reusable half of this module only. It links
[`state.c`](state.c) (the document model) and [`edit_ops.c`](edit_ops.c) (the primitives), plus the
generic text panel from `src/ui/core` — and drives them with its *own* input
dispatcher ([`tools/editor_demo/input.c`](../../tools/editor_demo/input.c)) and its *own* File menu
([`tools/editor_demo/menu.c`](../../tools/editor_demo/menu.c)). It must not link `src/ui/app`: the REPL
code-panel adapter, menu bar, [`UiRenderSnapshot`](../ui/app/snapshot.h#L71), [`UiState`](../ui/app/state.h#L20), and app chrome are
part of the full app composition, not the generic editor proof.

```bash
make editor_demo                # real GL: opens a minimal text-editor window
make editor_demo USE_GL_STUBS=1 # headless link-only smoke test (no GL dev libs)
```

In the real-GL window: typing inserts characters via `edit_op_type_char`,
Backspace deletes via `edit_op_backspace`, the arrow keys move the cursor
within the row, and the File menu shows Load / Save (placeholder handlers)
plus Quit.

The demo's value is what it *refuses* to link: [`input.c`](input.c), [`commit.c`](commit.c),
[`clipboard.c`](clipboard.c), [`undo.c`](undo.c), [`reformat.c`](reformat.c), [`search.c`](search.c), and [`completion.c`](completion.c) are all
recognized as the **REPL editor's** controllers, not generic ones. By
standing up a working editor without them, the demo proves the document
model and edit primitives are genuinely application-free — the boundary
between "text editing" and "REPL editing" is real, not aspirational. If a new
editor feature needs `src/ui/app` or `src/app` to make `editor_demo` link, first
extract a smaller `src/ui/core` primitive or pass a neutral view into the demo.

## In the REPL app

Inside the full app this is **layer 2** of the ownership map. The contract:
**the editor is the model/controller for text; UI is its view.**

- The editor exposes per-frame snapshots that `src/ui` renders, and it
  consumes neutral [`UiHit`](../ui/core/hit.h#L51) results (from UI hit-testing) to interpret mouse
  input. UI never decides text behavior.
- A keystroke that can change line text, cursor, scroll, selection,
  search/autocomplete, or undo history is handled here — after
  [`src/app/glr_ctrl.c`](../app/glr_ctrl.c) has already filtered out non-editor concerns (replay,
  audio, config, save, camera, the variable panel).
- **Commits are transactions.** [`commit.c`](commit.c) is the only path that crosses
  into the REPL: it calls `repl_compile` (pure validation); on success it
  takes an undo snapshot, writes the editor buffer, and applies the parsed
  command to REPL runtime state — all inside one undo boundary, so undo restores
  both sides together. On a validation failure, nothing mutates.
- Read-only documents are also editor sessions: [`help_session.c`](help_session.c) backs the
  F1 overlay with the same scroll/search/cursor model and no commit path.

The editor owns the **canonical per-line text**; [`GLCmd`](../repl/command.h#L109) in `src/repl`
carries none. That single-writer rule is why the REPL pipeline can be driven
without the editor at all (see `repl_demo`).

## File map

| File | Responsibility |
|---|---|
| [`state.c`](state.c) / `.h` | Owns [`EditorState`](state.h#L199): buffer, input, cursor, selection, scroll, search, autocomplete, undo, cursor blink |
| [`edit_ops.c`](edit_ops.c) / `.h` | Generic text-editing primitives (REPL-free; shared with `editor_demo`) |
| [`input.c`](input.c) / `.h` | REPL editor key/mouse dispatcher (`;` commit, Tab, Ctrl+R, comment toggle, …) |
| [`commit.c`](commit.c) / `.h` | Commit transaction boundary: compile → undo → buffer write → REPL apply |
| [`undo.c`](undo.c) / `.h` | Undo/redo rings (restore editor text + REPL command state together) |
| [`clipboard.c`](clipboard.c) / `.h` | Selection anchors, copy/cut/paste payloads (line-range + input-text) |
| [`search.c`](search.c) / `.h` | Case-insensitive search query + match navigation |
| [`completion.c`](completion.c) / `.h` | Completion-provider registry (provider itself is [`src/app/glr_completion.c`](../app/glr_completion.c)) |
| [`help_session.c`](help_session.c) / `.h` | Read-only editor session for the F1 overlay (tab + scroll) |
| [`inline_rename.c`](inline_rename.c) / `.h` | Inline scene-rename input buffer |
| [`inline_file_prompt.c`](inline_file_prompt.c) / `.h` | Inline save/load filename prompt |
| [`reformat.c`](reformat.c) / `.h` | Whole-document reindent (Ctrl+R) |
| [`limits.h`](limits.h) | Shared editor input / autocomplete capacity constants |

**Boundary:** the editor owns text behavior and the commit transaction. It
does **not** own GL execution, menu/HUD chrome, the variable-panel/replay
peers (those are `src/subsystems`), or parsed-command semantics (that's
`src/repl`).
