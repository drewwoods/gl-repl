# gl4es known gaps: closing the oracle failures

Status: **not started**
Date: 2026-10-04
Scope: gl4es, upstream (ptitSeb/gl4es, via the fork `~/src/drewwoods/gl4es`).
gl-repl changes only by deleting known-gap entries and re-pinning.

[`packaging/web/gl4es-known-gaps.txt`](../../../packaging/web/gl4es-known-gaps.txt)
lists every real-GL oracle (`make test-gl-web`) that fails under gl4es, and
says it doubles as the gl4es to-do list. This plan sorts the to-do list into
the part with a known fix (phase A) and the part that needs a decision first,
mostly ptitSeb's (phase B).

Measured against the current pin (a444cc94 + 11 patches, `4713bbf2`):
`test_ui_gl_state` 223/243, `test_attrib_bits_gl` 89/90,
`test_tour_overlay_feedback` 12/25, `test_gl_state_inspector_gl` 479/531.

## How each patch goes out

The same path the #515 series used:

1. Branch in the fork off upstream `master`, one topic per branch.
2. Build that tree for wasm and run
   `make test-gl4es-ab ARGS="--b <fork tree>"`. The oracle lane must
   show the targeted entries as "fixed in B" and **no regressions**; the
   catalog lane should be 0 px (these are queries and defaults, not
   rendering), and the render lane should be flat.
   The fork tree has to ship the same `include/` as the pin (checked by the
   script), so build the branch on top of the pin + gl-repl's patches, or
   use the `gl-repl-upstream` stack.
3. Open the PR: the *why* first (which GL behavior, which oracle shows it),
   known gaps, AI use disclosed. At a measured pace, not all at once.
4. When it lands: re-pin gl-repl, delete the matching known-gap entries
   (the runner lists them as "no longer failing"), and log it in
   [`packaging/web/patches/README.md`](../../../packaging/web/patches/README.md).
   gl-repl does not carry these as local patches in the meantime: no web
   feature depends on them, so waiting costs nothing.

## Phase A: known fixes

Each one reports or restores state gl4es already tracks. No new
emulation, and no rendering change.

### A1. Answer the state queries gl4es already tracks

One PR, all in `gl4es_commonGet` (`src/gl/getter.c`) so every `glGet*`
variant answers alike:

| Query | Source | Notes |
|---|---|---|
| `GL_ATTRIB_STACK_DEPTH` | `glstate->stack ? glstate->stack->len : 0` | `stack` stays NULL until the first push |
| `GL_CLIENT_ATTRIB_STACK_DEPTH` | `glstate->clientStack`, same shape | not in an oracle yet; same two lines, so it goes in alongside |
| `GL_LINE_STIPPLE_REPEAT` | `glstate->linestipple.factor` | defaults already right (1, `glstate.c:203`) |
| `GL_LINE_STIPPLE_PATTERN` | `glstate->linestipple.pattern` | default `0xFFFF` |
| `GL_CURRENT_RASTER_POSITION` | `glstate->raster.rPos` x, y, z, plus clip w | `rasterpos_t` has no w: store `transl[3]` in `gl4es_glRasterPos3f`, 1 for `glWindowPos`, 1 at init |
| `GL_CURRENT_RASTER_POSITION_VALID` | `glstate->raster.rPos_valid` | resolves the `state.h:129` TODO left by #517 |

Today none of these has a getter case, so each falls through to the
driver, which has no such state. The oracles then read zeros, which is why
the default raster x, y, z "pass" and only w (spec: 1) fails.

Clears: attribute stack depth (2), line stipple (4), raster position
`[3]` and "inside the clip volume" (2).

### A2. Default diffuse/specular alpha for `GL_LIGHT1..7`

The spec default for lights 1..7 is (0, 0, 0, 1). `NewGLState`
(`glstate.c:263`) has a per-light loop that sets `ambient[3] = 1` but
never `diffuse[3]` or `specular[3]`, so they stay 0. This is two
assignments in that loop. It shows only in the query: fixed-function
lighting takes its alpha from the material, not the light.

Clears: `light* diffuse[3]`, `light* specular[3]` (all seven lights).

### A3. `GL_STENCIL_BUFFER_BIT` saves the stencil write mask

`stack.c:275` and `:607` are both `//TODO: Stencil buffer writemask`. The
getter already answers `GL_STENCIL_WRITEMASK` / `_BACK_WRITEMASK` from
`glstate->stencil.mask[0..1]` (`getter.c:612`). Fix: add a field to
`glstack_t`, save both faces on push, and restore with
`glStencilMaskSeparate` on pop. Front and back both, since the struct
already tracks both, but leave the separate-func/op TODOs next to it
alone (out of scope).

Clears: `STENCIL_BUFFER_BIT restores stencil write mask`.

A2 and A3 are separate PRs: they touch different files, and a small PR
is easier to review than a themed bundle.

## Phase B: needs a decision first

Each item below has a cheap partial fix and an expensive faithful one.
The decision is which to offer, and it is a question to put to ptitSeb
(on #515 or a draft PR), not one to settle here.

### B1. `GL_POLYGON_MODE` per face

gl4es tracks one mode (`glstate->polygon_mode`, an int).
`gl4es_glPolygonMode` turns `GL_FRONT` into `GL_FRONT_AND_BACK` and
**returns early on `GL_BACK`** (`gl4es.c:1029`), and drawing applies the
one mode to every face.

- *Cheap:* a getter returning the tracked mode for both faces, mapping
  0 to `GL_FILL`. That fixes the defaults, but after
  `glPolygonMode(GL_FRONT, GL_LINE)` it reports back = `GL_LINE`, which
  is what gl4es draws but not what was asked for.
- *Faithful:* per-face state, and per-face drawing: split each primitive
  by winding and cull state before choosing fill, line or point. That is
  real emulation work in the polygon-line path that gl-repl's own patches
  already touch.

Question: is a getter that reports what gl4es draws acceptable, or does
he want per-face support or nothing?

Entries: `default polygon mode back`, `GL_POLYGON_MODE (front)` / `(back)`.

### B2. Raster color latched at `glRasterPos`

The spec latches the current color into `GL_CURRENT_RASTER_COLOR` at
`glRasterPos`, and `glBitmap` draws in that color, not the current one.
gl4es latches nothing and has no getter case.

- *Cheap:* copy `glstate->color` into new raster state in
  `glRasterPos` / `glWindowPos`, plus the getter. That fixes the default
  (1, 1, 1, 1) and the unlit latch test.
- *Faithful:* with `GL_LIGHTING` on, the raster color is the **lit**
  color of the raster position, which means evaluating fixed-function
  lighting on the CPU for one vertex. gl4es has no CPU lighting path.

Also open: whether `glBitmap` should switch to the latched color. That
changes rendering (a `glColor` between `glRasterPos` and `glBitmap` stops
taking effect), which is correct but could surprise programs that rely on
gl4es's current behavior.

Entries: `default raster color[*]`, `raster color latched at glRasterPos
(*)`, `GL_CURRENT_RASTER_COLOR[*]`.

### B3. `GL_MULTISAMPLE` initially enabled

Desktop GL starts with it enabled; gl4es's tracked flag starts at 0
(`enable.c:165`, `proxy_GOFPE`). Flipping the default is one line, but on
GLES the bit does not switch multisampling. Whether it is on depends on the
context's sample buffers, so "enabled" would be a promise gl4es cannot
keep. A faithful answer could be "enabled iff `GL_SAMPLE_BUFFERS` > 0".

Question: default to spec, mirror the context, or leave it?

Entry: `GL_MULTISAMPLE initially enabled`.

### B4. Feedback mode

`glFeedbackBuffer` is a stub (`wrap/glstub.c:48`) and
`glRenderMode(GL_FEEDBACK)` returns `GL_INVALID_ENUM` (`render.c:47`,
`// missing GL_FEEDBACK`). gl4es already emulates `GL_SELECT` in
`render.c`, and feedback would follow that shape: transform on the CPU and
write tokens instead of drawing. This is a feature, not a fix: vertex
types (`GL_2D` .. `GL_4D_COLOR_TEXTURE`), polygon/line/point/bitmap
tokens, clipping, and pass-through.

Today nothing user-facing on the web needs it. gl-repl's PLY export
(`glr_mesh_export.c`) uses feedback, but the web build hides the File
menu. The cost is the 13 `test_tour_overlay_feedback` checks, and a future
web PLY export would need it.

Question: does ptitSeb want feedback in gl4es at all? If so, a `GL_2D` /
`GL_3D`-only first cut (what both gl-repl users need) would be a
reasonable scope.

Entries: the 13 `test_tour_overlay_feedback` checks.

## Done when

- Phase A: A1-A3 merged upstream, gl-repl re-pinned, their entries
  deleted from the known-gaps file, and `make test-gl4es-ab` clean.
- Phase B: each item has an answer. Implemented, or recorded in the
  known-gaps file as deliberately unsupported, with the reason replacing
  the to-do text.
