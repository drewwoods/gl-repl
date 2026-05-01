# Plan: Editor-Owns-Text Redesign — Steps 2–6

## Context

Step 1 (spike) is **complete on `editor-text-spike` branch** and validated: ~4.2 ms worst-case
flatten on the largest example (3,580 flat commands), invisible at edit time since flatten only
runs when dirty. Results in `feature/editor-owns-text-spike-results.md`.

The spike added `ReplEditorBuffer { char lines[MAX_COMMANDS][MAX_LINE_LEN]; int line_count; }` to
`ReplRuntimeState` and made `repl_command_store.c` mirror every write to `cmd->source` into
`editor_buffer.lines[]`. The goal of Steps 2–6 is to **make `GLCmd` a pure parse-result struct**
(type, args[], flags, no text) so that all committed line text lives only in the editor buffer.

## Current state of the spike (on `editor-text-spike`)

| Item | Status |
|---|---|
| `ReplEditorBuffer` in `ReplRuntimeState` | ✅ |
| `repl_state_editor_buffer_line(idx)` / `_set_line()` API | ✅ |
| `repl_command_store.c` mirrors all store mutations → buffer | ✅ |
| `repl_flatten.c` `spike_text_for()` redirects most flatten reads | ✅ (partial — `flatten_get_for_var_name` line 51 missed) |
| `repl_search.c` reads buffer with fallback | ✅ |
| `repl_replay_annotations.c` reads buffer with fallback | ✅ (partial fallbacks remain at lines 645, 698, 731, 735) |
| `bench/bench_repl.c` `bench_spike_flatten_largest` sub-benchmark | ✅ |

## What `cmd->source` looks like today (to frame migration scope)

`GLCmd.source[256]` (`sample.h:264`) is the normalized form written by the parser (~25 `snprintf`
calls in `repl_parser.c`) and read by:

- `repl_flatten.c:51` — `flatten_get_for_var_name` (direct, not via spike shim)
- `ui_panels.c:567` — `hl_text = document_cmds[i].source` (syntax-highlight display)
- `repl_replay_annotations.c:645,698,731,735` — fallback reads for annotations
- `repl_export.c:1386,1459,1466,2217,2675,2717` — all save_output emit paths
- `repl_editor.c:200,368` — two read sites
- `repl_debug.c:28,35,83` — debug prints (low priority)
- `tests/test_repl_editor.c` — ~40 `ASSERT_STR` assertion sites reading `.source`
- `tests/test_scene_guides.c` — ~10 `snprintf(source_cmds[N].source, ...)` fixture writes

The spike mirrors mean `editor_buffer.lines[idx]` already holds the same normalized text as
`cmd->source` for all committed commands (same bytes, same indentation — the spike copies the
full normalized form, not the stripped raw form).

---

## Step 2: Migrate all `cmd->source` reads to editor buffer API (~3 days)

**Goal**: After this step, `cmd->source` is still written by the parser but read by nothing outside
the parser/commit pipeline. The editor buffer is the sole text source for display, export,
annotations, and search.

**Prerequisite**: Merge `editor-text-spike` branch to main first.

### 2a — Fix `flatten_get_for_var_name` (`repl_flatten.c:51`)

Currently reads `const char *p = cmd->source` directly, bypassing `spike_text_for`. The function
is called from within `flatten_range` where the command index (`src_cmd_idx`) is in scope — add
an `int cmd_idx` parameter and change the read to:

```c
const char *p = spike_text_for(cmd, cmd_idx);
```

Update the two call sites in `flatten_range` to pass the index.

### 2b — Fix `ui_panels.c:567`

```c
// before
hl_text = document_cmds[i].source;
// after — fallback keeps correctness during transition
hl_text = repl_state_editor_buffer_line(i);
if (!hl_text || !hl_text[0]) hl_text = document_cmds[i].source;
```

(The Phase B `UiRenderSnapshot` supplies `document_cmds`; `repl_state_editor_buffer_line` reads
global state — acceptable since the buffer is stable during render.)

### 2c — Fix `repl_replay_annotations.c` remaining fallbacks

Lines 645, 698, 731, 735 still fall back to `document_cmds[cmd_idx].source`. Make editor buffer
primary:

```c
// before
base = document_cmds[cmd_idx].source;
// after
base = repl_state_editor_buffer_line(cmd_idx);
```

Remove the fallback once confident (or keep as assert for a release or two).

### 2d — Fix `repl_export.c` reads (6 sites)

Lines 1386, 1459, 1466, 2217, 2675, 2717 all read `document_cmds[cmd_idx].source`. Switch to
`repl_state_editor_buffer_line(cmd_idx)`. The C-export path at 1250–1252
(`format_cmd_source_as_c`) does `repl_eval_expr_to_c(cmd->source, ...)` — switch that arg too.

The `@declare` reconstruction block (lines 1587–1618) builds a local `cmd.source` via `snprintf`
for a new command being inserted — keep as-is; it's building a temporary, not reading the store.

### 2e — Fix `repl_editor.c:200,368`

Both read `document_cmds[idx].source` — switch to `repl_state_editor_buffer_line(idx)`.

### Verification for Step 2

```bash
make test && make test-stubs
# Confirm no non-parser reads remain:
grep -n "\.source" repl_flatten.c repl_replay_annotations.c repl_export.c ui_panels.c repl_editor.c \
  | grep -v "source_cmds\|source_count\|source_line\|source_idx"
```

---

## Step 3: Drop `cmd.source[]` from `GLCmd` (~2 weeks)

This is the "big diff". Best done in sub-steps to keep each commit buildable.

### 3a — Make parser write to editor buffer alongside `cmd->source` (~1 day)

In `repl_parser.c`, every `snprintf(cmd->source, sizeof(cmd->source), ...)` write (~25 sites)
should be immediately followed by:

```c
if (ctx && ctx->source_line_idx >= 0)
    repl_state_editor_buffer_set_line(ctx->source_line_idx, cmd->source);
```

The `ReplParseContext.source_line_idx` field already exists for this purpose. Add the same pattern
to the var-declare snprintfs in `repl_commit.c:308–317`.

After 3a: both `cmd->source` and `editor_buffer.lines[]` are always in sync — no behavior change.

### 3b — Remove `source[]` from `GLCmd` (~3 days)

1. Delete `char source[MAX_LINE_LEN];` from `sample.h:264`.
2. The compiler will surface every remaining `cmd->source` access as an error — fix each:
   - `repl_parser.c` (~25 sites): write to a local `char normalized[MAX_LINE_LEN]` buffer, then
     call `repl_state_editor_buffer_set_line(ctx->source_line_idx, normalized)`.
   - `repl_command_store.c` color picker write (line 155): replace `memcpy(cmd->source, ...)` with
     `repl_state_editor_buffer_set_line(idx, new_text)` directly; remove the redundant mirror call.
   - `repl_core.c:230–236` reads indent from `out_cmd->source[parsed_indent]` — pass indent as a
     separate `int` return value from the parse helper instead.
   - `repl_debug.c` — update prints to use `repl_state_editor_buffer_line(i)`.

### 3c — Update undo snapshot (~1 day)

`ReplUndoSnapshot` (`repl_undo.h`) holds `GLCmd cmds[MAX_COMMANDS]` — after removing `source[]`
the text is gone from undo. Add a parallel text snapshot:

```c
typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    char  predef_names[MAX_PREDEF_VARS][16];
    int   num_predef_vars;
    char  editor_lines[MAX_COMMANDS][MAX_LINE_LEN];  /* text snapshot */
    int   editor_line_count;
} ReplUndoSnapshot;
```

Memory note: current per-snapshot cost = `MAX_COMMANDS * sizeof(GLCmd)` ≈ 4096 × 340 = 1.36 MB.
After: `MAX_COMMANDS * 84 + MAX_COMMANDS * 256` = 4096 × 340 = 1.36 MB — **identical total**.
The source bytes just moved from inside `GLCmd` to `editor_lines`.

`repl_undo_snapshot_save()` — also copy `editor_buffer.lines[]` into `snapshot->editor_lines`.
`repl_undo_snapshot_restore()` — restore `editor_lines` into `editor_buffer` before calling
`repl_command_store_load()` (so the flatten pass sees text immediately).

### 3d — Update 13+ test files (~3 days)

Key files and changes:

**`tests/test_repl_editor.c`** (~40 assertion sites): All `ASSERT_STR` calls reading
`repl_state_document_cmds_mut()[N].source` switch to `repl_state_editor_buffer_line(N)`.
Verify populated because Step 3a ensures parser always writes to buffer.

**`tests/test_scene_guides.c`** (~10 fixture writes): Replace
`snprintf(source_cmds[N].source, sizeof(...), "%s", text)` with
`repl_state_editor_buffer_set_line(N, text)`.

**Other test files** (`test_repl_core_commit.c`, `test_repl_core_parse.c`, etc.): compile errors
guide remaining fixes.

### Verification for Step 3

```bash
make test && make test-stubs
# GLCmd no longer has source field — any missed callers are compile errors
make bench BENCH_ARGS="--only spike_flatten_largest"  # regression check: still ≤ 4.5 ms
```

---

## Step 4: Transformer API — Color Picker (~1 week)

**Goal**: Color picker becomes a controller-pushed transformer. The editor renders a swatch inline;
drag rewrites the span in editor buffer; controller re-parses the affected line. Eliminates
`repl_command_store_write_color_source` entirely.

### Define `EditorTransformer` (new `editor_transformer.h`)

```c
typedef enum { TRANSFORMER_COLOR_PICKER, TRANSFORMER_NUMERIC_SLIDER } TransformerKind;

typedef struct {
    int line_idx;
    int char_start, char_end;   /* byte offsets into the editor buffer line */
    TransformerKind kind;
    union {
        struct { float r, g, b, a; int has_alpha; int is_clear; } color;
        struct { float min, max, current; int is_log; }             numeric;
    } state;
} EditorTransformer;

#define MAX_TRANSFORMERS 64
typedef struct {
    EditorTransformer items[MAX_TRANSFORMERS];
    int count;
} EditorTransformerList;
```

Add `EditorTransformerList editor_transformers;` to `ReplRuntimeState`.

### Push from controller each frame (`imrepl_ctrl.c`)

After flatten (fresh parsed args available), scan document commands for color commands and push:

```c
static void push_color_transformers(void) {
    repl_state_editor_transformers_clear();
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *cmd = repl_state_document_cmd_at(i);
        if (cmd->type == CMD_COLOR || cmd->type == CMD_CLEAR_COLOR) {
            EditorTransformer t = { .line_idx = i, .kind = TRANSFORMER_COLOR_PICKER, ... };
            // char_start/char_end: locate the color args in the editor buffer line text
            repl_state_editor_transformers_append(&t);
        }
    }
}
```

### On drag/pick (`ui_color_picker.c`)

Instead of calling `repl_command_store_set_color(cmd_idx, r, g, b)`:

1. Format new text: `snprintf(new_line, sizeof(new_line), "  glColor3f(%g, %g, %g);", r, g, b)`
2. Call `repl_state_editor_buffer_set_line(cmd_idx, new_line)`
3. Call `repl_state_mark_flat_dirty()` — triggers re-parse next frame

### Remove `repl_command_store_write_color_source` and wrappers

`repl_command_store_set_color()` and `repl_command_store_set_clear_color()` are removed.
`repl_command_store_write_color_source()` (lines 97–164 in `repl_command_store.c`) is deleted.

### Verification for Step 4

```bash
make test && make test-stubs
# Manual: open color picker, drag color, confirm text in code panel updates
# Manual: undo after color pick — text reverts correctly
```

---

## Step 5: Cross-line Highlight API (~1 week)

**Goal**: Controller pushes `EditorHighlight[]` each frame. UI renders from pushed data, not by
calling `repl_find_feeding_*` / search functions inline during draw.

### Define `EditorHighlight` (new `editor_highlight.h`)

```c
typedef enum {
    HIGHLIGHT_FEEDING_NORMAL,
    HIGHLIGHT_FEEDING_COLOR,
    HIGHLIGHT_REPLAY_PC,
    HIGHLIGHT_SEARCH_MATCH,
    HIGHLIGHT_SELECTION,
} HighlightKind;

typedef struct {
    int line_idx;
    int char_start, char_end;  /* -1, -1 = whole line */
    HighlightKind kind;
} EditorHighlight;

#define MAX_HIGHLIGHTS 256
typedef struct {
    EditorHighlight items[MAX_HIGHLIGHTS];
    int count;
} EditorHighlightList;
```

Add `EditorHighlightList editor_highlights;` to `ReplRuntimeState`. Include in `UiRenderSnapshot`.

### Push from controller each frame (`imrepl_ctrl.c`)

```c
static void push_highlights(void) {
    repl_state_editor_highlights_clear();
    int edit = repl_state_edit_line();
    int norm = repl_find_feeding_normal_cmd(edit);
    int col  = repl_find_feeding_color_cmd(edit);
    if (norm >= 0) repl_state_editor_highlights_append(norm, -1, -1, HIGHLIGHT_FEEDING_NORMAL);
    if (col  >= 0) repl_state_editor_highlights_append(col,  -1, -1, HIGHLIGHT_FEEDING_COLOR);
    if (repl_state_replay_playing())
        repl_state_editor_highlights_append(replay_pc_source_line(), -1, -1, HIGHLIGHT_REPLAY_PC);
    push_search_highlights();   /* search matches in visible range */
}
```

### Render from pushed data (`ui_panels.c`)

Code panel row loop reads `repl_state_flat_program_view().highlights` from snapshot and draws
gutter accents. Remove the inline calls to `repl_find_feeding_normal_cmd` /
`repl_find_feeding_color_cmd` from the render path (currently in `ui_panels.c`).

### Verification for Step 5

```bash
make test && make test-stubs
# Manual: cursor on glVertex3f — feeding normal/color lines get gutter accent
# Manual: replay mode active — PC line is highlighted correctly
# Manual: search active — matches highlighted across all visible rows
```

---

## Step 6: Config extraction + virtual lines for replay annotations (~3 days)

**Goal**: Color scheme and syntax rules become data structs pushed to the editor. Replay
annotations become virtual lines, not inline rendered rows.

### Color scheme / syntax rules as data

Move hardcoded color constants from `ui_panels.c` into a `EditorColorScheme` struct in
`repl_actions.c` (alongside `g_cfg_items[]`). Include in `UiRenderSnapshot`. `ui_panels.c` reads
colors from snapshot, not from compile-time constants.

Syntax keywords (for code-panel token coloring) move to a `SyntaxKeyword` table in the same
location.

### Virtual lines for replay annotations

```c
typedef struct {
    int  after_line_idx;        /* insert after this source line */
    char text[MAX_LINE_LEN];
    int  style;                 /* e.g. VIRTUAL_STYLE_REPLAY_SUBST */
} EditorVirtualLine;

#define MAX_VIRTUAL_LINES 512
typedef struct {
    EditorVirtualLine items[MAX_VIRTUAL_LINES];
    int count;
} EditorVirtualLineList;
```

Add to `ReplRuntimeState` and `UiRenderSnapshot`.

Controller (`imrepl_ctrl.c`): call `push_replay_virtual_lines()` each frame when replay +
`expand_args` are active. This replaces the current approach where `repl_replay_annotations.c`
drives extra row injection mid-render in `ui_panels.c`.

`ui_panels.c` code panel row loop: after rendering real line N, check if any virtual lines have
`after_line_idx == N` and render them in the annotation style.

### Verification for Step 6

```bash
make test && make test-stubs
# Manual: replay with expand_args ON — annotation lines appear below commands
# Manual: change color scheme via config — UI colors update immediately
```

---

## Full verification (all steps complete)

```bash
make test && make test-stubs
make bench BENCH_ARGS="--only spike_flatten_largest"  # must stay ≤ 4.5 ms
grep -rn "\.source" . --include="*.c" --include="*.h" | grep -v "source_cmds\|source_count\|source_line\|source_idx\|repl_source"
# ^ should return 0 hits (all source[] reads gone)
# Manual end-to-end: load largest example, edit, undo, color pick, replay, search, save/load
```

## Critical files summary

| File | Steps | Key change |
|---|---|---|
| `repl_flatten.c` | 2 | Fix `flatten_get_for_var_name` to use editor buffer |
| `ui_panels.c` | 2, 5, 6 | Editor buffer for display; render highlights + virtual lines from snapshot |
| `repl_replay_annotations.c` | 2, 6 | Remove fallbacks; push virtual lines |
| `repl_export.c` | 2 | 6 read sites → editor buffer |
| `repl_editor.c` | 2 | 2 read sites → editor buffer |
| `repl_parser.c` | 3a, 3b | Write normalized text to editor buffer; remove source[] field writes |
| `repl_commit.c` | 3a, 3b | Same for var-declare paths |
| `sample.h:264` | 3b | Delete `char source[MAX_LINE_LEN]` from `GLCmd` |
| `repl_undo.h / repl_undo.c` | 3c | Add `editor_lines[][]` to snapshot; save/restore it |
| `tests/test_repl_editor.c` | 3d | ~40 assertion sites → `repl_state_editor_buffer_line(N)` |
| `tests/test_scene_guides.c` | 3d | ~10 fixture writes → `repl_state_editor_buffer_set_line()` |
| `repl_command_store.c` | 3b, 4 | Remove `write_color_source`; color pick → buffer set + mark dirty |
| `editor_transformer.h` (new) | 4 | `EditorTransformer` / `EditorTransformerList` types |
| `editor_highlight.h` (new) | 5 | `EditorHighlight` / `EditorHighlightList` types |
| `imrepl_ctrl.c` | 4, 5, 6 | Push transformers, highlights, virtual lines each frame |
| `repl_state.h` | 4, 5, 6 | Add transformer/highlight/virtual-line slices to `ReplRuntimeState` |
| `ui_snapshot.h` | 5, 6 | Include highlight/virtual-line lists in `UiRenderSnapshot` |
