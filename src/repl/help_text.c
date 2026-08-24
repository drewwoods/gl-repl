/*
 * src/repl/help_text.c - Help-overlay text content owned by the REPL.
 *
 * Pulled out of ui_help_overlay.c so the renderer carries no REPL
 * knowledge. The Commands tab is fully static; the Editor and Scene tabs
 * interleave static controls with dynamic F-key entries supplied via
 * a controller-installed ReplHelpFkeyProvider (see help_text.h). The provider
 * keeps the F-key labels dynamic without reaching into the app shell.
 */
#include "repl/help_text.h"
#include "repl/command_spec.h"   /* MAX_FUNC_HINT_PARAMS */
#include "repl/eval.h"           /* REPL_SCRATCH_ARRAY_LEN */
#include "gl_includes.h"
#include "keymap.h"
#include "keys.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

static const ReplHelpFkeyProvider *g_fkey_provider = NULL;

void repl_help_text_install_fkey_provider(const ReplHelpFkeyProvider *provider) {
    g_fkey_provider = provider;
}

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

/* Leading byte that marks a titled divider row: ui_tabbed_overlay draws
 * it as an accent label + a GL rule out to the right edge (see the 0x13
 * branch there), so the marker glyph never renders as text. Octal \023
 * == 0x13 (DC3) - octal caps at 3 digits, so the following label's
 * letters can't extend the escape the way a \x13 hex escape would. */
#define HELP_SEP "\023"

/* Overview tab: orientation for a first-time user - what the program
 * is, what each region of the window is, what the menu bar does, and
 * where to look next. Static/immortal strings (handed to the renderer
 * by pointer like k_lang_sections_tail). Same '\t' left/right column
 * convention; section headers start in column 0, 4-space-indented
 * lines render in the accent colour (used here for the example). */
static const char *const k_tab_overview[] = {
    "OpenGL Immediate-Mode REPL",
    "",
    "  Type classic OpenGL commands, press ; to run each one, and",
    "  watch the geometry build up live in the 3D view. Every command",
    "  stays in an editable code panel, so a scene is just a readable",
    "  list of GL calls you can revisit, tweak, animate, replay",
    "  step-by-step, and export as standalone C.",
    "",
    "What you're looking at:",
    "  3D viewport        \tYour geometry, lit and rendered every frame",
    "  Code panel         \tThe live, editable list of GL commands",
    "  Status bar         \tMessages, AA / profile info, clickable keycaps",
    "  Menu bar (top)     \tFile / Scene / Tutorials / Tours / Config / Audio dropdowns",
    "  Scene tabs         \tSwitch between your saved scenes",
    "",
    "The menu bar:",
    "  File               \tNew / Save / Load a scene or whole workspace",
    "  Scene              \tLoad a built-in example or one of my scenes",
    "  Tutorials          \tGuided, step-by-step lessons",
    "  Config             \tToggle grid, axes, lighting, backdrop, etc.",
    "  Audio              \tPick tracks, play/pause, next/previous, loop",
    "  Replay (far right) \tStep through the scene one command at a time",
    "  search...          \tFind text in the code buffer",
    "",
    "Getting started:",
    "  Enter one command at a time, ending each with ;",
    "    glBegin(GL_TRIANGLES)",
    "    glVertex3f(0, 1, 0)",
    "    glVertex3f(-1, -1, 0)",
    "    glVertex3f(1, -1, 0)",
    "    glEnd()",
    "  Drag in the viewport to orbit; scroll to zoom.",
    "  Use Auto time to animate with the time variable 't'.",
    "  Cycle the built-in examples for ideas.",
    "  Toggle the Variables panel, hover any value,",
    "    Left-click drag  - linear scrub",
    "    Right-click drag - logarithmic",
    "",
    "Finding your way around:",
    "  Open this help any time with the help shortcut or keycap.",
    "  The Commands tab lists every GL command and the REPL language.",
    "  The Editor and Scene tabs cover keyboard and mouse controls.",
    "  The close shortcut or an outside click dismisses this overlay.",
    "  User Guide         \thttps://github.com/drewwoods/gl-repl/blob/main/docs/USER_GUIDE.md",
    "",
    NULL
};

/* '\t' marks the boundary between left column (command) and right
 * column (description). Lines without '\t' render in a single colour
 * based on indent level.
 *
 * Per-command rows (Supported Commands / Lighting / GLUT / GLU) are
 * generated at frame-build time from `k_func_completions[]` in
 * src/repl/command_spec.c so adding a new command only touches the spec.
 * The language-level sections below ("Math Expressions:", "Variables:",
 * For-Loops, etc.) stay hand-written - they document REPL syntax, not
 * commands. */
static const char *const k_lang_sections_tail[] = {
    "Math Operators / Comparisons:",
    "  Operators:  + - * / % ( )",
    "  Comparison: > < >= <= == !=  Logical: && || !",
    "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
    "",
    "Variables (declare before use):",
    "  float x, y, z;           \tDeclare variables",
    "  x = 1.5;                 \tAssign a value",
    "  glVertex3f(x, y, z);     \tUse in expressions",
    "  float n = 1; // @tune    \tTag as a tunable knob (see below)",
    "  float n = 1; // @config  \tKeep bright in the panel when assigned",
    "  Variables persist across commands and are saved/loaded",
    "",
    "Scratch Arrays (A/B/C[" _HELP_STR(REPL_SCRATCH_ARRAY_LEN) "]):",
    "  A[0] = 1;                \tWrite scratch storage (also B[] / C[])",
    "  glVertex3f(A[i], B[i], C[i]);\tRead scratch values in expressions",
    "  Indices truncate to int; use a + (b - a)*t or wrap that in func0..func9",
    "  Scratch arrays persist across commands and are saved/loaded",
    "",
    "For-Loops:",
    "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
    "  for(i, 0, N) {           \tMulti-line block:",
    "    glVertex3f(...)         \tend with }",
    "  }",
    "  Nesting supported up to 4 levels",
    "  break;                   \tLeave the innermost enclosing loop",
    "  continue;                \tSkip to that loop's next iteration",
    "",
    "Functions (func0..func9 or any user name, up to "
        _HELP_STR(MAX_FUNC_HINT_PARAMS) " params):",
    "  func0() {                \tZero-arg decl uses ()",
    "  drawCube() {             \tOr a user-named alias",
    "  func1(radius, sides) {   \tWith parameters",
    "    for(i, 0, sides) {",
    "      glVertex3f(radius*cos(i*TAU/sides), ...)",
    "    }",
    "  }",
    "  drawCube()               \tCall (parens always required)",
    "  func1(1.5, 6)            \tCall with args",
    "  Recursion works with if(...) guard",
    "  Up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " parameters per function",
    "",
    "Conditionals:",
    "  if(t > 1) {              \tBody runs when condition is true",
    "    glColor3f(1, 0, 0)",
    "  } else if(t > 0.5) {     \tFirst matching branch runs",
    "    glColor3f(1, 1, 0)",
    "  } else {                 \tFallback branch",
    "    glColor3f(0, 0, 1)",
    "  }",
    "",
    "  if(0) {                  \tDisable a block - the body emits nothing",
    "",
    "Comments & Tags:",
    "  // text                   \tType directly to add a comment line",
    "  // @tune                  \tOn a float decl: mark as a tunable knob",
    "  Tunable vars get a badge in the variable panel and keyboard",
    "  controls (+ HUD overlay) when exported as standalone C",
    "  // @config                \tOn a float decl: keep the var bright in",
    "  the variable panel even though the program assigns it (the panel",
    "  normally dims assigned vars as state; use for clamp/floor writes)",
    "  // @plot                  \tOn an assignment: open its value plot when",
    "  the scene loads - same as right-clicking the row; tag several rows to",
    "  overlay them (extra rows past the plot's series limit are ignored)",
    "",
    "Save / Load:",
    "  File > Save Scene as .c exports a standalone C program",
    "  Reload a saved file:  ./gl-repl output.c",
    "  Read a snippet from stdin:  printf 'glutSolidCube(1);\\n' | ./gl-repl -",
    "  (Commands between // Snippet start/end are imported)",
    "",
    "Time variable 't':",
    "  Auto-advances from its current value when playing.",
    "  You can pull it back manually, then resume from there.",
    "  Use in any expression: glVertex3f(sin(t), cos(t), 0)",
    "",
    "Accumulation Buffer:",
    "  Effect: Off / AA / Blur / Blur Cam (motion blur).",
    "  AA on by default; Blur is opt-in. Blur interpolates",
    "  camera motion, else the animation time t; Blur Cam",
    "  blurs only camera motion (else AA). --no-accum",
    "  disables; --accum forces it on where a software",
    "  accumulation buffer (Mesa) auto-disables it.",
    "  Passes (samples): 1/2/4/6/8/10/12/14/16. Status: AA 1x / Blur 8x.",
    "",
    NULL
};

/* About tab: attribution for the third-party libraries the build links
 * against. freeglut (windowing/solids/fonts) and miniaudio (playback)
 * ship in every build; the web build adds Emscripten (toolchain +
 * JS windowing) and gl4es (GL -> WebGL2), so those two lines are
 * compiled in only under __EMSCRIPTEN__. Immortal static strings,
 * handed to the renderer by pointer like k_tab_overview. */
static const char *const k_tab_about[] = {
    "OpenGL Immediate-Mode REPL",
    "",
    "  An interactive OpenGL command interpreter: type classic GL",
    "  calls, watch the geometry build up live, and export a scene",
    "  as standalone C.",
    "",
    "User Guide:",
    "  https://github.com/drewwoods/gl-repl/blob/main/docs/USER_GUIDE.md",
    "",
    "Powered by:",
    "  freeglut",
    "  miniaudio",
#if defined(__EMSCRIPTEN__)
    "  Emscripten",
    "  gl4es",
#endif
    "",
    NULL
};

#define HELP_KEY_TAB_COUNT 2
#define HELP_KEYS_MAX 128
#define HELP_KEY_LINE_BUF 144

static char        g_key_strbuf[HELP_KEY_TAB_COUNT][HELP_KEYS_MAX][HELP_KEY_LINE_BUF];
static const char *g_tab_keys[HELP_KEY_TAB_COUNT][HELP_KEYS_MAX];
static int         g_key_tab_build_idx = 0;

/* Commands tab: per-command rows generated from k_func_completions[];
 * the language-tail (Math, Variables, ...) lives in
 * k_lang_sections_tail above. Buffer is sized for current spec entries
 * plus a few hundred-byte cushion; raise both if a new help-grouped
 * command runs out of room. */
/* Bumped from 192 when the single "Supported Commands" group was split
 * into 7 sub-sections (+6 headers, +6 blank separators). */
#define HELP_CMD_LINES_MAX 224
#define HELP_CMD_LINE_BUF  120

static char        g_cmd_strbuf[HELP_CMD_LINES_MAX][HELP_CMD_LINE_BUF];
static const char *g_tab_commands[HELP_CMD_LINES_MAX];

static ReplHelpTab g_tabs[5];
static ReplHelpContent g_content;

static const char *help_group_header(ReplHelpGroup g) {
    switch (g) {
    case REPL_HELP_GROUP_NONE:        assert(0); return NULL;
    case REPL_HELP_GROUP_GEOMETRY:    return "Vertices, Normals & Color:";
    case REPL_HELP_GROUP_PRIMITIVE:   return "Primitive Blocks:";
    case REPL_HELP_GROUP_STATE:       return "Render State:";
    case REPL_HELP_GROUP_RASTER:      return "Raster Position & Bitmap Text:";
    case REPL_HELP_GROUP_BLEND:       return "Point Parameters & Blending:";
    case REPL_HELP_GROUP_TRANSFORM:   return "Matrix Transforms:";
    case REPL_HELP_GROUP_DEPTH_MASK:  return "Depth & Write-Mask State:";
    case REPL_HELP_GROUP_LIGHTING:    return "Lighting / Material:";
    case REPL_HELP_GROUP_GLUT_SHAPES: return "GLUT Solid Shapes:";
    case REPL_HELP_GROUP_GLU_TESS:    return "GLU Tessellator (concave / complex polygons):";
    case REPL_HELP_GROUP_MATH:        return "Math Functions (use anywhere floats are expected):";
    default:                          assert(0); return NULL;
    }
}

/* Optional trailing note printed under a group's last command. */
static const char *help_group_footer(ReplHelpGroup g) {
    if (g == REPL_HELP_GROUP_GLU_TESS)
        return "  Multiple contours in one polygon create holes (opposite winding)";
    return NULL;
}

static int cmd_emit(int n, const char *fmt, ...) {
    if (n >= HELP_CMD_LINES_MAX) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "repl_help: warning: HELP_CMD_LINES_MAX (%d) exceeded, help text truncated.\n", HELP_CMD_LINES_MAX);
            warned = 1;
        }
        return n;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_cmd_strbuf[n], HELP_CMD_LINE_BUF, fmt, ap);
    va_end(ap);
    g_tab_commands[n] = g_cmd_strbuf[n];
    return n + 1;
}

/* Emit one group: header, then `  hint\tdesc-line-1` per entry plus
 * `       \tcontinuation` rows for any `\n`-separated continuations. */
static int cmd_emit_group(int n, ReplHelpGroup group) {
    const char *header = help_group_header(group);
    if (header) n = cmd_emit(n, "%s", header);

    const ReplFuncCompletion *completions = repl_func_completions();
    for (int i = 0; completions[i].insert_text; i++) {
        const ReplFuncCompletion *c = &completions[i];
        if (c->help_group != group) continue;
        const char *desc = c->help_desc ? c->help_desc : "";
        const char *nl = strchr(desc, '\n');
        int seg_len = nl ? (int)(nl - desc) : (int)strlen(desc);
        if (seg_len > 0)
            n = cmd_emit(n, "  %s\t%.*s", c->display_text, seg_len, desc);
        else
            n = cmd_emit(n, "  %s", c->display_text);
        while (nl) {
            desc = nl + 1;
            nl = strchr(desc, '\n');
            seg_len = nl ? (int)(nl - desc) : (int)strlen(desc);
            n = cmd_emit(n, "       \t%.*s", seg_len, desc);
        }
    }

    const char *footer = help_group_footer(group);
    if (footer) n = cmd_emit(n, "%s", footer);

    /* Blank separator after the group. */
    n = cmd_emit(n, "%s", "");
    return n;
}

static int key_emit(int n, const char *fmt, ...) {
    if (n >= HELP_KEYS_MAX) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "repl_help: warning: HELP_KEYS_MAX (%d) exceeded, help text truncated.\n", HELP_KEYS_MAX);
            warned = 1;
        }
        return n;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_key_strbuf[g_key_tab_build_idx][n], HELP_KEY_LINE_BUF, fmt, ap);
    va_end(ap);
    g_tab_keys[g_key_tab_build_idx][n] = g_key_strbuf[g_key_tab_build_idx][n];
    return n + 1;
}

static void key_emit_end(int n) {
    if (n < HELP_KEYS_MAX)
        g_tab_keys[g_key_tab_build_idx][n] = NULL;
    else
        g_tab_keys[g_key_tab_build_idx][HELP_KEYS_MAX - 1] = NULL;
}

static int key_emit_binding(int n, const char *prefix,
                            int key, int mods, int is_special,
                            const char *suffix, const char *desc) {
    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             key, mods, is_special);
    return key_emit(n, "  %s%s%s\t%s",
                    prefix ? prefix : "",
                    shortcut,
                    suffix ? suffix : "",
                    desc ? desc : "");
}

/* Emit an F-key config row in Config-menu order while keeping its label
 * sourced from the controller-owned config table. */
static int key_emit_config_fkey(int n, int fn) {
    const char *label = (g_fkey_provider && g_fkey_provider->fkey_label)
                      ? g_fkey_provider->fkey_label(fn)
                      : NULL;
    if (!label) return n;
    return key_emit_binding(n, "", GLUT_KEY_F1 + fn - 1, 0, 1, "", label);
}

const ReplHelpContent *repl_help_text_build(void) {
    /* --- Commands tab: per-command sections from the spec, then the
     * hand-written language sections. --- */
    int nc = 0;
    for (int g = 1; g < REPL_HELP_GROUP_COUNT; g++) {
        nc = cmd_emit_group(nc, (ReplHelpGroup)g);
    }
    /* Language sections: copy pointers verbatim from the static array
     * (these strings are immortal so we can hand them to the renderer
     * directly without copying into g_cmd_strbuf). */
    for (int i = 0;
         k_lang_sections_tail[i] && nc < HELP_CMD_LINES_MAX - 1;
         i++) {
        g_tab_commands[nc++] = k_lang_sections_tail[i];
    }
    if (nc < HELP_CMD_LINES_MAX)
        g_tab_commands[nc] = NULL;
    else
        g_tab_commands[HELP_CMD_LINES_MAX - 1] = NULL;

    g_key_tab_build_idx = 0;
    int nk = 0;
    nk = key_emit(nk, HELP_SEP "Editor");
    nk = key_emit(nk, "Editing:");
    nk = key_emit(nk, "  ;                    \tCommit current line");
    nk = key_emit(nk, "  Enter                \tInsert new line");
    nk = key_emit(nk, "  Backspace            \tDelete character or selected lines");
    nk = key_emit(nk, "  Tab / Enter          \tAccept autocomplete suggestion");
    nk = key_emit(nk, "  Up / Down            \tNavigate lines");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_PREV_FUNC), KM_MODS(GLR_PREV_FUNC), 1, "",
                          "Previous function");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_NEXT_FUNC), KM_MODS(GLR_NEXT_FUNC), 1, "",
                          "Next function");
    nk = key_emit(nk, "  Left / Right         \tMove cursor within line");
    nk = key_emit_binding(nk, "Home / ", KM_KEY(GLR_LINE_START), KM_MODS(GLR_LINE_START), 0, "",
                          "Jump to start of line");
    nk = key_emit_binding(nk, "End / ", KM_KEY(GLR_LINE_END), KM_MODS(GLR_LINE_END), 0, "",
                          "Jump to end of line");
    nk = key_emit(nk, "  Shift+Left/Right     \tExtend input selection by one char");
    nk = key_emit(nk, "  Shift+Home/End       \tExtend input selection to row start / end");
    nk = key_emit(nk, "  Shift+Up/Down        \tSelect multiple lines");
    nk = key_emit(nk, "  Click + drag (text)  \tSelect characters within the active input row");
    nk = key_emit(nk, "  Click + drag (gutter)\tSelect lines (line-range)");
    nk = key_emit(nk, "  Double-click         \tSelect the word under the cursor");
    nk = key_emit(nk, "  Ctrl+Click / Cmd+Click\tGo to function definition (call site or comment)");
    nk = key_emit(nk, "  Right-click blank row\tInspect current vs default OpenGL state");
    nk = key_emit(nk, "  Caret on ( ) or { }  \tHighlights the matching bracket");
    nk = key_emit(nk, "  Caret inside ( )      \tHighlights text inside the enclosing parentheses");
    nk = key_emit(nk, "  PgUp / PgDn         \tScroll active panel/overlay");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "Inline numeric stepper (cursor on a number in the code panel):");
    nk = key_emit(nk, "  Click up / down      \tNudge the value under the cursor");
    nk = key_emit(nk, "  Right-click up / down\tCoarse step (x10)");
    nk = key_emit(nk, "  Shift+click up / down\tFine step (x1/5)");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "Clipboard & Undo:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_COPY), KM_MODS(GLR_COPY), 0, "",
                          "Copy input selection if active, else line/selection");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CUT), KM_MODS(GLR_CUT), 0, "",
                          "Cut input selection if active, else line/selection");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_PASTE), KM_MODS(GLR_PASTE), 0, "",
                          "Paste input text at cursor, or paste lines");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_UNDO), KM_MODS(GLR_UNDO), 0, "",
                          "Undo (source mutations only)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_REDO), KM_MODS(GLR_REDO), 0, "", "Redo");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "Buffer Operations:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_SEARCH), KM_MODS(GLR_SEARCH), 0, "", "Search source buffer");
    nk = key_emit(nk, "  Tab (in find bar)   \tCycle find field / replace field / whole-word chip");
    nk = key_emit(nk, "  Enter (replace field)\tReplace all matches (one undo step)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CODE_FOCUS), KM_MODS(GLR_CODE_FOCUS), 0, "",
                          "Toggle code focus (hide boilerplate chrome)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_DELETE_LINE), KM_MODS(GLR_DELETE_LINE), 0, "",
                          "Delete line or selection");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_REFORMAT), KM_MODS(GLR_REFORMAT), 0, "", "Reformat buffer");
    nk = key_emit(nk, "  Ctrl+/               \tToggle comment on line");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_DEBUG_DUMP), KM_MODS(GLR_DEBUG_DUMP), 0, "",
                          "Dump debug state to stdout");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_SAVE), KM_MODS(GLR_SAVE), 0, "", "Save Scene");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_QUIT), KM_MODS(GLR_QUIT), 0, "", "Exit and save to temp file");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_ESCAPE), KM_MODS(GLR_ESCAPE), 0, "",
                          "Clear input / close overlay");
    nk = key_emit(nk, "");
    key_emit_end(nk);

    g_key_tab_build_idx = 1;
    nk = 0;
    nk = key_emit(nk, HELP_SEP "Config Controls");
    nk = key_emit(nk, "  Shift+F2..F10        \tCycle the matching F-key item backward");
    nk = key_emit(nk, "RENDERING:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_MSAA), KM_MODS(GLR_MSAA), 0, "",
                          "Toggle GL_MULTISAMPLE");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_LINE_SMOOTH), KM_MODS(GLR_LINE_SMOOTH), 0, "",
                          "Toggle GL_LINE_SMOOTH");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_ACCUM_EFFECT), KM_MODS(GLR_ACCUM_EFFECT), 0, "",
                          "Cycle accumulation effect");
    nk = key_emit(nk, "  Ctrl+= / Ctrl+-      \tIncrease / decrease accumulation passes");
    nk = key_emit_config_fkey(nk, 10);
    nk = key_emit(nk, "TIME & REPLAY:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_AUTO_TIME), KM_MODS(GLR_AUTO_TIME), 0, "",
                          "Play / pause time variable");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_TIME_RESET), KM_MODS(GLR_TIME_RESET), 0, "",
                          "Reset time to 0");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_REPLAY), KM_MODS(GLR_REPLAY), 0, "", "Start / stop replay");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_REPLAY_JUMP), KM_MODS(GLR_REPLAY_JUMP), 0, "",
                          "Jump replay to cursor line (first geometry at/after)");
    nk = key_emit(nk, "  Space                \tPause / resume replay");
    nk = key_emit(nk, "  + / - / Up / Down    \tChange replay speed (Ctrl+Up/Down still jump functions)");
    nk = key_emit(nk, "  m / M                \tToggle polygon / vertex replay mode");
    nk = key_emit(nk, "  e / E                \tCycle Replay expand (Off / Expanded / Verbose)");
    nk = key_emit(nk, "  n / N                \tCycle replay normals (off / vector / + direction)");
    nk = key_emit(nk, "  v / V                \tToggle the replay focused-vertex label");
    nk = key_emit(nk, "  Left / Right         \tStep backward / forward (when paused)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_ESCAPE), KM_MODS(GLR_ESCAPE), 0, "", "Stop replay");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "SCENE:");
    nk = key_emit_config_fkey(nk, 2);
    nk = key_emit_binding(nk, "", KM_KEY(GLR_GRID_MAJOR), KM_MODS(GLR_GRID_MAJOR), 0, "",
                          "Cycle grid major tick spacing (1 / 2 / 5 / 10)");
    nk = key_emit_config_fkey(nk, 3);
    nk = key_emit_config_fkey(nk, 4);
    nk = key_emit_config_fkey(nk, 6);
    nk = key_emit_config_fkey(nk, 5);
    nk = key_emit_config_fkey(nk, 9);
    nk = key_emit_binding(nk, "", KM_KEY(GLR_LIGHT_INDICATORS), KM_MODS(GLR_LIGHT_INDICATORS), 0, "",
                          "Toggle light indicators");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "CAMERA:");
    nk = key_emit(nk, "  Left-drag / scroll   \tOrbit / zoom");
    nk = key_emit(nk, "  Right-drag / Shift+Right-drag\tPan (XZ) / Pan (Y)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_VIEW_MODE), KM_MODS(GLR_VIEW_MODE), 0, "",
                          "Toggle View mode (2D / 3D)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_PROJECTION), KM_MODS(GLR_PROJECTION), 0, "",
                          "Toggle Projection (Perspective / Ortho)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CAMERA_ROTATE), KM_MODS(GLR_CAMERA_ROTATE), 0, "",
                          "Toggle camera auto-rotate");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_FOCUS_ORIGIN), KM_MODS(GLR_FOCUS_ORIGIN), 0, "",
                          "Focus origin (ease target to 0,0,0)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_RESET_CAMERA), KM_MODS(GLR_RESET_CAMERA), 0, "",
                          "Reset camera to default (eased)");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "GEOMETRY:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_WIREFRAME), KM_MODS(GLR_WIREFRAME), 0, "",
                          "Cycle wireframe (Off / Wireframe / Hidden-line)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_WINDING_VIEW), KM_MODS(GLR_WINDING_VIEW), 0, "",
                          "Toggle winding view");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_DEPTH_VIZ), KM_MODS(GLR_DEPTH_VIZ), 0, "",
                          "Cycle depth view");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_STENCIL_VIZ), KM_MODS(GLR_STENCIL_VIZ), 0, "",
                          "Cycle stencil view");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "OVERLAYS:");
    nk = key_emit_config_fkey(nk, 8);
    nk = key_emit_config_fkey(nk, 7);
    nk = key_emit_binding(nk, "", KM_KEY(GLR_VERTEX_POINTS), KM_MODS(GLR_VERTEX_POINTS), 0, "",
                          "Toggle vertex points");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_VERTEX_OUTLINES), KM_MODS(GLR_VERTEX_OUTLINES), 0, "",
                          "Toggle vertex outlines");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_NORMAL_VECTORS), KM_MODS(GLR_NORMAL_VECTORS), 0, "",
                          "Toggle normal vectors");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_POLY_HIGHLIGHT), KM_MODS(GLR_POLY_HIGHLIGHT), 0, "",
                          "Cycle polygon highlight (Off / On / Clipped & culled)");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_XFORM_GUIDES), KM_MODS(GLR_XFORM_GUIDES), 0, "",
                          "Cycle transform guides");
    nk = key_emit(nk, "");
    nk = key_emit(nk, "INTERFACE:");
    nk = key_emit(nk, "  Statusbar buttons    \tUndo/redo, select all, copy/cut, trash, focus/help");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CONFIG_MENU), KM_MODS(GLR_CONFIG_MENU), 0, "", "Open Config menu");
    nk = key_emit(nk, "  Left / right-click item\tCycle config entry forward / backward");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_VARIABLE_PANEL), KM_MODS(GLR_VARIABLE_PANEL), 0, "",
                          "Toggle variable panel");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CPU_PROFILE), KM_MODS(GLR_CPU_PROFILE), 0, "",
                          "Cycle Compute profile panel");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_MEMORY_PROFILE), KM_MODS(GLR_MEMORY_PROFILE), 0, "",
                          "Cycle memory profile panel");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_CODE_PANEL), KM_MODS(GLR_CODE_PANEL), 0, "",
                          "Cycle code panel layout");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_SYNTAX_HL), KM_MODS(GLR_SYNTAX_HL), 0, "",
                          "Cycle syntax highlight (Off / On / On+Shadow)");
    nk = key_emit(nk, "");
    nk = key_emit(nk, HELP_SEP "Tours, Session & Audio");
    nk = key_emit(nk, "Guided Tours (while a Tours-menu tour runs):");
    nk = key_emit(nk, "  Click tour HUD       \tExpand / collapse transport details");
    nk = key_emit(nk, "  Space                \tPause / resume (restart from the end)");
    nk = key_emit(nk, "  Right / Left         \tStep one event / backstep, then pause");
    nk = key_emit(nk, "  + / -                \tFaster / slower (0.25x .. 16x)");
    nk = key_emit(nk, "  Esc                  \tExit tour (keeps its result)");
    nk = key_emit(nk, "Session:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_HELP), KM_MODS(GLR_HELP), 1, "", "Help overlay");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_NEXT_TUTORIAL), KM_MODS(GLR_NEXT_TUTORIAL), 1, "",
                          "Next tutorial");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_PREV_TUTORIAL), KM_MODS(GLR_PREV_TUTORIAL), 1, "",
                          "Previous tutorial");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_NEXT_EXAMPLE), KM_MODS(GLR_NEXT_EXAMPLE), 1, "",
                          "Next example / scene");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_PREV_EXAMPLE), KM_MODS(GLR_PREV_EXAMPLE), 1, "",
                          "Previous example / scene");
    nk = key_emit(nk, "Audio:");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_AUDIO), KM_MODS(GLR_AUDIO), 0, "", "Play / pause");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_AUDIO_PREV), KM_MODS(GLR_AUDIO_PREV), 1, "", "Previous track");
    nk = key_emit_binding(nk, "", KM_KEY(GLR_AUDIO_NEXT), KM_MODS(GLR_AUDIO_NEXT), 1, "", "Next track");
    nk = key_emit(nk, "");
    key_emit_end(nk);

    /* Overview leads so a first-time user lands on orientation, not
     * the raw command dump. */
    g_tabs[0].label = "Overview";
    g_tabs[0].lines = k_tab_overview;
    g_tabs[1].label = "Commands";
    g_tabs[1].lines = g_tab_commands;
    g_tabs[2].label = "Editor";
    g_tabs[2].lines = g_tab_keys[0];
    g_tabs[3].label = "Scene";
    g_tabs[3].lines = g_tab_keys[1];
    g_tabs[4].label = "About";
    g_tabs[4].lines = k_tab_about;

    g_content.title     = "HELP";
    g_content.tabs      = g_tabs;
    g_content.tab_count = (int)(sizeof(g_tabs) / sizeof(g_tabs[0]));
    return &g_content;
}
