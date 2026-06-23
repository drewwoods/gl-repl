# Not-Landed Plans

This directory holds plans that have been **implemented** (in part or in
full) but are **not merged to `main`**. The work lives on a named branch
and the doc explains why it was parked.

A plan moves here when the implementation was completed or substantially
done, but was not merged due to: unresolved vetting concerns,
platform or environment requirements that haven't been met, or a
deliberate decision to keep the work off the main line until conditions
change. It is distinct from `partial/` (some phases landed, rest
explicitly deferred) and `not-started/` (no implementation exists yet).

| Plan | Branch | Why not landed |
|---|---|---|
| `windows-port-mingw.md` | `windows-support` | Implemented but **not vetted on a real Windows machine**. The branch is available and the design is complete; landing is gated on a test run under MinGW-w64 on actual Windows hardware. |
| `ply-line-edge-export.md` | `feature/ply-line-edge-export` | Tests pass but the feature was parked — see the doc's ⚠️ section for the detailed rationale. |
