/*
 * tools/editor_demo/repl_shim.c -- Fake REPL semantics + direct
 * stubs for tools/editor_demo.
 *
 * Provides:
 *   - editor_services_default() with demo-local EditorServices
 *     bindings. compile() returns NO_CHANGE; apply* are no-ops;
 *     parse() treats every input as a CMD_COMMENT line that
 *     stores the text verbatim. The demo is a plain text editor:
 *     lines come in, lines get stored in the editor buffer, no
 *     GL parsing happens.
 *   - Direct stubs for repl_* / glr_* / ui_* / tutorial_* /
 *     color_picker_* / editor-internal helper symbols that the
 *     editor module set calls by name. Each stub backs onto tiny
 *     demo-local state (cmd array, edit_line, etc.) or returns a
 *     neutral default. Adding a new direct call site in the editor
 *     is a regression; the check-editor-repl-surface gate (Phase
 *     7b) catches it.
 *
 * Per the "Scope decision" in plans/active/editor-demo.md, UI chrome
 * (menu bar, help overlay, color picker, tutorial) is editor-inherent
 * and *would* link directly -- but in this demo we don't link the
 * widget/UI .c files (they transitively pull in REPL), so the chrome
 * surface gets stubbed here alongside the REPL surface. Migrating any
 * stub group into a coarser callback in EditorServices (or fixing the
 * widget module to not require REPL transitively) shrinks this file.
 *
 * Keep stubs boring: one stub per symbol, no clever substitution,
 * no parallel pipeline. Adding stubs here is a signal to consider
 * migrating the caller through EditorServices instead.
 */

#include "editor/services.h"

#include "repl/apply.h"
#include "repl/command_store.h"
#include "repl/compile.h"
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "ui/state.h"
#include "ui/layout.h"
#include "app/glr_state.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ---- Demo-local state -------------------------------------------- */

static GLCmd  g_shim_cmds[MAX_COMMANDS];
static int    g_shim_cmd_count    = 0;
static int    g_shim_edit_line    = 0;
static int    g_shim_capacity     = MAX_COMMANDS;
static ExprVar g_shim_predef_vars[MAX_PREDEF_VARS];
static int    g_shim_predef_count = 0;

static UiCodePanelRuntimeState g_shim_code_panel;
static UiHelpState             g_shim_help_state;
static UiStatusState           g_shim_status_state;
static GlrPresentationState    g_shim_presentation;

/* ---- EditorServices ---------------------------------------------- */

static ReplCompileContext shim_context(void *user) {
    (void)user;
    ReplCompileContext ctx = {0};
    ctx.insert_mode = 1;
    return ctx;
}

static ReplCompileResult shim_compile(const char *text,
                                      const ReplCompileContext *ctx,
                                      ReplCompiledChange *out,
                                      char *err, int err_size,
                                      void *user) {
    (void)text; (void)ctx; (void)err; (void)err_size; (void)user;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

static int shim_apply_repl_change(const ReplCompiledChange *change, void *user) {
    (void)change; (void)user;
    return 1;
}

static void shim_apply_predef_ops(const ReplCompiledChange *change, void *user) {
    (void)change; (void)user;
}

static void shim_apply_scratch_ops(const ReplCompiledChange *change, void *user) {
    (void)change; (void)user;
}

static int shim_parse_command_ctx(const char *line, ReplParsedLine *out,
                                  const ReplParseContext *ctx, void *user) {
    (void)ctx; (void)user;
    if (!out) return 0;
    memset(&out->cmd, 0, sizeof(out->cmd));
    out->cmd.type  = CMD_COMMENT;
    out->cmd.valid = 1;
    if (line)
        snprintf(out->text, sizeof(out->text), "%s", line);
    else
        out->text[0] = '\0';
    return 1;
}

EditorServices editor_services_default(void) {
    EditorServices svc = {
        .context           = shim_context,
        .compile           = shim_compile,
        .apply_repl_change = shim_apply_repl_change,
        .apply_predef_ops  = shim_apply_predef_ops,
        .apply_scratch_ops = shim_apply_scratch_ops,
        .parse_command_ctx = shim_parse_command_ctx,
        .user              = NULL,
    };
    return svc;
}

/* ---- repl_state_* ------------------------------------------------ */

int  repl_state_edit_line(void)            { return g_shim_edit_line; }
void repl_state_edit_line_set(int line)    { g_shim_edit_line = line; }
void repl_state_edit_line_clamp(void)      {
    if (g_shim_edit_line < 0) g_shim_edit_line = 0;
    if (g_shim_edit_line > g_shim_cmd_count) g_shim_edit_line = g_shim_cmd_count;
}
int  repl_state_document_count(void)       { return g_shim_cmd_count; }
GLCmd *repl_state_document_cmds_mut(void)  { return g_shim_cmds; }

/* ---- repl_command_store_* --------------------------------------- */

ReplCommandStore repl_command_store_live(void) {
    ReplCommandStore s = {
        .cmds      = g_shim_cmds,
        .count     = &g_shim_cmd_count,
        .capacity  = g_shim_capacity,
        .edit_line = &g_shim_edit_line,
    };
    return s;
}

void repl_command_store_clear(ReplCommandStore *store) {
    if (!store || !store->count) return;
    *store->count = 0;
}

int repl_command_store_can_insert(const ReplCommandStore *store, int count) {
    if (!store || !store->count) return 0;
    return (*store->count + count) <= store->capacity;
}

int repl_command_store_normalize_range(const ReplCommandStore *store,
                                       int start, int count,
                                       int *out_start, int *out_count) {
    if (!store || !store->count) return 0;
    int n = *store->count;
    if (start < 0) { count += start; start = 0; }
    if (start >= n) { if (out_start) *out_start = n; if (out_count) *out_count = 0; return 0; }
    if (count < 0) count = 0;
    if (start + count > n) count = n - start;
    if (out_start) *out_start = start;
    if (out_count) *out_count = count;
    return count > 0;
}

int repl_command_store_insert_one(ReplCommandStore *store, int pos,
                                  const GLCmd *cmd, int flags) {
    (void)flags;
    if (!store || !store->cmds || !store->count || !cmd) return 0;
    if (*store->count >= store->capacity) return 0;
    if (pos < 0) pos = 0;
    if (pos > *store->count) pos = *store->count;
    for (int i = *store->count; i > pos; i--)
        store->cmds[i] = store->cmds[i - 1];
    store->cmds[pos] = *cmd;
    (*store->count)++;
    return 1;
}

int repl_command_store_replace_one(ReplCommandStore *store, int pos, const GLCmd *cmd) {
    if (!store || !store->cmds || !store->count || !cmd) return 0;
    if (pos < 0 || pos >= *store->count) return 0;
    store->cmds[pos] = *cmd;
    return 1;
}

int repl_command_store_load(ReplCommandStore *store, const GLCmd *cmds,
                            int count, int edit_line) {
    if (!store || !store->cmds || !store->count) return 0;
    if (count < 0) count = 0;
    if (count > store->capacity) return 0;
    if (cmds && count > 0)
        memcpy(store->cmds, cmds, sizeof(GLCmd) * (size_t)count);
    *store->count = count;
    if (store->edit_line) *store->edit_line = edit_line;
    return 1;
}

/* ---- repl_compile_* (stubbed -- demo is plain text) -------------- */

ReplCompileContext repl_compile_context_from_live(void) {
    ReplCompileContext ctx = {0};
    return ctx;
}

ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size) {
    (void)start; (void)count; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_empty_line(int line_idx,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    (void)line_idx; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_float_decl(const char *text,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    (void)text; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_toggle_comment(int line_idx,
                                              const char *prefix,
                                              const ReplCompileContext *ctx,
                                              ReplCompiledChange *out,
                                              char *err, int err_size) {
    (void)line_idx; (void)prefix; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_var_assign(const char *text,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    (void)text; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

void repl_compiled_change_init(ReplCompiledChange *change) {
    if (change) memset(change, 0, sizeof(*change));
}

void repl_compiled_change_rollback_alias(const ReplCompiledChange *change) {
    (void)change;
}

/* ---- repl_apply_* (no-op success) ------------------------------- */

int  repl_apply_can_apply_compiled_change(const ReplCompiledChange *change) {
    (void)change; return 1;
}
int  repl_apply_compiled_change(const ReplCompiledChange *change) {
    (void)change; return 1;
}
void repl_apply_predef_ops(const ReplCompiledChange *change)   { (void)change; }
void repl_apply_scratch_ops(const ReplCompiledChange *change)  { (void)change; }

/* ---- repl_eval_* ------------------------------------------------ */

void repl_eval_copy_scratch_arrays(
        float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) { (void)dst; }
void repl_eval_restore_scratch_arrays(
        const float src[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) { (void)src; }
void repl_eval_init_predef_vars(void)             { g_shim_predef_count = 0; }
int  repl_eval_input_has_predef_vars(const char *s) { (void)s; return 0; }
int  repl_eval_parse_exprs(const char *s, float *out, int max,
                           ExprVar *vars, int num_vars) {
    (void)s; (void)out; (void)max; (void)vars; (void)num_vars;
    return 0;
}
int  repl_eval_parse_for_header_with_vars(const char *input, char *var_name, int var_sz,
                                          float *start, float *end, float *step,
                                          ExprVar *vars, int num_vars,
                                          const char **body_start) {
    (void)input; (void)var_name; (void)var_sz;
    (void)start; (void)end; (void)step; (void)vars; (void)num_vars;
    if (body_start) *body_start = NULL;
    return 0;
}
ExprVar *repl_eval_predef_vars_mut(void) { return g_shim_predef_vars; }
int     *repl_eval_predef_count_mut(void) { return &g_shim_predef_count; }
int  repl_eval_validate_expression_idents(const char *src, const ExprVar *vars,
                                          int num_vars, char *err, int errsz) {
    (void)src; (void)vars; (void)num_vars; (void)err; (void)errsz;
    return 1;
}

/* ---- repl_func_alias_* ------------------------------------------ */

void repl_func_alias_clear_all(void)                       { }
int  repl_func_alias_first_free_slot(void)                 { return 0; }
const char *repl_func_alias_get(int slot)                  { (void)slot; return ""; }
int  repl_func_alias_lookup_slot(const char *name)         { (void)name; return -1; }
int  repl_func_alias_name_is_valid(const char *name)       { (void)name; return 1; }
int  repl_func_alias_set(int slot, const char *name)       {
    (void)slot; (void)name; return 1;
}

/* ---- repl_source_scope_* ---------------------------------------- */

int  repl_source_scope_block_depth_at(int pos)             { (void)pos; return 0; }
int  repl_source_scope_block_extent(int pos, int *start_out, int *end_out) {
    (void)pos;
    if (start_out) *start_out = pos;
    if (end_out)   *end_out   = pos;
    return 1;
}
void repl_source_scope_cmd_indent(int pos, char *out, int out_sz) {
    (void)pos;
    if (out && out_sz > 0) out[0] = '\0';
}
int  repl_source_scope_find_block_end(int pos)             { (void)pos; return -1; }
int  repl_source_scope_in_begin_block_at(int pos)          { (void)pos; return 0; }
CmdType repl_source_scope_nearest_open_block_at(int pos)   { (void)pos; return CMD_EMPTY; }

/* ---- repl_line_* (line-shape predicates) ------------------------ */

int  repl_line_is_block_head(int line_idx)                 { (void)line_idx; return 0; }
int  repl_line_is_label(int line_idx)                      { (void)line_idx; return 0; }

/* ---- repl_* misc ------------------------------------------------ */

void repl_mark_normals_dirty(void)                         { }
void repl_set_status(const char *msg)                      {
    if (msg && msg[0])
        fprintf(stderr, "[demo-status] %s\n", msg);
}
int  repl_parse_and_normalize_strict(const char *line, int pos,
                                     ExprVar *vars, int num_vars,
                                     int preserve_expr, GLCmd *out_cmd,
                                     char *text_out, int text_sz) {
    (void)line; (void)pos; (void)vars; (void)num_vars; (void)preserve_expr;
    if (out_cmd) memset(out_cmd, 0, sizeof(*out_cmd));
    if (text_out && text_sz > 0) text_out[0] = '\0';
    return 0;
}
int  repl_promote_example_if_needed(void)                  { return 0; }
int  repl_range_contains_var_decl(int start, int count) {
    (void)start; (void)count; return 0;
}
void repl_reformat_program(void)                           { }

/* ---- repl_user_scene_* / repl_load_scene_* (no scenes in demo) -- */

int         repl_load_scene_as_new_slot(const char *path)  { (void)path; return -1; }
const char *repl_user_scene_name(int slot)                 { (void)slot; return NULL; }
int         repl_user_scene_rename(int slot, const char *new_name) {
    (void)slot; (void)new_name; return 0;
}
int         repl_user_scene_slot_used(int slot)            { (void)slot; return 0; }

/* ---- editor-internal helpers defined in src/repl/core.c --------- */

int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out) {
    (void)pos; (void)vars; (void)max_vars;
    if (total_out) *total_out = 0;
    return 0;
}
void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][16], int param_count) {
    (void)indent; (void)fn; (void)param_names; (void)param_count;
    if (out && out_sz > 0) out[0] = '\0';
}
int input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars) {
    (void)s; (void)vars; (void)num_vars; return 0;
}
int parse_repl_func_signature(const char *src, int *fn,
                              char param_names[][16], int max_params,
                              int *param_count) {
    (void)src; (void)fn; (void)param_names; (void)max_params;
    if (param_count) *param_count = 0;
    return 0;
}

/* ---- tutorial_* (no tutorial in demo) --------------------------- */

int  tutorial_active(void)                                 { return 0; }
void tutorial_advance_after_successful_commit(void)        { }
void tutorial_begin_expected_commit_attempt(void)          { }
void tutorial_cancel_pending(void)                         { }
const char *tutorial_current_expected_text(void)           { return ""; }
int  tutorial_expected_commit_line(void)                   { return -1; }
int  tutorial_guard_source_change(int line_idx)            { (void)line_idx; return 1; }
void tutorial_handle_commit_attempt(int kind)              { (void)kind; }
int  tutorial_line_is_locked(int line_idx)                 { (void)line_idx; return 0; }
void tutorial_note_expected_commit_applied(void)           { }

/* ---- color_picker_* (no picker in demo) ------------------------- */

void color_picker_close(void)                              { }

/* ---- ui_* (no chrome rendering in demo) ------------------------- */

void ui_layout_code_panel_rect(int *x, int *y, int *w, int *h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
}
void ui_menu_bar_close(void)                                 { }
UiCodePanelRuntimeState ui_state_code_panel(void)            { return g_shim_code_panel; }
UiCodePanelRuntimeState *ui_state_code_panel_mut(void)       { return &g_shim_code_panel; }
UiHelpState  ui_state_help(void)                             { return g_shim_help_state; }
UiHelpState *ui_state_help_mut(void)                         { return &g_shim_help_state; }
UiStatusState *ui_state_status_mut(void)                     { return &g_shim_status_state; }
UiViewportState ui_state_viewport(void) {
    UiViewportState v = {0};
    return v;
}
/* ---- glr_* (no controller / app shell in demo) ------------------ */

void glr_camera_controls_reset(void)                         { }
void glr_completion_accept_autocomplete(void)                { }
void glr_ctrl_router_reset_code_panel_drag(void)             { }
void glr_ctrl_sync_ui_chrome(void)                           { }
GlrPresentationState     glr_state_presentation(void)        { return g_shim_presentation; }
GlrPresentationState    *glr_state_presentation_mut(void)    { return &g_shim_presentation; }
