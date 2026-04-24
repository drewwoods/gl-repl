# Phase 2 State Header Migration

## Summary
This phase is a header-boundary refactor only. The goal is to make `sample.h` shared vocabulary, make `repl_state.h` the typed runtime-state facade, and move broad `extern g_*` access behind focused accessors without changing REPL behavior, file format, or UI semantics.

Bridge retirement is now tracked separately in
[feature/repl-state-phase2-bridge-retirement.md](feature/repl-state-phase2-bridge-retirement.md).

## Key Steps
1. **Lock the header boundary**
   - Remove `repl_state.h` from `sample.h` and keep `sample.h` to enums, fixed capacities, shared structs, defaults, and small stateless helpers.
   - Add `repl_config.h`, `repl_state_compat.h`, and move non-state pipeline DTOs out of `repl_state.h`:
     `FlatProgramView`, `ReplFlattenOptions`, `ReplFlattenResult`, and `FlatCmdLocalVars`.
   - Keep compatibility visible: old callers include `repl_state_compat.h` temporarily instead of getting state through `sample.h`.

2. **Define the runtime container and focused accessors**
   - Add `ReplRuntimeState` in `repl_state.c` and expose typed sub-states for:
     document, flat program, variables/time, editor input, selection/clipboard, code-panel/UI runtime, search/autocomplete, camera/pointer/viewport, presentation, render resources, replay, scene/workspace, variable drag, import/export metadata.
   - Expose the stable API surface from `repl_state.h`:
     init/reset helpers, `*_mut()` accessors, snapshots where needed, dirty helpers, `repl_status_set/clear/tick`, `repl_state_workspace_dir/set_dir`, and workspace-header refresh/parse helpers.
   - Add `ReplConfigKey` and a descriptor API so config mutations stop going through raw `int *` table entries.

3. **Migrate storage in thin, reviewable slices**
   - Move one domain at a time from globals to `ReplRuntimeState`, starting with:
     document + command-store hooks, then flat program + dirty flags, then editor input/selection/clipboard.
   - Follow with camera/pointer/viewport, presentation/config, render resources/derived render state, replay, scenes/workspace, search/autocomplete/status, and variable drag.
   - Each slice should replace direct field writes with a focused helper or accessor, then remove the corresponding compat alias once no production caller uses it.

4. **Stabilize reset/default semantics**
   - Implement `repl_state_init_defaults`, `repl_state_reset_all`, and the narrower example-presentation reset exactly to the sketch’s order and scope.
   - Preserve the current rules for camera inheritance, example defaults, workspace state, dirty flags, and time/predef variable reset.
   - Keep `ReplCommandStore` as the mutation boundary for source commands.

5. **Retire the bridge**
   - Once all production callers use the focused APIs, delete `repl_state_compat.h` aliases and any remaining broad `extern g_*` declarations.
   - Leave only intentional immutable descriptor data in module-local `static const` tables or dedicated descriptor accessors.

## Test Plan
- Run `make test-stubs TEST_JOBS=4` after every storage-migration slice.
- For state/reset/config work, run the focused REPL tests that cover parsing, commit, format, I/O, examples, search, autocomplete, editor, and UI behavior as relevant to the slice.
- On the final pass, run `make sample USE_GL_STUBS=1`, `make sample`, and `make coverage` to confirm the new boundary did not change behavior and to inspect the reset/state coverage.

## Assumptions
- Command language, file format, GLUT entrypoint, and sample-local structure stay unchanged.
- The migration is compatibility-first: keep a bridge while callers move, then delete it after the last production user is off the old globals.
- Phase 2 stops at the state-header boundary; it does not broaden into rendering, import/export format changes, or command-language changes beyond the header/API reshaping needed for ownership clarity.
