# Plan: Editor-Owns-Text Redesign — Steps 2–6

## Context

Step 1 (spike, commit a8a6569) validated the editor-buffer approach: flatten on 3,580 largest example ≤ 4.2 ms (invisible at edit time). The spike added `ReplEditorBuffer` to `ReplRuntimeState` and mirrors every parser write of `cmd->source` text into `editor_buffer.lines[]`.

**Goal of Steps 2–6**: Make `GLCmd` a pure parse-result struct (type, args[], flags only) — delete `source[256]` field and move all text ownership to the editor buffer. This completes the spike and unblocks future phases (UI-driven transformers, virtual annotations, replay snapshots).

---

## Architecture Alignment & Key Improvements

The plan follows the established snapshot pattern from `ARCHITECTURE.md`:
- Controller pushes immutable data structures (`EditorTransformer[]`, `EditorHighlight[]`, `EditorVirtualLine[]`) to UI each frame
- UI renderers read snapshots, never call `repl_state_*()` directly
- Mutations stay on the owner side (editor → buffer, replay → fade plan, etc.)
- Pattern matches `SceneRenderConfig` and `UiRenderSnapshot` precedent

**Critical improvements from code review**:
1. **Parser stays pure** — no side effects to editor buffer. Only commit path writes. Keeps flatten/replay re-parses safe.
2. **Step 2.5 introduces text-aware store APIs** — explicit `insert_many()`, `replace_one()`, `load()` handle text movement in parallel with command arrays. Makes Step 3 mechanical and reversible.
3. **Canonical buffer shape** — normalized committed text only (with indent/semicolon). All reads now go through editor buffer, not cmd->source.
4. **Color picker uses explicit reparse** — `replace_one()` updates document_cmds before UI reads them, avoiding stale arg visibility.
5. **Virtual lines are layout-affecting** — not just paint. Scroll, hit-test, search, visible-row-count all include them in the layout model.
6. **Flat-command text helper** — flat commands need text from source commands. Add `repl_flat_cmd_text(flat_idx)` backed by src_cmd_idx→editor_buffer.
7. **Parser returns normalized text** — commit code cannot reconstruct parser-normalized text reliably once source[] is gone. Parser must return text alongside cmd.
8. **Store API semantics preserved** — extend existing signatures with text params instead of replacing them; keep store/flags/edit_line behavior for cursor/undo/load.

---

## Editor Buffer Invariant

**Buffer shape**: Contains **normalized committed text only** (with indent and semicolon). This is what the parser writes, what display/export consume, and what undo snapshots preserve. This is distinct from raw user input (which lives in `ReplEditorInputState`).

## Current Spike State (on main)

| Item | Status |
|---|---|
| `ReplEditorBuffer` in `ReplRuntimeState` | ✅ |
| `repl_state_editor_buffer_line(idx)` / `_set_line()` API | ✅ |
| `spike_text_for()` in repl_flatten.c (buffer+fallback) | ✅ |
| Commit path mirrors writes to editor buffer | ✅ |
| 6+ fallback reads in ui_panels.c, repl_replay_annotations.c, repl_export.c | ✅ (partial, Step 2 completes) |
| cmd->source still in GLCmd | Still there (removed in Step 3) |

---

## Step 2: Migrate All cmd->source Reads to Editor Buffer API (~4 days)

**Goal**: Make editor buffer the sole text source for display, export, annotations, search. cmd->source is written by commit path only; read by nothing else.

**Key constraint**: Parser remains pure (no side effects to editor buffer). Only `repl_command_store.c` commit path writes buffer. This keeps flatten/replay re-parses from mutating committed text.

**Prerequisite**: Spike already merged; all files buildable.

### 2a — Fix repl_flatten.c reads (lines 51, 205, 280, 284, 340, 401)

Multiple sites read `cmd->source` directly or via embedded calls:

- **Line 51** (`flatten_get_for_var_name`): Add `int cmd_idx` parameter, replace `const char *p = cmd->source` with `const char *p = spike_text_for(src_cmd, cmd_idx)`. Update 1 call site (line 198).

- **Lines 205, 280, 284, 340, 401** (various re-parse checks): Replace `src_cmd->source` with `spike_text_for(src_cmd, src_cmd_idx)`. These are fallback-safe (spike_text_for already exists).

### 2b — Fix repl_export.c reads (lines 715, 1296, 1313, 1386, 1459, 1466, 2217, 2675, 2717)

Direct and helper-level reads:

- **Lines 1386, 1459, 1466, 2217, 2675, 2717** (`repl_state_document_cmds_mut()[cmd_idx].source`): Switch to `repl_state_editor_buffer_line(cmd_idx)`.

- **Lines 715, 1296, 1313** (helper function args, e.g. `export_command_text()`): Trace callers to get cmd_idx context, add parameter if needed, switch reads.

- **Line 1250–1252** (`format_cmd_source_as_c`): arg to `repl_eval_expr_to_c(cmd->source, ...)` — update to pass buffer text instead.

- **Lines 1587–1618** (@declare reconstruction): keep as-is; builds temporary normalized text for new insertion, not reading the store.

### 2c — Fix ui_panels.c:567

Already has fallback (from spike); stabilize it:
```c
const char *hl_text = repl_state_editor_buffer_line(i);
if (!hl_text || !hl_text[0]) hl_text = document_cmds[i].source;
```

No behavior change.

### 2d — Fix repl_replay_annotations.c fallbacks (lines 645, 698, 731, 735)

Replace fallback reads:
```c
// before
base = document_cmds[cmd_idx].source;
// after
base = repl_state_editor_buffer_line(cmd_idx);
```

Keep fallback or add assert for a release or two.

### 2e — Fix repl_editor.c (lines 200, 368)

Both read `document_cmds[idx].source` — switch to `repl_state_editor_buffer_line(idx)`.

### Verification for Step 2

```bash
make test && make test-stubs

# Audit .source reads by category (commit-path writes in repl_parser.c/repl_commit.c are OK):

# 1. Document text reads (should all use repl_state_editor_buffer_line):
echo "=== Document reads (should use editor buffer) ==="
grep -n "document_cmds\[.*\]\.source\|document_cmds->source" \
  repl_flatten.c repl_export.c repl_editor.c repl_replay_annotations.c ui_panels.c \
  | grep -v "repl_state_editor_buffer"
# Should return 0 hits

# 2. Flat-program reads (should use spike_text_for; later repl_flat_cmd_text):
echo "=== Flat command reads (should use spike_text_for) ==="
grep -n "flat_cmd.*\.source\|flat_cmds\[.*\]\.source" \
  repl_executor.c repl_flatten.c repl_replay_annotations.c repl_debug.c \
  | grep -v "spike_text_for"
# Should return 0 hits

# 3. Parser/commit writes (expected; these feed the buffer):
echo "=== Parser/commit writes (expected in repl_parser.c, repl_commit.c) ==="
grep -n "cmd->source\|cmd\.source" repl_parser.c repl_commit.c | head -20

# 4. Test fixtures (should migrate to pass text arrays to store APIs):
echo "=== Test fixtures (update to pass lines to store load) ==="
grep -n "source_cmds\[.*\]\.source" tests/*.c | wc -l
```

---

## Step 2.5: Text-Aware Command Store APIs (~3 days)

**Goal**: Before removing `cmd->source[]`, introduce explicit text-movement APIs so the store can shift lines in parallel with command arrays. This makes Step 3 mostly mechanical.

**Rationale**: Current insert/delete/load rely on re-reading `cmd->source` after array movement (e.g., repl_command_store.c:187). Once source is removed, we need to move text arrays in lockstep with command arrays.

### Extend existing store APIs with text parameters (repl_command_store.h)

Keep existing function signatures and semantics (store pointer, insert flags, edit_line handling for cursor/undo). Add optional text parameters:

```c
/* Existing signatures preserved; add optional text parameter */
int repl_command_store_insert(ReplCommandStore *store, int at_idx, 
                               const GLCmd *cmd, int insert_flags,
                               const char *text);  /* NEW: normalized source text */

int repl_command_store_replace(ReplCommandStore *store, int idx,
                                const GLCmd *cmd,
                                const char *text);  /* NEW: normalized source text */

int repl_command_store_load(ReplCommandStore *store, const GLCmd *cmds, int count,
                             const char *const *lines,  /* NEW: text parallel array */
                             int *out_edit_line);  /* existing edit_line semantics */
```

### Implement in repl_command_store.c

- **insert(store, at_idx, cmd, flags, text)**: 
  - Call existing insert logic (handles array shift, edit_line, undo)
  - If text provided, also shift `editor_buffer.lines[]` in parallel
  - Call `repl_state_editor_buffer_set_count()` to update line count

- **replace(store, idx, cmd, text)**:
  - Replace `cmds[idx]` with new cmd
  - If text provided, call `repl_state_editor_buffer_set_line(idx, text)`

- **load(store, cmds, lines, count, out_edit_line)**:
  - Call existing load logic (handles undo snapshot ownership, cursor adjustment, edit_line)
  - If lines provided, also load `editor_buffer.lines[]` in parallel
  - Call `repl_state_editor_buffer_set_count(count)`

### Migrate call sites to pass text alongside cmds

**repl_commit.c** (commit flow):
- After parser returns both cmd and text, pass both to `insert()` or `replace()`

**repl_undo.c** (snapshot restore):
- `repl_undo_snapshot_restore()` calls `load()` with both cmds and lines

**Other store callers** (import/examples/restart):
- Pass lines parameter; store handles undo snapshot ownership as before

### Verification for Step 2.5

```bash
make test && make test-stubs
# Parallel text/command arrays remain synchronized:
# - Insert/delete/load leave editor_buffer consistent with cmds[]
# - Undo/redo snapshots preserve text alongside commands
```

**After Step 2.5**: Parser still writes `cmd->source` (no change), but command store is text-aware. We can now remove `source[]` from GLCmd safely because all text movement is explicit.

---

## Step 3: Delete cmd->source[] from GLCmd (~2 weeks)

This is the big diff. Done in sub-steps to keep each commit buildable. Shrinks GLCmd from ~340 → ~84 bytes per instance.

### 3a — Parser returns normalized text + cmd (~1 day)

**Parser remains pure** — no side effects to editor buffer. But it must return the normalized text it builds, because commit code cannot reconstruct it reliably once source[] is gone.

Add `ReplParsedLine` result struct:
```c
typedef struct {
    GLCmd cmd;
    char text[MAX_LINE_LEN];  /* normalized source: parser output */
} ReplParsedLine;

ReplParsedLine repl_parser_parse_command_ctx(ReplParseContext *ctx, 
                                               const char *input, ...);
```

Update all parser call sites to use return value instead of reading cmd->source afterward.

**Commit path** (repl_commit.c, repl_command_store.c):

- In `repl_commit.c` (lines ~308–317), after parser returns ReplParsedLine, pass both cmd and text to `repl_command_store_insert()` or `replace()` with text parameter.

- In `repl_command_store.c`, extend APIs write to `editor_buffer.lines[]` in parallel with array movement.

After 3a: cmd->source is still in GLCmd (unchanged), but text is explicitly moved with commands. No behavior change; foundation for 3b.

### 3a.5 — Add flat-command text helper (~1 day)

Flat commands don't have source[] (they're generated); they reference source commands via `src_cmd_idx`. Runtime paths (repl_executor.c, replay annotations) read flat command text for goto labels, if conditions, assignment RHS.

Add helper to get source text for a flat command:
```c
const char *repl_flat_cmd_text(const GLCmd *flat_cmd) {
    if (!flat_cmd || flat_cmd->src_cmd_idx < 0)
        return "";
    return repl_state_editor_buffer_line(flat_cmd->src_cmd_idx);
}
```

Update call sites in repl_executor.c and repl_replay_annotations.c:
- Lines that read `flat_cmd->source` (if any after Step 3a) → `repl_flat_cmd_text(flat_cmd)`
- This indirection survives GLCmd.source removal

### 3b — Remove source[] field from GLCmd (~3 days)

1. Delete `char source[MAX_LINE_LEN];` from `sample.h:264`.

2. Compiler surfaces all remaining cmd->source accesses as errors — fix each:

   **repl_parser.c** (~26 snprintf sites): 
   - Snprintf to local `char normalized[MAX_LINE_LEN]` for temporary parsing use
   - Return normalized text in ReplParsedLine (from 3a)
   - No more writes to editor buffer (parser stays pure)
   
   **repl_commit.c** (commit flow):
   - Already uses parser result ReplParsedLine (from 3a)
   - No changes needed; text already flows via store APIs
   
   **repl_command_store.c** (color picker, existing inserts):
   - All insert/delete/load already use text-aware APIs (from Step 2.5)
   - Color picker write (line ~155) → already passes text param
   
   **repl_executor.c** / **repl_replay_annotations.c** (flat command text):
   - Lines reading flat_cmd->source → use `repl_flat_cmd_text(flat_cmd)` (from 3a.5)
   
   **repl_core.c:230–236** (reads indent from source):
   - Pass indent as separate `int` return value from parse helper instead
   
   **repl_debug.c** (debug prints):
   - Update to use `repl_state_editor_buffer_line(i)` or `repl_flat_cmd_text()` as appropriate

3. **Update test fixtures**: Some tests construct `cmd` structs directly. They no longer have source, so update snprintf sites that built text for test setup — pass text alongside cmd arrays to `repl_command_store_load()`.

### 3c — Update ReplUndoSnapshot (~1 day)

Add text snapshot alongside command snapshot:

**repl_undo.h** — extend ReplUndoSnapshot:
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

Memory: current ~1.36 MB (4096 × 340 bytes cmd). After: 4096 × 84 + 4096 × 256 = same 4096 × 340 bytes. No increase.

**repl_undo.c**:
- `repl_undo_snapshot_save()`: also copy `editor_buffer.lines[]` into `snapshot->editor_lines[]`
- `repl_undo_snapshot_restore()`: restore `editor_lines` into `editor_buffer` before calling `repl_command_store_load()` (so flatten sees text immediately)

### 3d — Update tests (~3 days)

**tests/test_repl_editor.c** (~40 `ASSERT_STR` sites):
- All reads of `repl_state_document_cmds_mut()[N].source` → `repl_state_editor_buffer_line(N)`
- Parser ensures buffer populated; no other change needed

**tests/test_scene_guides.c** (~10 fixture writes):
- Replace `snprintf(source_cmds[N].source, ...)` with `repl_state_editor_buffer_set_line(N, text)`

**Other test files** (`test_repl_core_commit.c`, `test_repl_parser.c`, etc.):
- Compile errors guide remaining fixes

### Verification for Step 3

```bash
make test && make test-stubs
# Confirm no cmd->source references remain:
grep -rn "\.source" . --include="*.c" --include="*.h" \
  | grep -v "source_cmds\|source_count\|source_line\|source_idx\|repl_source"
# Should return 0 hits
make bench BENCH_ARGS="--only spike_flatten_largest"
# Verify: ≤ 4.5 ms
```

---

## Step 4: Transformer API — Color Picker (~1 week)

**Goal**: Color picker becomes a controller-pushed transformer. Editor renders swatch; drag rewrites text in buffer; controller re-parses next frame. Removes `repl_command_store_write_color_source` entirely.

**File**: New `editor_transformer.h` in root

### Define EditorTransformer

```c
typedef enum { TRANSFORMER_COLOR_PICKER, TRANSFORMER_NUMERIC_SLIDER } TransformerKind;

typedef struct {
    int  line_idx;
    int  char_start, char_end;   /* byte offsets into editor buffer line */
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

Add `EditorTransformerList editor_transformers;` to `ReplRuntimeState` (via `repl_state_owners.h` accessors).

### Push from controller each frame (imrepl_ctrl.c)

After flatten, scan document for color commands and push:
```c
static void push_color_transformers(void) {
    repl_state_editor_transformers_clear();
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *cmd = repl_state_document_cmd_at(i);
        if (cmd->type == CMD_COLOR || cmd->type == CMD_CLEAR_COLOR) {
            EditorTransformer t = { .line_idx = i, .kind = TRANSFORMER_COLOR_PICKER, ... };
            // Locate color args in editor buffer line text
            repl_state_editor_transformers_append(&t);
        }
    }
}
```

Call `push_color_transformers()` from `imrepl_ctrl_display_frame()` after `flatten_commands()`.

### On drag/pick (ui_color_picker.c)

Replace `repl_command_store_set_color()` and `repl_command_store_set_clear_color()` calls:

**Coverage**: Handle all editable color types:
- `CMD_COLOR3F`, `CMD_COLOR4F` — preserve indentation, alpha, tessellation context
- `CMD_TESS_COLOR` — tessellation-specific formatting
- `CMD_CLEAR_COLOR` — clear color args + optional alpha clamping

**Implementation**: 
1. Get original source text: `const char *orig = repl_state_editor_buffer_line(cmd_idx);`
2. Parse and extract indentation + original command kind
3. Format by command type:
   ```c
   char new_line[MAX_LINE_LEN];
   if (cmd->type == CMD_COLOR3F)
       snprintf(new_line, sizeof(new_line), "%sglColor3f(%g, %g, %g);", indent, r, g, b);
   else if (cmd->type == CMD_COLOR4F)
       snprintf(new_line, sizeof(new_line), "%sglColor4f(%g, %g, %g, %g);", indent, r, g, b, a);
   else if (cmd->type == CMD_CLEAR_COLOR)
       snprintf(new_line, sizeof(new_line), "%sglClearColor(%g, %g, %g, %g);", indent, r, g, b, a);
   // ... etc
   ```
4. Re-parse synchronously via `repl_command_store_replace(store, cmd_idx, ..., new_line)` (uses store API to update both cmd and buffer)
5. Mark flat dirty for re-flatten next frame

**Why store.replace()**: Ensures document_cmds[cmd_idx] is re-parsed and updated before color picker reads args again (at lines 116, 386). Otherwise, stale cmd args could be displayed.

Update both call sites in `ui_color_picker.c:94,97`.

### Cleanup

Delete `repl_command_store_write_color_source()` (repl_command_store.c:97–164).
Delete `repl_command_store_set_color()` and `repl_command_store_set_clear_color()` declarations and definitions.

### Verification for Step 4

```bash
make test && make test-stubs
# Manual: open color picker, drag color, confirm text in code panel updates
# Manual: undo after color pick — text reverts correctly
```

---

## Step 5: Cross-line Highlight API (~1 week)

**Goal**: Controller pushes `EditorHighlight[]` each frame. UI renders from snapshot, not by calling `repl_find_feeding_*()` inline during draw.

**File**: New `editor_highlight.h` in root

### Define EditorHighlight

```c
typedef enum {
    HIGHLIGHT_FEEDING_NORMAL,
    HIGHLIGHT_FEEDING_COLOR,
    HIGHLIGHT_REPLAY_PC,
    HIGHLIGHT_SEARCH_MATCH,
    HIGHLIGHT_SELECTION,
} HighlightKind;

typedef struct {
    int  line_idx;
    int  char_start, char_end;  /* -1, -1 = whole line */
    HighlightKind kind;
} EditorHighlight;

#define MAX_HIGHLIGHTS 256
typedef struct {
    EditorHighlight items[MAX_HIGHLIGHTS];
    int count;
} EditorHighlightList;
```

Add `EditorHighlightList editor_highlights;` to `ReplRuntimeState`, include in `UiRenderSnapshot`.

### Push from controller each frame (imrepl_ctrl.c)

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
    
    push_search_highlights();  /* iterate search matches in visible range */
}
```

Call `push_highlights()` from `imrepl_ctrl_display_frame()`.

### Render from snapshot (ui_panels.c)

Code panel row loop reads `snap->editor_highlights` instead of calling `repl_find_feeding_normal_cmd()` inline. Remove inline function calls from render path.

### Verification for Step 5

```bash
make test && make test-stubs
# Manual: cursor on glVertex3f — feeding normal/color lines get gutter accent
# Manual: replay active — PC line highlighted correctly
# Manual: search active — matches highlighted across all visible rows
```

---

## Step 6: Config Extraction + Virtual Lines (~3 days)

**Goal**: Color scheme and syntax rules become snapshots. Replay annotations become virtual lines (not inline row injection mid-render).

### Color scheme as snapshot data

Move hardcoded colors from `ui_panels.c` into `EditorColorScheme` struct in `repl_actions.c` (alongside `g_cfg_items[]`). Include in `UiRenderSnapshot`. Renderers read colors from snapshot, not compile-time constants.

Syntax keywords (code-panel token coloring) → `SyntaxKeyword[]` table in same location.

### Virtual lines for replay annotations (editor_virtual_lines.h)

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

### Push virtual lines from controller (imrepl_ctrl.c)

When replay is active + expand_args ON:
```c
static void push_replay_virtual_lines(void) {
    repl_state_editor_virtual_lines_clear();
    if (!repl_state_replay_playing() || !expand_args_enabled())
        return;
    // repl_replay_annotations.c builds annotation text for each visible command
    // Push as virtual lines keyed to source line indices
}
```

Call from `imrepl_ctrl_display_frame()`.

### Layout and render virtual lines (ui_panels.c)

Virtual lines are **layout-affecting**, not paint-only:

- **Layout**: Code panel must compute visible row count including virtual lines before scroll/cursor calculations
- **Hit testing**: Click in annotation area maps to virtual line index, not real line
- **Search**: Search result row numbers must account for virtual lines
- **Scroll**: Cursor position and scroll-to-line must skip over virtual lines

Implement as part of `ReplCodePanelRuntimeState` layout computation:
- `repl_state_code_panel_view()` (or new snapshot accessor) includes `CodePanelLayout { real_line_idx, virtual_lines_after }` for each visible row
- UI render loop iterates the layout, not raw document; draws real then virtual lines sequentially
- Replaces current approach where repl_replay_annotations.c injects rows mid-render

### Verification for Step 6

```bash
make test && make test-stubs
# Manual: replay with expand_args ON — annotation lines appear below commands
# Manual: change color scheme via config — UI colors update immediately
```

---

## Full End-to-End Verification (All Steps Complete)

```bash
make test && make test-stubs
make bench BENCH_ARGS="--only spike_flatten_largest"  # must stay ≤ 4.5 ms

# No cmd->source reads remain:
grep -rn "\.source" . --include="*.c" --include="*.h" \
  | grep -v "source_cmds\|source_count\|source_line\|source_idx\|repl_source"
# Should return 0 hits

# Manual end-to-end:
# - Load largest example
# - Edit, undo, color pick, replay, search
# - Save/load single file and workspace
# - All text operations correct
```

---

## Critical Files Summary

| File | Steps | Key change |
|---|---|---|
| **repl_flatten.c** | 2 | Fix reads (lines 51, 205, 280, 284, 340, 401) to use spike_text_for() |
| **repl_export.c** | 2 | Fix reads (lines 715, 1296, 1313, 1386, 1459, 1466, 2217, 2675, 2717) to use editor buffer |
| **ui_panels.c** | 2, 5, 6 | Stabilize buffer fallback; render highlights + layout-aware virtual lines |
| **repl_replay_annotations.c** | 2, 3a.5, 6 | Remove fallbacks; use repl_flat_cmd_text(); push virtual lines |
| **repl_editor.c** | 2 | 2 read sites (200, 368) → editor buffer |
| **repl_parser.h** | 3a | New `ReplParsedLine { GLCmd cmd; char text[MAX_LINE_LEN]; }` return type |
| **repl_parser.c** | 3a, 3b | Return ReplParsedLine from parse_command_ctx(); snprintf to local buffer |
| **repl_command_store.h** | 2.5 | Extend existing APIs with optional text parameters |
| **repl_command_store.c** | 2.5, 3b, 4 | Implement text-aware insert/replace/load; color pick uses replace with text |
| **repl_commit.c** | 3a, 3b | Use parser's ReplParsedLine result; pass text to store APIs |
| **repl_core.c** | 3b | Read indent logic: accept int return instead of parsing source |
| **repl_executor.c** | 3a.5, 3b | Update flat-cmd reads to use repl_flat_cmd_text() |
| **repl_flat_cmd_text()** (new, repl_core.c or repl_flatten.c) | 3a.5 | Helper: retrieve source text for flat command via src_cmd_idx |
| **sample.h:264** | 3b | Delete `char source[MAX_LINE_LEN]` from GLCmd |
| **repl_undo.h** | 3c | Add `editor_lines[MAX_COMMANDS][MAX_LINE_LEN]` snapshot |
| **repl_undo.c** | 3c | Save/restore editor_lines via store APIs (which handle undo ownership) |
| **tests/test_repl_editor.c** | 3d | ~40 assertion sites → `repl_state_editor_buffer_line()` |
| **tests/test_scene_guides.c** | 3d | Test fixtures → pass lines to store load APIs |
| **tests/** | 3d | Update constructor-based cmd fixtures to pass text alongside |
| **ui_color_picker.c** | 4 | Format all color types (COLOR3F, COLOR4F, TESS_COLOR, CLEAR_COLOR); use store.replace() |
| **editor_transformer.h** (new) | 4 | EditorTransformer / list types |
| **editor_highlight.h** (new) | 5 | EditorHighlight / list types |
| **editor_virtual_lines.h** (new) | 6 | EditorVirtualLine / list types |
| **imrepl_ctrl.c** | 4, 5, 6 | Push transformers, highlights, virtual lines each frame |
| **repl_state.h / repl_state_owners.h** | 4, 5, 6 | Add transformer/highlight/virtual-line slices and accessors |
| **ui_snapshot.h** | 5, 6 | Include highlights, layout model, color scheme in snapshot |
| **repl_code_panel.c** (or new) | 6 | Layout model computation: map real/virtual lines for scroll, hit-test, search |
| **repl_actions.c** | 6 | Move color scheme + syntax rules to snapshot structs |

---

## Dependency Order

```
Step 2 (document text read migration)
  ↓
Step 2.5 (extend store APIs with text parameters)
  ↓
Step 3a (parser returns ReplParsedLine: cmd + normalized text)
  ↓
Step 3a.5 (add repl_flat_cmd_text() helper for flat command reads)
  ↓
Step 3b (delete source[], fix remaining compiles)
  ↓
Step 3c (undo snapshots)
  ↓
Step 3d (test updates) ← can run in parallel with 3b/3c
  ↓
Step 4 (color transformer — all color types, uses store.replace)
  ↓
Step 5 (highlights snapshot) ← can run in parallel with Step 4
  ↓
Step 6 (config extraction + virtual lines as layout model)
```

Steps 2–2.5–3a–3a.5–3b are sequential (structural change affecting parser, store, and all text flows). Steps 3c–3d can overlap. Steps 4–6 can be re-ordered or combined.
