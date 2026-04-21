/*
 * repl_editor.c — Editor state, line routing, and GLUT input dispatch.
 *
 * Subsystems in this file (top to bottom):
 *  - Editor state (g_input, config-item table)
 *  - Cmd-range deletion with var-decl guards
 *  - Line-input load/save and line navigation
 *  - Commit attempt orchestration and Enter/navigation outcomes
 *  - GLUT keyboard / special / mouse / wheel / motion / timer callbacks
 *  - feed_line() — the programmatic commit entry point
 *  - Public repl_*_func() wrappers forwarded from sample.c
 *
 * Shared state (g_input, g_edit_line, g_num_cmds, g_cmds[], etc.) is
 * declared in sample.h. This file OWNS the editor-side globals listed
 * above; commit handlers live in repl_commit.c and parser/executor side
 * lives in repl_core.c / repl_executor.c.
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_camera_controls.h"
#include "repl_clipboard.h"
#include "repl_undo.h"
#include "repl_replay.h"
#include "repl_keys.h"
#include "ui_panels.h"
#include "repl_audio.h"

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */
static void save_newline_buf(void);
void delete_cmd_range(int start, int count, const char *what);
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

typedef enum {
    COMMIT_UNCHANGED,
    COMMIT_OK,
    COMMIT_REJECTED
} CommitResult;

char g_input[MAX_INPUT_LEN];
int  g_input_len = 0;
int  g_cursor_pos = 0;
int  g_edit_line = 0;
char g_newline_buf[MAX_INPUT_LEN] = "";
int  g_newline_len = 0;
int  g_inserting = 0;

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

static const int g_accum_steps[] = { 1, 2, 4, 8, 16 };
static const char *quit_tempfile = "/tmp/temp-output.c";

static int delete_cmd_range_allowed(int start, int count) {
    if (repl_selection_cmd_range_contains_var_decl(start, count)) {
        repl_selection_set_var_decl_action_status("remove");
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
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_delete_range(&store, start, count);

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
    if (!repl_selection_normalize_cmd_range(start, count, &start, &count))
        return;
    if (!delete_cmd_range_allowed(start, count))
        return;
    remove_cmd_range_unchecked(start, count, what);
}

void repl_clear_all_cmds(void) {
    push_undo_snapshot();
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_clear(&store);
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

void repl_editor_reset_transients(void) {
    repl_commit_reset_transients();
    repl_camera_controls_reset();
}

static int normalize_navigation_target(int target) {
    target = repl_commit_resolve_insert_exit_target(target);
    if (target < 0)
        target = 0;
    if (target > g_num_cmds)
        target = g_num_cmds;
    return target;
}

static void navigate_to_line_raw_resolved(int target) {
    if (target == g_edit_line && !g_inserting)
        return;

    if (g_edit_line == g_num_cmds && !g_inserting)
        save_newline_buf();

    g_edit_line = target;
    g_inserting = 0;
    load_line_to_input(target);
    clear_autocomplete_state();
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

typedef struct {
    ReplUndoSnapshot undo;
    char input[MAX_INPUT_LEN];
    int input_len;
    int cursor_pos;
    int inserting;
    char newline_buf[MAX_INPUT_LEN];
    int newline_len;
} CommitAttemptState;

static CommitAttemptState g_commit_attempt_before;
static CommitAttemptState g_navigation_commit_before;

static void capture_commit_attempt_state(CommitAttemptState *s) {
    repl_undo_snapshot_save(&s->undo);
    memcpy(s->input, g_input, sizeof(s->input));
    s->input_len = g_input_len;
    s->cursor_pos = g_cursor_pos;
    s->inserting = g_inserting;
    memcpy(s->newline_buf, g_newline_buf, sizeof(s->newline_buf));
    s->newline_len = g_newline_len;
}

/* Navigation rejection reverts commands/predefs and the saved append-line
 * buffer.  The transient typed input stays discarded by the undo snapshot
 * restore; captured input fields are used only to detect commit progress. */
static void restore_commit_attempt_committed_state(const CommitAttemptState *s) {
    memcpy(g_newline_buf, s->newline_buf, sizeof(g_newline_buf));
    g_newline_len = s->newline_len;
    repl_undo_snapshot_restore(&s->undo);
}

static int input_matches_committed_line(int line) {
    if (line < 0 || line >= g_num_cmds)
        return 0;

    const char *s = g_cmds[line].source;
    while (*s && isspace((unsigned char)*s))
        s++;

    int slen = (int)strlen(s);
    while (slen > 0 &&
           (s[slen - 1] == ';' || isspace((unsigned char)s[slen - 1])))
        slen--;

    return slen == g_input_len && strncmp(g_input, s, (size_t)slen) == 0;
}

static int commit_progressed_since(const CommitAttemptState *s) {
    if (g_num_cmds != s->undo.num_cmds ||
        g_edit_line != s->undo.edit_line ||
        g_inserting != s->inserting ||
        g_input_len != s->input_len ||
        g_cursor_pos != s->cursor_pos)
        return 1;

    if (memcmp(g_input, s->input, (size_t)g_input_len + 1) != 0)
        return 1;

    if (g_num_cmds > 0 &&
        memcmp(g_cmds, s->undo.cmds,
               (size_t)g_num_cmds * sizeof(GLCmd)) != 0)
        return 1;

    return 0;
}

static int current_input_needs_navigation_commit(void) {
    if (g_input_len <= 0)
        return 0;
    if (!g_inserting && g_edit_line < g_num_cmds &&
        input_matches_committed_line(g_edit_line))
        return 0;
    return 1;
}

/* Shared line-commit path for Enter and navigation.  Enter keeps its
 * line-advance/insert-mode behavior for unchanged lines; navigation treats
 * unchanged input as a no-op and only uses this helper for modified text. */
static CommitResult commit_current_input(int enter_mode) {
    if (!enter_mode && !current_input_needs_navigation_commit())
        return COMMIT_UNCHANGED;

    if (!g_inserting && g_edit_line < g_num_cmds) {
        int unmodified = (g_input_len == 0 ||
                          input_matches_committed_line(g_edit_line));
        if (unmodified) {
            if (!enter_mode)
                return COMMIT_UNCHANGED;
            if (g_cursor_pos > 0)
                g_edit_line++;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            clear_autocomplete_state();
            set_status("Insert mode");
            mark_normals_dirty();
            return COMMIT_OK;
        }
    }

    if (g_input_len > 0)
        push_undo_snapshot();

    CommitAttemptState *before = &g_commit_attempt_before;
    capture_commit_attempt_state(before);

    if ((g_inserting || g_edit_line >= g_num_cmds) &&
        g_input_len > 0 && try_commit_block_structs()) {
        return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
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
                if (try_commit_var_statements())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                int saved_el = g_edit_line;
                g_edit_line = insert_idx;
                parsed = repl_parse_command_with_vars(g_input, &cmd, vis_vars, num_vis_vars);
                g_edit_line = saved_el;
                if (parsed)
                    rewrite_cmd_source_with_indent(&cmd, insert_idx, 1);
            } else {
                parsed = repl_parse_command(g_input, &cmd);
            }

            if (parsed) {
                ReplCommandStore store = repl_command_store_live();
                if (!repl_command_store_insert_one(&store, g_edit_line, &cmd, 0)) {
                    set_status("Command buffer full!");
                    return COMMIT_REJECTED;
                }
                g_edit_line++;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                set_status("Inserted");
                return COMMIT_OK;
            }
            return COMMIT_REJECTED;
        }

        if (enter_mode) {
            g_inserting = 0;
            if (g_edit_line <= g_num_cmds)
                load_line_to_input(g_edit_line);
            return COMMIT_OK;
        }
        return COMMIT_UNCHANGED;
    }

    if (g_edit_line < g_num_cmds) {
        int can_advance = 1;

        if (g_input_len > 0) {
            if (g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                if (try_commit_for_loop())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (g_cmds[g_edit_line].type == CMD_FUNC_DEF) {
                if (try_commit_func_def())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
                if (try_commit_if_block())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (try_commit_var_statements_then_insert())
                return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;

            GLCmd cmd;
            int parsed = parse_for_overwrite_enter(&cmd, g_edit_line);
            if (parsed) {
                ReplCommandStore store = repl_command_store_live();
                repl_command_store_replace_one(&store, g_edit_line, &cmd);
            } else {
                can_advance = 0;
            }
        }

        if (can_advance) {
            g_edit_line++;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            set_status("Insert mode");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    if (g_input_len > 0) {
        GLCmd cmd;
        int parsed = parse_for_overwrite_enter(&cmd, g_num_cmds);

        if (parsed) {
            ReplCommandStore store = repl_command_store_live();
            if (!repl_command_store_insert_one(&store, g_num_cmds, &cmd, 0)) {
                set_status("Command buffer full!");
                return COMMIT_REJECTED;
            }
            g_edit_line = g_num_cmds;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            g_newline_buf[0] = '\0';
            g_newline_len = 0;
            set_status("OK");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    return COMMIT_UNCHANGED;
}

static CommitResult commit_before_navigation(void) {
    CommitAttemptState *before = &g_navigation_commit_before;
    ReplUndoRingState undo_before;
    char rejected_status[sizeof(g_status)];
    int rejected_ttl;

    if (!current_input_needs_navigation_commit())
        return COMMIT_UNCHANGED;

    capture_commit_attempt_state(before);
    repl_undo_ring_state_capture(&undo_before);
    CommitResult result = commit_current_input(0);
    if (result != COMMIT_REJECTED)
        return result;

    memcpy(rejected_status, g_status, sizeof(rejected_status));
    rejected_ttl = g_status_ttl;
    restore_commit_attempt_committed_state(before);
    repl_undo_ring_state_restore(&undo_before);
    memcpy(g_status, rejected_status, sizeof(g_status));
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_ttl = rejected_ttl;
    clear_autocomplete_state();
    return COMMIT_REJECTED;
}

void navigate_to_line(int target) {
    target = normalize_navigation_target(target);
    if (target == g_edit_line && !g_inserting)
        return;

    if (target != g_edit_line)
        (void)commit_before_navigation();

    if (target > g_num_cmds)
        target = g_num_cmds;
    navigate_to_line_raw_resolved(target);
}

static void keyboard_begin_key(unsigned char key) {
    g_cursor_on = 1;
    g_blink_tick = 0;

    /* Cut / copy / backspace / delete preserve any active line-range
     * selection; everything else clears it before processing the key. */
    if (key != KEY_CTRL_C && key != KEY_CTRL_D && key != KEY_BACKSPACE &&
        key != KEY_CTRL_X && key != KEY_DELETE)
        clear_selection();

    g_scroll_follow_cursor = 1;
}

static int handle_rename_key_route(unsigned char key) {
    /* Rename overlay captures every keystroke while active, ahead of
     * the backtick/config, replay, and search branches — otherwise
     * typing `, or keys bound to replay would leak out of the rename
     * buffer and trigger unrelated UI. */
    return ui_panels_handle_rename_key(key);
}

static int handle_config_menu_key_route(unsigned char key) {
    if (!g_search_active && key == '`') {
        if (g_replay_active)
            replay_stop();
        editor_restore_hidden_code_panel();
        ui_panels_open_config();
        return 1;
    }
    return 0;
}

static int handle_active_replay_key_route(unsigned char key) {
    return g_replay_active && replay_handle_key(key);
}

static void restore_hidden_code_panel_for_key(unsigned char key) {
    if (editor_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_key_restores_hidden_code_panel(key, key_mods))
            editor_restore_hidden_code_panel();
    }
}

static int handle_search_key_route(unsigned char key) {
    return handle_search_key(key);
}

static int handle_escape_key_route(unsigned char key) {
    if (key == KEY_ESC) {
        if (ui_panels_handle_escape()) {
            glutPostRedisplay();
            return 1;
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
        return 1;
    }
    return 0;
}

static int handle_cfg_shortcut_key_route(unsigned char key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (!g_cfg_items[i].is_special && g_cfg_items[i].key_code > 0
                && g_cfg_items[i].key_code < 32 && g_cfg_items[i].key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return 1;
        }
    }
    return 0;
}

static int handle_cursor_endpoint_key_route(unsigned char key) {
    if (key == KEY_CTRL_A) {
        g_cursor_pos = 0;
        update_autocomplete();
        return 1;
    }
    if (key == KEY_CTRL_E) {
        g_cursor_pos = g_input_len;
        update_autocomplete();
        return 1;
    }
    return 0;
}

static int handle_undo_redo_key_route(unsigned char key) {
    if (key == KEY_CTRL_Z) {
        if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT)
            do_redo();
        else
            pop_undo_snapshot();
        return 1;
    }

    if (key == KEY_CTRL_Y) {
        do_redo();
        return 1;
    }
    return 0;
}

static int handle_replay_key_route(unsigned char key) {
    return replay_handle_key(key);
}

static int handle_line_delete_key_route(unsigned char key) {
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
        return 1;
    }
    return 0;
}

static int handle_buffer_command_key_route(unsigned char key) {
    if (key == KEY_CTRL_L) {
        repl_clear_all_cmds();
        return 1;
    }

    if (key == KEY_CTRL_BACKSLASH) {
        if (g_num_cmds > 0) {
            push_undo_snapshot();
            repl_reformat_commands();
            set_status("Reformatted command buffer");
        } else {
            set_status("Nothing to reformat");
        }
        return 1;
    }

    if (key == KEY_CTRL_P) {
        repl_debug_dump_editor(stdout);
        repl_debug_dump_flat_commands(stdout);
        set_status("Dumped editor + flat commands to stdout");
        return 1;
    }

    if (key == KEY_CTRL_S) {
        repl_save_default_output();
        return 1;
    }
    return 0;
}

static int handle_copy_key_route(unsigned char key) {
    if (key == KEY_CTRL_C) {
        repl_clipboard_copy_current();
        return 1;
    }
    return 0;
}

static int handle_cut_key_route(unsigned char key) {
    if (key == KEY_CTRL_X) {
        repl_clipboard_cut_current();
        return 1;
    }
    return 0;
}

static int handle_paste_key_route(unsigned char key) {
    if (key == KEY_CTRL_V) {
        repl_clipboard_paste_current();
        return 1;
    }
    return 0;
}

static int handle_comment_toggle_key_route(unsigned char key) {
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
                            ReplCommandStore store = repl_command_store_live();
                            repl_command_store_replace_one(&store, g_edit_line, &new_cmd);
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
        return 1;
    }
    return 0;
}

static int handle_accum_samples_key_route(unsigned char key) {
    if (key == '=' || key == '+') {
        int mods = editor_get_modifiers();
        if (!(mods & GLUT_ACTIVE_CTRL))
            return 0;
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
        return 1;
    }

    if (key == KEY_CTRL_DASH ||
        (key == '-' && (editor_get_modifiers() & GLUT_ACTIVE_CTRL))) {
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
        return 1;
    }
    return 0;
}

static int handle_text_delete_key_route(unsigned char key) {
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (sel_active() && !g_inserting) {
            int start = sel_lo();
            int hi = sel_hi();
            if (hi >= g_num_cmds)
                hi = g_num_cmds - 1;
            delete_cmd_range(start, hi - start + 1, "Deleted");
            return 1;
        }
        if (g_cursor_pos > 0 && g_input_len > 0) {
            memmove(&g_input[g_cursor_pos - 1], &g_input[g_cursor_pos],
                    (size_t)(g_input_len - g_cursor_pos + 1));
            g_input_len--;
            g_cursor_pos--;
            update_autocomplete();
        }
        return 1;
    }
    return 0;
}

static int handle_tab_key_route(unsigned char key) {
    if (key == '\t') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
        }
        return 1;
    }
    return 0;
}

static int handle_enter_key_route(unsigned char key) {
    if (key == '\r' || key == '\n') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
            return 1;
        }

        (void)commit_current_input(1);
        clear_autocomplete_state();
        mark_normals_dirty();
        return 1;
    }
    return 0;
}

static int handle_semicolon_commit_key_route(unsigned char key) {
    if (key == ';') {
        if (g_input_len > 0) {
            push_undo_snapshot();
            if (try_commit_any()) {
                clear_autocomplete_state();
                return 1;
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
                    parsed = repl_parse_and_normalize(g_input, insert_idx, vis_vars, num_vis_vars,
                                                      input_has_any_visible_vars(g_input, vis_vars, num_vis_vars),
                                                      &cmd);
                else
                    parsed = repl_parse_and_normalize(g_input, insert_idx, NULL, 0,
                                                      input_has_predef_vars(g_input), &cmd);

                if (parsed) {
                    ReplCommandStore store = repl_command_store_live();
                    if (g_inserting) {
                        if (repl_command_store_insert_one(&store, g_edit_line, &cmd, 0)) {
                            g_edit_line++;
                            g_input[0] = '\0';
                            g_input_len = 0;
                            g_cursor_pos = 0;
                            set_status("Inserted");
                        } else {
                            set_status("Command buffer full!");
                        }
                    } else if (g_edit_line < g_num_cmds) {
                        repl_command_store_replace_one(&store, g_edit_line, &cmd);
                        set_status("Line updated");
                        g_edit_line++;
                        load_line_to_input(g_edit_line);
                    } else {
                        if (repl_command_store_insert_one(&store, g_num_cmds, &cmd, 0)) {
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
        return 1;
    }
    return 0;
}

static int handle_quit_key_route(unsigned char key) {
    if (key == KEY_CTRL_Q) {
        repl_save_output(quit_tempfile);
        printf("Saved to %s\n", quit_tempfile);
        exit(0);
    }
    return 0;
}

static int handle_printable_input_key_route(unsigned char key) {
    if (key >= 32 && key < 127 && g_input_len < MAX_INPUT_LEN - 2) {
        memmove(&g_input[g_cursor_pos + 1], &g_input[g_cursor_pos],
                (size_t)(g_input_len - g_cursor_pos + 1));
        g_input[g_cursor_pos] = (char)key;
        g_input_len++;
        g_cursor_pos++;
        update_autocomplete();
        return 1;
    }
    return 0;
}

void keyboard_func(unsigned char key, int x, int y) {
    (void)x;
    (void)y;

    keyboard_begin_key(key);

    if (handle_rename_key_route(key))       return;
    if (handle_config_menu_key_route(key))  return;
    if (handle_active_replay_key_route(key)) return;

    restore_hidden_code_panel_for_key(key);

    if (handle_search_key_route(key))       return;
    if (handle_escape_key_route(key))       return;
    if (handle_cfg_shortcut_key_route(key)) return;
    if (handle_cursor_endpoint_key_route(key)) return;
    if (handle_undo_redo_key_route(key))    return;
    if (handle_replay_key_route(key))       return;
    if (handle_line_delete_key_route(key))  return;
    if (handle_buffer_command_key_route(key)) return;
    if (handle_copy_key_route(key))         return;
    if (handle_cut_key_route(key))          return;
    if (handle_paste_key_route(key))        return;
    if (handle_comment_toggle_key_route(key)) return;
    if (handle_accum_samples_key_route(key)) return;
    if (handle_text_delete_key_route(key))  return;
    if (handle_tab_key_route(key))          return;
    if (handle_enter_key_route(key))        return;
    if (handle_semicolon_commit_key_route(key)) return;
    if (handle_quit_key_route(key))         return;
    (void)handle_printable_input_key_route(key);
}

static void special_begin_key(int key) {
    (void)key;
    g_cursor_on = 1;
    g_blink_tick = 0;
    g_scroll_follow_cursor = 1;
}

static int handle_rename_special_route(int key) {
    /* Rename captures arrows and F-keys ahead of replay/search/navigation so
     * modal text entry cannot leak actions into the editor. */
    return ui_panels_handle_rename_special(key);
}

static int handle_replay_special_route(int key) {
    return replay_handle_special_key(key);
}

static void restore_hidden_code_panel_for_special(int key) {
    if (editor_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_special_restores_hidden_code_panel(key, key_mods))
            editor_restore_hidden_code_panel();
    }
}

static int handle_search_special_route(int key) {
    return handle_search_special(key);
}

static int handle_cfg_special_shortcut_route(int key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].is_special && g_cfg_items[i].key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return 1;
        }
    }
    return 0;
}

static int handle_horizontal_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_LEFT:
        if (editor_get_modifiers() & GLUT_ACTIVE_CTRL) {
            repl_audio_prev_track();
            return 1;
        }
        if (g_show_help) {
            if (g_help_tab > 0) {
                g_help_tab--;
                g_help_scroll = 0;
            }
            return 1;
        }
        if (g_cursor_pos > 0)
            g_cursor_pos--;
        update_autocomplete();
        return 1;
    case GLUT_KEY_RIGHT:
        if (editor_get_modifiers() & GLUT_ACTIVE_CTRL) {
            repl_audio_next_track();
            return 1;
        }
        if (g_show_help) {
            if (g_help_tab < 1) {
                g_help_tab++;
                g_help_scroll = 0;
            }
            return 1;
        }
        if (g_cursor_pos < g_input_len)
            g_cursor_pos++;
        update_autocomplete();
        return 1;
    case GLUT_KEY_HOME:
        g_cursor_pos = 0;
        update_autocomplete();
        return 1;
    case GLUT_KEY_END:
        g_cursor_pos = g_input_len;
        update_autocomplete();
        return 1;
    default:
        return 0;
    }
}

static int handle_vertical_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_UP:
        if (g_show_help) {
            g_help_scroll--;
            return 1;
        }
        if (g_ac_count > 1) {
            g_ac_sel = (g_ac_sel - 1 + g_ac_count) % g_ac_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!sel_active()) {
                repl_selection_start(g_edit_line);
            }
            int selection_end = repl_selection_end();
            if (selection_end > 0)
                selection_end--;
            repl_selection_set_end(selection_end);
            navigate_to_line(selection_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line - 1);
        }
        return 1;
    case GLUT_KEY_DOWN:
        if (g_show_help) {
            g_help_scroll++;
            return 1;
        }
        if (g_ac_count > 1) {
            g_ac_sel = (g_ac_sel + 1) % g_ac_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!sel_active()) {
                repl_selection_start(g_edit_line);
            }
            int selection_end = repl_selection_end();
            if (selection_end < g_num_cmds - 1)
                selection_end++;
            repl_selection_set_end(selection_end);
            navigate_to_line(selection_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line + 1);
        }
        return 1;
    default:
        return 0;
    }
}

static int handle_help_toggle_special_key_route(int key) {
    if (key == GLUT_KEY_F1) {
        g_show_help = !g_show_help;
        g_help_tab = 0;
        g_help_scroll = 0;
        return 1;
    }
    return 0;
}

static void cycle_example_or_user_scene(void) {
    /* F12 cycles: examples[0..N-1] -> user scenes (in slot order) -> back.
     * Active example moves to the next example, then first user scene.
     * Active user scene moves to the next occupied user slot, then example 0. */
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        for (int s = active_scene + 1; s < MAX_USER_SCENES; s++) {
            if (repl_user_scene_slot_used(s)) {
                repl_load_user_scene_idx(s);
                return;
            }
        }
        if (count > 0)
            repl_load_example(0);
        return;
    }

    if (count > 0) {
        int next = g_example_idx + 1;
        if (next < count) {
            repl_load_example(next);
            return;
        }
    }

    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (repl_user_scene_slot_used(s)) {
            repl_load_user_scene_idx(s);
            return;
        }
    }
    if (count > 0)
        repl_load_example(0);
}

static int handle_scene_cycle_special_key_route(int key) {
    if (key == GLUT_KEY_F12) {
        cycle_example_or_user_scene();
        return 1;
    }
    return 0;
}

static int handle_page_scroll_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_PAGE_UP:
        if (g_show_help)
            g_help_scroll -= 5;
        else
            g_scroll -= 5;
        g_scroll_follow_cursor = 0;
        return 1;
    case GLUT_KEY_PAGE_DOWN:
        if (g_show_help)
            g_help_scroll += 5;
        else
            g_scroll += 5;
        g_scroll_follow_cursor = 0;
        return 1;
    default:
        return 0;
    }
}

static void special_func(int key, int x, int y) {
    (void)x;
    (void)y;

    special_begin_key(key);

    if (handle_rename_special_route(key))   return;
    if (handle_replay_special_route(key))   return;

    restore_hidden_code_panel_for_special(key);

    if (handle_search_special_route(key))   return;
    if (handle_cfg_special_shortcut_route(key)) return;
    if (handle_horizontal_special_key_route(key)) return;
    if (handle_vertical_special_key_route(key)) return;
    if (handle_help_toggle_special_key_route(key)) return;
    if (handle_scene_cycle_special_key_route(key)) return;
    if (handle_page_scroll_special_key_route(key)) return;
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

    repl_camera_mouse_event(button, state, x, y, editor_get_modifiers());

#ifdef USE_GLUT
    if (button == 3 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll--;
        } else {
            if (editor_point_in_code_panel(x, y))
                g_scroll--;
            else
                repl_camera_add_zoom_velocity(-0.3f);
        }
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll++;
        } else {
            if (editor_point_in_code_panel(x, y))
                g_scroll++;
            else
                repl_camera_add_zoom_velocity(0.3f);
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
            repl_camera_add_zoom_velocity(-(float)direction * 0.1f);
    }
    glutPostRedisplay();
}
#endif

static void passive_motion_func(int x, int y) {
    repl_camera_pointer_set(x, y);

    if (editor_point_on_code_panel_divider(x, y))
        glutSetCursor(editor_code_panel_resize_cursor());
    else
        glutSetCursor(GLUT_CURSOR_INHERIT);
}

static void motion_func(int x, int y) {
    if (ui_panels_handle_motion(x, y)) {
        repl_camera_pointer_set(x, y);
        glutPostRedisplay();
        return;
    }

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
        repl_camera_pointer_set(x, y);
        glutPostRedisplay();
        return;
    }

    if (handle_code_panel_drag(x, y)) {
        repl_camera_pointer_set(x, y);
        glutPostRedisplay();
        return;
    }

    repl_camera_drag_motion(x, y);
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

    repl_camera_tick();

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
            parsed = repl_parse_and_normalize(g_input, insert_idx, vis_vars, num_vis_vars,
                                              input_has_any_visible_vars(g_input, vis_vars, num_vis_vars),
                                              &cmd);
        else
            parsed = repl_parse_and_normalize(g_input, insert_idx, NULL, 0,
                                              input_has_predef_vars(g_input), &cmd);

        if (parsed) {
            ReplCommandStore store = repl_command_store_live();
            if (g_inserting) {
                if (!repl_command_store_insert_one(&store, g_edit_line, &cmd, 0))
                    goto feed_line_done;
                g_edit_line++;
            } else if (g_edit_line < g_num_cmds) {
                repl_command_store_replace_one(&store, g_edit_line, &cmd);
                g_edit_line++;
            } else {
                if (!repl_command_store_insert_one(&store, g_num_cmds, &cmd, 0))
                    goto feed_line_done;
                g_edit_line = g_num_cmds;
            }
            handled = 1;
        }
feed_line_done:
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
