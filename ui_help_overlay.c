/*
 * repl_help_overlay.c -- Modal F1 help overlay.
 *
 * Two-tab reference card (Commands / Keys) drawn over the live editor.
 * The Keys tab's F-key section is generated dynamically from g_cfg_items
 * so the displayed bindings always match the actual config table.
 */
#include "sample.h"
#include "repl_config.h"
#include "repl_state_views.h"
#include "ui_help_overlay.h"
#include "./include/gl_2d.h"

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

void ui_help_overlay_render(void) {
    ReplHelpState help = repl_state_help();
    if (!help.visible) return;

    /* --- Tab 0: Commands ---
     * '\t' marks the boundary between left column (command) and
     * right column (description).  Lines without '\t' render in
     * a single colour based on indent level. */
    static const char *tab_commands[] = {
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
        "       \tGL_LIGHTING, GL_LINE_SMOOTH, GL_NORMALIZE, GL_POINT_SMOOTH",
        "       \tGL_LIGHT0..GL_LIGHT3",
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
        "GLU / GLUT Primitives:",
        "  gluSphere(r, slices, stacks)",
        "  gluCylinder(baseR, topR, h, slices, stacks)",
        "  gluDisk(innerR, outerR, slices, loops)",
        "  gluPartialDisk(innerR, outerR, slices, loops, start, sweep)",
        "  glutSolidTorus(innerR, outerR, nsides, rings)",
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
        "  Operators:  + - * / % ( ) \tAlso: min max floor ceil fmod  rand(seed[,iter])",
        "  Comparison: > < >= <= == !=  Logical: && || !",
        "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
        "",
        "Variables (declare before use):",
        "  float x, y, z;           \tDeclare variables",
        "  x = 1.5;                 \tAssign a value",
        "  glVertex3f(x, y, z);     \tUse in expressions",
        "  Variables persist across commands and are saved/loaded",
        "",
        "For-Loops:",
        "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
        "  for(i, 0, N) {           \tMulti-line block:",
        "    glVertex3f(...)         \tend with }",
        "  }",
        "  Nesting supported up to 4 levels",
        "",
        "Functions (func0..func9, up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " params):",
        "  func0(radius, sides) {   \tDefine with parameters",
        "    for(i, 0, sides) {",
        "      glVertex3f(radius*cos(i*TAU/sides), ...)",
        "    }",
        "  }",
        "  func0(1.5, 6)            \tCall with args",
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

    /* --- Tab 1: Keys ---
     * Same '\t' convention: left column = key, right = action.
     * The F-Key Toggles section is generated dynamically from g_cfg_items
     * so it always reflects the actual bindings without manual sync. */
    static const char *tab_keys_base[] = {
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

    /* Build complete tab_keys array: static base + dynamic F-key entries */
    #define HELP_FKEY_MAX 16
    #define HELP_KEYS_MAX 128
    static char      fkey_strbuf[HELP_FKEY_MAX][48];
    static const char *tab_keys[HELP_KEYS_MAX];
    {
        int nk = 0;
        for (int i = 0; tab_keys_base[i] != NULL && nk < HELP_KEYS_MAX - HELP_FKEY_MAX - 4; i++)
            tab_keys[nk++] = tab_keys_base[i];

        /* F1 - not in g_cfg_items */
        snprintf(fkey_strbuf[0], sizeof(fkey_strbuf[0]), "  F1   \tHelp overlay");
        tab_keys[nk++] = fkey_strbuf[0];

        /* F2–F11 - pulled from the config descriptor table by matching
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
                    snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]),
                             "  F%-2d  \t%s", fn, item->label);
                    tab_keys[nk++] = fkey_strbuf[di++];
                    break;
                }
            }
        }

        /* F12 - not in g_cfg_items */
        snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]), "  F12  \tCycle examples");
        tab_keys[nk++] = fkey_strbuf[di];

        tab_keys[nk++] = "";
        tab_keys[nk]   = NULL;
    }
    #undef HELP_FKEY_MAX
    #undef HELP_KEYS_MAX

    static const char *tab_labels[] = { "Commands", "Keys" };
    static const char **tabs[]      = { tab_commands, tab_keys };
    #define HELP_NUM_TABS 2

    int help_tab = help.tab_idx;
    int help_scroll = help.scroll;
    if (help_tab < 0) help_tab = 0;
    if (help_tab >= HELP_NUM_TABS) help_tab = HELP_NUM_TABS - 1;

    const char **text = tabs[help_tab];

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    gl2d_begin(repl_state_viewport()->window_w, repl_state_viewport()->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int win_w = repl_state_viewport()->window_w;
    int win_h = repl_state_viewport()->window_h;
    int hx = win_w / 6, hy = win_h / 12;
    int hw = win_w * 2 / 3, hh = win_h * 5 / 6;
    int tab_bar_h = LINE_H + 2;
    int title_h   = LINE_H + 4;
    int pad_top   = title_h + tab_bar_h + 6;
    int pad_bot   = 20;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (help_scroll > max_scroll) help_scroll = max_scroll;
    if (help_scroll < 0) help_scroll = 0;

    /* Background - matches config menu #222 */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f);
    glRectf((float)hx, (float)hy, (float)hx + (float)hw, (float)hy + (float)hh);

    /* Border - matches config menu #3a3a3a */
    glColor4f(0.227f, 0.227f, 0.227f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx,        (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx,        (float)(hy + hh));
    glEnd();

    /* --- Title bar --- */
    {
        int title_y = hy + hh - title_h;
        /* Title bar separator */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)title_y);
        glVertex2f((float)(hx + hw), (float)title_y);
        glEnd();

        /* Title text - dim, left-aligned like config menu section headers */
        glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
        gl2d_draw_string((float)(hx + 14), (float)(title_y + 4), "HELP", FONT_SMALL);

        /* Tab switch hint right-aligned */
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * FONT_SMALL_W - 14;
        glColor4f(0.533f, 0.533f, 0.533f, 0.70f);
        gl2d_draw_string((float)nh_x, (float)(title_y + 4), nav_hint, FONT_SMALL);
    }

    /* --- Tab bar --- */
    {
        int tab_y  = hy + hh - title_h - tab_bar_h;
        int tab_w  = hw / HELP_NUM_TABS;

        /* Tab bar background */
        glColor4f(0.10f, 0.10f, 0.10f, 1.0f);
        glRectf((float)hx, (float)tab_y, (float)hx + (float)hw, (float)tab_y + (float)tab_bar_h);

        for (int t = 0; t < HELP_NUM_TABS; t++) {
            int tx_tab = hx + t * tab_w;
            if (t == help_tab) {
                /* Active tab: bottom accent bar + bright label */
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.85f);
                glRectf((float)tx_tab, (float)tab_y, (float)tx_tab + (float)tab_w, (float)tab_y + 2.0f);
                glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            } else {
                glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            }
            int lbl_len = (int)strlen(tab_labels[t]);
            int lbl_x   = tx_tab + (tab_w - lbl_len * FONT_SMALL_W) / 2;
            gl2d_draw_string((float)lbl_x, (float)(tab_y + 3), tab_labels[t], FONT_SMALL);
        }

        /* Separator line below tab bar */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)tab_y);
        glVertex2f((float)(hx + hw), (float)tab_y);
        glEnd();
    }

    /* --- Content --- */
    {
        int scissor_x = hx + 1;
        int scissor_y = hy + pad_bot;
        int scissor_w = hw - 2;
        int scissor_h = content_h;
        int have_scissor = (scissor_w > 0 && scissor_h > 0);

        if (scissor_w < 0) scissor_w = 0;
        if (scissor_h < 0) scissor_h = 0;

        if (have_scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    int tx      = hx + 14;
    int ty_start = hy + hh - pad_top - LINE_H + 3;

    /* Compute tab stop from widest left column so all right columns align */
    int tab_stop = 0;
    for (int i = 0; i < n_lines; i++) {
        const char *t = strchr(text[i], '\t');
        if (t) {
            int ln = (int)(t - text[i]);
            if (ln > tab_stop) tab_stop = ln;
        }
    }

    for (int i = help_scroll; i < n_lines && i < help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;
        if (text[i][0] == '\0') continue;

        /* '\t' marks the left/right column boundary */
        const char *tab = strchr(text[i], '\t');
        if (tab) {
            /* Left column (command / key) - #d8d8d8 */
            char left[256];
            int ln = (int)(tab - text[i]);
            if (ln > 255) ln = 255;
            memcpy(left, text[i], ln);
            left[ln] = '\0';
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, left, FONT_SMALL);

            /* Right column (description) - aligned to shared tab stop */
            glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            gl2d_draw_string((float)(tx + tab_stop * FONT_SMALL_W), (float)ty,
                        tab + 1, FONT_SMALL);
        } else if (text[i][0] != ' ') {
            /* Section header - dim gray-blue like config menu */
            glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent - code example, green accent */
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.90f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else {
            /* 2-space indent, no split - light label colour */
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x   = hx + hw - 8;
        int bar_top = hy + hh - pad_top;
        int bar_h   = content_h;
        float frac  = (float)visible_lines / (float)n_lines;
        float pos   = (float)help_scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track - #333 */
        glColor4f(0.20f, 0.20f, 0.20f, 0.60f);
        glRectf((float)bar_x, (float)(bar_top - bar_h), (float)bar_x + 4.0f, (float)(bar_top - bar_h) + (float)bar_h);

        /* Thumb - #888 */
        glColor4f(0.533f, 0.533f, 0.533f, 0.80f);
        glRectf((float)bar_x, (float)thumb_y, (float)bar_x + 4.0f, (float)thumb_y + (float)thumb_h);

        /* Scroll hint at bottom */
        if (help_scroll < max_scroll) {
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - help_scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_SMALL_W) / 2;
            glColor4f(0.533f, 0.533f, 0.533f, 0.50f);
            gl2d_draw_string((float)hint_x, (float)(hy + 4), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    gl2d_end();

    #undef HELP_NUM_TABS
}
