## `console(...)` - printf debugging for the REPL

## Status - NOT STARTED (follow-up work, revised after design review)

A REPL primitive with `label()`'s exact argument shape that writes a text
line to a panel instead of drawing bitmap text into the scene.

**Sequenced after [`call-frame-provenance.md`](call-frame-provenance.md).**
It needs nothing from that plan and that plan needs nothing from this one, so
the ordering is about review load, not dependency. The two compose later; see
"Where the two plans meet".

Revision note, two review rounds. Round 1 rejected capturing console output
through an executor sink on `ReplExecutionOptions` (duplicate output under
accumulation passes) and exporting to a bare `printf` (silently changes
rendered numbers, and does not round-trip through import). Round 2 added a
second export divergence the plan had missed - indentation - and struck a
composition claim that was simply false: clicking a console line cannot make
the PATH row appear, because a `console()` step emits no draw. All corrected
below.

### Why, given `label()` exists

`label("d=%f type=%f", depth, type)` inside `divide_triangle` already works
today and already reads function-scoped locals. Three things stop it being a
debugging tool:

1. **It draws in the scene.** Sixteen recursive invocations put sixteen
   strings at whatever raster position happens to be current. In the
   Sierpinski scene they land on top of each other.
2. **It cannot go where the interesting code is.** `CMD_LABEL` is registered
   `CMD_TYPE_SPEC_NAMED_NOT_IN_BEGIN`, so it is rejected inside a
   `glBegin`/`glEnd` block. The vertices this scene is built from are all
   inside `glBegin(GL_POLYGON)`. You cannot put a `label()` next to the line
   you want to inspect.
3. **It needs a raster position.** `label()` draws at the current
   `glRasterPos3f`, so instrumenting a function means also thinking about
   where its text will land in 3D.

`console()` fixes all three by not being a drawing command.

### Shape

```
console("fmt", a, b, c, ...);
```

Same format grammar as `label()`, deliberately: `%f` and `%%` only, validated
at parse time, arg count checked against placeholder count, rendered with
`%g`. Sharing the grammar means sharing the parser, the export helper's
escaping, and the user's mental model.

The differences that fall out of it not drawing:

- **Legal inside `glBegin`.** Register it with the plain `CMD_TYPE_SPEC_NAMED`
  macro, not the `NOT_IN_BEGIN` variant. It emits no GL, so it cannot break a
  primitive. This is the point of the whole command.
- **No raster position**, and no `glRasterPos3f` pairing in the help text or
  the command description.

Storage is free: `CMD_CONSOLE` reads `payload.label` (the tagged union is
keyed on `type`, and a `char fmt[GLUT_BITMAP_FMT_MAX]` is exactly what this
needs). Add `CMD_CONSOLE -> payload.label` to the union's type list comment
in `command.h`; add no new member.

**On argument count.** An earlier draft claimed console could take 8
substitution args where `label()` takes 4, because label "spends `args[0..2]`
on position". That is wrong, and so is the comment it came from:
`GLUT_BITMAP_MAX_SUB_ARGS`'s docs in `command.h` say *"Position takes
args[0..2], substitutions live in args[3..6]"*, but `parse_label` writes
`cmd->args[i] = subs[i]` starting at index 0 and `glRasterPos3f` is a
separate command with its own `GLCmd`. **The comment is stale and should be
fixed as a standalone one-line correction**, independent of this plan. The
real situation is that the 4-arg cap is a free choice with 8 slots available,
for label and console alike. Pick a cap for console on its merits (8 is
reasonable for a trace line); do not justify it with the position story.

### Auto-indent by call depth - the feature that makes it worth building

`call_depth` is **already** on every flat command. Indenting each console
line by its depth costs one field read and turns a flat dump into a readable
trace. Sierpinski, with one `console()` at the top of `divide_triangle` and
one in `triangle`:

```
divide_triangle depth=2 type=0
  divide_triangle depth=1 type=3
    divide_triangle depth=0 type=3
      triangle type=3
    divide_triangle depth=0 type=5
      triangle type=5
    ...
  divide_triangle depth=1 type=1
    ...
```

That is the recursion structure, for free, with no frame table, no UI
navigation and no new provenance. It is the highest value-per-line item in
this plan.

### How the output is captured - one flat scan, not an executor sink

The obvious design is an executor hook: a `ReplConsoleSink *console_out` on
`ReplExecutionOptions` beside `observation_out`, appended as `CMD_CONSOLE`
executes. **That is wrong**, and the reason generalizes (design-review
finding 3):

- The executor runs the scene **once per accumulation sample**, not once per
  frame (`render3d_draw_scene`'s jitter loop). With accum passes at 8, a
  `console()` line is formatted eight times.
- Under Blur, the controller **re-bakes the program between samples** at a
  sub-step `t`, so the samples do not even agree on the values. Which sample
  clears the sink and which publishes it becomes observable.
- Other passes walk the same program too - hidden-line redraws, depth probes,
  replay fade overlays, the `.ply` feedback export - each needing an explicit
  NULL to stay quiet. That discipline is real work and easy to get wrong on
  the next pass someone adds.

**Instead, scan the flat program once per frame.** The arguments are already
baked into the flat commands by flatten, so no execution is needed to know
what a `console()` line says. Build the snapshot by walking
`[0, g_frame_replay_exec_limit)` at the point in `glr_ctrl_display_frame`
where that limit is known - immediately alongside `assign_plot_exec_progress`,
which does exactly this for the same reason and is the precedent to copy.

This is strictly better on every axis: once per frame regardless of accum
passes, no dependence on which sample won, no `ReplExecutionOptions` change,
no NULL discipline, and replay truncation comes free because the exec limit
*is* the replay clamp. It also inherits assign-plot's cost model - gate the
scan on the panel being open and it costs nothing when closed. Give it its
own `ProfSection`; per-frame work inside someone else's span shows up only as
unattributed remainder.

One consequence to accept deliberately: output is a **per-frame snapshot**,
not a scrollback. For a program that re-runs continuously that is the right
model - the console shows what *this* frame did - and it is what makes the
replay behaviour fall out for free. A retained history is a separate feature
with a separate justification.

Panel: a peer subsystem (`src/subsystems/console/`) plus a UI renderer, in
the shape `assign_plot` already established - state in the subsystem, a pure
renderer over a snapshot, mouse-only controls, no `GlrConfigKey` unless a
menu entry is actually wanted. Cap lines at `MAX_CONSOLE_LINES` with a
visible overflow count rather than dropping silently; a `console()` in a
64-iteration loop is a normal thing to write.

### Export

Not a bare `printf`. Two independent reasons (design-review finding 4):

1. **It changes the numbers.** The REPL renders `%f` placeholders through
   `%g` (`snprintf(..., "%g", ...)` in the `CMD_LABEL` executor arm), so
   `console("type=%f", 3)` shows `type=3`. C's `printf("%f")` would print
   `type=3.000000`. This is not a hypothetical - `write_label_helper` in
   `export_prologue.c` exists precisely to avoid it, and says so: *"Using
   vsnprintf with the raw format would print `1.000000` for `%f` while the
   REPL prints `1` - that divergence breaks visual round-trips."*
2. **It does not round-trip.** `import_make_repl_glut_bitmap_string` matches
   the literal `label` prefix to recognize the exported form. An arbitrary
   `printf(...)` line is not safely reversible into a `CMD_CONSOLE`.

So export a `console` helper, mirroring `write_label_helper` exactly: same
`%f` -> `%g` walk, same `%%` handling, emitted on demand, with the call site
written as `console("fmt", ...)` so import can match a distinctive prefix the
same way it matches `label`. Factor the two helper emitters against each
other if they turn out identical apart from the sink.

Trace parity is unaffected: `test_export_trace_parity` compares GL calls
captured through the stubs, and neither the helper nor its output is GL.

**Two deliberate divergences from the export, not one** - both belong in
`docs/USER_GUIDE.md`:

1. **Sink.** The exported program prints to stdout; the REPL prints to a
   panel.
2. **Indentation.** The panel auto-indents by the flat command's baked
   `call_depth` (see above). The exported helper has no such thing - by then
   the calls are real C recursion and the depth exists only on the machine
   stack - so stdout is unindented. Parity would mean instrumenting entry and
   exit of every generated function to carry a depth counter, which is far
   more than this feature is worth. Document the difference; do not chase it.

### Checklist

Follow skill `gl-repl-new-command`; `label()` is the model to copy
throughout, because it is the existing non-GL REPL primitive and it is
*not* table-driven for parsing.

- `CmdType`: add `CMD_CONSOLE` ([`command.h`](../../../src/repl/command.h)),
  plus the `payload.label` line in the union's type-list comment and
  `REPL_CONSOLE_MAX_SUB_ARGS`.
- `command_spec.c`: a `CMD_TYPE_SPEC_NAMED` row (plain, **not**
  `NOT_IN_BEGIN`) and a completion/help row next to `label(`. `label` has no
  `k_std_command_specs` row - it is dispatched by name in `parser.c` - so
  `console` needs none either. Keep both tables alphabetical.
- `parser.c`: a second arm at the `strcmp(func, "label")` dispatch.
  Factor `parse_label` into a shared `parse_label_like(..., CmdType,
  const char *name)` so the two cannot drift - the error strings currently
  hardcode `"label: ..."`.
- `executor.c`: `CMD_CONSOLE` is a **no-op** in the executor (capture is the
  flat scan above). Factor the `CMD_LABEL` format walk out so the scan and
  the label executor share one implementation.
- `flatten.c`: the payload-refill site that "refills `payload.label` for
  `CMD_LABEL`" must cover `CMD_CONSOLE` too, and its comment must say so.
- **Payload-sensitive tests** (finding 7): `test_repl_flatten_rebake.c` and
  `test_repl_flatten_differential.c` both inspect `payload.label` gated on
  `CMD_LABEL` specifically. A `CMD_CONSOLE` reusing that payload is invisible
  to both until they are widened - a silently untested rebake path.
- `glr_debug.c`: `debug_dump_flat_args` prints `fmt=` for `CMD_LABEL`; add
  `CMD_CONSOLE` so `--dump-flat` does not lose the format string.
- `attrib_bits.c` and `gl_state_inspector.c`: both switches are
  default-less, so `-Werror=switch` fails the build until `CMD_CONSOLE` is
  classified. `CMD_LABEL`'s arms are the reference; console touches no
  attribute state at all.
- `command_descriptions.txt`: a `[command CMD_CONSOLE]` block (build-enforced).
- Export: `export_cmd_writer.c` case, the `console` helper in
  `export_prologue.c`, and the `needs_label` equivalent in
  `export_display.c` that gates emitting it.
- Import: a `console` prefix matcher beside
  `import_make_repl_glut_bitmap_string`.
- Round-trip: an entry in `test_repl_export_all_commands.c`.
- `docs/USER_GUIDE.md` (semantics, and the stdout-vs-panel difference) and
  the supported-command list in `CLAUDE.md`.

### Risks

- **`GLUT_BITMAP_FMT_MAX` is 64 characters.** Fine for `label()`; tight for a
  trace line with a prefix and several `%f`. Either raise it for both or give
  console its own. Raising it for both widens `payload.label` and therefore
  `GLCmd` by the delta on every command, across the document, both undo
  rings, the scene snapshots and the flat program - measure before choosing.
- **Frequency.** A `console()` inside a hot loop runs at flat-program scale.
  The line cap plus the closed-panel early-out are what keep this from being
  a performance footgun, and both need to exist from the first commit.
- **`%f` only.** Everything in the REPL is a float, so this is consistent -
  but confirm `console("type=%f", type)` printing `type=3` reads well before
  committing to the grammar.

### Where the two plans meet

Once frame provenance exists, a console line can record the flat index and
frame it was emitted from - and the flat scan already has both in hand, since
it is walking the flat program. That is the reason to keep the console line
record one field wider than it strictly needs to be today.

What that unlocks needs stating carefully, because the obvious version does
not work. "Click a console line, replay seeks there, and the PATH row
explains it" is **false as written**: `CMD_CONSOLE` emits no draw, so
`replay_focus_anchor_flat_idx()` - which resolves the step's
`repl_cmd_consumes_current_color()` command - returns -1 for that step and
PATH is suppressed by design. Two honest options, both later work:

- seek to an associated *draw* within the same frame (the console record's
  frame gives `flat_begin`/`flat_end`, so "the first draw in this
  invocation" is a range scan), which makes PATH fire naturally; or
- render the breadcrumb directly from the console record's own frame index,
  bypassing the anchor accessor entirely - the formatter takes a frame, not
  a draw, so this is mostly plumbing.

Either is a small extension. Neither should be promised by this plan.
