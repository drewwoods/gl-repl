# Tunable Variables (`// @tune`) → exported keyboard knobs + HUD

## Context

REPL examples are deliberately capped in command/parameter scale, but the
**exported standalone C** has no such limit — there a user could crank
parameters far higher. Today the only way to drive a value in exported C is to
hand-edit the source and recompile.

This feature lets the user **tag** predefined variables in the REPL with an
inline `// @tune` comment. On export, each tagged variable becomes a live
**keyboard-adjustable knob** in the standalone program: `q/a` raise/lower the
1st tagged var, `w/s` the 2nd, `e/d` the 3rd, and so on down the QWERTY
columns. Adjustment uses the **same step math as the in-app numeric swatch**
(`repl_eval_swatch_step`), with **Shift = fine (×0.2)** and **Ctrl = coarse
(×10)** — mirroring the in-app swatch's modifier scaling. A generated **HUD**
lists each knob's name, current value, and its keys. Tags must **survive
round-trip** export → import, and the in-app **variable slider panel badges**
tagged vars so the author can see which are exported as knobs.

Decisions locked with the user:
- Tag is **bare `// @tune`** — no `min`/`max`/`step` fields; range/step are
  auto-derived from the value's magnitude (swatch formula).
- Exported modifiers **mirror the in-app swatch**: Shift ×0.2 (fine),
  Ctrl ×10 (coarse).
- In-app: authoring + round-trip **plus** the variable panel flags tagged vars.

## Design

### Source of truth
The tag lives as a **trailing comment on the variable's `float` declaration
line** (e.g. `float amp = 1.0; // @tune`). Trailing comments on decls are
already preserved through commit/reformat (`FloatDeclParse.decl_comment` →
`format_decl_text` in `src/repl/compile.c`). We do **not** add a field to
`GLCmd` (keeps it a pure parse result); tunable-ness is re-derived by scanning
the decl line's trailing comment. A line-level bare `// @tune` marks **every**
name on that decl line as tunable.

### Key assignment (max 9 knobs)
Ordered by declaration order, then by name order within a multi-name decl.
Pairs are QWERTY columns, **letters only** (avoids punctuation/modifier
ambiguity): `q/a w/s e/d r/f t/g y/h u/j i/k o/l`. Beyond 9 tagged vars, export
the first 9 and emit a `/* … capped at 9 */` note.

### Exported keyboard math (mirrors swatch)
Generate a C helper replicating `repl_eval_swatch_step` (`src/repl/eval.c:1344`):
```c
static float tune_step(float v){ float m=fabsf(v);
  float e=(m<10.0f)?0.0f:floorf(log10f(m)); return 0.05f*powf(10.0f,e); }
```
In `keyboard()`, normalize the key and read modifiers so letter keys work even
when Shift uppercases or Ctrl turns them into control codes:
```c
int m = glutGetModifiers();
unsigned char k = key;
if (k>=1 && k<=26) k = (unsigned char)(k-1+'a');   /* Ctrl+letter -> letter */
else k = (unsigned char)tolower(k);                /* Shift+letter -> lower  */
float scale = 1.0f;
if (m & GLUT_ACTIVE_SHIFT) scale *= 0.2f;   /* fine  */
if (m & GLUT_ACTIVE_CTRL)  scale *= 10.0f;  /* coarse */
/* per-knob: if (k=='q') amp += tune_step(amp)*scale; if (k=='a') amp -= ...; */
glutPostRedisplay();
```
No clamping (bare `@tune` has no range).

### Exported HUD
Generated `static void draw_tune_hud(void)` sets up a 2D ortho pass (mirroring
the `gl2d_begin`/`glRasterPos2f`+`glutBitmapCharacter` idiom used by the
exported `label()` helper and `replay_hud.c`), reads window size via
`glutGet(GLUT_WINDOW_WIDTH/HEIGHT)`, and draws one line per knob:
`q/a  amp = 1.23`. Lighting/depth saved+restored locally so it never disturbs
the scene.

## Files to modify

### `src/repl/eval.c` / `eval.h` — tag query (reused by export + UI)
- Add `int repl_eval_line_has_tune_tag(const char *line)` — true if the line's
  trailing comment (via existing `repl_line_trailing_comment`) contains the
  `@tune` token. Tolerates `// @tune`, leading/trailing spaces.
- Add `int repl_eval_collect_tuned_vars(SourceTextView text, const char **out,
  int max)` — walk `repl_state_document_cmds()`, for each `CMD_VAR_DECLARE`
  whose source line has the tag, append each `payload.decl.names[i]` (in order)
  up to `max`; returns count. This is the single ordered list driving key
  assignment, HUD, and the badge. (`SourceTextView` is what export already
  threads in via `s_export_text_view`; reuse the same accessor the export
  document-text reader uses.)

### `src/repl/export.c` — generate knobs + HUD, round-trip the tag
1. **Round-trip marker.** In `write_canonical_cmd_as_c` `CMD_VAR_DECLARE` case
   (≈ line 1214), after the `// @declare …` name list, append ` @tune` when the
   decl line carries the tag. Marker becomes e.g. `// @declare amp=1 @tune`.
2. **Prologue section.** New `emit_export_tune_section(f, ctx)` registered in
   `EXPORT_SCAFFOLD_SECTIONS[]` **before** `emit_export_display_section`
   (helpers must precede `display()`). Gated on
   `repl_eval_collect_tuned_vars(...) > 0`: emits `tune_step()` and
   `draw_tune_hud()`. Needs `<math.h>`/`<ctype.h>` (already in the prologue via
   rand/label helpers — verify).
3. **Inject into `display()` + `keyboard()`.** Add two sentinels to
   `g_footer_pre_init[]` (same mechanism as the existing
   `REPL_EXPORT_RESHAPE_PROJ_SENTINEL` expanded in `emit_export_display_tail`,
   `src/repl/export.c:1825`):
   - `REPL_EXPORT_TUNE_HUD_SENTINEL` immediately before `  glPopAttrib();` →
     expands to `  draw_tune_hud();` (empty when no knobs).
   - `REPL_EXPORT_TUNE_KEYS_SENTINEL` inside the `keyboard()` body after
     `(void)x; (void)y;` → expands to the modifier read + per-knob
     `if (k=='q') … ` block (empty when no knobs). The existing
     `if (key==' ')`/`if (key==27)` lines stay.
   When no vars are tagged, both expand to nothing and the exported keyboard is
   byte-for-byte the current stub. Because `repl_dump_code_panel_text` shares
   this emit path, the live code panel shows the generated knobs/HUD too —
   consistent with "code panel == what export produces."

### `src/repl/import.c` — restore the tag
In `parse_snippet_declare` (≈ line 367): detect a trailing `@tune` token in the
`@declare` args, strip it from name parsing, and when reconstructing the
canonical decl source append `; // @tune` (replacing the bare `;`). The
reconstructed line becomes the in-app source of truth again, so re-export and
the panel badge both see it.

### Variable panel badge (in-app)
- `src/app/glr_ctrl.c`: when building the per-frame editor/variable-panel
  snapshot, call `repl_eval_collect_tuned_vars(...)` (controller has the editor
  buffer view) and set a `tuned` flag per var row.
- `src/ui/subsystems/variable_panel.{c,h}` (+ the view struct it reads, see
  `src/subsystems/variable_panel/variable_panel_state.h` /
  `src/ui/app/editor.h`): render a small badge/marker (e.g. a `~` glyph or
  accent dot) on rows whose var is tagged. Pure-render change; no new state
  ownership.

## Tests
- **Roundtrip** (extend `tests/test_repl_core_io.c` or
  `tests/test_repl_export_all_commands.c`): declare `float amp = 1; // @tune`,
  export to a temp file, re-import, assert the decl line still carries `// @tune`
  and `repl_eval_line_has_tune_tag` is true.
- **Export content** (new focused `tests/test_repl_tune.c`): with one tagged
  var, exported text contains `tune_step`, `draw_tune_hud`, a `draw_tune_hud();`
  call, and an `if (k=='q')` handler; with zero tagged vars, none of these
  appear and the keyboard stub is unchanged. Verify ordering/key assignment for
  3 tagged vars (q/a, w/s, e/d) and the 9-knob cap.
- **Compile gate**: generate an export with knobs to a temp `.c` and run
  `gcc -std=c99 -fsyntax-only` against the GL stub headers (mirror however the
  suite already syntax-checks exported output), ensuring the generated
  `keyboard`/HUD compile.
- Run `make test` and `make check-c99`.

## End-to-end verification
1. `make gl-repl && ./gl-repl` — load an example, add `float k = 1; // @tune`,
   confirm the variable panel shows the badge. `Ctrl+S`, inspect `output.c`:
   `// @declare k=1 @tune`, a `draw_tune_hud()`, and `if (k=='q')` in
   `keyboard()`. Reload the file (`./gl-repl output.c`) and confirm the tag
   survived (badge still present).
2. Compile + run the exported `output.c` standalone against freeglut; press
   `q`/`a` to move `k`, `Shift+q` for fine, `Ctrl+q` for coarse; confirm the
   HUD updates.
3. Headless sanity (optional): `make gl-repl FREEGLUT_OSMESA=1` and
   `--export-ply`/dump paths still work.
