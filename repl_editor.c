/*
 * repl_editor.c — Editor state, commit handlers, and GLUT input dispatch.
 *
 * Subsystems in this file (top to bottom):
 *  - Editor state (g_input, undo stack, clipboard, selection, camera
 *    inertia, config-item table)
 *  - Undo / redo snapshot ring
 *  - Cmd-range deletion with var-decl guards
 *  - Line-input load/save and line navigation
 *  - Commit handlers: float decl, assign, for, func, if, close-brace
 *  - Consolidated commit-helper chain (try_commit_any et al.)
 *  - GLUT keyboard / special / mouse / wheel / motion / timer callbacks
 *  - feed_line() — the programmatic commit entry point
 *  - Public repl_*_func() wrappers forwarded from sample.c
 *
 * Shared state (g_input, g_edit_line, g_num_cmds, g_cmds[], etc.) is
 * declared in sample.h. This file OWNS the editor-side globals listed
 * above; the parser/executor side lives in repl_core.c.
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_replay.h"
#include "repl_keys.h"
#include "ui_panels.h"
#include "repl_audio.h"

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */
static void save_newline_buf(void);
static int function_decl_insert_pos(void);
static int function_leading_comment_start(int pos);
void push_undo_snapshot(void);
void pop_undo_snapshot(void);
void do_redo(void);
void delete_cmd_range(int start, int count, const char *what);
int try_commit_float_decl(void);
int try_assign_variable(void);
int try_commit_for_loop(void);
int try_commit_func_def(void);
int try_commit_if_block(void);
int try_commit_close_brace(void);
static void keyboard_func(unsigned char key, int x, int y);
static void special_func(int key, int x, int y);
static int editor_code_panel_hidden(void);
static int editor_restore_hidden_code_panel(void);
static int editor_key_restores_hidden_code_panel(unsigned char key, int mods);
static int editor_special_restores_hidden_code_panel(int key, int mods);
static void mouse_func(int button, int state, int x, int y);
#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y);
#endif
static void passive_motion_func(int x, int y);
static void motion_func(int x, int y);
static void timer_func(int value);

char g_input[MAX_INPUT_LEN];
int  g_input_len = 0;
int  g_cursor_pos = 0;
int  g_edit_line = 0;
char g_newline_buf[MAX_INPUT_LEN] = "";
int  g_newline_len = 0;
int  g_inserting = 0;
static int g_func_decl_resume_delta = 0;

int g_mouse_x, g_mouse_y;
int g_mouse_btn = -1;
int g_mouse_mods = -1; /* Modifiers at the time of the last mouse event.
                          Can't call glutGetModifiers() from mouse outside
                          keyboard, special, or mouse callbacks */

static float g_vel_ry = 0.0f;
static float g_vel_rx = 0.0f;
static float g_vel_tx = 0.0f;
static float g_vel_ty = 0.0f;
static float g_vel_tz = 0.0f;
static float g_vel_zoom = 0.0f;

#define CAM_DECAY 0.88f
#define CAM_DECAY_ZOOM 0.65f
#define CAM_MOMENTUM_THRESHOLD 1.0f

float g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
int   g_resizing_panel = 0;
int   g_scroll = 0;
int   g_scroll_follow_cursor = 0;

int g_cursor_on = 1;
int g_blink_tick = 0;

int   g_show_var_panel = 1;
int   g_drag_var = -1;
int   g_drag_log_mode = 0;  /* 0=linear (LMB drag), 1=logarithmic (RMB drag) */
float g_drag_start_val = 0.0f;
int   g_drag_start_x = 0;


GLCmd g_clipboard[MAX_COMMANDS];
int   g_clipboard_count = 0;

int g_sel_anchor = -1;
int g_sel_end = -1;

static const char *replay_mode_names[] = { "Polygon", "Vertex" };
static const char *backdrop_mode_names[] = { "Off", "Cityscape" };
static const char *xform_guide_mode_names[] = { "World", "Frame" };
static const char *profile_panel_mode_names[] = { "Off", "On", "Details" };
static const char *code_panel_layout_names[] = {
    "Left", "Top", "Bottom", "Hidden"
};

/* Unified audio cfg: collapses mute + loop mode into one cycling
 * menu entry. Indices:
 *   0 = Mute    — muted, loop mode untouched
 *   1 = Once    — playing, loop mode OFF  (playlist plays through)
 *   2 = Song    — playing, loop mode SONG (repeat current track)
 *   3 = All     — playing, loop mode ALL  (playlist, wrap forever)
 * Default 3 matches repl_audio.c's LOOP_ALL default with volume on. */
#define AUDIO_CFG_PAUSE 0
#define AUDIO_CFG_ONCE  1
#define AUDIO_CFG_SONG  2
#define AUDIO_CFG_ALL   3
static int g_audio_cfg_mode = AUDIO_CFG_ALL;
static const char *audio_cfg_names[] = { "Pause", "Once", "Song", "All" };

/* Browser autoplay policy: the Web Audio context stays suspended until
 * a user gesture. We call repl_audio_on_user_gesture() the first time
 * a key or mouse event arrives; native builds make this a no-op. */
static int g_audio_gesture_sent = 0;
static ReplModifierProvider g_modifier_provider_for_test = NULL;

void repl_set_modifier_provider_for_test(ReplModifierProvider provider) {
    g_modifier_provider_for_test = provider;
}

static int editor_get_modifiers(void) {
    if (g_modifier_provider_for_test)
        return g_modifier_provider_for_test();
    return glutGetModifiers();
}

static void apply_audio_cfg_mode(int mode) {
    switch (mode) {
    case AUDIO_CFG_PAUSE:
        repl_audio_set_paused(1);
        break;
    case AUDIO_CFG_ONCE:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_OFF);
        break;
    case AUDIO_CFG_SONG:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_SONG);
        break;
    case AUDIO_CFG_ALL:
    default:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_ALL);
        break;
    }
}

CfgItem g_cfg_items[] = {
    { "### RENDERING",    0, 0,     NULL,                      0,                NULL              },
    { "MSAA",             KEY_CTRL_U, 0, &g_multisample_enabled,    2,                NULL              },
    { "Line smooth",      0, 0,     &g_line_smooth_enabled,    2,                NULL              },
    { "Accum AA",         0, 0,     &g_accum_aa_enabled,       2,                NULL              },
    { "Wireframe",        GLUT_KEY_F2, 1, &g_wireframe,              2,                NULL              },
    { "Point attenuation",0, 0,     &g_init_attenuate_points,  2,                NULL              },
    { "---",              0, 0,     NULL,                      0,                NULL              },
    { "### TIME & REPLAY",0, 0,     NULL,                      0,                NULL              },
    { "Auto time",        KEY_CTRL_T, 0, &g_t_playing,              2,                NULL              },
    { "Replay",           KEY_CTRL_R, 0, &g_replay_active,          2,                NULL              },
    { "Replay mode",      0, 0,   &g_replay_mode,            2,                replay_mode_names },
    { "Replay expand",    0, 0,     &g_replay_expand_args,     2,                NULL              },
    { "---",              0, 0,     NULL,                      0,                NULL              },
    { "### OVERLAYS & SCENE",0, 0,  NULL,                      0,                NULL              },
    { "Grid",             GLUT_KEY_F3, 1, &g_grid_theme,             GRID_THEME_COUNT, g_grid_names      },
    { "Grid major",       KEY_CTRL_O, 0, &g_grid_major_idx,         GRID_MAJOR_COUNT, g_grid_major_names  },
    { "Grid extent",      0, 0,     &g_grid_extent_idx,        GRID_EXTENT_COUNT, g_grid_extent_names },
    { "Axes",             GLUT_KEY_F4, 1, &g_axes_theme,             AXES_THEME_COUNT, g_axes_names      },
    { "Vertex guides",    GLUT_KEY_F8, 1, &g_show_guides,            2,                NULL              },
    { "Xform guide mode", 0, 0,     &g_xform_guide_mode,       2,                xform_guide_mode_names },
    { "Light indicators", GLUT_KEY_F10, 1, &g_show_lights,            2,                NULL              },
    { "Poly highlight",   0, 0,     &g_highlight_current_poly, 2,                NULL              },
    { "Backdrop",         0, 0,     &g_backdrop_mode,          2,                backdrop_mode_names },
    { "Camera rotate",    GLUT_KEY_F11, 1, &g_cam_rotate,             2,                NULL              },
    { "Auto-normals",     GLUT_KEY_F9, 1, &g_autonormal,             2,                NULL              },
    { "---",              0, 0,     NULL,                      0,                NULL              },
    { "### GEOMETRY",     0, 0,     NULL,                      0,                NULL              },
    { "Vertex labels",    GLUT_KEY_F5, 1, &g_show_vnums,             2,                NULL              },
    { "Normal vectors",   GLUT_KEY_F6, 1, &g_show_normals,           2,                NULL              },
    { "Vertex outlines",  GLUT_KEY_F7, 1, &g_show_outlines,          2,                NULL              },
    { "Vertex points",    0, 0,     &g_show_vpoints,           2,                NULL              },
    { "---",              0, 0,     NULL,                      0,                NULL              },
    { "### INTERFACE",    0, 0,     NULL,                      0,                NULL              },
    { "Variable panel",   0, 0,   &g_show_var_panel,         2,                NULL              },
    { "CPU profile",      KEY_CTRL_W, 0, &g_show_profile_panel,     PROFILE_PANEL_MODE_COUNT, profile_panel_mode_names },
    { "Code panel",       KEY_CTRL_B, 0, &g_code_panel_layout,      CODE_PANEL_LAYOUT_COUNT, code_panel_layout_names },
    { "Wrap at commas",   0, 0,     &g_wrap_at_comma,          2,                NULL              },
    { "---",              0, 0,     NULL,                      0,                NULL              },
    { "### AUDIO",        0, 0,     NULL,                      0,                NULL              },
    { "Audio",            0, 0,     &g_audio_cfg_mode,         4,                audio_cfg_names   },
};

const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items) / sizeof(g_cfg_items[0]));

#define UNDO_DEPTH 32

typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    char  predef_names[MAX_PREDEF_VARS][16];
    int   num_predef_vars;
} UndoSnapshot;

static UndoSnapshot g_undo_buf[UNDO_DEPTH];
static int g_undo_head = 0;
static int g_undo_count = 0;
static UndoSnapshot g_redo_buf[UNDO_DEPTH];
static int g_redo_head = 0;
static int g_redo_count = 0;

static const int g_accum_steps[] = { 1, 2, 4, 8, 16 };
static const char *quit_tempfile = "/tmp/temp-output.c";

static void snapshot_save(UndoSnapshot *s) {
    memcpy(s->cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    s->num_cmds = g_num_cmds;
    s->edit_line = g_edit_line;
    s->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        s->predef_vals[i] = g_predef_vars[i].value;
        memcpy(s->predef_names[i], g_predef_vars[i].name, 16);
    }
}

static void snapshot_restore(const UndoSnapshot *s) {
    memcpy(g_cmds, s->cmds, (size_t)s->num_cmds * sizeof(GLCmd));
    g_num_cmds = s->num_cmds;
    g_edit_line = s->edit_line;
    g_num_predef_vars = s->num_predef_vars;
    for (int i = 0; i < s->num_predef_vars; i++) {
        g_predef_vars[i].value = s->predef_vals[i];
        memcpy(g_predef_vars[i].name, s->predef_names[i], 16);
    }
    g_inserting = 0;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
}

void push_undo_snapshot(void) {
    /* First mutation on a loaded example auto-promotes to a user scene,
     * inheriting the example's name.  The undo snapshot captures the
     * post-promotion state so Undo rewinds to the unedited example
     * reference still visible in the Scene menu. */
    repl_promote_example_if_needed();

    snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_DEPTH;
    if (g_undo_count < UNDO_DEPTH)
        g_undo_count++;
    g_redo_count = 0;
    g_redo_head = 0;
}

void pop_undo_snapshot(void) {
    if (g_undo_count == 0) {
        set_status("Nothing to undo");
        return;
    }
    snapshot_save(&g_redo_buf[g_redo_head]);
    g_redo_head = (g_redo_head + 1) % UNDO_DEPTH;
    if (g_redo_count < UNDO_DEPTH)
        g_redo_count++;
    g_undo_head = (g_undo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    g_undo_count--;
    snapshot_restore(&g_undo_buf[g_undo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Undo (%d more)", g_undo_count);
        set_status(msg);
    }
}

void do_redo(void) {
    if (g_redo_count == 0) {
        set_status("Nothing to redo");
        return;
    }
    snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_DEPTH;
    if (g_undo_count < UNDO_DEPTH)
        g_undo_count++;
    g_redo_head = (g_redo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    g_redo_count--;
    snapshot_restore(&g_redo_buf[g_redo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Redo (%d more)", g_redo_count);
        set_status(msg);
    }
}

void clear_selection(void) {
    g_sel_anchor = g_sel_end = -1;
}

int sel_active(void) {
    return g_sel_anchor >= 0 && g_sel_end >= 0;
}

int sel_lo(void) {
    return g_sel_anchor < g_sel_end ? g_sel_anchor : g_sel_end;
}

int sel_hi(void) {
    return g_sel_anchor > g_sel_end ? g_sel_anchor : g_sel_end;
}

static int normalize_cmd_range(int start, int count, int *out_start, int *out_count) {
    if (count <= 0 || start < 0 || start >= g_num_cmds)
        return 0;
    if (start + count > g_num_cmds)
        count = g_num_cmds - start;
    *out_start = start;
    *out_count = count;
    return 1;
}

static int cmds_contain_var_decl(const GLCmd *cmds, int count) {
    for (int i = 0; i < count; i++) {
        if (cmds[i].type == CMD_VAR_DECLARE)
            return 1;
    }
    return 0;
}

static int cmd_range_contains_var_decl(int start, int count) {
    return cmds_contain_var_decl(&g_cmds[start], count);
}

static void set_var_decl_action_status(const char *action) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Cannot %s float declarations", action);
    set_status(msg);
}

static int delete_cmd_range_allowed(int start, int count) {
    if (cmd_range_contains_var_decl(start, count)) {
        set_var_decl_action_status("remove");
        return 0;
    }

    return 1;
}

static void remove_cmd_range_unchecked(int start, int count, const char *what) {
    char msg[128];
    int end = start + count;

    /* Snapshot names to undeclare after the memmove */
    char removed_names[MAX_PREDEF_VARS][16] = {{0}};
    int n_removed = 0;
    for (int i = start; i < end; i++) {
        if (g_cmds[i].type != CMD_VAR_DECLARE) continue;
        for (int n = 0; n < g_cmds[i].var_decl_count && n_removed < MAX_PREDEF_VARS; n++) {
            repl_copy_string_fits(removed_names[n_removed],
                                  sizeof(removed_names[n_removed]),
                                  g_cmds[i].var_names[n]);
            n_removed++;
        }
    }

    push_undo_snapshot();
    memmove(&g_cmds[start], &g_cmds[start + count],
            (g_num_cmds - start - count) * sizeof(GLCmd));
    g_num_cmds -= count;

    /* Compact g_predef_vars and shift CMD_VAR_ASSIGN indices */
    for (int r = 0; r < n_removed; r++) {
        int slot = find_predef_var_idx(removed_names[r]);
        if (slot < 0) continue;
        undeclare_predef_var(removed_names[r]);
        for (int j = 0; j < g_num_cmds; j++) {
            if (g_cmds[j].type == CMD_VAR_ASSIGN && g_cmds[j].num_args > slot)
                g_cmds[j].num_args--;
        }
    }

    g_edit_line = start;
    if (g_edit_line > g_num_cmds)
        g_edit_line = g_num_cmds;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
    clear_selection();
    snprintf(msg, sizeof(msg), "%s %d line%s",
             what, count, count > 1 ? "s" : "");
    set_status(msg);
}

void delete_cmd_range(int start, int count, const char *what) {
    if (!normalize_cmd_range(start, count, &start, &count))
        return;
    if (!delete_cmd_range_allowed(start, count))
        return;
    remove_cmd_range_unchecked(start, count, what);
}

void repl_clear_all_cmds(void) {
    push_undo_snapshot();
    g_num_cmds = 0;
    g_edit_line = 0;
    g_inserting = 0;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
    init_predef_vars();
    mark_normals_dirty();
    set_status("All commands cleared");
}

void load_line_to_input(int idx) {
    if (idx >= 0 && idx < g_num_cmds) {
        const char *s = g_cmds[idx].source;
        while (*s && isspace((unsigned char)*s))
            s++;

        if (g_cmds[idx].type == CMD_LABEL) {
            int len = (int)strlen(s);
            while (len > 0 &&
                   (s[len - 1] == ':' || isspace((unsigned char)s[len - 1])))
                len--;
            if (len > MAX_INPUT_LEN - 2)
                len = MAX_INPUT_LEN - 2;
            g_input[0] = ':';
            memcpy(g_input + 1, s, (size_t)len);
            g_input[len + 1] = '\0';
            g_input_len = len + 1;
            g_cursor_pos = g_input_len;
            return;
        }

        int len = (int)strlen(s);
        while (len > 0 &&
               (s[len - 1] == ';' || isspace((unsigned char)s[len - 1])))
            len--;
        if (len >= MAX_INPUT_LEN)
            len = MAX_INPUT_LEN - 1;
        memcpy(g_input, s, (size_t)len);
        g_input[len] = '\0';
        g_input_len = len;
        g_cursor_pos = len;
    } else {
        memcpy(g_input, g_newline_buf, (size_t)g_newline_len + 1);
        g_input_len = g_newline_len;
        g_cursor_pos = g_newline_len;
    }
}

static void save_newline_buf(void) {
    memcpy(g_newline_buf, g_input, (size_t)g_input_len + 1);
    g_newline_len = g_input_len;
}

static int function_decl_insert_pos(void) {
    int pos = 0;

    while (pos < g_num_cmds && g_cmds[pos].type == CMD_VAR_DECLARE)
        pos++;

    while (pos < g_num_cmds) {
        if (g_cmds[pos].type == CMD_COMMENT) {
            pos++;
            continue;
        }
        if (g_cmds[pos].type != CMD_FUNC_DEF)
            break;

        int end = find_block_end(pos);
        if (end >= g_num_cmds)
            return g_num_cmds;
        pos = end + 1;
    }

    return pos;
}

static int function_leading_comment_start(int pos) {
    int start = pos;

    while (start > 0 &&
           g_cmds[start - 1].valid &&
           g_cmds[start - 1].type == CMD_COMMENT &&
           block_depth_at(start - 1) == 0)
        start--;

    return start;
}

static void apply_func_decl_resume(CmdType end_type) {
    if (end_type != CMD_FUNC_END || g_func_decl_resume_delta <= 0)
        return;

    g_edit_line += g_func_decl_resume_delta;
    if (g_edit_line > g_num_cmds)
        g_edit_line = g_num_cmds;
    g_func_decl_resume_delta = 0;
}

static int resolve_insert_exit_target(int target) {
    if (!g_inserting ||
        g_func_decl_resume_delta <= 0 ||
        g_edit_line < 0 ||
        g_edit_line >= g_num_cmds ||
        g_cmds[g_edit_line].type != CMD_FUNC_END)
        return target;

    if (target == g_edit_line) {
        target += g_func_decl_resume_delta;
        if (target > g_num_cmds)
            target = g_num_cmds;
    }

    g_func_decl_resume_delta = 0;
    return target;
}

void repl_editor_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

void navigate_to_line(int target) {
    target = resolve_insert_exit_target(target);
    if (target < 0)
        target = 0;
    if (target > g_num_cmds)
        target = g_num_cmds;
    if (target == g_edit_line && !g_inserting)
        return;

    if (g_edit_line == g_num_cmds && !g_inserting)
        save_newline_buf();

    g_edit_line = target;
    g_inserting = 0;
    load_line_to_input(target);
    clear_autocomplete_state();
}

int try_commit_float_decl(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "float", 5) != 0) return 0;
    if (isalnum((unsigned char)p[5]) || p[5] == '_') return 0;
    p += 5;

    char names[MAX_NAMES_PER_DECL][16];
    float init_vals[MAX_NAMES_PER_DECL];
    int has_init[MAX_NAMES_PER_DECL];
    int var_count = 0;
    memset(has_init, 0, sizeof(has_init));
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        /* Accept ';' or end-of-string as terminator (';' key doesn't
         * append to g_input, so interactive commits lack the ';'). */
        if (*p == ';' || *p == '\0') break;
        if (var_count > 0) {
            if (*p != ',') {
                /* Not a comma — might be '=' handled below, or junk.
                 * If we already consumed at least one name and the
                 * previous iteration didn't end with '=', this is
                 * not a valid float declaration. */
                return 0;
            }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (!isalpha((unsigned char)*p) && *p != '_') {
            set_status("syntax error in float declaration: expected identifier");
            return 1;
        }
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16) {
            set_status("invalid identifier (max 15 chars)");
            return 1;
        }
        if (var_count >= MAX_NAMES_PER_DECL) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "too many names per declaration (max %d); split across lines",
                     MAX_NAMES_PER_DECL);
            set_status(buf);
            return 1;
        }
        memcpy(names[var_count], start, len);
        names[var_count][len] = '\0';

        /* Check for optional initializer: float name = expr */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            p++;  /* skip '=' */
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == ';' || *p == ',') {
                set_status("expected expression after '='");
                return 1;
            }
            /* Declarations are placed at the top of non-decl code; visible
             * scope vars at that position are always empty (decls live at
             * block depth 0), so the init expression may only reference
             * already-declared predef vars — no loop/function locals.
             * Extract the initializer expression up to ',' or ';' or end. */
            char init_expr[MAX_LINE_LEN];
            const char *expr_start = p;
            int depth = 0;
            while (*p && *p != ';') {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                else if (*p == ',' && depth == 0) break;
                p++;
            }
            int elen = (int)(p - expr_start);
            if (elen >= (int)sizeof(init_expr)) elen = (int)sizeof(init_expr) - 1;
            memcpy(init_expr, expr_start, elen);
            init_expr[elen] = '\0';
            /* Trim trailing whitespace */
            while (elen > 0 && isspace((unsigned char)init_expr[elen - 1]))
                init_expr[--elen] = '\0';
            if (elen == 0) {
                set_status("expected expression after '='");
                return 1;
            }
            char verr[128];
            if (!validate_expression_idents(init_expr, NULL, 0,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
            ExprCtx ctx = { init_expr, g_predef_vars, g_num_predef_vars };
            init_vals[var_count] = eval_expr(&ctx);
            has_init[var_count] = 1;
        }

        var_count++;
    }
    if (var_count == 0) {
        set_status("float declaration requires at least one identifier");
        return 1;
    }
    /* Accept ';' or end-of-string as valid terminator */
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/')) {
        set_status("syntax error: unexpected trailing text after declaration");
        return 1;
    }

    /* Detect overwrite-in-place (editing an existing CMD_VAR_DECLARE) early
     * so validation can exempt the line's own names from the "already
     * declared" check. Without this, re-committing `float tmp;` after
     * editing — even unchanged — reports "'tmp' already declared". */
    int insert_idx = g_inserting ? g_edit_line :
               (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    int overwriting_decl = (!g_inserting && insert_idx < g_num_cmds &&
                            g_cmds[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &g_cmds[insert_idx] : NULL;

    /* Validate all names atomically before registering any */
    for (int i = 0; i < var_count; i++) {
        /* Reject duplicates within the same declaration (e.g. float a, a;) */
        for (int j = 0; j < i; j++) {
            if (strcmp(names[i], names[j]) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "duplicate name '%s' in declaration", names[i]);
                set_status(buf);
                return 1;
            }
        }
        /* Reject re-declaring an already-declared variable — but exempt
         * names carried over from the decl we're overwriting, since those
         * will be undeclared before the new registration runs. */
        if (find_predef_var_idx(names[i]) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                for (int d = 0; d < old_decl->var_decl_count; d++) {
                    if (strcmp(old_decl->var_names[d], names[i]) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl) {
                char buf[128];
                if (!repl_format_fits(buf, sizeof(buf), "'%s' is already declared", names[i]))
                    repl_format_fits(buf, sizeof(buf), "identifier is already declared");
                set_status(buf);
                return 1;
            }
        }
        if (is_reserved_ident(names[i])) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "'%s' is reserved", names[i]))
                repl_format_fits(buf, sizeof(buf), "identifier is reserved");
            set_status(buf);
            return 1;
        }
        if (!(isalpha((unsigned char)names[i][0]) || names[i][0] == '_')) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "invalid identifier '%s'", names[i]))
                repl_format_fits(buf, sizeof(buf), "invalid identifier");
            set_status(buf);
            return 1;
        }
    }

    /* Capacity check: in overwrite mode the old decl's slots will be freed
     * before the new ones are registered, so the net delta is
     * new_count - old_count. */
    int old_count = old_decl ? old_decl->var_decl_count : 0;
    if (g_num_predef_vars + var_count - old_count > MAX_PREDEF_VARS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "variable table full (max %d)", MAX_PREDEF_VARS);
        set_status(buf);
        return 1;
    }

    /* Build the GLCmd */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.var_decl_count = var_count;
    for (int i = 0; i < var_count; i++) {
        if (!repl_copy_string_fits(cmd.var_names[i], sizeof(cmd.var_names[i]),
                                   names[i])) {
            set_status("invalid identifier (max 15 chars)");
            return 1;
        }
    }

    {
        int decl_pos = 0;
        while (decl_pos < g_num_cmds &&
               g_cmds[decl_pos].type == CMD_VAR_DECLARE)
            decl_pos++;

        int ind = 2;  /* decls always at block depth 0 (top-level) */
        char indent[32];
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        int off = snprintf(cmd.source, sizeof(cmd.source), "%sfloat ", indent);
        for (int i = 0; i < var_count && off < (int)sizeof(cmd.source) - 4; i++) {
            if (i > 0) off += snprintf(cmd.source + off, sizeof(cmd.source) - off, ", ");
            off += snprintf(cmd.source + off, sizeof(cmd.source) - off, "%s", names[i]);
            if (has_init[i])
                off += snprintf(cmd.source + off, sizeof(cmd.source) - off,
                                " = %g", init_vals[i]);
        }
        snprintf(cmd.source + off, sizeof(cmd.source) - off, ";");

        /* Check overwrite feasibility BEFORE registering new names. Only
         * names being REMOVED (present in old decl, absent from new) need
         * the "in use" check — names being kept stay valid throughout. */
        if (overwriting_decl) {
            for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                const char *nm = g_cmds[insert_idx].var_names[d];
                int kept = 0;
                for (int k = 0; k < var_count; k++) {
                    if (strcmp(names[k], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                for (int j = 0; j < g_num_cmds; j++) {
                    if (j == insert_idx) continue;
                    if (source_uses_ident(g_cmds[j].source, nm)) {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "variable '%s' is in use, cannot overwrite", nm);
                        set_status(buf);
                        return 1;
                    }
                }
            }
        }

        /* Undeclare only names being removed (absent from new decl) so kept
         * names retain their slot indices and live values. */
        if (overwriting_decl) {
            for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                const char *nm = g_cmds[insert_idx].var_names[d];
                int kept = 0;
                for (int k = 0; k < var_count; k++) {
                    if (strcmp(names[k], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                int slot = find_predef_var_idx(nm);
                if (slot < 0) continue;
                undeclare_predef_var(nm);
                for (int j = 0; j < g_num_cmds; j++) {
                    if (g_cmds[j].type == CMD_VAR_ASSIGN && g_cmds[j].num_args > slot)
                        g_cmds[j].num_args--;
                }
            }
        }

        /* Register new names (safe — overwrite check passed, capacity verified).
         * Skip names already registered (kept from old decl) to preserve values. */
        for (int i = 0; i < var_count; i++) {
            if (overwriting_decl && find_predef_var_idx(names[i]) >= 0) {
                if (has_init[i]) {
                    int idx = find_predef_var_idx(names[i]);
                    g_predef_vars[idx].value = init_vals[i];
                }
                continue;
            }
            declare_predef_var(names[i], NULL, 0);
            if (has_init[i]) {
                int idx = find_predef_var_idx(names[i]);
                if (idx >= 0)
                    g_predef_vars[idx].value = init_vals[i];
            }
        }

        if (overwriting_decl) {
            g_cmds[insert_idx] = cmd;
            g_edit_line++;
            load_line_to_input(g_edit_line);
        } else if (g_num_cmds < MAX_COMMANDS) {
            for (int j = g_num_cmds; j > decl_pos; j--)
                g_cmds[j] = g_cmds[j - 1];
            g_cmds[decl_pos] = cmd;
            g_num_cmds++;
            if (g_edit_line >= decl_pos) g_edit_line++;
            if (!g_inserting && g_edit_line < g_num_cmds)
                load_line_to_input(g_edit_line);
        }
    }

    {
        char msg[128];
        int off = snprintf(msg, sizeof(msg), "declared ");
        for (int i = 0; i < var_count && off < (int)sizeof(msg) - 4; i++) {
            if (i > 0) off += snprintf(msg + off, sizeof(msg) - off, ", ");
            off += snprintf(msg + off, sizeof(msg) - off, "%s", names[i]);
        }
        set_status(msg);
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    mark_normals_dirty();
    return 1;
}

int try_assign_variable(void) {
    char name[16];
    char rhs[MAX_LINE_LEN];
    char comment[MAX_LINE_LEN];
    int has_rhs_vars;
    float val;
    char indent[32];
    int ind;

    if (!repl_extract_assignment_parts(g_input, name, sizeof(name), rhs, sizeof(rhs)))
        return 0;

    comment[0] = '\0';
    {
        const char *comment_p = strstr(g_input, "//");
        if (comment_p) {
            while (*comment_p && isspace((unsigned char)*comment_p))
                comment_p++;
            if (comment_p[0] == '/' && comment_p[1] == '/') {
                snprintf(comment, sizeof(comment), " %s", comment_p);
            }
        }
    }

    int var_idx = find_predef_var_idx(name);
    if (var_idx < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "undeclared variable '%s' — use 'float %s;' first", name, name);
        set_status(buf);
        return 1;
    }

    {
        int insert_idx = g_inserting ? g_edit_line :
                   (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar vis[MAX_EXPR_VARS];
        int vis_n = collect_visible_vars(insert_idx, vis, MAX_EXPR_VARS);
        char verr[128];
        if (!validate_expression_idents(rhs, vis_n > 0 ? vis : NULL, vis_n, verr, sizeof(verr))) {
            set_status(verr);
            return 1;
        }
    }
    {
        ExprCtx ctx = { rhs, g_predef_vars, g_num_predef_vars };
        val = eval_expr(&ctx);
    }
    g_predef_vars[var_idx].value = val;
    has_rhs_vars = input_has_predef_vars(rhs);

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CMD_VAR_ASSIGN;
        cmd.valid = 1;
        cmd.args[0] = val;
        cmd.num_args = var_idx;
        cmd.has_vars = has_rhs_vars;

        {
            int insert_idx = g_inserting ? g_edit_line :
                       (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
            ind = (in_begin_block_at(insert_idx) ? 4 : 2) + block_depth_at(insert_idx) * 2;
            if (ind > (int)sizeof(indent) - 1)
                ind = (int)sizeof(indent) - 1;
            memset(indent, ' ', (size_t)ind);
            indent[ind] = '\0';
            if (!repl_format_fits(cmd.source, sizeof(cmd.source),
                                  "%s%s = %s;%s",
                                  indent, name, rhs, comment)) {
                set_status("Command too long");
                return 1;
            }

            if (g_inserting) {
                if (g_num_cmds < MAX_COMMANDS) {
                    for (int j = g_num_cmds; j > insert_idx; j--)
                        g_cmds[j] = g_cmds[j - 1];
                    g_cmds[insert_idx] = cmd;
                    g_num_cmds++;
                    g_edit_line++;
                }
            } else if (insert_idx < g_num_cmds) {
                if (g_cmds[insert_idx].type == CMD_VAR_DECLARE) {
                    for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                        const char *nm = g_cmds[insert_idx].var_names[d];
                        for (int j = 0; j < g_num_cmds; j++) {
                            if (j == insert_idx) continue;
                            if (source_uses_ident(g_cmds[j].source, nm)) {
                                char buf[128];
                                snprintf(buf, sizeof(buf),
                                         "variable '%s' is in use, cannot overwrite", nm);
                                set_status(buf);
                                return 1;
                            }
                        }
                    }
                    for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                        const char *nm = g_cmds[insert_idx].var_names[d];
                        int slot = find_predef_var_idx(nm);
                        if (slot < 0) continue;
                        undeclare_predef_var(nm);
                        for (int j = 0; j < g_num_cmds; j++) {
                            if (g_cmds[j].type == CMD_VAR_ASSIGN && g_cmds[j].num_args > slot)
                                g_cmds[j].num_args--;
                        }
                    }
                }
                g_cmds[insert_idx] = cmd;
                g_edit_line++;
                load_line_to_input(g_edit_line);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s = %g", name, val);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            } else {
                if (g_num_cmds < MAX_COMMANDS)
                    g_cmds[g_num_cmds++] = cmd;
                g_edit_line = g_num_cmds;
            }
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s = %g", name, val);
        set_status(msg);
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    mark_normals_dirty();
    return 1;
}

int try_commit_for_loop(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        char var_name[16];
        float start, end, step;
        const char *body_start;

        if (!parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                        &start, &end, &step,
                                        visible_vars, visible_nv, &body_start)) {
            set_status("for syntax: for(var, start, end[, step]) body;");
            return 1;
        }

        while (*body_start && isspace((unsigned char)*body_start))
            body_start++;

        {
            int fdepth = block_depth_at(pos);
            int bb = in_begin_block_at(pos);
            int ind = (bb ? 4 : 2) + fdepth * 2;
            char indent[32];
            GLCmd fb;
            GLCmd fe;

            if (ind > (int)sizeof(indent) - 1)
                ind = (int)sizeof(indent) - 1;
            memset(indent, ' ', (size_t)ind);
            indent[ind] = '\0';

            memset(&fb, 0, sizeof(fb));
            fb.type = CMD_FOR_BEGIN;
            fb.args[0] = start;
            fb.args[1] = end;
            fb.args[2] = step;
            fb.valid = 1;

            {
                const char *raw = p;
                while (*raw && *raw != '(')
                    raw++;
                if (*raw)
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                while (*raw && (isalnum((unsigned char)*raw) || *raw == '_'))
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                if (*raw == ',')
                    raw++;

                {
                    const char *args_start = raw;
                    int paren = 1;
                    const char *ap = args_start;
                    char raw_args[MAX_LINE_LEN];
                    int rlen;

                    while (*ap && paren > 0) {
                        if (*ap == '(')
                            paren++;
                        else if (*ap == ')')
                            paren--;
                        if (paren > 0)
                            ap++;
                    }

                    rlen = (int)(ap - args_start);
                    if (rlen > (int)sizeof(raw_args) - 1)
                        rlen = (int)sizeof(raw_args) - 1;
                    memcpy(raw_args, args_start, (size_t)rlen);
                    raw_args[rlen] = '\0';
                    while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
                        raw_args[--rlen] = '\0';

                    {
                        char *ra = raw_args;
                        while (*ra && isspace((unsigned char)*ra))
                            ra++;

                        {
                            char verr[128];
                            if (!validate_expression_idents(ra, visible_vars, visible_nv, verr, sizeof(verr))) {
                                set_status(verr);
                                return 1;
                            }
                        }

                        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
                            fb.has_vars = 1;
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
                                                  "%sfor(%s, %s) {",
                                                  indent, var_name, ra)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else if (step != 1.0f) {
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
                                                  "%sfor(%s, %g, %g, %g) {",
                                                  indent, var_name, start, end, step)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else {
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
                                                  "%sfor(%s, %g, %g) {",
                                                  indent, var_name, start, end)) {
                                set_status("Command too long");
                                return 1;
                            }
                        }
                    }
                }
            }

            memset(&fe, 0, sizeof(fe));
            fe.type = CMD_FOR_END;
            fe.valid = 1;
            snprintf(fe.source, sizeof(fe.source), "%s}", indent);

            if (*body_start == '{' || *body_start == '\0') {
                if (!g_inserting && g_edit_line < g_num_cmds &&
                    g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                    g_cmds[g_edit_line] = fb;
                    g_edit_line++;
                    g_inserting = 1;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    clear_autocomplete_state();
                    set_status("for-loop header updated");
                    mark_normals_dirty();
                    return 1;
                }

                if (g_num_cmds + 2 > MAX_COMMANDS) {
                    set_status("Command buffer full!");
                    return 1;
                }
                memmove(&g_cmds[pos + 2], &g_cmds[pos],
                        (g_num_cmds - pos) * sizeof(GLCmd));
                g_cmds[pos] = fb;
                g_cmds[pos + 1] = fe;
                g_num_cmds += 2;

                g_edit_line = pos + 1;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                set_status("for-loop: type body lines, press Esc when done");
                mark_normals_dirty();
                return 1;
            }

            {
                char body[MAX_LINE_LEN];
                int blen;
                ExprVar dv[MAX_EXPR_VARS];
                int dvn = 0;
                GLCmd body_cmd;
                int saved;

                strncpy(body, body_start, MAX_LINE_LEN - 1);
                body[MAX_LINE_LEN - 1] = '\0';
                blen = (int)strlen(body);
                while (blen > 0 &&
                       (body[blen - 1] == ';' || isspace((unsigned char)body[blen - 1])))
                    body[--blen] = '\0';
                if (blen == 0) {
                    set_status("for-loop needs a body");
                    return 1;
                }

                repl_copy_string_fits(dv[dvn].name, sizeof(dv[dvn].name),
                                      var_name);
                dv[dvn].value = start;
                dvn++;
                for (int i = 0; i < visible_nv && dvn < MAX_EXPR_VARS; i++)
                    dv[dvn++] = visible_vars[i];

                memset(&body_cmd, 0, sizeof(body_cmd));
                saved = g_edit_line;
                g_edit_line = pos;
                if (!repl_parse_command_with_vars(body, &body_cmd, dv, dvn)) {
                    g_edit_line = saved;
                    set_status("Invalid for-loop body command");
                    return 1;
                }
                g_edit_line = saved;

                {
                    char bind[32];
                    int bi = ind + 2;
                    if (bi > (int)sizeof(bind) - 1)
                        bi = (int)sizeof(bind) - 1;
                    memset(bind, ' ', (size_t)bi);
                    bind[bi] = '\0';
                    snprintf(body_cmd.source, sizeof(body_cmd.source), "%s%s;", bind, body);
                }

                if (g_num_cmds + 3 > MAX_COMMANDS) {
                    set_status("Command buffer full!");
                    return 1;
                }
                memmove(&g_cmds[pos + 3], &g_cmds[pos],
                        (g_num_cmds - pos) * sizeof(GLCmd));
                g_cmds[pos] = fb;
                g_cmds[pos + 1] = body_cmd;
                g_cmds[pos + 2] = fe;
                g_num_cmds += 3;

                g_edit_line = pos + 3;
                g_inserting = 0;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                g_newline_buf[0] = '\0';
                g_newline_len = 0;

                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "for-loop: %s from %g to %g",
                             var_name, start, end);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            }
        }
    }
}

int try_commit_func_def(void) {
    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][16];
    const char *trimmed = g_input;

    while (*trimmed && isspace((unsigned char)*trimmed))
        trimmed++;
    if (strchr(trimmed, '(') && strchr(trimmed, '{') == NULL)
        return 0;
    if (!parse_repl_func_signature(g_input, &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count))
        return 0;

    {
        int edit_pos = g_inserting ? g_edit_line :
                       (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        int overwriting_func = (!g_inserting && edit_pos < g_num_cmds &&
                                g_cmds[edit_pos].type == CMD_FUNC_DEF);
        int pos = overwriting_func ? edit_pos : function_decl_insert_pos();
        int bdepth = overwriting_func ? block_depth_at(pos) : 0;
        int bb = overwriting_func ? in_begin_block_at(pos) : 0;
        int ind = (bb ? 4 : 2) + bdepth * 2;
        char indent[32];
        GLCmd fd;
        GLCmd fe;

        if (ind > (int)sizeof(indent) - 1)
            ind = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        if (overwriting_func) {
            g_cmds[edit_pos].args[0] = (float)fn;
            g_cmds[edit_pos].num_args = param_count;
            format_func_header(g_cmds[edit_pos].source,
                               (int)sizeof(g_cmds[edit_pos].source),
                               indent, fn, param_names, param_count);
            g_edit_line = edit_pos + 1;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            clear_autocomplete_state();
            set_status("func def header updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&fd, 0, sizeof(fd));
        fd.type = CMD_FUNC_DEF;
        fd.args[0] = (float)fn;
        fd.num_args = param_count;
        fd.valid = 1;
        format_func_header(fd.source, (int)sizeof(fd.source),
                           indent, fn, param_names, param_count);

        memset(&fe, 0, sizeof(fe));
        fe.type = CMD_FUNC_END;
        fe.valid = 1;
        snprintf(fe.source, sizeof(fe.source), "%s}", indent);

        int comment_start = edit_pos;
        int comment_count = 0;
        int resume_pos = edit_pos;
        GLCmd *comments = NULL;

        if (!overwriting_func) {
            comment_start = function_leading_comment_start(edit_pos);
            comment_count = edit_pos - comment_start;
        }
        if (comment_count > 0) {
            comments = (GLCmd *)malloc((size_t)comment_count * sizeof(*comments));
            if (comments) {
                memcpy(comments, &g_cmds[comment_start],
                       (size_t)comment_count * sizeof(*comments));
                memmove(&g_cmds[comment_start], &g_cmds[edit_pos],
                        (size_t)(g_num_cmds - edit_pos) * sizeof(GLCmd));
                g_num_cmds -= comment_count;
                resume_pos = edit_pos - comment_count;
            } else {
                comment_count = 0;
            }
        }

        pos = function_decl_insert_pos();
        int insert_count = comment_count + 2;
        if (g_num_cmds + insert_count > MAX_COMMANDS) {
            free(comments);
            set_status("Command buffer full!");
            return 1;
        }
        memmove(&g_cmds[pos + insert_count], &g_cmds[pos],
                (g_num_cmds - pos) * sizeof(GLCmd));
        if (comment_count > 0)
            memcpy(&g_cmds[pos], comments,
                   (size_t)comment_count * sizeof(*comments));
        g_cmds[pos + comment_count] = fd;
        g_cmds[pos + comment_count + 1] = fe;
        g_num_cmds += insert_count;
        g_func_decl_resume_delta = resume_pos > pos ? resume_pos - pos : 0;
        free(comments);

        g_edit_line = pos + comment_count + 1;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        set_status("func def: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_if_block(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0)
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        float cond_args[1];
        float cond_val;
        char cond_text[MAX_LINE_LEN];
        int clen;
        int bdepth;
        int bb;
        int ind;
        char indent[32];
        GLCmd ib;
        GLCmd ie;

        while (*p && *p != '(')
            p++;
        if (!*p)
            return 0;
        p++;

        {
            int paren = 1;
            const char *expr_start = p;

            while (*p && paren > 0) {
                if (*p == '(')
                    paren++;
                else if (*p == ')')
                    paren--;
                if (paren > 0)
                    p++;
            }
            if (paren != 0) {
                set_status("if syntax: if(expr) {");
                return 1;
            }

            clen = (int)(p - expr_start);
            if (clen > (int)sizeof(cond_text) - 1)
                clen = (int)sizeof(cond_text) - 1;
            memcpy(cond_text, expr_start, (size_t)clen);
            cond_text[clen] = '\0';
        }

        {
            char verr[128];
            if (!validate_expression_idents(cond_text,
                                            visible_nv > 0 ? visible_vars : NULL, visible_nv,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
        }
        {
            int neval = parse_exprs(cond_text, cond_args, 1,
                                    visible_nv > 0 ? visible_vars : NULL, visible_nv);
            cond_val = (neval >= 1) ? cond_args[0] : 0.0f;
        }

        p++;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '{' && *p != '\0') {
            set_status("if syntax: if(expr) {");
            return 1;
        }

        bdepth = block_depth_at(pos);
        bb = in_begin_block_at(pos);
        ind = (bb ? 4 : 2) + bdepth * 2;
        if (ind > (int)sizeof(indent) - 1)
            ind = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        memset(&ib, 0, sizeof(ib));
        ib.type = CMD_IF_BEGIN;
        ib.args[0] = cond_val;
        ib.valid = 1;
        ib.has_vars = input_has_any_visible_vars(cond_text, visible_vars, visible_nv);

        {
            char *ct = cond_text;
            int ctlen;

            while (*ct && isspace((unsigned char)*ct))
                ct++;
            ctlen = (int)strlen(ct);
            while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
                ct[--ctlen] = '\0';
            snprintf(ib.source, sizeof(ib.source), "%sif(%s) {", indent, ct);
        }

        if (!g_inserting && g_edit_line < g_num_cmds &&
            g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
            g_cmds[g_edit_line] = ib;
            g_edit_line++;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            clear_autocomplete_state();
            set_status("if condition updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&ie, 0, sizeof(ie));
        ie.type = CMD_IF_END;
        ie.valid = 1;
        snprintf(ie.source, sizeof(ie.source), "%s}", indent);

        if (g_num_cmds + 2 > MAX_COMMANDS) {
            set_status("Command buffer full!");
            return 1;
        }
        memmove(&g_cmds[pos + 2], &g_cmds[pos],
                (g_num_cmds - pos) * sizeof(GLCmd));
        g_cmds[pos] = ib;
        g_cmds[pos + 1] = ie;
        g_num_cmds += 2;

        g_edit_line = pos + 1;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        set_status("if-block: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_close_brace(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '}')
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        CmdType open_type = nearest_open_block_at(pos);
        CmdType end_type;
        const char *label;
        int bdepth;
        int bb_val;
        int ind_len;
        char indent[32];
        GLCmd fe;

        if (open_type == CMD_TYPE_COUNT)
            return 0;

        if (open_type == CMD_FOR_BEGIN) {
            end_type = CMD_FOR_END;
            label = "for-loop";
        } else if (open_type == CMD_FUNC_DEF) {
            end_type = CMD_FUNC_END;
            label = "func def";
        } else if (open_type == CMD_IF_BEGIN) {
            end_type = CMD_IF_END;
            label = "if-block";
        } else {
            return 0;
        }

        /* Reuse an existing synthesized end marker even if we're no longer
         * in insert mode (e.g. after closing an inner block). Otherwise a
         * function/if/for close brace can duplicate the trailing end command. */
        if (pos < g_num_cmds && g_cmds[pos].type == end_type) {
            int keep_inserting = (g_func_decl_resume_delta > 0 &&
                                  end_type != CMD_FUNC_END);
            g_edit_line = pos + 1;
            apply_func_decl_resume(end_type);
            g_inserting = keep_inserting ? 1 : 0;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            load_line_to_input(g_edit_line);
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s block closed", label);
                set_status(msg);
            }
            mark_normals_dirty();
            return 1;
        }

        bdepth = block_depth_at(pos) - 1;
        if (bdepth < 0)
            bdepth = 0;
        bb_val = in_begin_block_at(pos);
        ind_len = (bb_val ? 4 : 2) + bdepth * 2;
        if (ind_len > (int)sizeof(indent) - 1)
            ind_len = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind_len);
        indent[ind_len] = '\0';

        memset(&fe, 0, sizeof(fe));
        fe.type = end_type;
        fe.valid = 1;
        snprintf(fe.source, sizeof(fe.source), "%s}", indent);

        if (g_num_cmds >= MAX_COMMANDS) {
            set_status("Command buffer full!");
            return 1;
        }
        memmove(&g_cmds[pos + 1], &g_cmds[pos],
                (g_num_cmds - pos) * sizeof(GLCmd));
        g_cmds[pos] = fe;
        g_num_cmds++;
        int keep_inserting = (g_func_decl_resume_delta > 0 &&
                              end_type != CMD_FUNC_END);
        g_edit_line = pos + 1;
        apply_func_decl_resume(end_type);
        g_inserting = keep_inserting ? 1 : 0;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        g_newline_buf[0] = '\0';
        g_newline_len = 0;
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s block closed", label);
            set_status(msg);
        }
        mark_normals_dirty();
        return 1;
    }
}

/* Rewrite cmd->source from g_input with proper indentation.
 * Strips leading whitespace and trailing `;`/whitespace from g_input,
 * prefixes indent (2 outside a glBegin block, 4 inside), then appends `;`.
 * With include_block_depth, adds 2 spaces per open for/func/if scope at pos. */
static void rewrite_cmd_source_with_indent(GLCmd *cmd, int pos,
                                           int include_block_depth) {
    char stripped[MAX_LINE_LEN];
    const char *sp = g_input;
    while (*sp && isspace((unsigned char)*sp)) sp++;
    strncpy(stripped, sp, MAX_LINE_LEN - 1);
    stripped[MAX_LINE_LEN - 1] = '\0';
    int slen = (int)strlen(stripped);
    while (slen > 0 &&
           (stripped[slen - 1] == ';' ||
            isspace((unsigned char)stripped[slen - 1])))
        stripped[--slen] = '\0';
    int ind = in_begin_block_at(pos) ? 4 : 2;
    if (include_block_depth)
        ind += block_depth_at(pos) * 2;
    char indent[32];
    if (ind > (int)sizeof(indent) - 1)
        ind = (int)sizeof(indent) - 1;
    memset(indent, ' ', (size_t)ind);
    indent[ind] = '\0';
    snprintf(cmd->source, sizeof(cmd->source), "%s%s;", indent, stripped);
}

/* Block-structural commit handlers: `}`, `for(`, `funcN`, `if(`.
 * These inspect the start of g_input and are mutually exclusive with
 * var-statement handlers. */
static int try_commit_block_structs(void) {
    if (try_commit_close_brace()) return 1;
    if (try_commit_for_loop())    return 1;
    if (try_commit_func_def())    return 1;
    if (try_commit_if_block())    return 1;
    return 0;
}

/* Statement-level commit handlers. float decl MUST precede assign so that
 * `float x` is not misread as an assignment to an identifier named "float". */
static int try_commit_var_statements(void) {
    if (try_commit_float_decl())  return 1;
    if (try_assign_variable())    return 1;
    return 0;
}

/* Canonical order: var statements first (float/assign), then block structs.
 * Handlers are mutually exclusive on input prefix, so the relative ordering
 * of the two groups doesn't affect observed behavior. */
static int try_commit_any(void) {
    if (try_commit_var_statements()) return 1;
    if (try_commit_block_structs())  return 1;
    return 0;
}

/* Overwrite-mode Enter variant: on successful var-statement commit, enter
 * insert mode and clear the input. Assign additionally publishes the
 * "Insert mode" status and marks normals dirty (float decl does not). */
static int try_commit_var_statements_then_insert(void) {
    if (try_commit_float_decl()) {
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        clear_autocomplete_state();
        return 1;
    }
    if (try_assign_variable()) {
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        clear_autocomplete_state();
        set_status("Insert mode");
        mark_normals_dirty();
        return 1;
    }
    return 0;
}

/* Parse g_input into `cmd` as if it were being committed at source-line
 * `insert_idx`. Handles the three-way fan-out used by the overwrite-Enter
 * and append-at-end Enter paths:
 *   - loop/function locals visible at that line → parse_with_vars +
 *     reindent
 *   - else, predef vars referenced → plain parse, mark has_vars, reindent
 *     without vars
 *   - else, plain parse only
 * Returns 1 if parsing succeeded. */
static int parse_for_overwrite_enter(GLCmd *cmd, int insert_idx) {
    ExprVar vis_vars[MAX_EXPR_VARS];
    int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);
    memset(cmd, 0, sizeof(*cmd));
    int parsed;
    if (num_vis_vars > 0) {
        int saved_el = g_edit_line;
        g_edit_line = insert_idx;
        parsed = repl_parse_command_with_vars(g_input, cmd, vis_vars, num_vis_vars);
        g_edit_line = saved_el;
        if (parsed)
            rewrite_cmd_source_with_indent(cmd, insert_idx, 1);
    } else {
        parsed = repl_parse_command(g_input, cmd);
        if (parsed && input_has_predef_vars(g_input)) {
            cmd->has_vars = 1;
            rewrite_cmd_source_with_indent(cmd, insert_idx, 0);
        }
    }
    return parsed;
}

void keyboard_func(unsigned char key, int x, int y) {
    (void)x;
    (void)y;

    g_cursor_on = 1;
    g_blink_tick = 0;

    /* Cut / copy / backspace / delete preserve any active line-range
     * selection; everything else clears it before processing the key. */
    if (key != KEY_CTRL_C && key != KEY_CTRL_D && key != KEY_BACKSPACE &&
        key != KEY_CTRL_X && key != KEY_DELETE)
        clear_selection();

    g_scroll_follow_cursor = 1;

    /* Rename overlay captures every keystroke while active, ahead of
     * the backtick/config, replay, and search branches — otherwise
     * typing `, or keys bound to replay would leak out of the rename
     * buffer and trigger unrelated UI. */
    if (ui_panels_handle_rename_key(key))
        return;

    if (!g_search_active && key == '`') {
        if (g_replay_active)
            replay_stop();
        editor_restore_hidden_code_panel();
        ui_panels_open_config();
        return;
    }

    if (g_replay_active && replay_handle_key(key))
        return;

    if (editor_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_key_restores_hidden_code_panel(key, key_mods))
            editor_restore_hidden_code_panel();
    }

    if (handle_search_key(key))
        return;

    if (key == KEY_ESC) {
        if (ui_panels_handle_escape()) {
            glutPostRedisplay();
            return;
        }
        if (g_show_help) {
            g_show_help = 0;
            g_help_tab = 0;
            g_help_scroll = 0;
        } else if (g_ac_count > 0) {
            clear_autocomplete_state();
        } else if (g_inserting) {
            g_inserting = 0;
            if (g_edit_line <= g_num_cmds)
                load_line_to_input(g_edit_line);
            set_status("Insert mode exited");
        } else {
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            set_status("Input cleared");
        }
        return;
    }

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (!g_cfg_items[i].is_special && g_cfg_items[i].key_code > 0
                && g_cfg_items[i].key_code < 32 && g_cfg_items[i].key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return;
        }
    }

    if (key == KEY_CTRL_A) {
        g_cursor_pos = 0;
        update_autocomplete();
        return;
    }
    if (key == KEY_CTRL_E) {
        g_cursor_pos = g_input_len;
        update_autocomplete();
        return;
    }

    if (key == KEY_CTRL_Z) {
        if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT)
            do_redo();
        else
            pop_undo_snapshot();
        return;
    }

    if (key == KEY_CTRL_Y) {
        do_redo();
        return;
    }

    if (replay_handle_key(key))
        return;

    if (key == KEY_CTRL_D) {
        if (g_inserting) {
            g_inserting = 0;
            if (g_edit_line <= g_num_cmds)
                load_line_to_input(g_edit_line);
            set_status("Insert mode exited");
        } else if (sel_active()) {
            int start = sel_lo();
            int hi = sel_hi();
            if (hi >= g_num_cmds)
                hi = g_num_cmds - 1;
            delete_cmd_range(start, hi - start + 1, "Deleted");
        } else if (g_edit_line < g_num_cmds) {
            delete_cmd_range(g_edit_line, 1, "Deleted");
        }
        return;
    }

    if (key == KEY_CTRL_L) {
        repl_clear_all_cmds();
        return;
    }

    if (key == KEY_CTRL_BACKSLASH) {
        if (g_num_cmds > 0) {
            push_undo_snapshot();
            repl_reformat_commands();
            set_status("Reformatted command buffer");
        } else {
            set_status("Nothing to reformat");
        }
        return;
    }

    if (key == KEY_CTRL_P) {
        repl_debug_dump_editor(stdout);
        repl_debug_dump_flat_commands(stdout);
        set_status("Dumped editor + flat commands to stdout");
        return;
    }

    if (key == KEY_CTRL_S) {
        repl_save_default_output();
        return;
    }

    if (key == KEY_CTRL_C) {
        if (g_inserting) {
            clear_selection();
            return;
        }
        if (sel_active()) {
            int start = sel_lo();
            int count;
            int hi = sel_hi();
            if (hi >= g_num_cmds)
                hi = g_num_cmds - 1;
            count = hi - start + 1;
            if (!normalize_cmd_range(start, count, &start, &count))
                return;
            if (cmd_range_contains_var_decl(start, count)) {
                set_var_decl_action_status("copy");
                return;
            }
            g_clipboard_count = 0;
            for (int i = start; i < start + count && g_clipboard_count < MAX_COMMANDS; i++)
                g_clipboard[g_clipboard_count++] = g_cmds[i];
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Copied %d line%s",
                         g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
                set_status(msg);
            }
        } else if (g_edit_line < g_num_cmds) {
            int start = g_edit_line;
            int count = 1;
            int copying_for = (g_cmds[start].type == CMD_FOR_BEGIN);
            if (copying_for) {
                int fe = find_block_end(start);
                int end_idx = (fe < g_num_cmds) ? fe + 1 : g_num_cmds;
                count = end_idx - start;
            }
            if (!normalize_cmd_range(start, count, &start, &count))
                return;
            if (cmd_range_contains_var_decl(start, count)) {
                set_var_decl_action_status("copy");
                return;
            }
            g_clipboard_count = 0;
            for (int i = start; i < start + count && g_clipboard_count < MAX_COMMANDS; i++)
                g_clipboard[g_clipboard_count++] = g_cmds[i];
            if (copying_for) {
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Copied for-loop (%d lines)",
                             g_clipboard_count);
                    set_status(msg);
                }
            } else {
                g_clipboard[0] = g_cmds[g_edit_line];
                g_clipboard_count = 1;
                set_status("Copied line");
            }
        } else {
            g_clipboard_count = 0;
        }
        clear_selection();
        return;
    }

    if (key == KEY_CTRL_X) {
        if (g_inserting) {
            clear_selection();
            return;
        }
        {
            int start;
            int count;

            if (sel_active()) {
                start = sel_lo();
                {
                    int hi = sel_hi();
                    if (hi >= g_num_cmds)
                        hi = g_num_cmds - 1;
                    count = hi - start + 1;
                }
            } else if (g_edit_line < g_num_cmds) {
                start = g_edit_line;
                if (g_cmds[start].type == CMD_FOR_BEGIN) {
                    int fe = find_block_end(start);
                    count = ((fe < g_num_cmds) ? fe + 1 : g_num_cmds) - start;
                } else {
                    count = 1;
                }
            } else {
                g_clipboard_count = 0;
                clear_selection();
                return;
            }

            if (!normalize_cmd_range(start, count, &start, &count))
                return;
            if (!delete_cmd_range_allowed(start, count))
                return;

            g_clipboard_count = 0;
            for (int i = 0; i < count && g_clipboard_count < MAX_COMMANDS; i++)
                g_clipboard[g_clipboard_count++] = g_cmds[start + i];
            remove_cmd_range_unchecked(start, count, "Cut");
        }
        return;
    }

    if (key == KEY_CTRL_V) {
        if (g_clipboard_count > 0) {
            if (cmds_contain_var_decl(g_clipboard, g_clipboard_count)) {
                set_var_decl_action_status("paste");
                return;
            }
            if (g_num_cmds + g_clipboard_count > MAX_COMMANDS) {
                set_status("Command buffer full!");
                return;
            }
            push_undo_snapshot();
            {
                int pos = g_inserting ? g_edit_line :
                          (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
                memmove(&g_cmds[pos + g_clipboard_count], &g_cmds[pos],
                        (g_num_cmds - pos) * sizeof(GLCmd));
                memcpy(&g_cmds[pos], g_clipboard,
                       (size_t)g_clipboard_count * sizeof(GLCmd));
                g_num_cmds += g_clipboard_count;
                g_edit_line = pos + g_clipboard_count;
                g_inserting = 0;
                load_line_to_input(g_edit_line);
                mark_normals_dirty();
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Pasted %d line%s",
                         g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
                set_status(msg);
            }
        } else {
            set_status("Clipboard empty");
        }
        return;
    }

    if (key == '/' && (editor_get_modifiers() & GLUT_ACTIVE_CTRL)) {
        if (g_edit_line < g_num_cmds && !g_inserting) {
            push_undo_snapshot();
            {
                GLCmd *cur = &g_cmds[g_edit_line];
                if (cur->type == CMD_COMMENT) {
                    const char *s = cur->source;
                    while (*s && isspace((unsigned char)*s))
                        s++;
                    if (s[0] == '/' && s[1] == '/') {
                        s += 2;
                        if (*s == ' ')
                            s++;
                    }
                    {
                        GLCmd new_cmd;
                        memset(&new_cmd, 0, sizeof(new_cmd));
                        if (repl_parse_command(s, &new_cmd)) {
                            g_cmds[g_edit_line] = new_cmd;
                            load_line_to_input(g_edit_line);
                            mark_normals_dirty();
                            set_status("Uncommented");
                        } else {
                            set_status("Cannot uncomment: not a valid command");
                        }
                    }
                } else if (cur->type != CMD_FOR_BEGIN &&
                           cur->type != CMD_FOR_END) {
                    char new_src[MAX_LINE_LEN];
                    const char *s = cur->source;
                    int ind = 0;
                    while (s[ind] && isspace((unsigned char)s[ind]))
                        ind++;
                    snprintf(new_src, sizeof(new_src), "%.*s// %s", ind, s, s + ind);
                    cur->type = CMD_COMMENT;
                    cur->valid = 1;
                    repl_copy_string_fits(cur->source, sizeof(cur->source), new_src);
                    load_line_to_input(g_edit_line);
                    mark_normals_dirty();
                    set_status("Commented out");
                }
            }
        }
        return;
    }

    /* Removed Ctrl-B, Ctrl-N, Ctrl-T, Ctrl-U, Ctrl-O, Ctrl-W — handled via g_cfg_items loop */

    if ((key == '=' || key == '+') && (editor_get_modifiers() & GLUT_ACTIVE_CTRL)) {
        if (g_use_accum) {
            for (int i = 0; i < ACCUM_STEP_COUNT - 1; i++) {
                if (g_accum_samples <= g_accum_steps[i]) {
                    g_accum_samples = g_accum_steps[i + 1];
                    break;
                }
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Accum samples: %d", g_accum_samples);
                set_status(msg);
            }
        }
        return;
    }

    if (key == KEY_CTRL_DASH || (key == '-' && (editor_get_modifiers() & GLUT_ACTIVE_CTRL))) {
        if (g_use_accum) {
            for (int i = ACCUM_STEP_COUNT - 1; i > 0; i--) {
                if (g_accum_samples >= g_accum_steps[i]) {
                    g_accum_samples = g_accum_steps[i - 1];
                    break;
                }
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Accum samples: %d", g_accum_samples);
                set_status(msg);
            }
        }
        return;
    }

    /* Ctrl-W removed from here, handled via g_cfg_items loop */

    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (sel_active() && !g_inserting) {
            int start = sel_lo();
            int hi = sel_hi();
            if (hi >= g_num_cmds)
                hi = g_num_cmds - 1;
            delete_cmd_range(start, hi - start + 1, "Deleted");
            return;
        }
        if (g_cursor_pos > 0 && g_input_len > 0) {
            memmove(&g_input[g_cursor_pos - 1], &g_input[g_cursor_pos],
                    (size_t)(g_input_len - g_cursor_pos + 1));
            g_input_len--;
            g_cursor_pos--;
            update_autocomplete();
        }
        return;
    }

    if (key == '\t') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
        }
        return;
    }

    if (key == '\r' || key == '\n') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
            return;
        }

        if (!g_inserting && g_edit_line < g_num_cmds) {
            int unmodified = 0;
            {
                const char *s = g_cmds[g_edit_line].source;
                while (*s && isspace((unsigned char)*s))
                    s++;
                int slen = (int)strlen(s);
                while (slen > 0 &&
                       (s[slen - 1] == ';' || isspace((unsigned char)s[slen - 1])))
                    slen--;
                if ((slen == g_input_len && strncmp(g_input, s, (size_t)slen) == 0) ||
                    g_input_len == 0)
                    unmodified = 1;
            }
            if (unmodified) {
                if (g_cursor_pos > 0)
                    g_edit_line++;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                clear_autocomplete_state();
                set_status("Insert mode");
                mark_normals_dirty();
                return;
            }
        }

        if (g_input_len > 0)
            push_undo_snapshot();

        if ((g_inserting || g_edit_line >= g_num_cmds) &&
            g_input_len > 0 && try_commit_block_structs()) {
            clear_autocomplete_state();
            return;
        }

        if (g_inserting) {
            if (g_input_len > 0) {
                GLCmd cmd;
                int parsed;
                int insert_idx = g_edit_line;
                ExprVar vis_vars[MAX_EXPR_VARS];
                int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

                memset(&cmd, 0, sizeof(cmd));
                if (num_vis_vars > 0) {
                    if (try_commit_var_statements()) {
                        clear_autocomplete_state();
                        return;
                    }
                    int saved_el = g_edit_line;
                    g_edit_line = insert_idx;
                    parsed = repl_parse_command_with_vars(g_input, &cmd, vis_vars, num_vis_vars);
                    g_edit_line = saved_el;
                    if (parsed)
                        rewrite_cmd_source_with_indent(&cmd, insert_idx, 1);
                } else {
                    parsed = repl_parse_command(g_input, &cmd);
                }

                if (parsed && g_num_cmds < MAX_COMMANDS) {
                    for (int j = g_num_cmds; j > g_edit_line; j--)
                        g_cmds[j] = g_cmds[j - 1];
                    g_cmds[g_edit_line] = cmd;
                    g_num_cmds++;
                    g_edit_line++;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    set_status("Inserted");
                }
            } else {
                g_inserting = 0;
                if (g_edit_line <= g_num_cmds)
                    load_line_to_input(g_edit_line);
            }
        } else if (g_edit_line < g_num_cmds) {
            int can_advance = 1;

            if (g_input_len > 0) {
                if (g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                    if (try_commit_for_loop())
                        return;
                    can_advance = 0;
                }
                if (g_cmds[g_edit_line].type == CMD_FUNC_DEF) {
                    if (try_commit_func_def())
                        return;
                    can_advance = 0;
                }
                if (g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
                    if (try_commit_if_block())
                        return;
                    can_advance = 0;
                }
                if (try_commit_var_statements_then_insert())
                    return;

                GLCmd cmd;
                int parsed = parse_for_overwrite_enter(&cmd, g_edit_line);
                if (parsed)
                    g_cmds[g_edit_line] = cmd;
                else
                    can_advance = 0;
            }

            if (can_advance) {
                g_edit_line++;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                set_status("Insert mode");
            }
        } else {
            if (g_input_len > 0) {
                GLCmd cmd;
                int parsed = parse_for_overwrite_enter(&cmd, g_num_cmds);

                if (parsed && g_num_cmds < MAX_COMMANDS) {
                    g_cmds[g_num_cmds++] = cmd;
                    g_edit_line = g_num_cmds;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    g_newline_buf[0] = '\0';
                    g_newline_len = 0;
                    set_status("OK");
                }
            }
        }
        clear_autocomplete_state();
        mark_normals_dirty();
        return;
    }

    if (key == ';') {
        if (g_input_len > 0) {
            push_undo_snapshot();
            if (try_commit_any()) {
                clear_autocomplete_state();
                return;
            }
            {
                GLCmd cmd;
                int insert_idx = g_inserting ? g_edit_line :
                           (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
                int parsed;
                ExprVar vis_vars[MAX_EXPR_VARS];
                int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

                memset(&cmd, 0, sizeof(cmd));
                if (num_vis_vars > 0)
                    parsed = repl_parse_and_normalize(g_input, insert_idx, vis_vars, num_vis_vars, 1, &cmd);
                else
                    parsed = repl_parse_and_normalize(g_input, insert_idx, NULL, 0,
                                                      input_has_predef_vars(g_input), &cmd);

                if (parsed) {
                    if (g_inserting) {
                        if (g_num_cmds < MAX_COMMANDS) {
                            for (int j = g_num_cmds; j > g_edit_line; j--)
                                g_cmds[j] = g_cmds[j - 1];
                            g_cmds[g_edit_line] = cmd;
                            g_num_cmds++;
                            g_edit_line++;
                            g_input[0] = '\0';
                            g_input_len = 0;
                            g_cursor_pos = 0;
                            set_status("Inserted");
                        } else {
                            set_status("Command buffer full!");
                        }
                    } else if (g_edit_line < g_num_cmds) {
                        g_cmds[g_edit_line] = cmd;
                        set_status("Line updated");
                        g_edit_line++;
                        load_line_to_input(g_edit_line);
                    } else {
                        if (g_num_cmds < MAX_COMMANDS) {
                            g_cmds[g_num_cmds++] = cmd;
                            g_edit_line = g_num_cmds;
                            set_status("OK");
                            g_input[0] = '\0';
                            g_input_len = 0;
                            g_cursor_pos = 0;
                            g_newline_buf[0] = '\0';
                            g_newline_len = 0;
                        } else {
                            set_status("Command buffer full!");
                        }
                    }
                }
            }
        }
        clear_autocomplete_state();
        mark_normals_dirty();
        return;
    }

    if (key == KEY_CTRL_Q) {
        repl_save_output(quit_tempfile);
        printf("Saved to %s\n", quit_tempfile);
        exit(0);
    }

    if (key >= 32 && key < 127 && g_input_len < MAX_INPUT_LEN - 2) {
        memmove(&g_input[g_cursor_pos + 1], &g_input[g_cursor_pos],
                (size_t)(g_input_len - g_cursor_pos + 1));
        g_input[g_cursor_pos] = (char)key;
        g_input_len++;
        g_cursor_pos++;
        update_autocomplete();
    }
}

static void special_func(int key, int x, int y) {
    (void)x;
    (void)y;

    g_cursor_on = 1;
    g_blink_tick = 0;
    g_scroll_follow_cursor = 1;

    /* Swallow special keys (arrows, F-keys, etc.) while inline rename is
     * active so navigation doesn't leak through the overlay. */
    if (ui_panels_handle_rename_special(key))
        return;

    if (replay_handle_special_key(key))
        return;

    if (editor_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_special_restores_hidden_code_panel(key, key_mods))
            editor_restore_hidden_code_panel();
    }

    if (handle_search_special(key))
        return;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].is_special && g_cfg_items[i].key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return;
        }
    }

    switch (key) {
    case GLUT_KEY_LEFT:
        if (editor_get_modifiers() & GLUT_ACTIVE_CTRL) {
            repl_audio_prev_track();
            break;
        }
        if (g_show_help) {
            if (g_help_tab > 0) {
                g_help_tab--;
                g_help_scroll = 0;
            }
            break;
        }
        if (g_cursor_pos > 0)
            g_cursor_pos--;
        update_autocomplete();
        break;
    case GLUT_KEY_RIGHT:
        if (editor_get_modifiers() & GLUT_ACTIVE_CTRL) {
            repl_audio_next_track();
            break;
        }
        if (g_show_help) {
            if (g_help_tab < 1) {
                g_help_tab++;
                g_help_scroll = 0;
            }
            break;
        }
        if (g_cursor_pos < g_input_len)
            g_cursor_pos++;
        update_autocomplete();
        break;
    case GLUT_KEY_HOME:
        g_cursor_pos = 0;
        update_autocomplete();
        break;
    case GLUT_KEY_END:
        g_cursor_pos = g_input_len;
        update_autocomplete();
        break;
    case GLUT_KEY_UP:
        if (g_show_help) {
            g_help_scroll--;
            break;
        }
        if (g_ac_count > 1) {
            g_ac_sel = (g_ac_sel - 1 + g_ac_count) % g_ac_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!sel_active()) {
                g_sel_anchor = g_edit_line;
                g_sel_end = g_edit_line;
            }
            if (g_sel_end > 0)
                g_sel_end--;
            navigate_to_line(g_sel_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line - 1);
        }
        break;
    case GLUT_KEY_DOWN:
        if (g_show_help) {
            g_help_scroll++;
            break;
        }
        if (g_ac_count > 1) {
            g_ac_sel = (g_ac_sel + 1) % g_ac_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!sel_active()) {
                g_sel_anchor = g_edit_line;
                g_sel_end = g_edit_line;
            }
            if (g_sel_end < g_num_cmds - 1)
                g_sel_end++;
            navigate_to_line(g_sel_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line + 1);
        }
        break;
    case GLUT_KEY_F1:
        g_show_help = !g_show_help;
        g_help_tab = 0;
        g_help_scroll = 0;
        break;
    /* F2 through F11 removed from here, handled via g_cfg_items loop */
    case GLUT_KEY_F12: {
        /* F12 cycles: examples[0..N-1] → user scenes (in slot order) → back.
         * Active example -> next example, or first user scene if we ran
         * out of examples.  Active user scene -> next occupied user slot,
         * or back to example 0 when all user slots seen. */
        int count = repl_example_count();
        int active_scene = repl_active_user_scene();

        if (active_scene >= 0) {
            for (int s = active_scene + 1; s < MAX_USER_SCENES; s++) {
                if (repl_user_scene_slot_used(s)) {
                    repl_load_user_scene_idx(s);
                    goto f12_done;
                }
            }
            if (count > 0) repl_load_example(0);
            goto f12_done;
        }

        if (count > 0) {
            int next = g_example_idx + 1;
            if (next < count) {
                repl_load_example(next);
                goto f12_done;
            }
        }

        /* Past the last example: jump to first occupied user slot. */
        for (int s = 0; s < MAX_USER_SCENES; s++) {
            if (repl_user_scene_slot_used(s)) {
                repl_load_user_scene_idx(s);
                goto f12_done;
            }
        }
        if (count > 0) repl_load_example(0);
    f12_done:;
        break;
    }
    case GLUT_KEY_PAGE_UP:
        if (g_show_help)
            g_help_scroll -= 5;
        else
            g_scroll -= 5;
        g_scroll_follow_cursor = 0;
        break;
    case GLUT_KEY_PAGE_DOWN:
        if (g_show_help)
            g_help_scroll += 5;
        else
            g_scroll += 5;
        g_scroll_follow_cursor = 0;
        break;
    default:
        break;
    }
}

/* Cycle a config row by `delta` (+1 for forward, -1 for reverse) and
 * apply any per-item side effects. Right-click in the config menu
 * reuses this with delta=-1 so the user can seek back after
 * overshooting an entry like Grid or Axes. */
void repl_cfg_cycle_row(int row, int delta) {
    if (row < 0 || row >= CFG_ITEM_COUNT) return;

    /* Replay is special-cased: its cfg toggle kicks off/ends the
     * replay machinery rather than flipping the int directly. Both
     * directions collapse to "toggle". */
    if (g_cfg_items[row].value == &g_replay_active) {
        if (g_replay_active) {
            replay_stop();
            set_status("Replay: off");
        } else {
            replay_start();
        }
        return;
    }

    if (g_cfg_items[row].value == &g_t_playing) {
        if (glutGetModifiers() & GLUT_ACTIVE_SHIFT) {
            repl_reset_time_to_zero();
            set_status(g_t_playing ? "Time: reset to 0" : "Time: reset to 0 (paused)");
            return;
        }
    }

    if (g_replay_active)
        replay_stop();

    if (g_cfg_items[row].value == NULL) return;

    int n = g_cfg_items[row].n_states;
    if (n < 2) return;
    int v = (*g_cfg_items[row].value + delta) % n;
    if (v < 0) v += n;
    *g_cfg_items[row].value = v;

    if (g_cfg_items[row].value == &g_code_panel_layout) {
        g_panel_frac = 0.3f;
        if (g_code_panel_layout == CODE_PANEL_LAYOUT_TOP)
            set_status("Layout: top code panel");
        else if (g_code_panel_layout == CODE_PANEL_LAYOUT_BOTTOM)
            set_status("Layout: bottom code panel");
        else if (g_code_panel_layout == CODE_PANEL_LAYOUT_HIDDEN) {
            ui_panels_close_menus();
            clear_autocomplete_state();
            set_status("Layout: code panel hidden");
        } else
            set_status("Layout: left code panel");
    } else if (g_cfg_items[row].value == &g_autonormal) {
        if (g_autonormal) {
            mark_normals_dirty();
            set_status("Auto-normals: ON");
        } else {
            set_status("Auto-normals: OFF (existing normals kept)");
        }
    } else if (g_cfg_items[row].value == &g_init_attenuate_points) {
        apply_init_bootstrap();
        set_status(g_init_attenuate_points ? "Point attenuation: ON"
                                           : "Point attenuation: OFF");
    } else if (g_cfg_items[row].value == &g_audio_cfg_mode) {
        apply_audio_cfg_mode(g_audio_cfg_mode);
        repl_audio_set_cfg_mode(g_audio_cfg_mode);  /* keep INI in sync */
        static const char *labels[] = {
            "Audio: paused",
            "Audio: play once",
            "Audio: loop song",
            "Audio: loop all",
        };
        set_status(labels[g_audio_cfg_mode]);
    } else if (g_cfg_items[row].state_names) {
        snprintf(g_scratch_buf, sizeof(g_scratch_buf), "%s: %s",
                 g_cfg_items[row].label, g_cfg_items[row].state_names[v]);
        set_status(g_scratch_buf);
    } else if (n == 2) {
        snprintf(g_scratch_buf, sizeof(g_scratch_buf), "%s: %s",
                 g_cfg_items[row].label, v ? "ON" : "OFF");
        set_status(g_scratch_buf);
    }
}

static int editor_code_panel_layout(void) {
    if (g_code_panel_layout < 0 || g_code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return g_code_panel_layout;
}

static int editor_code_panel_hidden(void) {
    return editor_code_panel_layout() == CODE_PANEL_LAYOUT_HIDDEN;
}

static int editor_restore_hidden_code_panel(void) {
    if (!editor_code_panel_hidden())
        return 0;
    g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_panels_close_menus();
    return 1;
}

static int editor_key_restores_hidden_code_panel(unsigned char key, int mods) {
    if (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_ALT))
        return 0;
    return key == KEY_BACKSPACE ||
           key == KEY_DELETE ||
           key == '\t' ||
           key == '\r' ||
           key == '\n' ||
           (key >= 32 && key < 127);
}

static int editor_special_restores_hidden_code_panel(int key, int mods) {
    if (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_ALT))
        return 0;
    return key == GLUT_KEY_LEFT ||
           key == GLUT_KEY_RIGHT ||
           key == GLUT_KEY_UP ||
           key == GLUT_KEY_DOWN ||
           key == GLUT_KEY_HOME ||
           key == GLUT_KEY_END ||
           key == GLUT_KEY_PAGE_UP ||
           key == GLUT_KEY_PAGE_DOWN;
}

static int editor_point_in_code_panel(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = g_win_h - y;

    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    return x >= cp_x && x < cp_x + cp_w &&
           gl_y >= cp_y && gl_y < cp_y + cp_h;
}

static int editor_point_on_code_panel_divider(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = g_win_h - y;
    int layout = editor_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (layout == CODE_PANEL_LAYOUT_TOP)
        return abs(gl_y - cp_y) < 10;
    if (layout == CODE_PANEL_LAYOUT_BOTTOM)
        return abs(gl_y - (cp_y + cp_h)) < 10;
    return abs(x - (cp_x + cp_w)) < 10;
}

static int editor_code_panel_resize_cursor(void) {
    return editor_code_panel_layout() == CODE_PANEL_LAYOUT_LEFT
         ? GLUT_CURSOR_LEFT_RIGHT
         : GLUT_CURSOR_UP_DOWN;
}

static void editor_update_panel_frac_from_mouse(int x, int y) {
    int layout = editor_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        return;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        if (g_win_h > 0)
            g_panel_frac = (float)y / (float)g_win_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        if (g_win_h > 0)
            g_panel_frac = (float)(g_win_h - y) / (float)g_win_h;
    } else {
        if (g_win_w > 0)
            g_panel_frac = (float)x / (float)g_win_w;
    }

    if (g_panel_frac < 0.1f)
        g_panel_frac = 0.1f;
    if (g_panel_frac > 0.9f)
        g_panel_frac = 0.9f;
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_UP) {
        ui_panels_handle_mouse_release();
        if (g_drag_var >= 0) {
            g_drag_var = -1;
            g_drag_log_mode = 0;
            glutPostRedisplay();
            return;
        }
        if (g_resizing_panel) {
            g_resizing_panel = 0;
            glutSetCursor(GLUT_CURSOR_INHERIT);
            glutPostRedisplay();
            return;
        }
    }

    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        if (g_show_var_panel) {
            int row;
            if (var_panel_hit(x, y, &row)) {
                if (g_replay_active)
                    replay_stop();
                g_drag_var = row;
                g_drag_start_val = g_predef_vars[row].value;
                g_drag_start_x = x;
                glutPostRedisplay();
                return;
            }
        }

        /* The example dropdown can extend outside the code panel bounds (e.g.
         * below the panel in vertical layout).  Handle it before the
         * panel-area gate so clicks on any part of the dropdown register. */
        if (example_dropdown_is_open()) {
            int panel_actions = handle_code_panel_press(x, y);
            if (panel_actions & UI_PANEL_PRESS_OPENED_COLOR_PICKER)
                push_undo_snapshot();
            glutPostRedisplay();
            return;
        }

        if (editor_point_on_code_panel_divider(x, y)) {
            g_resizing_panel = 1;
            glutSetCursor(editor_code_panel_resize_cursor());
            return;
        }
        if (editor_point_in_code_panel(x, y)) {
            int panel_actions = handle_code_panel_press(x, y);
            if (panel_actions & UI_PANEL_PRESS_OPENED_COLOR_PICKER)
                push_undo_snapshot();
            glutPostRedisplay();
            return;
        }
        /* Scene-area click: let the color picker intercept before camera. */
        if (ui_panels_handle_scene_press(x, y)) {
            glutPostRedisplay();
            return;
        }
    }

    /* Right-click inside the Config dropdown cycles the item backward;
     * missed clicks leave the menu open so the user can keep seeking. */
    if (button == GLUT_RIGHT_BUTTON && state == GLUT_DOWN) {
        if (ui_panels_handle_right_press(x, y)) {
            glutPostRedisplay();
            return;
        }
    }

    /* Right-click on var panel: logarithmic drag mode. */
    if (button == GLUT_RIGHT_BUTTON && state == GLUT_DOWN && g_show_var_panel) {
        int row;
        if (var_panel_hit(x, y, &row)) {
            if (g_replay_active)
                replay_stop();
            g_drag_var = row;
            g_drag_log_mode = 1;
            g_drag_start_val = g_predef_vars[row].value;
            g_drag_start_x = x;
            glutPostRedisplay();
            return;
        }
    }

    // cache modifiers since can't be queried outside of an input event callback
    g_mouse_mods = editor_get_modifiers();

    if (state == GLUT_DOWN) {
        g_mouse_btn = button;
        g_mouse_x = x;
        g_mouse_y = y;
        g_vel_ry = g_vel_rx = g_vel_tx = g_vel_ty = g_vel_tz = g_vel_zoom = 0.0f;
    } else {
        g_vel_ry = fabsf(g_vel_ry) > CAM_MOMENTUM_THRESHOLD ? g_vel_ry : 0.0f;
        g_vel_rx = fabsf(g_vel_rx) > CAM_MOMENTUM_THRESHOLD ? g_vel_rx : 0.0f;
        g_vel_tx = fabsf(g_vel_tx) > CAM_MOMENTUM_THRESHOLD ? g_vel_tx : 0.0f;
        g_vel_ty = fabsf(g_vel_ty) > CAM_MOMENTUM_THRESHOLD ? g_vel_ty : 0.0f;
        g_vel_tz = fabsf(g_vel_tz) > CAM_MOMENTUM_THRESHOLD ? g_vel_tz : 0.0f;
        g_vel_zoom = fabsf(g_vel_zoom) > CAM_MOMENTUM_THRESHOLD ? g_vel_zoom : 0.0f;
        g_mouse_btn = -1;
    }

#ifdef USE_GLUT
    if (button == 3 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll--;
        } else {
            if (editor_point_in_code_panel(x, y))
                g_scroll--;
            else
                g_vel_zoom -= 0.3f;
        }
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll++;
        } else {
            if (editor_point_in_code_panel(x, y))
                g_scroll++;
            else
                g_vel_zoom += 0.3f;
        }
        glutPostRedisplay();
    }
#endif
}

#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel;
    if (g_show_help) {
        g_help_scroll -= direction;
    } else {
        if (editor_point_in_code_panel(x, y))
            g_scroll -= direction;
        else
            g_vel_zoom -= direction * 0.1f;
    }
    glutPostRedisplay();
}
#endif

static void passive_motion_func(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;

    if (editor_point_on_code_panel_divider(x, y))
        glutSetCursor(editor_code_panel_resize_cursor());
    else
        glutSetCursor(GLUT_CURSOR_INHERIT);
}

static void motion_func(int x, int y) {
    if (ui_panels_handle_motion(x, y)) {
        g_mouse_x = x;  g_mouse_y = y;
        glutPostRedisplay();
        return;
    }

    int dx = x - g_mouse_x;
    int dy = y - g_mouse_y;

    if (g_resizing_panel) {
        editor_update_panel_frac_from_mouse(x, y);
        glutPostRedisplay();
        return;
    }

    if (g_drag_var >= 0) {
        float new_val;
        if (g_drag_log_mode) {
            /* Logarithmic drag: ×10 / ÷10 per 200 pixels.
             * Preserves sign; near-zero start falls back to linear bootstrap. */
            float dx_total = (float)(x - g_drag_start_x);
            float mag = fabsf(g_drag_start_val);
            if (mag < 1e-6f) {
                /* Bootstrap from zero: treat first pixels as linear, then log. */
                new_val = dx_total * 0.001f;
            } else {
                float sign = (g_drag_start_val >= 0.0f) ? 1.0f : -1.0f;
                new_val = sign * mag * expf(dx_total * (logf(10.0f) / 200.0f));
            }
        } else {
            float delta = (float)(x - g_drag_start_x) * 0.05f;
            new_val = g_drag_start_val + delta;
        }
        g_predef_vars[g_drag_var].value = new_val;
        {
            const char *vname = g_predef_vars[g_drag_var].name;
            for (int i = 0; i < g_num_cmds; i++) {
                if (g_cmds[i].valid && g_cmds[i].type == CMD_VAR_ASSIGN &&
                    g_cmds[i].num_args == g_drag_var &&
                    !g_cmds[i].has_vars) {
                    g_cmds[i].args[0] = new_val;
                    snprintf(g_cmds[i].source, sizeof(g_cmds[i].source),
                             "  %s = %g;", vname, (double)new_val);
                }
            }
        }
        g_flat_dirty = 1;
        g_mouse_x = x;
        g_mouse_y = y;
        glutPostRedisplay();
        return;
    }

    if (handle_code_panel_drag(x, y)) {
        g_mouse_x = x;
        g_mouse_y = y;
        glutPostRedisplay();
        return;
    }

    if (g_mouse_btn == GLUT_LEFT_BUTTON) {
        g_cam_ry += (float)dx * 0.5f;
        g_cam_rx += (float)dy * 0.5f;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
        if (g_cam_rx > 89.0f)
            g_cam_rx = 89.0f;
        if (g_cam_rx < -89.0f)
            g_cam_rx = -89.0f;
        g_vel_rx *= CAM_DECAY;
        g_vel_ry *= CAM_DECAY;
        g_vel_ry += (float)dx * 0.25f;
        g_vel_rx += (float)dy * 0.25f;
    } else if (g_mouse_btn == GLUT_RIGHT_BUTTON) {
        float scale = 0.005f * g_cam_dist;
        float fdy = (float)dy;
        if (g_mouse_mods & GLUT_ACTIVE_SHIFT) {
            /* Shift + right-drag: pan the orbit target along world Y. */
            float wdy = -fdy * scale;
            g_cam_ty -= wdy;
            g_vel_ty *= CAM_DECAY;
            g_vel_ty += wdy * 0.5f;
            g_cam_motion_glow = 1.0f;
        } else {
            /* Pan the orbit target along the world XZ ground plane.
             *
             * Camera transform is Rx(rx)·Ry(ry)·T(-target), so a view-space
             * direction v maps back to world by Ry(-ry)·Rx(-rx)·v. Projected
             * onto the ground (Y=0):
             *   right_xz   = (+cos ry, 0, +sin ry)   (screen +X)
             *   forward_xz = (+sin ry, 0, -cos ry)   (screen -Y / mouse-up)
             *
             * Mouse-down (dy < 0) pulls the target back toward the camera,
             * so we subtract forward_xz * dy. World Y is preserved. */
            float ry_rad = g_cam_ry * (float)M_PI / 180.0f;
            float cry = cosf(ry_rad), sry = sinf(ry_rad);
            float fdx = (float)dx;
            float wdx = ( fdx * cry - fdy * sry) * scale;
            float wdz = ( fdx * sry + fdy * cry) * scale;
            g_cam_tx -= wdx;
            g_cam_tz -= wdz;
            g_vel_tx *= CAM_DECAY;
            g_vel_tz *= CAM_DECAY;
            g_vel_tx += wdx * 0.5f;
            g_vel_tz += wdz * 0.5f;
            g_cam_motion_glow = 1.0f;
        }
    } else if (g_mouse_btn == GLUT_MIDDLE_BUTTON) {
        g_cam_dist += (float)dy * 0.02f;
        if (g_cam_dist < 0.5f)
            g_cam_dist = 0.5f;
        if (g_cam_dist > 50.0f)
            g_cam_dist = 50.0f;
    }

    g_mouse_x = x;
    g_mouse_y = y;
}

static void timer_func(int value) {
    (void)value;

    /* Advance the audio playlist if the current song reached its end
     * (no-op under loop=Song; see repl_audio_tick). */
    repl_audio_tick();

    /* When the playing track changes (either auto-advance from tick
     * or manual next/prev), surface the song name in the status bar.
     * Tracking by generation avoids needing a callback hook into
     * the audio module. */
    {
        static unsigned int last_track_gen = 0;
        unsigned int gen = repl_audio_track_generation();
        if (gen != last_track_gen) {
            last_track_gen = gen;
            const char *path = repl_audio_get_current_track();
            if (path && *path) {
                const char *base = strrchr(path, '/');
                base = base ? base + 1 : path;
                char msg[128];
                snprintf(msg, sizeof(msg), "Now playing: %s", base);
                set_status(msg);
            }
        }
    }

    repl_advance_time(0.016f);

    if (g_replay_active)
        replay_tick_fade_batches(0.016f);

    if (g_replay_active && g_replay_state == REPLAY_PLAYING) {
        g_replay_accum += g_replay_speed * 0.016f;
        while (g_replay_accum >= 1.0f && g_replay_state == REPLAY_PLAYING) {
            g_replay_accum -= 1.0f;
            replay_advance();
        }
    }

    if (g_mouse_btn == -1) {
        g_cam_ry += g_vel_ry;
        g_cam_rx += g_vel_rx;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
        if (g_cam_rx > 89.0f) {
            g_cam_rx = 89.0f;
            g_vel_rx = 0.0f;
        }
        if (g_cam_rx < -89.0f) {
            g_cam_rx = -89.0f;
            g_vel_rx = 0.0f;
        }
        g_cam_tx += g_vel_tx;
        g_cam_ty += g_vel_ty;
        g_cam_tz += g_vel_tz;
        g_cam_dist += g_vel_zoom;
        if (g_cam_dist < 0.5f) {
            g_cam_dist = 0.5f;
            g_vel_zoom = 0.0f;
        }
        if (g_cam_dist > 50.0f) {
            g_cam_dist = 50.0f;
            g_vel_zoom = 0.0f;
        }
    }

    g_vel_ry *= CAM_DECAY;
    g_vel_rx *= CAM_DECAY;
    g_vel_tx *= CAM_DECAY;
    g_vel_ty *= CAM_DECAY;
    g_vel_tz *= CAM_DECAY;
    g_vel_zoom *= CAM_DECAY_ZOOM;

    /* Gizmo is a pan-only affordance. Keep it lit while pan momentum
     * carries the target, then fade out. Rotate/zoom do not trigger it. */
    float pan_vel = fabsf(g_vel_tx) + fabsf(g_vel_ty) + fabsf(g_vel_tz);
    if (pan_vel > 0.01f && g_cam_motion_glow < 0.6f)
        g_cam_motion_glow = 0.6f;
    g_cam_motion_glow *= 0.94f;
    if (g_cam_motion_glow < 0.005f) g_cam_motion_glow = 0.0f;

    if (g_cam_rotate) {
        g_cam_ry += 0.3f;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
    }

    g_blink_tick++;
    if (g_blink_tick >= 30) {
        g_blink_tick = 0;
        g_cursor_on = !g_cursor_on;
    }

    if (g_status_ttl > 0)
        g_status_ttl--;

    glutPostRedisplay();
    glutTimerFunc(16, timer_func, 0);
}

int feed_line(const char *line) {
    strncpy(g_input, line, MAX_INPUT_LEN - 1);
    g_input[MAX_INPUT_LEN - 1] = '\0';
    g_input_len = (int)strlen(g_input);
    g_cursor_pos = g_input_len;

    if (try_commit_any())
        return 1;

    {
        int handled = 0;
        GLCmd cmd;
        int insert_idx = g_inserting ? g_edit_line :
                   (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        int parsed;
        ExprVar vis_vars[MAX_EXPR_VARS];
        int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

        memset(&cmd, 0, sizeof(cmd));
        if (num_vis_vars > 0)
            parsed = repl_parse_and_normalize(g_input, insert_idx, vis_vars, num_vis_vars, 1, &cmd);
        else
            parsed = repl_parse_and_normalize(g_input, insert_idx, NULL, 0,
                                              input_has_predef_vars(g_input), &cmd);

        if (parsed && g_num_cmds < MAX_COMMANDS) {
            if (g_inserting) {
                for (int j = g_num_cmds; j > g_edit_line; j--)
                    g_cmds[j] = g_cmds[j - 1];
                g_cmds[g_edit_line] = cmd;
                g_num_cmds++;
                g_edit_line++;
            } else if (g_edit_line < g_num_cmds) {
                g_cmds[g_edit_line] = cmd;
                g_edit_line++;
            } else {
                g_cmds[g_num_cmds++] = cmd;
                g_edit_line = g_num_cmds;
            }
            depth_cache_invalidate();
            handled = 1;
        }
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        return handled;
    }
}

void repl_navigate_to_line(int target) {
    navigate_to_line(target);
}

static void notify_audio_gesture_once(void) {
    if (g_audio_gesture_sent) return;
    g_audio_gesture_sent = 1;
    repl_audio_on_user_gesture();
}

void repl_keyboard_func(unsigned char key, int x, int y) {
    notify_audio_gesture_once();
    keyboard_func(key, x, y);
}

void repl_special_func(int key, int x, int y) {
    notify_audio_gesture_once();
    special_func(key, x, y);
}

void repl_mouse_func(int button, int state, int x, int y) {
    notify_audio_gesture_once();
    mouse_func(button, state, x, y);
}

void repl_motion_func(int x, int y) {
    motion_func(x, y);
}

void repl_passive_motion_func(int x, int y) {
    passive_motion_func(x, y);
}

#ifndef USE_GLUT
void repl_mousewheel_func(int wheel, int direction, int x, int y) {
    mousewheel_func(wheel, direction, x, y);
}
#endif

void repl_timer_func(int value) {
    timer_func(value);
}

void repl_feed_line_public(const char *line) {
    feed_line(line);
}

/* Apply side effects for all cfg items whose defaults need honouring at
 * startup.  Call this once after every subsystem (audio, GL, etc.) has
 * finished initialising so the initial values of g_cfg_items entries take
 * effect immediately.  Add a new branch here whenever a CfgItem is added
 * whose backing variable alone is not enough to drive the desired state. */
void repl_editor_apply_defaults(void) {
    /* Restore the audio mode persisted from the previous session.
     * repl_audio_play_playlist() calls load_state() which stores the
     * cfg_mode in the audio module; we pull it here so both g_audio_cfg_mode
     * (the UI) and the actual audio engine agree before the first frame. */
    int saved_mode = repl_audio_get_cfg_mode();
    if (saved_mode >= AUDIO_CFG_PAUSE && saved_mode <= AUDIO_CFG_ALL)
        g_audio_cfg_mode = saved_mode;
    apply_audio_cfg_mode(g_audio_cfg_mode);
    /* Push the (possibly-restored) mode back so the audio module's copy
     * is valid immediately and the next save reflects the current state. */
    repl_audio_set_cfg_mode(g_audio_cfg_mode);
}
