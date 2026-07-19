# Tutorial Catalog Expansion: Block Steps, New Tags, 15 New Tutorials

## Context

The in-app tutorial catalog (`src/repl/tutorials.c`) ships 8 tutorials covering triangles, transforms, variables, animation start, depth test, basic lighting, and color interpolation. Large gaps remain on both axes:

- **OpenGL language**: nothing teaches blending/transparency, fog, clip planes, materials/shininess, normals/shade model, points/lines primitives, line stipple, culling/winding, depth-mask ordering, the GLUT solids, or `glRasterPos3f` + `label()`.
- **REPL features**: nothing teaches `for` loops, functions, `if` blocks, scratch arrays `A/B/C`, or math built-ins — the REPL's whole control-flow language is untaught.

The blocker for the REPL side: `expected_is_single_command()` (src/repl/tutorials.c:700) rejects `{`/`}` in COMMAND-step `expected` strings, so a tutorial cannot ask the user to type `for(i, 0, 8) {`. **User decisions:** extend the runner to support block steps (not scaffold-only workarounds), ship a comprehensive ~15-tutorial batch, and add new tags ("REPL Language", "Effects").

Verified ground truth the design rests on:
- Block-struct commits already flow through the tutorial notify path (both the `;` route and Enter route in `src/editor/input.c` run `tutorial_precheck_current_input()` before and `tutorial_advance_if_commit_ok()` after `editor_try_commit_block_structs`).
- Committing `for(...) {` / `if(...) {` inserts **2 rows** (header + auto `}`), cursor lands between them with insert mode ON; body commits insert at the cursor (which stays on the auto-`}` row); typing `}` hits the close-brace kernel's matched-existing branch (`src/repl/compile.c` ~2083): NO_CHANGE, delta 0, insert mode off. Named-func defs (`spoke(a) {`) also insert header+`}` but **relocate to the top of non-decl code** — identity only when nothing but comments/decls/completed funcs precede.
- Matching compares the **editor input** (not the committed canonical text) via `tutorial_normalize_text` (trim + drop one trailing `;` + strip ALL interior whitespace, then strcmp). So `for(i,0,8){` matches `for(i, 0, 8) {`, and flat shorthands (`glFogfv(GL_FOG_COLOR, r,g,b,a)`, `glClipPlane(GL_CLIP_PLANE0, a,b,c,d)`) are safe as `expected` even though committed rows canonicalize to compound-literal form.
- `tutorial_guard_source_change` blocks mutations at/below locked lines except a pure insert at `pending.commit_line` during an authorized commit — locking the auto-`}` row freezes the block while letting matched body commits through.
- Confirmed cfg slugs/symbols for showcase steps: `wireframe` (`WIREFRAME_OFF/PLAIN/HIDDEN`, src/render3d/render_types.h:79), `backdrop` (`RENDER3D_BACKDROP_STARS`, src/render3d/themes.h:61), `winding`, `normal_vectors`, `auto_time` (labels in `g_cfg_items[]`, src/app/glr_actions.c:401/463/487; slug = `cfg_slug_from_label`). Keybindings: `GLR_WIREFRAME` Ctrl+G, `GLR_WINDING_VIEW` Ctrl+Shift+B (keymap.h:111,124).
- `glMaterialf(face, GL_SHININESS, value)` is a real supported command (src/repl/command_spec.c:349,537) — no fallback needed.

## Phase A — Block-structure COMMAND steps

### A1. Expected-shape classifier — `src/repl/tutorials.{c,h}`

New public classifier shared by validator and runner:

```c
typedef enum {
    TUTORIAL_EXPECTED_ORDINARY = 0,
    TUTORIAL_EXPECTED_BLOCK_OPEN,    /* "for(...) {", "if(...) {", "name(...) {" */
    TUTORIAL_EXPECTED_BLOCK_BRANCH,  /* "} else {", "} else if(...) {" */
    TUTORIAL_EXPECTED_BLOCK_CLOSE,   /* "}" */
} TutorialExpectedShape;
TutorialExpectedShape repl_tutorial_expected_shape(const char *expected);
```

Rules: CLOSE = trimmed text exactly `}`. OPEN = ends in `{`, exactly one `{`, no `}`, prefix shaped `ident(`. BRANCH = starts `}`, ends `{`, middle `else` / `else if(...)`. Else ORDINARY. Plus a "is func-open" helper (OPEN whose ident is neither `for` nor `if`).

> Note: verify at implementation time whether the REPL's `if` supports `} else {` at all — if `editor_try_commit_close_brace` / the if-block commit path has no else-branch handling, drop BRANCH from the classifier and rework tutorial #14 to two separate `if` blocks.

### A2. Validator relaxation — `repl_tutorial_validate_entry` (src/repl/tutorials.c)

Replace the blanket `{`/`}` rejection with shape-aware rules, tracking a `depth` counter across the step walk:

- All shapes still reject `\n`, `;`, empty text, and `float ` decls. ORDINARY additionally rejects `{`/`}` (as today) **and** rejects `for(`/`if(`-prefixed text not ending in `{` (kills one-liner block forms whose body/close rows would go unlocked).
- OPEN → `depth++`; BRANCH requires `depth >= 1`; CLOSE requires `depth >= 1`, `depth--`. All three must be `TUTORIAL_STEP_APPEND`.
- While `depth > 0`: reject `TUTORIAL_STEP_LABEL` placement and REQUIRE_VAR steps. NOTE/SET/REQUIRE stay legal.
- At sentinel: require `depth == 0`.
- Func-open ordering (relocation hazard): reject a func-shaped OPEN if the entry has a `setup` scaffold or if any earlier depth-0 COMMAND step was not itself a func-open — guarantees relocation is an identity.
- Capacity check: count each OPEN step as **2** locked lines (header + auto-`}`).

### A3. Runner changes — `src/subsystems/tutorial/tutorial_runner.c`, `tutorial_state.h`

1. `TutorialRuntimeState`: add `int block_depth;` (reset in `tutorial_state_reset*`).
2. `tutorial_step_instruction_line()`: for APPEND placement with `block_depth > 0`, resolve to the live edit line (cursor sits on the auto-`}` row — the correct in-block insertion point) instead of `document_count`, clamped to `[0, document_count]`. The existing "expected_commit_line < document_count → insert mode ON" park logic already handles mid-document rows.
3. `tutorial_note_expected_commit_applied()`: after the existing delta shift, classify the pending step's expected:
   - OPEN → `block_depth++`; lock `pending.commit_line` (header) **and** `pending.commit_line + 1` (auto `}`), after the shift so indices are final.
   - BRANCH → lock `pending.commit_line`.
   - CLOSE → `block_depth--` (delta 0, no rows change).
4. `tutorial_append_locked_line()`: dedup (contains-check before append) so the `}` row isn't double-recorded by the comment-less auto-lock path.
5. `tutorial_match.c`: **no changes needed** (whitespace-stripped strcmp already covers block text; `tutorial_shadow_suffix` is shape-agnostic).
6. Esc-recovery hook in `src/editor/input.c` (~line 445, the navigate-to-line path): when navigation lands on `tutorial_expected_commit_line()` mid-document, re-enable insert mode — otherwise a user who pressed Esc during a block body is stranded ("must insert at the fading line" forever).
7. Update the v1-rule comments in `tutorial.h` / `tutorials.h`.

### A4. New tests

- `tests/test_tutorial_match.c`: `for(i, 0, 8) {` vs `for(i,0,8){` matches; `}` vs ` } ; ` matches; `for(i, 0, 9) {` mismatches.
- `tests/test_tutorial_runner.c` (follow `test_enter_route_advances_after_match` patterns):
  - Validator: accepts balanced open/body/close; rejects unbalanced-at-sentinel, close-without-open, branch at depth 0, LABEL placement inside a block, REQUIRE_VAR inside a block, `for(...)` without `{`, func-open after an ordinary top-level COMMAND, func-open with a setup scaffold; capacity counts opens as 2.
  - Runtime (drive "First Loop" / "Functions" via `editor_handle_key`): open commit grows doc by 2 with header+`}` locked; next body step's commit line is in-block; body commit shifts the locked `}`; wrong body input rejected, input preserved; paste inside block blocked; `}` step advances with doc count unchanged, insert mode off; post-block append step returns to `document_count`; Esc-then-navigate-back restores insert mode; full walks reach "Tutorial complete".

## Phase B — New tags

- `src/repl/tutorials.h`: add `REPL_TUTORIAL_TAG_REPL_LANGUAGE`, `REPL_TUTORIAL_TAG_EFFECTS` before `REPL_TUTORIAL_TAG_COUNT`.
- `src/repl/tutorials.c`: add the two `TUTORIAL_TAG_*` bit macros; append `"REPL Language"`, `"Effects"` to `g_tutorial_tag_labels[]` (STATIC_ASSERT enforces 1:1). No test edits — `test_catalog_tag_metadata` is generic over tag count; unused tags stay hidden until Phase C lands carriers.

## Phase C — 15 new catalog tutorials

Order constraint (enforced by `test_catalog_subheading_metadata`): each subheading is one contiguous global run — final order = existing Beginner run (0–4) + new Beginner (3), existing Intermediate (Depth Test Triangle, Lighting Basics, Color Interpolation) + new Intermediate (9), then a new **Advanced** run (3). Multi-tag entries keep one subheading everywhere.

**Beginner (insert after "First Animation"):**

1. **Points & Lines** — GEOMETRY. `@cfg view_mode = RENDER3D_VIEW_2D`. Steps: `glPointSize(8)`, `glBegin(GL_POINTS)`, 3 vertices (mix STEP_APPEND/STEP_CMD), `glEnd()`, `glLineWidth(3)`, `glBegin(GL_LINE_STRIP)`, 3 vertices, `glEnd()`; closing NOTE on GL_LINES vs GL_LINE_STRIP.
2. **GLUT Solids Tour** — GEOMETRY | DEPTH_LIGHTING. Setup scaffold (locked): depth/lighting/light0/color-material enables + `glColor3f(0.8, 0.7, 0.3)`. Steps alternate `glTranslatef(...)` with `glutSolidCube(0.5)`, `glutSolidSphere(0.35, 32, 24)`, `glutSolidTorus(0.12, 0.3, 16, 24)`, `glutSolidTeapot(0.3)`, `glutSolidCone(0.3, 0.6, 24, 8)`.
3. **First Loop** — REPL_LANGUAGE (first carrier — makes the tag visible). NOTE, `for(i, 0, 8) {` (OPEN), body: `glPushMatrix()`, `glRotatef(i * 45, 0, 0, 1)`, `glTranslatef(0.6, 0, 0)`, `glutSolidCube(0.15)`, `glPopMatrix()` (mostly STEP_CMD), `}` (CLOSE), closing NOTE mentioning sin/cos.

**Intermediate (insert after "Color Interpolation"):**

4. **Line Stipple** — EFFECTS. cfg 2D. `glEnable(GL_LINE_STIPPLE)`, `glLineWidth(2)`, `glLineStipple(1, 255)`, `glBegin(GL_LINE_LOOP)`, 4 vertices, `glEnd()`; NOTE on the 16-bit pattern (255 = dashes, 43690 = dots).
5. **Blending & Transparency** — EFFECTS. cfg 2D. `glEnable(GL_BLEND)`, `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, `glColor4f(1, 0.3, 0.2, 0.5)`, quad #1, `glColor4f(0.2, 0.4, 1, 0.5)`, overlapping quad #2; NOTE on src/dst factors.
6. **Depth Mask & Draw Order** — EFFECTS | DEPTH_LIGHTING. `glEnable(GL_DEPTH_TEST)`, opaque `glutSolidCube(0.5)`, `glEnable(GL_BLEND)`, `glBlendFunc(...)`, `glDepthMask(GL_FALSE)`, `glColor4f(0.4, 0.8, 1, 0.4)`, `glutSolidSphere(0.7, 32, 24)`, `glDepthMask(GL_TRUE)`; NOTEs on translucents-last.
7. **Fog** — EFFECTS. `@cfg backdrop = RENDER3D_BACKDROP_STARS`. Setup scaffold: `:draw` goto-label anchor followed by a locked for-block drawing a receding row of toruses. STEP_AT splices above `:draw`: `glEnable(GL_FOG)`, `glFogi(GL_FOG_MODE, GL_EXP2)`, `glFogf(GL_FOG_DENSITY, 0.4)`, `glFogfv(GL_FOG_COLOR, 0.05, 0.08, 0.12, 1)` (flat shorthand; matcher compares input, not canonical text).
8. **Clip Planes** — EFFECTS. `glClipPlane(GL_CLIP_PLANE0, 0, 1, 0, 0.1)` (flat shorthand), `glEnable(GL_CLIP_PLANE0)`, `glutSolidSphere(0.8, 48, 32)`; NOTE on kept half-space + the cursor edit-guide disc.
9. **Materials & Shininess** — DEPTH_LIGHTING | EFFECTS. Setup scaffold: depth+lighting+light0. Steps: `glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1)`, `glMaterialfv(GL_FRONT, GL_SPECULAR, 1, 1, 1, 1)`, `glMaterialf(GL_FRONT, GL_SHININESS, 40)`, `glutSolidSphere(0.7, 48, 32)`; NOTE contrasting glColorMaterial.
10. **Normals & Shade Model** — DEPTH_LIGHTING. Setup scaffold: lighting enables. `glShadeModel(GL_FLAT)`, `glBegin(GL_QUADS)`, `glNormal3f(0, 0, 1)`, 4 vertices, `glEnd()`; STEP_SET `normal_vectors = 1` showcase; NOTE on GL_SMOOTH + auto-normals.
11. **Culling & Winding** — GEOMETRY. `glEnable(GL_CULL_FACE)`, `glCullFace(GL_BACK)`, `glFrontFace(GL_CCW)`, one CCW triangle, one CW (culled) triangle; STEP_SET_SYM `wireframe = WIREFRAME_PLAIN`; STEP_REQUIRE_KEY `winding = 1` (GLR_WINDING_VIEW); NOTE.
12. **Bitmap Text** — GEOMETRY. `@cfg auto_time = 1`. `glutSolidCube(0.6)`, `glRasterPos3f(0, 0.8, 0)`, `label("t %f", t)`; NOTE on fmt restrictions (no parens/commas/backslash).

**Advanced (new subheading, trailing the catalog):**

13. **Functions** — REPL_LANGUAGE. Func def FIRST (relocation-identity rule): NOTE, `spoke(a) {` (func-open), body `glPushMatrix()` / `glRotatef(a, 0, 0, 1)` / `glTranslatef(0.6, 0, 0)` / `glutSolidSphere(0.15, 24, 16)` / `glPopMatrix()`, `}`, then calls `spoke(0)`, `spoke(120)`, `spoke(240)`; NOTE on funcN aliasing.
14. **If & Conditionals** — REPL_LANGUAGE | ANIMATION. `@cfg auto_time = 1`. NOTE, `if(sin(t) > 0) {`, `glColor3f(0.2, 1, 0.4)`, `} else {` (BRANCH — see A1 note; fall back to two `if` blocks if else isn't committable), `glColor3f(1, 0.3, 0.2)`, `}`, `glutSolidCube(0.7)`.
15. **Scratch Arrays** — REPL_LANGUAGE. NOTE, `A[0] = 0.2`, `A[1] = 0.7`, `A[2] = 0.4`, `for(i, 0, 3) {`, `glPushMatrix()`, `glTranslatef(i - 1, A[i], 0)`, `glutSolidSphere(0.2, 24, 16)`, `glPopMatrix()`, `}`; NOTE mentioning `rand(i)`.

Implementation-time verifications (runtime tests cover each): the alias-call commit path for `spoke(120)`, array-assign (`A[0] = 0.2`) committing as a single row through both routes, and `} else {` support (see A1 note).

## Verification

- `make test-stubs` — primary gate (runs `test_tutorial_runner` + `test_tutorial_match` under GL stubs, ASan+UBSan). Then `make test` and `make check-state-ownership` (C99 ratchet, keymap, boundary guards).
- Focused loop during development: `make test_tutorial_runner` equivalent via the stubbed test target.
- Manual in-app (`make gl-repl`, Tours untouched — Tutorials menu): run "First Loop", "Functions", "If & Conditionals" end-to-end — ghost text for `for(i, 0, 8) {`, both `;` and Enter advance, wrong body input rejected with input preserved, Esc-then-click-back recovery, block rows locked, teardown restores cfg (grid/backdrop/auto_time). Spot-check Fog/Culling SET/REQUIRE showcases and menu grouping: Effects + REPL Language flyouts appear, Beginner→Intermediate→Advanced headers render once each, All flyout ordered correctly.

## Risks

- **Func relocation** is the sharpest edge; the conservative validator rule (func-opens only above any other top-level content, no scaffold) sidesteps it but constrains future func tutorials.
- **`} else {`** committability is unverified — tutorial #14 has a designed fallback (two `if` blocks).
- **Esc-stranding** inside blocks depends on the small input.c navigation hook; fallback is a per-frame re-park in the controller tick.
- Blocks must sit at document end during their tutorial (curriculum rule, backed by the LABEL-in-block validator rejection) — a close-brace commit with rows below the block would load a locked row into the input.

## Suggested sequencing

Land as three commits matching the phases: (A) block-step runner+validator+tests, (B) tags, (C) catalog entries — each leaves `make test-stubs` green. Phase C can further split Beginner/Intermediate/Advanced if review size matters.
