/*
 * repl_replay.c — Replay state machine, fade batches, and replay input.
 */
#include "repl_replay.h"
#include "repl_core_internal.h"
#include "repl_keys.h"
#include "repl_state.h"
#include "ui_profile_panel.h"

int    g_replay_active = 0;
int    g_replay_state = REPLAY_OFF;
int    g_replay_pc = 0;
int    g_replay_mode = REPLAY_MODE_VERTEX;
float  g_replay_speed = 4.0f;
float  g_replay_accum = 0.0f;
float  g_replay_fade_speed = 2.0f;
int    g_replay_src_line = -1;
int    g_replay_total_flat = 0;
int    g_replay_expand_args = 1;

static float g_replay_baseline_predef_vals[MAX_PREDEF_VARS];
static int   g_replay_saved_t_playing = 1;
static int   g_replay_last_src_line = -1;

#define REPLAY_FADE_DURATION   0.20f
#define REPLAY_FADE_BATCH_MAX  24

typedef struct {
    int   old_pc;
    int   new_pc;
    float age;
} ReplayFadeBatch;

static ReplayFadeBatch g_replay_fade_batches[REPLAY_FADE_BATCH_MAX];
static int             g_replay_fade_batch_count = 0;

#define REPLAY_FLAT_STATE \
    FlatProgramView flat_program = repl_state_flat_program_view(); \
    const GLCmd *g_flat_cmds __attribute__((unused)) = flat_program.cmds; \
    int g_num_flat_cmds __attribute__((unused)) = flat_program.cmd_count

static int replay_enabled(void) {
    return g_replay_active;
}

static int replay_has_meaningful_cmds(void) {
    REPLAY_FLAT_STATE;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_COMMENT) continue;
        return 1;
    }
    return 0;
}

static int replay_find_open_begin_before(int limit) {
    REPLAY_FLAT_STATE;
    int open_begin = -1;
    int in_begin = 0;

    for (int i = 0; i < limit && i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_BEGIN) {
            open_begin = i;
            in_begin = 1;
        } else if (g_flat_cmds[i].type == CMD_END && in_begin) {
            open_begin = -1;
            in_begin = 0;
        }
    }

    return open_begin;
}

static int replay_find_open_tess_polygon_before(int limit, int *out_depth) {
    REPLAY_FLAT_STATE;
    int poly_start = -1;
    int tess_depth = 0;

    for (int i = 0; i < limit && i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        switch (g_flat_cmds[i].type) {
        case CMD_TESS_BEGIN_POLYGON:
            poly_start = i;
            tess_depth = 1;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (tess_depth == 1)
                tess_depth = 2;
            break;
        case CMD_TESS_END:
            if (tess_depth == 2) {
                tess_depth = 1;
            } else if (tess_depth == 1) {
                tess_depth = 0;
                poly_start = -1;
            }
            break;
        default:
            break;
        }
    }

    if (out_depth) *out_depth = tess_depth;
    return poly_start;
}

static int replay_find_matching_gl_end(int begin_idx) {
    REPLAY_FLAT_STATE;
    for (int i = begin_idx + 1; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_END)
            return i;
    }
    return g_num_flat_cmds > 0 ? g_num_flat_cmds - 1 : begin_idx;
}

static int replay_cmd_is_focus_candidate(CmdType type) {
    switch (type) {
    case CMD_COMMENT:
    case CMD_BEGIN:
    case CMD_END:
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:
    case CMD_FUNC_DEF:
    case CMD_FUNC_END:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_LABEL:
        return 0;
    default:
        return 1;
    }
}

static int replay_last_meaningful_src(int begin, int end_exclusive) {
    REPLAY_FLAT_STATE;
    for (int i = end_exclusive - 1; i >= begin && i >= 0; i--) {
        if (!g_flat_cmds[i].valid) continue;
        if (!replay_cmd_is_focus_candidate(g_flat_cmds[i].type)) continue;
        if (g_flat_cmds[i].src_cmd_idx >= 0)
            return g_flat_cmds[i].src_cmd_idx;
    }
    return -1;
}

static void replay_set_src_line(int src_line) {
    g_replay_src_line = src_line;
    if (src_line != g_replay_last_src_line) {
        g_replay_last_src_line = src_line;
        if (src_line >= 0)
            g_scroll_follow_cursor = 1;
    }
}

static float replay_batch_alpha(const ReplayFadeBatch *batch) {
    float alpha = batch->age * g_replay_fade_speed;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

static void replay_clear_fade_batches(void) {
    g_replay_fade_batch_count = 0;
}

static void replay_push_fade_batch(int old_pc, int new_pc) {
    ReplayFadeBatch *batch;

    if (new_pc <= old_pc)
        return;

    if (g_replay_fade_batch_count >= REPLAY_FADE_BATCH_MAX) {
        memmove(&g_replay_fade_batches[0], &g_replay_fade_batches[1],
                (size_t)(REPLAY_FADE_BATCH_MAX - 1) * sizeof(g_replay_fade_batches[0]));
        g_replay_fade_batch_count = REPLAY_FADE_BATCH_MAX - 1;
    }

    batch = &g_replay_fade_batches[g_replay_fade_batch_count++];
    batch->old_pc = old_pc;
    batch->new_pc = new_pc;
    batch->age = 0.016f;
}

static void replay_clamp_fade_batches(int max_pc) {
    int dst = 0;

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        ReplayFadeBatch batch = g_replay_fade_batches[i];

        if (batch.old_pc > max_pc)
            continue;
        if (batch.new_pc > max_pc)
            batch.new_pc = max_pc;
        if (batch.new_pc <= batch.old_pc)
            continue;
        g_replay_fade_batches[dst++] = batch;
    }

    g_replay_fade_batch_count = dst;
}

void replay_tick_fade_batches(float dt) {
    int dst = 0;

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        ReplayFadeBatch batch = g_replay_fade_batches[i];
        batch.age += dt;
        if (batch.age >= REPLAY_FADE_DURATION)
            continue;
        g_replay_fade_batches[dst++] = batch;
    }

    g_replay_fade_batch_count = dst;
}

int replay_has_active_fades(void) {
    return g_replay_active && g_replay_fade_batch_count > 0;
}

int replay_fill_base_limit(void) {
    REPLAY_FLAT_STATE;
    if (!replay_has_active_fades())
        return g_num_flat_cmds;
    if (g_replay_fade_batches[0].old_pc < 0)
        return 0;
    if (g_replay_fade_batches[0].old_pc > g_num_flat_cmds)
        return g_num_flat_cmds;
    return g_replay_fade_batches[0].old_pc;
}

static int replay_next_polygon_limit(int start, int *fade_begin, int *fade_end) {
    int saw_meaningful = 0;
    REPLAY_FLAT_STATE;

    *fade_begin = -1;
    *fade_end = -1;

    for (int i = start; i < g_num_flat_cmds; i++) {
        CmdType t;

        if (!g_flat_cmds[i].valid || g_flat_cmds[i].type == CMD_COMMENT)
            continue;

        t = g_flat_cmds[i].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN: {
            int end = replay_find_matching_gl_end(i);
            *fade_begin = start;
            *fade_end = end;
            return end + 1;
        }
        case CMD_GLU_SPHERE:
        case CMD_GLU_CYLINDER:
        case CMD_GLU_DISK:
        case CMD_GLU_PARTIAL_DISK:
        case CMD_GLUT_TORUS:
            *fade_begin = start;
            *fade_end = i;
            return i + 1;
        case CMD_TESS_BEGIN_POLYGON: {
            int tess_depth = 1;
            for (int j = i + 1; j < g_num_flat_cmds; j++) {
                if (!g_flat_cmds[j].valid) continue;
                if (g_flat_cmds[j].type == CMD_TESS_BEGIN_POLYGON) {
                    tess_depth = 1;
                } else if (g_flat_cmds[j].type == CMD_TESS_BEGIN_CONTOUR) {
                    if (tess_depth == 1)
                        tess_depth = 2;
                } else if (g_flat_cmds[j].type == CMD_TESS_END) {
                    if (tess_depth == 2) {
                        tess_depth = 1;
                    } else if (tess_depth == 1) {
                        *fade_begin = start;
                        *fade_end = j;
                        return j + 1;
                    }
                }
            }
            *fade_begin = start;
            *fade_end = g_num_flat_cmds > 0 ? g_num_flat_cmds - 1 : i;
            return g_num_flat_cmds;
        }
        default:
            break;
        }
    }

    if (saw_meaningful)
        return g_num_flat_cmds;
    return start;
}

static int replay_next_vertex_limit(int start, int *fade_begin, int *fade_end) {
    REPLAY_FLAT_STATE;
    int open_begin = replay_find_open_begin_before(start);
    int tess_depth = 0;
    int open_tess_poly = replay_find_open_tess_polygon_before(start, &tess_depth);
    int saw_meaningful = 0;

    *fade_begin = -1;
    *fade_end = -1;

    for (int i = start; i < g_num_flat_cmds; i++) {
        CmdType t;

        if (!g_flat_cmds[i].valid || g_flat_cmds[i].type == CMD_COMMENT)
            continue;

        t = g_flat_cmds[i].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN:
            open_begin = i;
            break;
        case CMD_END:
            open_begin = -1;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            open_tess_poly = i;
            tess_depth = 1;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (tess_depth == 1)
                tess_depth = 2;
            break;
        case CMD_TESS_END:
            if (tess_depth == 2) {
                tess_depth = 1;
            } else if (tess_depth == 1) {
                *fade_begin = (open_tess_poly >= 0) ? open_tess_poly : start;
                *fade_end = i;
                return i + 1;
            }
            break;
        case CMD_VERTEX3F:
        case CMD_VERTEX2F:
            *fade_begin = (open_begin >= 0) ? open_begin : start;
            *fade_end = i;
            return i + 1;
        case CMD_TESS_VERTEX:
            *fade_begin = (open_tess_poly >= 0) ? open_tess_poly : start;
            *fade_end = i;
            return i + 1;
        case CMD_GLU_SPHERE:
        case CMD_GLU_CYLINDER:
        case CMD_GLU_DISK:
        case CMD_GLU_PARTIAL_DISK:
        case CMD_GLUT_TORUS:
            *fade_begin = start;
            *fade_end = i;
            return i + 1;
        default:
            break;
        }
    }

    if (saw_meaningful)
        return g_num_flat_cmds;
    return start;
}

static int replay_prev_limit(int current_pc) {
    int pc = 0;
    int prev_pc = 0;

    if (current_pc <= 0)
        return 0;

    while (pc < current_pc) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        if (next_pc <= pc)
            break;

        prev_pc = pc;
        pc = next_pc;
    }

    return prev_pc;
}

void replay_seek(int new_pc) {
    REPLAY_FLAT_STATE;
    if (new_pc < 0)
        new_pc = 0;
    if (new_pc > g_num_flat_cmds)
        new_pc = g_num_flat_cmds;

    g_replay_pc = new_pc;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    replay_set_src_line(replay_last_meaningful_src(0, new_pc));
    g_replay_state = (new_pc >= g_num_flat_cmds && g_num_flat_cmds > 0)
                   ? REPLAY_DONE
                   : REPLAY_PAUSED;
}

int replay_seek_to_src_line(int target_line) {
    int pc = 0;
    int landed_pc = -1;
    int landed_src = -1;
    REPLAY_FLAT_STATE;

    if (repl_state_flat_program_dirty()) {
        float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
        repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
        flatten_commands();
        repl_state_flat_program_clear_dirty();
        repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    }

    while (pc < g_num_flat_cmds) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        if (next_pc <= pc)
            break;

        int step_src = replay_last_meaningful_src(pc, next_pc);
        if (step_src >= target_line) {
            landed_pc = next_pc;
            landed_src = step_src;
            break;
        }
        pc = next_pc;
    }

    if (landed_pc < 0)
        return -1;

    replay_seek(landed_pc);
    return landed_src;
}

void replay_restart_from_beginning(void) {
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_state = REPLAY_PLAYING;
    g_replay_src_line = -1;
    g_replay_last_src_line = -1;
}

void replay_start(void) {
    float live_predef_vals[MAX_PREDEF_VARS];
    REPLAY_FLAT_STATE;

    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    if (repl_state_flat_program_dirty()) {
        flatten_commands();
        repl_state_flat_program_clear_dirty();
        repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    }

    if (!replay_has_meaningful_cmds()) {
        set_status("Replay: nothing to play");
        return;
    }

    repl_copy_predef_values(g_replay_baseline_predef_vals, MAX_PREDEF_VARS);
    g_replay_saved_t_playing = g_t_playing;
    g_t_playing = 0;

    g_replay_active = 1;
    g_replay_state = REPLAY_PLAYING;
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_src_line = -1;
    g_replay_total_flat = g_num_flat_cmds;
    g_replay_last_src_line = -1;
    set_status("Replay: playing");
}

void replay_stop(void) {
    g_t_playing = g_replay_saved_t_playing;
    g_replay_active = 0;
    g_replay_state = REPLAY_OFF;
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_src_line = -1;
    g_replay_total_flat = 0;
    g_replay_last_src_line = -1;
}

void replay_advance(void) {
    REPLAY_FLAT_STATE;
    int old_pc;
    int next_pc;
    int src_line = -1;

    if (!replay_enabled())
        return;

    if (g_replay_pc >= g_num_flat_cmds) {
        g_replay_state = REPLAY_DONE;
        return;
    }

    old_pc = g_replay_pc;
    next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
            ? replay_next_polygon_limit(old_pc, &(int){ -1 }, &(int){ -1 })
            : replay_next_vertex_limit(old_pc, &(int){ -1 }, &(int){ -1 });

    if (next_pc <= old_pc)
        next_pc = g_num_flat_cmds;
    if (next_pc > g_num_flat_cmds)
        next_pc = g_num_flat_cmds;

    g_replay_pc = next_pc;
    replay_push_fade_batch(old_pc, next_pc);

    src_line = replay_last_meaningful_src(old_pc, next_pc);
    replay_set_src_line(src_line);

    if (g_replay_pc >= g_num_flat_cmds) {
        g_replay_state = REPLAY_DONE;
        set_status("Replay: done");
    }
}

void replay_step_back(void) {
    if (!g_replay_active)
        return;

    if (g_replay_pc <= 0) {
        replay_seek(0);
        set_status("Replay: at start");
        return;
    }

    replay_seek(replay_prev_limit(g_replay_pc));
}

int replay_exec_limit(void) {
    REPLAY_FLAT_STATE;
    if (replay_enabled())
        return g_replay_pc;
    return g_num_flat_cmds;
}

void replay_speed_adjust(float factor) {
    char msg[64];
    g_replay_speed *= factor;
    if (g_replay_speed < 0.5f) g_replay_speed = 0.5f;
    if (g_replay_speed > 200.0f) g_replay_speed = 200.0f;
    snprintf(msg, sizeof(msg), "Replay: %.1f step/s", g_replay_speed);
    set_status(msg);
}

int replay_prepare_frame(int full_flat_count) {
    REPLAY_FLAT_STATE;
    if (!g_replay_active)
        return g_num_flat_cmds;

    g_replay_total_flat = full_flat_count;
    if (g_replay_pc > g_num_flat_cmds)
        g_replay_pc = g_num_flat_cmds;
    if (g_replay_pc >= g_num_flat_cmds && g_num_flat_cmds > 0 &&
        g_replay_state == REPLAY_PLAYING)
        g_replay_state = REPLAY_DONE;
    replay_clamp_fade_batches(g_replay_pc);
    return replay_exec_limit();
}

void replay_restore_baseline_predef_values(void) {
    repl_restore_predef_values(g_replay_baseline_predef_vals, MAX_PREDEF_VARS);
}

void repl_copy_replay_baseline_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = max_vals < MAX_PREDEF_VARS ? max_vals : MAX_PREDEF_VARS;
    memcpy(dst, g_replay_baseline_predef_vals, (size_t)n * sizeof(float));
}

int replay_handle_key(unsigned char key) {
    if (!g_replay_active) {
        if (key == KEY_CTRL_R) {
            replay_start();
            return 1;
        }
        if (key == KEY_CTRL_K) {
            int target_line = repl_state_edit_line();
            replay_start();
            if (g_replay_active) {
                int landed = replay_seek_to_src_line(target_line);
                if (landed < 0) {
                    set_status("Jump: no geometry at or after cursor");
                } else {
                    char msg[64];
                    g_replay_state = REPLAY_PAUSED;
                    snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
                    set_status(msg);
                }
            }
            return 1;
        }
        return 0;
    }

    if (key == KEY_CTRL_R) {
        replay_stop();
        set_status("Replay: off");
        return 1;
    }
    if (key == KEY_CTRL_K) {
        int landed = replay_seek_to_src_line(repl_state_edit_line());
        if (landed < 0) {
            set_status("Jump: no geometry at or after cursor");
        } else {
            char msg[64];
            g_replay_state = REPLAY_PAUSED;
            snprintf(msg, sizeof(msg), "Jump: paused at line %d", landed + 1);
            set_status(msg);
        }
        return 1;
    }
    if (key == ' ') {
        if (g_replay_state == REPLAY_PLAYING) {
            g_replay_state = REPLAY_PAUSED;
            set_status("Replay: paused");
        } else if (g_replay_state == REPLAY_PAUSED) {
            g_replay_state = REPLAY_PLAYING;
            set_status("Replay: playing");
        } else if (g_replay_state == REPLAY_DONE) {
            replay_restart_from_beginning();
            set_status("Replay: restarted");
        }
        return 1;
    }
    if (key == '+' || key == '=') {
        replay_speed_adjust(1.5f);
        return 1;
    }
    if (key == '-') {
        replay_speed_adjust(0.67f);
        return 1;
    }
    if (key == 'm' || key == 'M') {
        int was_playing = (g_replay_state == REPLAY_PLAYING);
        g_replay_mode = (g_replay_mode == REPLAY_MODE_VERTEX)
                      ? REPLAY_MODE_POLYGON
                      : REPLAY_MODE_VERTEX;
        replay_seek(g_replay_pc);
        if (was_playing && g_replay_state != REPLAY_DONE)
            g_replay_state = REPLAY_PLAYING;
        set_status(g_replay_mode == REPLAY_MODE_VERTEX
                 ? "Replay: vertex mode"
                 : "Replay: polygon mode");
        return 1;
    }
    if (key == 'e' || key == 'E') {
        g_replay_expand_args = !g_replay_expand_args;
        return 1;
    }
    if (key == KEY_ESC) {
        replay_stop();
        set_status("Replay: off");
        return 1;
    }

    replay_stop();
    return 0;
}

static int replay_modifier_special_key(int key) {
#ifdef USE_GLUT
    return 0;
#else
    return key == GLUT_KEY_NUM_LOCK ||
           key == GLUT_KEY_SHIFT_L || key == GLUT_KEY_SHIFT_R ||
           key == GLUT_KEY_CTRL_L || key == GLUT_KEY_CTRL_R ||
           key == GLUT_KEY_ALT_L || key == GLUT_KEY_ALT_R ||
           key == GLUT_KEY_SUPER_L || key == GLUT_KEY_SUPER_R;
#endif
}

int replay_handle_special_key(int key) {
    if (!g_replay_active)
        return 0;

    if (key == GLUT_KEY_LEFT) {
        replay_step_back();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        replay_advance();
        return 1;
    }
    if (key == GLUT_KEY_UP) {
        replay_speed_adjust(1.5f);
        return 1;
    }
    if (key == GLUT_KEY_DOWN) {
        replay_speed_adjust(0.67f);
        return 1;
    }

    if (!replay_modifier_special_key(key))
        replay_stop();
    return 0;
}

void execute_replay_fade_batches(void) {
    int skip_limits[REPLAY_FADE_BATCH_MAX];
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;

    if (!replay_has_active_fades())
        return;

    prof_begin(PROF_SCENE_3D_FADE_PROLOGUE);

    {
        int batch_idx = 0;
        int open_begin = -1;
        int open_tess = -1;
        int tess_depth = 0;
        int max_old_pc = g_replay_fade_batches[g_replay_fade_batch_count - 1].old_pc;
        if (max_old_pc > g_num_flat_cmds) max_old_pc = g_num_flat_cmds;

        for (int pc = 0; pc <= max_old_pc; pc++) {
            while (batch_idx < g_replay_fade_batch_count &&
                   g_replay_fade_batches[batch_idx].old_pc <= pc) {
                int sl = g_replay_fade_batches[batch_idx].old_pc;
                if (open_begin >= 0 && open_begin < sl) sl = open_begin;
                if (open_tess  >= 0 && open_tess  < sl) sl = open_tess;
                skip_limits[batch_idx++] = sl;
            }
            if (pc >= g_num_flat_cmds || !g_flat_cmds[pc].valid)
                continue;
            switch (g_flat_cmds[pc].type) {
            case CMD_BEGIN: open_begin = pc; break;
            case CMD_END:   open_begin = -1; break;
            case CMD_TESS_BEGIN_POLYGON: open_tess = pc; tess_depth = 1; break;
            case CMD_TESS_BEGIN_CONTOUR: if (tess_depth == 1) tess_depth = 2; break;
            case CMD_TESS_END:
                if (tess_depth == 2) tess_depth = 1;
                else if (tess_depth == 1) { open_tess = -1; tess_depth = 0; }
                break;
            default: break;
            }
        }
        while (batch_idx < g_replay_fade_batch_count) {
            int sl = g_replay_fade_batches[batch_idx].old_pc;
            if (open_begin >= 0 && open_begin < sl) sl = open_begin;
            if (open_tess  >= 0 && open_tess  < sl) sl = open_tess;
            skip_limits[batch_idx++] = sl;
        }
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    prof_accum_end(PROF_SCENE_3D_FADE_PROLOGUE);

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        float alpha = replay_batch_alpha(&g_replay_fade_batches[i]);

        if (alpha <= 0.0f)
            continue;

        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
        FlatProgramView flat_program = repl_flat_program_view_live();
        ReplExecutionOptions exec_options = {
            .flat_cmd_count = g_replay_fade_batches[i].new_pc,
            .program = flat_program
        };
        replay_restore_baseline_predef_values();
        repl_execute_set_fade_context(alpha, skip_limits[i]);

        glDisable(GL_LIGHTING);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPolygonMode(GL_FRONT_AND_BACK, g_wireframe ? GL_LINE : GL_FILL);
        glColor4f(0.70f, 0.70f, 0.80f, alpha);
        glPushMatrix();
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_PREP);

        prof_begin(PROF_SCENE_3D_FADE_BATCH_EXEC);
        repl_execute_program(&exec_options);
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_EXEC);

        glPopMatrix();
    }

    prof_begin(PROF_SCENE_3D_FADE_BATCH_POST);
    repl_execute_set_fade_context(1.0f, 0);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_FADE_BATCH_POST);
}

int repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                            int count, float age) {
    int installed = 0;
    REPLAY_FLAT_STATE;

    if (count < 0) count = 0;
    if (count > REPLAY_FADE_BATCH_MAX) count = REPLAY_FADE_BATCH_MAX;

    if (count > 0 && (old_pcs == NULL || new_pcs == NULL))
        count = 0;

    for (int i = 0; i < count; i++) {
        int old_pc = old_pcs[i];
        int new_pc = new_pcs[i];

        if (old_pc < 0) old_pc = 0;
        if (old_pc > g_num_flat_cmds) old_pc = g_num_flat_cmds;
        if (new_pc < 0) new_pc = 0;
        if (new_pc > g_num_flat_cmds) new_pc = g_num_flat_cmds;
        if (new_pc <= old_pc)
            continue;

        g_replay_fade_batches[installed].old_pc = old_pc;
        g_replay_fade_batches[installed].new_pc = new_pc;
        g_replay_fade_batches[installed].age = age;
        installed++;
    }
    g_replay_fade_batch_count = installed;

    repl_copy_predef_values(g_replay_baseline_predef_vals, MAX_PREDEF_VARS);
    g_replay_active = (installed > 0);
    return installed;
}

void repl_bench_fade_clear(void) {
    g_replay_fade_batch_count = 0;
    g_replay_active = 0;
}
