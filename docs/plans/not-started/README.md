# Not-Started Plans

This directory holds drafted plans where **no implementation commits have
landed yet**. Plans here are green-field: the design has been written down
(and may have been in `in-review/` for a read-pass), but the work hasn't
begun. They move to `plans/active/` once implementation starts.

| Plan | Topic |
|---|---|
| `audio-playlist-and-organizer.md` | Audio menu: playlist browser, play/remove, tag organization - browser half **already shipped** by `done/audio-menu.md`; only right-click removal and `tags.txt` tag groups remain |
| `clang-ast-mutation-analysis.md` | Clang AST pass for mutation analysis |
| `float-returning-repl-functions.md` | Functions that return float values |
| `historic-benchmark.md` | Historic bench trend tracking |
| `local-aware-rebake.md` | Carry function-scoped locals through a value-only rebake, so a global feeding a local stops forcing a full flatten |
| `one-scene-loader.md` | Retire the catalog's own `.glr` walk and make `import.c` the single scene reader, so format - not arrival route - picks the loader; plus the F12 hard block on a failed catalog load |
| `repl-capability-gaps.md` | Prioritized index of the REPL's language + GL-surface gaps, derived from the four stencil-shadow-volume scenes: what each unlocks, effort, and sequencing (indexes `float-returning-repl-functions.md` and `stencil-buffer-support.md` Phase 3 rather than restating them). Carries the measured z-fail / far-plane result arguing why `glMatrixMode` stays out of scope |
| `repl-clarity-review.md` | Reviewed clarity/coupling/extensibility audit of `src/repl` (2026-08-14): the spine is sound; eleven ranked findings after correcting the first draft on table-driven parsing/flattening, the host-effects split urgency, the limits of a commit-chain corpus test, and the existing `one-scene-loader.md` design. Top items are the misfiled `geometry_query` implementation, the missing immediate-mode-vertex predicate, stale new-command guidance, and the one-directional attrib/inspector ratchet |
| `scene-close-capability.md` | Close/remove a user scene (design brief) |
| `smooth-autonormals-with-loop-support.md` | Smooth autonormals with loop awareness |
| `streamed-numeric-input.md` | Framed stdin float groups consumed by a baked `input` expression atom |
| `tutorial-catalog-review.md` | Read-only consistency/clarity/coverage review of the 25 built-in tutorials as a set (2026-08-14): twelve ranked findings - the lighting run ordered so each lesson depends on the next, two entries that exist to exercise the runner rather than teach GL, beginner lessons with no closing takeaway, runner vocabulary in learner-facing text, three wrong tags - plus ten proposed tutorials in three tiers (transform hierarchy, expressions & motion, replay, keeping your work) |
