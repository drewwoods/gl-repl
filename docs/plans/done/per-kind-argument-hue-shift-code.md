# Plan: Per-kind argument hue shift in the code panel

## Progress

- [x] Step 1 - segment cap 4 → 16 (`src/ui/text_panel.h`), committed
- [x] Step 2 - classifier + color model + wiring + public API
      (`src/ui/repl_code_panel.{c,h}`), `make sample` green,
      `check-ui-text-panel-pure` OK, committed
- [x] Step 3 - `test_repl_code_panel_syntax` + Makefile target;
      caught & fixed `t` (reserved but is the predefined var) being
      classified as structural, committed
- [x] Step 4 - optional contrast regression test (3.0 WCAG vs the
      `(0.06,0.06,0.10)` panel background), committed
- [x] Step 5 - verified: `make test` 4373/4373, `make test-stubs`
      4814/4814, `make sample` + `make sample USE_GL_STUBS=1` both link

**Status: complete.** All five commits on branch `feat/per-kind-arg-hue`.

## Context

Today every code-panel row is drawn in a **single color** chosen from the
command's `CmdType` → `CmdSyntaxCategory` → `k_category_colors[]`
(`src/ui/repl_code_panel.c:30-46`, `repl_code_panel_add_command_row`
at `:722-750`). The command class is recognizable by hue, but literal
arguments, constants, and variables all blend into the keyword color.

Goal (chosen scope **1B + 2B**): keep the command/keyword in its existing
class color, but give **numeric/string literals, constants, and variables**
their own distinguishable hues. Use a **global per-kind base palette**, then
nudge each base color a small fraction toward the command's class color so a
line stays visually cohesive (no rainbow). Optionally add a test that the
final tints clear a contrast threshold against the code-panel background.

## Why this is cheap

The generic text panel **already supports per-character color spans** via
`UiTextPanelRow.color_segments[]` (`src/ui/text_panel.h:93-103`,
renderer at `src/ui/text_panel.c` `text_panel_draw_colored_text`). It is
already used for tutorial fade (`repl_code_panel_apply_fade_segments`,
`:619-664`) and virtual-line aux text (`:796-808`). Gaps between segments
fall back to `row->color`, and the renderer **clamps** an over-long segment
list (`seg_count > UI_TEXT_PANEL_MAX_COLOR_SEGMENTS` → clamp) - so overflow
degrades gracefully to the class color, never crashes.

No change is needed to the generic renderer or the `text_panel.h` contract
(which must stay REPL-free - `make check-ui-text-panel-pure`). All new logic
lives in the REPL adapter `src/ui/repl_code_panel.c`, expressed purely as
generic color segments.

## Approach

### 1. Lightweight token classifier (new, in `src/ui/repl_code_panel.c`)

Add a self-contained scanner over the row's display text that emits
`(char_start, char_count, kind)` tuples. Kinds:

- `TOKEN_LITERAL` - numeric literal (`isdigit`/leading `.`), and any
  double-quoted run (the whole `"..."` as one span; do **not** descend into
  `label("...")` format-string interior).
- `TOKEN_CONSTANT` - identifier that is `PI`, `TAU`, or starts with `GL_`.
- `TOKEN_VARIABLE` - identifier that is a known variable: predef/declared
  (`repl_eval_find_predef_var_idx`, `src/repl/eval.h:207`), scratch array
  `A`/`B`/`C` (`repl_eval_scratch_array_index`, `:194`), or any other
  identifier **not** immediately followed by `(` and **not**
  `repl_eval_is_reserved_ident` (`:214`). This heuristic also colors
  funcN-local params/loop vars as variables without needing scope context.
- *No span* (falls back to `row->color` = class color) for: the command
  keyword, function-call names (ident followed by `(`, e.g. `sin`, `foo`),
  operators, commas, parens, whitespace, `;`. Operators/punctuation reading
  as "structural / same as the command" is intentional and reduces span
  count.

Skip tokenization entirely when the command's category is
`CMD_CAT_COMMENT` (whole comment line keeps comment color).

Reuse `repl_scan_next_arg_delim` (`src/repl/eval.h:243`) is **not** needed
here - this is a flat left-to-right lexer over display text, not an arg
splitter. Identifier/number scanning mirrors the logic already in
`eval.c::eval_primary` but only needs positions, not values.

### 2. Per-kind color model with class nudge (`src/ui/repl_code_panel.c`)

- Add a 3-entry base palette (literal / constant / variable). Suggested
  starting values (tune for contrast in step 4): literal ≈ warm amber
  `(0.85, 0.70, 0.45)`, constant ≈ gold `(0.80, 0.80, 0.40)`, variable ≈
  cool blue `(0.55, 0.75, 0.95)`.
- Add `repl_code_panel_kind_color(kind, CmdSyntaxCategory cat)`:
  `out = lerp(base_kind, k_category_colors[cat], NUDGE)` with
  `NUDGE ≈ 0.22` (single tunable constant). This is the "base color,
  class-shifted in that direction" behavior requested.

### 3. Wire into row building (`repl_code_panel_add_command_row`, `:722-750`)

After `row->color = repl_code_panel_category_color(...)` and the existing
overlay/right-action calls, and **before** the tutorial-fade block:

- If `tutorial_line_is_fading(...)` → keep current fade-segment behavior,
  skip syntax segments (fade is transient and rare; documented precedence).
- Else run the classifier on `display_text`, convert each emitted token to a
  `UiTextPanelColorSegment` with `repl_code_panel_kind_color(kind, cat)`,
  fill `row->color_segments[]` / `color_segment_count` up to the cap. Tokens
  beyond the cap simply stay class-colored (graceful).

### 4. Segment cap / memory

Current `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS = 4` (`src/ui/text_panel.h:97`) is
too small for real lines. Raise to **16**.

Memory note: `g_repl_code_panel_rows` is
`MAX_COMMANDS(4096)+MAX_VIRTUAL_LINES(512)+256+2 ≈ 4866` rows; each
`UiTextPanelColorSegment` ≈ 28 bytes. 4→16 segments adds ≈ `4866 * 12 * 28
≈ 1.6 MB` static. Acceptable for a desktop GL app. (If this is later judged
too heavy, the scalable alternative - out of scope here - is converting the
inline array to an index/count into a shared per-frame segment pool; not
recommended now since it churns the generic contract.) 16 covers virtually
all real lines (`MAX_LINE_LEN = 256`); longer/denser lines degrade to class
color past 16 with no visual breakage.

### 5. Tests

- New `tests/test_repl_code_panel_syntax.c` (or extend an existing
  code-panel test): feed representative display strings through the
  classifier and assert `(start,len,kind)` for: `glVertex3f(x + sin(t),
  y*2, z)`, `for(i, 0, 100, 2) {`, `glEnable(GL_BLEND)`, `// comment`
  (no segments), `label("a=%f", x)` (string is one LITERAL, `x` VARIABLE),
  `PI`/`TAU` as CONSTANT, scratch `A[0] = 1` (`A` VARIABLE, `1` LITERAL).
  Add a Makefile target mirroring the existing `test_repl_core_*` pattern.
- **Optional (mark optional in code review):** contrast test - for every
  (kind × `CmdSyntaxCategory`) pair, compute the class-nudged final color and
  assert relative luminance contrast vs the code-panel background exceeds a
  threshold (e.g. ≥ 3.0). Source the panel background from the actual
  code-panel fill color used in `src/ui/text_panel.c`/panel chrome (confirm
  exact RGB during implementation; statusbar uses `0.094` - verify the text
  area background). This guards future palette/NUDGE tweaks from producing
  invisible text.

## Critical files

- `src/ui/repl_code_panel.c` - classifier, color model, wiring (primary).
- `src/ui/text_panel.h` - bump `UI_TEXT_PANEL_MAX_COLOR_SEGMENTS` 4 → 16.
- `src/repl/eval.h` - reuse `repl_eval_find_predef_var_idx`,
  `repl_eval_scratch_array_index`, `repl_eval_is_reserved_ident` (already
  public; no change).
- `tests/test_repl_code_panel_syntax.c` (new) + `Makefile` target.
- No change to `src/ui/text_panel.c` rendering or the panel purity contract.

## Effort

**Moderate, low architectural risk** - roughly half a day to ~1.5 days:

- Classifier: ~120-180 lines (the bulk).
- Color model + palette + nudge: ~30 lines.
- Wiring + fade precedence: ~15 lines.
- Cap bump: 1 line + memory note.
- Tests: ~150 lines + Makefile target; optional contrast test +~40 lines.

Risk is contained because the draw path, span clipping across wrapped rows,
and graceful overflow already exist and are exercised by tutorial-fade /
virtual-aux today. The only genuine design decision (segment cap vs. memory)
is resolved above with a graceful-degradation fallback.

## Verification

1. `make sample` then `./sample` - load an example with loops, variables,
   constants, and `label(...)`. Confirm: command keyword keeps its class
   hue; literals/constants/variables show distinct but class-tinted shades;
   comments unchanged; tutorial fade still works (start a tutorial).
2. `./sample workspace/` with a multi-scene workspace - sanity-check dense
   `glVertex3f(...)` lines and `for(...)` headers.
3. `make test` (all suites) + the new
   `make test_repl_code_panel_syntax`.
4. `make check-ui-text-panel-pure` - confirm the generic panel stayed
   REPL-free (all new logic is in the adapter).
5. `make test-stubs && make sample USE_GL_STUBS=1 && make sample` - both
   build paths green.
