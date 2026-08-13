# Not-Started Plans

This directory holds drafted plans where **no implementation commits have
landed yet**. Plans here are green-field: the design has been written down
(and may have been in `in-review/` for a read-pass), but the work hasn't
begun. They move to `plans/active/` once implementation starts.

| Plan | Topic |
|---|---|
| `app-clarity-review.md` | Read-only clarity/consistency/maintainability review of `src/app`: ten ranked findings (a `GlrConfigKey` spread over five sites behind two non-exhaustive switches, five functions larger than the two `src/repl` god-functions the size ratchet already guards, the scene-subset roster spelled four times with a false "a test pins this" comment, three shapes of the bridge-installer idiom, …), plus the patterns other `src/app` code should copy |
| `audio-playlist-and-organizer.md` | Audio menu: playlist browser, play/remove, tag organization - browser half **already shipped** by `done/audio-menu.md`; only right-click removal and `tags.txt` tag groups remain |
| `clang-ast-mutation-analysis.md` | Clang AST pass for mutation analysis |
| `example-scene-consistency-review.md` | Read-only consistency pass over the 40 shipped `examples/scenes/*.glr`: fourteen ranked findings on filenames, tone, code style and naming accuracy (three underscore filenames whose section IDs are already hyphenated, `for (` spacing the REPL rewrites before it ships, the GL-state group naming its command in 3 of 5 filenames, a stale `snowfall-demo-550-particles` section id, …), plus the derived filename rule and the list of variations checked and found deliberate |
| `float-returning-repl-functions.md` | Functions that return float values |
| `historic-benchmark.md` | Historic bench trend tracking |
| `local-aware-rebake.md` | Carry function-scoped locals through a value-only rebake, so a global feeding a local stops forcing a full flatten |
| `one-scene-loader.md` | Retire the catalog's own `.glr` walk and make `import.c` the single scene reader, so format - not arrival route - picks the loader; plus the F12 hard block on a failed catalog load |
| `repl-capability-gaps.md` | Prioritized index of the REPL's language + GL-surface gaps, derived from the four stencil-shadow-volume scenes: what each unlocks, effort, and sequencing (indexes `float-returning-repl-functions.md` and `stencil-buffer-support.md` Phase 3 rather than restating them). Carries the measured z-fail / far-plane result arguing why `glMatrixMode` stays out of scope |
| `repl-clarity-review.md` | Read-only clarity/coupling/extensibility review of `src/repl`: ten ranked findings (misfiled `geometry_query` implementation, the missing immediate-mode-vertex predicate, the unguarded twin commit chains, `ReplHostEffects` drift into an editor service locator, the one-directional attrib/inspector ratchet, …), plus the four modules missing from `ARCHITECTURE.md` and four stale claims in CLAUDE.md / the new-command skill |
| `scene-close-capability.md` | Close/remove a user scene (design brief) |
| `smooth-autonormals-with-loop-support.md` | Smooth autonormals with loop awareness |
| `streamed-numeric-input.md` | Framed stdin float groups consumed by a baked `input` expression atom |
