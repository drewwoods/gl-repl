# Inline Numeric Swatch — Stateless Up/Down Stepper

Status: **in-review** — design ready; awaiting greenlight before
implementation. Pivoted from input-only edits to commit-on-click after
review (see §Writeback Path below).

> Canonical project copy lives at
> `plans/in-review/inline-numeric-swatch-stepper.md`.

## Context

The REPL already has stateful inline widgets that mutate source values:
the color picker (`src/subsystems/color_picker/color_picker_state.c` +
`src/ui/app/color_picker.c`) opens a floating HSV popup; the variable
panel (`src/subsystems/variable_panel/`) drags a slider to set predef
vars. Both carry peer-subsystem state — open flag, drag transaction,
session undo coalescing.

When fine-tuning a numeric arg (`glRotatef(45, ...)`,
`glColor3f(0.5, ...)`) the user wants a much cheaper affordance: a tiny
▲/▼ popup that appears next to the literal whenever the cursor is on
one, and on click steps the value by `0.05 × 10^max(0, floor(log10(|v|)))`
(so `5 ± 0.05`, `10 ± 0.5`, `0.5 ± 0.05`, `100 ± 5`).

Critically, the user wants this **purely stateless** — no peer
subsystem, no open/closed flag, no drag transaction. Every frame the
controller re-derives the swatch from live cursor + input text; every
click re-derives, applies one discrete step, and commits.

## User-Confirmed Design Choices

- **Placement:** right of the literal — two stacked 16×12px buttons
  (▲ on top, ▼ below) anchored 4px right of the numeric arg,
  vertically centered on the row.
- **Detection:** pure literal only. `5`, `-0.5`, `1e3` qualify. Skip
  `45*t`, `cos(phase)`, `(5)`, `1+2`. Rewriting expressions would
  mangle them.
- **Scope:** any function call's numeric args — standard GL/GLU/GLUT,
  user `funcN(...)`, math `cos/sin/rand`, etc. Just looks at the
  enclosing paren; no spec lookup required.
- **Effect on click:** commit the modified line through the normal
  pipeline so the scene re-renders with the new value visible
  immediately (user follow-up: *"please commit the line with the
  adjustment, so that the user can see the effect"*).

## Writeback Path — pivoted from input-only

**Original draft** proposed `editor_undo_push_snapshot()` +
`edit_op_input_replace_range()` (input-buffer-only edit). This is
incompatible with the undo system: the snapshot ring deliberately
**excludes** input-buffer text (`src/editor/undo.h`), and restore
reloads the committed source line back into input via
`load_line_to_input` (`src/editor/undo.c`). Per-click `Ctrl+Z` would
either no-op or wipe the entire in-progress line.

**Revised plan:** mirror the color picker's writeback —
`editor_commit_apply_external_change(&change, capture_undo=1)`
(`src/editor/commit.h:63`, used at
`src/subsystems/color_picker/color_picker_state.c:175`). Each click
synthesizes a rewritten source line, compiles it, and applies the
compiled change through the commit pipeline. Undo is captured per
click (no coalescing — clicks are discrete by definition).

**Gating consequence:** the swatch only makes sense on a parseable
committed line. Show only when `editor_state_edit_line() >= 0`
(`src/editor/state.h:177`) AND the input parses without error.
While the user is typing fresh input on the input row (no committed
line yet), the swatch hides.

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
              │ synthesize line text → compile → commit          ▲
              ▼                                                  │
       src/editor/commit.h                                       │
        editor_commit_apply_external_change(_,capture_undo=1)    │
              │                                                  │
              │ uses pixel anchor resolved by                    │
              ▼                                                  │
       src/ui/core/text_panel.c                                  │
        new helper: input_offset_to_pixel(buf,off) ──────────────┘
              (same wrap+scroll+indent math as the renderer)
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
   Reject anything else — operators, identifiers, nested parens,
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
    float anchor_x;    /* pixel anchor: right edge of the literal */
    float anchor_y;    /* row baseline (input-row pixel y) */
} UiNumericSwatchSnapshot;
```

### 3. Shared pixel-anchor helper (`src/ui/core/text_panel.{c,h}`)

The naive `FONT_SMALL_W * arg_end` math from the draft is wrong on
wrapped, scrolled, or indented rows. The input renderer in
`src/ui/core/text_panel.c` already does wrap+scroll+indent layout. Add
a shared helper that mirrors that math:

```c
/* Resolve a char offset in the input buffer to the pixel coordinates
 * the input-row renderer uses, honoring wrap, scroll, and indent. */
int ui_text_panel_input_offset_pixel(int char_offset,
                                     float *out_px, float *out_py);
```

Returns 0 if `char_offset` is outside the rendered range (e.g.
scrolled off). Used by `glr_ctrl_populate_numeric_swatch()` so the
swatch anchors exactly where the literal appears on screen.

### 4. Controller fill (`src/app/glr_ctrl.c`)

New `glr_ctrl_populate_numeric_swatch(UiRenderSnapshot *)` called from
`glr_ctrl_build_ui_snapshot()`. Steps:

1. `editor_state_input()` for cursor + input text.
2. Suppress (set `visible=0`) when **any** is true:
   - `editor_state_edit_line() < 0` — not editing a committed line
     (commit-on-click requires a target row).
   - `editor_state_autocomplete().match_count > 0` — popup conflict
     (ghost/hint alone do NOT suppress; only the visible match list).
   - `editor_inline_rename_active()` — rename owns keys.
   - `tutorial_active() && tutorial_block_noncommand_commit()` — the
     tutorial guard would reject the mutation (`tutorial.h:107`).
   - Empty input or `cursor_pos < 0`.
3. Else call `repl_eval_numeric_arg_at_cursor`. If `found`:
   - Try compiling the current input as-is (no rewrite); if it doesn't
     parse, set `visible=0` (commit would fail anyway).
   - Fill snapshot, calling `ui_text_panel_input_offset_pixel(arg_end,
     &anchor_x, &anchor_y)`. If the helper returns 0 (offset scrolled
     off), set `visible=0`.

### 5. Renderer + hit-test (new `src/ui/app/numeric_swatch.{c,h}`)

Mirrors `src/ui/app/color_picker.h:25-41` shape. Pure over
`UiNumericSwatchSnapshot`.

```c
void  ui_numeric_swatch_render(const UiNumericSwatchSnapshot *view,
                               const UiViewportState *vp);
UiHit ui_numeric_swatch_hit_test(const UiNumericSwatchSnapshot *view,
                                 int x, int y);
```

Geometry per user choice "right of the literal": two stacked 16×12px
buttons (▲ on top, ▼ below), 1px border, anchored 4px right of
`anchor_x`, vertically centered on `anchor_y`. Hit test returns
`UI_HIT_NUMERIC_SWATCH` with `item_idx = +1` (▲) or `-1` (▼) and no
other payload (route handler re-derives all offsets from live state).

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
— those fields have load-bearing meaning elsewhere
(source-command identity / row index). Keep the payload minimal.

### 8. Route handler (`src/app/glr_ctrl.c`)

`route_numeric_swatch_hit()` next to `route_inline_color_swatch_hit`
(currently `src/app/glr_ctrl.c:3220`); wired into the dispatch switch
inside `glr_ctrl_router_handle_code_panel_hit` (`glr_ctrl.c:3410`):

```c
static int route_numeric_swatch_hit(const UiHit *hit) {
    EditorInputView in = editor_state_input();
    ReplNumericArgAtCursor d =
        repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return 1;  /* cursor moved off mid-click */

    float step = repl_eval_swatch_step(d.value);
    float new_value = d.value + (hit->item_idx > 0 ? step : -step);
    char buf[32];
    repl_eval_format_swatch_number(new_value, buf, sizeof buf);

    /* Splice the rewritten literal into the line. */
    char new_line[REPL_INPUT_MAX_LEN];
    int n = snprintf(new_line, sizeof new_line, "%.*s%s%s",
                     d.arg_start, in.input, buf, in.input + d.arg_end);
    if (n < 0 || n >= (int)sizeof new_line) return 1;

    /* Compile + commit through the same path the color picker uses
     * (color_picker_state.c:175). Captures undo, updates source array,
     * refreshes editor buffer, marks flat-cmd dirty so the next frame
     * re-renders the scene with the new value. */
    /* ... build ReplCompileContext for the current edit line ...
     * ReplCompileResult res = repl_compile(...);
     * if (!res.ok) return 1;
     * editor_commit_apply_external_change(&res.change, /*capture_undo=*/1);
     * editor_completion_update();
     * editor_request_redraw();
     */
    return 1;
}
```

The `editor_completion_update()` + redraw mirror what `src/editor/input.c`
does after a typing-driven mutation, so the autocomplete state and
ghost text stay consistent with the new input text.

## Critical Files

- `src/repl/eval.{c,h}` — three new helpers
- `src/ui/core/text_panel.{c,h}` — new `ui_text_panel_input_offset_pixel`
  helper (shared with the renderer)
- `src/ui/app/snapshot.h` — new `UiNumericSwatchSnapshot` field
- `src/app/glr_ctrl.c` — populate fn + route handler, hooked into
  `glr_ctrl_build_ui_snapshot()` and the dispatch inside
  `glr_ctrl_router_handle_code_panel_hit` (`glr_ctrl.c:3410`)
- `src/ui/app/numeric_swatch.{c,h}` — **new** pure renderer + hit-test
  (mirror `src/ui/app/color_picker.c` shape)
- `src/ui/app/repl_code_panel.c` — add render call inside
  `ui_repl_code_panel_render` (`:1599`)
- `src/ui/app/panels.c` — add hit-test call inside `ui_panels_hit_test`
  (`:251`)
- `src/ui/core/hit.h` — new `UI_HIT_NUMERIC_SWATCH` kind + doc block
- `Makefile` — add `src/ui/app/numeric_swatch.o` to the UI app sources
  list AND register the new test executables (`test_eval_numeric_arg`,
  `test_numeric_swatch_route`) alongside existing test target rules

## What NOT to Do

- **No new `src/subsystems/numeric_swatch_state.c`** — the widget is
  stateless; a peer subsystem would be architectural noise.
- **No open/closed flag, no drag state, no undo coalescing.** Discrete
  clicks each get their own undo step.
- **No input-only edit primitive** (the original
  `edit_op_input_replace_range` proposal). The undo ring doesn't track
  input-buffer text — see *Writeback Path* above. We use the commit
  pipeline.
- **No packing offsets into `cmd_idx`/`line_idx`.** Those carry
  semantic meaning. Hit payload is just `item_idx`; route re-derives.
- **No new `check-numeric-swatch-ui-isolation` guard initially.** The
  renderer is small and pure-over-snapshot by construction.

## Verification

**Unit tests** (`make test-stubs` — no GL needed):

1. `tests/test_eval_numeric_arg.c` (new — add to Makefile test list):
   - Detection table: `"glVertex3f(1.0, 45*t, cos(x));"` with cursor at
     various offsets — `found=1` for slot 0; `found=0` for slot 1
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

2. `tests/test_numeric_swatch_route.c` (new — add to Makefile):
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
   - `Ctrl+Z` after a click restores the prior committed value.

**Gates** (`make check-state-ownership`):
- `check-c99` — `powf`/`log10f`/`floorf` are C99 + need `<math.h>` and
  `-lm` (already linked).
- `check-include-style` — new headers use quoted form.

**End-to-end manual check** (`./gl-repl`):

1. Load `Lit cube` example, navigate to a line containing a numeric
   arg (e.g. `glRotatef(45, 0, 1, 0)`), put cursor on the `45` —
   swatch ▲▼ should appear to its right.
2. Click ▲ → number commits to `45.05`, scene rotates accordingly, the
   swatch stays visible (cursor stays inside the new literal).
3. Move cursor off the number → swatch disappears.
4. Move cursor into `45*t` (an expression) — swatch does NOT appear.
5. `Ctrl+Z` — last click reverted, scene re-renders at prior value.
6. Open autocomplete with Tab/ghost — swatch hides while the match
   list is open.
