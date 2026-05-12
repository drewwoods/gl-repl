# Tutorial System

## Context

The GL REPL has examples (predefined read-only programs) and user scenes
(editable slots). Both are "here's a finished program." A tutorial is
different: it's a **guided, step-by-step interaction** where the user
builds a program one command at a time, with instruction comments that
fade in between steps. The tutorial waits for the user to type each
expected command, blocking wrong input with descriptive errors, and
allowing Tab to auto-fill the expected text (without auto-committing).

## Data Model

### Step definition

```c
typedef struct {
    const char *instruction;   /* shown as "// <instruction>" */
    const char *expected;      /* matched against user input (no ";") */
} TutorialStep;

typedef struct {
    const char *name;          /* menu display name */
    const TutorialStep *steps;
    int step_count;
} TutorialDef;
```

### Match specification

Initial matching: normalize whitespace, case-sensitive string compare.
The match function returns a result struct to support future partial-match
diagnostics:

```c
typedef struct {
    int  matched;       /* 1 = full match */
    int  cmd_matched;   /* 1 = command name correct, args wrong */
    char error[256];    /* human-readable mismatch description */
} TutorialMatchResult;

TutorialMatchResult tutorial_match_input(const char *input,
                                         const TutorialStep *step);
```

`tutorial_match_input` strips leading/trailing whitespace and any trailing
`;` from both sides before comparing. The struct has room for per-argument
matching, partial-command feedback, and tolerance modes later without
changing the call sites.

### Tutorial data file

**New file: `tutorial_data.c` / `tutorial_data.h`**

Parallel to `src/repl/examples.c`. Contains `g_tutorials[]` array of
`TutorialDef`, plus query API:

```c
int               tutorial_data_count(void);
const char       *tutorial_data_name(int idx);
const TutorialStep *tutorial_data_steps(int idx);
int               tutorial_data_step_count(int idx);
```

Ship with 1-2 starter tutorials (e.g. "First Triangle", "Color & Transforms").

## Runtime State (Peer Subsystem)

**New file: `tutorial_state.c` / `tutorial_state.h`**

Peer subsystem following the `replay_state` / `variable_panel_state` pattern:

```c
typedef struct {
    int   active;                /* tutorial mode on/off */
    int   tutorial_idx;          /* which tutorial (-1 = none) */
    int   step_idx;              /* current step within tutorial */
    int   instruction_line_idx;  /* source-line index of active comment */
    float instruction_birth;     /* anim_time when comment was inserted */
    int   completed;             /* 1 = all steps done */
} TutorialRuntimeState;
```

Public API:
- `tutorial_state_reset()` / `tutorial_state_view()` / `tutorial_state_mut()`
- `tutorial_state_active()` convenience predicate

The controller initializes this at startup and resets it on tutorial
load/exit. No capture/restore needed (tutorials don't participate in
scene snapshots).

## Controller Integration

### Loading a tutorial

**In `glr_ctrl.c` (or a new `tutorial.c` controller helper):**

`tutorial_load(int idx)`:
1. Clear the document (`repl_command_store_load` + `source_document_clear`)
2. Reset editor state (insert mode off, cursor to 0)
3. Reset tutorial runtime state: `tutorial_idx = idx`, `step_idx = 0`
4. Insert first instruction comment via `feed_line("// <instruction>;")` (the `;` is needed for `feed_line`)
5. Record `instruction_birth = anim_time` and `instruction_line_idx`
6. Set status: "Tutorial 1/N: Name"

### Commit interception (`;` key)

**In `glr_ctrl_keyboard()`, new router before the `editor_handle_key` fallthrough:**

```c
if (tutorial_state_active()) {
    int r = glr_ctrl_router_handle_tutorial_key(key);
    if (r) {
        glr_ctrl_apply_input_effects(editor_take_input_effects());
        return;
    }
    /* match succeeded for ';' — fall through to editor_handle_key,
       then advance tutorial after commit completes */
}

ReplInputDispatchEffects fx = editor_handle_key(key, x, y);
glr_ctrl_apply_input_effects(fx);

/* post-commit tutorial advance */
if (tutorial_state_active() && key == ';') {
    tutorial_advance_step();
}
```

`glr_ctrl_router_handle_tutorial_key(key)`:
- If `key == ';'`: validate via `tutorial_match_input()`. On mismatch:
  set status with `result.error`, return 1 (consumed — blocks commit).
  On match: return 0 (let `;` fall through to editor for normal commit).
- If `key == '\t'`: fill input buffer with expected text
  (`step->expected`), set cursor to end, return 1.
- Otherwise: return 0.

### Advancing to the next step

`tutorial_advance_step()`:
1. Increment `step_idx`
2. If `step_idx < step_count`: insert next comment via
   `feed_line("// <next_instruction>;")`, record birth time and line index
3. If `step_idx == step_count`: set `completed = 1`, show
   "Tutorial complete!" status

### Tab auto-fill

Tab fills the input buffer with `step->expected` but does not commit.
The user sees the expected command and presses `;` to execute it.
This lets them inspect the command before committing, or edit it.

Implementation: in `glr_ctrl_router_handle_tutorial_key`, when
`key == '\t'`:
```c
const TutorialStep *step = tutorial_current_step();
ReplEditorInputState *inp = editor_state_input_mut();
strncpy(inp->input, step->expected, MAX_INPUT_LEN - 1);
inp->input_len = strlen(inp->input);
editor_cursor_pos_set(inp->input_len);
```

Note: Tab is currently consumed by autocomplete (`repl_autocomplete.c`).
In tutorial mode, the tutorial router must intercept Tab **before**
autocomplete runs. Since the tutorial router is in `glr_ctrl_keyboard()`
and runs before `editor_handle_key()`, this is already the case.

## Menu Integration

### New menu dropdown

**In `glr_actions.h`:** add `GLR_MENU_TUTORIAL` to the enum before
`GLR_MENU_CONFIG`, bump `GLR_MENU_COUNT`.

**In `src/ui/menu_bar.c`:**
- Add `MENU_TUTORIAL = GLR_MENU_TUTORIAL` to local enum
- Add `"Tutorial"` to `g_menu_labels[]`
- `menu_item_count(MENU_TUTORIAL)`: return `tutorial_data_count()` + fixed items
- `menu_item_label(MENU_TUTORIAL, i)`: return tutorial names

Menu layout:
```
[0]        "### TUTORIALS"          (header)
[1..N]     tutorial names           (tutorial_data_count items)
[N+1]      "---"                    (separator, only when active)
[N+2]      "Restart"               (only when tutorial active)
[N+3]      "Exit tutorial"          (only when tutorial active)
```

**In `glr_actions.c`:** handle `GLR_MENU_TUTORIAL` clicks:
- Tutorial name click: `tutorial_load(idx)`
- Restart: `tutorial_load(current_tutorial_idx)`
- Exit: `tutorial_state_reset()`, restore fresh empty scene

### Active tutorial indicator

When a tutorial is active, the "Tutorial" menu label could show a dot
or the tutorial name in the menu bar. Minor polish, not required for MVP.

## Code Panel Fade-In

### New rendering helper

**In `include/gl_2d.h`:** add `gl2d_draw_string_reveal`:

```c
static inline void gl2d_draw_string_reveal(
    float x, float y, const char *s, void *font,
    float r, float g, float b,
    float reveal_progress)    /* 0.0 = invisible, 1.0+ = fully visible */
{
    int len = (int)strlen(s);
    float chars_visible = reveal_progress * (float)len;
    glRasterPos2f(x, y);
    for (int i = 0; s[i]; i++) {
        float a;
        if ((float)i + 1.0f <= chars_visible) a = 1.0f;
        else if ((float)i < chars_visible)    a = chars_visible - (float)i;
        else                                   a = 0.0f;
        glColor4f(r, g, b, a);
        glutBitmapCharacter(font, (unsigned char)s[i]);
    }
}
```

Requires `GL_BLEND` enabled during code panel rendering (already true
for the scroll fade and hover highlights in `src/ui/panels.c`).

### Integration in code panel renderer

**In `src/ui/panels.c`, `code_panel_draw_segment()` or
`code_panel_draw_command_row()`:**

Check if the current line index matches
`tutorial_state_view().instruction_line_idx` and the tutorial is active.
If so, compute `reveal_progress` from elapsed time:

```c
float elapsed = anim_time - tutorial_state_view().instruction_birth;
float reveal = elapsed / TUTORIAL_REVEAL_DURATION;  /* e.g. 0.8s */
```

Call `gl2d_draw_string_reveal()` instead of `gl2d_draw_string()`.
Previous instruction comments (already fully revealed) render normally
since their line index won't match the active instruction.

### Timing constant

`TUTORIAL_REVEAL_DURATION = 0.8f` seconds for the full comment to
reveal. Adjustable. Faster than a typical read speed so the text appears
to "type itself in" without making the user wait.

## New Files Summary

| File | Role |
|------|------|
| `tutorial_data.c` / `.h` | Tutorial definitions (step arrays, names, query API) |
| `tutorial_state.c` / `.h` | Peer subsystem: runtime state (active/step/birth) |
| `tutorial.c` / `.h` | Controller logic: load, match, advance, Tab fill |

Placement: root level alongside existing peer subsystems (`replay_state.c`,
`color_picker_state.c`). Moves to `src/app/` or `src/runtime/` when the
tree reorganization happens.

## Modified Files

| File | Change |
|------|--------|
| `src/app/glr_actions.h` | Add `GLR_MENU_TUTORIAL` enum value |
| `src/app/glr_actions.c` | Handle tutorial menu clicks |
| `src/app/glr_ctrl.c` | Tutorial key router in `glr_ctrl_keyboard()`, post-commit advance |
| `src/ui/menu_bar.c` | Tutorial menu dropdown (item count, labels, layout) |
| `src/ui/panels.c` | Fade-in rendering for active tutorial instruction line |
| `include/gl_2d.h` | `gl2d_draw_string_reveal()` helper |
| `Makefile` | Add new `.c` files to build |

## Verification

1. `make sample` — builds cleanly with new files
2. Run `./sample`, open Tutorial menu, select "First Triangle"
3. First comment fades in left-to-right
4. Type wrong command (e.g. `glEnd()`) — status bar shows error, line not committed
5. Type correct command (`glBegin(GL_TRIANGLES)`) — commits, next comment fades in
6. Press Tab on empty input — expected command fills input buffer, user presses `;` to commit
7. Complete all steps — "Tutorial complete!" status
8. Restart from menu — tutorial resets to step 0
9. Exit tutorial — returns to empty scene
10. Existing tests pass: `make test`
11. `make test-stubs` and `make sample USE_GL_STUBS=1` still build (add stub
    signatures if `gl2d_draw_string_reveal` uses new GL calls — it shouldn't,
    it uses the same `glColor4f`/`glutBitmapCharacter` already stubbed)

## Future Directions (Out of Scope)

- **UI element highlighting**: Flash or pulse a menu item / button / panel
  region to draw attention (e.g. "now click the Replay button"). Would need
  a highlight-target registry mapping names to screen rects, plus a
  pulsing overlay renderer.
- **Popup description windows**: Floating tooltip-style panels that
  describe a UI element when hovered or when a tutorial step points at it.
  Could reuse the tabbed-overlay shell (`src/ui/tabbed_overlay.c`) or a
  simpler single-panel variant.
- **Partial matching**: `TutorialMatchResult.cmd_matched` already has a
  slot for "command correct, argument wrong" feedback. Extend
  `tutorial_match_input` to parse the command name and compare args
  individually, reporting which argument is off.
- **Tutorial progression persistence**: Save completed-tutorial state to
  a dotfile so the menu can show checkmarks.
- **Interactive camera tutorials**: Steps that wait for camera gestures
  (orbit, pan, zoom) instead of typed commands.
