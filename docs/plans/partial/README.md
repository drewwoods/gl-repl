# Partial Plan Archive

This directory holds plans where part of the work landed in `main` and
the rest is **deferred on purpose** - paused pending a future review,
prerequisite, or priority shift, not abandoned. Files stay verbatim so
the deferred phases retain their context and a future reader can resume
from the same baseline that paused them.

A plan moves here from `plans/active/` when enough phases have landed
that the work delivers value standing on its own and the remaining
phases are explicitly deferred. A plan can also arrive directly from
`plans/in-review/` when the review finds that part of its scope already
shipped (e.g. cheap cleanups landed ahead of the headline deliverable)
and the rest is deferred on purpose - in that case its status header
should summarize what landed versus what remains. It promotes to
`plans/done/` when every phase lands; it moves back to `plans/active/`
if implementation resumes on the deferred phases.

Current residents:

- `module-architecture-doc-split-layering-audit.md` - three layering
  cleanups landed (replay-annotations move, `edit_overlays` decoupling,
  the REPL/scene `SceneLight` split); the actual `ARCHITECTURE.md` →
  per-module doc split and the heavier app/UI + editor/UI edges are
  deferred.
- `src-repl-simplicity-review.md`
- `makefile-target-conventions.md` - the generated-help and mechanical-cleanup
  half landed (one `## ` per target, three-tier generated help + `help-vars`,
  `.PHONY` derived from the docs, `FORMAT`/`SKIP_CHECKS` as axes instead of
  five near-duplicate targets, six `check-make-*` guards - including the
  name-grammar guard, landed early as a shrink-only ratchet over today's 28
  legacy names); the renames themselves and the `PLATFORM`/`GL_BACKEND` sugar
  are deferred - the plan calls both optional and they carry the ~150-file
  reference blast radius.
- `repl-clarity-review.md` - findings 1-4 and 9-11 landed, plus the
  architecture-document gaps (`src/repl/ARCHITECTURE.md` §4.6, §4.7, §5.4,
  §5.5 and the §10 invariants exist because of it). Findings 5-8 are deferred
  and signposted in the code with `DEFERRED (repl-clarity-review.md finding N)`
  comments: each is gated on a specific future change - a new structured form,
  a seventh state slice, a 17th host callback, or someone opening the import
  state machine - rather than on finding time.
- `app-clarity-review.md` - the three ranked extension guards landed
  (compiler-exhaustive `GlrConfigKey` maps + duplicate-key validation, the
  single-source and now-tested scene-local config roster, the modal-kind
  enum/switch guard with a per-kind wiring test), plus the Low synthetic
  right-click extraction; only the factual comment/declaration sweep
  (finding 4) is deferred.
