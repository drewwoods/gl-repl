# Plan: Editor-Owns-Text Spike

## Context

The current architecture stores the canonical text of every committed line as `GLCmd.source[256]` — a 256-byte char array baked directly into the command struct. Every display, export, search, undo snapshot, replay annotation, and re-indent pass reads this field. The color picker is the only code that modifies `source[]` of an already-committed command (in `repl_command_store_write_color_source`), doing so by reading the indent out of the existing source before overwriting.

The redesign direction: make the **editor own the text**. Committed lines live in a text buffer inside `ReplRuntimeState`; `GLCmd` drops `source[]` and becomes a pure parse-result struct. `GLCmd.args[]` becomes derived (re-parsed from text at flatten time, already the path for `has_vars` commands). Cross-line highlights (feeding normal/color lines), color picker mutations, and replay annotations become controller-pushed decorators/transformers/virtual-lines that the editor renders from configuration.

This plan covers **Step 1 only: the spike**. The spike proves out the load-bearing performance assumption ("re-parsing on every keystroke is fast enough") before committing to the full 6-step redesign. If the spike validates the assumption, the full staged plan follows naturally from its foundation.

## Scope of the Spike

**The one question to answer:** Can we stop caching `cmd.source` as the canonical text and instead re-derive the text from the editor buffer on every re-parse, with acceptable latency under worst-case conditions (large examples, many `has_vars` lines)?

**What the spike does:**

1. Adds `char lines[MAX_COMMANDS][MAX_LINE_LEN]` to `ReplEditorBuffer` inside `ReplRuntimeState`, alongside the existing `GLCmd[]` array.
2. On every commit (`;` key, `feed_line()`, paste, undo restore), writes the raw pre-normalization text into `editor_buffer.lines[idx]`.
3. Changes `GLCmd.args[]` to be re-derived at flatten time for ALL commands (not just `has_vars`), using `repl_parser_parse_command_ctx()` reading from `editor_buffer.lines[idx]`.
4. Keeps `GLCmd.source[]` alive (no struct change yet) but stops using it as the read path for display — code panel reads from `editor_buffer.lines[idx]` instead.
5. Instruments: measure time spent in `flatten_commands()` per frame on the largest built-in example under worst-case replay (every command is `has_vars`-style re-parse).
6. Sets a pass/fail bar: frame budget at 120fps is ~8ms. Flatten must stay under 4ms (half-budget) on the worst-case example.

## What Is NOT in the Spike

- No structural change to `GLCmd` (source[] stays, just stops being authoritative).
- No transformer/decorator/virtual-line API.
- No color picker changes.
- No undo model change (undo still snapshots `GLCmd[]` including the now-redundant `source[]`).
- No test changes beyond adding the spike's instrumentation test.
- No export/import changes.

The spike is independently revertable in one commit.

## Original discussion:
❯ I really want the UI editor to own the actual text. It can be instructed how to render all the actual color scheme or to highlight. The certain keywords. Via some sort of configuration and it could have
  The ability to have certain complications or decorators, which could be the color pickers, The idea and then the repo would actually query the UI editor. To get the actual current text back. The controller
   could What needs to be worked out is how would say the color picker, it modifies the actual color component. I'm not sure how that would be merged back into the geo color core, I want the controller to
  know about it. But perhaps there's You can have transformers of which the color picker could be one of them, actually. Transformers on the actual line. And something would need to describe how it gets
  transformed, maybe the controller. Possibly possibly the controller. And we also need to make sure we work out the currently, there are our highlights. When you if you if you have if you your cursor is on
  a vertex, it will highlight the corresponding The corresponding lines that set the color at all normal and normal. So that sort of a relationship might need to be built I'm not sure how it's actually done
  today.

⏺ This is a real architecture shift, not a local refactor. Let me think through the design carefully and lay out what's involved.

What you're describing

Today the REPL owns GLCmd[] — parsed arrays with cmd.source[] text and cmd.args[] floats kept in sync. The code panel renders that. Editing is "navigate to a line, retype it, hit ;, parser rebuilds the
GLCmd."

Your proposal inverts this: the editor owns a buffer of text, and the REPL queries the editor for current text when it needs to re-parse. Highlighting, color swatches, vertex-index labels become
configuration the editor consumes. Color picker becomes a transformer — a widget anchored to a span that can rewrite that span when manipulated.

This is approximately the architecture of:
- VS Code / LSP: editor owns buffer; language server queries via documentSymbol/foldingRange/codeLens; CodeLens is the "decorator" pattern; LSP commands are the transformer dispatch.
- Emacs: buffer is primary; major mode adds font-lock, decorators (overlays); defun-region mutations are the transformer pattern.
- Jupyter notebooks: each cell is a text buffer; widgets (sliders, color pickers) are anchored decorators that can rewrite cell text.

It's a coherent direction. It's also a significant refactor.

What "editor owns text" actually means

Concretely:

typedef struct EditorBuffer {
    char        text[MAX_DOCUMENT_LEN];     // single canonical buffer
    int         length;
    Line        lines[MAX_LINES];           // indexes into text
    int         line_count;
    int         cursor_line;
    int         cursor_col;
    int         selection_anchor_line;
    int         selection_anchor_col;
    Decoration  decorations[MAX_DECORATIONS]; // applied by configuration
    int         decoration_count;
    Transformer transformers[MAX_TRANSFORMERS]; // anchored widgets
    int         transformer_count;
    Highlight   highlights[MAX_HIGHLIGHTS];   // controller-pushed
    int         highlight_count;
} EditorBuffer;

The REPL calls editor_buffer_get_line(line_idx) to get text. When the user commits a line (presses ;), the REPL re-parses that line's text into a GLCmd and updates its model. The editor doesn't know about
GLCmd; it just owns text.

The four hard problems

1. Source of truth for command args

Today: cmd.source (text) and cmd.args[] (floats) are coupled. Parser writes both at commit. Color picker writes both at commit. Replay annotation can rewrite cmd.source (e.g., glColor3f(0.5*t, …) →
glColor3f(0.4, …)) without touching args.

Tomorrow: only text exists. args[] becomes a derived projection from text, computed by the parser on demand. That's fine for execution (you re-parse anyway). It's harder for things that need args now
without re-parsing — e.g., the autonormal computation, replay's expression-substitution annotation, save/load.

The honest answer: re-parsing is cheap (microseconds per line). Make args derived; don't store it.

2. The color picker as a transformer

This is the cleanest part of your idea. A transformer:

typedef struct Transformer {
    int         line_idx;
    int         char_start, char_end;     // span in line
    TransformerKind kind;                 // COLOR_PICKER, NUMERIC_SLIDER, ENUM_PICKER
    union {
        struct { float r, g, b, a; int has_alpha; } color;
        struct { float min, max, current; int log; }  numeric;
        struct { const char **options; int selected; } enum_choice;
    } state;
} Transformer;

The editor:
- Renders an inline swatch / slider / chip at [char_start, char_end] based on kind.
- On click, opens the appropriate editor (color picker popup, slider drag, dropdown).
- When the user changes the value, the editor rewrites the span in its own text buffer and fires a transformer_changed(line_idx) event.

The REPL listens for that event:
- Re-parses the affected line.
- Updates flat program / autonormals / replay state as needed.

This is much cleaner than today's architecture, where the color picker reaches into repl_command_store_set_color to rewrite text and float args separately. With transformers, there's one pathway: text
changes → REPL re-parses.

Who decides which transformers exist? The controller, based on parsed structure. After parsing a line, it calls editor_set_transformers(line_idx, [{COLOR_PICKER, span_of_color_args}, ...]). So:

text edit → re-parse → controller computes transformers from parsed result → push to editor
                                                                ↓
                                     editor renders transformers next frame

This is symmetric with how LSP servers push semantic tokens / code lenses to editors.

3. Cross-line highlights (cursor-on-vertex feeds normal/color lines)

Today: when cursor is on a vertex line, repl_find_feeding_normal_cmd(edit_line) and repl_find_feeding_color_cmd(edit_line) walk backward through the GLCmd array to find the most recent normal/color that
affects this vertex. The renderer then draws a gutter accent on those lines.

Tomorrow: this is a controller responsibility, not an editor one. Flow:
- Editor reports cursor_line_changed(line_idx) to controller.
- Controller looks up the parsed model, computes feeding-line indices, and pushes them as highlights:
editor_set_highlights(&editor, [
    { line: vertex_line, kind: PRIMARY },
    { line: feeding_normal_line, kind: SECONDARY_NORMAL },
    { line: feeding_color_line,  kind: SECONDARY_COLOR },
]);
- Editor renders highlights according to its color scheme config.

The editor doesn't know why those lines are related. It just knows "draw these in this style." That's the right separation — and it generalizes: any cross-line relationship (matching for/end, function call
site → definition, search match positions) follows the same pattern.

4. Replay annotations

This is the trickiest case. Today, when replay is running and expand_args is on, the code panel renders extra synthetic lines below a command showing the substituted-and-evaluated form (glVertex3f(0.5,
0.866, 0) below glVertex3f(cos(PI/3), sin(PI/3), 0)).

These aren't user text. They're synthetic decorations the renderer injects.

In the editor-owns-text model, these become virtual lines the controller pushes:

editor_insert_virtual_line(line_idx + 1, "  → glVertex3f(0.5, 0.866, 0)", STYLE_REPLAY_SUBST);

Virtual lines:
- Don't count toward the user's text buffer.
- Render between real lines but can't be cursored into.
- Are cleared when replay state changes.

Same pattern as VSCode's editor.decorations.set(...) with before/after content widgets.

Concrete design

Editor's public API (rough sketch)

// Text I/O
int  editor_line_count(const Editor *);
const char *editor_line_text(const Editor *, int line_idx);
void editor_set_line(Editor *, int line_idx, const char *text);
void editor_insert_line(Editor *, int line_idx, const char *text);
void editor_delete_line(Editor *, int line_idx);

// Cursor / selection (queried by controller for parsing context)
int  editor_cursor_line(const Editor *);
int  editor_cursor_col(const Editor *);
void editor_set_cursor(Editor *, int line, int col);

// Configuration: how to render
void editor_set_syntax_rules(Editor *, const SyntaxRule *, int n);
void editor_set_color_scheme(Editor *, const ColorScheme *);

// Decorators: per-line metadata pushed by controller
void editor_set_transformers(Editor *, int line_idx, const Transformer *, int n);
void editor_set_highlights(Editor *, const Highlight *, int n);
void editor_set_virtual_lines(Editor *, const VirtualLine *, int n);

// Events the editor publishes (consumed by controller)
typedef enum {
    EDITOR_EVENT_LINE_COMMITTED,    // user pressed ;
    EDITOR_EVENT_LINE_EDITED,       // any text change
    EDITOR_EVENT_CURSOR_MOVED,
    EDITOR_EVENT_TRANSFORMER_CHANGED,
} EditorEventKind;
typedef struct EditorEvent { /* tagged union */ } EditorEvent;
int  editor_pump_events(Editor *, EditorEvent *out, int max);

Per-frame controller loop

void imrepl_ctrl_display_frame(void) {
    // Drain editor events (text edits, cursor moves, transformer changes)
    EditorEvent events[64];
    int n = editor_pump_events(&g_editor, events, 64);
    for (int i = 0; i < n; i++) {
        switch (events[i].kind) {
        case EDITOR_EVENT_LINE_COMMITTED:
            // Re-parse this line, update GLCmd model
            repl_reparse_line(events[i].line_committed.line_idx,
                              editor_line_text(&g_editor,
                                               events[i].line_committed.line_idx));
            break;
        case EDITOR_EVENT_TRANSFORMER_CHANGED:
            // Color picker rewrote span; treat as line edit + reparse
            repl_reparse_line(events[i].transformer_changed.line_idx, ...);
            break;
        case EDITOR_EVENT_CURSOR_MOVED:
            // Recompute feeding-line highlights
            recompute_cross_line_highlights();
            break;
        }
    }

    // Push fresh decorations / highlights / virtual lines based on REPL state
    push_transformers_for_changed_lines();
    push_feeding_line_highlights();
    push_replay_annotations();          // virtual lines

    // Render (editor + scene + UI)
    ...
}

Highlights & transformers as data, not code

The color scheme lives in a config:

ColorScheme imrepl_scheme = {
    .keywords = (Keyword[]){
        { "glBegin",   COLOR_KEYWORD_PRIMITIVE },
        { "glVertex3f",COLOR_KEYWORD_VERTEX },
        ...
    },
    .syntax = {
        .number  = COLOR_NUMBER,
        .string  = COLOR_STRING,
        .comment = COLOR_COMMENT,
        ...
    },
    .highlights = {
        .primary          = COLOR_HL_CURRENT_LINE,
        .secondary_normal = COLOR_HL_FEEDING_NORMAL,
        .secondary_color  = COLOR_HL_FEEDING_COLOR,
        .selection        = COLOR_HL_SELECTION,
        .search_match     = COLOR_HL_SEARCH,
    },
};

editor_set_color_scheme(&g_editor, &imrepl_scheme);

That's the "editor is configurable" promise. Everything specific to the REPL is data, not editor logic.

What this costs

- repl_editor.c becomes the editor. It owns the buffer, cursor, selection, autocomplete UI, search UI. ~3-4kLOC of new code, replacing the current ~67kLOC repl_editor.c (which is mostly cross-layer routing
today, not editor logic — that's at least a sign the work isn't strictly additive).
- GLCmd.source goes away. The parsed model holds only type, args[], references to source line numbers. Text lives in the editor.
- Re-parse model. Every text edit triggers an incremental re-parse. Today's commit-driven parsing becomes edit-driven, with a debounce. Manageable but a real change.
- Persistence. Save/load currently round-trips via cmd.source. Tomorrow it round-trips via editor_line_text(). Same shape, different source. Probably easier actually.
- Tests. A lot of test_repl_core_*.c poke cmd.source directly. Those tests need updating to drive the editor instead. ~50% of test setup code.
- The replay system needs to learn virtual lines. Right now it directly edits cmd.source in some paths (via the replay-annotations module). That has to become "push virtual line below line N" instead.
- Undo. Today undoes GLCmd[] snapshots. Tomorrow it undoes editor text + cursor. Either two undo stacks (editor undo, REPL undo) — surprisingly common in real systems — or one unified one rooted in the
editor.

Sequencing — how I'd actually do this
Don't try to land this in one branch. Sequence:
Step 1: Make text the source of truth for GLCmd.source already. Today cmd.source is computed by the parser and reflects the normalized form, not the user's typed text. Make cmd.source strictly mirror the
editor's line text — kill normalization-on-commit, keep normalization-on-display only. This forces the existing system to treat text as primary without changing architecture. ~1 week.
Step 2: Introduce a thin editor wrapper around repl_state text. editor_line_text(idx) just returns cmd.source for now. editor_set_line(idx, str) updates cmd.source and triggers re-parse. Same data, new
façade. ~3 days.
Step 3: Move text storage into the editor. Now EditorBuffer owns char text[]; GLCmd drops source[] and stores (line_idx, char_start, char_end) references instead. ~2 weeks. This is the big diff.
Step 4: Transformers. Generalize the existing color-picker into a Transformer API. Add numeric-slider transformers for plain numbers (replaces some var-panel use cases). ~1 week.
Step 2: Introduce a thin editor wrapper around repl_state text. editor_line_text(idx) just returns cmd.source for now. editor_set_line(idx, str) updates cmd.source and triggers re-parse. Same data, new
façade. ~3 days.
Step 3: Move text storage into the editor. Now EditorBuffer owns char text[]; GLCmd drops source[] and stores (line_idx, char_start, char_end) references instead. ~2 weeks. This is the big diff.
Step 4: Transformers. Generalize the existing color-picker into a Transformer API. Add numeric-slider transformers for plain numbers (replaces some var-panel use cases). ~1 week.
Step 5: Decorate everything. Move feeding-line highlights, replay annotations, search highlights to the highlight/virtual-line API. ~1 week.
Step 6: Configuration extraction. The color scheme, syntax rules, transformer-kind dispatch all become data files. ~3 days.
That's roughly 6-8 weeks of focused work, assuming no scope creep. It's probably the biggest single architecture change this codebase would have done, but it's tractable.
My recommendation
Two thoughts, separately:
On whether to do it at all: Yes, this direction is genuinely cleaner than what's there now. The current cmd.source/cmd.args coupling is awkward, the color-picker writing both halves separately is a bug
magnet, and having "text → parse → execute" be one-way (instead of "text owns; parse derives") makes future features (refactoring, multi-cursor, find-and-replace, vim mode, etc.) much easier. You're
describing a real improvement.
On whether the existing C-1/C-2 work helps: Mostly no. UiAction doesn't survive this redesign — when the editor owns text and emits EditorEvent, the action vocabulary is "edit happened, here's the new
text", not "the user pressed the up arrow." Phase B (the snapshot pattern) does survive — the editor would still consume a render snapshot. The Phase C work was solving a problem (UI mutation indirection)
that this redesign deletes the premise of.
Where to start: I'd suggest starting with Step 1 (text as source of truth) on a feature branch before committing to the full redesign. It's the smallest move that proves out the assumption "re-parsing on
every edit is cheap enough" — and if it turns out re-parsing has performance issues with a 1000-line example, you discover that at the cheapest possible point, before any architecture work.
Specifically, write a one-day spike: stop normalizing cmd.source at commit, route every keystroke through "rewrite line text → re-parse." Profile it. If frame time stays under 4ms on the worst built-in
example, you have evidence the rest of the redesign is viable. If it doesn't, you need an incremental-parse story before going further, and that's a much bigger commitment.
The transformers idea, the cross-line highlights, the configurable color scheme — they all naturally fall out after the text-as-truth move. Don't design them in isolation; let the foundation prove out
first.

## Critical Files

| File | Change |
|---|---|
| `repl_state.h` | Add `ReplEditorBuffer { char lines[MAX_COMMANDS][MAX_LINE_LEN]; int line_count; }` field to `ReplRuntimeState` |
| `repl_state_views.h` | Add `ReplEditorBuffer` struct; add `repl_state_editor_buffer()` getter and `repl_state_editor_buffer_mut()` mutator |
| `repl_state.c` | Implement getter/mutator; add `editor_buffer` to `repl_state_capture()` / `repl_state_restore()` round-trip |
| `repl_editor.c` | On every commit path (`;` handler, `feed_line`, undo restore, `load_line_to_input`) write raw input text into `editor_buffer.lines[edit_line]` |
| `repl_command_store.c` | In `repl_command_store_insert_one`, `replace_one`, `load`, also write to `editor_buffer.lines[]` |
| `repl_flatten.c` | In `flatten_range()`, for ALL commands (not just `has_vars`), re-parse from `editor_buffer.lines[src_cmd_idx]` instead of using `src_cmd->args` directly. Time this path. |
| `ui_panels.c` | In `ui_panels_render_code_panel()`, for committed non-edit lines, read from `editor_buffer.lines[i]` instead of `cmds[i].source`. Compare visual output. |
| `repl_search.c` | In `editor_search_row_text()`, read committed lines from `editor_buffer.lines[row_idx]` instead of `cmds[row_idx].source`. |
| `tests/test_spike_perf.c` (new) | Loads the largest built-in example, runs flatten 1000 times, prints mean and max flatten time per frame. Pass/fail vs 4ms target. |

## Key Existing Functions to Reuse

- `repl_parser_parse_command_ctx()` — `repl_parser.c:700` — re-parse a line of text into a `GLCmd`. Already called in `flatten_range()` for `has_vars` commands; the spike extends this to all commands.
- `flatten_range()` — `repl_flatten.c` — the main flatten pass; the spike instruments it.
- `repl_command_store_load()` — `repl_command_store.c` — bulk restore used by undo; must also restore `editor_buffer.lines[]`.
- `load_line_to_input()` — `repl_editor.c` — already the canonical "sync g_input from source" function; read direction stays, but write direction now also updates `editor_buffer.lines[]`.
- `repl_state_capture()` / `repl_state_restore()` — `repl_state.c` — must include `editor_buffer` to keep round-trip tests passing.

## Invariants to Maintain

1. **`editor_buffer.lines[idx]` is the raw user-typed text**, stripped of trailing `;` and leading whitespace (matching what `load_line_to_input` currently produces). It is NOT the normalized form.
2. **`GLCmd.source[]` keeps the normalized form** during the spike (unchanged), so all display/export code that reads `cmd.source` keeps working. The spike runs both paths in parallel; correctness is confirmed by comparing output.
3. **`GLCmd.args[]` is no longer the primary args source** for the flatten path; it's still written at commit time (so undo snapshots stay consistent) but the spike proves we can re-derive it cheaply at flatten time.
4. **`repl_state_capture()` must include `editor_buffer`** so undo round-trips don't break `test_repl_state.c`.

## Implementation Steps

1. **Add `ReplEditorBuffer` to `ReplRuntimeState`** (repl_state.h, repl_state_views.h, repl_state.c). Wire into capture/restore. Run `make test` — should be green (no behavior change yet).

2. **Write into `editor_buffer.lines[]` on every commit** (repl_editor.c: `;` handler, `feed_line()`, `load_line_to_input`, undo restore; repl_command_store.c: insert_one, replace_one, load). Write the text BEFORE normalization — what the user typed, same as what `load_line_to_input` currently loads back from `cmd.source` after stripping the semicolon. Run `make test` green.

3. **Switch flatten to re-parse from `editor_buffer.lines`** for ALL commands (repl_flatten.c). Add a `prof_begin(PROF_FLATTEN_REPARSE)` / `prof_end` bracket around this path. Build and run the largest example; compare visual output to confirm correctness. Run `make test`.

4. **Switch display read path in ui_panels.c** from `cmds[i].source` to `editor_buffer.lines[i]` for committed non-edit lines. Confirm code panel renders identically.

5. **Switch search** (repl_search.c) from `cmds[row_idx].source` to `editor_buffer.lines[row_idx]`.

6. **Write the performance test** (`tests/test_spike_perf.c`). Load `repl_examples_lines(example_idx_of_largest_example)`, feed all lines via `feed_line()`, then loop 1000x calling `flatten_commands()` with a dirty flag set, recording wall-clock time. Print mean/max, pass at <4ms.

7. **Run and record results.** If pass: proceed to full redesign plan. If fail: document where the time is spent, and design an incremental-parse story before continuing.

## Verification

```bash
make sample USE_GL_STUBS=1        # must be clean
make test                          # 24/24 binaries must pass
make test-stubs                    # 27/27 binaries must pass
./test_spike_perf                  # must report mean flatten < 4ms on largest example
# Manual: run ./sample with the largest example, confirm visual correctness
# Manual: open color picker, confirm it still writes correctly
# Manual: test undo/redo across 10 commits, confirm no corruption
```

## Exit Criteria

- All automated tests pass.
- `test_spike_perf` reports mean flatten time < 4ms on the largest built-in example.
- Code panel renders identically to before the spike (confirmed visually).
- Search, color picker, undo all work correctly.
- The spike commit is clearly labeled and independently revertable.

## If the Spike Fails the Performance Bar

Record the profiling breakdown and document it in `feature/editor-owns-text-spike-results.md`. Likely cause: `repl_parser_parse_command_ctx()` being called O(n) times per frame where n is command count. Solutions to evaluate: batch dirty-tracking (only re-parse commands where text changed since last flatten), partial parse cache, or incremental parse.

## Full Staged Plan (Post-Spike, For Reference)

If the spike validates the performance assumption, the full redesign proceeds in these stages:

- **Step 2** — Thin editor wrapper façade: `editor_buffer_line_text(idx)` / `editor_buffer_set_line(idx, str)` API hiding the storage. Remove `cmd.source[]` reads from all callers in favor of the API. (~3 days)
- **Step 3** — Drop `cmd.source[]` from `GLCmd`. Struct shrinks from ~340 bytes to ~84 bytes. Update all 13 test files that write `.source` directly. Update undo snapshot (which currently copies the whole `GLCmd[]` array). (~2 weeks)
- **Step 4** — Transformer API: `Transformer { line_idx, char_start, char_end, kind, state }`. Convert color picker to a transformer pushed by the controller. (~1 week)
- **Step 5** — Cross-line highlight API: controller pushes `Highlight[]` to editor after cursor moves. Feeding-line accents, replay PC highlight, selection, search matches all become controller-pushed highlights. (~1 week)
- **Step 6** — Configuration extraction: color scheme, syntax rules as data structs. Virtual lines for replay annotations. (~3 days)
