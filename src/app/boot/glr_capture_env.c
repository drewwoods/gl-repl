/*
 * glr_capture_env.c - Headless-capture environment hooks.
 *
 * See glr_capture_env.h for the apply-vs-frame-hook split. Everything here is
 * a no-op unless its GLR_* env var is set, so it never affects a normal run.
 */
#include "app/boot/glr_capture_env.h"

#include "app/glr_actions.h"        /* glr_action_toggle_view_mode / _help_tab_next */
#include "app/glr_ctrl.h"           /* glr_ctrl_* capture entry points */
#include "app/glr_pointer_script.h" /* glr_pointer_script_load_env / _active */
#include "app/boot/splash.h"             /* splash_skip */
#include "app/glr_config.h"         /* accum-effect state lookup */
#include "app/glr_state.h"          /* code-focus level check */
#include "subsystems/assign_plot/assign_plot.h"  /* plot presentation chips */

#include <ctype.h>                  /* tolower */
#include <stdio.h>                  /* fprintf */
#include <stdlib.h>                 /* getenv, atoi, atof, strtof */
#include <string.h>                 /* strchr, strcmp */

/* Every GLR_* hook that names a source row spells it the way the code panel's
 * gutter does: 1-based, so what a reader sees beside the line is what the
 * script sets. The controller entry points stay 0-based document indices, and
 * this is the one place that converts. Returns -1 for a line that is not a
 * positive number, which every caller treats as "ignore this hook" rather than
 * silently acting on row 0 — an off-by-one that pointed at real geometry was
 * exactly the failure this numbering is meant to stop. */
static int capture_env_line(const char *s, const char *var) {
    int line;

    if (!s || *s < '0' || *s > '9') {
        fprintf(stderr, "gl-repl: %s=%s ignored (want a line number)\n",
                var, s ? s : "");
        return -1;
    }
    line = atoi(s);
    if (line < 1) {
        fprintf(stderr,
                "gl-repl: %s=%d ignored (code-panel lines start at 1)\n",
                var, line);
        return -1;
    }
    return line - 1;
}

/* Config state labels are user-facing ("Blur Cam"); an env var should not have
 * to reproduce the capitalization or the space. Compare on letters only. */
static int accum_effect_name_eq(const char *label, const char *arg) {
    while (*label && *arg) {
        while (*label == ' ' || *label == '_') label++;
        while (*arg   == ' ' || *arg   == '_') arg++;
        if (!*label || !*arg) break;
        if (tolower((unsigned char)*label) != tolower((unsigned char)*arg))
            return 0;
        label++; arg++;
    }
    while (*label == ' ' || *label == '_') label++;
    while (*arg   == ' ' || *arg   == '_') arg++;
    return *label == 0 && *arg == 0;
}

/* Capture affordance (doc GIFs), sibling of GLR_TIME / GLR_EDIT_LINE:
 * GLR_VIEW_TOGGLE_AT="t1,t2,..." toggles the 2D/3D view mode once as the
 * rendered-frame clock crosses each listed second (t advances 1/60 s per
 * frame), so the menu-bar swatch transition is recordable headlessly.
 * glr_capture_env_apply enables GLR_TICK_PER_FRAME semantics whenever this
 * hook is active, so the transition and the rest of the simulation advance
 * exactly once per captured frame. Unset => no-op; production behavior is
 * unchanged. */
static void maybe_capture_view_toggle(void) {
    static int   inited = 0;
    static int   frame = 0;
    static int   fire_frame[8];
    static int   fired[8];
    static int   n = 0;
    if (!inited) {
        inited = 1;
        const char *s = getenv("GLR_VIEW_TOGGLE_AT");
        while (s && *s && n < 8) {
            char *end;
            float secs = strtof(s, &end);
            if (end == s) break;
            fire_frame[n] = (int)(secs * 60.0f + 0.5f);
            fired[n] = 0;
            n++;
            s = end;
            while (*s == ',' || *s == ' ') s++;
        }
    }
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
        if (!fired[i] && frame >= fire_frame[i]) {
            fired[i] = 1;
            glr_action_toggle_view_mode();
        }
    }
    frame++;
}

/* Capture affordance, sibling of GLR_VIEW_TOGGLE_AT: GLR_OPEN_COLOR_PICKER=
 * <line> opens the floating color picker on that source line (1-based like the
 * code panel; no-op unless the line is a picker-editable color command). The
 * picker only opens via a swatch click, which a capture run has no mouse to
 * deliver. Applied on the first display callback — not at bootstrap —
 * because the popup placement clamps against the live viewport, which
 * reshape has not populated until the main loop starts. */
static void maybe_capture_open_color_picker(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_COLOR_PICKER");
    if (s && *s)
        glr_ctrl_open_color_picker(
            capture_env_line(s, "GLR_OPEN_COLOR_PICKER"));
}

/* Capture affordance, sibling of GLR_OPEN_COLOR_PICKER: GLR_OPEN_GL_STATE=
 * <line> routes a real synthetic right-click to that visible source row
 * (1-based; must be a visually blank committed or live editor row). A
 * headless capture run has no physical mouse to deliver the click. */
static void maybe_capture_open_gl_state(void) {
    static int done = 0;
    const char *s;
    int line;

    if (done) return;
    s = getenv("GLR_OPEN_GL_STATE");
    if (!s || !*s) {
        done = 1;
        return;
    }
    /* GLR_EDIT_LINE follow-scroll lands during the display pass. If the
     * requested row is initially off-screen, leave the hook pending and try
     * its real routed right-click again on the next frame. */
    line = capture_env_line(s, "GLR_OPEN_GL_STATE");
    if (line < 0 || glr_ctrl_open_gl_state_popup(line))
        done = 1;
}

/* Capture affordance, sibling of GLR_OPEN_GL_STATE: GLR_OPEN_ASSIGN_PLOT=
 * <line>[,<line>...] routes a real synthetic right-click to the first source
 * row, which must be an assignment, opening its value plot; any further rows
 * are added as extra series (the Shift+right-click gesture), so a multi-series
 * plot is reachable headlessly. Same retry-until-on-screen shape as the state
 * popup above — only the first row needs to be on screen, since the rest do
 * not go through the code panel's hit model.
 *
 * GLR_ASSIGN_PLOT_EXPANDED=1, GLR_ASSIGN_PLOT_LOG=1 and
 * GLR_ASSIGN_PLOT_RATE=once|1hz|frame set the panel's chips, which are
 * otherwise mouse-only. The rate matters to a capture beyond convenience: at
 * the default 1 Hz a capture run's plot fills at wall-clock speed, so how many
 * samples land in the shot depends on how fast the machine renders. `frame`
 * makes it one column per rendered frame, and the asset deterministic. */
static void maybe_capture_open_assign_plot(void) {
    static int done = 0;
    const char *s;
    const char *p;
    int line;

    if (done) return;
    s = getenv("GLR_OPEN_ASSIGN_PLOT");
    if (!s || !*s) {
        done = 1;
        return;
    }
    line = capture_env_line(s, "GLR_OPEN_ASSIGN_PLOT");
    if (line < 0) {
        done = 1;
        return;
    }
    if (!glr_ctrl_open_assign_plot(line))
        return;   /* row not on screen yet — retry next frame */

    for (p = strchr(s, ','); p; p = strchr(p + 1, ',')) {
        int series = capture_env_line(p + 1, "GLR_OPEN_ASSIGN_PLOT");
        if (series >= 0)
            glr_ctrl_add_assign_plot_series(series);
    }

    {
        const char *expanded = getenv("GLR_ASSIGN_PLOT_EXPANDED");
        const char *log_y    = getenv("GLR_ASSIGN_PLOT_LOG");
        const char *rate     = getenv("GLR_ASSIGN_PLOT_RATE");
        if (expanded && *expanded) assign_plot_toggle_expanded();
        if (log_y && *log_y)       assign_plot_toggle_y_log();
        if (rate && *rate) {
            if (!strcmp(rate, "once"))
                assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
            else if (!strcmp(rate, "1hz"))
                assign_plot_set_rate(ASSIGN_PLOT_RATE_1HZ);
            else if (!strcmp(rate, "frame"))
                assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
            else
                fprintf(stderr,
                        "gl-repl: GLR_ASSIGN_PLOT_RATE=%s ignored"
                        " (want once/1hz/frame)\n", rate);
        }
    }
    done = 1;
}

/* Capture affordance, sibling of GLR_OPEN_ASSIGN_PLOT: GLR_OPEN_COMMAND_HELP=
 * <line>[,<dx>] right-clicks a committed GL-family row to raise its authored
 * help card. Ordered after the assign-plot hook in the frame hook because the
 * routing closes the card when a right-click lands on an assignment row —
 * posing both means opening the plot first.
 *
 * The optional dx slides the card along x from where the click left it (screen
 * px, right positive; the renderer clamps it into the window). The row picks
 * the card and fixes its y, so x is the only axis a capture can curate — which
 * it needs, because the click has to land on the row being explained, not on
 * clear space. */
static void maybe_capture_open_command_help(void) {
    static int done = 0;
    const char *s;
    const char *comma;
    int dx;
    int line;

    if (done) return;
    s = getenv("GLR_OPEN_COMMAND_HELP");
    if (!s || !*s) {
        done = 1;
        return;
    }
    comma = strchr(s, ',');
    dx = comma ? atoi(comma + 1) : 0;
    line = capture_env_line(s, "GLR_OPEN_COMMAND_HELP");
    if (line < 0 || glr_ctrl_open_command_description(line, dx))
        done = 1;   /* else the row is not on screen yet — retry next frame */
}

/* Capture affordance, sibling of GLR_EDIT_LINE: GLR_CODE_SCROLL=<row> parks
 * the code panel's first visible row. Cursor parking cannot reach the
 * generated C that code focus normally hides (init(), the display() prologue)
 * — those rows belong to no document line — so a capture that wants to
 * photograph them scrolls instead. Applied per frame until it lands, since the
 * row count depends on a laid-out panel. */
static void maybe_capture_code_scroll(void) {
    static int done = 0;
    const char *s;
    int row;

    if (done) return;
    s = getenv("GLR_CODE_SCROLL");
    if (!s || !*s) {
        done = 1;
        return;
    }
    row = capture_env_line(s, "GLR_CODE_SCROLL");
    if (row >= 0)
        glr_ctrl_set_code_panel_scroll(row);
    done = 1;
}

/* Capture affordance, sibling of GLR_OPEN_COLOR_PICKER: GLR_OPEN_HELP=<tab>
 * opens the F1 help overlay on the given tab index (0-based, clamped by
 * the tab-advance action) on the first displayed frame. The overlay
 * otherwise needs an F1 special-key press, which a headless capture run
 * has no way to deliver (GLR_TYPE_KEYS only feeds ASCII bytes). */
static void maybe_capture_open_help(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_HELP");
    if (!s) return;
    glr_ctrl_toggle_help();
    int tab = *s ? atoi(s) : 0;
    for (int i = 0; i < tab; i++)
        glr_action_help_tab_next();
}

void glr_capture_env_frame_hook(void) {
    maybe_capture_view_toggle();
    maybe_capture_open_gl_state();
    maybe_capture_open_assign_plot();
    maybe_capture_open_command_help();
    maybe_capture_code_scroll();
    /* Keep the picker last: a real code-panel right-click closes it, so when
     * both capture hooks are requested in one frame the final posed state
     * should still include the picker requested by the caller. */
    maybe_capture_open_color_picker();
    maybe_capture_open_help();
}

void glr_capture_env_apply(const char *time_arg) {
    /* Initial animation time: --time SECS wins over GLR_TIME. Applied after
     * any example load (which resets t to 0) so the override sticks. Lets a
     * headless capture start the animation from a later point in its
     * timeline (e.g. skip a long intro before recording). */
    {
        const char *t_src = time_arg ? time_arg : getenv("GLR_TIME");
        if (t_src && *t_src)
            glr_ctrl_set_time((float)atof(t_src));
    }
    /* Scripted pointer/keyboard input: GLR_POINTER_SCRIPT=<file> drives
     * menu navigation & co. on the rendered-frame clock (video capture
     * hook — see src/app/glr_pointer_script.h for the grammar). Loaded
     * before the tick-per-frame resolve below because an active script
     * implies the mode. GLR_NO_SPLASH skips the startup banner (any
     * capture that shouldn't open on the splash band). */
    glr_pointer_script_load_env();
    {
        const char *no_splash = getenv("GLR_NO_SPLASH");
        if (no_splash && *no_splash)
            splash_skip();
    }
    /* Offline/capture clock: move the complete fixed-dt controller tick from
     * the wall-clock timer to completed frames. GLR_VIEW_TOGGLE_AT implies the
     * mode for backward-compatible deterministic swatch captures. */
    {
        const char *frame_tick = getenv("GLR_TICK_PER_FRAME");
        const char *view_toggle = getenv("GLR_VIEW_TOGGLE_AT");
        glr_ctrl_set_tick_per_frame((frame_tick && *frame_tick) ||
                                    (view_toggle && *view_toggle) ||
                                    glr_pointer_script_active());
    }
    /* Cursor line override: GLR_EDIT_LINE=<line> (1-based, clamped)
     * parks the cursor on a committed source line and loads it into the
     * input buffer, then requests follow-scroll so the next frame makes
     * that line visible, exactly as arrowing to it would. Headless-
     * capture hook: cursor-bound overlays (transform guides, vertex
     * labels) can't otherwise be posed without live keyboard input.
     * Applied after the file/example load so the line exists. */
    {
        const char *l_src = getenv("GLR_EDIT_LINE");
        if (l_src && *l_src) {
            int line = capture_env_line(l_src, "GLR_EDIT_LINE");
            if (line >= 0)
                glr_ctrl_set_edit_line(line);
        }
    }
    /* Typed-input override: GLR_TYPE_KEYS=<text> feeds each character
     * through the keyboard dispatch exactly as typing would (guides,
     * autocomplete ghost, and all). Headless-capture hook for
     * mid-typing states — e.g. a partially-entered glVertex3f( whose
     * 2-DOF plane / 1-DOF line guide can't be posed from a committed
     * line. Applied after GLR_EDIT_LINE so it can extend a loaded line
     * or (on a fresh row) start a new one. */
    {
        const char *k_src = getenv("GLR_TYPE_KEYS");
        if (k_src)
            for (const char *k = k_src; *k; k++)
                glr_ctrl_keyboard((unsigned char)*k, 0, 0);
    }
    /* GLR_OPEN_COLOR_PICKER is applied on the first display callback
     * (glr_capture_env_frame_hook), not here: the picker clamps its
     * placement against the live viewport, which reshape has not
     * populated yet at bootstrap time. */
    /* Accumulation-AA boost: GLR_ACCUM_PASSES=<count> (1/2/4/8/12/16)
     * raises the accumulation sample count. Capture hook: the 2D UI
     * renders outside the accumulation loop, so this antialiases the
     * 3D scene while text keeps its full size — unlike 2x supersampling,
     * which halves the apparent UI text. Applied after the file/example
     * load so it wins over any @cfg accum_passes header. */
    {
        const char *p_src = getenv("GLR_ACCUM_PASSES");
        if (p_src && *p_src)
            glr_ctrl_set_accum_passes(atoi(p_src));
    }
    /* Code focus: GLR_CODE_FOCUS=0 shows the generated C the focused view
     * hides (init(), the display() prologue) — where the light rig's positions
     * and colors are actually written, and the only place to read them, since
     * no REPL command sets them. Toggle-only in the app and bound to a
     * Shift-modified key GLR_TYPE_KEYS cannot deliver, so a capture needs
     * this. Compared against the live state so the var is a level, not an
     * edge. */
    {
        const char *f_src = getenv("GLR_CODE_FOCUS");
        if (f_src && *f_src) {
            int want_focus = atoi(f_src) != 0;
            if (want_focus != (glr_state_presentation().code_focus != 0))
                glr_ctrl_toggle_code_focus();
        }
    }
    /* Accumulation effect: GLR_ACCUM_EFFECT=<state name> picks the Config
     * row's state by its own label (off/aa/blur/blur cam, case- and
     * space-insensitive), so the env var spells what the menu spells. Unlike
     * the pass count this is not scene metadata — no @cfg slug reaches it —
     * and its keyboard binding carries a Shift the synthetic-key path cannot
     * deliver, so a capture has no other route to Blur. */
    {
        const char *e_src = getenv("GLR_ACCUM_EFFECT");
        if (e_src && *e_src) {
            int n = glr_config_state_count(GLR_CONFIG_ACCUM_EFFECT);
            int found = 0;
            for (int i = 0; i < n && !found; i++) {
                const char *name = glr_config_state_name(
                    GLR_CONFIG_ACCUM_EFFECT, i);
                if (name && accum_effect_name_eq(name, e_src)) {
                    glr_config_set(GLR_CONFIG_ACCUM_EFFECT, i);
                    found = 1;
                }
            }
            if (!found)
                fprintf(stderr,
                        "gl-repl: GLR_ACCUM_EFFECT=%s ignored"
                        " (want off/aa/blur/'blur cam')\n", e_src);
        }
    }
}
