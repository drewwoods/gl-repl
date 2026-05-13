# Clang AST Mutation-Analysis Pass

## Context

The current file-level Mermaid callgraph work can distinguish structure, but it
cannot answer an important semantic question reliably:

> Does this edge represent a mutating access, or only a read / render-only use?

Today the repository can answer parts of that with naming conventions and grep
checks:

- `repl_state_*_mut()` accessors are explicitly mutable in `repl_state.h`
- mutator-style verbs such as `set_`, `clear_`, and `reset_` are already used
  pervasively in the runtime-state facade
- the Makefile already contains boundary checks that grep for direct mutation
  patterns such as `repl_state_*_mut(...)`

Those heuristics are useful, but they are not enough for high-confidence edge
classification. `cflow` gives only call relationships. It does not know whether
the callee writes through a returned pointer, mutates globals, or merely reads a
state snapshot.

This note scopes the work required to build a repo-specific Clang AST /
static-analysis pass that classifies mutating accesses and can eventually feed
that information into the generated callgraph outputs.

## Goal

Build a Clang-based analysis tool that can answer, for each function and each
call edge in this codebase:

- `read_only` - no writes to tracked REPL/runtime state
- `direct_mutation` - function directly mutates tracked state
- `transitive_mutation` - function itself may only call helpers, but the call
  chain mutates tracked state
- `unknown` - analysis could not prove read-only or mutating behavior safely

For Mermaid integration, the immediate use is simpler:

- style file-to-file or function-to-function edges differently when the
  underlying callsite is mutating versus non-mutating

## Recommendation

Use a **standalone Clang LibTooling executable**, not a Clang plugin and not a
custom extension of `clang --analyze`.

Why:

- this repo is Makefile-driven, not CMake-driven, so a standalone tool is the
  least invasive integration path
- the current `make analyze` target uses Clang Static Analyzer checkers, which
  are valuable for bug finding but are the wrong abstraction for a custom
  repository-specific ownership analysis
- a standalone tool can emit exactly the JSON needed by the Mermaid generators
  without forcing the project into Clang plugin loading or `clang-tidy` wiring

`clang-tidy` is a reasonable second option if long-term ergonomics become more
important than initial implementation speed, but it is not the best starting
point for this codebase.

## What The Tool Must Understand

The analysis does not need to solve arbitrary C semantics. It needs to solve
the mutation model that actually exists here:

### 1. Runtime-state facade mutations

Examples:

- `repl_state_*_mut()`
- `repl_state_*set_*()`
- `repl_state_*clear_*()`
- `repl_state_*reset_*()`
- writes through pointers obtained from mutable accessors

This is the highest-value and lowest-ambiguity analysis target.

### 2. REPL-owned mutation APIs

Examples:

- `repl_command_store_*`
- `repl_actions_*`
- `repl_var_drag_*`
- scene/workspace persistence paths such as `repl_save_workspace()` and
  `repl_export_save_output()`

These are semantically mutating even when they do not spell mutation with a
`_mut` suffix.

### 3. Global writes

The sample still contains historical global-state patterns outside the typed
facade. The pass needs to treat direct assignment to tracked globals as
mutation.

### 4. Pointer-based indirect writes

The hard part is not recognizing a call to `repl_state_document_mut()`. The
hard part is recognizing this pattern:

```c
ReplDocumentState *doc = repl_state_document_mut();
doc->cmd_count[0] = 0;
```

or:

```c
ReplEditorInputState *inp = repl_state_editor_input_mut();
inp->input_len[0] = 0;
```

That requires AST-level tracking of values returned from mutable accessors and
then following subsequent stores through those aliases.

## Proposed V1 Scope

V1 should be intentionally narrow and useful:

1. Parse the repo using Clang tooling with the real compile flags.
2. Build a per-function summary of:
   - direct mutable-accessor calls
   - writes through aliases derived from mutable accessors
   - calls to known mutator families
   - writes to tracked globals
3. Build a call graph from AST call expressions.
4. Propagate mutation summaries transitively through that graph.
5. Emit machine-readable JSON suitable for the Mermaid pipeline.

V1 should **not** attempt full alias analysis or proof-grade purity checking.
Anything ambiguous should stay `unknown`.

## Expected Output

Recommended JSON shape:

```json
{
  "functions": {
    "repl_editor_handle_key": {
      "file": "repl_editor.c",
      "classification": "direct_mutation",
      "direct_reasons": [
        "calls repl_state_editor_input_mut",
        "writes through mutable accessor alias",
        "calls repl_command_store_insert_one"
      ]
    }
  },
  "edges": [
    {
      "caller": "ui_panels_handle_click",
      "callee": "repl_actions_toggle_grid",
      "classification": "mutating"
    }
  ]
}
```

The Mermaid stage can then collapse from function-level data down to
file-to-file summaries while preserving mutation counts or mutation flags.

## Architecture Of The Analysis

### Phase A: Build / tooling plumbing

The tool will need:

- `compile_commands.json`
- a reproducible LLVM/Clang version choice
- a local build target in the Makefile

This repo does not currently produce `compile_commands.json`. That is the first
real integration task.

Possible approaches:

1. `bear -- make sample`
2. `intercept-build make sample`
3. generate the compile database directly from `Makefile` variables

Recommendation: start with `bear` or `intercept-build`. Generating the database
directly from the Makefile is possible, but it becomes its own maintenance
problem.

### Phase B: Function and callsite collection

Use Clang AST visitors or AST matchers to collect:

- every `FunctionDecl` with a body
- every `CallExpr`
- the containing function for each callsite
- source file / line information

This gives the tool an AST-derived call graph independent of `cflow`.

### Phase C: Direct mutation detection

The pass should classify a function as directly mutating if it contains any of:

- a call to a known mutable accessor
- a call to a known mutator family
- an assignment / increment / decrement to a tracked global
- a store through a pointer alias derived from a mutable accessor

Important implementation detail:

- do not classify a function as mutating merely because it *mentions* a mutable
  pointer type; require an actual write sink

### Phase D: Alias tracking for mutable accessors

This is the first part that is truly AST-work rather than grep-work.

Required V1 capabilities:

- detect local variables initialized from `repl_state_*_mut()`
- detect simple copies of those locals
- recognize writes through:
  - `alias->field = ...`
  - `(*alias->ptr) = ...`
  - `alias->field[index] = ...`
  - helper forms such as `memcpy(alias->cmds, ...)`

It is acceptable in V1 to stop at function-local, intra-procedural aliasing.

### Phase E: Transitive propagation

Once direct mutation summaries exist, compute:

- `read_only` if no direct mutation and no mutating callees
- `direct_mutation` if direct mutation exists
- `transitive_mutation` if direct mutation does not exist but any reachable
  callee mutates
- `unknown` if function pointers, unresolved externals, or unsupported patterns
  prevent a safe classification

Graph propagation is straightforward compared to alias tracking.

### Phase F: Output integration

Add a second stage that converts the JSON into:

- function-level mutation-aware Mermaid
- file-level mutation-aware Mermaid
- optional review reports listing mutating boundary violations

This part is cheap once the AST output exists.

## Repo-Specific Classification Rules

The tool should not rely purely on generic C semantics. It should load a small
repo-specific config describing known mutation families.

Suggested config categories:

- `mutable_accessor_patterns`
  - `repl_state_*_mut`
- `mutator_function_patterns`
  - `repl_state_*set_*`
  - `repl_state_*clear_*`
  - `repl_state_*reset_*`
  - `repl_command_store_*`
  - `repl_actions_*`
  - `repl_var_drag_*`
- `tracked_globals`
  - any remaining `g_*` state outside the facade that matters for ownership
- `known_read_only_prefixes`
  - e.g. pure layout/model queries where the tool can suppress noise

This is important because pure AST analysis without repo knowledge will produce
too much `unknown`.

## Hard Parts

### 1. Compile database generation

Without `compile_commands.json`, the AST tool is dead on arrival. This is
plumbing work, but it is non-optional.

### 2. Alias tracking through mutable accessors

This is the core implementation cost. Simple direct calls are easy. Proving that
later writes came from a mutable accessor is the part that makes this a real
tool instead of a regex pass.

### 3. Macro and helper noise

Some state writes happen through wrapper helpers or macros. The pass must decide
whether to:

- inline through wrapper calls via transitive summaries, or
- hard-code some helper families as mutating

### 4. Function pointers and callbacks

OpenGL/GLUT code and callback registration may produce unresolved or indirect
call edges. These should likely classify as `unknown` unless the target can be
resolved statically.

### 5. Avoiding false confidence

The dangerous failure mode is not a noisy `unknown`. The dangerous failure mode
is a tool that marks an edge `read_only` when it is actually mutating.

Bias toward:

- `unknown` over wrong
- `transitive_mutation` over pretending a helper chain is pure

## What V1 Does Not Need

V1 does not need:

- interprocedural pointer alias analysis across arbitrary helper functions
- formal purity proofs
- symbolic execution
- custom Clang Static Analyzer checkers
- perfect handling of every macro path
- immediate CI enforcement

That keeps the tool buildable.

## Suggested Implementation Plan

### Step 0: Feasibility spike

Time: **0.5 to 1 day**

Deliverables:

- generate `compile_commands.json`
- compile a tiny standalone LibTooling binary
- dump function names and callsites for 2-3 target files

Success criterion:

- can parse `repl_editor.c`, `ui_panels.c`, and `imrepl_ctrl.c` with the real
  project flags

### Step 1: Direct mutation detector

Time: **1 to 2 days**

Deliverables:

- function summaries for direct calls to mutable accessors / known mutators
- JSON output

Success criterion:

- correctly marks obviously mutating functions in `repl_editor.c`,
  `repl_scenes.c`, and `repl_actions.c`

### Step 2: Intra-procedural alias tracking

Time: **2 to 4 days**

Deliverables:

- detect writes through locals derived from `repl_state_*_mut()`
- detect common store patterns through struct fields and pointer fields

Success criterion:

- catches the bulk of real mutable facade usage in `repl_editor.c` and
  `repl_state.c` callers

### Step 3: Callgraph propagation

Time: **1 day**

Deliverables:

- per-edge and per-function transitive classification

Success criterion:

- can separate `read_only`, `direct_mutation`, and `transitive_mutation`
  sensibly on a sample slice

### Step 4: Mermaid integration

Time: **0.5 to 1 day**

Deliverables:

- extend the existing Mermaid generator pipeline to consume JSON
- add mutation-aware styles and legends

Success criterion:

- generated graphs can highlight mutating versus non-mutating edges

### Step 5: Hardening

Time: **1 to 3 days**

Deliverables:

- config file for tracked mutator families
- regression fixtures
- better diagnostics for unsupported constructs

Success criterion:

- tool output is stable enough to review diffs and architecture changes

## Effort Estimate

### Practical V1

Estimated effort: **medium to large**

Roughly:

- **5 to 9 focused engineering days** for a useful standalone tool

That assumes:

- one engineer already comfortable with Clang AST tooling
- acceptable V1 scope
- conservative classification with `unknown` fallback

### Ambitious V2

If the goal becomes “prove mutation with high confidence across pointer-heavy
helpers”, the effort expands quickly:

- **2 to 4 weeks** is plausible once aliasing, helper wrappers, unresolved
  calls, and output polish are included

## Testing Strategy

Recommended tests:

### Analysis fixtures

Create small dedicated fixture `.c` files that exercise:

- direct mutable accessor calls
- aliasing through one or two local variables
- read-only accessors
- transitive helper chains
- unresolved / indirect calls

These tests should not depend on the whole REPL.

### Repo slice tests

Run the tool against a small set of real files:

- `repl_editor.c`
- `ui_panels.c`
- `repl_actions.c`
- `imrepl_ctrl.c`
- `scene_render.c`

Use these as behavioral snapshots rather than hard semantic truth.

### Output tests

Snapshot the generated JSON and Mermaid fragments for a few focused entry
points so later refactors do not silently change the analysis categories.

## Integration With Existing Repo Tooling

The existing `make analyze` target should stay separate.

Reason:

- `make analyze` is a bug-finding pass using compiler analyzers
- the new tool is an architecture / ownership / mutation-classification pass

Suggested eventual targets:

```make
clang-mutation-db
clang-mutation-analyze
callgraph-files-ast
```

The current `cflow`-based graphs can remain as the fast default. The AST-backed
graphs should be opt-in until they are trustworthy and cheap enough to run.

## Recommended First Deliverable

Do **not** start by trying to replace `cflow` wholesale.

Start with a narrow tool that:

1. parses the project with Clang tooling
2. emits per-function `read_only` / `direct_mutation` / `unknown`
3. handles the `repl_state_*_mut()` family plus obvious mutator verbs
4. proves the compile-database and alias-tracking story works here

If that spike fails or produces too much `unknown`, stop there and keep the
current heuristic pipeline. That is the right failure boundary.

## Bottom Line

This is feasible, but it is a real tooling project, not a small script tweak.

The cost is not in building a call graph. The cost is in:

- compile-database plumbing
- alias tracking for mutable accessors
- repo-specific mutation classification rules
- staying conservative enough not to lie

The likely sweet spot is a standalone LibTooling-based V1 that is honest about
`unknown`, feeds JSON into the existing Mermaid pipeline, and focuses first on
the runtime-state facade plus the obvious REPL-owned mutator families.
