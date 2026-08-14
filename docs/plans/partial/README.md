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
- `repl-clarity-review.md` - the four top-ranked findings landed
  (`geometry_query.c` split out of `autonormal.c`,
  `repl_cmd_emits_immediate_vertex()` replacing six open-coded spellings,
  one accurate new-command checklist, and the now-exhaustive
  `repl_attrib_bits_for_cmd` switch). Findings 5-11, the architecture-document
  gaps, and the `round` doc omission are deferred; 5 and 7 are deliberately
  gated on a future consumer landing, and 11 is record-only.
- `app-clarity-review.md` - the three ranked extension guards landed
  (compiler-exhaustive `GlrConfigKey` maps + duplicate-key validation, the
  single-source and now-tested scene-local config roster, the modal-kind
  enum/switch guard with a per-kind wiring test), plus the Low synthetic
  right-click extraction; only the factual comment/declaration sweep
  (finding 4) is deferred.
