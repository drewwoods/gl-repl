# Edit-overlay guide-label placement coordination

## Summary

- Make vertex-label placement avoid text-bearing edit guides: partial-vertex,
  normal, clip-plane, translation, rotation, and replay-focused normal labels.
- Keep guide labels fixed and higher priority. Vertex labels continue using the
  existing bounded, sticky fixed-padding solver and may move vertically or be
  dropped when no nearby row is clear.
- Preserve `At vertex` as the explicit exact-position bypass.

## Status (2026-07-13)

- [x] Added a nullable guide-label sink to `Render3dGuideSnapshot`.
- [x] Recorded single- and compound-text labels from geometry guides,
  transform guides, and replay-focused normals.
- [x] Rebuilt a bounded obstacle store per overlay subpass, retained the final
  subpass modelviews, and reprojected them with the canonical post-resolve
  projection.
- [x] Made guide obstacles strict: incumbent vertex-label leniency never
  permits overlap with guide text.
- [x] Kept the original fixed 15 px entry/homing padding, 17 px row pitch,
  sticky row memory, easing, and bounded drop behavior unchanged.
- [x] Updated user documentation and focused guide/edit-overlay tests.

Verification:

- `make check-c99` and `make test-stubs` pass locally.
- `make test-stubs`: 59/59 binaries and 18,182/18,182 assertions pass.
- Focused results: `test_edit_overlays` 251/251 and
  `test_render3d_guides` 80/80.
- Targeted guide tests cover partial-vertex, normal, clip-plane, translation,
  rotation, and compound runs.
- Targeted edit-overlay tests cover canonical projection, strict separation,
  forced displacement, bounded dropping, subpass reset, replay normals, and
  `At vertex` bypass.
- The real-GCC gracemont cross-check was skipped at the user's request because
  that host was unavailable.

## Boundaries

- The sink is an internal rendering interface; guide renderers do not depend
  on the edit-overlay subsystem.
- User `label()` text, grid/axes labels, and light indicators remain outside
  this collision system.
- Population-based padding, elastic row pitch, and placement-churn pressure
  are intentionally excluded and remain on the stacked experimental branch.
