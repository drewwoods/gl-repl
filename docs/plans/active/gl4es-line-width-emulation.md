# gl4es `glLineWidth` emulation via screen-space quads

Status: **active** (patches landed; the focused browser case matrix and
headful outline smoke check pass. Canonical fog/clip screenshots and a
full-scene frame-rate sweep remain open.)
Date: 2026-08-19
Scope: Emscripten / WebGL2 build only (two local gl4es patches).
Native Cocoa / GLX / OSMesa are unchanged — they already rasterize
wide lines and honour `GL_POLYGON_OFFSET_LINE`.

This is the line-width twin of
[`gl4es-point-smooth.patch`](../../../packaging/web/patches/gl4es-point-smooth.patch):
fix the translator, keep one C path in gl-repl. Do not add
`#ifdef __EMSCRIPTEN__` geometry proxies in `grid.c`, overlays, or
guides.

Two patches, in this order:

1. `gl4es-polygon-offset-line.patch` — pre-existing web bug;
   independently testable at width 1 (case 2); no quad machinery.
   Does carry a shared matrix save/poke/restore helper — the
   projection-row depth bias needs it, and patch 2 reuses it.
2. `gl4es-line-width-quads.patch` — the width emulator, last in
   `GL4ES_PATCHES`. Reuses the helper; does not reimplement the
   bias.

Reviews are folded into the design below rather than left as a
changelog. Standing items already in this document: getter tokens
are two not three (`0x0B22` is both range names), **range values
live in each `glGet*` switch not `commonGet`**, the identity-MVP
bracket forces `GL_FILL` + cull off, dirties **all three**
matrix-cache flags, and translates `GL_POLYGON_OFFSET_LINE` →
GLES `FILL` on polygon-mode quads, `PUSH_IF_COMPILING` stays
first (same-width early-out is non-compiling only), width-1
offset is a **projection-row poke** that never writes
`line_arrays`, `GL_POLYGON_BIT` push/pop includes
factor/units, the wide-path branch sits **before**
`line_arrays_to_vbo`, tess `GL_LINE_LOOP` overlays are out
of the offset scope, fog/lighting/clip planes live in Known
deviations, `drawing.c` intercepts all three line modes,
verification does not claim `check-c99` /
`check-trailing-whitespace` cover the patch.

## Implementation findings (2026-08-19)

Landed as `233d34d4` (`web: emulate glLineWidth and
GL_POLYGON_OFFSET_LINE in gl4es`). The two patches apply cleanly
on the pin plus the earlier `GL4ES_PATCHES`, the wasm `libGL.a`
rebuilds, and `build/release-web/gl4es-line-width.html` links.
`make test-web` was not run — it links the GL stubs, not gl4es.

### Deviations from this document

| Plan said | What landed | Why |
|---|---|---|
| `GL4ES_PATCHES` slot immediately after `gl4es-pushattrib-gaps.patch` | After `gl4es-point-size-batch.patch` (still after pushattrib-gaps) | Patch 1 wraps the polygon-line `DrawArrays` path. A mid-list slot fails `git apply` because that path and the `maxlinewidth` field do not exist yet. |
| `hardext.maxlinewidth` already probed; only add `depthbits` next to it | Patch 1 adds **both** `maxlinewidth` and `depthbits` | The probe lived only as uncommitted WIP in the local gl4es tree. None of the official earlier patches introduce it. Regenerating against the pin made that obvious. |
| `enable.c`: “same shape as `proxy_GO`” | `GO(GL_POLYGON_OFFSET_LINE, polyline_offset)` | `proxy_GO` forwards the enum to GLES. LINE is not a GLES enum; `GO` shadows and does not forward. The “do not forward” sentence was the load-bearing one. |
| Establish compiled-list order; add `linewidth_op` only if needed | No `linewidth_op` | `draw_renderlist` walks `list->calls` (`glPackedCall`) before the geometry draw on the same node. A compiled `STAGE_GLCALL` `glLineWidth` already updates the mirror first. |
| Same-width early-out after `PUSH_IF_COMPILING`; keep the existing wrapper | Early-out after `PUSH`; dropped the dead `if (hardext.maxlinewidth > 1) FLUSH_BEGINEND` | `PUSH_IF_COMPILING` already flushes a pending batch. The maxlinewidth-gated flush was leftover WIP, not in getter-client-state. |
| Screenshot case 2a on the current tree **before** writing either patch; do not start the width patch until width-1 case 2 is clean | Both patches written in one pass | Implementation was requested after the reviews folded. The split is still independently testable (width-1 uses only patch 1) but that screenshot was not taken first. |
| Degenerate `p0 == p1` → axis-aligned NDC square **or** skip | Square triangulation is `BL, BR, TR` + `BL, TR, TL` | Both ribbon and square now use `hx = n * width / vp` (half-extent). An extra `* 0.5` had halved every emulated line. |
| `vp_* < 1` → treat as 1 to avoid div0 | First cut used that clamp; now **skip** the expander | `raster.viewport` is 0 until the first `glViewport`. Clamping to 1 made `Δndc = width` and filled the clip volume. `gl4es_wide_lines_needed` / `gl4es_draw_wide_lines` now return 0 when width or height is `< 1`, so the native 1 px path is used. The bench also calls `glViewport(0,0,640,480)` so the expander actually runs. |

Makefile: the three `bench-web-gl4es` recipes gained `-Isrc` so
`packaging/web/gl4es_bootstrap.c` can see `app/glr_log_prefix.h`.
That include landed after the original polygon-line bench rules
and had been silently broken.

### Benchmark (headless Chrome / SwiftShader)

`make WEB=1 build/release-web/gl4es-line-width.html`, served from
`build/release-web`, fresh query string. Chrome
`--headless=new --use-angle=swiftshader --dump-dom` (killed after
25 s — emscripten keeps the event loop alive). The bench puts
every coverage count in `document.title` even on FAIL, runs one
display (no idle redraw), calls `glViewport(0,0,640,480)` plus a
reshape callback, and clears to black.

640×480 canvas, 40 horizontal segments plus a 4-vertex loop and a
zero-length segment. Floors: w1 ≥ 800, w6 ≥ 4000 and ≥ 3× w1,
loop ≥ 400, zero ≤ 200 (a canvas-filling “square” fails).

**First run (before viewport skip / bench `glViewport`):** every
width reported 245760 and the zero-length frame was a
canvas-filling triangle. Title `FAIL coverage`. That is the
unset-viewport clamp, not a thickening measurement.

**Second run (skip unset viewport + bench `glViewport`):**

| Shot | Covered px | vs w1 |
|---|---:|---:|
| width 1 (native `GL_LINES`) | 8192 | 1.00× |
| width 1.5 | 16384 | 2.00× |
| width 3 | 32768 | 4.00× |
| width 6 | 61440 | 7.50× |
| `GL_LINE_LOOP` width 4 | 1340 | — |
| zero-length width 4 | 4 | — |

Title (half-width still in): `line-width w1 8192 … w6 61440 … zero 4`.
Floors of that cut passed. w6 was exactly half of `40×512×6`.

**Third run (drop extra `* 0.5`, geometry w6 floor, loop−strip):**

| Shot | Covered px | vs w1 |
|---|---:|---:|
| width 1 (native) | 20480 | 1.00× |
| width 1.5 | 32768 | 1.60× |
| width 3 | 61440 | 3.00× |
| width 6 | 122880 | 6.00× |
| `GL_LINE_LOOP` width 4 | 2672 | — |
| same verts as `GL_LINE_STRIP` | 2104 | — |
| zero-length width 4 | 16 | — |

Title: `line-width w1 20480 w1.5 32768 w3 61440 w6 122880 loop 2672 strip 2104 zero 16`.
w6 = `N_SEGS × 512 × 6` exactly and clears the 98304 floor (the old 61440 would not). Loop − strip = 568 ≥ 384 (half of one 192 px × 4 edge). Zero = 16 is a 4×4 square. The last frame is a small white square on black.

### Post-land review (four items, folded into patch 2)

1. **Stipple `s` dropped.** `gen_stipple_tex_coords` wrote
   `list->tex[stipple_tmu]` but left `list->maxtex` at 0, so the
   wide path passed `maxtex == 0` and never copied those coords.
   Native ES2 iterates `hardext.maxtex` and still saw the pointer;
   the expander does not. Fix: bump `list->maxtex` when the
   stipple array is installed (same shape as `drawing.c:179`).
2. **`wide_ensure` skipped new attribute buffers** once vertex
   capacity was enough. A first uncolored draw left `g_wide.color
   == NULL`; a later colored draw reused that cap and wrote
   nowhere. Fix: grow existing slots if needed, then allocate any
   newly required color / secondary / fog / normal / tex slot at
   the current cap.
3. **Zero-length segments twice as wide (vs the then-ribbon).**
   Degenerate used `hx = width / vp` while the ribbon had an
   extra `* 0.5`. Both now use `width / vp` (true half-extent)
   and a real square (`BL, BR, TR` + `BL, TR, TL`). See (5).
4. **`glLineWidthx` ignored the mirror.** 16.16 → float, then the
   same post-`PUSH_IF_COMPILING` early-out as `glLineWidth`.
   Otherwise `glGet(GL_LINE_WIDTH)` and the expander could see a
   stale float width.
5. **Half-width ribbons.** Both paths multiplied `width / vp` by
   0.5. Window mapping is `(ndc+1)*vp/2`, so the half-extent is
   already `width / vp`. Dropped the extra 0.5. The coverage
   oracle now uses `N_SEGS * 512 * 6 * 0.8` as the w6 floor
   (98304) so a 3 px “6 px” line fails, and loop−strip must
   exceed half of one edge.

Polygon-offset factor/units, PushAttrib restore, projection-row
bias, and “wide path before `line_arrays_to_vbo`” were already
correct in `233d34d4` and were not retouched.

### Post-land review (round 3)

The four items above plus the half-width ribbon were already in
the committed patches (`1b9d87b6`, `721452cb`). A follow-up review
of a stale worktree still reported three of them; the files on
`main` already had the viewport skip, the bench `glViewport`, and
the wide-path-before-`list2VBO` order. Corrections recorded here:

1. **P1 — the first cut did not initialize `maxlinewidth` /
   `depthbits` safely.** Emscripten calls
   `GetHardwareExtensions(notest=1)` (`gles_getProcAddress` is
   missing; see `gl4es-accum-fbo`) before `glutCreateWindow`, so the
   first p1 cut inserted the gets after `GL_MAX_TEXTURE_SIZE`, past
   `if (notest) return`. Zero `maxlinewidth` sent width 1 through
   the expander; zero `depthbits` made `r = ldexpf(1, 0) = 1`.
   Patch 1 now installs defaults of 1 and 16 before that return. It
   probes only on the native context-backed path; Emscripten keeps
   those safe defaults because no WebGL context exists during the
   constructor. `gl4es_polygon_offset_ndc_d` also clamps bits to
   `[8, 32]` else 16. Patch 2 treats `maxlinewidth < 1` as 1 as well.
2. **P2 — `web-deps.sh` treated any failed `git apply --check`
   as “already applied”.** An existing `third_party/web/gl4es`
   that still had the first cut of the width patch would keep
   building it after the patch file gained `wide_realloc` and
   `glLineWidthx`. The patch *set* is now stamped
   (`pinSHA:sha256(all patch files)`) in
   `$GL4ES_DIR/.gl4es-patches.sha` and `PINNED.txt`. When the stamp
   changes, the managed clone resets to the pin, drops `libGL.a` +
   `build_wasm`, and reapplies only if the checkout is clean; local
   edits cause a hard error so the reset cannot discard them. A
   `GL4ES_DIR` override is never reset: apply if `--check` passes,
   accept if `--reverse --check` passes, otherwise exit 1.
3. **P2 — first named-list draw uploaded an unused object-space
   VBO.** Already ordered in `721452cb` (wide decision before
   `list2VBO`); regenerated p2 still has that order. Confirmed
   by apply-check of the full `GL4ES_PATCHES` chain on a fresh
   pin.

### Focused case verification (2026-08-19)

The focused browser matrix is now covered by
`packaging/web/bench/gl4es_line_width_cases.c`, built by
`make bench-web-gl4es`. In the Codex in-app browser, the local WebGL2
run reported:

```text
line-width cases: reset=4096 cull=10300 near=584 stipple=1024 client-loop=5040/4184 instances=102/0 lists=2048/2048/3584/2048 poly-list=4842 fill=49152/0 offset-line=70648/1072 fog=3072 clip=256 failures=0
```

That closes the focused checks for trailing width reset, culling, near-plane
clipping, stipple, client-array loop closure, compiled-list width changes,
source-list deletion, mixed-width lists, polygon-mode list caching, fill
offset isolation, and width-1 polygon offset. `fog=3072` confirms that the
known fog deviation still produces geometry, while `clip=256` confirms the
documented native-width fallback when a live clip plane is enabled.

The implementation review also found that the first wide-path version drew
an instanced render list once instead of replaying it for `instanceCount`
(and could draw once for `primcount=0`). The patch now preserves the existing
fixed-pipeline instance-ID loop; the rebuilt gl4es archive and browser case
page pass after that change.

The existing width oracle still reports
`w1 20480 w1.5 32768 w3 61440 w6 122880 loop 2672 strip 2104 zero 16`.
The polygon-line timing oracle reports 130,500 covered pixels for both
immediate and display-list paths. A headful gl-repl smoke check also enabled
Vertex outlines, cycled the outline style through Bold and Inverted, and
orbited the scene without visible z-fighting or a width-reset artifact.

### Still open (plan verification, not done)

Remaining verification is deliberately narrower:

- Capture canonical gl-repl screenshots for the fog and live-clip known
  deviations, so the visual behavior is documented rather than inferred
  only from coverage.
- Run one sustained canonical wireframe scene and record the browser frame
  cadence / 1 px fallback, rather than treating the microbenchmarks as an
  application-level FPS claim.

## Problem

WebGL's `lineWidth` does not widen a line. ANGLE (Chrome, Safari,
Firefox on most machines) reports `ALIASED_LINE_WIDTH_RANGE` of
`[1, 1]`; every `glLineWidth` is a no-op and every `GL_LINES` /
`GL_LINE_STRIP` / `GL_LINE_LOOP` rasterizes one pixel wide.

gl4es does not emulate the missing width. `gl4es_glLineWidth` is the
generated GLES wrapper in `src/gl/wrap/gles.c`. It forwards to
`gles_glLineWidth` and, after
[`gl4es-getter-client-state.patch`](../../../packaging/web/patches/gl4es-getter-client-state.patch),
mirrors the value in `gl4es_mirror_line_width` so `glGet(GL_LINE_WIDTH)`
and `glPushAttrib(GL_LINE_BIT)` stay client-side. That is enough for
state round-trip. It is not enough to change a pixel.

Immediate-mode batching is already handled. The generated wrapper
opens with `PUSH_IF_COMPILING(glLineWidth)` (`loader.h:119–128`):

- compiling a display list (`list.active && !list.pending`) → record
  a `STAGE_GLCALL` packed call and return
- a pending immediate batch (`list.active && list.pending`) →
  `gl4es_flush()` and fall through

`pending` implies `active`, so the later
The reproducible baseline contains a leftover
`if (hardext.maxlinewidth > 1) FLUSH_BEGINEND` guard, but it is
redundant: `PUSH_IF_COMPILING` already flushes a pending batch.
Patch 2 removes that guard without changing the ordinary
immediate-mode flush semantics. The point-size patch is the wrong
template here: `gl4es_glPointSize` lives in
`pointsprite.c` with no `PUSH_IF_COMPILING` at all, which is why
that wrapper had to grow its own compile/flush path.

The remaining compile-list question is whether a `STAGE_GLCALL`
`glLineWidth` replays in the right order relative to the geometry
that follows it inside `glNewList`. That is the thing to establish
(see Implementation order, step 1). It is smaller than “copy
`gl4es-point-size-batch.patch`.”

`stack.c:546` restores width via `gl4es_glLineWidth`, so
`glPopAttrib(GL_LINE_BIT)` — which gl-repl does every frame —
picks up whatever mechanism lands. Load-bearing, and the reason
the wrapper (not a side door) has to stay the single writer of
the mirror.

The mirror-equality early-out currently sits **after**
`PUSH_IF_COMPILING`, so a redundant same-width call still flushes
a pending batch. That is the right *place* for the early-out
relative to compile: do **not** hoist it above
`PUSH_IF_COMPILING`. A compiled `glLineWidth(4)` must record
even when the live mirror is already 4 — the list may later be
called with the mirror at 1. Same-width early-out applies only
to non-compiling calls (see Compiled-list width).

## What gl-repl is missing on web

Chrome that is authored as “thick” is 1 px in the browser. The
payoff list, not an exhaustive inventory:

| Surface | Typical widths |
|---|---|
| Grid casing / checker ink | 1.3–3.4, plus 2 px casing (`GRID_CASING_WIDTH_ADD`) |
| Axes | 2.5 / 3.0 |
| Orbit gizmo halo / core | **6.0** over 1.5 |
| Focused-normal overlay | 6.0 halo, 2.2 core |
| Transform-guide shafts | 5.5 halo, 2.5 core (`TG_SHAFT_*`) |
| Active vertex outline | 3.0 (×2.5 if Bold) |
| Tutorials “Points & Lines” / “Line Stipple” | 3 and 2 |
| User `glLineWidth` in scenes | whatever the document sets |

`GL_LINE_SMOOTH` is a config toggle and is also a no-op on this
backend. It is **out of v1** (see Non-goals); width alone is the
gap this plan closes.

Width 1.0 must stay a native `GL_LINES` draw. That is the
polygon-mode / wireframe path
[`gl4es-polygon-line-drawarrays.patch`](../../../packaging/web/patches/gl4es-polygon-line-drawarrays.patch)
already made cheap.

## Current gl4es surfaces this will touch

Verified against the pinned checkout
`17f0894e19d1553e4176276c759915dab44c08e2` plus the existing local
patches (the tree under `third_party/web/gl4es` is that pin with the
patches applied):

- `src/gl/line.c` already projects endpoints through `getMVPMat()`,
  perspective-divides, and maps to NDC×`(vp_w/2, vp_h/2)` to build
  stipple texcoords (`gen_stipple_tex_coords`). The viewport origin
  cancels in any difference, which is why that function never reads
  `viewport.x` / `viewport.y`. Width expansion is the same
  projection; the perpendicular is taken in that pixel-scaled
  space (not raw NDC) so a non-square viewport does not skew the
  ribbon.
- `src/gl/listdraw.c` is every immediate-mode and display-list draw,
  including the polygon-line expansion that already emits
  `gles_glDrawArrays(GL_LINES, …)`.
- `src/gl/drawing.c` `should_intercept_render()` special-cases
  `mode == GL_LINES && line_stipple` only. The wide-line intercept
  has to name `GL_LINES`, `GL_LINE_STRIP`, and `GL_LINE_LOOP`, or
  client-array strips and loops slip past the same way. Stipple
  stays `GL_LINES`-only; do not silently inherit that predicate.
  The existing `polygon_mode == GL_LINE && mode >= GL_TRIANGLES`
  intercept (line 260) is why the identity-MVP bracket must force
  `GL_FILL` — see Identity-MVP draw.
- `gen_stipple_tex_coords` (`line.c:135–136`) walks `GL_LINE_LOOP`
  as a strip and skips the closing last→first segment on purpose.
  The width expander must emit that edge; the stipple helper will
  not.
- `src/glx/hardext.c` is where patch 1 adds the defaulted
  `hardext.maxlinewidth` cache and the native context-safe
  `GL_ALIASED_LINE_WIDTH_RANGE` probe. It is an `int`; Emscripten
  keeps the default of 1 during pre-context startup.
- `0x0B22` (`GL_LINE_WIDTH_RANGE` in `const.h` /
  `GL_SMOOTH_LINE_WIDTH_RANGE` in `gles.h`) and
  `GL_ALIASED_LINE_WIDTH_RANGE` have **no** getter cases. They fall
  through to WebGL and report `[1, 1]`. After emulation the getter
  must tell the truth. The two names for `0x0B22` are both in
  scope in `getter.c` — they are one `case`, not two. The
  two-element range cannot go in `gl4es_commonGet` (see
  Advertise a real range); granularity (`GL_LINE_WIDTH_GRANULARITY`
  in `const.h:89` only — no `SMOOTH_` alias in getter.c’s
  include graph) can.
- Renderlist merge (`islistscompatible_renderlist` in `list.c`) does
  not know about line width. Immediate mode already flushes on a
  width change (`PUSH_IF_COMPILING`), so two pending batches at
  different widths do not sit together. A compiled list that
  contains a `STAGE_GLCALL` `glLineWidth` between two geometry
  nodes is the case that still needs an ordering check.

`line.c` is the natural home. Do not start in `fpe_shader.c`.

## Prerequisite: `GL_POLYGON_OFFSET_LINE` is a silent no-op

This is a **pre-existing** web bug. It is independent of width
emulation and should land first as
`packaging/web/patches/gl4es-polygon-offset-line.patch`.

### What the stack does today

`enable.c` proxies only `GL_POLYGON_OFFSET_FILL` (line 168).
`GL_POLYGON_OFFSET_LINE` (`0x2A02`) falls through to
`default: … next(cap)` (line 274), which forwards it to GLES.
It is not a GLES enum, so the driver sets `GL_INVALID_ENUM` and
changes nothing. `glIsEnabled` likewise has no case.

`stack.c` agrees. Three `//TODO: GL_POLYGON_OFFSET_LINE` markers
(112, 245, 436) and only `polygon_offset_fill` is shadowed.
[`gl4es-pushattrib-gaps.patch`](../../../packaging/web/patches/gl4es-pushattrib-gaps.patch)
filled `GL_POLYGON_BIT` for cull / front-face / polygon-mode /
`OFFSET_FILL` and left the LINE/POINT enables and factor/units
as an explicit TODO (line 88 of that patch).

`glPolygonOffset(factor, units)` itself is a generated wrapper
that already reaches GLES (`wrap/gles.c`). Factor/units live in
the driver. They are **not** mirrored client-side; a
`glGet(GL_POLYGON_OFFSET_FACTOR)` is a WebGL sync get.

### Why it is visible in gl-repl

`edit_overlays_render_outlines()` (`edit_overlays.c:1117–1121`)
is built entirely on the missing enable:

```
glEnable(GL_POLYGON_OFFSET_LINE);
glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,   /* 0.1 */
                REPL_OUTLINE_POLYGON_OFFSET_UNITS);   /* -1000 */
glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
```

Units of −1000 is a deliberate hard pull-forward. On web it is
doing nothing. The vertex-outline pass should be z-fighting with
its own faces in the browser **right now**, at width 1.

Confirm that with a headful screenshot **before writing either
patch** (verification case 2a). If it speckles, this is a
standalone bug.

`glPushAttrib(GL_POLYGON_BIT)` must save and restore the
new enable **and** the shared factor/units. gl-repl
pushes `GL_ALL_ATTRIB_BITS` around the user program
(`glr_ctrl.c`, `executor.c`), and
`edit_overlays.c:1088–1089` explicitly comments that it
relies on that pop to restore polygon state. A LINE
enable that is stacked without the floats still leaks
`glPolygonOffset(0.1, −1000)` into the next scene.

### What the prerequisite patch does

Shadow `GL_POLYGON_OFFSET_LINE` client-side, same shape as
`proxy_GO(GL_POLYGON_OFFSET_FILL, polyfill_offset)`:

- `enable.c` — `proxy_GO` / `isenabled` for `GL_POLYGON_OFFSET_LINE`
  (`polyline_offset` next to `polyfill_offset` in `state.h`).
  Do **not** forward the enum to GLES.
- `stack.h` / `stack.c` — `polygon_offset_line` on both
  `GL_ENABLE_BIT` and `GL_POLYGON_BIT` push and pop. Close the
  LINE enable TODOs (`stack.c:112`, `245`, `436`). POINT
  stays a TODO.
- Mirror `glPolygonOffset` factor/units client-side
  (`wrap/gles.c` + getter cases, the
  `gl4es-getter-client-state` pattern). **Both** the float
  wrapper and `gl4es_glPolygonOffsetx` write the same two
  floats (`x` is 16.16: `(GLfloat)fixed / 65536.0f`), then
  forward. A compiled-list or ES1 `x` call that skipped
  the mirror would desync the bias.
- **`GL_POLYGON_BIT` owns the shared factor/units**, not
  just the LINE enable. The existing TODO at
  [`gl4es-pushattrib-gaps.patch:88`](../../../packaging/web/patches/gl4es-pushattrib-gaps.patch)
  names both: “`GL_POLYGON_OFFSET_LINE/POINT` enables,
  offset factor & units (not shadowed).” Add
  `polygon_offset_factor` / `polygon_offset_units` on
  `glstate_attrib` next to the LINE enable. Push copies
  the mirror; pop calls `gl4es_glPolygonOffset` so both
  the mirror **and** the GLES driver snap back. Without
  that, `glPushAttrib(GL_ALL_ATTRIB_BITS)` around the
  user program (`glr_ctrl.c`, `executor.c`) leaks the
  overlay’s `(0.1, −1000)` into the next scene’s fill
  offset. `GL_ENABLE_BIT` still saves only the enable.

### How the offset actually reaches pixels at width 1

**Chosen mechanism: a temporary projection-row bias
around the polygon-mode line draw.** Cached
`line_arrays->vert` / `list->vert` are **never written**.
Those arrays are object-space (`listdraw.c:389`) and
are shared across frames; mutating them would bake a
camera-dependent z into a camera-invariant cache.

Two things that do **not** work, and are not this plan:

- **`glEnable(GL_POLYGON_OFFSET_FILL)` around today’s
  `GL_LINES` draw.** GLES applies fill offset only to
  triangles. `listdraw.c` has already lowered
  `glPolygonMode(GL_LINE)` to
  `gles_glDrawArrays(GL_LINES)`. FILL-around-lines is
  the right mapping once the primitives are quads
  (patch 2); it is a no-op on today’s line draw.
- **A CPU store into `clip.z` (or object-space `z`)
  on the generated edge verts.** There is no clip-space
  value on the CPU. Nudging object-space `z` biases
  along the object’s axis, not toward the eye.

The projection poke is the same save-16 / poke /
restore / mark-dirty bracket the identity-MVP section
already specifies. `getPMat()` (`matrix.h:45–47`)
returns the live top-of-stack 16 floats. Column-major:

```
/* P_row2 is [2, 6, 10, 14]; P_row3 is [3, 7, 11, 15] */
P[2]  += d * P[3];
P[6]  += d * P[7];
P[10] += d * P[11];
P[14] += d * P[15];
```

That is algebraically `clip.z' = clip.z + d · clip.w`
**on the GPU**, after MVP. It is not a CPU store.

**Units-to-NDC scale and clamp.** Window `z =
ndc.z · (Far − Near) / 2 + (Far + Near) / 2`. A GL
offset of `units · r` in window depth is therefore

```
d = 2 · units · r / (Far − Near)     /* Far==Near → treat denom as 1 */
if (d >  1.f) d =  1.f;
if (d < -1.f) d = -1.f;
```

- `r = 2^{-depth_bits}`, the minimum resolvable
  window-depth unit.
- cache `GL_DEPTH_BITS` **once when a GL context exists**
  (`hardext.depthbits` next to the `maxlinewidth`
  probe; Emscripten keeps the default during startup). If the get is
  `<= 0`, default to 16. Clamp
  the stored bits to `[8, 32]` so `r` is neither 1
  nor 0. A per-draw `glGet` is the WebGL sync stall
  that `gl4es-getter-client-state` exists to kill.
- `Near` / `Far` are the already-tracked
  `glstate->depth.Near` / `.Far` (default `[0, 1]`,
  which reduces to `d = 2 · units · r`).
- `|d| ≤ 1` so a pathological `units` cannot invert
  the depth range. The outline’s −1000 at 16-bit is
  `~0.03`; at 24-bit `~1.2e-4`. The clamp is a
  fence, not the working value.

The `factor × slope` term stays out of reach this way.
That is fine: the plan already defers it, and the
outline pass is pure units
(`REPL_OUTLINE_POLYGON_OFFSET_UNITS` = −1000, factor
0.1 unused). Genuine line primitives are not touched
— native `GL_POLYGON_OFFSET_LINE` never applied to
them (see Offset scope below).

The save/poke/restore helper **lands in this patch**
(`matrix.h` / `matrix.c` — `getPMat` / `getMVMat`
already live there) and is **reused by patch 2** for
the identity-MVP poke. The helper always dirties
`mvp_matrix_dirty`, `inv_mv_matrix_dirty`, and
`normal_matrix_dirty` on both poke and restore (see
Identity-MVP draw). “No quad machinery” still holds;
“no matrix machinery” does not.

Independently testable **at width 1** (case 2):
`glIsEnabled(GL_POLYGON_OFFSET_LINE)` round-trips,
`glEnable` no longer raises `GL_INVALID_ENUM`,
`glPushAttrib(GL_POLYGON_BIT)` / `GL_ALL_ATTRIB_BITS`
restores the **enable and the factor/units**, and the
width-1 outline-over-face screenshot stops speckling
on polygon-mode / GLUT solids. That screenshot is the
whole claim that the split is independently testable
— do not let the bias work drift into patch 2.

`GL4ES_PATCHES` order: after `gl4es-point-size-batch.patch`
(it wraps the polygon-line `DrawArrays` path and the
`maxlinewidth` probe), before `gl4es-line-width-quads.patch`.
Must also sit after `gl4es-pushattrib-gaps.patch` (same
`stack.c` polygon block).

## Design

Expand each wide line segment into a screen-space quad (two
triangles) at draw time, then draw those triangles with a temporary
identity MVP so the existing FPE vertex line

```
gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
```

becomes a passthrough.

That is the “draw it in ortho” idea, cleaned up so depth stays
perspective-correct. A literal `glOrtho(0, vp_w, 0, vp_h, …)` with
window XY works in 2D but forces a hand-rolled Z remap through
`glDepthRange`. The clip-space sibling does not.

### Per-segment expansion

For each pair of object-space endpoints `(p0, p1)`:

1. `c0 = MVP * p0`, `c1 = MVP * p1` (same `getMVPMat()` /
   `vector_matrix` as stipple).
2. **4D clip against the near plane**, not a `w > ε` test.
   Under a standard perspective, `w_clip = −z_eye`. A vertex
   between the eye and the near plane has `w > 0` but
   `z_clip + w_clip < 0`; perspective divide then produces
   huge window coordinates and the 2D perpendicular blows up.
   A vertex behind the eye (`w ≤ 0`) flips the NDC direction
   and draws a spurious inverted line.

   Parametric clip against `z + w = 0`:

   ```
   C(t) = C0 + t (C1 − C0)
   t     = −(z0 + w0) / ((z1 + w1) − (z0 + w0))
   ```

   Interpolate **every duplicated attribute** (color RGBA, secondary,
   fog coord, every texcoord set including stipple `s`) at `t`.
   Discard the segment if both endpoints have `z + w < 0`. After
   this clip every surviving endpoint has `w > 0` and is in front
   of the near plane. Far / side planes are out of v1 — the GPU
   still clips the expanded triangles.
3. Perspective-divide to NDC. The viewport origin still cancels
   (that is why `line.c` only ever uses `w * 0.5f` / `h * 0.5f`).
   The direction, though, must be measured in **pixels**, not raw
   NDC — see step 4.
4. 2D perpendicular in pixel space, then convert the offset
   back to NDC. Normalizing `perp(ndc1 − ndc0)` is perpendicular
   in NDC, which is **not** 90° on screen when `vp_width !=
   vp_height`; a 45° line would get a skewed ribbon.

   ```
   dx_px = (ndc1.x − ndc0.x) * vp_width  * 0.5
   dy_px = (ndc1.y − ndc0.y) * vp_height * 0.5
   n     = normalize(−dy_px, dx_px)          /* unit, in pixels */
   Δndc  = (n.x * width / vp_width, n.y * width / vp_height)
   ```

   `Δndc` **is** the half-extent: window mapping is
   `window = (ndc + 1) * vp / 2`, so one pixel is `2 / vp` in
   NDC and `width/2` pixels is `width / vp`. Corners are
   `ndc ± Δndc`. Do **not** multiply by 0.5 again — that was
   the half-width bug (a 6 px line rendered 3 px).

   Degenerate / ~0-length segments (`dx_px² + dy_px² ≈ 0`,
   including `p0 == p1`) become an axis-aligned NDC square
   whose **half-side** is `width / vp_*` (full side = `width`
   pixels), or are skipped. No division by a
   zero-length normal. Test degeneracy in pixel space, not NDC
   — a line that is only tall in a very wide viewport is still
   a real segment.
5. Four NDC corners → clip space, **keeping each endpoint’s
   original `clip.z` and `clip.w`** (post-step-2 values, so a
   clipped endpoint carries interpolated `z`/`w`):

   ```
   clip.xy = (ndc.xy + Δndc * side) * clip.w
   clip.z  = parent clip.z
   clip.w  = parent clip.w
   ```

6. Emit two triangles. Broadcast each parent endpoint’s attributes
   to both corners of that end (see Attribute policy). Run
   `gen_stipple_tex_coords()` on the line endpoints first (or
   fold it into the expander loop) and broadcast each `s` to
   both corners; `t = 0` samples the 16×1 `GL_REPEAT` stipple
   texture safely across the quad. `s` then interpolates along
   the line and stays constant across the width.

Caps are square. No miters, no round joins. Desktop aliased
`glLineWidth` is independent square-capped segments; `GL_LINE_STRIP`
/ `LOOP` just overlap those square ends. Matching that is the v1
contract.

### Strip / loop walk, including the closing edge

`GL_LINE_STRIP` and `GL_LINE_LOOP` are expanded pair-wise. Do **not**
inherit `gen_stipple_tex_coords`’s walk for the loop close:
`line.c:135–136` treats `GL_LINE_LOOP` as a strip and admits
“the last segment, the loop one, will look strange, but I will
not add a vertex for that.” Width emulation has to emit that
last→first segment or a `GL_LINE_LOOP` of N vertices is missing
an edge.

For stipple on that closing edge, compute `s` from the
window-space length of last→first ourselves. Do not ask
`gen_stipple_tex_coords` for a coordinate it never wrote.

`rlEnd` already fans merged strips/loops into indexed `GL_LINES`
pairs (including the loop close). Those arrive as `GL_LINES` and
need no extra walk. The explicit close is for a still-packed
`mode_init == GL_LINE_LOOP` list and for the `drawing.c`
client-array intercept.

Polygon-mode edges arrive as `GL_LINES` from `fill_lineIndices` /
the existing line-array expansion; they pick the emulator up for
free if the current width needs it, and stay on the cheap
`DrawArrays(GL_LINES)` path when it does not.

### Attribute policy

Duplicate every attribute the source list (or, on the
`drawing.c` path, the enabled VAO) actually carries — the same
set `build_line_arrays` already expands for polygon-mode lines:

| Attribute | Action |
|---|---|
| position | replaced by the four clip-space corners |
| color | duplicated per endpoint; interpolated at a near-plane clip |
| secondary color | same |
| fog coord | same (the draw still disables fog; keep the values so a later fog pass is not reading garbage) |
| every `tex[a]` | same; the stipple TMU’s `s` is the generated length coord |
| normal | duplicated even though lighting is off, so a leftover FPE lighting path cannot read an unbound pointer |

Presence of secondary / tex / fog / normal does **not** skip
emulation. If any expansion allocation fails, fall back to the
native 1 px `GL_LINES` draw — the same missing-array fallback
`build_line_arrays` already has. Do not emit a half-expanded
quad.

### Identity-MVP draw

Around the triangle draw only. Do **not** call public
`glPushMatrix` / `glPopMatrix`: a scene that is already at
`GL_MAX_MODELVIEW_STACK_DEPTH` (or the projection twin) would
`GL_STACK_OVERFLOW` and silently fail the draw. Reuse the
save/poke/restore helper from patch 1: save the top 16 floats
of each stack (`getMVMat()` / `getPMat()`), poke identity,
draw, write the 16 floats back.

**Dirty all three matrix-cache flags on both the poke and
the restore** — not just `mvp_matrix_dirty`. `matrix.h`
caches two more derivatives off the modelview, each with
its own flag: `getInvMVMat()` (`inv_mv_matrix_dirty`,
line 31) and `getNormalMat()` (`normal_matrix_dirty`,
line 38). If anything calls either during the identity
draw, it computes from identity and **clears** the flag.
After you restore the real modelview and set only
`mvp_matrix_dirty`, the cached inverse / normal matrix is
stale but marked clean, and the next lit draw silently
gets an identity normal matrix. Setting all three on
both sides is cheap and removes the need to reason about
whether the FPE touches those paths with lighting and
texgen off. The helper always does this, so the
projection-row bias in patch 1 gets the same safety.

FPE then does `gl_Position = I * clip` for the duration.

Raster / enable state the bracket must force, and restore:

| State | Why |
|---|---|
| `glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)` | `drawing.c:260` intercepts `polygon_mode == GL_LINE && mode >= GL_TRIANGLES`. Expanded quads drawn as `GL_TRIANGLES` under wireframe/hidden-line would be re-expanded into 1 px outlines of themselves. This is the same path verification case 8 is there to protect. |
| `glDisable(GL_CULL_FACE)` | Quad winding follows the window-space segment direction. With culling on, roughly half the segments vanish. |
| `GL_POLYGON_OFFSET_FILL` — **set, do not inherit** | Parent `clip.z` faithfully reproduces native depth, so it also faithfully reproduces the z-fight. The rule depends on the source; see Offset on the quad draw. |
| lighting / texgen off | Lighting the clip-space corners is nonsense; chrome is unlit `glColor`. |
| fog off | Fog would use the same wrong eye position. **Known deviation** — see below. |

Clip planes: skip emulation while a plane is enabled, rather than
trying to clip the object-space segment in v1. Also a known
deviation.

Do not introduce a new FPE program key. The point of the identity
MVP is to reuse the shader that is already bound.

### Offset on the quad draw

This is the overlay translation. `edit_overlays_render_outlines()`
does `glEnable(GL_POLYGON_OFFSET_LINE)` +
`glPolygonOffset(0.1, -1000)` + `glPolygonMode(GL_LINE)`. The
prerequisite makes that enable a real client-side bit. The
quad draw then **approximates** `_LINE` as GLES
`GL_POLYGON_OFFSET_FILL` for the generated triangles, using
the factor/units already in the driver (`glPolygonOffset`
already reached GLES; no extra upload).

Inheriting whatever FILL happens to be live is wrong in both
directions. Thread a `from_polygon_mode` flag into the
expander — the intercept site already distinguishes the two
cases (`polygon_mode == GL_LINE && mode >= GL_TRIANGLES` vs a
genuine line primitive).

| Source | Native rule | Quad-draw rule |
|---|---|---|
| Polygon-mode edges (the overlay) | `_LINE` offsets polygons rasterized as lines | If the `polyline_offset` shadow is on: temporarily `glEnable(GL_POLYGON_OFFSET_FILL)` for this draw. The quads *are* the polygons now. Skip the prerequisite’s **projection-row bias** on this path — the GPU offset replaces it. If the shadow is off: FILL off. |
| Genuine `GL_LINES` / `LINE_STRIP` / `LINE_LOOP` | No polygon offset. `_LINE` never governed line primitives. | Force `GL_POLYGON_OFFSET_FILL` **off**. A fill offset the scene left enabled for its solids (grid decals, `render.c`) must not silently shift emulated chrome that native leaves alone. |

Restore the previous FILL enable after the draw. Factor/units
stay whatever `glPolygonOffset` last sent; do not get or set
them in the bracket.

The outline pass is exactly where both conditions are live.
That is why the two patches reinforce each other and why the
Bold outline (7.5 px) is the widest, most offset-sensitive
thing in the app. Case 2 / 2b is the coplanar overlay
z-fighting oracle.

### Offset scope: polygon-mode / GLUT solids only

`edit_overlays_render_outlines()` enables
`GL_POLYGON_OFFSET_LINE` once, then runs three passes
(`edit_overlays.c:1117–1125`):

| Pass | How it draws | Offset? |
|---|---|---|
| `render_outlines_glbegin_pass` | re-emits filled `glBegin` blocks under the already-active `glPolygonMode(GL_LINE)` | yes — polygon-mode edges |
| `render_outlines_glut_pass` | re-invokes `glutSolid*` under the same polygon mode | yes — same |
| `render_outlines_tess_pass` | `glBegin(GL_LINE_LOOP)` (`edit_overlays.c:955`) | **no** |

Native `_LINE` never governed genuine line primitives, so
the tessellation overlay is **not** pulled forward on
desktop either. v1 matches that. Covering it would be a
separate, explicit bias policy (app-side or a translator
hook that is not `GL_POLYGON_OFFSET_LINE`). Do not
silently give `from_polygon_mode` to `GL_LINE_LOOP` to
“fix” it — that would shift every user `LINE_LOOP` too.

v1 therefore covers polygon-mode / GLUT solid outlines.
A tess overlay that still speckles after both patches is
out of scope, not a failed patch.

### When to activate

```
requested_width > (GLfloat)hardext.maxlinewidth
```

`maxlinewidth` is an `int`. There is no separate tolerance
constant; the cast is just so the comparison is not int-vs-float
by accident. On WebGL `maxlinewidth` is 1, so every width greater
than 1 takes the quad path. On a GLES driver that can do 8 px
natively, leave it alone — this patch must not change those
pixels.

Treat `width <= 1` as native 1 px in v1. The grid uses `0.9f` on a
faded rank; desktop `GL_LINE_SMOOTH` can actually draw that, we
cannot until the follow-up AA step. Emitting a 0.9 px quad without
coverage AA is just a 1 px quad with more math.

### Compiled-list width, not a new immediate-mode flush

Immediate mode already flushes (see Problem). Do not add another
`FLUSH_BEGINEND`, and do not copy `rlPointSizeOp`.

What remains:

- **Trailing-reset oracle against the current tree first.**
  `glLineWidth(8); draw; glLineWidth(1)` should already keep the
  first batch at whatever width the flush-time mirror held — which
  is 8, because `PUSH_IF_COMPILING` flushes *before* the wrapper
  updates the mirror. Run this before writing any flush code. If
  it fails, the failure is somewhere other than “we forgot to
  flush,” and the fix is not the point-size patch.
- **`linewidth_op` only if compiled `glNewList` needs it.**
  `PUSH_IF_COMPILING` records a `STAGE_GLCALL`. Establish whether
  that packed `glLineWidth` replays before the geometry that
  follows it in the same list. If it does, leave it; the wrapper
  updates the mirror on replay and the expander reads the mirror
  at draw. If it does not (geometry node drawn with the
  *pre-call* width), add a dedicated `linewidth_op` /
  `linewidth_val` exclusive stage, replayed in `listdraw.c` next
  to the point-size op — and only then.
- A list with no in-list width op uses the current mirror at draw
  time. That is the polygon-mode case: wireframe uses whatever
  `glLineWidth` is in force, almost always 1.
- `islistscompatible_renderlist` only needs a width check if two
  geometry nodes with different stamped widths can actually merge.
  Immediate mode will not produce that. A compiled list that
  splits on `STAGE_GLCALL` will not either. Do not add the check
  speculatively.

**Do not hoist the same-width early-out above
`PUSH_IF_COMPILING`.** The existing wrapper calls
`PUSH_IF_COMPILING` first (`gles.c:1405`) on purpose.
Compiling `glLineWidth(4); draw;` while the live mirror is
already 4 must still record the packed call: the list may
later be `glCallList`’d while the mirror is 1. Skipping it
makes the list silently draw at 1.

Conditional ordering in `gl4es_glLineWidth`:

```
if (list.active && !list.pending) {          /* named-list compile */
    PUSH_IF_COMPILING / NewStage(STAGE_GLCALL);
    return;                                  /* always record */
}
if (gl4es_mirror_line_width == width)
    return;                                  /* non-compiling only */
if (list.active && list.pending)
    gl4es_flush();                           /* already what PUSH does */
gl4es_mirror_line_width = width;
gles_glLineWidth(width);
```

`PUSH_IF_COMPILING` already has that compile-vs-pending split;
the only change is the same-width return **after** it, not
before. A same-width immediate call still skips GLES. A
same-width compile still records.

### Do not cache the expanded quads

They depend on MVP and viewport. A display-list VBO of expanded
corners would freeze width in camera space and look wrong the
moment the user orbits. Rebuild every draw, like stipple texcoords.

The polygon-line `line_arrays` VBO cache stays. It caches **topology
in object space**, which is camera-invariant. Width expansion is a
separate, transient buffer sitting on top of that (or on top of the
original `GL_LINES` verts). `clear_line_arrays` / `append_calllist`
do not grow a new persistent cache for it.

Immediate lists already allocate per frame for stipple; the same
budget applies. Use `gl4es_scratch()` if a single scratch region is
enough for the expanded verts + colors of one list, otherwise a
file-local reallocating buffer. Do not add 5+ malloc/free pairs per
list the way the first polygon-line cut did.

### Draw-time VBO / client-array contract

`listdraw.c` uploads **and** activates before the GLES
draw, in that order:

```
891  if (use_line_arrays && list->line_arrays == line_arrays &&
892          !line_arrays->vbo_array)
893      line_arrays_to_vbo(line_arrays, list);   /* upload */
894  save_vbo_t saved[NB_VA];
895  if (use_vbo_array == 2)
896      listActiveVBO(list, saved);              /* bind + rewrite pointers */
```

`listActiveVBO` binds `GL_ARRAY_BUFFER` and rewrites
every enabled attrib’s `real_pointer` to a **VBO
offset**. A naïve `gles_glVertexPointer(..., cpu_quads)`
after that interprets the CPU pointer as an offset into
the still-bound buffer — empty frame, or a GPU page
fault. Hooking “before `listActiveVBO`” but after
`line_arrays_to_vbo` still does a wasted upload and
leaves `line_arrays->vbo_array` set, so
`use_line_vbo` at 899–902 then activates the object-space
cache anyway.

The wide-line path therefore **does not share that
draw**. The branch sits **before both operations**
(before line 891), not merely before line 895:

```
if (wide_needed) {            /* before line_arrays_to_vbo */
    /* expand from CPU arrays; identity-MVP; DrawArrays TRIANGLES */
    continue;                 /* skip 891–902 entirely */
}
if (use_line_arrays && !line_arrays->vbo_array)
    line_arrays_to_vbo(...);
if (use_vbo_array == 2)
    listActiveVBO(...);
```

The activate is a condition on `use_vbo_array`, not just
prose: `list.h:194` documents `0 = not evaluated, 1 =
no, 2 = yes`. Skip means **never reach those two
`if`s** — `use_vbo_array` stays off 2 and
`use_line_vbo` stays false because `line_arrays_to_vbo`
did not run. The object-space VBO cache can stay
allocated for a later width-1 draw; this frame just
does not upload or bind it. `listInactiveVBO` at
`listdraw.c:1004` is gated the same way, so a skipped
activate needs no matching inactive.

1. Source the expansion from the **CPU** arrays only
   (`list->vert` / `list->color` / …, or `line_arrays->vert` if
   the polygon-line expansion already ran). Never from
   `list->vbo_vert` — those are offsets, not addresses.
2. Take the wide branch **before** `line_arrays_to_vbo`
   so that call never runs and `use_vbo_array` never
   becomes 2. Neither activate fires.
3. `bindBuffer(GL_ARRAY_BUFFER, 0)` and
   `bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` (or
   `wantBufferIndex(0)`) so the subsequent pointer calls are
   genuine client arrays.
4. Point the enabled attribs at the transient expanded buffers
   (`gles_glVertexPointer` / `ColorPointer` / …).
5. Identity-MVP (helper, all three dirty flags) + `GL_FILL` +
   cull off + offset per `from_polygon_mode`, then
   `gles_glDrawArrays(GL_TRIANGLES, 0, expanded_count)`.
6. Restore the previous ARRAY/ELEMENT bindings. No
   `listInactiveVBO` — step 2 never activated.

If a future hook has to sit *after* `line_arrays_to_vbo`
/ `listActiveVBO`, it must `listInactiveVBO` and unbind
first. Do not upload the transient quads into the
list’s existing `vbo_array` — that buffer is the
object-space cache and is the wrong lifetime. Never
mutate `line_arrays->vert` in place for offset or
width; those verts stay object-space.

On the `drawing.c` intercept the arrays are already client-side
(that is why the intercept exists). Same unbind-before-pointer
rule if a user VBO happens to be bound.

### Advertise a real range

Add getter cases for **two** pnames, not three:

- `0x0B22` — `GL_LINE_WIDTH_RANGE` / `GL_SMOOTH_LINE_WIDTH_RANGE`
  (same token; one `case` label, using whichever name is convenient,
  with a comment that the other is an alias)
- `GL_ALIASED_LINE_WIDTH_RANGE`

Plus granularity:

- `GL_LINE_WIDTH_GRANULARITY` (`0x0B23`, `const.h:89`).
  `GL_SMOOTH_LINE_WIDTH_GRANULARITY` is **not** in
  getter.c’s include graph — `gles.h` defines the
  `0x0B22` alias (`GL_SMOOTH_LINE_WIDTH_RANGE`) but
  not a granularity alias, and `const.h` has only
  `GL_LINE_WIDTH_GRANULARITY`. No alias hazard on
  this one (unlike `0x0B22`, where both names really
  are in scope). One `case` label. getter.c does not
  currently include `const.h`; pull the name in (or
  add it next to the range in `gles.h`).

Values:

- if `hardext.maxlinewidth > 1`, report the driver’s range
  (unchanged) and its granularity
- else report `[1, 64]` (or another documented cap the expander
  actually clamps to) and granularity `1.0f`

**`gl4es_commonGet` cannot carry a two-element range.**
It is a single-value helper (`int gl4es_commonGet(GLenum
pname, GLfloat *params)`). `gl4es_glGetIntegerv` calls it
as `GLfloat fparam; if (gl4es_commonGet(pname, &fparam))`
(`getter.c:824–828`) — writing `params[1]` there overruns
a scalar on the stack. `gl4es_glGetDoublev` copies only
`*tmp` (`getter.c:1020–1022`), so the max would be
silently dropped.

The precedent is already in the file and is the opposite
of putting ranges in `commonGet`:
`GL_POINT_SIZE_RANGE` / `GL_ALIASED_POINT_SIZE_RANGE`
are handled in **each getter’s own switch** (`854`,
`938`, `1027`), with `glGetDoublev` doing the explicit
`params[0]` / `params[1]` copy. Follow that — **three
switch arms** for the two line-width ranges. Granularity
is a single value and can legitimately live in
`commonGet`.

`glGet(GL_LINE_WIDTH)` already returns the mirror. The expander
clamps the requested width to that advertised max so a scene that
asks for 200 px cannot allocate a pathological quad.

`gl2d_begin()` already forces `glLineWidth(1)` and disables
`GL_LINE_SMOOTH`. 2D chrome that sets 1.5–2 px *inside* that
bracket (color-picker accents) will start working; that is
intended.

## Known deviations (v1)

Defensible, not accidental. Measure them in verification case 2
rather than discovering them in a fogged or clipped scene later.

| Deviation | What the user sees | Why v1 accepts it |
|---|---|---|
| Fog disabled for the expanded draw | Wide lines render unfogged against a fogged native reference | Fog distance is computed from `MV * object`; the vertices are clip-space corners. Doing it right means either CPU fog into the vertex color or an FPE program key. |
| Lighting disabled for the expanded draw | Lit wide lines lose per-vertex lighting | Same reason. gl-repl chrome is unlit `glColor`. |
| Clip plane enabled → skip emulation | Wide lines in a clipped scene stay 1 px | Object-space clip of the segment is real work; v1 will not pretend it did it. |
| `width <= 1` stays native | `0.9f` grid minors are 1 px | Coverage AA is the smooth follow-up. |
| Tess overlay stays unoffset | `render_outlines_tess_pass` `GL_LINE_LOOP` can still z-fight the tessellated face | Native `_LINE` does not apply to line primitives. A pull-forward here is a separate explicit-bias policy, not this patch. |

A later FPE expander, or a CPU fog/clip pass, can close these
without changing the expansion math.

## Patch shape

Two new files, in `GL4ES_PATCHES` order:

### 1. `gl4es-polygon-offset-line.patch`

After `gl4es-point-size-batch.patch` (and therefore after
`gl4es-pushattrib-gaps.patch` / the polygon-line DrawArrays
path). No quad code. Scope has grown (enable shadow +
PushAttrib + factor/units mirror + projection-row bias +
the shared helper) but the split is still worth it: the
independently-testable claim is case 2 at width 1. Do not
let the bias work drift into patch 2.

| File | Change |
|---|---|
| `src/gl/state.h` / `enable.c` | `polyline_offset` shadow; `GO` + `isenabled`; do not forward the enum to GLES (`proxy_GO` would) |
| `src/gl/stack.h` / `stack.c` | `polygon_offset_line` on `GL_ENABLE_BIT` and `GL_POLYGON_BIT`; **factor/units on `GL_POLYGON_BIT` only**; pop via `gl4es_glPolygonOffset`; close the LINE + factor/units TODOs |
| `src/gl/wrap/gles.c` / `getter.c` | mirror factor/units from **both** `glPolygonOffset` and `glPolygonOffsetx` (16.16 → float) |
| `src/glx/hardext.h` / `hardext.c` | default/cache `GL_ALIASED_LINE_WIDTH_RANGE` (`hardext.maxlinewidth`) and `GL_DEPTH_BITS` (`hardext.depthbits`, default 16, clamp `[8, 32]`); probe only on a context-backed native path. The width probe was not in any earlier official patch. |
| `src/gl/matrix.h` / `matrix.c` | shared save/poke/restore helper; dirties `mvp` / `inv_mv` / `normal` on both poke and restore |
| `src/gl/listdraw.c` | when `polyline_offset` and polygon-mode lines: **projection-row depth bias** around the line draw (`P_row2 += d · P_row3`, scaled and clamped as above). Never write `line_arrays->vert`. FILL-around-`GL_LINES` is a GLES no-op |

### 2. `gl4es-line-width-quads.patch`

**Last** in `GL4ES_PATCHES`. Retouches `wrap/gles.c` that
getter-client-state already patched. Reuses the patch-1
helper; does not reimplement the projection-row bias.

| File | Change |
|---|---|
| `src/gl/line.c` / `line.h` | `expand_wide_lines` + 4D near clip + identity-MVP draw (reuse the helper, all three dirty flags, `GL_FILL`, cull off, offset per `from_polygon_mode`) |
| `src/gl/listdraw.c` | call it for line modes when width needs it, **before `line_arrays_to_vbo` (line 891)** — which is also before `listActiveVBO`; pass `from_polygon_mode`; skip the prerequisite’s projection-row bias when the quad path takes over; replay `linewidth_op` only if step 1 showed `STAGE_GLCALL` is not enough |
| `src/gl/drawing.c` | intercept `glDrawArrays` / `glDrawElements` of `GL_LINES` / `GL_LINE_STRIP` / `GL_LINE_LOOP` when `gl4es_mirror_line_width > (GLfloat)hardext.maxlinewidth` (stipple stays `GL_LINES`-only; `from_polygon_mode = 0` on this path) |
| `src/gl/wrap/gles.c` | keep `PUSH_IF_COMPILING` first; same-width early-out only on the non-compiling path; do not add another flush |
| `src/gl/list.h` / `list.c` | `linewidth_op` / `linewidth_val` **only if** the compiled-list oracle needs it |
| `src/gl/getter.c` | `0x0B22` and `GL_ALIASED_LINE_WIDTH_RANGE` in **each** of `glGetIntegerv` / `glGetFloatv` / `glGetDoublev` (POINT_SIZE_RANGE shape; Doublev copies both elements). `0x0B23` granularity in `gl4es_commonGet` |

Header comments follow the existing house style (problem, what
was tried, what landed). Add an inventory row per patch and a
dated section to
[`packaging/web/patches/README.md`](../../../packaging/web/patches/README.md)
while the measurements are still available.

No gl-repl C changes in v1. Native and stub builds never see
either patch. A bench under `packaging/web/bench/` is a new file
in that tree, not a gl-repl C change, and is not in `$(SRCS)`.

## Verification

Same discipline as the polygon-line work: FPS without coverage is
not a result, and a reused browser tab can keep the old wasm
module. Fresh navigation, coverage oracle, then timing.

1. **Microbenchmark** in the style of
   [`packaging/web/bench/gl4es_polygon_line.c`](../../../packaging/web/bench/gl4es_polygon_line.c):
   a few dozen segments at widths 1 / 1.5 / 3 / 6, `glReadPixels`
   coverage. Width 1 must match the pristine build to the pixel.
   Width 6 must cover on the order of 6× a 1 px line of the same
   screen length (not 6× the *pixel count of the whole canvas*).
   Title the page `FAIL` below the coverage floor so an empty frame
   cannot report a speedup. Also a **zero-length** segment
   (`p0 == p1`, width > 1): no NaN, either an axis-aligned square
   or a clean skip.
2. **Outline-over-face, before any patch.** A solid face with the
   vertex-outline pass on, width 1. Native shows a clean outline.
   If web speckles today, that is the pre-existing
   `GL_POLYGON_OFFSET_LINE` no-op (Prerequisite). Fix it with
   `gl4es-polygon-offset-line.patch` — enable shadow +
   factor/units on `GL_POLYGON_BIT` + projection-row
   bias — and re-shoot **before** the quads land. That
   width-1 screenshot is the whole independently-testable
   claim for the split; the bias must not drift into
   patch 2. GLUT solids must clean up; a remaining tess
   `GL_LINE_LOOP` speckle is out of scope (Offset scope).

   Then the usual headful set: default grid + axes, orbit gizmo
   (6 px halo), a live transform guide, the Points & Lines
   tutorial. Compare against native, not against “looks thicker.”
   **Also a fogged scene and a clip-plane scene** so the Known
   deviations (unfogged wide lines, 1 px under a clip plane) are
   measured rather than discovered.

2b. **Bold outline.** Same solid face,
    `VERTEX_OUTLINE_ACTIVE_WIDTH 3.0 × VERTEX_OUTLINE_BOLD_SCALE
    2.5 = 7.5 px`. Widest line in the app, and the case where a
    quad that lost its offset is most visible. After both
    patches: clean 7.5 px outline, pulled forward, no z-fight
    and no “halo of the face.”

2c. **Fill-offset must not leak onto chrome.**
    `glEnable(GL_POLYGON_OFFSET_FILL)` live (grid decal, or a
    scene that sets it) plus wide genuine `GL_LINES` chrome.
    The chrome must sit at native depth — the negative half of
    the `from_polygon_mode` rule. A missing FILL-off in the
    bracket shows up as chrome that has slid in front of or
    behind the solids.
3. **Culling on.** `glEnable(GL_CULL_FACE)` + `glFrontFace` both
   windings, a handful of wide segments whose window-space
   direction flips. Every segment stays visible; the bracket
   disabled cull. A missing disable shows up as “every other
   line vanished.”
4. **Trailing-reset oracle, against the current tree first.**
   Immediate mode only: `glLineWidth(8); draw; glLineWidth(1)`
   must keep the first batch at 8 px. This is how gl-repl
   actually writes overlays, and `PUSH_IF_COMPILING` is supposed
   to already make it true. If it fails before the patch, that
   is a finding, not a reason to copy the point-size patch.
   Named-list replay is case 9, not this case.
5. **Near-plane.** Orbit around a long axis line until it passes
   through the camera **and** until an endpoint sits between the
   eye and the near plane (`w > 0` but `z + w < 0`). No giant
   slivers, no inverted lines, no NaN uploads. A stippled wide
   line clipped this way must keep a continuous dash pattern
   (attributes interpolated at `t`, not snapped to an endpoint).
6. **Stipple + width 2.** Occluded-axis ghost (`GL_LINE_STIPPLE` +
   a non-1 width) still dashes. The expander must broadcast the
   stipple `s` that `gen_stipple_tex_coords` already produced.
7. **`GL_LINE_LOOP` / `GL_LINE_STRIP`, both paths.** A 4-vertex
   loop at width 4, once via `glBegin`/`glEnd` and once via
   `glDrawArrays(GL_LINE_LOOP, …)`. Coverage must include the
   closing last→first edge (the one `gen_stipple_tex_coords`
   skips). A strip of the same verts is missing that edge, so
   the two are distinguishable — use that as the oracle, not
   “it looks closed.”
8. **Wireframe / hidden-line stay at 60 FPS, and stay 1 px.**
   Those are width 1 and must remain on the existing
   `DrawArrays(GL_LINES)` path. This is also the path that would
   re-expand expanded quads if the bracket forgot `GL_FILL`. A
   regression here — slower *or* “wireframe of the halo quads” —
   is a failed patch even if wide chrome looks perfect.
9. **Named display list** — not covered by the trailing-reset
   case. All of:

   - One compiled list containing a 1 px segment, a
     `glLineWidth(4)` op, and a 4 px segment, played back
     across a camera orbit. This is the `STAGE_GLCALL` /
     `linewidth_op` replay oracle from step 1.
   - `glCallList` of a list that recorded width 4 while the
     current mirror is 1, and the reverse (unstamped list
     drawn while the current width is 4). Stamped lists keep
     their recorded width; unstamped lists follow the mirror.
   - **Compile at the live width, then change it.**
     `glLineWidth(4); glNewList(); glLineWidth(4); draw;
     glEndList(); glLineWidth(1); glCallList();` The list
     must still draw at 4. This is the early-out-vs-compile
     oracle: hoisting the same-width return above
     `PUSH_IF_COMPILING` fails it.
   - `glNewList` / `glCallList` of a source list, then
     `glDeleteLists` on the source (the `append_calllist`
     shallow-copy case from the polygon-line patch). The
     caller must not share or double-free transient expanded
     storage, and must not inherit a VBO of object-space
     verts as if it were the quad cache.
   - A compiled width-4 list whose `list2VBO` path has
     already filled `vbo_array`. First draw and later draws
     both produce the wide quad, not an empty frame from
     treating CPU pointers as VBO offsets.

10. **Patch mechanics.** `git apply --check` clean on the pristine
   pin and on the pin with the earlier patches applied;
   reverse-apply against the tested tree. `make check-c99` does
   **not** cover this: it syntax-checks `$(SRCS)`
   (`Makefile:2390`), and a new file under `packaging/web/bench/`
   is not in that list. `check-trailing-whitespace` explicitly
   excludes `packaging/web/patches/` (`Makefile:2406`). Instead:
   `grep -n ' $'` on the new patch, `web-deps.sh`, this plan, and
   the bench; and an `emcc -fsyntax-only` (or the existing
   `make bench-web-gl4es` compile) of the bench.

`make test-web` will not catch this. It links the GL stubs, not
gl4es. The browser lane *is* the test.

## Non-goals (v1)

- **App-side workarounds.** No octahedron-for-points redux. The
  point-smooth lesson: fix gl4es, delete the forks, keep one C path.
- **World-space expansion.** Desktop `glLineWidth` is pixels. A
  world-space ribbon would thicken under the camera.
- **Cached expanded VBOs** on named display lists.
- **Miter / round joins.** Different look from native aliased
  lines, and a second implementation.
- **`GL_LINE_SMOOTH`.** Follow-up patch, same directory, same
  relationship `gl4es-point-smooth` had to points: a 1 px alpha
  falloff across the quad (a varying, or a 1D gradient) folded into
  alpha after the alpha test. Do not block width on AA.
- **FPE vertex-shader expander.** Correct for lit lines (lighting
  stays on the original object-space position) and for clip planes,
  but it is a new program key, new attributes (other endpoint +
  side), and a much larger review. gl-repl’s wide lines are almost
  all unlit chrome. Revisit only if a lit user scene is actually
  wrong.
- **Sub-pixel widths** (`0.9f` grid minors) without coverage AA.
- **Changing native or stub builds.**
- **Closing the Known deviations** (fog, lighting, clip-plane skip,
  tess `GL_LINE_LOOP` overlay) in this patch.
- **An explicit bias for the tessellation overlay.** Native
  `_LINE` does not pull `glBegin(GL_LINE_LOOP)` forward. If
  that overlay later needs protection, it is its own policy.

## Follow-up (not this patch)

`gl4es-line-smooth.patch`: when `GL_LINE_SMOOTH` is enabled, emit
the same quad with a 1 px feather on each long edge and multiply
coverage into alpha after the alpha test — the point-smooth
pattern. Width emulation must already be in so there is a quad to
feather; native 1 px `GL_LINES` cannot AA on WebGL either, so the
smooth follow-up should take the quad path for width == 1 as well
when the enable is on. That is a deliberate extra cost, gated on
the enable, and is why it is not folded into v1.

## Implementation order

0. **Outline-over-face screenshot on the current tree.** Width 1,
   vertex outlines on. If it speckles, that is Prerequisite,
   not a width-emulator bug. **Not done before the patches**
   (see Implementation findings). Still the right first
   headful shot.
1. **`gl4es-polygon-offset-line.patch`.** Shadow the enable,
   PushAttrib (LINE enable **and** factor/units), both
   `glPolygonOffset` / `glPolygonOffsetx` mirrors, cache
   `DEPTH_BITS`, land the shared matrix helper,
   projection-row depth bias around the polygon-mode line
   draw (never mutate `line_arrays`). Re-shoot case 2 at
   width 1 — that screenshot is the independent-test
   claim. No quad code. `git apply --check` against the
   pin with the earlier patches on it.
2. **Oracles on that tree, no width change.** Trailing-reset
   (immediate) against current gl4es — it should already flush.
   Mixed-width display list: `draw_renderlist` walks
   `list->calls` (`glPackedCall`) *before* the geometry draw
   on the same node, so a compiled `STAGE_GLCALL` `glLineWidth`
   updates the mirror first. No `linewidth_op`. Leave
   `PUSH_IF_COMPILING` first; same-width early-out only on
   the non-compiling path.
3. `expand_wide_lines` in `line.c` and the `listdraw.c` hook
   **before `line_arrays_to_vbo` (line 891)**,
   `width > (GLfloat)hardext.maxlinewidth` only.
   Identity-MVP draw via the patch-1 helper (all three
   dirty flags), 4D near clip, `GL_FILL` + cull off, offset
   per `from_polygon_mode`, square caps, unbind ARRAY/ELEMENT,
   expand from CPU arrays only. Emit the `GL_LINE_LOOP`
   close. Skip the prerequisite’s projection-row bias on
   the quad path (GPU FILL replaces it). Do not move the
   bias implementation here. Do not write the cached
   object-space verts.
4. `drawing.c` intercept for `GL_LINES` / `GL_LINE_STRIP` /
   `GL_LINE_LOOP` so client-array draws cannot sneak past
   (`from_polygon_mode = 0`).
5. Getter range (three switch arms, not `commonGet`) +
   granularity (`commonGet` is fine) + clamp.
6. Bench + the verification cases above, including the
   width-1 then Bold outline-over-face pair, the
   fill-offset-must-not-leak scene, the cull-on scene, the
   loop-close coverage oracle, the fogged and clip-plane
   screenshots, and the named-list suite.
7. Patch headers, `GL4ES_PATCHES` append (offset-line then
   width-quads), README inventory + dated section.

Do not start by adding a flush. The flush is already there.
Do not start the width patch until case 2 at width 1 is clean.

## Rejected approaches

Recorded so they are not rediscovered during implementation.

- **Geometry shader.** WebGL 2 still has none.
- **Pass-through to GLES `glLineWidth`.** Already what we do; ANGLE
  ignores it.
- **Literal window-space `glOrtho`.** Works, but Z and
  perspective-correct interpolation become a second problem. Clip
  space + identity MVP is the same 2D expansion without that remap.
- **Public `glPushMatrix` / `glPopMatrix` for the identity MVP.**
  Uses the application's matrix stack and can
  `GL_STACK_OVERFLOW`. Save 16 floats, poke identity, restore.
  Dirty all three matrix-cache flags on both sides.
- **CPU clip-Z (or object-space z) bias on the generated
  edge verts.** `expand_line_array` copies object-space
  `list->vert` (`listdraw.c:369–395`). There is no
  clip-space value on the CPU, and writing the cached
  array would bake a camera-dependent z into a shared
  cache. Use a temporary `P_row2 += d · P_row3` poke;
  never mutate `line_arrays`.
- **Putting the two-element line-width ranges in
  `gl4es_commonGet`.** It writes one `GLfloat`.
  `glGetIntegerv` passes a scalar; `glGetDoublev` copies
  only `*tmp`. Follow `GL_POINT_SIZE_RANGE`: three
  switch arms.
- **Marking only `mvp_matrix_dirty` after an identity
  poke.** `getInvMVMat` / `getNormalMat` can recompute
  from identity, clear their flags, and leave a stale
  clean cache after restore. Dirty all three on poke
  and restore.
- **Copy `gl4es-point-size-batch.patch`.** That wrapper has no
  `PUSH_IF_COMPILING`. This one does, and it already flushes.
- **Separate `case GL_LINE_WIDTH_RANGE` and
  `case GL_SMOOTH_LINE_WIDTH_RANGE`.** Same token (`0x0B22`);
  that is a duplicate-case compile error.
- **Only hook `listdraw.c`.** Misses `glDrawArrays` of line
  primitives from `drawing.c`. The stipple intercept is
  `GL_LINES`-only; the wide-line condition has to name strip
  and loop too.
- **Reuse `gen_stipple_tex_coords` as the segment walker.**
  It skips the `GL_LINE_LOOP` close on purpose. Width
  emulation cannot.
- **Expand in the FPE vertex shader first.** See Non-goals. The
  CPU path reuses the stipple projector and does not fragment the
  shader cache.
- **Cache expanded geometry on `glEndList`.** Camera-dependent.
  The polygon-line cache is the wrong precedent here; it caches
  object-space edge topology, not window-space offsets.
- **`w > ε` as the only clip.** Misses the between-eye-and-near
  case (`w > 0`, `z + w < 0`) and still inverted-projects
  `w ≤ 0` if the test is on the wrong side of the divide.
- **Expand after `listActiveVBO` and point at CPU arrays.**
  The bound `GL_ARRAY_BUFFER` makes those pointers VBO
  offsets. Hook before the activate, or unbind first.
- **Insert the wide-path branch after `line_arrays_to_vbo`
  (891–893) but before `listActiveVBO` (895).** That still
  uploads the object-space cache and leaves
  `line_arrays->vbo_array` set, so `use_line_vbo` then
  activates it. The branch belongs before line 891.
- **`glEnable(GL_POLYGON_OFFSET_FILL)` around today’s
  polygon-mode `GL_LINES` draw.** GLES does not offset line
  primitives. That mapping is correct for the quads, and a
  no-op for the width-1 expansion — hence the
  projection-row bias in the prerequisite.
- **Inherit live FILL offset onto every expanded draw.**
  Wrong for genuine `GL_LINES` chrome (grid decals leak)
  and wrong for polygon-mode outlines if LINE is on and
  FILL happens to be off.
- **Hoist the same-width early-out above
  `PUSH_IF_COMPILING`.** Compiling `glLineWidth(4)` while
  the mirror is already 4 drops the packed call; the list
  then draws at whatever the caller’s width is.
- **Give `from_polygon_mode` (and the LINE→FILL
  translation) to genuine `GL_LINE_LOOP` to “fix” the
  tess overlay.** That would also shift every user
  `LINE_LOOP`. Tess protection is a separate explicit
  bias, or nothing.
