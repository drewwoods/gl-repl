# glPushAttrib / glPopAttrib: commands + per-bit cursor highlighting

> **Status: landed.** All planned phases and the documented follow-ups are complete.

## Context

The REPL supports many fixed-function state commands but no way to scope them. This adds
`glPushAttrib(mask)` / `glPopAttrib()` as REPL commands with real-GL bitmask semantics
(restricted to bits whose state REPL commands can actually set), plus an editor affordance:
cursor on the push line highlights the prior setter lines whose values the push saves;
cursor on the pop line highlights the setter lines between push/pop whose effects the pop
reverts. Each `GL_*_BIT` token gets a unique color, and highlighted lines get left-edge
markers in their covering bit's color (segmented marker when a line is covered by several
bits). Unbalanced push/pops get the existing red gutter warning, and — since the app itself
brackets the user program in `glPushAttrib(GL_ALL_ATTRIB_BITS)` (`src/app/glr_ctrl.c:999/1021`)
— the executor guarantees only balanced pairs reach GL.

**User-confirmed choices:** unique color per bit (not cursor-position-driven); push shows
prior setters ("what's saved"), pop shows scoped changes ("what's reverted"); saved set is
the *last* setter per distinct state item (glColor red→blue before a CURRENT_BIT push
highlights only blue). The matching push/pop bracket line always highlights (like
glPushMatrix/glPopMatrix today).

## Key design decisions

- **10 supported bits** (canonical order = ascending GL value, all < 2^24 so the
  `GLCmd.args[]` float storage invariant at `command.h:96-98` holds):
  `GL_CURRENT_BIT(0x1)`, `GL_POINT_BIT(0x2)`, `GL_LINE_BIT(0x4)`, `GL_POLYGON_BIT(0x8)`,
  `GL_LIGHTING_BIT(0x40)`, `GL_FOG_BIT(0x80)`, `GL_DEPTH_BUFFER_BIT(0x100)`,
  `GL_TRANSFORM_BIT(0x1000)`, `GL_ENABLE_BIT(0x2000)`, `GL_COLOR_BUFFER_BIT(0x4000)`.
  (`GL_FOG_BIT` was added after the initial 9-bit landing — see the fog follow-up
  at the end of this doc.)
  **`GL_ALL_ATTRIB_BITS` alias**: instead of storing the platform's broader GL
  value (0xFFFFFFFF in the stub headers), the token resolves to the union of the
  10 supported single-bit entries (currently 0x71CF), which round-trips through
  float args. Canonical text retains the compact alias rather than expanding it.
- **New mapping module** `src/repl/attrib_bits.c/.h` (CmdType → bit mask + normalized
  state-cell identity + the two highlight collectors). Pure, no GL calls. The normalized
  cell model is also consumed by `gl_state_inspector.c`, so the editor affordance and the
  state popup cannot drift on which values each attribute bit saves/restores.
- **Highlight transport**: add `int aux;` to `UiHighlight` (`src/ui/app/editor.h:74-79`)
  rather than 9 new kinds. Blast radius is one constructor (`src/editor/state.c:641`,
  designated initializer → zero-fills) and the `repl_code_panel.c` scanners. Two new kinds:
  `HIGHLIGHT_ATTRIB_STATE` (line marker; aux = mask of canonical bit *indices*) and
  `HIGHLIGHT_ATTRIB_BIT_TOKEN` (char-range on the push line; aux = single bit index —
  first real user of the existing `char_start/char_end` fields). Bracket match reuses
  `HIGHLIGHT_MATCHING_PUSH_MATRIX` (update its comment).
- **Multi-bit marker**: extend `UiTextPanelRow` with
  `UiTextPanelColor left_marker_band_colors[4]; int left_marker_band_count;`
  (0 = legacy single-color path, existing rows untouched). Marker draw
  (`src/ui/core/text_panel.c:471`) splits the strip vertically into stacked bands;
  >4 bits shows the first 4 in canonical order.
- **No indent scope** for glPushAttrib (unlike glPushMatrix) — not requested, avoids
  touching format/source_scope depth caches.
- **Shared real-GL cap** `REPL_ATTRIB_STACK_CAP = 8`; virtual depth is unbounded.
  The executor, inspector, and overlay mirrors all consume this one definition.
  Real depth is always `min(virtual, CAP)` — LIFO means the level being popped is real iff
  `virtual <= CAP`, so no per-level flag; the controller's bracket can never be over-popped.
  The worst current live nesting is 8 user levels + the render3d outer bracket + a
  winding/hidden-line pass bracket + the controller geometry bracket = 11; fade replay is
  8 + render3d outer + fade bracket = 10. Both stay below GL's guaranteed minimum attribute
  stack depth of 16.

## Phase 1 — Commands + executor safety

- `src/repl/command.h:87`: append `CMD_PUSH_ATTRIB, CMD_POP_ATTRIB` before `CMD_TYPE_COUNT`.
  (`-Wswitch` flags the exhaustive switches that must learn them.)
- `src/repl/command_spec.c`:
  - `k_attrib_bits[]` table (10 entries, NULL-terminated) + accessor
    `repl_attrib_bit_entries()` so attrib_bits.c and the UI share canonical order.
  - `k_enum_command_specs[]` row (glClear at ~:442 is the exact template):
    `{ "glPushAttrib", CMD_PUSH_ATTRIB, 1, "%sglPushAttrib(%s);", 0, .args = { ENUM_SLOT_BITS(k_attrib_bits, "mask: ...") } }`
    — the ENUM_BITFIELD parser branch (`parser.c:336-399`) gives `|`-parsing,
    canonical ordering, dupe-drop, expression rejection for free.
  - `g_command_type_specs[]`: `CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN(...)` for both
    (real GL forbids them inside glBegin), `CMD_CAT_STATE`.
  - `k_func_completions[]`: `glPushAttrib(` (1 arg, help lists the 10 bits) and
    `glPopAttrib()` (zero-arg; glPushMatrix entry at :281 is the template),
    `REPL_HELP_GROUP_STATE`.
- `src/repl/parser.c`: glPushAttrib is auto-handled by the table-driven path. Add a
  zero-arg `glPopAttrib` branch mirroring glEnd (:1518-1529), plain indent; add
  `"glPopAttrib"` to `special_funcs[]` in `is_known_incomplete_func_name` (:215-225).
- `src/repl/executor.{h,c}`: on `ReplExecCursor` add
  `int attrib_depth;` + `struct { unsigned mask; ReplRenderState render; }
  attrib_save[REPL_ATTRIB_STACK_CAP];`. Add `int suppress_attrib_gl` to
  `ReplExecutionOptions`: default 0 uses normal GL semantics; the hidden-line walker sets it
  to 1 so push/pop still scope the REPL bookkeeping mirror without saving/restoring that
  pass's deliberately-owned live GL state.
  New handlers next to the tracked-transform pair (:212-243):
  - Push: `attrib_depth++`; if `<= CAP`: `glPushAttrib((GLbitfield)cmd->args[0])` and
    snapshot the ReplRenderState bookkeeping mirror (light-enable slots, clear color —
    what `repl_apply_state_bookkeeping` :277-302 tracks) into `attrib_save`. Skip only the
    GL call when `suppress_attrib_gl`; the snapshot/depth work is unchanged.
  - Pop: if `attrib_depth == 0` → silent skip (protects the controller bracket); else if
    `<= CAP`: `glPopAttrib()` + restore mirrored bookkeeping gated on the saved mask
    (ENABLE/LIGHTING bits → light slots, COLOR_BUFFER bit → clear color); then decrement.
    Again, `suppress_attrib_gl` skips only `glPopAttrib`.
  - Dispatch as their own `case` block in the not-in-begin switch (before the
    `repl_apply_state_cmd` cluster ~:864) — they need cursor depth, like transforms.
    No fade-context gate needed (unlike CMD_CLEAR :889).
  - `repl_exec_cursor_end` (:924-942): after the matrix unwind (:940), unwind attribs
    (real pop + mirror restore only while `depth <= CAP`; bookkeeping-only cursors restore
    the mirror but issue no GL pops).
- `flatten.c` / `export.c` / `import.c`: zero changes expected (no-vars enum commands take
  the literal fast path; export/import are canonical-text driven). GL stubs already have
  everything (`tests/gl-stubs/include/GL/gl.h:104-124, 407-409`).
- Makefile: the wildcard-backed `REPL_SRCS`/`SRCS` picks up `src/repl/attrib_bits.c`
  automatically; add it to the explicit `REPL_DEMO_DEP_SRCS` list so `repl_demo` keeps
  proving that every pipeline TU links without app/editor/UI backfill.

## Phase 2 — Unbalanced warning + bracket matching

- `src/repl/source_scope.c` `repl_source_scope_view_collect_unbalanced` (:177-213): third
  stack (`g_unbal_attrib_stack`) + `CMD_PUSH_ATTRIB`/`CMD_POP_ATTRIB` cases + third
  leftover-drain loop. Orphans then get `HIGHLIGHT_UNBALANCED` automatically via the
  always-on block in `glr_ctrl_push_highlights` (`glr_ctrl.c:684-690`).
- `repl_find_matching_push_attrib(int)` / `repl_find_matching_pop_attrib(int)` mirroring
  the matrix pair through the shared source-order LIFO
  `repl_find_matching_bracket(line, open, close, dir)` helper.

## Phase 3 — Mapping module `src/repl/attrib_bits.c/.h`

```c
#define REPL_ATTRIB_BIT_COUNT 10
#define REPL_ATTRIB_MAX_ITEMS_PER_CMD 5
typedef struct {
    int color_material_enabled;
    unsigned color_material_face;
    unsigned color_material_mode;
} ReplAttribFlowState;
typedef struct { unsigned item_id; unsigned attrib_bits; } ReplAttribCellWrite;
unsigned repl_attrib_bits_for_cmd(const GLCmd *cmd);  /* GL_*_BIT mask; 0 = not a setter */
int      repl_attrib_bit_index(unsigned single_bit);  /* canonical index 0..9, -1 */
int      repl_attrib_cmd_writes(const GLCmd *cmd, ReplAttribFlowState *flow,
                                ReplAttribCellWrite out[REPL_ATTRIB_MAX_ITEMS_PER_CMD]);
typedef struct { int line_idx; unsigned bit_idx_mask; } ReplAttribHighlightLine;
int repl_attrib_collect_push_saved(int push_line, ReplAttribHighlightLine *out, int max);
int repl_attrib_collect_pop_reverted(int pop_line, ReplAttribHighlightLine *out, int max);
```

- **Bits-for-cmd** (multi-bit membership falls out naturally, matching real GL):
  COLOR3F/4F, RASTER_POS3F, NORMAL3F, EDGE_FLAG → CURRENT; ENABLE/DISABLE → ENABLE **plus** the
  cap-specific bit (LIGHTING/LIGHTi/COLOR_MATERIAL→LIGHTING, LINE_SMOOTH/LINE_STIPPLE→LINE,
  POINT_SMOOTH→POINT, CULL_FACE→POLYGON, DEPTH_TEST→DEPTH_BUFFER, BLEND→COLOR_BUFFER,
  CLIP_PLANEi/NORMALIZE→TRANSFORM); SHADE_MODEL/LIGHT_MODEL_I/COLOR_MATERIAL/MATERIALFV/
  MATERIALF → LIGHTING; LINE_WIDTH/LINE_STIPPLE → LINE; POINT_SIZE/POINT_PARAMETER_FV →
  POINT; CULL_FACE/FRONT_FACE → POLYGON; DEPTH_FUNC/DEPTH_MASK → DEPTH_BUFFER;
  BLEND_FUNC/COLOR_MASK/CLEAR_COLOR → COLOR_BUFFER; CLIP_PLANE → TRANSFORM.
- **State-cell identity** (for last-setter-wins): 32-bit id `(kind << 16) | key`; one kind
  per state family (ITEM_COLOR, ITEM_CAP with key = cap enum, ITEM_MATERIAL with key =
  face+property indices, one combined ITEM_COLOR_MATERIAL_CONFIG for its face+mode pair,
  ITEM_CLIP_PLANE with key = plane idx, etc.). Compound material
  writes expand all the way to atomic cells: `GL_FRONT_AND_BACK ×
  GL_AMBIENT_AND_DIFFUSE` is four ids. Five is the maximum because a `glColor*` line owns
  its CURRENT color cell plus, while color material is enabled, as many as four selected
  material cells.
- **Flow-dependent color material:** the analyzer tracks the `GL_COLOR_MATERIAL` enable,
  face, and mode. A `glColor*` and, matching the inspector's existing semantics, the
  commands that enable or reconfigure color material become the latest writer of the
  selected material cells as well as their direct state. Those derived cells are covered
  by `GL_LIGHTING_BIT`. `repl_attrib_cmd_writes` updates the caller's flow state and returns
  both direct and derived writes; keep this context-sensitive result there rather than
  baking a false unconditional LIGHTING result into `repl_attrib_bits_for_cmd`.
- **Collectors use a masked attribute-stack fold**, not raw range scans. They still use
  the same documented block-unaware/source-order policy as `collect_unbalanced`, but within
  that policy nested attribute pairs have real LIFO semantics. The fold maintains a fixed
  table for the ~60 atomic cells; each entry carries its last-writer line plus the small
  scalar value needed by flow-sensitive cells (cap enabled, color-material face/mode).
  Frames contain `{mask, saved cells}` so pop restores both provenance and the flow values:
  - on any push, snapshot only cells covered by its mask; on a matched pop, restore those
    writers. An orphan pop is ignored, matching executor safety.
  - push query: fold lines `0..push-1`, then emit the current writer of every cell selected
    by the target push mask. Coalesce cells by line and union their canonical bit-index
    colors. Thus setters inside an earlier completed pair cannot leak into the saved set.
  - pop query: fold through the matching push and its body, honoring all nested pairs, then
    compare the effective cells immediately before the target pop with that frame's saved
    cells. Emit only a current writer from inside the body whose provenance differs from
    the saved writer, then coalesce by line/bit as above. Overwritten setters and setters
    already reverted by an inner pop are not highlighted, because the target pop does not
    revert their effects; unchanged cells do not incorrectly point back to pre-push lines.

## Phase 4 — Highlighting

- `src/ui/app/editor.h`: `int aux;` on `UiHighlight` + two new kinds.
  `src/editor/state.{h,c}`: `editor_state_highlights_append_aux(...)`; existing `_append`
  becomes a wrapper passing 0 (no call-site churn).
- `src/app/glr_ctrl.c` `glr_ctrl_push_highlights` (:626), next to the matrix bracket block
  (:645-655): cursor on push → matching-pop bracket + `collect_push_saved` lines as
  `HIGHLIGHT_ATTRIB_STATE` (aux = bit_idx_mask); cursor on pop → matching-push bracket +
  `collect_pop_reverted` lines. Both cases: scan the push line's canonical text for each
  mask token present and push `HIGHLIGHT_ATTRIB_BIT_TOKEN` with char range + bit index
  (canonical text guarantees spelling/order).
- `src/ui/app/repl_code_panel.c`:
  - Scanner `repl_code_panel_line_attrib_bits(snap, line)` → OR of aux over
    ATTRIB_STATE entries (pattern: `_line_is_unbalanced` :571).
  - Marker ladder (:770-890): `MARKER_PRIORITY_ATTRIB_STATE` between AFFECTING_TRANSFORM
    and UNBALANCED; block fills `left_marker_band_colors[]` (≤4, canonical order).
  - Per-bit palette: `static const k_attrib_bit_colors[10]` local to the panel (same
    ownership as the other marker rgba constants). 10 distinguishable colors, staying clear
    of warning-red (0.95,0.35,0.30), replay-green, and the violet bracket match.
  - Token segments: append one `color_segments[]` entry per ATTRIB_BIT_TOKEN **after**
    `repl_code_panel_apply_syntax_segments_for_category` (it resets segment count at :1379)
    and the trailing-comment append (:1416). Renderer draws segments sequentially so
    later opaque segments win over syntax spans; verify visually in On+Shadow mode.
- `src/ui/core/text_panel.{h,c}`: the banded left-marker extension (marker draw :471).

## Phase 5 — Consistency

- `src/subsystems/edit_overlays/edit_overlays.c` `overlay_gl_apply_cmd` (:301-339): make
  the tiny OverlayGlState mirror (clip_enabled_mask, cull_enabled, front_face) attrib-aware
  with a depth-8 snapshot stack keyed on push (when mask ∩ ENABLE|TRANSFORM|POLYGON) / pop
  (restore + re-apply to GL), so overlay walks stay in sync with user pops.
- `src/subsystems/hidden_lines/hidden_lines.c`: add PUSH_ATTRIB/POP_ATTRIB to
  `hidden_lines_cursor_owns_cmd`, set `ReplExecutionOptions.suppress_attrib_gl = 1`, and
  drive them through `repl_exec_cursor_step`. This scopes light-enable/clear-color
  bookkeeping (including end-of-prefix unwind) while leaving the hidden-line pass's live
  depth/color/polygon state untouched. Do not merely call `repl_apply_state_bookkeeping`
  and advance: push/pop need the cursor's saved frames.
- `src/repl/gl_state_inspector.c`: teach the pure checkpoint fold both user commands.
  Add an 8-frame masked snapshot stack for the inspector's tracked state cells and a
  separate virtual user-attrib depth so an orphan user pop cannot consume the generated
  display bracket. Push snapshots the groups selected by the mask while virtual depth is
  within the executor's cap; pops above the cap only reduce virtual depth, and accepted pops
  restore the saved groups. Mark restored cells touched and attribute each restored row's
  latest change source to the pop line (the same policy matrix pop uses). The reported
  `GL_ATTRIB_STACK_DEPTH` is generated depth + `min(virtual_user_depth, CAP)`. Reuse the
  atomic cell/group mapping from `attrib_bits` rather than duplicating bit membership.
- Replay fade batches (`replay_render.c:85`): no special handling — each batch replays
  inside its own attrib bracket; the depth guard + unwind cover truncated prefixes.
- Winding-pass `state_filter` doesn't see push/pop (they bypass `repl_apply_state_cmd`);
  bounded by that pass's own bracket — document at the dispatch site.

## Phase 6 — Tests

- `tests/test_repl_core_parse.c`: single/multi-bit parse, `|` canonicalization to table
  order, dupe-drop, `GL_ALL_ATTRIB_BITS` union alias, reject numeric/expr/unknown, zero-arg
  `glPopAttrib()`, reject both inside glBegin.
- `tests/test_repl_core_commit.c`: bracket-match helpers (mirror the matrix block
  ~:1779-1808); `repl_attrib_bits_for_cmd` / `_cmd_writes` / collector tests (last-setter
  wins, mask filtering, multi-bit union, EDGE_FLAG/CURRENT, four-cell
  FRONT_AND_BACK+AMBIENT_AND_DIFFUSE supersession, and flow-dependent color-material
  writes). Add nested collector cases proving that a completed inner pair does not leak
  into a later push and that an outer pop does not highlight a setter already reverted by
  an inner pop.
- `tests/test_repl_core_extra.c` (~:1845): collect_unbalanced with orphan push/pop-attrib
  and mixed matrix/attrib nesting.
- **Real-GL oracle: `tests/test_attrib_bits_gl.c` (register in `GL_TEST_BINS`, run via
  `make gl-tests` / `make gl-tests FREEGLUT_OSMESA=1`).** The `attrib_bits.c` cell→bit
  coverage (`cell_cover` / `repl_attrib_bits_for_cmd`) is a *claim about real GL semantics*
  — "clear color rides `GL_COLOR_BUFFER_BIT`", "color-material face/mode ride
  `GL_LIGHTING_BIT`", "depth func rides `GL_DEPTH_BUFFER_BIT`", etc. This test uses a live
  GL context (GLUT, same setup as `test_ui_gl_state.c`) as the oracle: for each supported
  bit and each representative state cell the table maps to it, run
  `set(V1) → glPushAttrib(bit) → set(V2) → glPopAttrib() → glGet == V1` to prove the bit
  **covers** the cell, and a negative pass `glPushAttrib(other_bit) → set(V2) → glPopAttrib()
  → glGet == V2` with a bit the table says does *not* cover it, to prove the mapping isn't
  **over-broad**. Drives the same GL calls the executor emits (`glColor*`, `glEnable`,
  `glColorMaterial`, `glDepthFunc`, `glClearColor`, `glLineWidth`, `glClipPlane`, …) and reads
  back with `glGetFloatv` / `glGetMaterialfv` / `glIsEnabled` / `glGetClipPlane` — reusing
  the `assert_*_state` helpers already in `test_ui_gl_state.c`. It queries GL state only (no
  pixels), so the OSMesa colour-render caveat doesn't apply and it runs headless. This is a
  direct table-vs-driver check (no REPL document needed) and is the load-bearing guard that
  the bit membership the whole feature rests on matches the GL the executor actually drives;
  the stub-based `_cmd_writes` / collector tests above cover the *derivation* logic layered
  on top of it. Keep it out of `TEST_BINS` (needs a real context), like the other
  `GL_TEST_BINS`.
- `tests/test_repl_executor.c`: orphan pop leaves depth 0; pairing; unwind at cursor_end;
  virtual depth past the cap (e.g. 12 pushes) pairs down cleanly; bookkeeping restore
  (clear color under COLOR_BUFFER_BIT, light enable under ENABLE_BIT), plus
  `suppress_attrib_gl` proving the same mirror restoration with zero push/pop GL calls.
- Add `tests/test_hidden_lines.c` and register its target in the Makefile: scoped clear color
  and light enables do not leak from a visible-lines walk, including an unmatched push
  unwound at cursor end.
- `tests/test_repl_state.c`: state-popup fold reports user attribute depth and restores a
  representative value/source for each supported group; include nested pairs, orphan pop,
  and the executor cap boundary.
- `tests/test_glr_ctrl.c`: produced highlight list for cursor-on-push / cursor-on-pop
  (lines, aux masks, token char ranges).
- `tests/test_repl_code_panel_document.c` / `_syntax.c`: bit-token segments + marker bands.
- `tests/test_repl_export_all_commands.c` (:70): add the two CmdTypes + sample lines.
- `tests/test_repl_autocomplete.c`: update any hardcoded completion count/order asserts.

## Phase 7 — Docs

- `CLAUDE.md` Supported Commands: add entry after the matrix-stack line — 10 bits, `|`
  policy (same as glClear), depth guard/cap, cursor highlighting, and the
  `GL_ALL_ATTRIB_BITS` supported-union alias. Add `attrib_bits.c/.h` rows to the
  File Layout table and `docs/MODULES.md`. Add mandatory
  `[command CMD_PUSH_ATTRIB]` and
  `[command CMD_POP_ATTRIB]` entries to `src/repl/command_descriptions.txt`; the generator
  requires one entry for every GL/GLU/GLUT CmdType and fails the build if either is absent.

## Verification

1. `make test` and `make test-stubs` (stub counts pick up glPushAttrib/glPopAttrib calls).
2. `make check-state-ownership` (includes check-c99, include-style; new TU in `$(SRCS)`).
3. Native visual check (`make gl-repl`, **native** backend — OSMesa mis-renders colors):
   type a push/pop pair around state changes, park cursor on each
   (`GLR_EDIT_LINE=<n>` for headless repro), confirm: per-bit token colors, per-line
   markers incl. a segmented multi-bit marker (e.g. `glEnable(GL_BLEND)` under
   `GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT`), red gutter on an orphan pop, scene state
   correctly scoped (e.g. color pushed/popped), no chrome corruption after an orphan push
   (unwind working), and the right-click GL-state popup shows the restored value/depth
   after a pop. Repeat the scoped clear-color check in hidden-line wireframe mode.
4. gracemont: `git pull && make check-c99 && make test-stubs`.

## Risks / notes

- **Exported C parity**: no auto-balance mechanism exists in export (the `editor.h:70`
  comment overstates it) — leftover user `glPushMatrix` already leaks in exported C, and
  leftover `glPushAttrib` will behave the same (accumulates until GL's stack cap, GL
  errors are ignored). Parity, not a regression; the in-app gutter warning is the
  mitigation. Optional follow-up: emit balancing pops before the frame bracket pop.
- `glNormal3f` under CURRENT_BIT is GL-correct but slightly noisy; it's one switch case to
  drop if it reads wrong.
- Source-level matching/highlighting intentionally retains the editor's existing
  block-unaware heuristic: a push in a loop/function can pair with a pop outside that block
  even though flattening may execute them at different multiplicities. The masked LIFO fold
  is exact for the chosen source-order model; flat-program/block-aware analysis remains a
  possible follow-up shared with matrix-scope highlighting.

## Review follow-up (2026-07-18)

A source-level review of phases 1–5 found three defects plus a duplication
concern; all addressed on the branch:

- **Overlay scoping was glBegin-pass-only (P1).** The push/pop snapshot stack
  lived inline in `render_outlines_glbegin_pass`; the tess, glut-solid, and
  both vertex-point walkers ignored the commands entirely, and snapshots missed
  the clip-plane *equations* (GL_TRANSFORM_BIT) and the separately tracked
  `color_writes` gate (GL_COLOR_BUFFER_BIT). Factored one `OverlayGlTracker`
  (`overlay_gl_track_cmd` / `overlay_gl_restore_frame`) stepped by every
  walker; a TRANSFORM-covering push captures the live eye-space equations via
  `glGetClipPlane` and the pop re-issues them under an identity modelview.
- **Bit-token highlighting could colour comment text (P2).** The push-line
  token scan now confines itself to the `(...)` argument range and gates each
  token on the parsed mask (`glr_ctrl_push_attrib_bit_tokens`).
- **Attrib-stack depth provenance (P2).** User pushes/pops adjusted the
  reported `GL_ATTRIB_STACK_DEPTH` but left its latest-change source pointing
  at the generated display bracket; the fold now stamps the last user push/pop
  line while user depth is open.
- **Membership dedup.** New `repl_attrib_bits_for_type(CmdType, enum_arg0)` in
  attrib_bits is now the single gate for cell→bit membership everywhere it is
  re-interpreted: the inspector's per-field group restore (per-field probes,
  only the command-less per-light params keep a literal GL_LIGHTING_BIT), the
  executor's bookkeeping restore, and the overlay tracker's restore gating.
  The four state models remain (executor GL semantics, inspector report,
  analyzer writer-lines, overlay mirror — each consumes a different state
  shape) but which-bit-covers-what now lives only in attrib_bits.c.

Regression tests: `test_edit_overlays.c` (per-walker pop scoping, scoped
colour-mask revert, equation capture/restore), `test_glr_ctrl.c` (token
confined to arg range/mask, comment token not coloured), `test_repl_state.c`
(depth row source = latest user push/pop, bracket-inclusive depth values).

## Fog follow-up (2026-07-21)

`GL_FOG_BIT` was added as a **10th** supported bit, after the fog commands
(`glFogi`/`glFogf`/`glFogfv`, `CMD_FOG_*`) merged in from `main`. Without it, the
fog *parameters* fell in `repl_attrib_bits_for_cmd`'s `default: 0` branch, so no
`glPushAttrib` mask could scope them. Changes (10 bits, canonical indices 0..9):

- **`command_spec.c`** `k_attrib_bits[]`: `GL_FOG_BIT` inserted in canonical
  ascending-GL-value position (between `GL_LIGHTING_BIT` 0x40 and
  `GL_DEPTH_BUFFER_BIT` 0x100) → bit-index **5**; the four later bits shift to
  6..9. `glPushAttrib` help text lists it.
- **`attrib_bits.{c,h}`**: `REPL_ATTRIB_BIT_COUNT` 9 → 10; `CMD_FOG_I/F/FV →
  GL_FOG_BIT`; new `ITEM_KIND_FOG` cell keyed by pname (mode / density / start /
  end / colour are distinct cells) covered by `GL_FOG_BIT`; `cap_group_bit(GL_FOG)
  → GL_FOG_BIT`, so `glEnable(GL_FOG)` rides `GL_ENABLE_BIT | GL_FOG_BIT`,
  matching real GL.
- **`gl_state_inspector.c`**: `gl_state_restore_attrib_groups` restores the fog
  params when the pop's mask covers `GL_FOG_BIT`.
- **`repl_code_panel.c`** `k_attrib_bit_colors[]`: a 10th, unique **haze-grey**
  `(0.72, 0.75, 0.78)` entry at index 5 — the sole desaturated hue, distinct from
  the nine saturated ones and clear of the reserved marker colours.
- **Executor**: no change — it already passes the literal mask to real
  `glPushAttrib`, and fog params live in real GL, not the REPL render tail.

Regression tests: `test_repl_core_commit.c` (fog `bits_for_cmd`, enable
dual-membership, distinct-cell `cmd_writes`, push-saved / pop-reverted
collectors), `test_repl_state.c` (`glPushAttrib(GL_FOG_BIT)` scope restores fog
density on pop).

**Real-GL oracle now created** (closes the Phase 6 gap): `tests/test_attrib_bits_gl.c`,
registered in `GL_TEST_BINS`, run via `make gl-tests` / `make gl-tests
FREEGLUT_OSMESA=1`. Drives the same GL calls the executor emits and, for every
supported bit, checks `set(V1) → glPushAttrib(bit) → set(V2) → glPopAttrib() →
glGet == V1` (bit *covers* the cell) plus a negative pass with a non-covering bit
(mapping is not over-broad) — a direct `repl_attrib_bits_for_type`-vs-driver
check, including the fog params and the `GL_FOG` enable dual-membership.

## GL_ALL_ATTRIB_BITS follow-up (2026-07-22)

The initial plan incorrectly treated the broad OpenGL constant (0xFFFFFFFF in
the project's stub headers; platform headers may use a narrower all-groups
value) as requiring rejection of the token itself. The REPL now accepts
`GL_ALL_ATTRIB_BITS` as an alias for the union of all groups in `k_attrib_bits[]`
(currently 0x71CF). That union is exactly representable in `GLCmd.args[]` and
means “everything a REPL command can observably change.”

- `ReplEnumArgSpec.bitfield_all_alias` makes the exception explicit and local to
  the `glPushAttrib` mask slot; `glClear(GL_ALL_ATTRIB_BITS)` remains invalid.
- The parser accepts the alias as any `|` term. Alias-plus-bit and an explicit
  spelling of all ten bits both canonicalize to `GL_ALL_ATTRIB_BITS`, avoiding a
  roughly 180-character expansion.
- The executor is unchanged and pushes the supported union. Export retains the
  canonical alias token, so generated C uses real OpenGL's `GL_ALL_ATTRIB_BITS`
  and may save groups beyond the REPL model; this can only provide additional
  isolation around the exported user program.
- The alias token itself receives no per-bit token colour because one text span
  cannot represent ten hues. Saved/reverted setter lines still receive their
  normal per-bit gutter markers because collectors consume the resolved union.
- Autocomplete and command help offer the alias alongside the ten atomic bits.

The alias is intentionally version-relative: if support for another attribute
group is added later, existing scenes using `GL_ALL_ATTRIB_BITS` begin saving that
group too. This matches the token's “everything the REPL can set” semantics and
the behavior of the controller's own all-attributes bracket.
