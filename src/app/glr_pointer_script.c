/*
 * glr_pointer_script.c - scripted synthetic pointer/keyboard input for
 * headless/offline video capture (GLR_POINTER_SCRIPT). See the header for
 * the script grammar. Events dispatch through the public glr_ctrl_* GLUT
 * entry points so hover highlights, dropdown opens, flyouts, and clicks
 * behave exactly as live input; the overlay pass draws a cursor arrow,
 * a click ripple, and an optional highlight ring so the pointer is
 * visible in the recorded frames.
 */
#include "app/glr_pointer_script.h"
#include "app/glr_actions.h"      /* idempotent view-mode selection */
#include "app/glr_camera.h"       /* deterministic camera reconstruction scope */
#include "app/glr_config.h"       /* GLR_CONFIG_ORTHO_MODE */
#include "app/glr_ctrl.h"
#include "app/glr_tour_snapshot.h" /* whole-app baseline for backstep/restart */
#include "repl/host_effects.h"   /* repl_set_status_error (tour target loss) */
#include "support/cpuprof.h"     /* PROF_TOUR_OVERLAY bracket in the overlay pass */
#include "ui/app/layout.h"       /* ui_layout_scene_rect (scene: targets) */
#include "ui/app/menu_bar.h"     /* ui_menu_bar_target_* (menu/item/sub/pin) */
#include "ui/app/state.h"        /* ui_state_viewport (GL->mouse y flip) */
#include "render3d/view_mode.h"  /* RENDER3D_VIEW_2D / _3D */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "ui/core/gl_2d.h"   /* gl2d_begin/_end; pulls gl_includes.h */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PS_MAX_EVENTS   256
#define PS_MAX_KEY_TEXT 512
#define PS_MAX_TARGET   64
/* Rendered-frame clock rate: script seconds -> frames (GLR_FRAME_DT_SECS). */
#define PS_FPS          60.0f
/* Frames between a click's press and its synthesized release (~0.1s). */
#define PS_CLICK_RELEASE_FRAMES 6
/* Click ripple lifetime in frames. */
#define PS_RIPPLE_FRAMES 24

#if defined(__EMSCRIPTEN__)
/* Resolve browser-shell chrome into the canvas's framebuffer coordinate
 * space. The New button sits above the canvas, so y is normally negative;
 * the scripted cursor glides toward the canvas edge while activation is
 * forwarded to the real DOM control below. */
EM_JS(int, ps_web_shell_target, (const char *name_ptr, int *mx, int *my), {
    var name = UTF8ToString(name_ptr);
    if (name !== 'new' && name !== 'new_scene') return 0;
    var button = document.getElementById('newSceneButton');
    var canvas = Module['canvas'] || document.getElementById('canvas');
    if (!button || !canvas || button.disabled) return 0;
    var br = button.getBoundingClientRect();
    var cr = canvas.getBoundingClientRect();
    if (!cr.width || !cr.height) return 0;
    var x = Math.round((br.left + br.width * 0.5 - cr.left) *
                       canvas.width / cr.width);
    var y = Math.round((br.top + br.height * 0.5 - cr.top) *
                       canvas.height / cr.height);
    if (mx) HEAP32[mx >> 2] = x;
    if (my) HEAP32[my >> 2] = y;
    return 1;
});

/* Queue the real DOM click after the current Wasm frame returns. The shell's
 * existing listener owns the New-scene bridge and restores canvas focus. */
EM_JS(int, ps_web_shell_click, (const char *name_ptr), {
    var name = UTF8ToString(name_ptr);
    if (name !== 'new' && name !== 'new_scene') return 0;
    var button = document.getElementById('newSceneButton');
    if (!button || button.disabled) return 0;
    setTimeout(function() { button.click(); }, 0);
    return 1;
});
#endif

typedef enum {
    PS_MOVE,
    PS_GLIDE,
    PS_CLICK,
    PS_RIGHTCLICK,
    PS_DOWN,
    PS_UP,
    PS_WHEEL,
    PS_VIEW,
    PS_KEY,
    PS_SKEY,
    PS_CHORD,
    PS_RING,
    PS_ECHO,
    PS_PAUSE
} PsVerb;

typedef struct {
    int    frame;                  /* fire frame (time * 60, rounded)   */
    PsVerb verb;
    int    has_xy;                 /* event carries a point (literal or  */
                                   /* symbolic)                          */
    int    x, y;                   /* literal point (move/glide/ring...) */
    char   target[PS_MAX_TARGET];  /* symbolic point token ("" = use     */
                                   /* x/y); resolved at fire time        */
    int    dur_frames;             /* glide/ring/echo duration           */
    int    button;                 /* down/up: GLUT_LEFT/RIGHT_BUTTON    */
    int    wheel_dir;              /* wheel: +1 / -1                     */
    int    view_mode;              /* view: RENDER3D_VIEW_2D / _3D       */
    int    special;                /* skey/chord: GLUT_KEY_* code, or    */
                                   /* -1 for a chord's printable path    */
    int    mods;                   /* chord: GLUT_ACTIVE_* override mask  */
    unsigned char key_byte;        /* chord: control byte (special < 0)  */
    float  size;                   /* echo: requested cap height (px);   */
                                   /* picks the nearest GLUT bitmap font */
    float  cps;                    /* key@N: chars/sec (0 = all at once) */
    int    source_line;            /* physical script line (1-based) for */
                                   /* the tour HUD; 0 when unknown       */
    char   text[PS_MAX_KEY_TEXT];  /* key: unescaped bytes to feed;      */
                                   /* echo: caption text to draw         */
} PsEvent;

/* Which run kind is active (see the header). Two kinds only: the env-driven
 * capture hook (GLR_POINTER_SCRIPT — never canceled, never auto-stops, may be
 * timed or untimed) and the menu-driven controlled tour (untimed, transport
 * controls, HUD, and a brief Done linger for the final caption). Replaces the
 * old g_tour flag. */
typedef enum {
    PS_RUN_NONE = 0,
    PS_RUN_ENV_CAPTURE,
    PS_RUN_CONTROLLED_TOUR
} PsRunKind;
static PsRunKind g_run_kind = PS_RUN_NONE;

static PsEvent g_events[PS_MAX_EVENTS];
static int     g_event_count = 0;
static int     g_active = 0;

static int g_frame = 0;        /* rendered-frame clock, first frame = 0  */
static int g_next_event = 0;   /* events fire in file order              */
static int g_sequential = 0;   /* untimed grammar: wait for each step    */

/* --- controlled-tour transport state ------------------------------------ */

/* Discrete speed ladder (see the header). The virtual clock advances
 * g_frame_credit by g_speed each rendered frame and spends whole credits as
 * virtual tour frames; speed touches pointer-script timing only. */
static const float k_tour_speeds[] = {
    0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f
};
#define PS_TOUR_SPEED_COUNT ((int)(sizeof(k_tour_speeds) / sizeof(k_tour_speeds[0])))
#define PS_TOUR_SPEED_DEFAULT 2   /* index of 1.0x */
#define PS_SEEK_EVENTS_PER_FRAME 32

static GlrTourPlaybackState g_tour_state = GLR_TOUR_OFF;
static int   g_current_event = -1;      /* in-flight event index, or -1     */
static int   g_completed_events = 0;    /* events driven to completion      */
static int   g_speed_idx = PS_TOUR_SPEED_DEFAULT;
static float g_frame_credit = 0.0f;     /* virtual-frame accumulator        */
static int   g_done_until = -1;         /* final-caption expiry; Done only  */
static const char *g_tour_name = NULL;  /* borrowed HUD metadata            */
static const char *g_tour_file = NULL;
static int   g_tour_hud_expanded = 0;   /* compact by default; not rewound  */
static GlrTourSnapshot *g_baseline = NULL;  /* rewind baseline (NULL until   */
                                            /* BASELINE_PENDING resolves)    */
static int   g_seek_target = -1;        /* SEEKING: prefix length to reach  */
/* Stays asserted through the render/tick phase of every seek frame, including
 * the frame whose prefix replay transitions Seeking -> Paused. */
static int   g_suppress_app_tick = 0;
static int   g_camera_reconstructing = 0;

/* An explicit pause blocks event dispatch in either grammar. */
static int g_pause_until = -1;

/* Untimed scripts wait for the current step's intrinsic work to finish
 * before starting the next one. Immediate verbs complete on their fire
 * frame; dispatch still advances by at most one step per frame. */
typedef enum {
    PS_WAIT_NONE,
    PS_WAIT_GLIDE,
    PS_WAIT_CLICK,
    PS_WAIT_TYPE,
    PS_WAIT_RING,
    PS_WAIT_VIEW,
    PS_WAIT_PAUSE
} PsWait;
static PsWait g_step_wait = PS_WAIT_NONE;

static float g_px = 0.0f, g_py = 0.0f;   /* current pointer (window px) */

/* Scripted button currently held (a `down` awaiting its `up`, or a click's
 * synthesized-release window), so moves route through glr_ctrl_motion and
 * drags — camera orbit, slider drags — track the pointer like a real one. */
static int g_button_held = -1;           /* GLUT_*_BUTTON, or -1 = none  */

/* Active glide: ease g_px/g_py toward (to_x, to_y) with smoothstep. */
static int   g_glide_active = 0;
static int   g_glide_start = 0, g_glide_dur = 1;
static float g_glide_from_x = 0.0f, g_glide_from_y = 0.0f;
static float g_glide_to_x = 0.0f, g_glide_to_y = 0.0f;

/* Pending synthesized release for click/rightclick. */
static int g_release_frame = -1;
static int g_release_button = 0;

/* Active paced typing (`key@N`): remaining text feeds through the keyboard
 * dispatch one character at a time on the frame clock, like the tutorial
 * comment reveal — instead of the whole payload landing on one frame. */
static int   g_type_active = 0;
static int   g_type_start = 0;               /* frame of the first char   */
static int   g_type_sent = 0;
static float g_type_cps = 0.0f;
static char  g_type_text[PS_MAX_KEY_TEXT] = "";

/* Overlay bookkeeping: last press (ripple) and active highlight ring. */
static int g_ripple_frame = -1;
static float g_ripple_x = 0.0f, g_ripple_y = 0.0f;
static int   g_ring_start = -1, g_ring_dur = 0;
static float g_ring_x = 0.0f, g_ring_y = 0.0f;

/* Active echo caption: bitmap text shown at a fixed screen spot. */
static int   g_echo_start = -1, g_echo_dur = 0;
static float g_echo_x = 0.0f, g_echo_y = 0.0f, g_echo_size = 0.0f;
static char  g_echo_text[PS_MAX_KEY_TEXT] = "";

static float ps_smoothstep(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Unescape `\n` (Enter, the CR GLUT delivers), `\e` (Esc), `\t` (Tab),
 * `\cX` (the Ctrl+X control byte, e.g. `\cT` toggles animation) and `\\`
 * in a key-text payload; everything else copies verbatim. */
static void ps_unescape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (; *src && o + 1 < cap; src++) {
        char c = *src;
        if (c == '\\' && src[1]) {
            src++;
            switch (*src) {
            case 'n':  c = '\r'; break;
            case 'e':  c = 27;   break;
            case 't':  c = '\t'; break;
            case '\\': c = '\\'; break;
            case 'c':
                if (src[1]) {
                    src++;
                    c = (char)(toupper((unsigned char)*src) & 0x1F);
                } else
                    c = *src;
                break;
            default:   c = *src; break;
            }
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

/* Caption payloads are display text, not keyboard input: `\n` is a visual
 * line break rather than the carriage return emitted by the key verb. Keep
 * unknown escapes verbatim so captions can still show paths and notation. */
static void ps_unescape_caption(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (; *src && o + 1 < cap; src++) {
        if (*src == '\\' && src[1] == 'n') {
            dst[o++] = '\n';
            src++;
        } else if (*src == '\\' && src[1] == '\\') {
            dst[o++] = '\\';
            src++;
        } else {
            dst[o++] = *src;
        }
    }
    dst[o] = '\0';
}

/* Special-key names for the `skey` verb -> GLUT_KEY_* codes. */
static int ps_special_from_name(const char *name) {
    static const struct { const char *name; int key; } k_map[] = {
        { "f1", GLUT_KEY_F1 },   { "f2", GLUT_KEY_F2 },
        { "f3", GLUT_KEY_F3 },   { "f4", GLUT_KEY_F4 },
        { "f5", GLUT_KEY_F5 },   { "f6", GLUT_KEY_F6 },
        { "f7", GLUT_KEY_F7 },   { "f8", GLUT_KEY_F8 },
        { "f9", GLUT_KEY_F9 },   { "f10", GLUT_KEY_F10 },
        { "f11", GLUT_KEY_F11 }, { "f12", GLUT_KEY_F12 },
        { "up", GLUT_KEY_UP },       { "down", GLUT_KEY_DOWN },
        { "left", GLUT_KEY_LEFT },   { "right", GLUT_KEY_RIGHT },
        { "home", GLUT_KEY_HOME },   { "end", GLUT_KEY_END },
        { "pageup", GLUT_KEY_PAGE_UP },
        { "pagedown", GLUT_KEY_PAGE_DOWN },
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
        if (strcmp(name, k_map[i].name) == 0)
            return k_map[i].key;
    return -1;
}

/* Parse a `+`-joined modifier token ("ctrl", "shift", "alt", combos like
 * "ctrl+shift", order-free, case-insensitive) into a GLUT_ACTIVE_* mask for
 * modified key and mouse verbs. Returns -1 on any empty component
 * (leading/trailing/doubled
 * `+`), a repeated modifier, or an unknown name — so a typo fails at load. */
static int ps_parse_mods(const char *tok) {
    int mask = 0;
    const char *p = tok;
    if (!*p) return -1;
    while (*p) {
        char name[16];
        size_t n = 0;
        while (*p && *p != '+' && n + 1 < sizeof(name))
            name[n++] = (char)tolower((unsigned char)*p++);
        name[n] = '\0';
        if (n == 0) return -1;                 /* empty component: "a++b", "+a" */
        int bit;
        if      (strcmp(name, "ctrl") == 0)  bit = GLUT_ACTIVE_CTRL;
        else if (strcmp(name, "shift") == 0) bit = GLUT_ACTIVE_SHIFT;
        else if (strcmp(name, "alt") == 0)   bit = GLUT_ACTIVE_ALT;
        else return -1;                        /* unknown modifier name        */
        if (mask & bit) return -1;             /* duplicate: "ctrl+ctrl"        */
        mask |= bit;
        if (*p == '+') {
            p++;
            if (*p == '\0') return -1;         /* trailing '+'                  */
        }
    }
    return mask;
}

/* Scan a point from the front of `args`: either literal "<x> <y>" pixels
 * or one symbolic target token (see the header's grammar). Fills the
 * event's x/y or target[] and sets has_xy. Returns a pointer just past the
 * point (for trailing args like a glide's duration), or NULL when `args`
 * does not start with a point. Symbolic tokens are prefix-validated here
 * (and `scene:` fractions fully parsed) so a typo fails at load, not
 * mid-run; label existence can only be checked at fire time. */
static const char *ps_scan_point(const char *args, PsEvent *ev) {
    static const char *const k_prefixes[] = {
        "menu:", "item:", "subenter:", "sub:", "pin:", "shell:",
        "scene:", "helptab:", "code:"
    };
    int n = 0;

    while (*args == ' ' || *args == '\t') args++;
    if (sscanf(args, "%d %d%n", &ev->x, &ev->y, &n) == 2) {
        ev->has_xy = 1;
        return args + n;
    }

    char word[PS_MAX_TARGET];
    if (sscanf(args, "%63s%n", word, &n) != 1)
        return NULL;
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++) {
        size_t plen = strlen(k_prefixes[i]);
        if (strncmp(word, k_prefixes[i], plen) != 0 || word[plen] == '\0')
            continue;
        if (strcmp(k_prefixes[i], "scene:") == 0) {
            float fx, fy;
            if (sscanf(word + plen, "%f,%f", &fx, &fy) != 2)
                return NULL;
        }
        snprintf(ev->target, sizeof(ev->target), "%s", word);
        ev->has_xy = 1;
        return args + n;
    }
    return NULL;
}

/* Parse one script line into *ev. Returns 1 on an event, 0 on a blank or
 * comment line, -1 on a malformed line. */
static int ps_parse_line(const char *line, PsEvent *ev, int *timed) {
    char verb[24];
    float secs = 0.0f;
    int consumed = 0;
    int verb_consumed = 0;
    char *time_end;

    /* Skip leading whitespace; blank and #-comment lines are no-ops. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n' || *line == '#') return 0;

    /* A leading timestamp selects the legacy absolute-time grammar. With
     * no timestamp, the first token is the verb and the script runs as a
     * completion-driven sequence. Loaders reject mixing the two forms. */
    secs = strtof(line, &time_end);
    if (time_end != line) {
        if (secs < 0.0f || (*time_end != ' ' && *time_end != '\t'))
            return -1;
        if (sscanf(time_end, " %23s %n", verb, &verb_consumed) < 1)
            return -1;
        consumed = (int)(time_end - line) + verb_consumed;
        *timed = 1;
    } else {
        if (sscanf(line, "%23s %n", verb, &consumed) < 1)
            return -1;
        *timed = 0;
    }

    memset(ev, 0, sizeof(*ev));
    ev->frame = (int)(secs * PS_FPS + 0.5f);
    const char *args = line + consumed;

    if (strcmp(verb, "move") == 0) {
        ev->verb = PS_MOVE;
        return ps_scan_point(args, ev) ? 1 : -1;
    }
    if (strcmp(verb, "glide") == 0) {
        float dur = 0.0f;
        ev->verb = PS_GLIDE;
        args = ps_scan_point(args, ev);
        if (!args || sscanf(args, "%f", &dur) != 1 || dur <= 0.0f)
            return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
        return 1;
    }
    if (strcmp(verb, "click") == 0 || strcmp(verb, "rightclick") == 0 ||
        strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0 ||
        strcmp(verb, "rightdown") == 0 || strcmp(verb, "rightup") == 0) {
        if (strcmp(verb, "click") == 0) {
            ev->verb = PS_CLICK;
        } else if (strcmp(verb, "rightclick") == 0) {
            ev->verb = PS_RIGHTCLICK;
        } else {
            ev->verb = (strcmp(verb, "down") == 0 ||
                        strcmp(verb, "rightdown") == 0) ? PS_DOWN : PS_UP;
            ev->button = (strncmp(verb, "right", 5) == 0)
                       ? GLUT_RIGHT_BUTTON : GLUT_LEFT_BUTTON;
        }
        /* Clicks and a held right press may declare modifiers before their
         * optional point: `click shift item:foo`. A token that is not a
         * modifier remains the point, preserving the short form. */
        if (ev->verb == PS_CLICK || ev->verb == PS_RIGHTCLICK ||
            (ev->verb == PS_DOWN && ev->button == GLUT_RIGHT_BUTTON)) {
            char modtok[32];
            int modread = 0;
            if (sscanf(args, "%31s%n", modtok, &modread) == 1) {
                int mods = ps_parse_mods(modtok);
                if (mods >= 0) {
                    ev->mods = mods;
                    args += modread;
                }
            }
        }
        /* Point is optional (press/release at the current pointer), but a
         * present-yet-malformed one is an error, not a bare click: only
         * blank args or a trailing `#` comment may fall through. */
        if (!ps_scan_point(args, ev)) {
            const char *rest = args;
            while (*rest == ' ' || *rest == '\t' ||
                   *rest == '\n' || *rest == '\r')
                rest++;
            if (*rest != '\0' && *rest != '#')
                return -1;
        }
        return 1;
    }
    if (strcmp(verb, "wheel") == 0) {
        ev->verb = PS_WHEEL;
        return (sscanf(args, "%d", &ev->wheel_dir) == 1 &&
                ev->wheel_dir != 0) ? 1 : -1;
    }
    if (strcmp(verb, "view") == 0) {
        char mode[8];
        int mode_read = 0;
        if (sscanf(args, "%7s%n", mode, &mode_read) != 1)
            return -1;
        const char *rest = args + mode_read;
        while (*rest == ' ' || *rest == '\t' ||
               *rest == '\n' || *rest == '\r')
            rest++;
        if (*rest != '\0' && *rest != '#')
            return -1;
        ev->verb = PS_VIEW;
        if (strcmp(mode, "3d") == 0)
            ev->view_mode = RENDER3D_VIEW_3D;
        else if (strcmp(mode, "2d") == 0)
            ev->view_mode = RENDER3D_VIEW_2D;
        else
            return -1;
        return 1;
    }
    if (strncmp(verb, "key", 3) == 0 &&
        (verb[3] == '\0' || verb[3] == '@')) {
        ev->verb = PS_KEY;
        /* `key@<cps>` paces the payload at chars/sec on the frame clock
         * (bare `key` feeds every byte on the fire frame, as before). */
        if (verb[3] == '@') {
            int n = 0;
            if (sscanf(verb + 4, "%f%n", &ev->cps, &n) != 1 ||
                verb[4 + n] != '\0' || ev->cps <= 0.0f)
                return -1;
        }
        /* Payload is the rest of the line, newline stripped, escapes
         * resolved — so `key glColor3f(` keeps its punctuation. */
        char raw[PS_MAX_KEY_TEXT];
        size_t n = strlen(args);
        while (n > 0 && (args[n - 1] == '\n' || args[n - 1] == '\r'))
            n--;
        if (n == 0 || n >= sizeof(raw)) return -1;
        memcpy(raw, args, n);
        raw[n] = '\0';
        ps_unescape(raw, ev->text, sizeof(ev->text));
        return 1;
    }
    if (strcmp(verb, "skey") == 0) {
        char name[16];
        ev->verb = PS_SKEY;
        if (sscanf(args, "%15s", name) != 1) return -1;
        for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
        ev->special = ps_special_from_name(name);
        return (ev->special >= 0) ? 1 : -1;
    }
    if (strcmp(verb, "chord") == 0) {
        /* chord <mods> <key> — one modified key press. <key> is a special-key
         * name (skey vocabulary) or a single printable char; ctrl on a
         * printable folds to its control byte (like \cX). A shift/alt-only
         * printable has no keymap meaning and is rejected (type it via `key`). */
        char modtok[32], keytok[16];
        int nread = 0;
        ev->verb = PS_CHORD;
        ev->special = -1;               /* printable path unless a name matches */
        if (sscanf(args, "%31s %15s%n", modtok, keytok, &nread) != 2)
            return -1;
        /* Only whitespace or a trailing comment may follow the key token. */
        const char *rest = args + nread;
        while (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')
            rest++;
        if (*rest != '\0' && *rest != '#')
            return -1;
        ev->mods = ps_parse_mods(modtok);
        if (ev->mods < 0)
            return -1;
        char lowered[16];
        size_t i = 0;
        for (; keytok[i] && i + 1 < sizeof(lowered); i++)
            lowered[i] = (char)tolower((unsigned char)keytok[i]);
        lowered[i] = '\0';
        int sp = ps_special_from_name(lowered);
        if (sp >= 0) {
            ev->special = sp;
            return 1;
        }
        /* Not a special-key name: require exactly one printable char + ctrl. */
        if (keytok[1] != '\0' || !(ev->mods & GLUT_ACTIVE_CTRL))
            return -1;
        ev->key_byte = (unsigned char)(toupper((unsigned char)keytok[0]) & 0x1F);
        return 1;
    }
    if (strcmp(verb, "ring") == 0) {
        float dur = 0.0f;
        ev->verb = PS_RING;
        args = ps_scan_point(args, ev);
        if (!args || sscanf(args, "%f", &dur) != 1 || dur <= 0.0f)
            return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        return 1;
    }
    if (strcmp(verb, "echo") == 0) {
        float size = 0.0f, dur = 0.0f;
        int nread = 0;
        ev->verb = PS_ECHO;
        /* echo <point> <size> <dur> <text...> — position + cap height +
         * on-screen lifetime, then the rest of the line is the caption. */
        args = ps_scan_point(args, ev);
        if (!args ||
            sscanf(args, "%f %f %n", &size, &dur, &nread) < 2 ||
            size <= 0.0f || dur <= 0.0f)
            return -1;
        ev->size = size;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
        /* Caption is the remainder verbatim (newline stripped) so a label
         * like `Ctrl+K` keeps its punctuation and spacing. */
        const char *txt = args + nread;
        size_t n = strlen(txt);
        while (n > 0 && (txt[n - 1] == '\n' || txt[n - 1] == '\r')) n--;
        if (n == 0 || n >= sizeof(ev->text)) return -1;
        char raw[PS_MAX_KEY_TEXT];
        memcpy(raw, txt, n);
        raw[n] = '\0';
        ps_unescape_caption(raw, ev->text, sizeof(ev->text));
        return 1;
    }
    if (strcmp(verb, "pause") == 0) {
        float dur = 0.0f;
        int nread = 0;
        const char *rest;
        ev->verb = PS_PAUSE;
        if (sscanf(args, "%f%n", &dur, &nread) != 1 || dur <= 0.0f)
            return -1;
        rest = args + nread;
        while (*rest == ' ' || *rest == '\t' ||
               *rest == '\n' || *rest == '\r')
            rest++;
        if (*rest != '\0' && *rest != '#') return -1;
        ev->dur_frames = (int)(dur * PS_FPS + 0.5f);
        if (ev->dur_frames < 1) ev->dur_frames = 1;
        return 1;
    }
    return -1;
}

int glr_pointer_script_load_env(void) {
    const char *path = getenv("GLR_POINTER_SCRIPT");
    if (!path || !*path) return 0;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: cannot open %s\n", path);
        exit(1);
    }

    char line[512];
    int lineno = 0;
    int mode = -1;
    g_event_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        PsEvent ev;
        int timed = 0;
        int r = ps_parse_line(line, &ev, &timed);
        if (r < 0 || (r > 0 && mode >= 0 && timed != mode)) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s:%d: bad line: %s",
                    path, lineno, line);
            exit(1);
        }
        if (r == 0) continue;
        if (mode < 0) mode = timed;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: GLR_POINTER_SCRIPT: %s: too many events "
                    "(max %d)\n", path, PS_MAX_EVENTS);
            exit(1);
        }
        ev.source_line = lineno;
        g_events[g_event_count++] = ev;
    }
    fclose(fp);

    g_sequential = (mode == 0);
    g_active = 1;
    g_run_kind = PS_RUN_ENV_CAPTURE;
    fprintf(stderr, "gl-repl: pointer script: %d event(s) from %s\n",
            g_event_count, path);
    return 1;
}

int glr_pointer_script_active(void) {
    return g_active;
}

int glr_pointer_script_owns_pointer_motion(void) {
    return g_active && g_button_held >= 0;
}

int glr_pointer_script_tour_active(void) {
    return g_active && g_run_kind == PS_RUN_CONTROLLED_TOUR;
}

static void ps_tour_camera_reconstruction_begin(void) {
    if (g_camera_reconstructing)
        return;
    glr_camera_reconstruction_begin();
    g_camera_reconstructing = 1;
}

static void ps_tour_camera_reconstruction_end(void) {
    if (!g_camera_reconstructing)
        return;
    glr_camera_reconstruction_end();
    g_camera_reconstructing = 0;
}

/* Reset the per-run state (clocks, glide, held button, overlays) without
 * touching the loaded event list. The env loader runs once at startup so it
 * never needed this; runtime tour starts reuse the statics mid-session. */
static void ps_reset_runtime(void) {
    ps_tour_camera_reconstruction_end();
    g_frame = 0;
    g_next_event = 0;
    g_current_event = -1;
    g_completed_events = 0;
    g_frame_credit = 0.0f;
    g_done_until = -1;
    g_suppress_app_tick = 0;
    g_pause_until = -1;
    g_step_wait = PS_WAIT_NONE;
    g_glide_active = 0;
    g_release_frame = -1;
    g_button_held = -1;
    g_ripple_frame = -1;
    g_ring_start = -1;
    g_echo_start = -1;
    g_echo_text[0] = '\0';
    /* A canceled tour drops unsent paced-typing text — the user took over;
     * more synthetic keystrokes would fight their input. */
    g_type_active = 0;
    g_type_text[0] = '\0';
}

int glr_pointer_script_start_tour(const char *name, const char *file,
                                  const char *const *lines, int line_count) {
    glr_pointer_script_stop();
    g_event_count = 0;
    for (int i = 0; i < line_count; i++) {
        PsEvent ev;
        int timed = 0;
        int r = ps_parse_line(lines[i], &ev, &timed);
        /* Controlled tours are untimed, completion-driven scripts: a
         * timestamped or malformed line is an authoring error (built-in tours
         * are validated by gen_tours.py + tests, so this is a backstop). */
        if (r < 0 || (r > 0 && timed)) {
            fprintf(stderr, "gl-repl: tour script line %d: %s: %s\n", i + 1,
                    (r < 0) ? "bad line"
                            : "tours must use untimed, completion-driven events",
                    lines[i]);
            g_event_count = 0;
            return 0;
        }
        if (r == 0) continue;
        if (g_event_count >= PS_MAX_EVENTS) {
            fprintf(stderr, "gl-repl: tour script: too many events (max %d)\n",
                    PS_MAX_EVENTS);
            g_event_count = 0;
            return 0;
        }
        ev.source_line = i + 1;
        g_events[g_event_count++] = ev;
    }
    if (g_event_count == 0) return 0;
    g_sequential = 1;
    ps_reset_runtime();
    g_active = 1;
    g_run_kind = PS_RUN_CONTROLLED_TOUR;
    g_tour_name = name;
    g_tour_file = file;
    g_tour_hud_expanded = 0;
    g_speed_idx = PS_TOUR_SPEED_DEFAULT;
    /* Defer the baseline capture + first event to the next frame: the Tours
     * menu's close path must fully run first (its state is part of the
     * baseline), and event zero must not fire before the snapshot exists. */
    g_tour_state = GLR_TOUR_BASELINE_PENDING;
    return 1;
}

static void ps_dispatch_move(float x, float y) {
    g_px = x;
    g_py = y;
    /* While a scripted button is held this is a drag: route through the
     * motion callback (orbit/slider handlers), not passive motion. */
    if (g_button_held >= 0)
        glr_ctrl_motion((int)(x + 0.5f), (int)(y + 0.5f));
    else
        glr_ctrl_passive_motion((int)(x + 0.5f), (int)(y + 0.5f));
}

static void ps_press(int button, int mods) {
    glr_ctrl_mouse_with_modifiers(button, GLUT_DOWN,
                                  (int)(g_px + 0.5f),
                                  (int)(g_py + 0.5f), mods);
    g_button_held = button;
    g_ripple_frame = g_frame;
    g_ripple_x = g_px;
    g_ripple_y = g_py;
}

static void ps_release(int button) {
    glr_ctrl_mouse(button, GLUT_UP, (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    g_button_held = -1;
}

static void ps_ensure_view_mode(int view_mode) {
    if (glr_config_get(GLR_CONFIG_ORTHO_MODE) != view_mode)
        glr_action_toggle_view_mode();
}

/* Feed paced-typing characters up to index `upto` (exclusive). */
static void ps_type_send(int upto) {
    while (g_type_text[g_type_sent] && g_type_sent < upto) {
        glr_ctrl_keyboard((unsigned char)g_type_text[g_type_sent++],
                          (int)(g_px + 0.5f), (int)(g_py + 0.5f));
    }
    if (!g_type_text[g_type_sent])
        g_type_active = 0;
}

/* Complete any in-flight paced typing immediately. Called before another
 * key/skey event dispatches so a too-tight schedule never interleaves or
 * drops payload text — the remainder snaps in, then the new event fires. */
static void ps_type_flush(void) {
    if (g_type_active)
        ps_type_send((int)sizeof(g_type_text));
}

void glr_pointer_script_stop(void) {
    if (!g_active) return;
    /* Never leave the app mid-drag: complete a pending synthesized click
     * release, then any explicitly held button. */
    if (g_release_frame >= 0) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    if (g_button_held >= 0)
        ps_release(g_button_held);
    g_active = 0;
    g_run_kind = PS_RUN_NONE;
    /* Tear down the controlled-tour transport: drop the rewind baseline and
     * clear playback metadata so a later legacy/env run starts clean. */
    glr_tour_snapshot_destroy(g_baseline);
    g_baseline = NULL;
    g_tour_state = GLR_TOUR_OFF;
    g_tour_name = NULL;
    g_tour_file = NULL;
    g_tour_hud_expanded = 0;
    g_seek_target = -1;
    ps_reset_runtime();
}

int glr_pointer_script_resolve_target(const char *target, int *mx, int *my) {
    if (!target || !*target)
        return 0;
    if (strncmp(target, "menu:", 5) == 0)
        return ui_menu_bar_target_menu(target + 5, mx, my);
    if (strncmp(target, "pin:", 4) == 0)
        return ui_menu_bar_target_pin(target + 4, mx, my);
    if (strncmp(target, "shell:", 6) == 0) {
#if defined(__EMSCRIPTEN__)
        return ps_web_shell_target(target + 6, mx, my);
#else
        return 0;
#endif
    }
    if (strncmp(target, "helptab:", 8) == 0)
        return glr_ctrl_help_tab_point(target + 8, mx, my);
    if (strncmp(target, "code:", 5) == 0)
        return glr_ctrl_code_line_point(target + 5, mx, my);
    if (strncmp(target, "item:", 5) == 0)
        return ui_menu_bar_target_open_row(target + 5, mx, my);
    if (strncmp(target, "subenter:", 9) == 0)
        return ui_menu_bar_target_flyout_entry(target + 9, mx, my);
    if (strncmp(target, "sub:", 4) == 0) {
        char parent[PS_MAX_TARGET];
        const char *rest = target + 4;
        const char *colon = strchr(rest, ':');
        size_t len;
        if (!colon || colon == rest || colon[1] == '\0')
            return 0;
        len = (size_t)(colon - rest);
        if (len >= sizeof(parent))
            return 0;
        memcpy(parent, rest, len);
        parent[len] = '\0';
        return ui_menu_bar_target_flyout_row(parent, colon + 1, mx, my);
    }
    if (strncmp(target, "scene:", 6) == 0) {
        float fx = 0.0f, fy = 0.0f;
        int sx, sy, sw, sh;
        if (sscanf(target + 6, "%f,%f", &fx, &fy) != 2)
            return 0;
        ui_layout_scene_rect(&sx, &sy, &sw, &sh);
        if (sw <= 0 || sh <= 0)
            return 0;
        /* Scene rect is GL space (y up from the bottom); the fraction is
         * measured from the rect's top-left like every mouse coordinate. */
        if (mx) *mx = sx + (int)(fx * (float)sw + 0.5f);
        if (my) *my = ui_state_viewport().window_h - (sy + sh) +
                      (int)(fy * (float)sh + 0.5f);
        return 1;
    }
    return 0;
}

/* Resolve an event's point for firing (a literal point passes through).
 * On a failed symbolic resolve, a capture run exits nonzero — recording
 * the wrong interaction is worse than failing — while a tour stops with a
 * status message. Returns 0 when the event must be dropped. */
static int ps_fire_point(const PsEvent *ev, float *x, float *y) {
    int mx, my;
    if (!ev->target[0]) {
        *x = (float)ev->x;
        *y = (float)ev->y;
        return 1;
    }
    if (glr_pointer_script_resolve_target(ev->target, &mx, &my)) {
        *x = (float)mx;
        *y = (float)my;
        return 1;
    }
    fprintf(stderr, "gl-repl: pointer script: cannot resolve target '%s'\n",
            ev->target);
    if (g_run_kind == PS_RUN_ENV_CAPTURE)
        exit(1);
    glr_pointer_script_stop();
    repl_set_status_error("Tour stopped (target not found)");
    return 0;
}

static int ps_fire_shell_click(const PsEvent *ev) {
#if defined(__EMSCRIPTEN__)
    if (ps_web_shell_click(ev->target + 6))
        return 1;
#endif
    fprintf(stderr, "gl-repl: pointer script: cannot activate target '%s'\n",
            ev->target);
    if (g_run_kind == PS_RUN_ENV_CAPTURE)
        exit(1);
    glr_pointer_script_stop();
    repl_set_status_error("Tour stopped (target unavailable)");
    return 0;
}

static void ps_fire(const PsEvent *ev) {
    float x = 0.0f, y = 0.0f;
    switch (ev->verb) {
    case PS_MOVE:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 0;
        ps_dispatch_move(x, y);
        break;
    case PS_GLIDE:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 1;
        g_glide_start = g_frame;
        g_glide_dur = ev->dur_frames;
        g_glide_from_x = g_px;
        g_glide_from_y = g_py;
        g_glide_to_x = x;
        g_glide_to_y = y;
        break;
    case PS_CLICK:
    case PS_RIGHTCLICK: {
        int button = (ev->verb == PS_CLICK) ? GLUT_LEFT_BUTTON
                                            : GLUT_RIGHT_BUTTON;
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        /* Browser-shell controls live outside GLUT's canvas. Resolve them
         * for pointer motion as normal, then activate the real DOM button
         * instead of sending an impossible negative-y canvas click. */
        if (ev->verb == PS_CLICK &&
            strncmp(ev->target, "shell:", 6) == 0) {
            if (!ps_fire_shell_click(ev)) break;
            g_ripple_frame = g_frame;
            g_ripple_x = g_px;
            g_ripple_y = g_py;
            break;
        }
        ps_press(button, ev->mods);
        g_release_frame = g_frame + PS_CLICK_RELEASE_FRAMES;
        g_release_button = button;
        break;
    }
    case PS_DOWN:
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_press(ev->button, ev->mods);
        break;
    case PS_UP:
        /* Drag release: the button is still held, so a coordinate move
         * routes through motion and drag handlers see the final position.
         * (A failed resolve in tour mode stops the script, which releases
         * the held button itself — no explicit release needed then.) */
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_release(ev->button);
        break;
    case PS_WHEEL:
        glr_ctrl_mousewheel(0, ev->wheel_dir,
                            (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_VIEW:
        ps_ensure_view_mode(ev->view_mode);
        break;
    case PS_KEY:
        ps_type_flush();
        if (ev->cps > 0.0f) {
            /* Paced: stash the payload; the per-frame step below the event
             * loop sends the first character this same frame. */
            g_type_active = 1;
            g_type_start = g_frame;
            g_type_sent = 0;
            g_type_cps = ev->cps;
            snprintf(g_type_text, sizeof(g_type_text), "%s", ev->text);
        } else {
            for (const char *c = ev->text; *c; c++)
                glr_ctrl_keyboard((unsigned char)*c,
                                  (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        }
        break;
    case PS_SKEY:
        ps_type_flush();
        glr_ctrl_special(ev->special,
                         (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_CHORD:
        ps_type_flush();
        if (ev->special >= 0)
            glr_ctrl_special_with_modifiers(ev->special,
                                            (int)(g_px + 0.5f),
                                            (int)(g_py + 0.5f), ev->mods);
        else
            glr_ctrl_keyboard_with_modifiers(ev->key_byte,
                                             (int)(g_px + 0.5f),
                                             (int)(g_py + 0.5f), ev->mods);
        break;
    case PS_RING:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_ring_start = g_frame;
        g_ring_dur = ev->dur_frames;
        g_ring_x = x;
        g_ring_y = y;
        break;
    case PS_ECHO:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_echo_start = g_frame;
        g_echo_dur = ev->dur_frames;
        g_echo_x = x;
        g_echo_y = y;
        g_echo_size = ev->size;
        snprintf(g_echo_text, sizeof(g_echo_text), "%s", ev->text);
        break;
    case PS_PAUSE:
        g_pause_until = g_frame + ev->dur_frames;
        break;
    }
}

/* Select the completion condition for one untimed step. A ring is itself an
 * action and completes with its overlay; an echo is a non-blocking caption
 * whose on-screen lifetime may overlap later actions. A click completes at
 * synthesized release, while its decorative ripple may overlap too. */
static void ps_wait_for_event(const PsEvent *ev) {
    switch (ev->verb) {
    case PS_GLIDE:      g_step_wait = PS_WAIT_GLIDE; break;
    case PS_CLICK:
    case PS_RIGHTCLICK: g_step_wait = PS_WAIT_CLICK; break;
    case PS_KEY:
        g_step_wait = ev->cps > 0.0f ? PS_WAIT_TYPE : PS_WAIT_NONE;
        break;
    case PS_RING:       g_step_wait = PS_WAIT_RING; break;
    case PS_VIEW:       g_step_wait = PS_WAIT_VIEW; break;
    case PS_ECHO:       g_step_wait = PS_WAIT_NONE; break;
    case PS_PAUSE:      g_step_wait = PS_WAIT_PAUSE; break;
    default:            g_step_wait = PS_WAIT_NONE; break;
    }
}

static int ps_step_complete(void) {
    switch (g_step_wait) {
    case PS_WAIT_NONE:  return 1;
    case PS_WAIT_GLIDE: return !g_glide_active;
    case PS_WAIT_CLICK: return g_release_frame < 0;
    case PS_WAIT_TYPE:  return !g_type_active;
    case PS_WAIT_RING:
        return g_ring_start < 0 || g_frame - g_ring_start >= g_ring_dur;
    case PS_WAIT_VIEW:  return !glr_ctrl_view_transition_active();
    case PS_WAIT_PAUSE: return g_pause_until < 0;
    }
    return 1;
}

/* Advance the script one virtual tour frame: fire due events, step glide +
 * paced typing, tick the frame clock, and run the auto-stop check. A rendered
 * frame maps to exactly one virtual frame for env-capture and legacy runtime
 * scripts; controlled tours will drive this zero-or-more times per rendered
 * frame under the virtual clock (speed control). */
static void ps_advance_one_virtual_frame(void) {
    /* Synthesized click release first, so a click's press and release
     * bracket any events scheduled between them. */
    if (g_release_frame >= 0 && g_frame >= g_release_frame) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }

    if (g_pause_until >= 0 && g_frame >= g_pause_until)
        g_pause_until = -1;

    /* Untimed scripts are completion-driven and start at most one new step
     * per rendered frame. Timestamped scripts retain their absolute-time
     * behavior; an explicit pause temporarily gates dispatch. g_active can
     * drop inside ps_fire when a tour target fails to resolve. */
    if (g_sequential) {
        if (ps_step_complete()) {
            g_step_wait = PS_WAIT_NONE;
            if (g_next_event < g_event_count) {
                const PsEvent *ev = &g_events[g_next_event++];
                ps_fire(ev);
                if (g_active) ps_wait_for_event(ev);
            }
        }
    } else if (g_pause_until < 0) {
        while (g_active && g_next_event < g_event_count &&
               g_events[g_next_event].frame <= g_frame) {
            const PsEvent *ev = &g_events[g_next_event++];
            ps_fire(ev);
            if (g_pause_until >= 0) break;
        }
    }
    if (!g_active) return;

    /* Step paced typing: characters due by now = elapsed * cps, plus one
     * so the first character lands on the event's own fire frame. */
    if (g_type_active)
        ps_type_send((int)((float)(g_frame - g_type_start) *
                           g_type_cps / PS_FPS) + 1);

    /* Step the active glide AFTER event fire so the glide's own start
     * frame emits its first (near-source) sample this same frame. */
    if (g_glide_active) {
        float t = (float)(g_frame - g_glide_start) / (float)g_glide_dur;
        float s = ps_smoothstep(t);
        ps_dispatch_move(g_glide_from_x + (g_glide_to_x - g_glide_from_x) * s,
                         g_glide_from_y + (g_glide_to_y - g_glide_from_y) * s);
        if (t >= 1.0f)
            g_glide_active = 0;
    }

    g_frame++;
    /* No auto-stop on this path: env-driven capture keeps the overlay up until
     * the recorder exits (the clip should never end cursor-less). Controlled
     * tours run ps_tour_frame() and auto-close after their Done linger. */
}

/* --- controlled-tour transport engine ----------------------------------- */

/* Seed the scripted pointer from the live UI pointer so the first glide/move
 * eases from where the real cursor sits (baseline capture + every seek). */
static void ps_tour_seed_pointer(void) {
    UiPointerState p = ui_state_pointer();
    g_px = (float)p.mouse_x;
    g_py = (float)p.mouse_y;
}

/* HUD source line: the active event while playing/stepping, the next event
 * while paused before it, the final event in Done, -1 with no event. */
static int ps_tour_hud_source_line(void) {
    if (g_event_count <= 0)
        return -1;
    if (g_tour_state == GLR_TOUR_DONE)
        return g_events[g_event_count - 1].source_line;
    if (g_current_event >= 0 && g_current_event < g_event_count)
        return g_events[g_current_event].source_line;
    if (g_next_event < g_event_count)
        return g_events[g_next_event].source_line;
    return g_events[g_event_count - 1].source_line;
}

/* Execute one event to completion synchronously, bypassing its PsWait. Shared
 * by the Right-Arrow step and the fast-seek prefix. `allow_overlays` gates the
 * decorative ring/echo/ripple: suppressed for historical seek events, permitted
 * for a Right step and for the final replayed seek event. */
static void ps_finish_event_immediate(const PsEvent *ev, int allow_overlays) {
    float x = 0.0f, y = 0.0f;
    switch (ev->verb) {
    case PS_MOVE:
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 0;
        ps_dispatch_move(x, y);
        break;
    case PS_GLIDE: {
        if (!ps_fire_point(ev, &x, &y)) break;
        g_glide_active = 0;
        float fx = g_px, fy = g_py;
        int dur = ev->dur_frames > 0 ? ev->dur_frames : 1;
        /* Dispatch smoothstep samples across the original duration, including
         * the exact endpoint (f == dur => s == 1), so a held-button drag
         * reproduces its intermediate motion. */
        for (int f = 1; f <= dur; f++) {
            float s = ps_smoothstep((float)f / (float)dur);
            ps_dispatch_move(fx + (x - fx) * s, fy + (y - fy) * s);
        }
        break;
    }
    case PS_CLICK:
    case PS_RIGHTCLICK: {
        int button = (ev->verb == PS_CLICK) ? GLUT_LEFT_BUTTON
                                            : GLUT_RIGHT_BUTTON;
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        if (ev->verb == PS_CLICK && strncmp(ev->target, "shell:", 6) == 0) {
            if (!ps_fire_shell_click(ev)) break;
            if (allow_overlays) {
                g_ripple_frame = g_frame;
                g_ripple_x = g_px;
                g_ripple_y = g_py;
            }
            break;
        }
        ps_press(button, ev->mods); /* press + release synchronously (no wait) */
        ps_release(button);
        if (!allow_overlays)
            g_ripple_frame = -1;   /* ps_press armed a ripple; suppress it */
        break;
    }
    case PS_DOWN:
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_press(ev->button, ev->mods);   /* leave held (drag reproduction) */
        if (!allow_overlays)
            g_ripple_frame = -1;
        break;
    case PS_UP:
        if (ev->has_xy) {
            if (!ps_fire_point(ev, &x, &y)) break;
            ps_dispatch_move(x, y);
        }
        ps_release(ev->button);
        break;
    case PS_WHEEL:
        glr_ctrl_mousewheel(0, ev->wheel_dir,
                            (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_VIEW:
        ps_ensure_view_mode(ev->view_mode);
        if (g_tour_state == GLR_TOUR_SEEKING)
            glr_ctrl_view_transition_finish_reconstruction();
        break;
    case PS_KEY:
        ps_type_flush();
        /* Deliver the whole payload synchronously (ignore key@N pacing). */
        for (const char *c = ev->text; *c; c++)
            glr_ctrl_keyboard((unsigned char)*c,
                              (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_SKEY:
        ps_type_flush();
        glr_ctrl_special(ev->special,
                         (int)(g_px + 0.5f), (int)(g_py + 0.5f));
        break;
    case PS_CHORD:
        ps_type_flush();
        if (ev->special >= 0)
            glr_ctrl_special_with_modifiers(ev->special, (int)(g_px + 0.5f),
                                            (int)(g_py + 0.5f), ev->mods);
        else
            glr_ctrl_keyboard_with_modifiers(ev->key_byte, (int)(g_px + 0.5f),
                                             (int)(g_py + 0.5f), ev->mods);
        break;
    case PS_RING:
        if (!allow_overlays) break;   /* create overlay only when permitted */
        if (!ps_fire_point(ev, &x, &y)) break;
        g_ring_start = g_frame;
        g_ring_dur = ev->dur_frames;
        g_ring_x = x;
        g_ring_y = y;
        break;
    case PS_ECHO:
        if (!allow_overlays) break;
        if (!ps_fire_point(ev, &x, &y)) break;
        g_echo_start = g_frame;
        g_echo_dur = ev->dur_frames;
        g_echo_x = x;
        g_echo_y = y;
        g_echo_size = ev->size;
        snprintf(g_echo_text, sizeof(g_echo_text), "%s", ev->text);
        break;
    case PS_PAUSE:
        break;   /* skip the dwell entirely */
    }
}

/* Clear the decorative overlays (highlight ring, echo caption, click ripple).
 * Their lifetime is measured against the virtual clock (g_frame), which is
 * FROZEN while paused/stepping — so an overlay shown at a step would otherwise
 * hang on screen forever, and each step would stack a new stuck one. Transport
 * steps and seeks clear them at the boundary; only the event the step lands on
 * re-creates its own. */
static void ps_clear_decor_overlays(void) {
    g_ring_start = -1;
    g_echo_start = -1;
    g_echo_text[0] = '\0';
    g_ripple_frame = -1;
}

/* Force the in-flight event's pending waits to completion without re-firing it
 * (Right Arrow mid-event): snap a glide to its endpoint, release a pending
 * synthesized click, flush paced typing, cancel a pause. */
static void ps_force_current_complete(void) {
    if (g_glide_active) {
        ps_dispatch_move(g_glide_to_x, g_glide_to_y);
        g_glide_active = 0;
    }
    if (g_release_frame >= 0) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    ps_type_flush();
    g_pause_until = -1;
    /* A ring/echo overlay simply keeps running out on its own clock. */
}

/* Enter Done: release any held button and clear in-flight state, but retain
 * the HUD/cursor until the currently visible final caption reaches its authored
 * expiry. With no live caption, Done is presented for this frame and the tour
 * closes on the next one. */
static void ps_tour_enter_done(void) {
    if (g_release_frame >= 0) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    if (g_button_held >= 0)
        ps_release(g_button_held);
    ps_type_flush();
    g_glide_active = 0;
    g_pause_until = -1;
    g_current_event = -1;
    g_step_wait = PS_WAIT_NONE;
    g_frame_credit = 0.0f;
    g_done_until = (g_echo_start >= 0 &&
                    g_frame < g_echo_start + g_echo_dur)
                 ? g_echo_start + g_echo_dur : g_frame;
    g_tour_state = GLR_TOUR_DONE;
}

/* Advance only the overlay clock while Done. Application time and REPL replay
 * remain owned by the normal frame path, so closing the tour never stops a
 * replay the script launched. */
static void ps_tour_advance_done(void) {
    if (g_frame >= g_done_until) {
        glr_pointer_script_stop();
        return;
    }
    g_frame_credit += k_tour_speeds[g_speed_idx];
    while (g_active && g_frame_credit >= 1.0f) {
        if (g_frame >= g_done_until) {
            glr_pointer_script_stop();
            return;
        }
        g_frame++;
        g_frame_credit -= 1.0f;
    }
    if (g_active && g_frame >= g_done_until)
        glr_pointer_script_stop();
}

/* One virtual tour frame under normal playback: complete the in-flight event,
 * fire the next, step glide + paced typing, tick the clock, and detect Done.
 * Event accounting: firing does not count completion; the PsWait completing
 * does (immediate verbs complete the following virtual frame, as before). */
static void ps_tour_advance_one_virtual_frame(void) {
    if (g_release_frame >= 0 && g_frame >= g_release_frame) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    if (g_pause_until >= 0 && g_frame >= g_pause_until)
        g_pause_until = -1;

    if (g_current_event >= 0 && ps_step_complete()) {
        g_completed_events++;
        g_current_event = -1;
        g_step_wait = PS_WAIT_NONE;
    }
    if (g_active && g_current_event < 0 && g_next_event < g_event_count) {
        int idx = g_next_event++;
        g_current_event = idx;
        const PsEvent *ev = &g_events[idx];
        ps_fire(ev);
        if (g_active)
            ps_wait_for_event(ev);
    }
    if (!g_active)
        return;

    if (g_type_active)
        ps_type_send((int)((float)(g_frame - g_type_start) *
                           g_type_cps / PS_FPS) + 1);

    if (g_glide_active) {
        float t = (float)(g_frame - g_glide_start) / (float)g_glide_dur;
        float s = ps_smoothstep(t);
        ps_dispatch_move(g_glide_from_x + (g_glide_to_x - g_glide_from_x) * s,
                         g_glide_from_y + (g_glide_to_y - g_glide_from_y) * s);
        if (t >= 1.0f)
            g_glide_active = 0;
    }

    g_frame++;

    if (g_completed_events >= g_event_count && g_current_event < 0)
        ps_tour_enter_done();
}

/* Begin a backstep/restart seek to `target` completed events: enter Seeking,
 * release held/pending mouse state, clear in-flight typing/glide/waits/
 * overlays, restore the baseline, re-sync controller chrome, and reset the
 * script cursor. The prefix [0, target) is fast-executed by ps_tour_seek_step
 * across subsequent frames. */
static void ps_tour_begin_seek(int target) {
    if (!g_baseline) {
        g_tour_state = GLR_TOUR_PAUSED;
        return;
    }
    g_tour_state = GLR_TOUR_SEEKING;
    g_suppress_app_tick = 1;
    ps_tour_camera_reconstruction_begin();

    if (g_release_frame >= 0) {
        g_release_frame = -1;
        ps_release(g_release_button);
    }
    if (g_button_held >= 0)
        ps_release(g_button_held);
    g_glide_active = 0;
    g_type_active = 0;
    g_type_text[0] = '\0';
    g_pause_until = -1;
    g_step_wait = PS_WAIT_NONE;
    ps_clear_decor_overlays();

    glr_tour_snapshot_restore(g_baseline);
    glr_ctrl_after_tour_restore();

    g_frame = 0;
    g_next_event = 0;
    g_current_event = -1;
    g_completed_events = 0;
    g_frame_credit = 0.0f;
    ps_tour_seed_pointer();
    g_seek_target = target;
}

/* Resolve an event's point WITHOUT the tour-stopping side effect of
 * ps_fire_point: overlay reconstruction is best-effort, so an unresolvable
 * target just skips that overlay. Literal points pass through. */
static int ps_resolve_event_point(const PsEvent *ev, float *x, float *y) {
    int mx, my;
    if (!ev->target[0]) {
        *x = (float)ev->x;
        *y = (float)ev->y;
        return 1;
    }
    if (glr_pointer_script_resolve_target(ev->target, &mx, &my)) {
        *x = (float)mx;
        *y = (float)my;
        return 1;
    }
    return 0;
}

/* Frames an event occupies in normal playback before the next one fires — its
 * intrinsic blocking wait. Used to reconstruct the virtual timeline during a
 * seek so a still-live caption lands at the right age. Approximate (it ignores
 * the one-frame completion slop between events), which is fine: captions run
 * seconds, the slop is sub-frame-per-event. */
static int ps_event_playback_cost(const PsEvent *ev) {
    switch (ev->verb) {
    case PS_GLIDE:
    case PS_RING:
    case PS_PAUSE:
        return ev->dur_frames > 0 ? ev->dur_frames : 1;
    case PS_CLICK:
    case PS_RIGHTCLICK:
        return PS_CLICK_RELEASE_FRAMES;
    case PS_KEY:
        if (ev->cps > 0.0f) {
            int len = (int)strlen(ev->text);
            int f = (int)((float)(len > 1 ? len - 1 : 0) * PS_FPS /
                          ev->cps + 0.5f);
            return f > 1 ? f : 1;
        }
        return 1;
    default:
        return 1;   /* move / wheel / key / skey / chord / down / up / echo */
    }
}

/* After a backstep settles, restore the decorative overlays normal playback
 * would STILL be showing at the landing boundary — chiefly a caption (echo)
 * whose multi-second window spans the events after it, which the plain
 * suppress-the-prefix rule would have dropped. Reconstructs the virtual
 * timeline over [0, target), sets g_frame to the landing time, and re-creates
 * the last echo / click-ripple still within its lifetime; a ring landed on
 * (the final replayed event) shows fresh as "you are here" context.
 *
 * Single-slot semantics match live playback: only the most-recent echo/ripple
 * can show, and only while its own window covers the landing frame. */
static void ps_tour_restore_landing_overlays(int target) {
    int sim = 0;
    int echo_fire = -1, echo_idx = -1;
    int ripple_fire = -1, ripple_idx = -1;
    float x, y;

    if (target > g_event_count)
        target = g_event_count;
    for (int i = 0; i < target; i++) {
        const PsEvent *ev = &g_events[i];
        if (ev->verb == PS_ECHO) { echo_fire = sim; echo_idx = i; }
        if (ev->verb == PS_CLICK || ev->verb == PS_RIGHTCLICK ||
            ev->verb == PS_DOWN)  { ripple_fire = sim; ripple_idx = i; }
        sim += ps_event_playback_cost(ev);
    }
    /* Anchor the frozen virtual clock at the landing time so every restored
     * overlay's age (g_frame - start) reflects real elapsed playback. */
    g_frame = sim;

    if (echo_idx >= 0 && sim - echo_fire < g_events[echo_idx].dur_frames &&
        ps_resolve_event_point(&g_events[echo_idx], &x, &y)) {
        const PsEvent *ev = &g_events[echo_idx];
        g_echo_start = echo_fire;
        g_echo_dur = ev->dur_frames;
        g_echo_x = x;
        g_echo_y = y;
        g_echo_size = ev->size;
        snprintf(g_echo_text, sizeof(g_echo_text), "%s", ev->text);
    }
    if (ripple_idx >= 0 && sim - ripple_fire < PS_RIPPLE_FRAMES &&
        ps_resolve_event_point(&g_events[ripple_idx], &x, &y)) {
        g_ripple_frame = ripple_fire;
        g_ripple_x = x;
        g_ripple_y = y;
    }
    /* A ring is a blocking beat (its window closes exactly as it completes), so
     * it is never "still live" at a boundary — but landing directly on one
     * should still highlight what it points at, shown fresh. */
    if (target > 0 && g_events[target - 1].verb == PS_RING &&
        ps_resolve_event_point(&g_events[target - 1], &x, &y)) {
        const PsEvent *ev = &g_events[target - 1];
        g_ring_start = sim;
        g_ring_dur = ev->dur_frames;
        g_ring_x = x;
        g_ring_y = y;
    }
}

/* Resumable prefix replay: fast-execute up to PS_SEEK_EVENTS_PER_FRAME events,
 * suppressing all decoration during the sweep (ps_tour_restore_landing_overlays
 * re-creates whatever is still live once the seek settles). A shell: click
 * schedules an async DOM callback, so yield the frame after one and resume next
 * frame. Advances no scene/application time. */
static void ps_tour_seek_step(void) {
    int processed = 0;
    while (g_active && g_next_event < g_seek_target &&
           processed < PS_SEEK_EVENTS_PER_FRAME) {
        int idx = g_next_event++;
        const PsEvent *ev = &g_events[idx];
        ps_finish_event_immediate(ev, /*allow_overlays=*/0);
        g_completed_events++;
        processed++;
#if defined(__EMSCRIPTEN__)
        if (ev->verb == PS_CLICK && strncmp(ev->target, "shell:", 6) == 0)
            return;   /* yield to the browser event loop; resume next frame */
#endif
    }
    if (!g_active)
        return;
    if (g_next_event >= g_seek_target) {
        int target = g_seek_target;
        g_current_event = -1;
        g_step_wait = PS_WAIT_NONE;
        g_seek_target = -1;
        ps_tour_camera_reconstruction_end();
        /* Re-create the overlays live playback would still show here (the
         * still-live caption the user rewound expecting to see). */
        ps_tour_restore_landing_overlays(target);
        if (g_completed_events >= g_event_count)
            ps_tour_enter_done();
        else
            g_tour_state = GLR_TOUR_PAUSED;
    }
}

/* Right Arrow: force the in-flight event complete, or execute the next event
 * immediately when between events; then pause (or enter Done at the end). */
static void ps_tour_step_forward(void) {
    switch (g_tour_state) {
    case GLR_TOUR_PLAYING:
    case GLR_TOUR_PAUSED:
        break;
    case GLR_TOUR_DONE:
        repl_set_status("Tour: at end");
        return;
    default:   /* baseline pending, seeking, stepping, off: consume, no-op */
        return;
    }
    /* Drop the prior step's frozen ring/echo/ripple before advancing; only the
     * event we land on (executed below) re-creates its own overlay. */
    ps_clear_decor_overlays();
    if (g_current_event >= 0) {
        ps_force_current_complete();
        g_completed_events++;
        g_current_event = -1;
        g_step_wait = PS_WAIT_NONE;
    } else if (g_next_event < g_event_count) {
        const PsEvent *ev = &g_events[g_next_event++];
        ps_finish_event_immediate(ev, 1);
        g_completed_events++;
    }
    if (!g_active)
        return;
    if (g_completed_events >= g_event_count)
        ps_tour_enter_done();
    else
        g_tour_state = GLR_TOUR_PAUSED;
}

/* Left Arrow: compute the backstep target (plan's formula) and seek to it. */
static void ps_tour_backstep(void) {
    int target;
    if (g_tour_state == GLR_TOUR_DONE) {
        target = g_event_count > 0 ? g_event_count - 1 : 0;
    } else if (g_tour_state == GLR_TOUR_PLAYING ||
               g_tour_state == GLR_TOUR_PAUSED) {
        if (g_current_event >= 0)
            target = g_completed_events;
        else
            target = g_completed_events > 0 ? g_completed_events - 1 : 0;
    } else {
        return;   /* baseline pending, seeking, stepping, off: consume, no-op */
    }
    ps_tour_begin_seek(target);
}

/* Speed ladder step, clamped. Affects only pointer-script timing. */
static void ps_tour_speed_adjust(int dir) {
    g_speed_idx += dir;
    if (g_speed_idx < 0)
        g_speed_idx = 0;
    if (g_speed_idx >= PS_TOUR_SPEED_COUNT)
        g_speed_idx = PS_TOUR_SPEED_COUNT - 1;
}

/* Done -> restart: restore the baseline (a seek to target 0, no prefix) and
 * replay from the start at the current speed. */
static void ps_tour_restart(void) {
    ps_tour_begin_seek(0);
    if (g_tour_state == GLR_TOUR_SEEKING) {   /* baseline existed */
        g_seek_target = -1;
        ps_tour_camera_reconstruction_end();
        g_tour_state = GLR_TOUR_PLAYING;
        g_frame_credit = 0.0f;
    }
}

/* Space: pause without completing the current event, resume, or (from Done)
 * restore the baseline and replay from the start. No-op in the transient
 * baseline-pending / seeking / stepping states. */
static void ps_tour_space(void) {
    switch (g_tour_state) {
    case GLR_TOUR_PLAYING:
        g_tour_state = GLR_TOUR_PAUSED;
        break;
    case GLR_TOUR_PAUSED:
        g_tour_state = GLR_TOUR_PLAYING;
        g_frame_credit = 0.0f;
        break;
    case GLR_TOUR_DONE:
        ps_tour_restart();
        break;
    default:
        break;
    }
}

/* Per-rendered-frame driver for a controlled tour: capture the baseline on the
 * first frame, spend virtual-frame credit while Playing, or step a seek. */
static void ps_tour_frame(void) {
    /* The previous seek-completion frame has now been presented. A live seek
     * below reasserts this before controller ticking for the current frame. */
    g_suppress_app_tick = 0;
    switch (g_tour_state) {
    case GLR_TOUR_BASELINE_PENDING:
        /* The Tours menu close path has finished; capture now, seed the
         * pointer, enter Playing — but wait until the next frame to fire
         * event zero (do not advance a virtual frame this frame). */
        g_baseline = glr_tour_snapshot_capture();
        if (!g_baseline) {
            glr_pointer_script_stop();
            repl_set_status_error("Tour could not capture rewind state");
            return;
        }
        ps_tour_seed_pointer();
        g_frame_credit = 0.0f;
        g_tour_state = GLR_TOUR_PLAYING;
        break;
    case GLR_TOUR_PLAYING:
        g_frame_credit += k_tour_speeds[g_speed_idx];
        while (g_active && g_tour_state == GLR_TOUR_PLAYING &&
               g_frame_credit >= 1.0f) {
            ps_tour_advance_one_virtual_frame();
            g_frame_credit -= 1.0f;
        }
        break;
    case GLR_TOUR_SEEKING:
        g_suppress_app_tick = 1;
        ps_tour_seek_step();
        break;
    case GLR_TOUR_DONE:
        ps_tour_advance_done();
        break;
    case GLR_TOUR_PAUSED:
    case GLR_TOUR_STEPPING:
    case GLR_TOUR_OFF:
    default:
        break;
    }
}

GlrTourPlaybackView glr_pointer_script_tour_view(void) {
    GlrTourPlaybackView v;
    memset(&v, 0, sizeof(v));
    if (g_run_kind != PS_RUN_CONTROLLED_TOUR) {
        v.state = GLR_TOUR_OFF;
        v.current_event = -1;
        v.source_line = -1;
        return v;
    }
    v.active = 1;
    v.state = g_tour_state;
    v.name = g_tour_name;
    v.file = g_tour_file;
    v.hud_expanded = g_tour_hud_expanded;
    v.speed = k_tour_speeds[g_speed_idx];
    v.completed_events = g_completed_events;
    v.current_event = g_current_event;
    v.total_events = g_event_count;
    v.source_line = ps_tour_hud_source_line();
    return v;
}

int glr_pointer_script_toggle_tour_hud(void) {
    if (g_run_kind != PS_RUN_CONTROLLED_TOUR)
        return 0;
    g_tour_hud_expanded = !g_tour_hud_expanded;
    return 1;
}

int glr_pointer_script_tour_suppresses_app_tick(void) {
    return g_active && g_run_kind == PS_RUN_CONTROLLED_TOUR &&
           g_suppress_app_tick;
}

int glr_pointer_script_handle_tour_key(unsigned char key) {
    if (g_run_kind != PS_RUN_CONTROLLED_TOUR)
        return 0;
    switch (key) {
    case ' ':
        ps_tour_space();
        return 1;
    case '+':
    case '=':
        ps_tour_speed_adjust(+1);
        return 1;
    case '-':
    case '_':
        ps_tour_speed_adjust(-1);
        return 1;
    case 27:   /* Esc: stop, preserving the current app result */
        glr_pointer_script_stop();
        repl_set_status("Tour stopped");
        return 1;
    default:
        return 0;   /* fall through to the tour-cancel intercept */
    }
}

int glr_pointer_script_handle_tour_special(int key) {
    if (g_run_kind != PS_RUN_CONTROLLED_TOUR)
        return 0;
    if (key == GLUT_KEY_LEFT) {
        ps_tour_backstep();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        ps_tour_step_forward();
        return 1;
    }
    return 0;
}

void glr_pointer_script_frame(void) {
    if (!g_active) return;
    if (g_run_kind == PS_RUN_CONTROLLED_TOUR) {
        ps_tour_frame();
        return;
    }
    ps_advance_one_virtual_frame();
}

/* --- overlay rendering -------------------------------------------------- */

static void ps_circle(float cx, float cy, float radius, int filled) {
    int i;
    glBegin(filled ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
    if (filled) glVertex2f(cx, cy);
    for (i = 0; i <= 40; i++) {
        float a = (float)i * (float)(2.0 * M_PI / 40.0);
        glVertex2f(cx + cosf(a) * radius, cy + sinf(a) * radius);
    }
    glEnd();
}

/* A caption's chosen bitmap font plus the vertical extents the backing
 * plate needs. Ascent/descent are carried in the ladder rather than asked
 * of GLUT: glutBitmapHeight is a freeglut extension, absent from Apple's
 * GLUT framework (the USE_GLUT build), and one height would not separate
 * above-baseline from below-baseline anyway. */
typedef struct {
    void  *font;
    float  ascent;   /* baseline -> top of tall glyphs, px */
    float  descent;  /* baseline -> bottom of descenders, px */
} PsEchoFont;

/* Echo captions render in a fixed-size GLUT bitmap font (bitmap glyphs
 * don't scale, so the script's requested cap height only selects the
 * nearest font from this ladder). */
static PsEchoFont ps_echo_font(float cap_px) {
    static const struct { float px; PsEchoFont f; } k_fonts[] = {
        { 10.0f, { GLUT_BITMAP_HELVETICA_10,    8.0f, 2.0f } },
        { 12.0f, { GLUT_BITMAP_HELVETICA_12,   10.0f, 3.0f } },
        { 18.0f, { GLUT_BITMAP_HELVETICA_18,   14.0f, 4.0f } },
        { 24.0f, { GLUT_BITMAP_TIMES_ROMAN_24, 18.0f, 6.0f } },
    };
    size_t best = 0;
    float best_d = -1.0f;
    for (size_t i = 0; i < sizeof(k_fonts) / sizeof(k_fonts[0]); i++) {
        float d = fabsf(cap_px - k_fonts[i].px);
        if (best_d < 0.0f || d < best_d) { best_d = d; best = i; }
    }
    return k_fonts[best].f;
}

/* Draw one byte span with its baseline left edge at (x, y) in window pixels
 * (2D ortho is already active). */
static void ps_bitmap_text(float x, float y, void *font, const char *s,
                           size_t len) {
    glRasterPos2f(x, y);
    for (size_t i = 0; i < len; i++)
        glutBitmapCharacter(font, (unsigned char)s[i]);
}

/* Total advance width of a byte span in the given bitmap font, in pixels.
 * Summed per glyph via glutBitmapWidth (core GLUT; the no-op stubs supply
 * it too) so it stays portable across the real and stubbed builds. */
static float ps_bitmap_width(void *font, const char *s, size_t len) {
    float w = 0.0f;
    for (size_t i = 0; i < len; i++)
        w += (float)glutBitmapWidth(font, (unsigned char)s[i]);
    return w;
}

/* Classic pointer arrow at the current position. Local coords are y-down
 * (mouse space); px/py convert to the gl2d y-up ortho at the call site. */
static void ps_draw_cursor(float px, float py) {
    /* Arrow outline, y-down, ~19px tall: tip, left edge down, notch,
     * tail right side, back up the hypotenuse. */
    static const float pts[][2] = {
        { 0.0f,  0.0f }, { 0.0f, 16.5f }, { 3.9f, 12.9f }, { 6.6f, 19.0f },
        { 9.3f, 17.8f }, { 6.6f, 11.9f }, { 11.9f, 11.9f },
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));
    int i;

    /* Fill: the arrow polygon is star-shaped from a point inside the
     * head, so a triangle fan anchored there fills it correctly. */
    glColor4f(0.97f, 0.97f, 0.98f, 0.96f);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(px + 2.6f, py - 8.5f);
    for (i = 0; i < n; i++)
        glVertex2f(px + pts[i][0], py - pts[i][1]);
    glVertex2f(px + pts[0][0], py - pts[0][1]);
    glEnd();

    /* Dark outline so the cursor reads on light and dark panels alike. */
    glColor4f(0.08f, 0.09f, 0.11f, 0.9f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    for (i = 0; i < n; i++)
        glVertex2f(px + pts[i][0], py - pts[i][1]);
    glEnd();
}

float glr_pointer_script_echo_alpha(int age, int dur, int clock_frozen) {
    if (clock_frozen)
        return 1.0f;               /* paused inspection view: fully readable */
    float alpha = 1.0f;
    if (age < 9) alpha *= (float)age / 9.0f;          /* ease in  ~0.15s */
    int left = dur - age;
    if (left < 30) alpha *= (float)left / 30.0f;      /* ease out ~0.5s  */
    if (alpha < 0.0f) alpha = 0.0f;
    return alpha;
}

void glr_pointer_script_render_overlay(int win_w, int win_h) {
    if (!g_active) return;

    /* This pass owns its own profile row rather than leaving the host to
     * bracket it: the caption's bitmap text can cost more than the entire 3D
     * scene, and the bracket belongs with the code that spends the time. Opened
     * after the inactive guard above, so the row stays honestly stale outside a
     * tour instead of reporting a per-frame zero. */
    prof_begin(PROF_TOUR_OVERLAY);

    /* When a controlled tour's virtual clock is frozen (anything but Playing —
     * paused, done, stepped, or settled after a backstep), the age-driven
     * ease-in/out can't advance: a caption caught mid-fade would hang half-
     * transparent forever, and a backstep can land it at a tiny age. A paused
     * tour is an inspection view, so render captions at full alpha then. Env-
     * capture always advances its clock, so its fades are untouched. */
    int clock_frozen = (g_run_kind == PS_RUN_CONTROLLED_TOUR &&
                        g_tour_state != GLR_TOUR_PLAYING &&
                        g_tour_state != GLR_TOUR_DONE);

    /* The overlay pass runs after glr_ctrl_display_frame, mirroring
     * splash_render: 2D ortho, blending on, y flipped from mouse space. */
    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Unlike the panels, everything drawn here is a curve or a diagonal — ring,
     * ripple, cursor outline — so this overlay opts back into the antialiasing
     * gl2d_begin() turns off for pixel-aligned chrome. gl2d_end() pops it. */
    glEnable(GL_LINE_SMOOTH);

    /* Highlight ring: two concentric pulsing circles in warm amber. */
    if (g_ring_start >= 0 && g_frame - g_ring_start < g_ring_dur) {
        float age = (float)(g_frame - g_ring_start);
        float pulse = 0.5f + 0.5f * sinf(age * (float)(2.0 * M_PI) / 45.0f);
        float alpha = 0.35f + 0.4f * pulse;
        /* Ease out over the last half second. */
        float left = (float)g_ring_dur - age;
        if (left < 30.0f) alpha *= left / 30.0f;
        float cy = (float)win_h - g_ring_y;
        glLineWidth(2.5f);
        glColor4f(1.0f, 0.72f, 0.30f, alpha);
        ps_circle(g_ring_x, cy, 17.0f + 2.0f * pulse, 0);
        glColor4f(1.0f, 0.72f, 0.30f, alpha * 0.5f);
        ps_circle(g_ring_x, cy, 23.0f + 3.0f * pulse, 0);
    }

    /* Click ripple: one expanding, fading circle at the press point. */
    if (g_ripple_frame >= 0 && g_frame - g_ripple_frame < PS_RIPPLE_FRAMES) {
        float t = (float)(g_frame - g_ripple_frame) / (float)PS_RIPPLE_FRAMES;
        glLineWidth(2.0f);
        glColor4f(0.55f, 0.78f, 1.0f, 0.8f * (1.0f - t));
        ps_circle(g_ripple_x, (float)win_h - g_ripple_y,
                  4.0f + 18.0f * t, 0);
    }

    /* Echo caption: bitmap text (e.g. "Ctrl+K") pinned at a screen spot to
     * label how the next action was triggered. A translucent dark plate goes
     * down first so the glyphs read over any scene, then the caption is drawn
     * once on top; the whole thing eases in/out together.
     *
     * The plate replaced an eight-pass 1px glyph outline, and the reason is
     * performance, not taste. Apple's GL rasterizes tall bitmap glyphs ~30x
     * slower per glyph than the short fonts the rest of the app uses (measured
     * 337 glyphs: Times-24 ~12 ms vs Helvetica-12 ~0.4 ms; the cliff is around
     * 16 glyph rows), and tours request 24 px. Nine passes over a ~35-char
     * caption was therefore ~10 ms of CPU plus ~4 ms of extra glFinish drain
     * every frame a caption was up — more than the whole 3D scene, and enough
     * to miss the 60 Hz deadline on its own. One pass over a plate costs a
     * ninth of that and gives *better* contrast over busy geometry. Keep it to
     * one text pass: any per-glyph multiplier here is paid at that rate. */
    if (g_echo_start >= 0 && g_frame - g_echo_start < g_echo_dur &&
        g_echo_text[0]) {
        /* Plate padding around the glyph box, px. Asymmetric on purpose: the
         * ladder's ascent covers tall glyphs, so the extra room reads better
         * split toward the sides. */
        const float pad_x = 10.0f, pad_y = 5.0f;
        float alpha = glr_pointer_script_echo_alpha(g_frame - g_echo_start,
                                                    g_echo_dur, clock_frozen);
        PsEchoFont font = ps_echo_font(g_echo_size);
        float cy = (float)win_h - g_echo_y;
        float w = 0.0f;
        int line_count = 1;
        const char *line = g_echo_text;
        for (const char *p = g_echo_text;; p++) {
            if (*p == '\n' || *p == '\0') {
                float line_w = ps_bitmap_width(font.font, line,
                                               (size_t)(p - line));
                if (line_w > w) w = line_w;
                if (*p == '\0') break;
                line_count++;
                line = p + 1;
            }
        }
        /* Center the caption horizontally on its anchor point rather than
         * left-aligning at it, so the label sits symmetrically over the
         * target it annotates. Keep its raster origin inside the viewport:
         * legacy GL invalidates a raster position outside the clip volume,
         * which silently drops the entire bitmap string. If a caption is
         * wider than the viewport, preserve a valid left edge and let GL clip
         * only the tail instead of losing every glyph. */
        float cx = g_echo_x - w * 0.5f;
        float max_cx = (float)win_w - pad_x - w;
        if (cx < pad_x) cx = pad_x;
        if (max_cx >= pad_x && cx > max_cx) cx = max_cx;

        /* Center a multiline block vertically around the old single-line
         * baseline, then keep every line's raster origin in the viewport too. */
        float line_step = font.ascent + font.descent + 4.0f;
        float first_cy = cy + (float)(line_count - 1) * line_step * 0.5f;
        float plate_bottom = first_cy - (float)(line_count - 1) * line_step -
                             font.descent - pad_y;
        float plate_top = first_cy + font.ascent + pad_y;
        if (plate_bottom < 0.0f) first_cy -= plate_bottom;
        plate_top = first_cy + font.ascent + pad_y;
        if (plate_top > (float)win_h) first_cy -= plate_top - (float)win_h;
        plate_bottom = first_cy - (float)(line_count - 1) * line_step -
                       font.descent - pad_y;
        plate_top = first_cy + font.ascent + pad_y;

        glColor4f(0.06f, 0.07f, 0.09f, alpha * 0.72f);
        glBegin(GL_QUADS);
        glVertex2f(cx - pad_x,     plate_bottom);
        glVertex2f(cx + w + pad_x, plate_bottom);
        glVertex2f(cx + w + pad_x, plate_top);
        glVertex2f(cx - pad_x,     plate_top);
        glEnd();

        glColor4f(0.98f, 0.98f, 0.99f, alpha);
        line = g_echo_text;
        for (int row = 0; row < line_count; row++) {
            const char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line) : strlen(line);
            float line_w = ps_bitmap_width(font.font, line, len);
            ps_bitmap_text(cx + (w - line_w) * 0.5f,
                           first_cy - (float)row * line_step,
                           font.font, line, len);
            if (!end) break;
            line = end + 1;
        }
    }

    ps_draw_cursor(g_px, (float)win_h - g_py);

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    gl2d_end();
    prof_end(PROF_TOUR_OVERLAY);
}
