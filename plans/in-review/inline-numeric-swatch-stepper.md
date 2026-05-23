# Inline Numeric Swatch — Stateless Up/Down Stepper

## Context

The REPL already has stateful inline widgets that mutate source values:
the color picker (`src/subsystems/color_picker/color_picker_state.c` +
`src/ui/app/color_picker.c`) opens a floating HSV popup; the variable
panel (`src/subsystems/variable_panel/`) drags a slider to set predef
vars. Both carry peer-subsystem state — open flag, drag transaction,
session undo coalescing.

When fine-tuning a numeric arg (`glRotatef(45, ...)`, `glColor3f(0.5,
...)`) the user wants a much cheaper affordance: a tiny ▲/▼ popup that
appears next to the literal whenever the cursor is on one, and on click
steps the value by `0.05 × 10^max(0, floor(log10(|v|)))` (so `5 ± 0.05`,
`10 ± 0.5`, `0.5 ± 0.05`, `100 ± 5`).

Critically, the user wants this **purely stateless** — no peer
subsystem, no open/closed flag, no drag transaction. Every frame the
controller re-derives the swatch from live cursor + input text; every
click re-derives and applies one discrete step. This consciously
departs from the color-picker/variable-panel templates because the
widget has no genuine modes.

## Architecture

```
┌──────────────────────────────┐         ┌──────────────────────────────┐
│ src/repl/eval.{c,h}          │         │ src/ui/app/numeric_swatch.{c,h}│
│  pure helpers (testable)     │         │  pure renderer + hit-test     │
│  - numeric_arg_at_cursor     │         │  - render(view)               │
│  - swatch_step               │         │  - hit_test(view) → UiHit     │
│  - format_swatch_number      │         └──────────────────────────────┘
└──────────────────────────────┘                       ▲
              ▲                                        │ reads
              │ called by                              │
              │                                        │
┌──────────────────────────────┐         ┌──────────────────────────────┐
│ src/app/glr_ctrl.c           │ fills   │ UiNumericSwatchSnapshot       │
│  - populate_numeric_swatch() ├────────►│  (new field on               │
│  - route_numeric_swatch_hit()│         │   UiRenderSnapshot)          │
└──────────────────────────────┘         └──────────────────────────────┘
              │ on click: rewrites input via
              ▼
       src/editor/edit_ops.c
        edit_op_input_replace_range  (new primitive)
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
crossings: `new = value ± step` with no clamp — `-0.02 + 0.05 = 0.03`.

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

**`numeric_arg_at_cursor` algorithm:**

1. Scan right-to-left from `cursor` tracking `()`/`{}`/`[]` depth; find
   the innermost enclosing `(`. (Mirrors the depth-walk in
   `build_param_hint_text` at `glr_completion.c:46-100`.) Any function
   call's args qualify — standard GL, user `funcN`, math `cos/sin/rand`
   — per user choice "any function call's numeric args".
2. From that `(`, walk top-level slots with `repl_scan_next_arg_delim`
   (`eval.c:984`). Pick the slot whose `[start, end)` contains
   `cursor`. Cursor on `,` belongs to the right slot; cursor on `)`
   → not found.
3. Trim whitespace inside the slot to literal text `[t_lo, t_hi)`.
4. **Pure-literal validator** (per user choice): optional leading `+`
   or `-`, then digits, optional single `.`, more digits, optional
   `[eE][+-]?digits`. Reject anything else — operators, identifiers,
   nested parens, function calls. So `45*t`, `cos(x)`, `t`, `1+2`,
   `(5)` all return `found=0`.
5. Call `repl_eval_expr()` for `value` (trivial for a pure literal;
   handles exponent uniformly).

**`format_swatch_number`:** use `%.6g`, matching the project's universal
float-printing style (`export.c:2810`, `parser.c:778-1032`,
`workspace_format_float` in `export.c:294`). Round-trips through
`repl_eval_expr`; strips trailing zeros.

### 2. UI snapshot field (`src/ui/app/snapshot.h`)

Add by-value field grouped near `color_picker` (line 87 of `snapshot.h`):

```c
typedef struct {
    int   visible;     /* 0 = render nothing */
    int   arg_start;   /* offsets into editor_state_input().input */
    int   arg_end;
    float value;
    float step;
    float anchor_x;    /* pixel anchor: right edge of the literal */
    float anchor_y;    /* row baseline */
} UiNumericSwatchSnapshot;
```

### 3. Controller fill (`src/app/glr_ctrl.c`)

New `glr_ctrl_populate_numeric_swatch(UiRenderSnapshot *)` called from
`glr_ctrl_build_ui_snapshot()`. Steps:

1. `editor_state_input()` for cursor + input text.
2. Suppress (set `visible=0`) when **any** is true:
   - `editor_state_autocomplete().visible` — popup conflict.
   - `editor_inline_rename_active()` — rename owns keys.
   - `tutorial_active() && tutorial_block_noncommand_commit()` — the
     tutorial guard would reject the mutation anyway (`tutorial.h:107`).
   - Empty input or `cursor_pos < 0`.
3. Else call `repl_eval_numeric_arg_at_cursor`. If `found`, fill the
   snapshot fields and compute pixel anchor (`anchor_x`, `anchor_y`):
   the input-row baseline from `ui_layout_code_panel_rect`
   + `FONT_SMALL_W * arg_end`. Clamp inside viewport (push left if it
   would clip right).

### 4. Renderer + hit-test (new `src/ui/app/numeric_swatch.{c,h}`)

Mirrors `color_picker.h:25-41` shape. Pure over `UiNumericSwatchSnapshot`.

```c
void  ui_numeric_swatch_render(const UiNumericSwatchSnapshot *view,
                               const UiViewportState *vp);
UiHit ui_numeric_swatch_hit_test(const UiNumericSwatchSnapshot *view,
                                 int x, int y);
```

Geometry per user choice "right of the literal": two stacked 16×12px
buttons (▲ on top, ▼ below), 1px border, anchored 4px right of
`anchor_x`, vertically centered on the row.

Hit-test returns `UI_HIT_NUMERIC_SWATCH` with `item_idx = +1` (▲) or
`-1` (▼); `cmd_idx = arg_start`, `line_idx = arg_end` so the route
handler doesn't depend on snapshot fields being live.

### 5. New `UiHitKind` (`src/ui/core/hit.h`)

Add one kind near `UI_HIT_INLINE_COLOR_SWATCH` (line 26):

```c
UI_HIT_NUMERIC_SWATCH,   /* inline numeric stepper (item_idx = +1 / -1) */
```

Document the field semantics block (currently line 79 onward) following
the `UI_HIT_VARIABLE_SLIDER` precedent (item_idx as sub-region).

### 6. Route handler (`src/app/glr_ctrl.c`)

`route_numeric_swatch_hit()` next to `route_inline_color_swatch_hit`
(currently `glr_ctrl.c:3220`); wired into the dispatch switch at
`glr_ctrl.c:3443`:

```c
static int route_numeric_swatch_hit(const UiHit *hit) {
    EditorInputView in = editor_state_input();
    ReplNumericArgAtCursor d =
        repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return 1;          /* cursor moved off mid-click */

    float step = repl_eval_swatch_step(d.value);
    float new_value = d.value + (hit->item_idx > 0 ? step : -step);

    char buf[32];
    repl_eval_format_swatch_number(new_value, buf, sizeof(buf));

    editor_undo_push_snapshot();     /* one snapshot per click */
    edit_op_input_replace_range(d.arg_start, d.arg_end, buf);
    return 1;
}
```

Re-detecting (rather than trusting `hit->cmd_idx/line_idx`) is the
defensive path — typing between render and click might have shifted
offsets.

### 7. Input-range replace primitive (`src/editor/edit_ops.{c,h}`)

```c
/* Replace input[start..end) with new_text. Cursor lands at
 * start + strlen(new_text). Selection cleared. No-op if range
 * is out of bounds. Returns 1 on success. */
int edit_op_input_replace_range(int start, int end, const char *new_text);
```

Cursor lands inside the new literal so the swatch stays visible for
rapid sequential clicks. REPL-free; passes `check-edit-ops-pure`.

## Critical Files

- `src/repl/eval.{c,h}` — three new helpers (detection, step, format)
- `src/ui/app/snapshot.h` — new `UiNumericSwatchSnapshot` field
- `src/app/glr_ctrl.c` — populate fn + route handler, hooked into
  `glr_ctrl_build_ui_snapshot()` and the mouse-press dispatch switch
  (around `glr_ctrl.c:3443`)
- `src/ui/app/numeric_swatch.{c,h}` — **new** pure renderer + hit-test
  (mirror `src/ui/app/color_picker.c` shape)
- `src/ui/core/hit.h` — new `UI_HIT_NUMERIC_SWATCH` kind + doc block
- `src/editor/edit_ops.{c,h}` — new `edit_op_input_replace_range`
- `Makefile` — add the new `numeric_swatch.o` to the UI app sources list

## What NOT to Do

- **No new `src/subsystems/numeric_swatch_state.c`** — the widget is
  stateless; a peer subsystem would be architectural noise.
- **No open/closed flag, no drag state, no undo coalescing.** Color
  picker uses `g_cp_undo_captured` (`color_picker_state.c:175`) to fold
  drag ticks into one undo step; with discrete clicks each step is one
  natural undo.
- **No `editor_commit_apply_external_change`** — that API rewrites
  *committed* source lines (`commit.h:63`). The swatch only edits the
  input buffer, so it uses `edit_op_*` primitives directly.
- **No new `check-numeric-swatch-ui-isolation` guard initially** — the
  renderer is small and pure-over-snapshot by construction; add the
  guard later if the file grows.

## Verification

**Unit tests** (`make test-stubs` — no GL needed):

1. `tests/test_eval_numeric_arg.c` (new):
   - Detection table: `"glVertex3f(1.0, 45*t, cos(x));"` with cursor at
     various offsets — `found=1` for slot 0 across positions 11–13;
     `found=0` for slot 1 (expression) and slot 2 (function call).
   - Signed literal: `"glTranslatef(-0.5, 0, 0)"` → `arg_start=13`,
     `value=-0.5`.
   - Exponent: `"glPointSize(1e3)"` → `value=1000`.
   - Cursor exactly on `(`, `,`, `)` boundaries.
   - User funcN: `"func3(0.5, 1, 2);"` → `found=1`.
   - Step table: `5→0.05`, `10→0.5`, `0.5→0.05`, `100→5`, `1000→50`,
     `0→0.05`, `-5→0.05`.
   - Format round-trip: `format_swatch_number(5.05)` → `"5.05"`;
     `format_swatch_number(0.1)` → `"0.1"`; reparses to same float via
     `repl_eval_expr`.
2. `tests/test_edit_ops.c` (extend) — `edit_op_input_replace_range`
   cursor placement, selection-clear, OOB guards.
3. `tests/test_numeric_swatch_route.c` (new) — stub-mode integration:
   - Seed `"glVertex3f(5, 0, 0);"`, cursor on `5`, build snapshot,
     assert `numeric_swatch.visible == 1`.
   - Fabricate hit with `item_idx=+1`, call route handler, assert input
     is `"glVertex3f(5.05, 0, 0);"`.
   - Click `▼` 10× on `100` → input ends with `50` (10 × −5).
   - Cursor on `45*t`: snapshot `visible == 0`.
   - Suppression: open autocomplete, assert `visible == 0`.

**Gates** (`make check-state-ownership`):
- `check-edit-ops-pure` — new primitive must be REPL-free.
- `check-c99` — `powf`/`log10f`/`floorf` are C99 + need `<math.h>` and
  `-lm` (already in the build).
- `check-include-style` — new headers use quoted form.

**End-to-end manual check** (`./gl-repl`):

1. Load `Lit cube` example, press `i` to edit a line containing a
   numeric arg, navigate cursor onto a number — swatch ▲▼ should
   appear to its right.
2. Click ▲ → number increments by the formula's step, swatch stays
   visible.
3. Move cursor off the number → swatch disappears.
4. Move cursor into `45*t` (any expression) — swatch should NOT appear.
5. Press Ctrl+Z — last click undone.
6. Open F2 autocomplete — swatch should hide.
