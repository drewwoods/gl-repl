/*
 * repl_core.h — Public API for the REPL core module.
 *
 * repl_core.c owns the source-command array (g_cmds), the flattened command
 * array (g_flat_cmds), parser entrypoints, display callback, and app-level
 * lifecycle wrappers. Execution, flattening, replay, examples, and scenes live
 * in focused repl_* modules behind this API. Everything listed here is safe to
 * call from sample.c, scene_render.c, ui_panels.c, and the test binaries.
 *
 * Implementation-only helpers that share state across the repl_* translation
 * units (editor, export, search, etc.) live in repl_core_internal.h; keep
 * them out of this header to preserve the boundary.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include "sample.h"

/* --- Parsing & normalization ------------------------------------------- */
/* Parse a single REPL line into `cmd`. Returns 1 on success, 0 otherwise.
 * The _with_vars variant makes loop/function locals visible to the
 * expression evaluator (for flattening contexts). */
int  repl_parse_command(const char *line, GLCmd *cmd);
int  repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                  ExprVar *vars, int num_vars);

/* --- Save / load ------------------------------------------------------- */
void repl_save_default_output(void);   /* writes ./output.c */
int  repl_load_from_file(const char *filename);
void repl_save_output(const char *filename);

/* Workspace I/O: save every occupied user-scene slot to `<dir>/<slug>.c`.
 * repl_load_workspace scans `dir` for *.c and imports each into a fresh
 * slot.  Both remember `dir` so single-file exports carry a
 * `@workspace-dir` hint.
 * Sets status message on success or failure.
 * returns -1 on error or the number of files saved */
int  repl_save_workspace(const char *dir);
/*
 * Sets status message on success or failure.
 * returns -1 on error or the number of files loaded. */
int  repl_load_workspace(const char *dir);
const char *repl_workspace_dir(void);         /* "" if not bound */
void repl_set_workspace_dir(const char *dir); /* copies; NULL = clear */

/* --- Command pipeline -------------------------------------------------- */
/* Rebuild g_flat_cmds from g_cmds (expanding for/func/if). Idempotent;
 * `mark_normals_dirty()` sets the rebuild flag. */
void repl_flatten_commands(void);
/* Recompute auto-normals for every glBegin/glEnd batch in g_cmds. */
void repl_recompute_autonormals(void);

/* --- Example library & user scene -------------------------------------- */
#define MAX_USER_SCENES      8
#define USER_SCENE_NAME_MAX 64

int  repl_example_count(void);
const char *repl_example_name(int idx);
void repl_load_example(int idx);

/* User scenes: up to MAX_USER_SCENES slots. Slot 0 is the "home" scene
 * (captured the first time an example is loaded) and is never evicted. The
 * active slot is the one currently loaded into g_cmds[]; -1 means an
 * example or fresh workspace is active instead. */
int  repl_user_scene_valid(void);         /* any slot used? (back-compat) */
void repl_load_user_scene(void);          /* loads slot 0 (back-compat) */
int  repl_user_scene_count(void);         /* number of occupied slots */
int  repl_user_scene_slot_used(int slot); /* 1 if occupied */
const char *repl_user_scene_name(int slot);
int  repl_user_scene_rename(int slot, const char *new_name);
int  repl_load_user_scene_idx(int slot);  /* returns 1 on success */
int  repl_active_user_scene(void);        /* -1 if none active */

/* --- Replay ------------------------------------------------------------ */
void replay_start(void);
void replay_stop(void);

/* --- Editor / navigation helpers called from outside repl_core.c ------- */
void repl_navigate_to_line(int target);
void repl_load_initial_commands(const char *import_file);
void repl_reformat_commands(void);

/* --- Debug dumps (used by tests / CLI flags) --------------------------- */
void repl_debug_dump_editor(FILE *out);
void repl_debug_dump_flat_commands(FILE *out);

/* --- Cursor / feed queries --------------------------------------------- */
int  repl_flat_cmd_matches_cursor(int flat_idx);
int  repl_find_feeding_normal_cmd(int line_idx);
int  repl_find_feeding_color_cmd(int line_idx);
void repl_copy_replay_baseline_predef_values(float *dst, int max_vals);

/* --- GLUT callback entry points (wired in sample.c) -------------------- */
void repl_display_func(void);
void repl_reshape_func(int w, int h);
void repl_keyboard_func(unsigned char key, int x, int y);
void repl_special_func(int key, int x, int y);
void repl_mouse_func(int button, int state, int x, int y);
void repl_motion_func(int x, int y);
void repl_passive_motion_func(int x, int y);
#ifndef USE_GLUT
void repl_mousewheel_func(int wheel, int direction, int x, int y);
#endif
void repl_timer_func(int value);
void repl_init_gl(void);

/* --- Test helpers ------------------------------------------------------ */
/* Reset global REPL state (cmds, input, camera, predef vars, etc.) to the
 * same configuration the real binary starts in. Used by every test fixture. */
void repl_reset_state(void);
/* Public wrapper over the internal feed_line() so tests outside
 * repl_core_internal.h users can drive lines end-to-end. */
void repl_feed_line_public(const char *line);

#endif /* REPL_CORE_H */
