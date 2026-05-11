/*
 * src/repl/help_text.c - Help-overlay text content owned by the REPL.
 *
 * Pulled out of ui_help_overlay.c so the renderer carries no REPL
 * knowledge. The Commands tab is fully static; the Keys tab interleaves
 * a static base with dynamic F-key entries pulled from g_cfg_items
 * (matched on key_code == GLUT_KEY_Fn).
 */
#include "repl/help_text.h"
#include "repl/command_spec.h"   /* MAX_FUNC_HINT_PARAMS */
#include "glr_config.h"
#include "repl/eval.h"           /* REPL_SCRATCH_ARRAY_LEN */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

/* '\t' marks the boundary between left column (command) and right
 * column (description). Lines without '\t' render in a single colour
 * based on indent level.
 *
 * Per-command rows (Supported Commands / Lighting / GLUT / GLU) are
 * generated at frame-build time from `k_func_completions[]` in
 * src/repl/command_spec.c so adding a new command only touches the spec.
 * The language-level sections below ("Math Expressions:", "Variables:",
 * For-Loops, etc.) stay hand-written — they document REPL syntax, not
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
    "  }",
    "",
    "Labels / Goto (experimental, top-level only):",
    "  :loop                    \tDeclare a jump target",
    "  goto loop                \tJump back; pair with if(...) to exit",
    "",
    "Comments:",
    "  // text                   \tType directly to add a comment line",
    "",
    "Save / Load:",
    "  Click Save C or press Ctrl+S to export output.c",
    "  Reload a saved file:  ./sample output.c",
    "  (Commands between // Snippet start/end are imported)",
    "",
    "Time variable 't':",
    "  Auto-advances from its current value when playing.",
    "  You can pull it back manually, then resume from there.",
    "  Use in any expression: glVertex3f(sin(t), cos(t), 0)",
    "",
    "Accumulation Buffer AA:",
    "  On by default; launch with --noaccum to disable.",
    "  Status shown in info bar (AA:8x / AA:off).",
    "",
    NULL
};

/* Same '\t' convention: left column = key, right = action. The F-Key
 * Toggles section is generated dynamically from g_cfg_items so it
 * always reflects the actual bindings without manual sync. */
static const char *const k_tab_keys_base[] = {
    "Editing:",
    "  ;                    \tCommit current line",
    "  Enter                \tInsert new line",
    "  Backspace            \tDelete character or selected lines",
    "  Tab / Enter          \tAccept autocomplete suggestion",
    "  Up / Down            \tNavigate lines",
    "  Left / Right         \tMove cursor within line",
    "  Home / Ctrl+A        \tJump to start of line",
    "  End / Ctrl+E         \tJump to end of line",
    "  Shift+Up/Down        \tSelect multiple lines",
    "  Click + drag         \tSelect lines with mouse",
    "  PgUp / PgDn         \tScroll active panel/overlay",
    "",
    "Clipboard & Undo:",
    "  Ctrl+C               \tCopy line/selection",
    "  Ctrl+X               \tCut line/selection",
    "  Ctrl+V               \tPaste before current line",
    "  Ctrl+Z               \tUndo",
    "  Ctrl+Y               \tRedo",
    "",
    "Buffer Operations:",
    "  Ctrl+F               \tSearch source buffer",
    "  Ctrl+D               \tDelete line or selection",
    "  Ctrl+L               \tClear all commands",
    "  Ctrl+\\              \tReformat buffer",
    "  Ctrl+/               \tToggle comment on line",
    "  Ctrl+P               \tDump debug state to stdout",
    "  Ctrl+S               \tSave to output.c",
    "  Ctrl+Q               \tExit and save to temp file",
    "  Escape               \tClear input / close overlay",
    "",
    "Camera:",
    "  Left-drag            \tOrbit",
    "  Right-drag           \tPan (XZ)",
    "  Shift+Right-drag     \tPan (Y)",
    "  Scroll wheel         \tZoom (viewport) / Scroll (code panel)",
    "",
    "Time & Replay:",
    "  Ctrl+T               \tPlay / pause time variable",
    "  Ctrl+Shift+T         \tReset t to 0",
    "  Ctrl+R               \tStart / stop replay",
    "  Ctrl+K               \tJump replay to cursor line (first geometry at/after)",
    "  Space                \tPause / resume replay",
    "  + / -                \tChange replay speed",
    "  m / M                \tToggle polygon / vertex replay mode",
    "  Left / Right         \tStep backward / forward (when paused)",
    "  Esc                  \tStop replay",
    "",
    "Render State:",
    "  Ctrl+B               \tCycle code panel layout",
    "  Ctrl+=               \tIncrease jitter samples",
    "  Ctrl+-               \tDecrease jitter samples",
    "  Ctrl+U               \tToggle GL_MULTISAMPLE",
    "  Ctrl+O               \tCycle grid major tick spacing (1 / 2 / 5 / 10)",
    "  Ctrl+W               \tCycle CPU profile panel",
    "  Ctrl+B               \tToggle Accum AA",
    "",
    "Interface:",
    "  `                    \tOpen Config menu",
    "  Left-click item      \tCycle config entry forward",
    "  Right-click item     \tCycle config entry backward",
    "",
    "Audio:",
    "  Ctrl+Left            \tPrevious track",
    "  Ctrl+Right           \tNext track",
    "",
    "F-Key Toggles:",
    NULL  /* dynamic F-key lines follow */
};

#define HELP_FKEY_MAX 16
#define HELP_KEYS_MAX 128

static char        g_fkey_strbuf[HELP_FKEY_MAX][48];
static const char *g_tab_keys[HELP_KEYS_MAX];

/* Commands tab: per-command rows generated from k_func_completions[];
 * the language-tail (Math, Variables, …) lives in
 * k_lang_sections_tail above. Buffer is sized for current spec entries
 * plus a few hundred-byte cushion; raise both if a new help-grouped
 * command runs out of room. */
#define HELP_CMD_LINES_MAX 192
#define HELP_CMD_LINE_BUF  120

static char        g_cmd_strbuf[HELP_CMD_LINES_MAX][HELP_CMD_LINE_BUF];
static const char *g_tab_commands[HELP_CMD_LINES_MAX];

static ReplHelpTab g_tabs[2];
static ReplHelpContent g_content;

static const char *help_group_header(ReplHelpGroup g) {
    switch (g) {
    case REPL_HELP_GROUP_TOP:         return "Supported Commands (type + ;):";
    case REPL_HELP_GROUP_LIGHTING:    return "Lighting / Material:";
    case REPL_HELP_GROUP_GLUT_SHAPES: return "GLUT Solid Shapes:";
    case REPL_HELP_GROUP_GLU_TESS:    return "GLU Tessellator (concave / complex polygons):";
    case REPL_HELP_GROUP_MATH:        return "Math Functions (use anywhere floats are expected):";
    default:                          return NULL;
    }
}

/* Optional trailing note printed under a group's last command. */
static const char *help_group_footer(ReplHelpGroup g) {
    if (g == REPL_HELP_GROUP_GLU_TESS)
        return "  Multiple contours in one polygon create holes (opposite winding)";
    return NULL;
}

static int cmd_emit(int n, const char *fmt, ...) {
    if (n >= HELP_CMD_LINES_MAX) return n;
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

const ReplHelpContent *repl_help_text_build(void) {
    /* --- Commands tab: per-command sections from the spec, then the
     * hand-written language sections. --- */
    int nc = 0;
    nc = cmd_emit_group(nc, REPL_HELP_GROUP_TOP);
    nc = cmd_emit_group(nc, REPL_HELP_GROUP_LIGHTING);
    nc = cmd_emit_group(nc, REPL_HELP_GROUP_GLUT_SHAPES);
    nc = cmd_emit_group(nc, REPL_HELP_GROUP_GLU_TESS);
    nc = cmd_emit_group(nc, REPL_HELP_GROUP_MATH);
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

    int nk = 0;
    for (int i = 0;
         k_tab_keys_base[i] != NULL && nk < HELP_KEYS_MAX - HELP_FKEY_MAX - 4;
         i++)
        g_tab_keys[nk++] = k_tab_keys_base[i];

    /* F1 - not in g_cfg_items */
    snprintf(g_fkey_strbuf[0], sizeof(g_fkey_strbuf[0]), "  F1   \tHelp overlay");
    g_tab_keys[nk++] = g_fkey_strbuf[0];

    /* F2-F11 - pulled from the config descriptor table by matching
     * key_code (GLUT_KEY_Fn == n). */
    int di = 1;
    for (int fn = 2; fn <= 11 && di < HELP_FKEY_MAX - 1; fn++) {
        int cfg_count = 0;
        const GlrConfigItem *items = glr_config_items(&cfg_count);
        for (int ci = 0; ci < cfg_count; ci++) {
            const GlrConfigItem *item = &items[ci];
            if (item->section_header || item->key == GLR_CONFIG_NONE)
                continue;
            if (item->is_special && item->key_code == fn) {
                snprintf(g_fkey_strbuf[di], sizeof(g_fkey_strbuf[di]),
                         "  F%-2d  \t%s", fn, item->label);
                g_tab_keys[nk++] = g_fkey_strbuf[di++];
                break;
            }
        }
    }

    /* F12 - not in g_cfg_items */
    snprintf(g_fkey_strbuf[di], sizeof(g_fkey_strbuf[di]),
             "  F12  \tCycle examples");
    g_tab_keys[nk++] = g_fkey_strbuf[di];

    g_tab_keys[nk++] = "";
    g_tab_keys[nk]   = NULL;

    g_tabs[0].label = "Commands";
    g_tabs[0].lines = g_tab_commands;
    g_tabs[1].label = "Keys";
    g_tabs[1].lines = g_tab_keys;

    g_content.title     = "HELP";
    g_content.tabs      = g_tabs;
    g_content.tab_count = 2;
    return &g_content;
}
