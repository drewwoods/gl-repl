# Tunable Variables (`// @tune`) → exported keyboard knobs + HUD

## Context

REPL examples are deliberately capped in command/parameter scale, but the
**exported standalone C** has no such limit - there a user could crank
parameters far higher. Today the only way to drive a value in exported C is to
hand-edit the source and recompile.

This feature lets the user **tag** predefined variables in the REPL with an
inline `// @tune` comment. On export, each tagged variable becomes a live
**keyboard-adjustable knob** in the standalone program: `q/a` raise/lower the
1st tagged var, `w/s` the 2nd, `e/d` the 3rd, and so on down the QWERTY
columns. Adjustment uses the **same step math as the in-app numeric swatch**
(`repl_eval_swatch_step`), with **Shift = fine (×0.2)** and **Ctrl = coarse
(×10)** - mirroring the in-app swatch's modifier scaling. A generated **HUD**
lists each knob's name, current value, and its keys. Tags must **survive
round-trip** export → import, and the in-app **variable slider panel badges**
tagged vars so the author can see which are exported as knobs.

Decisions locked with the user:
- Tag is **bare `// @tune`** - no `min`/`max`/`step` fields; range/step are
  auto-derived from the value's magnitude (swatch formula).
- Exported modifiers **mirror the in-app swatch**: Shift ×0.2 (fine),
  Ctrl ×10 (coarse).
- In-app: authoring + round-trip **plus** the variable panel flags tagged vars.

**Semantic caveat (document for the user):** a knob only "sticks" if the
`display()` body does not reassign the var every frame. `@tune` on a variable
the body overwrites each frame will appear inert - fine for parameter-style
vars; just set the expectation.

## Design

### Source of truth
The tag lives as a **trailing comment on the variable's `float` declaration
line** (e.g. `float amp = 1.0; // @tune`). Trailing comments on decls are
already preserved through commit/reformat (`FloatDeclParse.decl_comment` →
`format_decl_text`, `src/repl/compile.c:587,660`; the decl validator permits
`//` trailing text). We do **not** add a field to `GLCmd` (keeps it a pure
parse result); tunable-ness is re-derived by scanning the decl line's trailing
comment. A line-level bare `// @tune` marks **every** name on that decl line as
tunable.

### Key assignment (max 9 knobs)
Ordered by declaration order, then by name order within a multi-name decl.
Pairs are QWERTY columns, **letters only** (avoids punctuation/modifier
ambiguity): `q/a w/s e/d r/f t/g y/h u/j i/k o/l`. Beyond 9 tagged vars, export
the first 9 and emit a `/* … N tagged, capped at 9 */` note (requires the
collector to report the *total* match count, not just the emitted count - see
collector API below).

### Generated identifier names (must not collide with user symbols)
User variable names and function aliases are valid up to **15 chars**
(`compile.c:527`, `eval.h:112`) and exported as file-scope C identifiers
(`export.c:1426`). A scene with `float tune_step;` or alias `draw_tune_hud()`
would collide once the helper section is enabled. **Every generated identifier
is therefore >15 chars**, which structurally cannot collide:
`g_tune_window_width`, `g_tune_window_height`, `tune_compute_step`,
`draw_tunable_overlay`. The export-content test pins these names.

### Exported keyboard math (mirrors swatch)
Generate a C helper replicating `repl_eval_swatch_step` (`src/repl/eval.c:1344`)
- emit `fabsf` (not a manual abs) so the body is genuinely byte-identical:
```c
static float tune_compute_step(float v){ float m=fabsf(v);
  float e=(m<10.0f)?0.0f:floorf(log10f(m)); return 0.05f*powf(10.0f,e); }
```
`fabsf/floorf/log10f/powf` are covered by `<math.h>` (already in the exported
prologue). In `keyboard()`, decode the key **without `<ctype.h>`** (the prologue
includes only `<math.h>`/`<stdlib.h>` - adding `tolower` would be an implicit
declaration, which project builds make fatal). Use inline ASCII case-fold, and
**gate the Ctrl control-code decode on the Ctrl modifier** so non-Ctrl control
keys (Backspace=8→`h`, Tab=9→`i`, Enter→`j`) don't alias onto knob letters:
```c
int m = glutGetModifiers();
unsigned char k = key;
if ((m & GLUT_ACTIVE_CTRL) && k>=1 && k<=26) k = (unsigned char)(k-1+'a'); /* Ctrl+letter */
else if (k>='A' && k<='Z')                   k = (unsigned char)(k+('a'-'A')); /* Shift+letter */
float scale = 1.0f;
if (m & GLUT_ACTIVE_SHIFT) scale *= 0.2f;   /* fine  */
if (m & GLUT_ACTIVE_CTRL)  scale *= 10.0f;  /* coarse */
/* per-knob: if (k=='q') amp += tune_compute_step(amp)*scale; if (k=='a') amp -= ...; */
glutPostRedisplay();
```
No clamping (bare `@tune` has no range).

### Exported HUD (self-contained 2D pass - model on `replay_hud.c`, NOT `label()`)
`label()` relies on a user `glRasterPos3f` in world space; the HUD must set up
its **own** pass. Generate `static void draw_tunable_overlay(void)` that: pushes
projection+modelview, loads an ortho matching the window, disables
lighting+depth, draws one `glRasterPos2f`+`glutBitmapCharacter` line per knob
(`q/a  amp = 1.23`), then restores all state - mirroring
`src/ui/subsystems/replay_hud.c`.

**Window size without `glutGet(GLUT_WINDOW_WIDTH/HEIGHT)`** - the stub GLUT
header only defines `GLUT_ELAPSED_TIME` in that query group, so a `glutGet`
window-size call would fail the stub compile gate. Instead, record dimensions
in `reshape(w,h)`: add `static int g_tune_window_width = 800,
g_tune_window_height = 600;` to the generated globals and set them at the top of
the exported `reshape()`. The HUD reads those globals. Works identically under
stubs and real GLUT.

### tune_step / swatch parity
The generated formula is a frozen copy of `repl_eval_swatch_step`; standalone C
can't call back in. Add a parity test (see Tests) pinning
`repl_eval_swatch_step` at several magnitudes, plus a comment in the export
generator linking the two, so a future swatch change forces a conscious test
edit that flags the export update.

## Files to modify

### `src/repl/eval.c` / `eval.h` - pure tag predicate only (stays a leaf)
- Add `int repl_eval_line_has_tune_tag(const char *line)`: true iff the line's
  trailing comment (via existing `repl_line_trailing_comment`, `eval.c:1212`)
  contains a **whole-token** `@tune` (bounded by whitespace/end - must NOT
  match `@tuned=5`). Pure string predicate; no command/state access, preserving
  eval's leaf status (eval.c includes only eval.h today).

### `src/repl/core.c` / `core.h` - document collector (state-aware layer)
- Add `int repl_collect_tuned_vars(const GLCmd *cmds, int count,
  SourceTextView text, const char **out, int max, int *total_out)`: walk the
  passed cmd array, for each `CMD_VAR_DECLARE` whose source line (from `text`)
  satisfies `repl_eval_line_has_tune_tag`, append each `payload.decl.names[i]`
  in order. Writes up to `max` into `out`, sets `*total_out` to the **total**
  match count (so callers can report the 9-knob cap), and returns the emitted
  count. Takes cmds + text as **parameters** (no global reach) so it stays
  neutral; both export and the controller call it. (core.c already sits above
  the command/state layer and is the right home for a document walk.)

### `src/repl/export.c` - generate knobs + HUD, round-trip the tag
1. **Round-trip marker.** In `write_canonical_cmd_as_c` `CMD_VAR_DECLARE` case
   (≈ line 1214), after the `// @declare …` name list, append ` @tune` when the
   decl line carries the tag. Marker becomes e.g. `// @declare amp=1 @tune`.
   (Import's name loop breaks on `@`, so the trailing token detaches cleanly.)
2. **Prologue section.** New `emit_export_tune_section(f, ctx)` registered in
   `EXPORT_SCAFFOLD_SECTIONS[]` **before** `emit_export_display_section`
   (helpers must precede `display()`). Gated on collector count `> 0`: emits
   `g_tune_window_width`/`g_tune_window_height` globals, `tune_compute_step()`,
   and `draw_tunable_overlay()`.
3. **Inject into the SAVE PATH ONLY - do not touch `g_footer_pre_init[]`.**
   The shared array is also iterated by the on-screen panel renderer
   (`src/ui/app/repl_code_panel.c:235,1515`), which expands **only** the
   reshape-proj sentinel and would print any new sentinel token verbatim as a
   chrome row. `repl_dump_code_panel_text` does not emit the footer at all. So
   adding sentinels there would create a panel bug for zero benefit. Instead,
   in `emit_export_display_tail` (`src/repl/export.c:1821`), when knobs exist:
   - set `g_tune_window_width/_height` at the top of the emitted `reshape()`
     body,
   - emit `  draw_tunable_overlay();` just before the `glPopAttrib();` line,
   - emit the modifier-read + per-knob `if (k=='q') …` block inside the
     emitted `keyboard()` body (after `(void)x;(void)y;`, keeping the existing
     `' '`/`27` handlers).
   These injections key off footer literal strings (`"void reshape(int w, int
   h) {"`, `"  (void)x; (void)y;"`, `"  glPopAttrib();"`). The injector
   **asserts each anchor was found** (rather than silently emitting zero knobs)
   so a future footer edit fails loudly; the export-content test below is the
   regression guard. When no vars are tagged, output is byte-for-byte the
   current footer. The
   "code panel == export output" parity is explicitly **dropped** (it was a
   nice-to-have, not a requirement); panel + `--dump-code` stay unchanged.

### `src/repl/import.c` - restore the tag
In `parse_snippet_declare` (≈ line 373): detect a trailing `@tune` token in the
`@declare` args (the name loop already stops at `@`), and when reconstructing
the canonical decl source append `; // @tune` (replacing the bare `;`). The
reconstructed line becomes the in-app source of truth again, so re-export and
the panel badge both see it.

### Variable panel badge (in-app)
- `src/app/glr_ctrl.c`: when building the per-frame variable-panel snapshot,
  call `repl_collect_tuned_vars(...)` (controller has cmds + editor buffer
  view) and set a `tuned` flag per var row.
- `src/ui/subsystems/variable_panel.{c,h}`: add `int tuned;` to `UiVariable`
  (`variable_panel.h:36`) and render a small badge (e.g. a `~` glyph / accent
  dot) on tagged rows. Pure-render change; no new state ownership.

## Tests
- **Roundtrip** (extend `tests/test_repl_core_io.c` or
  `tests/test_repl_export_all_commands.c`): declare `float amp = 1; // @tune`,
  export, re-import, assert the decl line still carries `// @tune` and
  `repl_eval_line_has_tune_tag` is true. Add a negative case: `// @tuned=5`
  must NOT match.
- **Export content** (new focused `tests/test_repl_tune.c`): with one tagged
  var, exported text contains `tune_compute_step`, `draw_tunable_overlay`, a
  `draw_tunable_overlay();` call, `g_tune_window_width`, and an `if (k=='q')`
  handler; with zero tagged vars, none appear and the footer is unchanged.
  Verify key assignment for 3 tagged vars (q/a, w/s, e/d) and that tagging 10
  vars emits 9 knobs + the cap note (exercises `*total_out`). This test is also
  the guard against footer-anchor drift in the save-path injector.
- **Swatch parity**: pin `repl_eval_swatch_step` at a few magnitudes (e.g.
  0.5→0.05, 5→0.05, 50→0.5, 500→5) so any change to the in-app formula forces a
  conscious test edit (the comment in the export generator points here). For
  stricter end-to-end parity, the compile-gate test (which already runs the
  exported binary) can drive `keyboard('q', 0, 0)` on a value like 50 and assert
  the resulting delta equals `repl_eval_swatch_step(50)`.
- **Compile gate**: generate an export with knobs to a temp `.c` and compile it
  against the GL stub headers with at least
  `gcc -std=c99 -Werror=implicit-function-declaration -fsyntax-only` (mirror
  `tests/test_export_trace_parity.c`, which already compiles+runs exported C via
  `system()` against the stubs), so a missing generated include fails loudly
  rather than passing as a warning.
- Run `make test` and `make check-c99`.

## End-to-end verification
1. `make gl-repl && ./gl-repl` - load an example, add `float k = 1; // @tune`,
   confirm the variable panel shows the badge. `Ctrl+S`, inspect `output.c`:
   `// @declare k=1 @tune`, `g_tune_window_width/_height`, a
   `draw_tunable_overlay()`, and `if (k=='q')` in `keyboard()`. Reload
   (`./gl-repl output.c`) and confirm the
   tag survived (badge still present).
2. Compile + run the exported `output.c` standalone against freeglut; press
   `q`/`a` to move `k`, `Shift+q` for fine, `Ctrl+q` for coarse; confirm the
   HUD updates and Backspace/Tab/Enter do nothing.
3. Headless sanity (optional): `make gl-repl FREEGLUT_OSMESA=1`; export/dump
   paths still work.
