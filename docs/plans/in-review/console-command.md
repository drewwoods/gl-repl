## `console(...)` - printf debugging for the REPL

## Status - NOT STARTED (exploration)

A REPL primitive with `label()`'s exact argument shape that writes a text
line to a panel instead of drawing bitmap text into the scene.

Independent of [`call-frame-provenance.md`](call-frame-provenance.md) - it
needs no new provenance and can ship on its own - but the two compose, and
the composition is most of the value. See "Where the two plans meet".

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
at parse time, arg count checked against placeholder count. Sharing the
grammar means sharing the parser, the export writer's escaping, and the
user's mental model.

Three differences fall out of it not drawing:

- **Legal inside `glBegin`.** Register it with the plain `CMD_TYPE_SPEC_NAMED`
  macro, not the `NOT_IN_BEGIN` variant. It emits no GL, so it cannot break a
  primitive. This is the point of the whole command.
- **Eight substitution args, not four.** `GLUT_BITMAP_MAX_SUB_ARGS` is 4
  because `label()` spends `args[0..2]` on position within an 8-slot
  `args[]`. `console()` spends none, so it gets the full 8. Worth a separate
  `REPL_CONSOLE_MAX_SUB_ARGS = 8` rather than silently reusing the label
  constant.
- **No raster position, no `glRasterPos3f` pairing** in the help text or the
  command description.

Storage is free: `CMD_CONSOLE` reads `payload.label` (the tagged union is
keyed on `type`, and a `char fmt[GLUT_BITMAP_FMT_MAX]` is exactly what this
needs). Add `CMD_CONSOLE -> payload.label` to the union's type list comment
in `command.h`; add no new member.

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
navigation and no new provenance. It is the single highest
value-per-line-of-code item across both plans and should probably be built
first.

### Where the output lives

Not the status-message ring. That is a 16-entry notification history for
one-shot events; the flat program re-executes every frame, so console output
is **regenerated per frame** - sixteen lines a frame, sixty times a second
would destroy it.

Instead: a per-frame buffer, cleared at the start of the main execution pass,
appended during it, rendered after. That makes the console a live view of
*this frame's* execution rather than a scrollback, which is the right model
for a program that re-runs continuously - and it composes with replay for
free, because the executor stops at `replay_exec_limit()`, so the console
shows exactly the prefix that has run.

The precedent to copy is `ReplBackgroundObservation`: the executor already
records what it observed into a caller-supplied out-pointer on
`ReplExecutionOptions`, explicitly NULL for every pass that must not speak
for the frame. A `ReplConsoleSink *console_out` alongside it is the same
pattern, and **the NULL discipline is a correctness requirement, not
tidiness**: hidden-line redraws, depth probes, replay fade overlays, the
`.ply` feedback export and the legacy `repl_execute_commands()` entry point
all walk the same program, and without an explicit NULL each would append a
duplicate copy of every line.

Panel: a peer subsystem (`src/subsystems/console/`) plus a UI renderer, in
the shape `assign_plot` already established - state in the subsystem, a pure
renderer over a snapshot, mouse-only controls, no `GlrConfigKey` unless a
menu entry is actually wanted. Cap lines at a `MAX_CONSOLE_LINES` with an
overflow count rather than dropping silently; a `console()` in a 64-iteration
loop is a normal thing to write.

An open question worth deciding before building: whether an *unopened*
console panel should skip capture entirely (the assign-plot model - the scan
early-outs when closed, so cost is zero when off) or always capture so the
panel is populated the moment it opens. Zero-when-off is more consistent with
the codebase; populate-on-open is friendlier. Leaning zero-when-off.

### Export

`console()` is a REPL primitive, like `label()`, so export owns a spelling
for it. `printf("...\n", ...)` is the natural one - `write_shape_helpers`
already emits per-used-builtin helpers, and this needs no helper at all, just
`<stdio.h>`, which the exported file can already assume.

Trace parity is unaffected: `test_export_trace_parity` compares GL calls
captured through the stubs, and `printf` is not GL. The exported program
prints to stdout where the REPL prints to a panel - a deliberate difference,
and the one place `console()` is not behavior-identical to its export. Say so
in `docs/USER_GUIDE.md`.

### Checklist

Follow skill `gl-repl-new-command`; `label()` is the model to copy
throughout, because it is the existing non-GL REPL primitive and it is
*not* table-driven for parsing.

- `CmdType`: add `CMD_CONSOLE` ([`command.h`](../../../src/repl/command.h)),
  plus the `payload.label` line in the union's type comment and
  `REPL_CONSOLE_MAX_SUB_ARGS`.
- `command_spec.c`: a `CMD_TYPE_SPEC_NAMED` row (plain, **not**
  `NOT_IN_BEGIN`) and a completion/help row next to `label(`. `label` has no
  `k_std_command_specs` row - it is dispatched by name in `parser.c` - so
  `console` needs none either. Keep both tables alphabetical.
- `parser.c`: a second arm at the `strcmp(func, "label")` dispatch.
  Factor `parse_label` into a shared `parse_label_like(..., CmdType,
  const char *name)` so the two cannot drift - the error strings currently
  hardcode `"label: ..."`.
- `executor.c`: the `CMD_LABEL` case's format walk, emitting to the console
  sink instead of `glutBitmapCharacter`. Factor the walk out; it is the
  same code.
- `executor.h`: `ReplConsoleSink *console_out` on `ReplExecutionOptions`,
  with the NULL discipline documented as `observation_out` documents its
  own.
- `attrib_bits.c` and `gl_state_inspector.c`: both switches are
  default-less, so `-Werror=switch` will fail the build until `CMD_CONSOLE`
  is classified. `CMD_LABEL`'s arms are the reference; console touches no
  attribute state at all.
- `command_descriptions.txt`: a `[command CMD_CONSOLE]` block (build-enforced).
- Export: `export_cmd_writer.c` case, and whatever `export_display.c`'s
  `needs_label` equivalent turns out to be for `<stdio.h>`.
- Import round-trip, and a `test_repl_export_all_commands.c` entry.
- `docs/USER_GUIDE.md` (semantics, and the stdout-vs-panel difference) and
  the supported-command list in `CLAUDE.md`.

### Risks

- **`GLUT_BITMAP_FMT_MAX` is 64 characters.** Fine for `label()`; tight for a
  trace line with a prefix and several `%f`. Either raise it for both or give
  console its own. Raising it for both widens `payload.label` and therefore
  `GLCmd` by the delta on every command - measure before choosing.
- **Frequency.** A `console()` inside a hot loop runs at flat-program scale.
  The cap plus the closed-panel early-out are what keep this from being a
  performance footgun, and both need to exist from the first commit.
- **`%f` only.** Everything in the REPL is a float, so this is consistent -
  but `console("type=%f", type)` printing `type=3` (the executor formats with
  `%g`) rather than `type=3.000000` is worth confirming reads well before
  committing to the grammar.

### Where the two plans meet

Once `call_frame_idx` exists, a console line can record the flat index and
frame it was emitted from. That upgrades the panel from a text dump to a
navigable execution trace: click a line, replay seeks to that flat command,
and the PATH annotation explains how it got there. Neither plan needs the
other to ship; this is the reason to keep the console line record one field
wider than it strictly needs to be today.
