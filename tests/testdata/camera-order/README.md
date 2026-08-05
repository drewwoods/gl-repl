# Frozen pre-migration `.glr` fixtures

Verbatim copies of three corpus files as they stood *before* the
`@camera` tag migration (docs/plans/active/camera-header-tags.md). They are
never migrated and never load cleanly again.

Their job is the second of two complementary comparisons, which are easy to
conflate:

- **A/B** — does the new form still render the old scene? That is checked
  against the *migrated* files.
- **Rejection** — does the old form fail loudly? That is what these are for:
  the loader must reject the old shape with the exact `(rule, line,
  conflicting line)` triple the migration guide promises.

Without them, the only record of what the old form looked like is git
history, and a rejection message could silently degrade to "parse error"
without a single test noticing.

The three were chosen to exercise different violation shapes:

| File | Shape |
|---|---|
| `matrix-stack-recursion-stress.glr` | no declarations; `func0` below the body |
| `function-local-shadowing-stress.glr` | declarations already above the camera; markerless camera-shaped transforms |
| `torus-knot-animated.glr` | every violation at once, in 35 lines |
