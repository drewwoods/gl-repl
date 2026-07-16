# Partial Plan Archive

This directory holds plans where part of the work landed in `main` and
the rest is **deferred on purpose** — paused pending a future review,
prerequisite, or priority shift, not abandoned. Files stay verbatim so
the deferred phases retain their context and a future reader can resume
from the same baseline that paused them.

A plan moves here from `plans/active/` when enough phases have landed
that the work delivers value standing on its own and the remaining
phases are explicitly deferred. A plan can also arrive directly from
`plans/in-review/` when the review finds that part of its scope already
shipped (e.g. cheap cleanups landed ahead of the headline deliverable)
and the rest is deferred on purpose — in that case its status header
should summarize what landed versus what remains. It promotes to
`plans/done/` when every phase lands; it moves back to `plans/active/`
if implementation resumes on the deferred phases.

Current residents:

- `module-architecture-doc-split-layering-audit.md` — three layering
  cleanups landed (replay-annotations move, `edit_overlays` decoupling,
  the REPL/scene `SceneLight` split); the actual `ARCHITECTURE.md` →
  per-module doc split and the heavier app/UI + editor/UI edges are
  deferred.
- `src-repl-simplicity-review.md`
- `flatten-performance-without-vm.md` — Phases 0–2 landed: benchmarks,
  direct evaluation, warm compiled-expression cache, and slider transaction
  split. Dependency-aware rebake (Phase 3) is intentionally deferred on
  `origin/codex/improve-flatten-phase3`.
