# C99 compatibility (C2x stays the main target)

Status: **design ready; not implemented**. Decisions locked with the
user: the C99 bar is **source-owned app/library translation units
syntax-check clean with GCC and Clang under `-std=c99 -pedantic-errors`**
(GNU extensions GCC/Clang accept under that mode may stay); the
regression guard is a **`make c99` target wired into the standard gate**.

Do not implement until this file moves to `not-started/`.

## Context

This is a deliberately retro OpenGL project; the user wants the source to
also build clean as **C99** while `-std=c2x` (C23) remains the primary,
default build. Today the tree compiles only because GCC/Clang accept
C11/GNU constructs as extensions in `-std=c2x`. An audit of the
source-owned app/library translation units found these known blockers
for a `gcc/clang -std=c99 -pedantic-errors` syntax check:

1. **C11 `_Static_assert`** — 3 sites: `src/app/glr_debug.c:107`,
   `src/app/glr_ctrl.c:1238`, `src/app/glr_ctrl.c:1240`. `_Static_assert`
   is a C11 keyword; under `-std=c99 -pedantic-errors` it errors.
2. **GNU `, ##__VA_ARGS__`** — `WRITE_TEXT` / `WRITE_TEXT_APPEND` macros
   in `src/repl/parser.c:257` / `:261`. The GNU comma-elision is not ISO
   C and `-pedantic-errors` rejects it.
3. **C11-compatible duplicate typedef redefinition** —
   `VariablePanelValueChange` is forward-typedefed in
   `src/widgets/variable_panel_drag.h:18` and typedefed again with the
   struct body in `src/widgets/variable_panel_state.h:42`. Clang accepts
   that as a C11 typedef-redefinition extension, but rejects it under
   `-std=c99 -pedantic-errors`.
4. **Old-style function-pointer casts for GLU tessellation callbacks** —
   `src/repl/executor.c:137`–`:146` casts callback functions to
   `void (*)()`. Clang diagnoses that as a strict-prototypes error under
   the C99 pedantic bar. `src/repl/export.c:561`–`:566` emits the same
   cast shape into exported C; update it too if exported output is covered
   by the C99 promise.

Everything else in the current source-owned app/library TU set is
expected to be clean for the chosen bar, but `make c99` compiler output
is authoritative:

- `_Alignof` is already gated (`REPL_HAS_ALIGNOF` in `glr_debug.c`:
  `_Alignof` under C11+, GNU `__alignof__` otherwise) — C99-safe as-is.
- `__attribute__((constructor/format/unused))` — GCC/Clang accept these
  under `-std=c99 -pedantic-errors` without diagnostic; the behavioral
  `((constructor))` sites (`src/repl/state.c:198`,
  `src/widgets/tutorial_state.c:25`) are already
  `#if defined(__GNUC__) || defined(__clang__)`-gated. **Chosen bar is
  gcc/clang**, so no change and no behavior change.
- The `union` in `src/ui/editor.h:27` is a *named* member (`} state;`),
  not a C11 anonymous union — fine.
- Sweep found no statement-expressions, `typeof`, `_Generic`, binary
  literals, case-ranges, `[[attributes]]`, `nullptr`, or VLAs.

Scope note: `make c99` checks the maintained app/library source set
(`$(SRCS)` plus the standalone demo entry TUs if they are not already in
that list). It does **not** blindly syntax-check every `*.c` in the
repository: generated/export harness files such as
`tests/export_trace_driver.c` require build-time macros, and some tests
use test-only idioms such as empty variadic initializer macros that are
not part of the product-source compatibility promise.

Outcome: `make` (c2x) unchanged; a new `make c99` syntax-checks the
maintained app/library source set under `-std=c99 -pedantic-errors` and
is wired into the standard gate so C99 conformance can't silently
regress.

## Approach

### 1. Portable `STATIC_ASSERT` shim — new header `include/c_compat.h`

`include/` is the documented home for header-only helpers (CLAUDE.md) and
is on every TU's `-Iinclude` path.

```c
#ifndef C_COMPAT_H
#define C_COMPAT_H

/* Real _Static_assert under C11+; C99 negative-array-size fallback
 * otherwise. The fallback drops the message text (only the
 * negative-size error shows) — acceptable. Distinct __LINE__ per use
 * keeps typedef names unique; current call sites are all on separate
 * lines. Valid at file and block scope (typedef). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#else
#  define STATIC_ASSERT__CAT2(a, b) a##b
#  define STATIC_ASSERT__CAT(a, b)  STATIC_ASSERT__CAT2(a, b)
#  define STATIC_ASSERT(expr, msg) \
     typedef char STATIC_ASSERT__CAT(c_compat_static_assert_at_line_, __LINE__) \
         [(expr) ? 1 : -1]
#endif

#endif /* C_COMPAT_H */
```

Then:

- `src/app/glr_debug.c`: `#include <c_compat.h>`; replace the
  `_Static_assert(...)` at line 107 with `STATIC_ASSERT(...)` (keep it
  inside the existing `#if defined(REPL_HAS_ALIGNOF)` guard, same block
  scope).
- `src/app/glr_ctrl.c`: `#include <c_compat.h>`; replace both
  `_Static_assert(...)` (lines 1238, 1240) with `STATIC_ASSERT(...)`
  (file scope).
- `src/ui/snapshot.h:52`: refresh the stale comment that says
  "`_Static_assert` in glr_ctrl.c" → "`STATIC_ASSERT` in glr_ctrl.c".

Keep argument order/messages identical so the c2x build (which takes the
real `_Static_assert` path) is byte-for-byte unchanged.

### 2. Fix the duplicate `VariablePanelValueChange` typedef

Move the complete typedef into `src/widgets/variable_panel_drag.h` and
remove the second typedef from `src/widgets/variable_panel_state.h`.
Then include `widgets/variable_panel_drag.h` from
`variable_panel_state.h` before the handler prototypes.

Target shape:

```c
/* src/widgets/variable_panel_drag.h */
typedef struct VariablePanelValueChange_s {
    char  name[16];
    float value;
} VariablePanelValueChange;
```

`variable_panel_state.h` should use the type but not redeclare it. That
keeps existing include order working for `glr_ctrl.c`,
`variable_panel_drag.c`, and tests that include either header first.

### 3. Replace old-style GLU callback function-pointer casts

The real GLU headers declare `gluTessCallback` with an old-style
callback pointer on some platforms, and the local GL stubs currently use
a cleaner `GLUfuncptr` typedef. Keep source casts C99-clean by adding a
single local callback-pointer typedef in `src/repl/executor.c`, for
example:

```c
typedef void (*ReplGluCallback)(void);
```

Then cast callback registrations to `ReplGluCallback` instead of spelling
`void (*)()` inline. If the real platform header still exposes an
old-style callback parameter and clang diagnoses the header itself, keep
`make c99` on the local GL stubs path or mark external GL includes as
system includes for this target; do not weaken source-owned diagnostics.

Also update the export template in `src/repl/export.c` so generated code
emits a named C99-clean callback typedef and uses that typedef in the
`gluTessCallback` lines, rather than emitting `(void (*)())`.

### 4. Drop the GNU comma-elision in `src/repl/parser.c`

Audit result: **every** `WRITE_TEXT(...)` / `WRITE_TEXT_APPEND(...)` call
passes at least one variadic argument after the format string (verified
across all ~20 call sites; no `WRITE_TEXT(fmt)`-only call exists). So the
`##` is never functionally needed — replace `, ##__VA_ARGS__` with
`, __VA_ARGS__` in both macro definitions (lines 257–264). Zero
behavioral change in either standard mode.

- `WRITE_TEXT_APPEND` is **defined but unused** (only the `#define`
  matches; no call sites). Default: delete it (and its `#undef`). If a
  reviewer prefers a minimal diff, just convert its `##__VA_ARGS__`.

### 5. `make c99` syntax-check target + standard-gate wiring (Makefile)

- Add a `c99` target: per-TU `-fsyntax-only` over the maintained
  app/library source set with the existing warning flags plus
  `-std=c99 -pedantic-errors` (syntax-only; no run). Reuse the existing
  source lists, but be explicit about scope:
  - include `$(SRCS)`;
  - include `tools/scene_demo/scene_demo.c`, `tools/repl_demo/repl_demo.c`,
    `tools/repl_demo/stubs.c`, and other maintained standalone entry TUs
    if they are outside `$(SRCS)`;
  - do not include generated harnesses that require ad-hoc `-D...`
    inputs, unless the target supplies those inputs.
- Ensure `-std=c99` is the final `-std=` flag or filter `-std=c2x` out of
  `COMMON_CFLAGS` for this target. `COMMON_CFLAGS` currently contains
  `-std=c2x`, so appending the C99 flag in the wrong position can silently
  test the wrong standard.
- Force the local GL stub include path for `make c99` (or mark external
  GL/freeglut include directories as system includes). The guard is meant
  to hold source-owned code accountable; it should not fail on old-style
  callback typedefs in platform OpenGL headers.
- Mirror the project's guard idiom: add `check-c99` into the
  `check-state-ownership` aggregator / documented standard gate so the
  expected pre-merge sequence includes it. Match the existing
  `▶ … OK` sub-target formatting.
- The compiler output is the authoritative blocker list; the audit above
  is the expected, not exhaustive, set. Most likely extra is the
  `#warning` in `glr_debug.c` (GCC/Clang generally accept it under
  `-pedantic-errors`; if it errors, gate it the same
  `__GNUC__`/`__clang__` way used nearby). The C99 fallback typedef may
  trip `-Wunused-local-typedefs` at the block-scope site only if
  `-Wextra` is added — current flags are `-Wall` only, so not expected;
  add `-Wno-unused-local-typedefs` to the `c99` target only if it
  surfaces.

### 6. Docs

`CLAUDE.md`: document that the tree must build clean under both
`-std=c2x` (default) and `make c99` (`-std=c99 -pedantic-errors`
syntax-check over maintained app/library TUs); the `STATIC_ASSERT` shim
(`include/c_compat.h`) is the required spelling for compile-time asserts
(no raw `_Static_assert`); callback function-pointer typedefs must be
named/prototyped rather than old-style `void (*)()`; `WRITE_TEXT`-style
variadic macros must use plain `__VA_ARGS__` (no `, ##`). Add `make c99`
to the Boundary Checks / gate list.

## Critical files

- `include/c_compat.h` — **new**, portable `STATIC_ASSERT`.
- `src/app/glr_debug.c` — include shim; line 107 → `STATIC_ASSERT`.
- `src/app/glr_ctrl.c` — include shim; lines 1238/1240 → `STATIC_ASSERT`.
- `src/ui/snapshot.h` — line 52 comment refresh.
- `src/widgets/variable_panel_drag.h` — own the full
  `VariablePanelValueChange` typedef.
- `src/widgets/variable_panel_state.h` — include/use that typedef; remove
  the duplicate typedef body.
- `src/repl/executor.c` — replace old-style `void (*)()` callback casts
  with a named C99-clean callback typedef.
- `src/repl/export.c` — update exported tessellation callback setup so
  generated C does not emit old-style `void (*)()` casts.
- `src/repl/parser.c` — lines 257–264: drop `, ##`; delete unused
  `WRITE_TEXT_APPEND`.
- `Makefile` — new `c99` / `check-c99` target; wire into aggregator/gate.
- `CLAUDE.md` — document the dual-standard requirement + `make c99`.

## Reuse / conventions followed

- Feature gating mirrors existing patterns: `REPL_PRINTF_LIKE`
  (`src/repl/util.h`, `__GNUC__`/`__clang__`) and
  `REPL_HAS_ALIGNOF`/`REPL_ALIGNOF` (`src/app/glr_debug.c`,
  `__STDC_VERSION__ >= 201112L`). The shim uses the same
  `__STDC_VERSION__ >= 201112L` test.
- `include/` placement matches CLAUDE.md ("`include/` is for header-only
  helpers and vendored single-header dependencies").
- `make c99` + aggregator wiring matches the established
  `check-state-ownership` standing-guard culture.

## Verification

1. `make c99` — maintained app/library source set syntax-checks clean
   under `-std=c99 -pedantic-errors` (the new authoritative check).
   Iterate until zero diagnostics.
2. Default gate unchanged and green: `make sample` (GL +
   `USE_GL_STUBS=1`), `make repl_demo`, `make scene_demo`, `make test`,
   `make test-stubs`, `make check-state-ownership`.
3. Confirm the target really tests C99: run `make c99` with both
   `CC=clang` and `CC=gcc` where available, and inspect the printed
   command or failure output to ensure `-std=c99` is the final standard
   flag.
4. c2x build behaviorally identical: real `_Static_assert` path is taken
   under c2x, the `VariablePanelValueChange` representation stays the
   same, and the parser/export/executor changes are spelling-only at
   runtime. Expect the same pass counts (~4590 `make test`, ~5074
   `make test-stubs`).
5. Spot-check the C99 fallback fires: temporarily break one
   `STATIC_ASSERT` condition, confirm `make c99` fails with the
   negative-array-size error, then revert.

## Folder note

`plans/in-review/` = decision pending. Direction and the two locked
decisions (C99 bar = GCC/Clang `-std=c99 -pedantic-errors` syntax-check
over maintained app/library TUs; guard = `make c99` in the standard
gate) are settled; this stays here until scheduled. Lifecycle:
in-review → `not-started/` → `active/` → `done/`.
