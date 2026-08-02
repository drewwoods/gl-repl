# Project skills

Task-scoped instructions for Claude Code, split out of `CLAUDE.md` so they cost
nothing until they're relevant.

## Are they auto-discovered?

**Yes - with one caveat worth understanding.**

Every `SKILL.md` under `.claude/skills/*/` is discovered automatically at
session start. No registration, no settings entry. What gets loaded into the
model's context up front is only each skill's `name` and `description` - a line
or two apiece. The body is read on demand, when the agent decides the skill
applies, or when you invoke it by name.

The caveat: *discovery is automatic, invocation is a judgment call.* The agent
matches your request against those one-line descriptions. If the description
doesn't contain the words you'd naturally use, the skill silently doesn't fire
and you get default behavior. That's why the descriptions here are stuffed with
concrete trigger phrases and file names rather than tidy summaries - a
description is a matching surface, not documentation.

You can always force it: `/gl-repl-capture`, or just "use the capture skill".

## The skills

| Skill | Fires on | Covers |
|---|---|---|
| `gl-repl-new-command` | "add `glFoo`", "support `glSomething`", editing `CmdType` or the spec tables | The five required edits (CmdType → parser → executor → `flatten_range()` → spec tables), enum slot kinds, arg-splitting rules, export round-trip, commit-path differences |
| `gl-repl-scene-authoring` | "write a scene", "add an example", editing `examples.c` | Full language + math reference, per-command parser policies, `@cfg`/camera headers, presentation reset, the five files that move together, size budgets |
| `gl-repl-config-toggle` | "add a setting/toggle/config key", editing `glr_actions.c` | `g_cfg_items[]` row + section, `CFG_DEFAULT_*`, keymap binding, and the 32-golden regeneration a new `GlrConfigKey` forces |
| `gl-repl-capture` | "screenshot", "record a GIF", "regenerate docs images", "run it headless" | CLI flags, `GLR_*` env hooks, OSMesa build, freeglut frame capture, record scripts, shot staging, startup diagnostics |

## Skill vs CLAUDE.md - where does a new fact go?

The test is **where the trigger lives**:

- **The trigger is in the user's request** → skill. "Add a new command", "cut a
  release", "record a GIF". You said the thing that names the task, so the
  agent can match it.
- **The trigger is only in the code about to be touched** → `CLAUDE.md`.
  Nothing in "fix this panel layout bug" warns you that
  `editor_try_commit_float_decl` must run before
  `editor_try_commit_assign_variable`, or that any wholesale document
  replacement must call `editor_undo_clear()` first. A skill can't fire on a
  hazard you didn't know you were near.

So `CLAUDE.md` keeps the trip-wires: C99-everywhere, include style, ownership
prefixes, commit-path ordering, undo-ring rules, "glClear is load-bearing", the
guard commands. Skills get the checklists.

Second test, once the first passes: **is it a procedure with an order, or a
constraint?** Skills are good at "do these five things, in this order, then run
these three commands." Constraints ("never use bare `strchr(s, ',')`") work
better as always-on one-liners.

## Maintenance

`CLAUDE.md` was cut from 43 KB to ~28 KB when these were extracted. It's loaded
in full on every single turn, so it's the expensive file - when you find
yourself adding a paragraph to it, check whether the fact is task-triggered
first.

When a skill's facts change, update the skill *and* check whether `CLAUDE.md`
still has a stale copy of the same fact. The pointers in `CLAUDE.md` name the
skills explicitly (`skill gl-repl-capture`), so `grep -n 'skill gl-repl'
CLAUDE.md` finds every handoff point.

Skills are checked into the repo, so they apply to everyone working on it, not
just one machine. Personal ones go in `~/.claude/skills/` instead.
