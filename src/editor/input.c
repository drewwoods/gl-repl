/*
 * editor_input.c — Editor input dispatch.
 *
 * Owns text-document dispatch only: cursor / selection / scroll /
 * search / autocomplete / clipboard / undo / commit chain / code-panel
 * mouse-and-resize / divider hover. Non-editor concerns (replay,
 * audio, config, save / debug / quit, scene cycle, variable panel,
 * scene press, camera, scroll-wheel zoom) live in src/app/glr_ctrl.c and
 * are routed there before the editor's keyboard_func / special_func /
 * mouse_func / motion_func / mousewheel_func / passive_motion_func
 * dispatchers run.
 *
 * Editor-owned concerns hosted here:
 *  - Effect accumulation (g_pending_input_effects + reset/take helpers)
 *  - Modifier provider test seam
 *  - Cmd-range deletion + editor_clear_all_cmds
 *  - editor_load_line_to_input / save_newline_buf / line navigation
 *  - Commit-attempt orchestration (try_commit_*, navigation commit,
 *    parse_input_for_enter_commit, rewrite_source_text_with_indent,
 *    editor_resolve_insert_idx, editor_place_parsed_command)
 *  - Code-panel-hidden helpers used by the keyboard dispatch
 *  - keyboard_func / special_func / mouse_func / motion_func /
 *    passive_motion_func / mousewheel_func bodies (editor routes only)
 *  - Hit-test predicates (point_in_code_panel, point_on_code_panel_divider,
 *    code_panel_resize_cursor) used by the controller to decide who
 *    owns a click
 *  - editor_feed_line() programmatic commit entry
 *
 * Cross-domain concerns the editor used to reach for directly
 * (camera reset, hidden code-panel restore, app-frame transients
 * reset) moved to src/app/glr_ctrl.c per audit #8. The editor talks
 * to the controller through the EditorInputDispatchEffects struct
 * (restore_hidden_code_panel flag) or a registered provider hook
 * (EditorCodePanelLayoutProvider for layout reads).
 */

#include "state.h"
#include "clipboard.h"
#include "commit.h"
#include "completion.h"
#include "edit_ops.h"
#include "help_session.h"
#include "inline_file_prompt.h"
#include "inline_rename.h"
#include "input.h"
#include "reformat.h"
#include "search.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "undo.h"

#include "keys.h"
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "ui/app/layout.h"
#include "ui/app/state.h"

#include "config.h"            /* REPL_INDENT_TEXT_MAX */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* macOS Cmd-key support. The freeglut-fork sets GLUT_ACTIVE_SUPER
 * when Cmd is held; mainline freeglut and other GLUT builds don't
 * deliver Cmd at all, so on those platforms the constant is absent.
 * Define it to 0 there so the bitwise checks below compile out to
 * no-ops without #ifdef-cluttering the call sites. */
#ifndef GLUT_ACTIVE_SUPER
#define GLUT_ACTIVE_SUPER 0
#endif

/* Forward declarations. */
static void keyboard_func(unsigned char key, int x, int y);
static void special_func(int key, int x, int y);
static void mouse_func(int button, int state, int x, int y);
static void motion_func(int x, int y);
static void passive_motion_func(int x, int y);
#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y);
#endif

typedef enum {
    COMMIT_UNCHANGED,
    COMMIT_OK,
    COMMIT_REJECTED
} CommitResult;

static EditorModifierProvider g_modifier_provider_for_test = NULL;
static EditorInputDispatchEffects g_pending_input_effects = {0};

void editor_input_set_modifier_provider_for_test(EditorModifierProvider provider) {
    g_modifier_provider_for_test = provider;
}

void editor_reset_input_effects(void) {
    g_pending_input_effects = (EditorInputDispatchEffects){0};
}

/* Snapshot the per-dispatch effects struct AND reset the file-
 * scope storage in one call. The reset is the load-bearing half:
 * most callers don't manually reset between dispatches, so this
 * helper guarantees the next dispatch starts from a clean slate.
 * The name spells the side effect out explicitly. */
EditorInputDispatchEffects editor_take_and_reset_input_effects(void) {
    EditorInputDispatchEffects out = g_pending_input_effects;
    editor_reset_input_effects();
    return out;
}

/* Production sets this from src/app/glr_ctrl.c after glutInit so
 * editor_input_active_modifiers() can safely call glutGetModifiers(). Tests
 * that don't install a modifier provider leave it at 0; the read
 * is suppressed and modifier checks see "no modifiers" rather than
 * aborting freeglut for being called pre-init. */
static int g_glut_modifier_reads_enabled = 0;

/* Production installs this from glr_ctrl_init_gl so the editor can
 * query the current code-panel layout without including
 * src/app/glr_state.h directly. Tests that need a non-default
 * layout install their own reader. */
static EditorCodePanelLayoutProvider g_code_panel_layout_provider = NULL;

void editor_input_set_code_panel_layout_provider(EditorCodePanelLayoutProvider provider) {
    g_code_panel_layout_provider = provider;
}

void editor_input_enable_glut_modifier_reads(void) {
    g_glut_modifier_reads_enabled = 1;
}

unsigned char editor_input_normalize_super_to_ctrl(unsigned char key) {
    /* Alpha check first so non-letter keys never read modifiers — that
     * read goes through glutGetModifiers() in production, which aborts
     * pre-init. The controller calls this at the top of its keyboard
     * route, before the cfg-shortcut chain that compares against
     * KEY_CTRL_* constants (= 1..26). Without the translation, Cmd+B
     * would arrive as 'b' (0x62) and miss KEY_CTRL_B (0x02). */
    if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) &&
        (editor_input_active_modifiers() & GLUT_ACTIVE_SUPER)) {
        return (unsigned char)(key & 0x1F);
    }
    return key;
}

int editor_input_active_modifiers(void) {
    int mods;
    if (g_modifier_provider_for_test)
        mods = g_modifier_provider_for_test();
    else if (g_glut_modifier_reads_enabled)
        mods = glutGetModifiers();
    else
        mods = 0;
    /* Treat Cmd (GLUT_ACTIVE_SUPER on the freeglut-fork) as a Ctrl
     * alias so every existing GLUT_ACTIVE_CTRL check fires on macOS
     * Cmd shortcuts (Cmd+/ for comment toggle, mouse-modifier checks,
     * etc.). The SUPER bit stays visible so keyboard_func can do the
     * letter→control-char translation specifically for Cmd+letter
     * combos without disturbing the real-Ctrl path. */
    if (mods & GLUT_ACTIVE_SUPER)
        mods |= GLUT_ACTIVE_CTRL;
    return mods;
}

/* Binding matcher for keymap.h (declared there; homed here so it reads the
 * canonical modifier accessor above — and so every test's modifier
 * provider flows through unchanged). binding_mods == 0 matches the key
 * regardless of held modifiers (the handler may inspect them itself);
 * a non-zero binding_mods additionally requires those bits to be held. */
int keymap_event_is(int event_key, int binding_key, int binding_mods) {
    if (event_key != binding_key)
        return 0;
    if (binding_mods == 0)
        return 1;
    return (editor_input_active_modifiers() & binding_mods) == binding_mods;
}

void editor_request_redraw(void) {
    g_pending_input_effects.request_redraw = 1;
}

void editor_set_cursor(int cursor) {
    g_pending_input_effects.set_cursor = 1;
    g_pending_input_effects.cursor = cursor;
}

void editor_request_close_help(void) {
    g_pending_input_effects.close_help_overlay = 1;
}

/* Editor-side tutorial adapters.  These are NOT misplaced tutorial policy —
 * the real matching/step/locked-line rules live in tutorial.c.  These statics
 * bridge editor facts (input text, cursor position, insert mode) into the
 * tutorial policy API and translate the result back into editor side effects
 * (completion clear, status message).  They stay here because they read editor
 * state that tutorial.c must not include directly. */
static int tutorial_precheck_current_input(void);
static void tutorial_advance_if_commit_ok(CommitResult result);

static int tutorial_guard_source_change_or_status(int pos,
                                                  int delete_count,
                                                  int insert_count) {
    if (tutorial_guard_source_change(pos, delete_count, insert_count))
        return 1;
    repl_set_status_error("Tutorial line is read-only");
    return 0;
}

static int tutorial_guard_pending_input_commit_or_status(int enter_mode) {
    int pos;
    int delete_count = 0;
    int insert_count = 0;

    if (!tutorial_active())
        return 1;

    pos = editor_state_edit_line();
    if (editor_insert_mode() || pos >= repl_state_document_count()) {
        if (editor_insert_mode() || enter_mode || editor_state_input().input_len > 0)
            insert_count = 1;
    } else if (editor_state_input().input_len > 0) {
        delete_count = 1;
        insert_count = 1;
    }

    if (delete_count == 0 && insert_count == 0)
        return 1;
    return tutorial_guard_source_change_or_status(pos, delete_count, insert_count);
}


void editor_delete_cmd_range(int start, int count, const char *what) {
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    if (!editor_selection_normalize_cmd_range(start, count, &start, &count))
        return;
    if (!tutorial_guard_source_change_or_status(start, count, 0))
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    err[0] = '\0';

    if (repl_compile_delete_range(start, count, &ctx, &change,
                                  err, sizeof(err)) != REPL_COMPILE_OK) {
        if (err[0]) repl_set_status_error(err);
        return;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE)
        return;

    /* Editor framing: verb chosen by caller, not by compile. */
    snprintf(change.commit_message, sizeof(change.commit_message),
             "%s %d line%s",
             what, change.count, change.count > 1 ? "s" : "");

    if (!editor_commit_apply_external_change(&change, /*capture_undo=*/1, /*publish_status=*/1)) {
        repl_set_status_error("Command buffer error");
        return;
    }

    editor_state_edit_line_set(change.pos);
    if (editor_state_edit_line() > repl_state_document_count())
        editor_state_edit_line_set(repl_state_document_count());
    editor_load_line_to_input(editor_state_edit_line());
    repl_mark_source_dirty();
    editor_selection_clear_line_range();
}

static void editor_reset_document_to_empty(void) {
    color_picker_stop();
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_clear(&store);
    /* Editor owns the source-text buffer, not just a UI mirror of
     * the REPL command-store. A wholesale "clear all cmds" has to
     * drop the buffer too, otherwise the cleared command-array and
     * the surviving editor text drift out of lockstep — the user
     * sees the old lines in the code panel while every commit acts
     * on an empty cmd-store (implemented in Phase 4 of
     * plans/done/edit-line-ownership.md). */
    editor_buffer_clear();
    editor_state_edit_line_set(0);
    editor_insert_mode_set(0);
    editor_input_clear();
    editor_pending_newline_clear();
    repl_eval_init_predef_vars();
    repl_mark_source_dirty();
}

void editor_clear_all_cmds(void) {
    if (!tutorial_guard_source_change_or_status(0, repl_state_document_count(), 0))
        return;

    editor_undo_push_snapshot();
    editor_reset_document_to_empty();
    repl_set_status("All commands cleared");
}

void editor_reset_for_new_scene(void) {
    editor_reset_document_to_empty();
}

void editor_load_line_to_input(int idx) {
    EditorInputState *inp = editor_state_input_mut();
    if (idx >= 0 && idx < repl_state_document_count()) {
        if (tutorial_line_is_locked(idx)) {
            editor_input_clear();
            editor_cursor_pos_set(0);
            repl_set_status_error("Tutorial line is read-only");
            return;
        }

        const char *s = editor_buffer_line(idx);
        while (*s && isspace((unsigned char)*s))
            s++;

        if (repl_line_is_label(idx)) {
            if (*s == ':') {
                /* Label written as ":name"; strip trailing whitespace
                 * only — keep the ':' and the body verbatim. */
                int len = (int)strlen(s);
                while (len > 0 && isspace((unsigned char)s[len - 1]))
                    len--;
                if (len >= MAX_INPUT_LEN)
                    len = MAX_INPUT_LEN - 1;
                memcpy(inp->input, s, (size_t)len);
                inp->input[len] = '\0';
                inp->input_len = len;
                editor_cursor_pos_set(inp->input_len);
                return;
            }

            /* Label written as "name:"; rewrite to ":name" and strip
             * trailing ':' plus whitespace from the source. */
            int len = (int)strlen(s);
            while (len > 0 &&
                   (s[len - 1] == ':' || isspace((unsigned char)s[len - 1])))
                len--;
            if (len > MAX_INPUT_LEN - 2) /* -1 NUL, -1 leading ':' */
                len = MAX_INPUT_LEN - 2;
            inp->input[0] = ':';
            memcpy(inp->input + 1, s, (size_t)len);
            inp->input[len + 1] = '\0';
            inp->input_len = len + 1;
            editor_cursor_pos_set(inp->input_len);
            return;
        }

        /* Default: skip leading whitespace, drop trailing ';' + ws. */
        const char *cs;
        int len;
        repl_canonical_input_view(editor_buffer_line(idx), &cs, &len);
        if (len >= MAX_INPUT_LEN)
            len = MAX_INPUT_LEN - 1;
        memcpy(inp->input, cs, (size_t)len);
        inp->input[len] = '\0';
        inp->input_len = len;
        editor_cursor_pos_set(len);
    } else {
        memcpy(inp->input, inp->pending_newline, (size_t)inp->pending_newline_len + 1);
        inp->input_len = inp->pending_newline_len;
        editor_cursor_pos_set(inp->pending_newline_len);
    }
}

static void save_newline_buf(void) {
    EditorInputState *inp = editor_state_input_mut();
    memcpy(inp->pending_newline, inp->input, (size_t)inp->input_len + 1);
    inp->pending_newline_len = inp->input_len;
}

static int normalize_navigation_target(int target) {
    target = editor_commit_resolve_insert_exit_target(target);
    if (target < 0)
        target = 0;
    if (target > repl_state_document_count())
        target = repl_state_document_count();
    return target;
}

static void navigate_to_line_raw_resolved(int target) {
    if (target == editor_state_edit_line() && !editor_insert_mode())
        return;

    if (editor_state_edit_line() == repl_state_document_count() && !editor_insert_mode())
        save_newline_buf();

    editor_state_edit_line_set(target);
    editor_insert_mode_set(0);
    editor_load_line_to_input(target);
    /* Land back on the tutorial's expected commit line → re-show the
     * shadow ghost. Anywhere else, navigation clears so stale
     * completions from the previous row don't linger. */
    if (tutorial_active() && target == tutorial_expected_commit_line())
        editor_completion_update();
    else
        editor_completion_clear();
}

/* Rewrite the canonical source text for g_input with proper indentation.
 * Strips leading whitespace and trailing `;`/whitespace from g_input,
 * prefixes indent (2 outside a glBegin block, 4 inside), adds 2 spaces per
 * open glPushMatrix scope, then appends `;`.
 * With include_block_depth, adds 2 spaces per open for/func/if scope at pos.
 * Writes the result into text_out[text_sz]. */
static void rewrite_source_text_with_indent(char *text_out, int text_sz,
                                            int pos, int include_block_depth) {
    char stripped[MAX_LINE_LEN];
    const char *sp;
    int slen;
    repl_canonical_input_view(editor_state_input().input, &sp, &slen);
    if (slen >= MAX_LINE_LEN) slen = MAX_LINE_LEN - 1;
    memcpy(stripped, sp, (size_t)slen);
    stripped[slen] = '\0';

    /* Split off a trailing `// ...` so the ';' the formatter re-adds below
     * lands BEFORE the comment (`code; // c`), not after it (`code // c;`).
     * The text is the user's input buffer; a label() format string forbids
     * `//`, so the first `//` here is always the real trailing comment.
     * Plain strstr (not repl_line_trailing_comment) keeps input.c's REPL
     * surface unchanged for the check-editor-repl-surface ratchet. */
    char trailing_comment[MAX_LINE_LEN];
    trailing_comment[0] = '\0';
    {
        char *cmt = strstr(stripped, "//");
        if (cmt) {
            const char *ce = cmt + strlen(cmt);
            while (ce > cmt && isspace((unsigned char)ce[-1])) ce--;
            int clen = (int)(ce - cmt);
            if (clen >= (int)sizeof(trailing_comment))
                clen = (int)sizeof(trailing_comment) - 1;
            memcpy(trailing_comment, cmt, (size_t)clen);
            trailing_comment[clen] = '\0';
            *cmt = '\0';
        }
        /* Trim the code's own trailing ';'/whitespace (repl_canonical_input_view
         * could not, since the comment shielded the line end). */
        int cl = (int)strlen(stripped);
        while (cl > 0 && (stripped[cl - 1] == ';' ||
                          isspace((unsigned char)stripped[cl - 1])))
            stripped[--cl] = '\0';
    }

    int indent_len = repl_source_scope_in_begin_block_at(pos) ? 4 : 2;
    /* glPushMatrix blocks indent their bodies like glBegin, so a command
     * with vars (which routes through this manual rewriter instead of the
     * parser's canonical text) must match the same matrix-depth indent. */
    indent_len += repl_source_scope_matrix_scope_depth_at(pos) * 2;
    if (include_block_depth)
        indent_len += repl_source_scope_block_depth_at(pos) * 2;
    char indent[REPL_INDENT_TEXT_MAX];
    if (indent_len > (int)sizeof(indent) - 1)
        indent_len = (int)sizeof(indent) - 1;
    memset(indent, ' ', (size_t)indent_len);
    indent[indent_len] = '\0';
    if (trailing_comment[0])
        snprintf(text_out, (size_t)text_sz, "%s%s; %s",
                 indent, stripped, trailing_comment);
    else
        snprintf(text_out, (size_t)text_sz, "%s%s;", indent, stripped);
}

/* Surface MAX_EXPR_VARS truncation as a status when the visible-var
 * collector had to drop frames. Called after the parse so it overrides
 * any "undeclared variable" status the parser set when truncation was
 * the actual cause. */
static void warn_if_scope_truncated(int vis_total) {
    if (vis_total <= MAX_EXPR_VARS) return;
    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg),
             "scope has %d loop vars (max %d); deepest iterator vars may appear undeclared",
             vis_total, MAX_EXPR_VARS);
    repl_set_status_error(msg);
}

/* Parse g_input into `cmd` as if it were being committed at source-line
 * `insert_idx`. Used by two Enter-key paths — overwrite-Enter (replace
 * the line under the cursor) and append-at-end Enter (append a new line
 * past document end) — hence the neutral name. Handles the three-way
 * fan-out shared by both paths:
 *   - loop/function locals visible at that line -> parse_with_vars +
 *     reindent
 *   - else, predef vars referenced -> plain parse, mark has_vars, reindent
 *     without vars
 *   - else, plain parse only
 * Returns 1 if parsing succeeded. */

static int parse_input_for_enter_commit(GLCmd *cmd, char *text_out, int text_sz,
                                     int insert_idx) {
    ExprVar vis_vars[MAX_EXPR_VARS];
    int vis_total = 0;
    int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS, &vis_total);
    memset(cmd, 0, sizeof(*cmd));
    if (text_out && text_sz > 0)
        text_out[0] = '\0';
    int parsed;
    char parse_err_buf[REPL_STATUS_TEXT_MAX];
    parse_err_buf[0] = '\0';
    if (num_vis_vars > 0) {
        ReplParseContext parse_ctx = {
            .source_line_idx = insert_idx,
            .vars = vis_vars, .num_vars = num_vis_vars,
            .err_buf = parse_err_buf,
            .err_sz  = (int)sizeof(parse_err_buf),
        };
        ReplParsedLine pl;
        parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
        if (parsed) {
            *cmd = pl.cmd;
            rewrite_source_text_with_indent(text_out, text_sz, insert_idx, 1);
        }
    } else {
        ReplParseContext parse_ctx = {
            .source_line_idx = insert_idx,
            .err_buf = parse_err_buf,
            .err_sz  = (int)sizeof(parse_err_buf),
        };
        ReplParsedLine pl;
        parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
        if (parsed) {
            *cmd = pl.cmd;
            if (repl_eval_input_has_predef_vars(editor_state_input().input)) {
                cmd->has_vars = 1;
                rewrite_source_text_with_indent(text_out, text_sz, insert_idx, 0);
            } else {
                /* No local vars: use the parsed canonical text directly. */
                if (text_out && text_sz > 0)
                    repl_copy_string_fits(text_out, text_sz, pl.text);
            }
        }
    }
    if (!parsed && parse_err_buf[0])
        repl_set_status_error(parse_err_buf);
    warn_if_scope_truncated(vis_total);
    return parsed;
}

/* Resolve the source-document index where a freshly-parsed command should
 * land, derived from the current editor mode and cursor:
 *   - in insert mode → at edit_line (the cursor row)
 *   - else edit_line < doc_count → at edit_line (the replace target)
 *   - else → at doc_count (append at end)
 * Used by every commit site that runs the parse-and-place tail. */
static int editor_resolve_insert_idx(void) {
    int edit = editor_state_edit_line();
    int count = repl_state_document_count();
    if (editor_insert_mode()) return edit;
    if (edit < count)         return edit;
    return count;
}

typedef enum {
    EDITOR_PLACE_INSERTED,    /* inserted at insert_idx in insert mode; edit_line+1 */
    EDITOR_PLACE_REPLACED,    /* replaced at insert_idx; edit_line+1 */
    EDITOR_PLACE_APPENDED,    /* inserted at doc_count; edit_line = new doc_count */
    EDITOR_PLACE_BUFFER_FULL  /* command-store capacity exceeded; no mutation */
} EditorPlaceResult;

/* Commit-site shared tail: place a parsed command into the source document
 * using the right insert/replace/append op based on mode + cursor, and
 * advance edit_line to its post-place resting position. Callers handle
 * their own status messages and side effects (status text, input clear,
 * editor_load_line_to_input, pending_newline) based on the returned
 * result — each caller's user-visible behavior is intentionally distinct. */
static EditorPlaceResult editor_place_parsed_command(int insert_idx,
                                                     const GLCmd *cmd,
                                                     const char *cmd_text) {
    ReplCommandStore store = repl_command_store_live();
    if (editor_insert_mode()) {
        if (!repl_command_store_insert_one(&store, insert_idx, cmd, NULL))
            return EDITOR_PLACE_BUFFER_FULL;
        editor_buffer_insert_line(insert_idx, cmd_text);
        editor_state_edit_line_set(insert_idx + 1);
        return EDITOR_PLACE_INSERTED;
    }
    if (insert_idx < repl_state_document_count()) {
        if (repl_command_store_replace_one(&store, insert_idx, cmd))
            editor_buffer_replace_line(insert_idx, cmd_text);
        editor_state_edit_line_set(insert_idx + 1);
        return EDITOR_PLACE_REPLACED;
    }
    /* append: insert_idx == doc_count */
    if (!repl_command_store_insert_one(&store, insert_idx, cmd, NULL))
        return EDITOR_PLACE_BUFFER_FULL;
    editor_buffer_insert_line(insert_idx, cmd_text);
    editor_state_edit_line_set(repl_state_document_count());
    return EDITOR_PLACE_APPENDED;
}

typedef struct {
    EditorUndoSnapshot undo;
    char input[MAX_INPUT_LEN];
    int input_len;
    int cursor_pos;
    int inserting;
    char newline_buf[MAX_INPUT_LEN];
    int newline_len;
} CommitAttemptState;

/* File-scope (not locals) only to keep these >1 MB structs — each
 * embeds a full EditorUndoSnapshot — off the stack. They are
 * captured and consumed within a single call, never retained
 * across calls. */
static CommitAttemptState g_commit_attempt_before;
static CommitAttemptState g_navigation_commit_before;

static void capture_commit_attempt_state(CommitAttemptState *s) {
    EditorInputView inp = editor_state_input();
    editor_undo_snapshot_save(&s->undo);
    memcpy(s->input, inp.input, sizeof(s->input));
    s->input_len = inp.input_len;
    s->cursor_pos = editor_cursor_pos();
    s->inserting = editor_insert_mode();
    memcpy(s->newline_buf, inp.pending_newline, sizeof(s->newline_buf));
    s->newline_len = inp.pending_newline_len;
}

/* Navigation rejection reverts commands/predefs and the saved append-line
 * buffer.  The transient typed input stays discarded by the undo snapshot
 * restore; captured input fields are used only to detect commit progress. */
static void restore_commit_attempt_committed_state(const CommitAttemptState *s) {
    EditorInputState *inp = editor_state_input_mut();
    memcpy(inp->pending_newline, s->newline_buf, sizeof(s->newline_buf));
    inp->pending_newline_len = s->newline_len;
    editor_undo_snapshot_restore(&s->undo);
}

static int input_matches_committed_line(int line) {
    if (line < 0 || line >= repl_state_document_count())
        return 0;

    const char *s;
    int slen;
    repl_canonical_input_view(editor_buffer_line(line), &s, &slen);

    EditorInputView inp = editor_state_input();
    return slen == inp.input_len && strncmp(inp.input, s, (size_t)slen) == 0;
}

/* The editor_try_commit_* chain returns 1 for BOTH a successful commit
 * and a handled error, so "did anything actually change?" can't be read
 * off the return value — it must be detected structurally by diffing
 * the document/cursor/input against the pre-attempt snapshot. */
static int commit_progressed_since(const CommitAttemptState *s) {
    EditorInputView inp = editor_state_input();
    if (repl_state_document_count() != s->undo.num_cmds ||
        editor_state_edit_line() != s->undo.edit_line ||
        editor_insert_mode() != s->inserting ||
        inp.input_len != s->input_len ||
        editor_cursor_pos() != s->cursor_pos)
        return 1;

    if (memcmp(inp.input, s->input, (size_t)inp.input_len + 1) != 0)
        return 1;

    if (repl_state_document_count() > 0 &&
        memcmp(repl_state_document_cmds(), s->undo.cmds,
               (size_t)repl_state_document_count() * sizeof(GLCmd)) != 0)
        return 1;

    return 0;
}

static int current_input_needs_navigation_commit(void) {
    if (editor_state_input().input_len <= 0 && !editor_insert_mode())
        return 0;
    if (!editor_insert_mode() && editor_state_edit_line() < repl_state_document_count() &&
        input_matches_committed_line(editor_state_edit_line()))
        return 0;
    return 1;
}

/* Shared line-commit path for Enter and navigation.  Enter keeps its
 * line-advance/insert-mode behavior for unchanged lines; navigation treats
 * unchanged input as a no-op and only uses this helper for modified text.
 *
 * ORDERING INVARIANTS (audit #10 — both are load-bearing, in opposite
 * directions; do not "normalize" to a single shape):
 *
 *  1. WITHIN var_statements: float_decl MUST run before assign_variable.
 *     `editor_try_commit_var_statements()` enforces this order — the
 *     reverse would misread `float x` as an assignment to an
 *     identifier named "float". See `editor_try_commit_var_statements`
 *     in src/editor/commit.c and CLAUDE.md "Commit Dispatch Sites".
 *
 *  2. UNDER Enter: block_structs FIRST, then var_statements. Diverges
 *     from the canonical `editor_try_commit_any` order used by the ;-
 *     key and editor_feed_line, where var_statements run first. Under
 *     Enter a closing `}` on its own line must close the active
 *     block, not be misread as a stray-`}` to var-statement fallthrough.
 *     The overwrite-Enter branch additionally uses
 *     `_var_statements_then_insert()` to flip into insert-mode + clear
 *     the input after a successful var-statement commit — a post-effect
 *     `editor_try_commit_any` does not perform.
 *
 * test_repl_editor.c pins both invariants (search for
 * "commit_current_input ordering"). */
/* `needs_commit_hint`: pass 1 when the caller has already verified
 * via `current_input_needs_navigation_commit()` that a commit is
 * needed (skips the redundant re-evaluation here — see
 * `commit_before_navigation`). Pass -1 to make this helper compute
 * the predicate itself; pass 0 to assert no precheck happened (then
 * `enter_mode` is the only thing that can skip the early-exit). */
static CommitResult commit_current_input(int enter_mode,
                                         int needs_commit_hint) {
    int needs_commit = needs_commit_hint;
    if (needs_commit < 0)
        needs_commit = current_input_needs_navigation_commit();
    if (!enter_mode && !needs_commit)
        return COMMIT_UNCHANGED;

    if (!editor_insert_mode() && editor_state_edit_line() < repl_state_document_count()) {
        int unmodified = (editor_state_input().input_len == 0 ||
                          input_matches_committed_line(editor_state_edit_line()));
        if (unmodified) {
            if (!enter_mode)
                return COMMIT_UNCHANGED;
            if (editor_cursor_pos() > 0)
                editor_state_edit_line_set(editor_state_edit_line() + 1);
            editor_insert_mode_set(1);
            editor_input_clear();
            editor_completion_clear();
            repl_set_status("Insert mode");
            repl_mark_source_dirty();
            return COMMIT_OK;
        }
    }

    if (!tutorial_guard_pending_input_commit_or_status(enter_mode))
        return COMMIT_REJECTED;

    if (editor_state_input().input_len > 0 ||
        editor_insert_mode() ||
        (enter_mode && editor_state_edit_line() >= repl_state_document_count()))
        editor_undo_push_snapshot();

    CommitAttemptState *before = &g_commit_attempt_before;
    capture_commit_attempt_state(before);

    if ((editor_insert_mode() || editor_state_edit_line() >= repl_state_document_count()) &&
        editor_state_input().input_len > 0 && editor_try_commit_block_structs()) {
        return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
    }

    /* Var statements (float decl / assignment) typed at the trailing
     * append row with insert mode OFF. The ;-key route reaches these via
     * editor_try_commit_any; the Enter / navigation route would otherwise
     * fall through to the GL-command parser below, which rejects
     * `float n = 1;` — a tutorial REQUIRE_VAR declaration step parks the
     * cursor exactly here, so Enter (not just ';') must commit the decl.
     * block_structs already ran above, preserving the Enter "block_structs
     * first" ordering invariant (#2); the insert-mode branch below keeps
     * owning its own var-statement commits. */
    if (!editor_insert_mode() &&
        editor_state_edit_line() >= repl_state_document_count() &&
        editor_state_input().input_len > 0 &&
        editor_try_commit_var_statements()) {
        return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
    }

    if (editor_insert_mode()) {
        if (editor_state_input().input_len == 0) {
            ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
            ReplCompiledChange change;
            char err[REPL_STATUS_TEXT_MAX] = "";
            int insert_pos = editor_state_edit_line();

            if (repl_compile_empty_line(insert_pos, &ctx, &change,
                                        err, sizeof(err)) != REPL_COMPILE_OK) {
                repl_set_status(err[0] ? err : "Cannot insert blank line");
                return COMMIT_REJECTED;
            }
            if (!editor_commit_apply_external_change(&change, /*capture_undo=*/0, /*publish_status=*/1)) {
                repl_set_status("Command buffer full!");
                return COMMIT_REJECTED;
            }
            if (enter_mode)
                editor_state_edit_line_set(insert_pos + 1);
            editor_input_clear();
            return COMMIT_OK;
        }

        if (editor_state_input().input_len > 0) {
            GLCmd cmd;
            int parsed;
            int insert_idx = editor_state_edit_line();
            ExprVar vis_vars[MAX_EXPR_VARS];
            int vis_total = 0;
            int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS, &vis_total);

            char cmd_text[MAX_LINE_LEN] = "";
            char parse_err_buf[REPL_STATUS_TEXT_MAX];
            parse_err_buf[0] = '\0';
            memset(&cmd, 0, sizeof(cmd));
            if (num_vis_vars > 0) {
                ReplParseContext parse_ctx = {
                    .source_line_idx = insert_idx,
                    .vars = vis_vars, .num_vars = num_vis_vars,
                    .err_buf = parse_err_buf,
                    .err_sz  = (int)sizeof(parse_err_buf),
                };
                if (editor_try_commit_var_statements())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                ReplParsedLine pl;
                parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
                if (parsed) {
                    cmd = pl.cmd;
                    rewrite_source_text_with_indent(cmd_text, sizeof(cmd_text),
                                                    insert_idx, 1);
                }
            } else {
                ReplParseContext parse_ctx = {
                    .source_line_idx = insert_idx,
                    .err_buf = parse_err_buf,
                    .err_sz  = (int)sizeof(parse_err_buf),
                };
                ReplParsedLine pl;
                parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
                if (parsed) {
                    cmd = pl.cmd;
                    repl_copy_string_fits(cmd_text, sizeof(cmd_text), pl.text);
                }
            }
            if (!parsed && parse_err_buf[0])
                repl_set_status_error(parse_err_buf);

            if (parsed) {
                EditorPlaceResult res =
                    editor_place_parsed_command(insert_idx, &cmd, cmd_text);
                if (res == EDITOR_PLACE_BUFFER_FULL) {
                    repl_set_status("Command buffer full!");
                    return COMMIT_REJECTED;
                }
                editor_input_clear();
                repl_set_status("Inserted");
                warn_if_scope_truncated(vis_total);
                return COMMIT_OK;
            }
            warn_if_scope_truncated(vis_total);
            return COMMIT_REJECTED;
        }

        if (enter_mode) {
            editor_insert_mode_set(0);
            if (editor_state_edit_line() <= repl_state_document_count())
                editor_load_line_to_input(editor_state_edit_line());
            return COMMIT_OK;
        }
        return COMMIT_UNCHANGED;
    }

    if (editor_state_edit_line() < repl_state_document_count()) {
        int can_advance = 1;

        if (editor_state_input().input_len > 0) {
            /* Sticky-edit semantics on a structured-block head: try
             * the block-shaped commit chain first; if no block-shaped
             * commit succeeds, hold the cursor on this line so the
             * user keeps editing the header rather than auto-advancing
             * past a half-edited block. The CmdType-specific dispatch
             * collapses into one chain because each try_commit_*
             * already validates input shape internally. */
            if (repl_line_is_block_head(editor_state_edit_line())) {
                if (editor_try_commit_block_structs())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (editor_try_commit_var_statements_then_insert())
                return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;

            GLCmd cmd;
            char cmd_text[MAX_LINE_LEN] = "";
            int parsed = parse_input_for_enter_commit(&cmd, cmd_text, sizeof(cmd_text),
                                                   editor_state_edit_line());
            if (parsed) {
                ReplCommandStore store = repl_command_store_live();
                int replace_idx = editor_state_edit_line();
                if (repl_command_store_replace_one(&store, replace_idx, &cmd))
                    editor_buffer_replace_line(replace_idx, cmd_text);
            } else {
                can_advance = 0;
            }
        }

        if (can_advance) {
            editor_state_edit_line_set(editor_state_edit_line() + 1);
            editor_insert_mode_set(1);
            editor_input_clear();
            repl_set_status("Insert mode");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    if (enter_mode && editor_state_input().input_len == 0) {
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        ReplCompiledChange change;
        char err[REPL_STATUS_TEXT_MAX] = "";
        int insert_pos = repl_state_document_count();

        if (repl_compile_empty_line(insert_pos, &ctx, &change,
                                    err, sizeof(err)) != REPL_COMPILE_OK) {
            repl_set_status(err[0] ? err : "Cannot insert blank line");
            return COMMIT_REJECTED;
        }
        if (!editor_commit_apply_external_change(&change, /*capture_undo=*/0, /*publish_status=*/1)) {
            repl_set_status("Command buffer full!");
            return COMMIT_REJECTED;
        }
        editor_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(1);
        editor_input_clear();
        editor_pending_newline_clear();
        return COMMIT_OK;
    }

    if (editor_state_input().input_len > 0) {
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        int parsed = parse_input_for_enter_commit(&cmd, cmd_text, sizeof(cmd_text),
                                               repl_state_document_count());

        if (parsed) {
            EditorPlaceResult res = editor_place_parsed_command(
                repl_state_document_count(), &cmd, cmd_text);
            if (res == EDITOR_PLACE_BUFFER_FULL) {
                repl_set_status("Command buffer full!");
                return COMMIT_REJECTED;
            }
            editor_input_clear();
            editor_pending_newline_clear();
            repl_set_status("OK");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    return COMMIT_UNCHANGED;
}

static CommitResult commit_before_navigation(void) {
    CommitAttemptState *before = &g_navigation_commit_before;
    EditorUndoRingState undo_before;
    char rejected_status[REPL_STATUS_TEXT_MAX];
    int rejected_ttl;

    if (!current_input_needs_navigation_commit())
        return COMMIT_UNCHANGED;

    /* Tutorials route navigation commits through the same precheck +
     * advance as Enter/;. Without this gate, typing any parseable
     * line at the tutorial append row and pressing Up/Down would
     * slip the line into the document without advancing the step,
     * leaving the runner out of sync with the source. */
    if (!tutorial_precheck_current_input())
        return COMMIT_REJECTED;

    capture_commit_attempt_state(before);
    editor_undo_ring_state_capture(&undo_before);
    /* Hint = 1: we already verified above (L883) that the input
     * needs a commit; spare commit_current_input the second call. */
    CommitResult result = commit_current_input(0, 1);
    if (result != COMMIT_REJECTED) {
        tutorial_advance_if_commit_ok(result);
        return result;
    }

    {
        UiStatusState *status = ui_state_status_mut();
        memcpy(rejected_status, status->text, sizeof(rejected_status));
        rejected_ttl = status->ttl;
        restore_commit_attempt_committed_state(before);
        editor_undo_ring_state_restore(&undo_before);
        memcpy(status->text, rejected_status, sizeof(rejected_status));
        status->text[REPL_STATUS_TEXT_MAX - 1] = '\0';
        status->ttl = rejected_ttl;
    }
    /* Idempotent: clears the pending record without shifting
     * anything when the precheck reached _begin but the commit was
     * then rolled back. Safe to dispatch when no pending exists. */
    tutorial_cancel_pending();
    editor_completion_clear();
    return COMMIT_REJECTED;
}

void editor_navigate_to_line(int target) {
    target = normalize_navigation_target(target);
    if (target == editor_state_edit_line() && !editor_insert_mode())
        return;

    if (target != editor_state_edit_line()) {
        int inserted_at = editor_state_edit_line();
        int doc_before  = repl_state_document_count();
        (void)commit_before_navigation();
        if (repl_state_document_count() > doc_before && target > inserted_at)
            target++;
    }

    if (target > repl_state_document_count())
        target = repl_state_document_count();
    navigate_to_line_raw_resolved(target);
}

/* Code-panel-hidden helpers used by both the keyboard and special
 * dispatchers here, and exposed via editor_input_code_panel_*
 * declarations so the controller (src/app/glr_ctrl.c) can decide who
 * owns a click before routing it. The layout itself is owned by the
 * controller; the editor reads it through the provider hook installed
 * by editor_input_set_code_panel_layout_provider. */
int editor_input_code_panel_layout(void) {
    int layout = g_code_panel_layout_provider
        ? g_code_panel_layout_provider()
        : CODE_PANEL_LAYOUT_LEFT;
    if (layout < 0 || layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return layout;
}

int editor_input_code_panel_hidden(void) {
    return editor_input_code_panel_layout() == CODE_PANEL_LAYOUT_HIDDEN;
}

static int editor_key_restores_hidden_code_panel(unsigned char key, int mods) {
    if (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_ALT))
        return 0;
    return key == KEY_BACKSPACE ||
           key == KEY_DELETE ||
           key == '\t' ||
           key == '\r' ||
           key == '\n' ||
           key_is_printable_ascii(key);
}

static void keyboard_begin_key(unsigned char key) {
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
    cb->cursor_visible = 1;
    cb->blink_tick = 0;

    /* Cut / copy / backspace / delete preserve any active line-range
     * selection; everything else clears it before processing the key. */
    if (!keymap_event_is(key, GLR_COPY) && !keymap_event_is(key, GLR_DELETE_LINE) &&
        key != KEY_BACKSPACE && !keymap_event_is(key, GLR_CUT) && key != KEY_DELETE)
        editor_selection_clear_line_range();

    editor_scroll_follow_cursor_set(1);
}

int editor_input_rename_capture_key(unsigned char key) {
    /* Rename overlay captures every keystroke while active, ahead of
     * the backtick/config, replay, and search branches - otherwise
     * typing `, or keys bound to replay would leak out of the rename
     * buffer and trigger unrelated UI. */
    return editor_inline_rename_handle_key(key);
}

int editor_input_file_prompt_capture_key(unsigned char key) {
    /* File-prompt overlay: same hard-modal contract as rename. */
    return editor_inline_file_prompt_handle_key(key);
}

static void restore_hidden_code_panel_for_key(unsigned char key) {
    if (editor_input_code_panel_hidden()) {
        int key_mods = editor_input_active_modifiers();
        if (editor_key_restores_hidden_code_panel(key, key_mods))
            g_pending_input_effects.restore_hidden_code_panel = 1;
    }
}


static int handle_escape_key_route(unsigned char key) {
    if (key == KEY_ESC) {
        /* Escape drops transient state. The input-selection anchor is
         * one such state — clear it alongside whatever specific branch
         * fires below, so Esc is the universal "dismiss" key for both
         * visible selection bands and the other overlays. */
        editor_input_anchor_clear();
        if (editor_state_autocomplete()->match_count > 0) {
            editor_completion_clear();
        } else if (editor_insert_mode()) {
            editor_insert_mode_set(0);
            if (editor_state_edit_line() <= repl_state_document_count())
                editor_load_line_to_input(editor_state_edit_line());
            repl_set_status("Insert mode exited");
        } else {
            editor_input_clear();
            repl_set_status("Input cleared");
        }
        return 1;
    }
    return 0;
}

static int handle_cursor_endpoint_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_LINE_START)) {
        editor_cursor_pos_set(0);
        editor_completion_update();
        return 1;
    }
    if (keymap_event_is(key, GLR_LINE_END)) {
        editor_cursor_pos_set(editor_state_input().input_len);
        editor_completion_update();
        return 1;
    }
    return 0;
}

static int handle_undo_redo_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_UNDO)) {
        if (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT)
            editor_undo_do_redo();
        else
            editor_undo_pop_snapshot();
        return 1;
    }

    if (keymap_event_is(key, GLR_REDO)) {
        editor_undo_do_redo();
        return 1;
    }
    return 0;
}

static int handle_line_delete_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_DELETE_LINE)) {
        if (editor_insert_mode()) {
            editor_insert_mode_set(0);
            if (editor_state_edit_line() <= repl_state_document_count())
                editor_load_line_to_input(editor_state_edit_line());
            repl_set_status("Insert mode exited");
        } else if (editor_clipboard_sel_active()) {
            int start = editor_clipboard_sel_lo();
            int hi = editor_clipboard_sel_hi();
            if (hi >= repl_state_document_count())
                hi = repl_state_document_count() - 1;
            editor_delete_cmd_range(start, hi - start + 1, "Deleted");
        } else if (editor_state_edit_line() < repl_state_document_count()) {
            editor_delete_cmd_range(editor_state_edit_line(), 1, "Deleted");
        }
        return 1;
    }
    return 0;
}

/* Editor halves of the buffer-command route: Ctrl+L clear-all,
 * Ctrl+\ reformat. The save (Ctrl+S), debug dump (Ctrl+P), and quit
 * (Ctrl+Q) variants are router-side. */
static int handle_buffer_command_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_CLEAR_ALL)) {
        editor_clear_all_cmds();
        return 1;
    }

    if (keymap_event_is(key, GLR_REFORMAT)) {
        if (repl_state_document_count() > 0) {
            if (!tutorial_guard_source_change_or_status(
                    0, repl_state_document_count(), repl_state_document_count()))
                return 1;
            editor_undo_push_snapshot();
            editor_reformat_commands();
            repl_set_status("Reformatted command buffer");
        } else {
            repl_set_status("Nothing to reformat");
        }
        return 1;
    }
    return 0;
}

static int handle_copy_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_COPY)) {
        editor_clipboard_copy_current();
        return 1;
    }
    return 0;
}

static int handle_cut_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_CUT)) {
        editor_clipboard_cut_current();
        return 1;
    }
    return 0;
}

static int handle_paste_key_route(unsigned char key) {
    if (keymap_event_is(key, GLR_PASTE)) {
        editor_clipboard_paste_current();
        return 1;
    }
    return 0;
}

static int handle_comment_toggle_key_route(unsigned char key) {
    const char *prefix;
    int line;
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    if (key != '/' || !(editor_input_active_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;

    prefix = editor_line_comment_prefix();
    if (!prefix || !prefix[0])
        return 1;
    if (editor_insert_mode())
        return 1;

    line = editor_state_edit_line();
    if (line < 0 || line >= repl_state_document_count())
        return 1;
    if (!tutorial_guard_source_change_or_status(line, 1, 1))
        return 1;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    err[0] = '\0';

    if (repl_compile_toggle_comment(line, prefix, &ctx, &change,
                                    err, sizeof(err)) != REPL_COMPILE_OK) {
        /* *2 allows prepending "Toggle failed: " to the error message. */
        char msg[REPL_STATUS_TEXT_MAX * 2];
        snprintf(msg, sizeof(msg), "Toggle failed: %s",
                 err[0] ? err : "not a valid command");
        repl_set_status_error(msg);
        return 1;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE)
        return 1;

    if (!editor_commit_apply_external_change(&change, /*capture_undo=*/1, /*publish_status=*/1)) {
        repl_set_status_error("Command buffer error");
        return 1;
    }

    editor_load_line_to_input(editor_state_edit_line());
    repl_mark_source_dirty();
    return 1;
}

static int handle_text_delete_key_route(unsigned char key) {
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        /* Input-buffer selection wins over the line-range selection.
         * Editing the input row is the more local concern and matches
         * standard editor behavior. Works in insert mode too — the
         * line-range guard below only matters when no input selection
         * is active. Both Backspace and Delete consume the selection
         * the same way; the asymmetry below only applies to the
         * cursor-only buffer mutation. */
        if (edit_op_consume_input_selection()) {
            editor_completion_update();
            return 1;
        }
        if (editor_clipboard_sel_active() && !editor_insert_mode()) {
            int start = editor_clipboard_sel_lo();
            int hi = editor_clipboard_sel_hi();
            if (hi >= repl_state_document_count())
                hi = repl_state_document_count() - 1;
            editor_delete_cmd_range(start, hi - start + 1, "Deleted");
            return 1;
        }
        /* Standard editor semantics (audit #7): Backspace deletes the
         * character to the left, Delete deletes the character at the
         * cursor. The two used to collapse to delete-left until the
         * delete-right primitive landed. */
        int changed = (key == KEY_BACKSPACE)
            ? edit_op_buffer_delete_left_of_cursor()
            : edit_op_buffer_delete_right_of_cursor();
        if (changed)
            editor_completion_update();
        return 1;
    }
    return 0;
}

static int handle_tab_key_route(unsigned char key) {
    if (key == '\t') {
        const char *expected;

        /* Tab/Enter/; clear the input-selection anchor without
         * deleting: the commit/accept behavior runs on the buffer as
         * it stands. Selecting "foo" inside `bar foo;` and pressing
         * `;` should commit `bar foo;`, not delete the selection
         * first. */
        editor_input_anchor_clear();
        expected = tutorial_current_expected_text();
        if (tutorial_active() && expected &&
            editor_state_edit_line() == tutorial_expected_commit_line()) {
            editor_input_set_text(expected);
            editor_completion_clear();
            repl_set_status(
                "Replaced input with expected tutorial command; press ; to commit");
            return 1;
        }
        /* Accept the autocomplete ghost on Tab. REQUIRE_VAR tutorial
         * steps populate ac->ghost directly (the synthesized
         * `float n = 5` / `n = 10` suffix) with NO match-list entry, so
         * the match_count gate alone would make Tab a silent no-op on
         * those steps. Gate on a non-empty ghost during a REQUIRE_VAR
         * step too; editor_completion_accept appends ac->ghost either
         * way. (REQUIRE_VAR's `expected` is NULL, so the COMMAND
         * autofill branch above never fires for it.) */
        if (editor_state_autocomplete()->match_count > 0 ||
            (tutorial_active() &&
             tutorial_current_step_kind() == TUTORIAL_STEP_KIND_REQUIRE_VAR &&
             editor_state_autocomplete()->ghost[0] != '\0')) {
            editor_completion_accept();
            editor_completion_update();
        }
        return 1;
    }
    return 0;
}

/* Editor adapter: gathers editor facts (input text, cursor, insert mode)
 * and feeds them into the tutorial policy API.  Returns 1 if the commit
 * may proceed, 0 if the tutorial rejected it (with status set). */
static int tutorial_precheck_current_input(void) {
    TutorialMatchResult result;

    if (!tutorial_active())
        return 1;

    /* SET/REQUIRE steps don't accept typed commits — block here with a
     * kind-appropriate hint and let the SET-step ack key (Enter / Tab /
     * Space, handled in glr_ctrl_keyboard's router) drive advancement
     * instead. Routing the hint through a tutorial_* widget call keeps
     * input.c's direct repl_* surface frozen at its baseline (the
     * check-editor-repl-surface ratchet). */
    if (tutorial_reject_noncommand_commit_with_hint()) {
        editor_completion_clear();
        return 0;
    }

    /* Empty-input silent reject: the cursor lands on
     * expected_commit_line in insert mode at start, and we don't
     * want to spam "expected: ..." before the user has typed
     * anything. Applies to both the ;/Enter path and the
     * navigation auto-commit path; Enter on an empty expected line
     * is accepted as a no-op. */
    if (editor_state_input().input[0] == '\0')
        return 0;

    /* REQUIRE_VAR steps don't pin a single expected commit line — the
     * user is free to type any `name = expr;` (or unrelated source)
     * anywhere they like; the step advances when the watched predef
     * variable reaches its target value (notify hook in
     * apply_compiled_change_full). The expected-line and pending-
     * commit machinery below is COMMAND-only; let normal commits
     * through here unchanged. */
    if (tutorial_current_step_kind() == TUTORIAL_STEP_KIND_REQUIRE_VAR)
        return 1;

    /* The matched commit must land on the current expected line.
     * Anywhere else risks overwriting prior progress or drifting
     * tracked-line indices. */
    if (editor_state_edit_line() != tutorial_expected_commit_line()) {
        repl_set_status("Move cursor to the tutorial insertion line");
        editor_completion_clear();
        return 0;
    }
    /* If the expected line is mid-document, the commit must insert
     * a new row rather than overwrite the line that currently sits
     * there (typically the originally-labeled command shifted down
     * by the runner's instruction-comment splice). */
    if (tutorial_expected_commit_line() < repl_state_document_count() &&
        !editor_insert_mode()) {
        repl_set_status("Tutorial step must insert at the fading line");
        editor_completion_clear();
        return 0;
    }
    if (!tutorial_handle_commit_attempt(editor_state_input().input, &result)) {
        repl_set_status(result.message);
        editor_completion_clear();
        return 0;
    }
    /* Matcher passed. Stamp a pending record so the guard exception
     * and success bookkeeping have an authorized window. Cleared by
     * exactly one of tutorial_note_expected_commit_applied (on
     * COMMIT_OK) or tutorial_cancel_pending (every other outcome). */
    tutorial_begin_expected_commit_attempt();
    return 1;
}

/* Editor adapter: translates the editor's CommitResult into the tutorial
 * policy calls that advance or cancel the pending step. */
static void tutorial_advance_if_commit_ok(CommitResult result) {
    if (!tutorial_active())
        return;
    if (result == COMMIT_OK) {
        /* Only advance from the commit when a pending COMMAND expected-
         * command attempt was in flight (the return value). A free-form
         * REQUIRE_VAR commit sets no pending record and advances via the
         * predef-writeback notify hook inside the commit instead; if that
         * notify already advanced onto a COMMAND step, a second advance
         * here would skip it (instruction comment shown, command never
         * typed — the `float n = ...;` → glBegin skip). */
        if (tutorial_note_expected_commit_applied())
            tutorial_advance_after_successful_commit();
    } else {
        tutorial_cancel_pending();
    }
}

static int handle_enter_key_route(unsigned char key) {
    if (key == '\r' || key == '\n') {
        CommitResult result;

        editor_input_anchor_clear();
        if (editor_state_autocomplete()->match_count > 0) {
            editor_completion_accept();
            editor_completion_update();
            return 1;
        }

        if (!tutorial_precheck_current_input())
            return 1;

        /* enter_mode=1 short-circuits the needs-commit early-exit
         * regardless of the hint; pass -1 (compute) to keep the
         * call site neutral. */
        result = commit_current_input(1, -1);
        tutorial_advance_if_commit_ok(result);
        /* update, not clear: tutorial_advance may have just changed
         * the expected text, and the provider's tutorial branch needs
         * to re-populate the shadow ghost. For non-tutorial mode the
         * update sees empty input and returns early, matching the
         * previous clear-only behavior. */
        editor_completion_update();
        repl_mark_source_dirty();
        return 1;
    }
    return 0;
}

static int handle_semicolon_commit_key_route(unsigned char key) {
    if (key == ';') {
        editor_input_anchor_clear();
        if (editor_state_input().input_len > 0) {
            CommitAttemptState *before = &g_commit_attempt_before;

            if (!tutorial_precheck_current_input())
                return 1;
            if (!tutorial_guard_pending_input_commit_or_status(/*enter_mode=*/0)) {
                tutorial_cancel_pending();
                return 1;
            }

            editor_undo_push_snapshot();
            capture_commit_attempt_state(before);
            if (editor_try_commit_any()) {
                tutorial_advance_if_commit_ok(
                    commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED);
                /* see Enter-route note above: update lets the tutorial
                 * provider re-emit the shadow ghost after the advance. */
                editor_completion_update();
                return 1;
            }
            {
                GLCmd cmd;
                char cmd_text[MAX_LINE_LEN] = "";
                int insert_idx = editor_resolve_insert_idx();
                int parsed;
                ExprVar vis_vars[MAX_EXPR_VARS];
                int vis_total = 0;
                int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS, &vis_total);

                memset(&cmd, 0, sizeof(cmd));
                if (num_vis_vars > 0)
                    parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, vis_vars, num_vis_vars,
                                                             input_has_any_visible_vars(editor_state_input().input, vis_vars, num_vis_vars),
                                                             &cmd, cmd_text, sizeof(cmd_text));
                else
                    parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, NULL, 0,
                                                             repl_eval_input_has_predef_vars(editor_state_input().input),
                                                             &cmd, cmd_text, sizeof(cmd_text));

                if (parsed) {
                    EditorPlaceResult res =
                        editor_place_parsed_command(insert_idx, &cmd, cmd_text);
                    if (res == EDITOR_PLACE_INSERTED) {
                        editor_input_clear();
                        repl_set_status("Inserted");
                    } else if (res == EDITOR_PLACE_REPLACED) {
                        repl_set_status("Line updated");
                        editor_load_line_to_input(editor_state_edit_line());
                    } else if (res == EDITOR_PLACE_APPENDED) {
                        repl_set_status("OK");
                        editor_input_clear();
                        editor_pending_newline_clear();
                    } else {
                        /* EDITOR_PLACE_BUFFER_FULL */
                        repl_set_status("Command buffer full!");
                    }
                }
                warn_if_scope_truncated(vis_total);
            }

            tutorial_advance_if_commit_ok(
                commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED);
        }
        /* see Enter-route note above. */
        editor_completion_update();
        repl_mark_source_dirty();
        return 1;
    }
    return 0;
}

void editor_input_word_bounds_at(const char *text, int len, int char_idx,
                                 int *out_start, int *out_end) {
    int start = char_idx;
    int end = char_idx;

    if (!text || len <= 0 || char_idx < 0 || char_idx >= len ||
        !repl_eval_is_ident_continue((unsigned char)text[char_idx])) {
        if (out_start) *out_start = char_idx;
        if (out_end)   *out_end   = char_idx;
        return;
    }

    while (start > 0 && repl_eval_is_ident_continue((unsigned char)text[start - 1]))
        start--;
    while (end < len && repl_eval_is_ident_continue((unsigned char)text[end]))
        end++;

    if (out_start) *out_start = start;
    if (out_end)   *out_end   = end;
}

static int handle_printable_input_key_route(unsigned char key) {
    if (!key_is_printable_ascii(key))
        return 0;
    if (edit_op_type_char((char)key)) {
        editor_completion_update();
        return 1;
    }
    return 0;
}

/* Editor's keyboard dispatch. Non-editor concerns (config menu,
 * replay forwarding, cfg shortcut, replay toggle, accum samples,
 * save / debug / quit) are routed by src/app/glr_ctrl.c directly to their
 * owning subsystem before this runs — they don't appear here. */
static void keyboard_func(unsigned char key, int x, int y) {
    (void)x;
    (void)y;

    /* Defensive translation. The controller's glr_ctrl_keyboard
     * already normalizes Cmd+letter → control-character before this
     * runs, but tests call editor_handle_key (and thence keyboard_func)
     * directly without going through the controller chain. The
     * helper is a no-op on already-translated keys (control chars
     * fail the alpha check). */
    key = editor_input_normalize_super_to_ctrl(key);

    keyboard_begin_key(key);

    if (editor_input_rename_capture_key(key)) return;
    if (editor_input_file_prompt_capture_key(key)) return;

    restore_hidden_code_panel_for_key(key);

    if (editor_search_handle_key(key))       return;
    if (handle_escape_key_route(key))       return;
    if (handle_cursor_endpoint_key_route(key)) return;
    if (handle_undo_redo_key_route(key))    return;
    if (handle_line_delete_key_route(key))  return;
    if (handle_buffer_command_key_route(key)) return;
    if (handle_copy_key_route(key))         return;
    if (handle_cut_key_route(key))          return;
    if (handle_paste_key_route(key))        return;
    if (handle_comment_toggle_key_route(key)) return;
    if (handle_text_delete_key_route(key))  return;
    if (handle_tab_key_route(key))          return;
    if (handle_enter_key_route(key))        return;
    if (handle_semicolon_commit_key_route(key)) return;
    (void)handle_printable_input_key_route(key);
}

/* Programmatic commit entry for file/example/workspace loading. Mirrors
 * the keyboard ;-key path: load `line` into the input buffer, try the
 * structured editor_try_commit_any() chain, and if that doesn't consume
 * it, run the same general-command parse → command-store → editor-buffer
 * tail used by the Enter/insert handler above (handle_enter_key_route).
 *
 * This tail is intentionally distinct from the interactive ;-key /
 * Enter route: loading replays many lines with no per-line undo
 * snapshot, status message, or tutorial gating (callers bracket the
 * whole load with one undo/clear). Those interactive paths push
 * undo per commit; this is the bulk-loader twin. The two share
 * repl_parse_and_normalize_strict + the
 * repl_command_store_* primitives so parse/apply semantics stay in
 * lockstep even though the surrounding transaction policy differs. */
int editor_feed_line(const char *line) {
    editor_input_set_text(line);

    if (editor_try_commit_any())
        return 1;

    {
        int handled = 0;
        GLCmd cmd;
        int insert_idx = editor_resolve_insert_idx();
        int parsed;
        ExprVar vis_vars[MAX_EXPR_VARS];
        int vis_total = 0;
        int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS, &vis_total);

        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        if (num_vis_vars > 0)
            parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, vis_vars, num_vis_vars,
                                                     input_has_any_visible_vars(editor_state_input().input, vis_vars, num_vis_vars),
                                                     &cmd, cmd_text, sizeof(cmd_text));
        else
            parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, NULL, 0,
                                                     repl_eval_input_has_predef_vars(editor_state_input().input),
                                                     &cmd, cmd_text, sizeof(cmd_text));

        if (parsed) {
            EditorPlaceResult res =
                editor_place_parsed_command(insert_idx, &cmd, cmd_text);
            if (res != EDITOR_PLACE_BUFFER_FULL)
                handled = 1;
        }
        editor_input_clear();
        warn_if_scope_truncated(vis_total);
        return handled;
    }
}

/* ===========================================================================
 * Special-key dispatch (F-keys, arrows, Page Up/Down, Home/End).
 *
 * Editor concerns only: rename modal capture, search-overlay arrows,
 * bare cursor moves (Left/Right + Home/End), autocomplete cycle,
 * shift-extend selection, and code-panel page scroll.
 *
 * Non-editor concerns (replay forwarding, cfg special shortcut,
 * Ctrl+Left/Right audio, help-tab toggle, help-overlay scroll, F1
 * help toggle, F12 scene cycle) live in src/app/glr_ctrl.c and never reach
 * this dispatcher.
 * ===========================================================================
 */

static void special_begin_key(void) {
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
    cb->cursor_visible = 1;
    cb->blink_tick = 0;
    editor_scroll_follow_cursor_set(1);
}

int editor_input_rename_capture_special(int key) {
    /* Rename captures arrows and F-keys ahead of replay/search/navigation so
     * modal text entry cannot leak actions into the editor. */
    return editor_inline_rename_handle_special(key);
}

int editor_input_file_prompt_capture_special(int key) {
    /* File-prompt overlay: same hard-modal contract as rename. */
    return editor_inline_file_prompt_handle_special(key);
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

static void restore_hidden_code_panel_for_special(int key) {
    if (editor_input_code_panel_hidden()) {
        int key_mods = editor_input_active_modifiers();
        if (editor_special_restores_hidden_code_panel(key, key_mods))
            g_pending_input_effects.restore_hidden_code_panel = 1;
    }
}


/* Editor-side cursor moves: bare Left/Right + Home/End. Ctrl+Left/Right
 * (audio prev/next) and help-tab toggling on Left/Right when help is
 * visible are router-side and never reach this dispatcher. */
static int handle_horizontal_special_key_route(int key) {
    /* Shift+Left/Right/Home/End extend the input-buffer selection.
     * editor_cursor_pos_extend_selection pins the pre-move cursor as
     * the anchor on the first extending press (when no selection is
     * active yet) and then grows or shrinks the range. The unshifted
     * cases keep the plain editor_cursor_pos_set, which clears the
     * anchor as part of the default cursor-move policy (Phase B). */
    int shift = (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) != 0;
    int input_len = editor_state_input().input_len;
    int cur = editor_cursor_pos();

    switch (key) {
    case GLUT_KEY_LEFT:
        if (cur > 0) {
            if (shift)
                editor_cursor_pos_extend_selection(cur - 1);
            else
                editor_cursor_pos_set(cur - 1);
        }
        editor_completion_update();
        return 1;
    case GLUT_KEY_RIGHT:
        if (cur < input_len) {
            if (shift)
                editor_cursor_pos_extend_selection(cur + 1);
            else
                editor_cursor_pos_set(cur + 1);
        }
        editor_completion_update();
        return 1;
    case GLUT_KEY_HOME:
        if (shift)
            editor_cursor_pos_extend_selection(0);
        else
            editor_cursor_pos_set(0);
        editor_completion_update();
        return 1;
    case GLUT_KEY_END:
        if (shift)
            editor_cursor_pos_extend_selection(input_len);
        else
            editor_cursor_pos_set(input_len);
        editor_completion_update();
        return 1;
    default:
        return 0;
    }
}

/* Up/Down: autocomplete cycle, shift-extend selection, or move cursor
 * line. Help-overlay scroll on Up/Down is router-side
 * (glr_ctrl_router_handle_help_scroll_special) and never reaches
 * this dispatcher when help is visible. */
static int handle_vertical_special_key_route(int key) {
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    switch (key) {
    case GLUT_KEY_UP:
        if (ac->match_count > 1) {
            ac->selected_idx = (ac->selected_idx - 1 + ac->match_count) % ac->match_count;
            editor_completion_update_selected_preview();
        } else if (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!editor_clipboard_sel_active()) {
                editor_selection_start(editor_state_edit_line());
            }
            int selection_end = editor_selection_end();
            if (selection_end > 0)
                selection_end--;
            editor_selection_set_end(selection_end);
            editor_navigate_to_line(selection_end);
        } else {
            editor_selection_clear_line_range();
            editor_navigate_to_line(editor_state_edit_line() - 1);
        }
        return 1;
    case GLUT_KEY_DOWN:
        if (ac->match_count > 1) {
            ac->selected_idx = (ac->selected_idx + 1) % ac->match_count;
            editor_completion_update_selected_preview();
        } else if (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!editor_clipboard_sel_active()) {
                editor_selection_start(editor_state_edit_line());
            }
            int selection_end = editor_selection_end();
            if (selection_end < repl_state_document_count())
                selection_end++;
            editor_selection_set_end(selection_end);
            editor_navigate_to_line(selection_end);
        } else {
            editor_selection_clear_line_range();
            editor_navigate_to_line(editor_state_edit_line() + 1);
        }
        return 1;
    default:
        return 0;
    }
}


/* PageUp/PageDown scroll the code panel. Help-overlay scroll on the
 * same keys is router-side and never reaches this dispatcher when
 * help is visible. */
static int handle_page_scroll_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_PAGE_UP:
        editor_scroll_set(editor_scroll() - CP_PAGE_SCROLL_ROWS);
        editor_scroll_follow_cursor_set(0);
        return 1;
    case GLUT_KEY_PAGE_DOWN:
        editor_scroll_set(editor_scroll() + CP_PAGE_SCROLL_ROWS);
        editor_scroll_follow_cursor_set(0);
        return 1;
    default:
        return 0;
    }
}

/* Editor's special-key dispatch. Non-editor concerns (replay
 * forwarding, cfg special shortcut, audio prev/next, help tab,
 * help scroll, F1 help toggle, F12 scene cycle) are routed by
 * src/app/glr_ctrl.c directly to their owning subsystem before this runs.
 * The bare cursor moves, autocomplete cycle, selection navigation,
 * and code-panel page scroll stay here. */
static void special_func(int key, int x, int y) {
    (void)x;
    (void)y;

    special_begin_key();

    if (editor_input_rename_capture_special(key)) return;
    if (editor_input_file_prompt_capture_special(key)) return;

    restore_hidden_code_panel_for_special(key);

    if (editor_search_handle_special(key))   return;
    if (handle_horizontal_special_key_route(key)) return;
    if (handle_vertical_special_key_route(key)) return;
    if (handle_page_scroll_special_key_route(key)) return;
}

/* ===========================================================================
 * Mouse / motion / passive-motion / mousewheel dispatch.
 *
 * Editor concerns only: clicks that land on the code panel proper, on
 * the divider, or while the example dropdown is open; ui_panels mouse
 * release on UP; panel-resize end on UP; code-panel resize tracking;
 * code-panel selection drag; divider hover cursor; freeglut wheel
 * scroll when over the code panel.
 *
 * Non-editor concerns (variable-panel drag, scene press / color
 * picker, right-click config dropdown, camera orbit/pan/zoom, camera
 * pointer tracking, help-overlay scroll, scene-area zoom velocity)
 * live in src/app/glr_ctrl.c and never reach this dispatcher.
 * ===========================================================================
 */

int editor_input_point_in_code_panel(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = ui_state_viewport().window_h - y;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    return x >= cp_x && x < cp_x + cp_w &&
           gl_y >= cp_y && gl_y < cp_y + cp_h;
}

int editor_input_point_on_code_panel_divider(int x, int y) {
    int cp_x, cp_y, cp_w, cp_h;
    int gl_y = ui_state_viewport().window_h - y;
    int layout = editor_input_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (layout == CODE_PANEL_LAYOUT_TOP)
        return abs(gl_y - cp_y) < CP_DIVIDER_HIT_PX;
    if (layout == CODE_PANEL_LAYOUT_BOTTOM)
        return abs(gl_y - (cp_y + cp_h)) < CP_DIVIDER_HIT_PX;
    return abs(x - (cp_x + cp_w)) < CP_DIVIDER_HIT_PX;
}

int editor_input_code_panel_resize_cursor(void) {
    return editor_input_code_panel_layout() == CODE_PANEL_LAYOUT_LEFT
         ? GLUT_CURSOR_LEFT_RIGHT
         : GLUT_CURSOR_UP_DOWN;
}

void editor_input_code_panel_scroll(int direction) {
    editor_scroll_set(editor_scroll() + direction);
}

static void editor_update_panel_frac_from_mouse(int x, int y) {
    UiCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
    int layout = editor_input_code_panel_layout();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        return;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int win_h = ui_state_viewport().window_h;
        if (win_h > 0)
            code_panel_state->panel_frac = (float)y / (float)win_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int win_h = ui_state_viewport().window_h;
        if (win_h > 0)
            code_panel_state->panel_frac = (float)(win_h - y) / (float)win_h;
    } else {
        int win_w = ui_state_viewport().window_w;
        if (win_w > 0)
            code_panel_state->panel_frac = (float)x / (float)win_w;
    }

    if (code_panel_state->panel_frac < CFG_PANEL_FRAC_MIN)
        code_panel_state->panel_frac = CFG_PANEL_FRAC_MIN;
    if (code_panel_state->panel_frac > CFG_PANEL_FRAC_MAX)
        code_panel_state->panel_frac = CFG_PANEL_FRAC_MAX;
}

/* Editor-side mouse dispatch reduces to UP-only panel-resize end.
 *
 * The controller (glr_ctrl_mouse) handles every DOWN event by
 * routing the UiHit returned by ui_panels_hit_test through
 * glr_ctrl_router_handle_code_panel_hit. Picker / variable panel
 * / menu / scene / camera / scroll wheel all dispatch from there.
 * The editor only sees UP events, where it clears the resizing flag
 * the controller set on UI_HIT_PANEL_DIVIDER. */
/* GLUT mouse callback shape; only state == GLUT_UP is editor-relevant. */
static void mouse_func(int button, int state, int x, int y) {
    (void)button;
    (void)x;
    (void)y;
    if (state != GLUT_UP)
        return;
    if (ui_state_code_panel().resizing_panel) {
        ui_state_code_panel_mut()->resizing_panel = 0;
        editor_set_cursor(GLUT_CURSOR_INHERIT);
        editor_request_redraw();
    }
}

#ifndef USE_GLUT
/* Editor's freeglut wheel handler scrolls the code panel only. The
 * controller dispatches help-overlay scroll and camera zoom velocity
 * before this runs (when the cursor isn't in the code panel rect). */
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel;
    if (editor_input_point_in_code_panel(x, y)) {
        editor_input_code_panel_scroll(-direction);
        editor_request_redraw();
    }
}
#endif

/* Passive-motion (no button held) updates the editor's hover cursor.
 * Camera pointer tracking happens controller-side first. */
static void passive_motion_func(int x, int y) {
    if (editor_input_point_on_code_panel_divider(x, y))
        editor_set_cursor(editor_input_code_panel_resize_cursor());
    else
        editor_set_cursor(GLUT_CURSOR_INHERIT);
}

/* Editor-side drag-motion only handles panel-resize tracking. The
 * controller dispatches UI overlay motion (color picker), variable-
 * panel drag motion, code-panel selection drag motion (via
 * glr_ctrl_router_handle_code_panel_drag), and camera drag motion
 * before this runs. */
static void motion_func(int x, int y) {
    if (ui_state_code_panel().resizing_panel) {
        editor_update_panel_frac_from_mouse(x, y);
        editor_request_redraw();
    }
}

EditorInputDispatchEffects editor_handle_key(unsigned char key, int x, int y) {
    editor_reset_input_effects();
    keyboard_func(key, x, y);
    return editor_take_and_reset_input_effects();
}

EditorInputDispatchEffects editor_handle_special(int key, int x, int y) {
    editor_reset_input_effects();
    special_func(key, x, y);
    return editor_take_and_reset_input_effects();
}

EditorInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y) {
    editor_reset_input_effects();
    mouse_func(button, state, x, y);
    return editor_take_and_reset_input_effects();
}

EditorInputDispatchEffects editor_handle_motion(int x, int y) {
    editor_reset_input_effects();
    motion_func(x, y);
    return editor_take_and_reset_input_effects();
}

EditorInputDispatchEffects editor_handle_passive_motion(int x, int y) {
    editor_reset_input_effects();
    passive_motion_func(x, y);
    return editor_take_and_reset_input_effects();
}

#ifndef USE_GLUT
EditorInputDispatchEffects editor_handle_mousewheel(int wheel, int direction, int x, int y) {
    editor_reset_input_effects();
    mousewheel_func(wheel, direction, x, y);
    return editor_take_and_reset_input_effects();
}
#endif
