/*
 * keymap.h - Single source of truth for keyboard shortcut bindings.
 *
 * Each user-facing action is ONE macro that expands to a `key, mods`
 * pair, so a binding is reassigned HERE without hunting through the
 * dispatch sites that consume it (the g_cfg_items[] table in
 * glr_actions.c, the glr_ctrl_router_* handlers, and the editor input
 * dispatcher).
 *
 *   #define GLR_<ACTION>   <key>, <mods>
 *
 *     <key>   a KEY_CTRL_* / ASCII byte (keys.h) for the normal keyboard
 *             callback, or a GLUT_KEY_* special code for the special
 *             callback.
 *     <mods>  extra GLUT_ACTIVE_* bits required BEYOND the modifier the
 *             key already implies (Ctrl is implicit in a KEY_CTRL_* byte);
 *             0 = none.
 *
 * Call sites never spell out modifiers — they pass the whole pair to the
 * matcher, which folds in the live modifier state in one place:
 *
 *     if (keymap_event_is(key, GLR_CODE_FOCUS)) { ... }
 *         // expands to keymap_event_is(key, KEY_CTRL_F, GLUT_ACTIVE_SHIFT)
 *
 * keymap_event_is(event_key, binding_key, binding_mods) is an EXACT match:
 *   - returns 0 unless event_key == binding_key;
 *   - the held modifiers must equal binding_mods, AFTER normalizing away
 *     the modifier the key already implies — a KEY_CTRL_* byte carries
 *     Ctrl (and on macOS the Cmd/SUPER alias). So binding_mods == 0 means
 *     strictly "no extra modifiers": a Shift binding and its plain twin on
 *     the same key never alias, and the dispatch ORDER of two bindings on
 *     one key no longer matters. (Ctrl is dropped from the held set unless
 *     the binding requires it, e.g. the Ctrl+Arrow audio bindings; Shift
 *     is always significant.)
 * A handler that wants a key with-or-without a modifier and branches on it
 * itself (Ctrl+Z vs Ctrl+Shift+Z) matches the bare key via KM_KEY()
 * instead, NOT this matcher.
 * It reads the live modifiers through the editor's modifier accessor, so
 * it is implemented in src/editor/input.c (next to that accessor) and
 * declared here so every dispatcher — including subsystems that don't
 * include editor headers — can call it.
 *
 * Because a binding is a comma pair it cannot be a `case` label; the one
 * switch on a binding (replay's Ctrl+K) lifts it to an `if`. The cfg
 * table (g_cfg_items[] in glr_actions.c) splits the pair into its
 * .key_code / .modifiers fields with KM_KEY() / KM_MODS().
 *
 * Dependency contract (mirrors config.h's FONT_* = GLUT_BITMAP_* macros):
 * the KEY_CTRL_* / KEY_ESC codes (keys.h) and the GLUT_KEY_* /
 * GLUT_ACTIVE_* codes (GLUT) appear only inside macro BODIES, so nothing
 * resolves until a dispatch site uses a binding — and every such site
 * already has keys.h and the GLUT headers in scope. keymap.h itself
 * includes NOTHING and is safe to pull in everywhere: config.h includes
 * it, so it reaches every TU; TUs that never reference a binding pay
 * nothing, and standalone tools that avoid keys.h (e.g. tools/editor_demo)
 * are not forced to take it on. Reassigning a binding ACROSS callback
 * classes (an F-key to a Ctrl-letter or vice versa) still needs the
 * consuming site to move between the normal and special callbacks / flip
 * g_cfg_items[].is_special — the pair alone doesn't carry that.
 *
 * Modifier pairs: several keys host a plain-Ctrl action and a Ctrl+Shift
 * action on the same byte (the matcher disambiguates by mods). They are
 * paired in comments below so the overlap stays visible.
 *
 * Lives at the repo root beside config.h / prof_sections.h (the
 * force-included project-config headers); the keys.h-style zero-dependency
 * portability headers live in include/. The GLR_ prefix denotes the
 * gl-repl application's bindings, not app-layer ownership — any layer may
 * reference these.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

/* No includes on purpose — see the dependency contract above. */

/* ---- Special-key (function-key) bindings ----------------------------- *
 * Delivered to the GLUT special callback. The F2..F9 config cycles read
 * Shift as direction (forward / backward) in the cfg dispatcher, not as a
 * binding requirement, so their mods are 0. F10 (below) follows the same
 * "mods always 0, Shift read live" idiom via the cfg dispatcher. */
#define GLR_HELP             GLUT_KEY_F1, 0    /* toggle help overlay */
#define GLR_POST_FX_SCOPE_CYCLE GLUT_KEY_F10, 0  /* cycle Post FX Scope; Shift+F10 = backward (via config table) */
#define GLR_PREV_EXAMPLE     GLUT_KEY_F12, GLUT_ACTIVE_SHIFT   /* previous example / scene */
#define GLR_NEXT_EXAMPLE     GLUT_KEY_F12, 0   /* next example / scene */
#define GLR_EXPORT_PLY       GLUT_KEY_F11, 0   /* export geometry to output.ply */
#define GLR_AUDIO_PREV       GLUT_KEY_LEFT,  GLUT_ACTIVE_CTRL  /* prev track */
#define GLR_AUDIO_NEXT       GLUT_KEY_RIGHT, GLUT_ACTIVE_CTRL  /* next track */

/* ---- App action bindings (normal keyboard callback) ------------------ */
#define GLR_SAVE             KEY_CTRL_S, 0     /* save to output.c */
#define GLR_QUIT             KEY_CTRL_Q, 0     /* save + quit */
#define GLR_DEBUG_DUMP       KEY_CTRL_D, GLUT_ACTIVE_SHIFT   /* dump state */
#define GLR_CONFIG_MENU      KEY_CTRL_K, GLUT_ACTIVE_SHIFT   /* open Config dropdown */
#define GLR_ESCAPE           KEY_ESC, 0        /* clear input / close overlay */
#define GLR_CODE_FOCUS       KEY_CTRL_F, GLUT_ACTIVE_SHIFT  /* pairs w/ Search (plain Ctrl+F) */
#define GLR_AUDIO            KEY_CTRL_A, GLUT_ACTIVE_SHIFT  /* play/pause; plain Ctrl+A = Line start */

/* ---- Config-row bindings (g_cfg_items[] in glr_actions.c) ------------- *
 * Ctrl-letter rows that double up with an editor / app action carry
 * GLUT_ACTIVE_SHIFT; the paired plain-Ctrl owner is noted alongside. */
#define GLR_GRID             GLUT_KEY_F2, 0
#define GLR_GRID_EXTENT      GLUT_KEY_F3, 0
#define GLR_GRID_BRIGHTNESS  GLUT_KEY_F4, 0
#define GLR_BACKDROP         GLUT_KEY_F5, 0
#define GLR_AXES             GLUT_KEY_F6, 0
#define GLR_VERTEX_LABELS    GLUT_KEY_F7, 0
#define GLR_OVERLAY_SCOPE   GLUT_KEY_F8, 0
#define GLR_LIGHT_THEME      GLUT_KEY_F9, 0
#define GLR_CODE_PANEL       KEY_CTRL_B, 0
#define GLR_RESET_CAMERA     KEY_CTRL_C, GLUT_ACTIVE_SHIFT  /* plain Ctrl+C = Copy */
#define GLR_WIREFRAME        KEY_CTRL_G, 0
#define GLR_GRID_MAJOR       KEY_CTRL_G, GLUT_ACTIVE_SHIFT
#define GLR_LIGHT_INDICATORS KEY_CTRL_L, GLUT_ACTIVE_SHIFT  /* plain Ctrl+L = Clear all */
#define GLR_NORMAL_VECTORS   KEY_CTRL_N, GLUT_ACTIVE_SHIFT  /* plain Ctrl+N is unbound */
#define GLR_FOCUS_ORIGIN     KEY_CTRL_O, 0
#define GLR_VERTEX_OUTLINES  KEY_CTRL_O, GLUT_ACTIVE_SHIFT
#define GLR_POLY_HIGHLIGHT   KEY_CTRL_P, 0
#define GLR_VERTEX_POINTS    KEY_CTRL_P, GLUT_ACTIVE_SHIFT
#define GLR_REPLAY           KEY_CTRL_R, 0     /* pairs w/ Camera rotate */
#define GLR_CAMERA_ROTATE    KEY_CTRL_R, GLUT_ACTIVE_SHIFT  /* plain Ctrl+R = Replay */
#define GLR_SPLIT_DECL       KEY_CTRL_S, GLUT_ACTIVE_SHIFT  /* plain Ctrl+S = Save */
#define GLR_AUTO_TIME        KEY_CTRL_T, 0
#define GLR_WINDING_VIEW     KEY_CTRL_B, GLUT_ACTIVE_SHIFT
#define GLR_MSAA             KEY_CTRL_U, 0
#define GLR_ACCUM_EFFECT     KEY_CTRL_U, GLUT_ACTIVE_SHIFT  /* Shift toggles accum-AA (read in handler) */
#define GLR_VIEW_MODE        KEY_CTRL_V, GLUT_ACTIVE_SHIFT  /* plain Ctrl+V = Paste */
#define GLR_CPU_PROFILE      KEY_CTRL_W, 0     /* pairs w/ Memory profile */
#define GLR_MEMORY_PROFILE   KEY_CTRL_W, GLUT_ACTIVE_SHIFT  /* plain Ctrl+W = CPU profile */
#define GLR_XFORM_GUIDES     KEY_CTRL_X, GLUT_ACTIVE_SHIFT
#define GLR_SYNTAX_HL        KEY_CTRL_Y, GLUT_ACTIVE_SHIFT  /* plain Ctrl+Y = Redo */
#define GLR_VARIABLE_PANEL   '`', 0            /* open variable panel */

/* ---- Editor text-editing bindings (src/editor/input.c) --------------- *
 * Plain Ctrl bytes; the Ctrl+Shift twin of several is a config row above,
 * claimed first by the cfg dispatcher (so these stay mods-agnostic). */
#define GLR_LINE_START       KEY_CTRL_A, 0     /* pairs w/ Audio play/pause */
#define GLR_LINE_END         KEY_CTRL_E, 0     /* pairs w/ Vertex outlines */
#define GLR_COPY             KEY_CTRL_C, 0     /* pairs w/ Reset camera */
#define GLR_CUT              KEY_CTRL_X, 0
#define GLR_PASTE            KEY_CTRL_V, 0     /* pairs w/ View mode */
#define GLR_DELETE_LINE      KEY_CTRL_D, 0
#define GLR_UNDO             KEY_CTRL_Z, 0     /* Shift -> redo (read in handler) */
#define GLR_REDO             KEY_CTRL_Y, 0     /* pairs w/ Syntax highlight (Ctrl+Shift+Y) */
#define GLR_CLEAR_ALL        KEY_CTRL_L, 0     /* pairs w/ Light indicators */
#define GLR_REFORMAT         KEY_CTRL_BACKSLASH, 0
#define GLR_SEARCH           KEY_CTRL_F, 0     /* pairs w/ Code focus (Ctrl+Shift+F) */
#define GLR_REPLAY_JUMP      KEY_CTRL_K, 0     /* jump replay PC to cursor */

/* Not registered here on purpose: the accum-AA sample fine-adjust is a
 * multi-key +/- gesture ('='/'+' up, '-'/Ctrl+- down), not a single named
 * binding — it stays inline in glr_ctrl_router_handle_accum_samples_key.
 * The Accum AA cycle itself is GLR_ACCUM_AA (F2) above. */

/* Match the dispatched event against a binding pair. See the contract in
 * the file banner. Implemented in src/editor/input.c. */
int keymap_event_is(int event_key, int binding_key, int binding_mods);

/* User-facing shortcut labels. The caller owns `out`; each helper returns
 * `out` for convenience, or an empty string if out_size <= 0.
 *
 * Pass `is_special=1` for GLUT special-callback keys (`GLUT_KEY_*`), and
 * 0 for normal keyboard bytes (`KEY_CTRL_*`, printable ASCII, Escape).
 * The flag is required because GLUT_KEY_F1 (numeric 1) aliases KEY_CTRL_A
 * after preprocessing; keymap.h's textual tokens are distinct, but the
 * runtime integer pair is not.
 *
 * Examples:
 *   keymap_binding_to_string(buf, sizeof buf, KM_KEY(GLR_SAVE),
 *                            KM_MODS(GLR_SAVE), 0)          -> Ctrl+S
 *   keymap_binding_to_string(buf, sizeof buf, KM_KEY(GLR_PREV_EXAMPLE),
 *                            KM_MODS(GLR_PREV_EXAMPLE), 1)  -> Shift+F12
 *   keymap_binding_to_string(buf, sizeof buf, KM_KEY(GLR_AUDIO_PREV),
 *                            KM_MODS(GLR_AUDIO_PREV), 1)    -> Ctrl+Left
 */
#define KEYMAP_SHORTCUT_LABEL_MAX 32
const char *keymap_binding_to_string(char *out, int out_size,
                                     int binding_key, int binding_mods,
                                     int is_special);

/* Pull one element out of a binding pair, for the rare site that needs a
 * bare key/mods constant rather than a match: a `case` label (which can't
 * take a comma pair), a synthetic dispatch that feeds a key byte to a
 * handler, or a struct-initializer field (g_cfg_items[]). Generic over any
 * GLR_<ACTION> pair — not per-binding macros.
 *
 * The two-level form is load-bearing: a function-like macro fixes its
 * argument count from the commas present BEFORE its arguments expand, so
 * KM_KEY_(GLR_X) would see one argument, not two. Routing through KM_KEY()
 * first lets GLR_X expand to `key, mods` so KM_KEY_ then sees two. */
#define KM_KEY_(key, mods)  (key)
#define KM_MODS_(key, mods) (mods)
#define KM_KEY(binding)  KM_KEY_(binding)
#define KM_MODS(binding) KM_MODS_(binding)

#endif /* KEYMAP_H */
