# Inline Numeric Swatch - Stateless Up/Down Stepper

Status: **in-review** - design ready; awaiting greenlight before
implementation. Pivoted from input-only edits to commit-on-click after
review (see §Writeback Path below).

> Canonical project copy lives at
> `plans/in-review/inline-numeric-swatch-stepper.md`.

## Context

The REPL already has stateful inline widgets that mutate source values:
the color picker (`src/subsystems/color_picker/color_picker_state.c` +
`src/ui/app/color_picker.c`) opens a floating HSV popup; the variable
panel (`src/subsystems/variable_panel/`) drags a slider to set predef
vars. Both carry peer-subsystem state - open flag, drag transaction,
session undo coalescing.

When fine-tuning a numeric arg (`glRotatef(45, ...)`,
`glColor3f(0.5, ...)`) the user wants a much cheaper affordance: a tiny
▲/▼ popup that appears next to the literal whenever the cursor is on
one, and on click steps the value by `0.05 × 10^max(0, floor(log10(|v|)))`
(so `5 ± 0.05`, `10 ± 0.5`, `0.5 ± 0.05`, `100 ± 5`).

Critically, the user wants this **purely stateless** - no peer
subsystem, no open/closed flag, no drag transaction. Every frame the
controller re-derives the swatch from live cursor + input text; every
click re-derives, applies one discrete step, and commits.

## User-Confirmed Design Choices

- **Placement:** right edge of the code panel - two stacked 16×12px
  buttons (▲ on top, ▼ below) anchored at the panel's right margin,
  vertically centered on the input row. Avoids overlap with adjacent
  argument text that the original "right of literal" placement caused.
- **Detection:** pure literal only. `5`, `-0.5`, `1e3` qualify. Skip
  `45*t`, `cos(phase)`, `(5)`, `1+2`. Rewriting expressions would
  mangle them.
- **Scope:** any function call's numeric args - standard GL/GLU/GLUT,
  user `funcN(...)`, math `cos/sin/rand`, etc. Just looks at the
  enclosing paren; no spec lookup required.
- **Effect on click:** commit the modified line through the normal
  pipeline so the scene re-renders with the new value visible
  immediately (user follow-up: *"please commit the line with the
  adjustment, so that the user can see the effect"*).

## Writeback Path - pivoted from input-only

**Original draft** proposed `editor_undo_push_snapshot()` +
`edit_op_input_replace_range()` (input-buffer-only edit). This is
incompatible with the undo system: the snapshot ring deliberately
**excludes** input-buffer text (`src/editor/undo.h`), and restore
reloads the committed source line back into input via
`load_line_to_input` (`src/editor/undo.c`). Per-click `Ctrl+Z` would
either no-op or wipe the entire in-progress line.

**Revised plan:** mirror the color picker's writeback -
`editor_commit_apply_external_change(&change, capture_undo=1)`
(`src/editor/commit.h:63`, used at
`src/subsystems/color_picker/color_picker_state.c:175`). Each click
synthesizes a rewritten source line, parses it as one REPL command,
builds a `REPL_COMPILED_REPLACE_ONE`, and applies that change through
the commit pipeline immediately. The scene sees the adjusted command on
the next redraw after the same click. Undo is captured per click (no
coalescing - clicks are discrete by definition).

**Gating consequence:** the swatch only makes sense on a parseable
committed line. Show only when the editor is in overwrite/edit mode:
`!editor_insert_mode()` AND `editor_state_edit_line() <
repl_state_document_count()`. Do **not** use `edit_line >= 0` as the
fresh-input test; `editor_state_edit_line_set()` clamps negatives to
zero (`src/editor/state.c:262`). While the user is typing fresh input
in insert mode, the swatch hides.

## Architecture

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│ src/repl/eval.{c,h}          │         │ src/ui/app/numeric_swatch.{c,h}│
│  pure helpers (testable)     │         │  pure renderer + hit-test     │
│  - numeric_arg_at_cursor     │         │  - render(view)               │
│  - swatch_step               │         │  - hit_test(view) → UiHit     │
│  - format_swatch_number      │         └──────────────────────────────┘
└──────────────────────────────┘                       ▲
              ▲                                        │ called from
              │ called by                              │
              │                                        │
┌──────────────────────────────┐         ┌──────────────────────────────┐
│ src/app/glr_ctrl.c           │ fills   │ src/ui/app/repl_code_panel.c │
│  - populate_numeric_swatch() ├────────►│  - render call into          │
│  - route_numeric_swatch_hit()│         │    ui_repl_code_panel_render │
└──────────────────────────────┘         │  - hit emission near :1681   │
              │ on click:                └──────────────────────────────┘
              │ synthesize line text → parse → REPLACE_ONE commit ▲
              ▼                                                  │
       src/editor/commit.h                                       │
        editor_commit_apply_external_change(_,capture_undo=1)    │
              │                                                  │
              │ uses input-row y resolved by                     │
              ▼                                                  │
       src/ui/app/repl_code_panel.{c,h}                         │
        ui_repl_code_panel_input_row_y(snap) ───────────────────┘
              (builds the same rows as render/hit-test; delegates
               y-math to a pure text-panel helper over a snapshot+row)
```

No new file under `src/subsystems/`. The widget owns zero state.

## Step Formula

```c
float repl_eval_swatch_step(float value) {
    float mag = fabsf(value);
    float exp10 = (mag < 10.0f) ? 0.0f : floorf(log10f(mag));
    return 0.05f * powf(10.0f, exp10);
}
```

Matches the user's `5→0.05`, `10→0.5`, `0.5→0.05`, `100→5`, `1000→50`.
Zero → 0.05 (so `▲` on `0` = `0.05`). Sign-preserving across zero
crossings: `new = value ± step` with no clamp.

## Implementation Steps

### 1. Pure detection + step + format helpers (`src/repl/eval.{c,h}`)

Append next to `repl_scan_next_arg_delim` (`eval.h:245`) and
`repl_eval_expr` (`eval.h:228`).

```c
typedef struct {
    int   found;       /* 1 if cursor sits in a pure numeric literal arg */
    int   arg_start;   /* offset of first char (incl. leading sign) */
    int   arg_end;     /* offset one past last digit/decimal/exponent */
    float value;       /* parsed via repl_eval_expr */
} ReplNumericArgAtCursor;

ReplNumericArgAtCursor repl_eval_numeric_arg_at_cursor(const char *src, int cursor);
float                  repl_eval_swatch_step(float value);
void                   repl_eval_format_swatch_number(float v, char *out, int out_sz);
```

**Algorithm** (mirrors the depth-walk in `build_param_hint_text` at
`src/app/glr_completion.c:46-100` + the slot scanner in
`parse_vertex_arg_slots` at `src/app/glr_ctrl.c:111`):

1. Scan right-to-left from `cursor` tracking `()`/`{}`/`[]` depth; find
   the innermost enclosing `(`. Any function call's args qualify.
2. From that `(`, walk top-level slots with `repl_scan_next_arg_delim`
   (`src/repl/eval.c:984`). Pick the slot whose `[start, end)` contains
   `cursor`. Cursor on `,` belongs to the right slot; cursor on `)` →
   not found.
3. Trim whitespace inside the slot to literal text `[t_lo, t_hi)`.
4. **Pure-literal validator**: optional leading `+`/`-`, digits,
   optional single `.`, more digits, optional `[eE][+-]?digits`.
   Reject anything else - operators, identifiers, nested parens,
   function calls.
5. Call `repl_eval_expr()` for `value`.

**`format_swatch_number`:** use `%.6g`, matching the project's universal
float-printing style (`src/repl/export.c:294`, `parser.c:778-1032`).
Strips trailing zeros; round-trips through `repl_eval_expr`.

### 2. UI snapshot field (`src/ui/app/snapshot.h`)

Add by-value field grouped near `color_picker` (line 87):

```c
typedef struct {
    int   visible;     /* 0 = render nothing */
    int   arg_start;   /* offsets into editor_state_input().input */
    int   arg_end;
    float value;       /* for the renderer (e.g. tooltip in future) */
    float step;        /* for the renderer */
    float anchor_x;    /* pixel anchor: right edge of code panel minus margin */
    float anchor_y;    /* row baseline (input-row pixel y) */
} UiNumericSwatchSnapshot;
```

### 3. Input-row pixel-y helper (`src/ui/core/text_panel.{c,h}` +
`src/ui/app/repl_code_panel.{c,h}`)

With the swatch anchored at the panel's right edge, `anchor_x` is
trivially `cp_x + cp_w - SWATCH_TOTAL_W - margin` - no character-offset
math needed. `anchor_y` still requires resolving the input row's pixel
baseline through the same wrap+scroll+indent layout the renderer uses.
Add a pure core helper:

```c
/* Return the pixel baseline y-coordinate of the input row identified by
 * `input_row_idx` (index into snap->rows).  Honors wrap, scroll, top
 * chrome, and row indent.  Writes *out_py and returns 1 on success;
 * returns 0 if the row is scrolled off-screen.  No live-state reads. */
int ui_text_panel_input_row_y(const UiTextPanelSnapshot *snap,
                              int input_row_idx,
                              int *out_py);
```

Then add the REPL adapter wrapper:

```c
int ui_repl_code_panel_input_row_y(const UiRenderSnapshot *snap,
                                   float *out_py);
```

The wrapper builds the same `ReplCodePanelBuilder` rows as
`ui_repl_code_panel_render()` / `ui_repl_code_panel_hit_test()`, finds the
active input row, and delegates to `ui_text_panel_input_row_y`.
Returns 0 if the input row is scrolled off-screen.
Used by `glr_ctrl_populate_numeric_swatch()` so the swatch's vertical
anchor tracks the input row exactly without UI code reading live
editor/REPL state.

### 4. Controller fill (`src/app/glr_ctrl.c`)

New `glr_ctrl_populate_numeric_swatch(UiRenderSnapshot *)` called from
`glr_ctrl_build_ui_snapshot()`. Steps:

1. `editor_state_input()` for cursor + input text.
2. Suppress (set `visible=0`) when **any** is true:
   - `editor_insert_mode()` - fresh/insertion input, not an overwrite edit
     of a committed row.
   - `editor_state_edit_line() < 0 ||
     editor_state_edit_line() >= repl_state_document_count()` - no committed
     target row. (`< 0` is defensive; in practice the setter clamps.)
   - `repl_state_document_cmds()[edit_line].type == CMD_COMMENT` - the
     committed line is a comment. Stepping a number inside a commented-out
     GL call would change the comment text with no scene effect and burn
     an undo slot.
   - The input row carries an inline color swatch
     (`right_action.active != 0` on the `UiTextPanelRow` for the edit
     line - the same field `repl_code_panel_set_right_action` fills).
     The color swatch takes precedence; showing both on the same row
     is redundant (the color picker already handles those args).
     Implementation: the row builder runs before
     `glr_ctrl_populate_numeric_swatch`, so the adapter can expose a
     `ui_repl_code_panel_input_row_has_color_swatch(snap)` query, or the
     controller can check `color_picker_view()->visible` as a coarser
     gate (suppresses whenever the picker popup is open on any line).
     Prefer the per-row check - it's more precise and has no ordering
     dependency on the picker subsystem.
   - `editor_state_autocomplete().match_count > 0` - popup conflict
     (ghost/hint alone do NOT suppress; only the visible match list).
   - `editor_inline_rename_active()` - rename owns keys.
   - `tutorial_active() && tutorial_block_noncommand_commit()` - the
     tutorial guard would reject the mutation (`tutorial.h:107`).
   - Empty input or `cursor_pos < 0`.
3. Else call `repl_eval_numeric_arg_at_cursor`. If `found`:
   - Try parsing the current input as a single REPL command with
     `repl_parser_parse_command_ctx` (same parser shape the color picker
     writeback uses, not `repl_compile_dispatch`, which only covers float
     declarations / assignments). If parse fails or
     `pl.cmd.type == CMD_COMMENT`, set `visible=0`.
   - Fill snapshot: `anchor_x = cp_x + cp_w - SWATCH_TOTAL_W - 4`
     (right edge of code panel); `anchor_y` via
     `ui_repl_code_panel_input_row_y(snap, &anchor_y)`. If the helper
     returns 0 (input row scrolled off), set `visible=0`.

### 5. Renderer + hit-test (new `src/ui/app/numeric_swatch.{c,h}`)

Mirrors `src/ui/app/color_picker.h:25-41` shape. Pure over
`UiNumericSwatchSnapshot`.

```c
void  ui_numeric_swatch_render(const UiNumericSwatchSnapshot *view,
                               const UiViewportState *vp);
UiHit ui_numeric_swatch_hit_test(const UiNumericSwatchSnapshot *view,
                                 int x, int y);
```

Geometry: two stacked 16×12px buttons (up on top, down below), 1px
border, positioned at `anchor_x` (panel right edge), vertically centered
on `anchor_y` (input-row baseline). Draw the arrows as tiny GL
triangles or ASCII `^`/`v`; do not rely on UTF-8 `▲`/`▼` through
`glutBitmapCharacter`, which consumes bytes rather than Unicode codepoints.
Hit test returns `UI_HIT_NUMERIC_SWATCH` with `item_idx = +1` (increment)
or `-1` (decrement) and no other payload (route handler re-derives all
offsets from live state).

### 6. Wiring into the code-panel render + hit pipeline

**Render:** add a call to `ui_numeric_swatch_render(&snap->numeric_swatch, vp)`
inside `ui_repl_code_panel_render` (`src/ui/app/repl_code_panel.c:1599`),
after the inline color swatches and other overlays.

**Hit-test:** inside `ui_panels_hit_test` (`src/ui/app/panels.c:251`),
before returning the code-panel hit, call
`ui_numeric_swatch_hit_test(&snap->numeric_swatch, mx, my)` and return
its result if non-NONE. This sits at the same precedence as the
inline color swatch (around `repl_code_panel.c:1681`).

### 7. New `UiHitKind` (`src/ui/core/hit.h`)

Add one kind near `UI_HIT_INLINE_COLOR_SWATCH` (line 26):

```c
UI_HIT_NUMERIC_SWATCH,   /* inline numeric stepper (item_idx = +1 / -1) */
```

Document field semantics in the block at line 79 onward:

```
UI_HIT_NUMERIC_SWATCH
  item_idx = direction (+1 = increment, -1 = decrement)
  All other fields unused (-1). Route handler re-derives the arg
  offsets from live editor state to stay correct if the user typed
  between render and click.
```

**Explicitly avoid** packing `arg_start`/`arg_end` into `cmd_idx`/`line_idx`
- those fields have load-bearing meaning elsewhere
(source-command identity / row index). Keep the payload minimal.

### 8. Route handler (`src/app/glr_ctrl.c`)

`route_numeric_swatch_hit()` next to `route_inline_color_swatch_hit`
(currently `src/app/glr_ctrl.c:3220`); wired into the dispatch switch
inside `glr_ctrl_router_handle_code_panel_hit` (`glr_ctrl.c:3410`):

```c
/* After commit + editor_load_line_to_input, the cursor is at the end of
 * the reloaded input.  Put it back on (or just past) the rewritten
 * literal so a rapid repeat click still lands inside the same arg.
 *
 * Algorithm:
 * 1. Search the reloaded input for `literal` starting near `hint_start`
 *    (the pre-commit arg_start offset).  Canonical reformatting may
 *    shift whitespace, so scan forward from max(0, hint_start - 4)
 *    through hint_start + 16.  Use strstr on successive sub-offsets.
 * 2. If found, set cursor to match_pos + strlen(literal) - the caret
 *    sits at the literal's right edge, so the next detection sees the
 *    same slot.
 * 3. If not found (e.g. heavy reformat), fall back to
 *    min(hint_start, input_len).
 *
 * Declared static in glr_ctrl.c; not exported. */
static void numeric_swatch_restore_cursor_near_literal(
    const char *literal, int hint_start);

static int route_numeric_swatch_hit(const UiHit *hit) {
    int edit_line = editor_state_edit_line();
    EditorInputView in = editor_state_input();

    if (editor_insert_mode() ||
        edit_line < 0 || edit_line >= repl_state_document_count())
        return 1;

    ReplNumericArgAtCursor d =
        repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return 1;  /* cursor moved off mid-click */

    float step = repl_eval_swatch_step(d.value);
    float new_value = d.value + (hit->item_idx > 0 ? step : -step);
    char buf[32];
    repl_eval_format_swatch_number(new_value, buf, sizeof buf);

    /* Splice the rewritten literal into the line. */
    char new_line[MAX_LINE_LEN];
    int n = snprintf(new_line, sizeof new_line, "%.*s%s%s",
                     d.arg_start, in.input, buf, in.input + d.arg_end);
    if (n < 0 || n >= (int)sizeof new_line) return 1;

    /* Parse + commit through the same shape the color picker uses:
     * parse the rewritten single command, build a REPLACE_ONE change,
     * and send it through editor_commit_apply_external_change().
     * Do NOT use repl_compile_dispatch here; it only handles float
     * declarations and assignments, not ordinary GL calls. */
    char parse_err[REPL_STATUS_TEXT_MAX] = "";
    ReplParseContext parse_ctx = {
        .source_line_idx = edit_line,
        .err_buf = parse_err,
        .err_sz = (int)sizeof parse_err,
    };
    ReplParsedLine pl;
    if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx)) {
        if (parse_err[0]) repl_set_status(parse_err);
        return 1;
    }

    /* Reject comments - stepping a number inside "// glVertex3f(5,...)"
     * would change comment text with no scene effect. */
    if (pl.cmd.type == CMD_COMMENT) return 1;

    ReplCompiledChange change;
    repl_compiled_change_init(&change);
    change.kind = REPL_COMPILED_REPLACE_ONE;
    change.pos = edit_line;
    change.count = 1;
    change.cmds[0] = pl.cmd;
    /* Match color_picker_state.c:170-173 - explicit length + memcpy. */
    int text_len = (int)strlen(pl.text);
    if (text_len >= MAX_LINE_LEN) text_len = MAX_LINE_LEN - 1;
    memcpy(change.text[0], pl.text, (size_t)text_len);
    change.text[0][text_len] = '\0';

    if (editor_commit_apply_external_change(&change, /*capture_undo=*/1)) {
        /* External-change apply updates the source buffer and command
         * store, but it does not refresh the live input row. Reload it
         * so the active row shows the committed value, then restore
         * cursor position so rapid repeat clicks keep working. */
        editor_load_line_to_input(edit_line);
        numeric_swatch_restore_cursor_near_literal(buf, d.arg_start);
        editor_completion_update();
        editor_request_redraw();
    }
    return 1;
}
```

The `editor_completion_update()` + redraw mirror what `src/editor/input.c`
does after a typing-driven mutation, so the autocomplete state and
ghost text stay consistent with the new input text.

## Critical Files

- `src/repl/eval.{c,h}` - three new helpers
- `src/ui/core/text_panel.{c,h}` - new pure
  `ui_text_panel_input_row_y` helper over `UiTextPanelSnapshot`
  + input-row index
- `src/ui/app/repl_code_panel.{c,h}` - new
  `ui_repl_code_panel_input_row_y` adapter wrapper +
  `ui_repl_code_panel_input_row_has_color_swatch` query, plus render
  call inside `ui_repl_code_panel_render` (`:1599`)
- `src/ui/app/snapshot.h` - new `UiNumericSwatchSnapshot` field
- `src/app/glr_ctrl.c` - populate fn + route handler, hooked into
  `glr_ctrl_build_ui_snapshot()` and the dispatch inside
  `glr_ctrl_router_handle_code_panel_hit` (`glr_ctrl.c:3410`)
- `src/ui/app/numeric_swatch.{c,h}` - **new** pure renderer + hit-test
  (mirror `src/ui/app/color_picker.c` shape)
- `src/ui/app/panels.c` - add hit-test call inside `ui_panels_hit_test`
  (`:251`)
- `src/ui/core/hit.h` - new `UI_HIT_NUMERIC_SWATCH` kind + doc block
- `Makefile` - add `src/ui/app/numeric_swatch.o` to the UI app sources
  list AND register the new test executables (`test_eval_numeric_arg`,
  `test_numeric_swatch_route`, `test_text_panel_input_row_y`) alongside
  existing test target rules

## What NOT to Do

- **No new `src/subsystems/numeric_swatch_state.c`** - the widget is
  stateless; a peer subsystem would be architectural noise.
- **No open/closed flag, no drag state, no undo coalescing.** Discrete
  clicks each get their own undo step.
- **No input-only edit primitive** (the original
  `edit_op_input_replace_range` proposal). The undo ring doesn't track
  input-buffer text - see *Writeback Path* above. We use the commit
  pipeline.
- **No packing offsets into `cmd_idx`/`line_idx`.** Those carry
  semantic meaning. Hit payload is just `item_idx`; route re-derives.
- **No new `check-numeric-swatch-ui-isolation` guard initially.** The
  renderer is small and pure-over-snapshot by construction.

## Verification

**Unit tests** (`make test-stubs` - no GL needed):

1. `tests/test_eval_numeric_arg.c` (new - add to Makefile test list):
   - Detection table: `"glVertex3f(1.0, 45*t, cos(x));"` with cursor at
     various offsets - `found=1` for slot 0; `found=0` for slot 1
     (expression) and slot 2 (function call).
   - Signed literal: `"glTranslatef(-0.5, 0, 0)"` → `value=-0.5`.
   - Exponent: `"glPointSize(1e3)"` → `value=1000`.
   - Cursor exactly on `(`, `,`, `)` boundaries.
   - User funcN: `"func3(0.5, 1, 2);"` → `found=1`.
   - Step table: `5→0.05`, `10→0.5`, `0.5→0.05`, `100→5`, `1000→50`,
     `0→0.05`, `-5→0.05`.
   - Format round-trip: `format_swatch_number(5.05)` → `"5.05"`;
     `format_swatch_number(0.1)` → `"0.1"`; each reparses to the same
     float via `repl_eval_expr`.

2. `tests/test_numeric_swatch_route.c` (new - add to Makefile):
   - Drive **through the public entry**
     `glr_ctrl_router_handle_code_panel_hit()`, not the static
     `route_numeric_swatch_hit` directly.
   - Commit `"glVertex3f(5, 0, 0);"` as a source line; navigate cursor
     onto the `5`; build snapshot; assert `numeric_swatch.visible == 1`.
   - Fabricate `UiHit{kind=UI_HIT_NUMERIC_SWATCH, item_idx=+1}` and
     hand to the router. Assert the committed source line is now
     `"glVertex3f(5.05, 0, 0);"`, undo ring grew by 1, and the
     flat-cmd array marked dirty.
   - Click `▼` 10× on `100` → source ends with `50`.
   - Cursor on `45*t`: snapshot `visible == 0`.
   - Suppression: open autocomplete (set `match_count > 0`), assert
     `visible == 0`.
   - Suppression: typing on fresh input (`edit_line < 0`), assert
     `visible == 0`.
   - Suppression: comment line (`"// glVertex3f(5, 0, 0);"` committed
     as `CMD_COMMENT`, cursor on `5`) → `visible == 0`.
   - Suppression: color line (`"glColor3f(1, 0, 0);"`, cursor on `1`)
     → input row has color swatch → `visible == 0`.
   - Route handler on comment: feed a
     `UiHit{kind=UI_HIT_NUMERIC_SWATCH, item_idx=+1}` while editing a
     comment line - assert the source line is unchanged (CMD_COMMENT
     guard rejects).
   - `Ctrl+Z` after a click restores the prior committed value.

3. `tests/test_text_panel_input_row_y.c` (new - add to Makefile):
   - Build a `UiTextPanelSnapshot` with a few rows and an input row at
     a known index; call `ui_text_panel_input_row_y` and assert the
     returned y matches expected `cp_y + cp_h - top_chrome_h -
     statusbar_h - (visual_row - scroll) * LINE_H`.
   - Wrapping: add a long preceding row that wraps to 2 visual lines;
     assert the input row's y shifts down by one `LINE_H`.
   - Scrolled off: set scroll past the input row; assert returns 0.

**Gates** (`make check-state-ownership`):
- `check-c99` - `powf`/`log10f`/`floorf` are C99 + need `<math.h>` and
  `-lm` (already linked).
- `check-include-style` - new headers use quoted form.

**End-to-end manual check** (`./gl-repl`):

1. Load `Lit cube` example, navigate to a line containing a numeric
   arg (e.g. `glRotatef(45, 0, 1, 0)`), put cursor on the `45` -
   swatch ▲▼ should appear at the right edge of the code panel,
   vertically aligned with the input row.
2. Click ▲ → number commits to `45.05`, scene rotates accordingly, the
   swatch stays visible (cursor stays inside the new literal).
3. Move cursor off the number → swatch disappears.
4. Move cursor into `45*t` (an expression) - swatch does NOT appear.
5. Navigate to a `glColor3f(...)` line, cursor on a number - swatch
   does NOT appear (color swatch takes precedence).
6. Navigate to a `// commented-out` line with numbers - swatch does
   NOT appear.
7. `Ctrl+Z` - last click reverted, scene re-renders at prior value.
8. Open autocomplete with Tab/ghost - swatch hides while the match
   list is open.
