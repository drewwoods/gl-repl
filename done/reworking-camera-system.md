# Camera System Rethink — Orbit Around a Visible Ground Target

## Context

Today the camera uses a **screen-space pan** model: `glTranslatef(px, py, -dist)` applied *after* the orbit rotation, so `g_cam_px`/`g_cam_py` pan the image on screen rather than moving a point in the world. There is no notion of an orbit target, so right-drag slides the whole scene in view space (vertical drag changes the target's world Y unpredictably), and nothing in the scene tells the user where they are orbiting around.

Goals:

1. Right-mouse drag moves the camera target along the world **XZ ground plane** (world Y stays fixed, interpreted as "x/y plane while keeping y constant" = the horizontal plane).
2. The orbit target is visualized in the scene (gizmo).
3. Orbit rotation (left-drag / auto-rotate) pivots *around that target*.
4. Exported `output.c` preserves the same view with the **minimum possible amount of camera code** — drop the `gluLookAt` facade entirely and emit the raw `glTranslatef` / `glRotatef` sequence the REPL already uses internally. Exported view is guaranteed to match the REPL view because it *is* the same sequence.
5. Loading `output.c` round-trips back to `(target, yaw, pitch, dist)` by parsing those same 4 calls directly.

## Approach

### New camera state (repl_core.c ~L332)

Replace:
```c
float g_cam_px = 0.0f, g_cam_py = 0.0f;   /* screen-space pan */
```
with:
```c
float g_cam_tx = 0.0f;   /* orbit target, world X */
float g_cam_ty = 0.0f;   /* orbit target, world Y — held constant by right-drag */
float g_cam_tz = 0.0f;   /* orbit target, world Z */
```
Keep `g_cam_rx`, `g_cam_ry`, `g_cam_dist` unchanged.

Update velocity globals in `repl_editor.c:41-45`: rename `g_vel_px/py` → `g_vel_tx/tz` (XZ plane momentum). Decay + threshold logic stays.

### View transform (scene_render.c:1094-1096)

Replace the current 3-line block with:
```c
glTranslatef(0.0f, 0.0f, -g_cam_dist);
glRotatef(g_cam_rx, 1, 0, 0);
glRotatef(g_cam_ry, 0, 1, 0);
glTranslatef(-g_cam_tx, -g_cam_ty, -g_cam_tz);
```
This is the canonical "orbit around a world-space target" form and makes `rx`/`ry` rotate *around* `(tx, ty, tz)`.

### Right-drag panning (repl_editor.c:2178-2249, `motion_func`)

Current code:
```c
g_cam_px += dx * 0.01f;
g_cam_py -= dy * 0.01f;
```
Replace with an XZ-plane pan derived from the current yaw so "drag right" feels like "right on the ground under the camera":
```c
float ry = g_cam_ry * (float)M_PI / 180.0f;
float cry = cosf(ry), sry = sinf(ry);
float scale = 0.01f * g_cam_dist;   /* zoom-aware */
/* screen-right in world XZ  = ( cry, 0,  sry) */
/* screen-up   in world XZ  = (-sry, 0,  cry)  (projected; ignores pitch) */
float wdx =  dx * cry - dy * -sry;   /* see note */
float wdz =  dx * sry - dy *  cry;
g_cam_tx += wdx * scale;
g_cam_tz += wdz * scale;
/* g_cam_ty intentionally unchanged */
```
Signs to be dialed in during implementation so drag-right moves target right and drag-up moves target away from the camera (the "forward on the ground" direction). Feed the same deltas into `g_vel_tx`/`g_vel_tz` for momentum.

Middle-drag (zoom) and left-drag (orbit) are unchanged.

### Target gizmo (scene_render.c, after `draw_axes()` at L1152)

Add `draw_orbit_target()` — drawn only in the REPL, never exported. Simple, cheap, readable:
- A small XZ-plane cross (two line segments through `(tx, ty, tz)`, ~`0.05 * g_cam_dist` long) in a distinctive color (e.g. yellow).
- Depth-test off + line-smooth on so it stays visible through geometry.
- Gated on an existing overlay toggle or a new `CfgItem` in `g_cfg_items[]` (repl_core.c ~L663) — default on. This follows the same pattern as grid/axes toggles.

### Export (repl_export.c — replace `update_lookat_strings` and rename)

Drop the gluLookAt facade. Emit exactly the same transform sequence the REPL uses in `render_3d_scene()`. No eye/up computation, no trig, no inversion — the export *is* the REPL's camera:

```c
/* renamed from g_lookat[] / update_lookat_strings() */
char g_cam_lines[4][96];

void update_cam_lines(void) {
    snprintf(g_cam_lines[0], sizeof(g_cam_lines[0]),
             "  glTranslatef(0.0f, 0.0f, %.4ff);", -g_cam_dist);
    snprintf(g_cam_lines[1], sizeof(g_cam_lines[1]),
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);", g_cam_rx);
    snprintf(g_cam_lines[2], sizeof(g_cam_lines[2]),
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);", g_cam_ry);
    snprintf(g_cam_lines[3], sizeof(g_cam_lines[3]),
             "  glTranslatef(%.4ff, %.4ff, %.4ff);",
             -g_cam_tx, -g_cam_ty, -g_cam_tz);
}
```

`save_output()` (repl_export.c:1674, around L1714) writes 4 lines instead of 3. The `LOOKAT_LINE_COUNT` constant becomes `CAM_LINE_COUNT = 4`. Exported `display()` now looks like:

```c
void display() {
  glClear(...);
  glLoadIdentity();
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glEnable(GL_MULTISAMPLE);
  glEnable(GL_LINE_SMOOTH);
  glTranslatef(0.0f, 0.0f, -5.0000f);
  glRotatef(20.0000f, 1.0f, 0.0f, 0.0f);
  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);
  glTranslatef(-1.5000f, 0.0000f, 2.0000f);
  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   /* optional animation */
  /* ... user geometry ... */
}
```

No `gluLookAt`, no helper function in the exported file, no extra camera globals — camera is 4 plain GL calls. Since this is the *exact* sequence `render_3d_scene()` emits, REPL view and exported view are provably identical (no floating-point drift through trig round-trip).

### Live code-panel display (the "no magic" requirement)

Today `update_lookat_strings()` is called on every camera change and the `g_lookat[]` strings are rendered directly in the REPL's visible code panel (see `ui_panels.c:512`, `ui_panels.c:1186`, via `RENDER_STATIC_LINE`, and iterated in `repl_core.c:719`, `repl_export.c:1993`, `repl_export.c:2029`). The user sees the exact `gluLookAt(...)` the exported app will run, updating live as they orbit.

The new `g_cam_lines[4]` replaces `g_lookat[]` as a **drop-in**: same call sites, same display mechanism, same per-frame refresh. Every place currently iterating `for (i = 0; i < LOOKAT_LINE_COUNT; i++) ... g_lookat[i]` becomes `for (i = 0; i < CAM_LINE_COUNT; i++) ... g_cam_lines[i]` with `CAM_LINE_COUNT = 4`. Every call site of `update_lookat_strings()` becomes `update_cam_lines()`:

- `repl_core.c:717`, `repl_core.c:3543` (pre-render refresh)
- `repl_export.c:1687` (save), `:1981`, `:2017` (dump paths)

The code panel now shows the user the four real `glTranslatef`/`glRotatef` calls updating live as they drag — the same calls the exported `output.c` will run. No hidden conversion, nothing the user sees in the panel that isn't in the exported file.

Also update the test file that iterates over these strings: `test_repl_core_commit.c:133`. And the SVG annotation doc (`repl-code-panel-annotated.svg:179`) can be left alone or updated to `g_cam_lines[]` — cosmetic.

### Import (repl_export.c — replace `lookat_to_cam_state` / `import_parse_lookat_block`)

Replace the multiline gluLookAt accumulator in `load_from_file()` (~L1795) with a per-line sniffer that recognizes the 4 camera calls by their fingerprints and extracts scalars directly:

- `glTranslatef(0.0f, 0.0f, <d>)` → `g_cam_dist = -d`
- `glRotatef(<a>, 1.0f, 0.0f, 0.0f)` → `g_cam_rx = a`
- `glRotatef(<a>, 0.0f, 1.0f, 0.0f)` → `g_cam_ry = a`
- `glTranslatef(<x>, <y>, <z>)` (the *second* translate, non-zero X/Y axis) → `g_cam_tx = -x; g_cam_ty = -y; g_cam_tz = -z`

Matching is done with `sscanf` on a handful of format strings; order is determined by axis-vector fingerprint, not by position, so the parser is robust to whitespace. Trivial — no `asinf`/`atan2f`/`sqrtf` anywhere in the import path.

### Backward compatibility with pre-refactor `output.c`

Old files use 3-line `gluLookAt`. Keep a minimal legacy branch: if `load_from_file()` sees `gluLookAt` it runs the *current* parser (moved mostly untouched into a `legacy_parse_lookat_block` helper), which still computes `(rx, ry, dist, tx, ty, tz)` via the old inversion math and assigns to the new globals. Old `update_lookat_strings()` and `g_lookat[]` are deleted — legacy path is read-only. This keeps the refactor small and lets existing checked-in `output.c` files keep loading.

## Files touched

| File | Change |
|------|--------|
| `repl_core.c` (~L332) | rename pan globals → target globals |
| `repl_core.c` (~L663, optional) | add `CfgItem` toggle for target gizmo |
| `sample.h` | update `extern` declarations |
| `repl_editor.c` (L41-45, L2178-2249) | rename velocity globals; rewrite right-drag as XZ-plane pan |
| `scene_render.c` (L1094-1096) | new transform order (orbit-around-target) |
| `scene_render.c` (after L1152) | add `draw_orbit_target()` gizmo |
| `repl_export.c` (L133-137, L404-425) | replace `g_lookat[]`/`update_lookat_strings` with `g_cam_lines[4]`/`update_cam_lines` emitting raw translate+rotate sequence |
| `repl_export.c` (L1674+, ~L1714) | `save_output()` writes 4 cam lines instead of 3 |
| `repl_export.c` (L354-401, ~L1795) | rewrite importer: per-line sscanf on the 4 calls; move old gluLookAt parser into `legacy_parse_lookat_block()` read-only fallback |

Search-and-replace for `g_cam_px`/`g_cam_py` across the tree to catch any other readers (slider panel, undo snapshot, tests).

## Verification

1. `make sample && ./sample` — left-drag orbits around origin; yellow target gizmo visible at origin.
2. Right-drag: target gizmo slides along ground plane; camera follows it; world Y of the gizmo never changes (watch against grid lines). Left-drag after panning orbits around the *new* target, not the origin.
3. Middle-drag zoom still works; pan speed scales with zoom (far view → larger pan steps).
4. Ctrl+S → inspect `output.c`: contains exactly 4 camera lines (`glTranslatef` / `glRotatef` / `glRotatef` / `glTranslatef`), no `gluLookAt`, no helper function.
5. `gcc ... output.c && ./a.out` — rendered view matches the REPL view at save time.
6. `./sample output.c` — reloaded scene shows the same camera; orbit target gizmo appears at the saved center; left-drag orbits around it.
7. `make test_repl_core_io` — save/load round-trip tests pass. Add a case: set `(tx, ty, tz) = (1.5, 0, -2.0)`, save, reload, assert recovered within `1e-3`.
8. Load a *pre-refactor* `output.c` to confirm backward compatibility.
