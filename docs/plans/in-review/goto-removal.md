# Remove `goto` and Labels From the REPL Language

## Status - IN REVIEW (2026-08-04)

**Blocked on** `block-comment-toggle.md`. That plan delivers the only thing
`goto` is really used for; this one is not startable until it lands.

Recent work that bounds this plan:

- `738f3c1c` - stop a `:label` line from swallowing the line after it;
- `87c26f4c` - make `name:` the only label spelling, deleting the `:name` form
  and the import-accumulator special case it forced.

Those two closed the *silent data loss* around labels. What they did not
change is that `goto` is the one construct in the language resolved at execute
time, inside a pipeline that resolves all other control flow at flatten time -
and that the resulting semantics do not work.

### Design read (2026-08-04)

Reviewed against the tree, by the author of the background-observation work
that touched the same code. **Verdict: sound, proceed.**

Claims checked and confirmed:

- **§1 reproduces.** The exact `USER_GUIDE` example, loaded through the real
  pipeline, flattens to the dump shown there: `if(n < 5)` erased at flatten,
  the jump unconditional, row 2's `n + 1` baked to `args[0] = 1`. The
  documented feature does not work.
- **§4's usage survey holds** - 0 of 39 scenes, 0 tours, and exactly the two
  tutorials (`left:` / `right:` at `tutorials.c:480,482`, `draw:` at `:748`).
- **§3's cursor claim holds** - `repl_exec_cursor_step()` returns 0 in exactly
  two places.
- The `repl_extract_label_name` / `repl_extract_goto_label` caller set
  (executor, reformat, tutorial_runner, replay_annotations) is fully covered by
  §2.

Three corrections, folded into the sections below: the removal decouples the
executor further than §3 claims (see "[3. The cursor contract gets
simpler](#3-the-cursor-contract-gets-simpler)"); `import.c` and
`replay_playback.c` are missing from the §2 site table, and `-Wswitch` cannot
find the first of them; and export's handling of a `goto` row is unknown and
needs answering before step 2.

### Response to the design read (2026-08-04)

Both §3a counts re-verified against the tree: `cursor->text` has exactly one
reader (the `flat_goto_target()` call in the `CMD_GOTO` arm) and
`options.status_out` exactly one writer (the loop-limit message). All four
`import.c` line references are exact. §3a is accepted in full and is now the
plan's step 3.

The open export question is **answered by measurement** (§2), which unblocks
step 2 - and the measurement turned up a divergence between the live REPL and
exported C that is now §5, the strongest argument in the plan.

One review call is **reversed**: `is_stmt_terminator()`'s `:` case must stay.
See §2 - after the removal its job changes from labels to error containment,
and dropping it would let a legacy `loop:` swallow the row beneath it on
import.

**No backward compatibility is owed, and the removal is total.** `goto` can
disappear leaving the codebase with no memory of it: no compatibility shim, no
retained-but-unused `CmdType`, no deprecation period, no migration for user
files on disk that contain `goto` / `name:`, and - importantly - **no
goto-specific rejection path**. A file or a typed line carrying `goto foo;`
should fall through to the *generic* unknown-statement diagnostic that any
other unrecognized text gets. Adding a special case that recognizes `goto` in
order to refuse it would leave exactly the goto-shaped hole this removal is
meant to close.

The one rule that survives is a data-loss rule, not a compatibility one, and it
is already an acceptance criterion: a rejected line must be *visible* rather
than silently dropped.

## Why remove it

### 1. It does not do what its own documentation says

`docs/USER_GUIDE.md:1074` documents a counting loop. Loaded through the real
pipeline it flattens to:

```
0 | CMD_VAR_ASSIGN   n = 0;
1 | CMD_GOTO_LABEL   loop:
2 | CMD_VAR_ASSIGN   n = n + 1;      args=[1]     <- baked once, at flatten
3 | CMD_GOTO         goto loop;                   <- unconditional
4 | CMD_GLUT_CUBE    glutSolidCube(n/5);          <- never reached
```

`if(n < 5)` is gone: `if` is resolved at flatten time, so the guard that was
supposed to stop the loop is constant-folded away before the executor starts.
The jump is therefore unconditional, it spins until `REPL_GOTO_LOOP_LIMIT`
(100000) trips, the status line reports "goto: loop limit reached", and the
`glutSolidCube` row never executes. Re-running row 2 re-applies the *baked*
`args[0] = 1` rather than recomputing `n + 1`, so the counter never counts
either.

This is not a bug to fix in isolation. `goto` is executed in a stream from
which its own exit condition has been erased, over args that were specialized
for a single pass. The executor's own comment
(`src/repl/executor.c`, the `CMD_GOTO` arm) has said so since it
landed: goto loops are "only reliable for control flow and assignments", and
"replay cannot follow the dynamic jump trace".

### 2. Making it work means rewriting flatten

`flatten_range()` is a recursive descent over **nested source ranges**. Every
construct it resolves is range-nested and resumes just past its own extent:

| construct | flatten does |
|---|---|
| `for(...)` | `flatten_range(i+1, loop_end)` once per iteration |
| `if(...)` | recurse into the taken arm's range |
| `funcN(...)` | recurse into the body's range |

All three find their extent through
`flatten_repl_source_scope_find_block_end()`. `goto` is the one construct with
no extent and no resume point - an arbitrary target, possibly backwards,
possibly across block boundaries. Unrolling it would require `flatten_range`
to stop being a structural walker and become a program counter over the whole
document. A for-loop's trip count is also computable from its header *before*
unrolling; a goto loop's is only discoverable by running it, against a
`MAX_FLAT_COMMANDS` ceiling of 8192.

That work is not obviously worth doing for a construct nothing uses.

### 3. The real use case is not looping

Labels and `goto` are reached for in testing as a way to **skip a region of
code** - a stand-in for `#if 0` / a block comment. The language already has
the first (`if(0) { … }` emits nothing; verified against `--dump-flat`,
including inside `glBegin`/`glEnd`), but using it means *restructuring* the
code you wanted to leave alone: writing the `if(0) {` / `}` pair and moving
rows inside it.

The honest fix for that use case is a working block comment/uncomment, which
`block-comment-toggle.md` specifies. Once that exists, `goto` has no remaining
job.

### 4. Nothing ships that uses it as a jump

No built-in example, scene, or tour contains a `goto`. The only shipped users
of `CMD_GOTO_LABEL` are two tutorials, and they use labels as **positional
anchors, never as jump targets** - see "Tutorial anchors" below, which is the
one real dependency this plan has to replace.

### 5. Live REPL and exported C disagree

Established while answering the export question, and it is the strongest
argument here because it breaks a project invariant rather than a feature.

Export writes from **document rows**, not from the flat program. So the `if`
guard that flatten constant-folds away is still in the exported C. Feeding the
`USER_GUIDE` example through `repl_export_save_output()` emits:

```c
  n = 0;
loop:
  n = n + 1;
  if(n < 5) {
    goto loop;
  }
  glutSolidCube(0.5);
```

That is a *working* counting loop. Real C control flow, real per-iteration
re-evaluation: it loops five times and draws the cube at the intended size.
The live REPL running the same document does not - the guard is gone, the jump
is unconditional, and the geometry is never reached (§1).

**The same source produces two different programs.** Every other construct in
the language is resolved once, at flatten time, and the executor and the
exporter agree by construction - which is what `test_export_trace_parity`
exists to pin. `goto` is the sole construct whose meaning lives in its source
line rather than its baked args, and it is correspondingly the sole construct
where the live session and the file you save disagree about what the program
does.

No amount of fixing the REPL side closes this without the flatten rewrite in
§2, because the divergence *is* the flatten/execute split. Removing `goto`
removes the only case where the invariant does not hold.

## Goals

- Remove `CMD_GOTO` and `CMD_GOTO_LABEL` from the language: parser, flatten,
  executor, replay, inspector, spec tables, and docs.
- Replace the tutorial splice-anchor mechanism that currently rides on
  `CMD_GOTO_LABEL` with one that does not depend on a language construct.
- Delete the machinery that exists only because a PC can move backwards:
  `REPL_GOTO_LOOP_LIMIT`, the executor's `goto_count`, and the duplicate
  budget in the replay-annotation walker.
- Simplify `repl_exec_cursor_step()`'s contract: with the goto budget gone it
  can no longer stop mid-program, so "steps until done" becomes true.
- **Cut the executor's last two ties to anything outside the flat program.**
  `goto` is the only reader of the execution source-text view and the only
  writer of the execution status channel, so both go with it and
  `repl_execute_program()` ends up taking a flat program and nothing else. See
  §3 - this is the largest single effect of the removal and the reason it is
  worth more than "delete two `CmdType`s".
- Leave `if(0) { … }` documented as the supported way to disable a block, and
  point the removed USER_GUIDE section at it and at the block-comment toggle.

## Non-goals

- Reworking `flatten_range()` into a program-counter interpreter. That is the
  alternative to this plan, not part of it.
- Changing `for` / `if` / `funcN` semantics in any way.
- Adding a replacement jump construct under another name.
- Source or binary compatibility for the removed `CmdType` values. There is no
  released version to migrate.

## Prerequisite

**`block-comment-toggle.md` must land first.** It delivers comment *and*
uncomment over a line range, which is the capability `goto`-skipping is
standing in for. Removing `goto` before that would take away a workflow
without providing its replacement, even though the workflow barely works.

## Design

### 1. Tutorial anchors move off the language

This is the only load-bearing consumer, and it is not a jump.

`TUTORIAL_STEP_LABEL` steps splice content at a named position. The name
resolves first against earlier *step* labels, and failing that against a
`name:` goto label in the tutorial's **setup scaffold** - resolved at
step-entry time by scanning the live document for `CMD_GOTO_LABEL` rows
(`tutorial_runner.c:469`). Two shipped tutorials depend on it:

| Tutorial | Setup labels used as anchors |
|---|---|
| Color Interpolation | `left:`, `right:` |
| Fog | `draw:` |

Replace the label row with a comment directive, `// @anchor <name>`:

- It is a `CMD_COMMENT` row, so flatten already drops it and no `CmdType`,
  parser grammar, or spec-table entry is needed.
- It matches the existing `@`-directive convention (`// @cfg`, `// @declare`,
  `// @tune`, `// @config`).
- It survives the block-comment toggle as ordinary text.

Changes:

- `setup_line_goto_label()` (`tutorials.c:1551`) becomes
  `setup_line_anchor_name()`, matching `// @anchor <name>` instead of `name:`.
  Its callers - `setup_defines_goto_label()` and the validator - follow.
- The runtime scan in `tutorial_runner.c:469` matches the directive text on
  `CMD_COMMENT` rows instead of reading `CMD_GOTO_LABEL`.
- The two shipped scaffolds change `left:` / `right:` / `draw:` to
  `// @anchor left` etc.
- The uniqueness/collision rules in `repl_tutorial_validate` keep their shape;
  only what counts as "an anchor defined in setup" changes.

`test_tutorial_runner` already covers anchor resolution and rejects
forward/missing references, so this is a mechanical retarget with existing
coverage. `test_validate_setup_label_rules` (`test_tutorial_runner.c:3361`) is
the direct fixture.

### 2. Language removal

Remove both types from `CmdType` (`src/repl/command.h`) and every site
the `-Wswitch` build then flags:

| Site | Change |
|---|---|
| `parser.c` `parse_keyword_statement` | drop the `goto name` arm and the `name:` label arm |
| `flatten.c:1456` | drop the "goto and labels are not supported inside functions" rejection |
| `executor.c` | drop the `CMD_GOTO` / `CMD_GOTO_LABEL` cases, `flat_goto_target()`, `cursor->goto_count` |
| `replay_annotations.c:663` | drop the `CMD_GOTO` case and its `goto_count` budget |
| `gl_state_inspector.c:1422` | drop from the no-op enumeration |
| `command_spec.c:803` | drop the `CMD_GOTO` spec row; retire `CMD_CAT_LABEL` if it has no other member |
| `reformat.c:377` | drop the `CMD_GOTO` indent case |
| `hidden_lines.c:227` | drop from `hidden_lines_cursor_owns_cmd` |
| `replay_playback.c:98` | drop `CMD_GOTO_LABEL` from the "is this a visible replay step" predicate |
| `text_helpers.c` | delete `repl_extract_label_name` / `repl_extract_goto_label` |
| `control_flow.h:10` | delete `REPL_GOTO_LOOP_LIMIT`; `REPL_GOTO_LABEL_MAX` goes with the last user |

**`-Wswitch` does not find every site.** It enumerates the `CmdType` switches,
which is most of the table above, but the label handling in `import.c` is
*string* matching and stays silent:

| Site | Change |
|---|---|
| `import.c:1167` `import_make_repl_label()` | delete; it exists only to claim a `name:` line before the C-expression converter rewrites it. Drop it from the converter chain at `:1767` too |
| `import.c:2575` `is_stmt_terminator()` | **keep the `:` case** - see below |

**Correction to the design read: do not drop `:` from `is_stmt_terminator()`.**
The read is right that the case exists because of labels, but after the removal
its remaining job is error containment, and that job is load-bearing. No valid
statement ends in `:` once labels are gone, so the case can only ever fire on
invalid input - and there its effect is to keep a stray `loop:` line
self-contained. Drop it and the accumulator treats `loop:` as an unfinished
statement, glues the next physical line onto it, and reports one joined parse
error - which loses the following row from the document. That is precisely the
bug class `738f3c1c` fixed, re-entering through the back door on legacy files.
Keeping one character in a predicate is the cost of the plan's own acceptance
criterion that no row is silently dropped on import. Re-comment it to say it
now exists for stray-colon containment rather than for labels.

**Export: answered.** Measured, not read - a document with `loop:` /
`goto loop;` exported through `repl_export_save_output()` emits both rows
**verbatim**, via the `default:` arm at `export_cmd_writer.c:554` →
`write_cmd_source_as_c()`. Nothing is dropped. The read's first hypothesis
(silent drop) is false; it is the second, and the verbatim path needs no audit
because `loop:` and `goto loop;` are already valid C.

That answer is clean, but the measurement turned up something that is not -
see "[5. Live REPL and exported C disagree](#5-live-repl-and-exported-c-disagree)".

Check `CMD_CAT_LABEL` before deleting it - `command_spec.h:164` is a
`CmdSyntaxCategory` member, and the drift test in
`tests/test_replay_walk.c` asserts the control-flow
and visual taxonomies agree.

### 3. The cursor contract gets simpler

`repl_exec_cursor_step()` returns 0 in exactly two places today: at
`repl_exec_cursor_done()`, and when the goto budget trips. Removing the second
means a step can no longer stop a walk mid-program. Callers
(`repl_execute_program`, `hidden_lines_execute`) keep their loop shape, but the
"a step may abort the frame" case disappears from the contract - state that
explicitly in the header comment rather than leaving it implied.

### 3a. The executor stops depending on the document

An earlier draft asked whether `execution_flat_text()` still has callers once
the goto arm is gone. Checked: it does not, and the chain does not stop there.
Two counts settle it -

- `cursor->text` has **exactly one reader**: the `flat_goto_target()` call in
  the `CMD_GOTO` arm.
- `options.status_out` has **exactly one writer in the whole executor**: the
  goto loop-limit message.

So the removal cascades through both:

| Dies | Because |
|---|---|
| `execution_flat_text()`, `flat_goto_target()` | only the goto arm calls them |
| `ReplExecCursor.text`, `ReplExecutionOptions.text` | no reader left |
| `.text = source_document_view()` at 5 production call sites, `HiddenLinesRenderContext.text` | nothing to pass |
| `executor.h`'s `#include "source_document.h"` | no `SourceTextView` in the header |
| `ReplExecutionOptions.status_out` / `.status_out_sz`, and the same pair on `HiddenLinesRenderContext` | no writer left |
| `glr_ctrl.c`'s `exec_status[REPL_DIAG_TEXT_MAX]` buffer and its `repl_set_status_error()` relay | nothing writes it |

That is the largest single effect of this plan: `repl_execute_program()` ends up
taking a **flat program and nothing else** - no source text, no diagnostic
channel. It completes the rule the executor header already states, that the
executor evaluates no expression text and renders only from baked `args[]`.
`goto` was the sole exception, because it is the only command whose meaning
lives in its *source line* rather than its args.

Sequence this as its own step rather than as a tail of the language removal:
the blast radius reaches app and subsystem call sites, not just `src/repl/`,
and it is easier to review as "the executor loses two inputs" than as fallout.

### 4. Ordering against the background plan - resolved

`clear-background-execution-observation.md` **landed in full** (`74738f77` ..
`6da10176`), so this constraint is satisfied: start from current `main`.

Two things it left behind that make this removal smaller:

- Phase 0 (`74738f77`) factored goto target lookup into one
  `flat_goto_target()`. There is now a single lookup to delete rather than two
  copies - the executor's inlined scan and the resolver's own - which had
  silently diverged: the executor searched the replay-clamped `flat_cmd_count`
  while the resolver searched the full `program.cmd_count`, so during replay
  they could take different jumps.
- Phase 3 deleted `repl_flat_resolve_clear_color()`, the second walker that
  followed gotos. Nothing outside `ReplExecCursor` interprets a jump any more.

## Implementation sequence

1. **Tutorial anchors** (standalone, lands first). Introduce `// @anchor`,
   retarget the validator and the runtime scan, convert the two shipped
   scaffolds, update `test_tutorial_runner`. At this point nothing shipped uses
   `CMD_GOTO_LABEL`.
2. **Language removal.** Answer the export question in §2 first. Then delete
   both `CmdType` values and let `-Wswitch` drive the switch sites, adding the
   two string-matching sites in `import.c` by hand. Delete the tests that
   exercise goto semantics; convert the ones that merely *use* a label as a
   document row to something else.
3. **Executor decoupling** (§3a, its own step). Drop the now-unread source-text
   view and the now-unwritten status channel from `ReplExecutionOptions`,
   `ReplExecCursor` and `HiddenLinesRenderContext`, and remove the matching
   plumbing at the app/subsystem call sites. Separate from step 2 because it
   reaches outside `src/repl/`.
4. **Docs.** Remove the USER_GUIDE "Labels & goto" section and replace it with
   the disable-a-block idiom; update `CLAUDE.md:642`, `src/repl/ARCHITECTURE.md`
   (the construct table and the `REPL_GOTO_LOOP_LIMIT` paragraph), and
   `docs/ARCHITECTURE.md`.

## Test plan

Tests that exercise goto *semantics* are deleted with the feature:

- `test_repl_core_commit.c` - label commit + round-trip (`walk:`, `stripe:`,
  `after:`);
- `test_repl_editor.c:1618` - `editor_load_line_to_input` label path;
- `test_repl_executor.c` - `test_goto_uses_caller_text_view` and the two goto
  cases in `test_background_observation` (forward jump skips a clear; backward
  jump terminates under the budget). `test_resolve_frame_clear_color` is
  already gone - Phase 3 of the background plan deleted it with the resolver;
- `test_repl_core_parse.c:195` - `repl_extract_label_name`;
- `test_repl_core_io.c` cases 11-13 - the label spelling / swallowing
  regressions added by `738f3c1c` and `87c26f4c`.

Tests that must be **retargeted, not deleted**, because they are about
something else and only happen to use a label:

- `test_tutorial_runner.c:3361` `test_validate_setup_label_rules` - the anchor
  rules survive, spelled `// @anchor`.

New coverage:

- `goto foo;` and `foo:` are rejected as unknown commands with a diagnostic,
  not silently accepted.
- A tutorial whose `target_label` names a `// @anchor` in setup resolves it;
  a missing or forward-referenced anchor still fails validation.
- `if(0) { … }` emits no flat commands - the documented replacement, pinned so
  the idiom cannot regress once it is the only one.

Full verification: `make test-stubs`, `make gl-repl USE_GL_STUBS=1`,
`make gl-repl`, `make check-c99`, `make check-state-ownership`, `make test-web`.

## Documentation updates

- `docs/USER_GUIDE.md` - delete "Labels & goto (experimental, top-level only)";
  add the disable-a-block idiom (`if(0) { }` plus the Ctrl+/ range toggle) in
  its place, near `if`.
- `CLAUDE.md:642` - drop `name: / goto name` from the supported-command block.
- `src/repl/ARCHITECTURE.md` - drop the `name:, goto name` construct row, and
  the paragraph stating goto is the one construct resolved at execute time.
  That claim becomes false in the best way: after this plan, **every**
  construct is resolved at flatten time.
- `docs/ARCHITECTURE.md:2720` - the tutorial-scaffold anchor sentence.
- `src/repl/tutorials.h` - the `TUTORIAL_STEP_LABEL` contract, which currently
  names goto labels in three places.

## Acceptance criteria

- `goto` and `name:` are not accepted by the parser, and produce a diagnostic
  rather than a silent no-op.
- `CMD_GOTO` / `CMD_GOTO_LABEL` are absent from `CmdType`; no switch, spec
  table, or taxonomy references them.
- No jump budget remains anywhere: `REPL_GOTO_LOOP_LIMIT` is gone from the
  executor and from the replay-annotation walker.
- `repl_exec_cursor_step()` cannot stop a walk mid-program, and its header says
  so.
- `repl_execute_program()` takes a flat program and nothing else: no
  `SourceTextView`, no status-out channel, and `executor.h` no longer includes
  `source_document.h`.
- Nothing in the tree recognizes `goto` or `name:` in order to reject it. A
  file or typed line carrying one gets the same generic unknown-statement
  diagnostic as any other unrecognized text - and gets it *visibly*, with no
  row silently dropped on import or export.
- The two shipped tutorials splice at the same rows as before, via
  `// @anchor`, with validation still rejecting missing and forward references.
- Every language construct is resolved at flatten time; the flat program
  contains no execute-time control flow.
- Focused tests, stubbed full tests, C99/ownership guards, native build, and
  the web lane pass.

## Open questions for the design read

1. **`// @anchor` vs. reusing step labels.** The alternative is to promote the
   two scaffolds' anchor rows into real tutorial steps and let
   `TUTORIAL_STEP_LABEL` resolve against step labels only, deleting the
   setup-scaffold anchor path entirely. That is a smaller runtime but changes
   the shape of two lessons. `// @anchor` is proposed because it preserves the
   lessons exactly.

   *Design read:* take `// @anchor`. One comment-directive matcher against
   reshaping two working lessons is the cheaper side, and it keeps the change
   mechanical enough that the existing `test_tutorial_runner` coverage carries
   over.
2. **Does anything else want a bookmark?** If a document-level anchor is
   useful beyond tutorials (jump-to, folding, the code panel), `// @anchor`
   should be specified as a general feature rather than a tutorial-private one.
   If not, keep it tutorial-private and undocumented for users.

   *Design read:* keep it tutorial-private until a second consumer actually
   exists. Same rule the background plan applied when it deleted
   `ReplRenderState.clear_color`: a facility with no consumer is not justified
   by calling it general-purpose.
3. **Export behavior for a `goto` row** (raised by the design read, §2). Not a
   preference question - a fact to establish before step 2.

   *Answered (2026-08-04), by measurement:* both rows export **verbatim**
   through the `default:` arm, and nothing is dropped. Step 2 is unblocked.

   The probe also settled a question nobody had asked, and it is the one thing
   in this plan that changed a conclusion rather than confirming one: because
   export writes document rows while the executor runs the flat program, the
   exported C keeps the `if` guard the REPL folds away, and therefore *works*
   where the live session does not. `goto` is the only construct in the
   language where the live REPL and the file you save disagree about what the
   program does. See §5 - it is now the strongest single argument for removal,
   and it is an invariant break rather than a missing feature.
4. **`is_stmt_terminator()`'s `:` case** (raised here, against the design
   read). The read lists it as unclaimed simplification; §2 argues it must
   stay, because after the removal its job is keeping a stray `loop:` from
   swallowing the row beneath it on a legacy file. Cheap to keep, and the
   plan's own "no row silently dropped" criterion depends on it. Flagged for
   the next reader rather than settled unilaterally, since it reverses a
   review call.
