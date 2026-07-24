/*
 * glr_capture_env.c - Headless-capture environment hooks.
 *
 * See glr_capture_env.h for the apply-vs-frame-hook split. Everything here is
 * a no-op unless its GLR_* env var is set, so it never affects a normal run.
 */
#include "app/glr_capture_env.h"

#include "app/glr_actions.h"        /* glr_action_toggle_view_mode / _help_tab_next */
#include "app/glr_ctrl.h"           /* glr_ctrl_* capture entry points */
#include "app/glr_pointer_script.h" /* glr_pointer_script_load_env / _active */
#include "app/splash.h"             /* splash_skip */

#include <stdlib.h>                 /* getenv, atoi, atof, strtof */

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
 * <line> opens the floating color picker on that source line (0-based,
 * clamped; no-op unless the line is a picker-editable color command). The
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
        glr_ctrl_open_color_picker(atoi(s));
}

/* Capture affordance, sibling of GLR_OPEN_COLOR_PICKER: GLR_OPEN_GL_STATE=
 * <line> opens the floating OpenGL-state popup anchored to that source line
 * (0-based; must be a visually blank committed or live editor row — the
 * controller's per-frame view builder closes the popup otherwise). The popup
 * only opens via a right-click, which a headless capture run has no mouse to
 * deliver. */
static void maybe_capture_open_gl_state(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *s = getenv("GLR_OPEN_GL_STATE");
    if (s && *s)
        glr_ctrl_open_gl_state_popup(atoi(s));
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
    maybe_capture_open_color_picker();
    maybe_capture_open_gl_state();
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
    /* Cursor line override: GLR_EDIT_LINE=<line> (0-based, clamped)
     * parks the cursor on a committed source line and loads it into the
     * input buffer, exactly as arrowing to it would. Headless-capture
     * hook: cursor-bound overlays (transform guides, vertex labels)
     * can't otherwise be posed without live keyboard input. Applied
     * after the file/example load so the line exists. */
    {
        const char *l_src = getenv("GLR_EDIT_LINE");
        if (l_src && *l_src)
            glr_ctrl_set_edit_line(atoi(l_src));
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
}
