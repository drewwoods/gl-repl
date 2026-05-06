/*
 * repl_help_text.c - Help-overlay text content owned by the REPL.
 *
 * Pulled out of ui_help_overlay.c so the renderer carries no REPL
 * knowledge. The Commands tab is fully static; the Keys tab interleaves
 * a static base with dynamic F-key entries pulled from g_cfg_items
 * (matched on key_code == GLUT_KEY_Fn).
 */
#include "repl_help_text.h"
#include "repl_command_spec.h"   /* MAX_FUNC_HINT_PARAMS */
#include "repl_config.h"
#include "repl_eval.h"           /* REPL_SCRATCH_ARRAY_LEN */

#include <stdio.h>

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

/* '\t' marks the boundary between left column (command) and right
 * column (description). Lines without '\t' render in a single colour
 * based on indent level. */
static const char *const k_tab_commands[] = {
    "Supported Commands (type + ;):",
    "  glBegin(MODE)        \tGL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
    "  glEnd()              \tEnd current primitive block",
    "  glVertex3f(x,y,z)    \tSpecify a vertex position",
    "  glVertex2f(x,y)      \tSpecify a 2D vertex (z=0)",
    "  glNormal3f(x,y,z)    \tSpecify a vertex normal",
    "  glColor3f(r,g,b)     \tSpecify vertex color",
    "  glColor4f(r,g,b,a)   \tSpecify color with alpha",
    "  glClearColor(r,g,b,a)\tSet the background clear color",
    "  glTranslatef(x,y,z)  \tTranslate the modelview matrix",
    "  glScalef(sx,sy,sz)   \tScale the modelview matrix",
    "  glRotatef(d,x,y,z)   \tRotate the modelview matrix",
    "  glPushMatrix()       \tPush current matrix onto stack",
    "  glPopMatrix()        \tPop matrix from stack",
    "  glEnable(CAP) / glDisable(CAP)",
    "       \tGL_BLEND, GL_COLOR_MATERIAL, GL_CULL_FACE, GL_DEPTH_TEST",
    "       \tGL_LIGHTING, GL_LIGHT0..GL_LIGHT3, GL_LINE_SMOOTH, GL_LINE_STIPPLE",
    "       \tGL_MULTISAMPLE, GL_NORMALIZE, GL_POINT_SMOOTH",
    "  glShadeModel(MODE)   \tGL_SMOOTH, GL_FLAT",
    "  glFrontFace(MODE)    \tGL_CW, GL_CCW",
    "  glDepthMask(FLAG)    \tGL_TRUE, GL_FALSE (depth-buffer writes)",
    "  glPointSize(size)    \tRasterized point diameter",
    "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)",
    "       \tDistance attenuation: size *= 1/sqrt(const + linear*d + quadratic*d*d)",
    "  glBlendFunc(sfactor, dfactor)",
    "       \tGL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA / GL_ONE",
    "",
    "Lighting / Material:",
    "  glColorMaterial(face, mode)",
    "       \tface: GL_FRONT, GL_BACK, or GL_FRONT_AND_BACK",
    "       \tmode: GL_AMBIENT / GL_AMBIENT_AND_DIFFUSE / GL_DIFFUSE / GL_SPECULAR / GL_EMISSION",
    "  glMaterialf(face, pname, value | {r,g,b,a})",
    "  glLightModeli(pname, param)",
    "       \tpname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER",
    "",
    "GLUT Solid Shapes:",
    "  glutSolidTorus(innerR, outerR, nsides, rings)",
    "  glutSolidCube(size)",
    "  glutSolidSphere(radius, slices, stacks)",
    "  glutSolidTeapot(size)",
    "  glutSolidCone(base, height, slices, stacks)",
    "",
    "GLU Tessellator (concave / complex polygons):",
    "  gluBegin(GLU_POLYGON)  \tStart a tessellated polygon",
    "  gluBegin(GLU_CONTOUR)  \tStart a contour within the polygon",
    "  gluEnd()               \tEnd contour or polygon",
    "  gluNormal(x,y,z)       \tSet per-vertex normal",
    "  gluColor(r,g,b[,a])    \tSet per-vertex color",
    "  gluVertex(x,y,z)       \tAdd vertex to current contour",
    "  Multiple contours in one polygon create holes (opposite winding)",
    "",
    "Math Expressions (use anywhere floats are expected):",
    "  Constants:  PI, TAU       \tFunctions: sin cos tan sqrt abs pow rand",
    "  Operators:  + - * / % ( ) \tAlso: min max floor ceil fmod rem",
    "       \trand(seed[,iter])",
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

static UiOverlayTab g_tabs[2];
static UiOverlayContent g_content;

const UiOverlayContent *repl_help_text_build(void) {
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
        const ReplConfigItem *items = repl_config_items(&cfg_count);
        for (int ci = 0; ci < cfg_count; ci++) {
            const ReplConfigItem *item = &items[ci];
            if (item->section_header || item->key == REPL_CONFIG_NONE)
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
    g_tabs[0].lines = k_tab_commands;
    g_tabs[1].label = "Keys";
    g_tabs[1].lines = g_tab_keys;

    g_content.title     = "HELP";
    g_content.tabs      = g_tabs;
    g_content.tab_count = 2;
    return &g_content;
}
