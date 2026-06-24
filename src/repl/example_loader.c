/*
 * src/repl/example_loader.c -- Built-in example loading and metadata handling.
 */
#include "repl/load.h"           /* repl_load_apply_line — step 5b */
#include "repl/export.h"         /* ReplExportCameraBridge */
#include "repl/command_store.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include <stdio.h>
#include <string.h>
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "source_document.h"     /* source_document_clear */
#include "config.h"              /* REPL_DIAG_TEXT_MAX */

static const char *example_cam_skip_ws(const char *text) {
    while (*text && isspace((unsigned char)*text))
        text++;
    return text;
}

static const char *example_cam_skip_sep(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == ',' ||
           *text == 'f' || *text == 'F')
        text++;
    return text;
}

static int example_cam_read_floats(const char *text, float *out_vals,
                                   int out_count, const char **end_out) {
    for (int i = 0; i < out_count; i++) {
        char *end = NULL;

        text = example_cam_skip_sep(text);
        out_vals[i] = strtof(text, &end);
        if (end == text)
            return 0;
        text = end;
    }

    if (end_out)
        *end_out = text;
    return 1;
}

static int example_cam_finish_call(const char *text) {
    text = example_cam_skip_sep(text);
    while (*text == ')' || *text == ';' || isspace((unsigned char)*text))
        text++;
    return *text == '\0';
}

static int example_cam_parse_translate(const char *text,
                                       float *x, float *y, float *z) {
    const char *end = NULL;
    float vals[3];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glTranslatef", 12) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 3, &end) ||
        !example_cam_finish_call(end))
        return 0;

    *x = vals[0];
    *y = vals[1];
    *z = vals[2];
    return 1;
}

static int example_cam_parse_rotate(const char *text,
                                    float axis_x, float axis_y, float axis_z,
                                    float *angle_out) {
    const char *end = NULL;
    float vals[4];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glRotatef", 9) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 4, &end) ||
        !example_cam_finish_call(end))
        return 0;

    if (fabsf(vals[1] - axis_x) > 1e-4f ||
        fabsf(vals[2] - axis_y) > 1e-4f ||
        fabsf(vals[3] - axis_z) > 1e-4f)
        return 0;

    *angle_out = vals[0];
    return 1;
}

static int try_apply_example_camera_header(const char *const *lines) {
    /* Validate the example's `// camera` block shape before applying.
     * Pre-7e the loader called glr_camera_set_* directly, dragging
     * the controller's camera storage into the demo link set. The
     * camera bridge is the controller-installed adapter the export
     * importer already uses for `// camera` blocks; routing example
     * loading through the same bridge removes the direct
     * glr_camera_* dependency. */
    float dist_x, dist_y, dist_z;
    float rx, ry;
    float tx, ty, tz;

    if (!lines || !lines[0] || strcmp(lines[0], "// camera") != 0)
        return 0;
    if (!lines[1] || !lines[2] || !lines[3] || !lines[4])
        return 0;

    if (!example_cam_parse_translate(lines[1], &dist_x, &dist_y, &dist_z) ||
        fabsf(dist_x) > 1e-4f || fabsf(dist_y) > 1e-4f ||
        !example_cam_parse_rotate(lines[2], 1.0f, 0.0f, 0.0f, &rx) ||
        !example_cam_parse_rotate(lines[3], 0.0f, 1.0f, 0.0f, &ry) ||
        !example_cam_parse_translate(lines[4], &tx, &ty, &tz))
        return 0;

    /* Apply via the camera bridge. The bridge is unset in the
     * standalone demo (no rendering), where camera state is
     * irrelevant; the example still loads, the camera block is just
     * a no-op. App bridges can consume the validated block directly
     * (used for animated example-camera presets). Older/fallback
     * bridges can still receive the block through the import parser. */
    const ReplExportCameraBridge *bridge = repl_export_camera_bridge();
    if (bridge && bridge->apply_example_block) {
        ReplExportCameraBlock block;

        for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
            snprintf(block.lines[i], sizeof(block.lines[i]), "%s", lines[i + 1]);
        block.present = 1;
        bridge->apply_example_block(&block);
        return 1;
    }

    if (!bridge || !bridge->try_consume_import_line)
        return 1;  /* validated; bridge absent; treat as successfully consumed */

    /* The bridge's import parser is tuned for the 5-line save-file
     * camera block: distance translate, X-rotate, Y-rotate,
     * `glRotatef(g_angle, 0, 1, 0)` animation hook, target
     * translate. Examples ship the 4-line variant without the
     * animation hook; inject a synthetic hook line so the bridge
     * advances through state 3 → state 4 between lines 3 and 4. */
    if (bridge->reset_import) bridge->reset_import();
    if (!bridge->try_consume_import_line(lines[1])) return 0;
    if (!bridge->try_consume_import_line(lines[2])) return 0;
    if (!bridge->try_consume_import_line(lines[3])) return 0;
    if (!bridge->try_consume_import_line("glRotatef(g_angle, 0, 1, 0)")) return 0;
    if (!bridge->try_consume_import_line(lines[4])) return 0;
    return 1;
}

static void reset_example_presentation_defaults(unsigned int tag_mask) {
    /* presentation slice moved to glr_state.c in step 7a; the
     * controller-installed sink does the actual reset. The demo
     * leaves the sink unset and ships without example presentation
     * resets, which is fine because the demo doesn't load examples
     * via this loader.
     *
     * `tag_mask` is forwarded opaquely to the host; the loader does
     * not interpret tag bits. The controller layers tag-specific
     * `@cfg` defaults on top of the global reset before the example's
     * own `@cfg` metadata is consumed below. */
    repl_dispatch_example_presentation_reset(tag_mask);
}

static int example_cfg_extract_slug(const char *text,
                                    char *slug, int slug_sz) {
    const char *end_p = NULL;
    if (!repl_config_extract_slug(text, slug, (size_t)slug_sz, &end_p))
        return 0;
    while (*end_p && isspace((unsigned char)*end_p))
        end_p++;
    return *end_p == '=';
}

#include "repl/cfg_baseline.h"

static int example_cfg_slug_allowed(const char *slug) {
    const ReplConfigBridge *bridge = repl_config_bridge();
    return bridge && bridge->slug_is_scene_subset
        ? bridge->slug_is_scene_subset(slug)
        : 0;
}

static int consume_example_cfg_header(const char *const *lines) {
    int count = 0;

    while (lines && lines[count]) {
        char slug[32];

        if (!example_cfg_extract_slug(lines[count], slug, sizeof(slug)))
            break;
        if (example_cfg_slug_allowed(slug))
            repl_state_parse_workspace_header_line(lines[count]);
        count++;
    }

    return count;
}

/* ----- Body emission with editor-canonical func_def placement ----- */

/* Upper bound on an example's post-metadata body lines. Sizes the
 * func-reorder `kinds[]` buffer below and bounds every emission pass;
 * lines past it are silently dropped, so it must clear the largest
 * example (the "Dusk lighthouse atoll" stress test, ~260 lines). */
#define EXAMPLE_BODY_LINES_MAX 384

#define EXAMPLE_KIND_OTHER      0
#define EXAMPLE_KIND_FUNC_BLOCK 1
#define EXAMPLE_KIND_VAR_DECL   2

/* Empty lines + `// ...` comments are equivalent for leading-run
 * detection: editor_compile_func_def's compile_func_leading_comment_start
 * walks back over BOTH CMD_COMMENT and CMD_EMPTY, so blank lines
 * between func defs travel with the func block they precede. */
static int example_line_is_comment(const char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return 1;
    return line[0] == '/' && line[1] == '/';
}

static int example_line_is_var_decl(const char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    /* Optional `static ` prefix — canonical form per format_decl_text. */
    if (strncmp(line, "static", 6) == 0 && isspace((unsigned char)line[6])) {
        line += 6;
        while (*line && isspace((unsigned char)*line)) line++;
    }
    return strncmp(line, "float ", 6) == 0 ||
           strncmp(line, "float\t", 6) == 0;
}

static int example_line_is_func_def(const char *line) {
    /* Match `funcN(...) {` or `name(...) {` shape. The cheap test
     * is "identifier followed by `(`, with a `{` somewhere after"
     * — that distinguishes a definition from a function CALL.
     * Comments and other prefix forms (control-flow keywords) are
     * rejected by the alphabetic-identifier prefix. */
    while (*line && isspace((unsigned char)*line)) line++;
    if (!isalpha((unsigned char)*line) && *line != '_') return 0;

    /* Reject control-flow keywords whose `keyword(...) { ... }`
     * shape would otherwise look like a func def: `if`, `for`. */
    if (strncmp(line, "for", 3) == 0 &&
        !isalnum((unsigned char)line[3]) && line[3] != '_')
        return 0;
    if (strncmp(line, "if", 2) == 0 &&
        !isalnum((unsigned char)line[2]) && line[2] != '_')
        return 0;

    const char *p = line;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    /* Skip any whitespace between identifier and `(`. */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    return strchr(p, '{') != NULL;
}

static int example_line_brace_delta(const char *line) {
    int delta = 0;
    int in_str = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '"') in_str = !in_str;
        if (in_str) continue;
        if (*p == '{') delta++;
        else if (*p == '}') delta--;
    }
    return delta;
}

static void emit_example_body_two_pass(const char *const *body,
                                       int *edit_line_inout) {
    if (!body) return;

    char err[REPL_DIAG_TEXT_MAX] = "";

    /* Classify each line into one of three buckets so emission
     * matches the editor's canonical layout (decls first, then
     * func_def blocks, then everything else). The float-decl pass
     * runs FIRST so func body lines that reference top-level
     * predef vars (`x`, `y`, `n`, ...) already see them registered
     * by repl_apply_predef_ops when their CMD_VAR_ASSIGN compiles.
     *
     * KIND_VAR_DECL:    `float X[, Y, ...];` at depth 0
     * KIND_FUNC_BLOCK:  func_def line, its body lines, and any
     *                   contiguous depth-0 comments leading up to
     *                   the func_def
     * KIND_OTHER:       setup commands, function calls, non-leading
     *                   comments — anything that should land below
     *                   the func defs in the canonical layout */
    char kinds[EXAMPLE_BODY_LINES_MAX] = {0};
    int n = 0;
    while (n < EXAMPLE_BODY_LINES_MAX && body[n]) n++;

    int depth = 0;
    int comment_run_start = -1;

    for (int i = 0; i < n; i++) {
        int delta = example_line_brace_delta(body[i]);

        if (depth > 0) {
            kinds[i] = EXAMPLE_KIND_FUNC_BLOCK;
            depth += delta;
            if (depth < 0) depth = 0;
            comment_run_start = -1;
            continue;
        }

        if (example_line_is_func_def(body[i])) {
            /* Mark only OTHER (comment) lines in the pending run as
             * FUNC_BLOCK; var_decls keep their VAR_DECL bucket so
             * pass 1 still emits them in the decl pass. */
            if (comment_run_start >= 0) {
                for (int j = comment_run_start; j < i; j++)
                    if (kinds[j] == EXAMPLE_KIND_OTHER)
                        kinds[j] = EXAMPLE_KIND_FUNC_BLOCK;
            }
            kinds[i] = EXAMPLE_KIND_FUNC_BLOCK;
            depth += delta;
            if (depth < 0) depth = 0;
            comment_run_start = -1;
            continue;
        }

        if (example_line_is_var_decl(body[i])) {
            /* Var decls auto-promote to the top of non-decl code
             * inside the lean loader, so a depth-0 comment above
             * `float X;` is still a leading comment for the next
             * func_def — the decl moves out of the way at apply
             * time. Keep the pending comment run alive. */
            kinds[i] = EXAMPLE_KIND_VAR_DECL;
            continue;
        }

        if (example_line_is_comment(body[i])) {
            if (comment_run_start < 0) comment_run_start = i;
            continue;
        }

        comment_run_start = -1;
    }

    /* Pass 1: float decls (so predef-var registrations are live
     * before func bodies that reference them compile). */
    for (int i = 0; i < n; i++) {
        if (kinds[i] != EXAMPLE_KIND_VAR_DECL) continue;
        if (!repl_load_apply_line(body[i], err, sizeof(err), edit_line_inout))
            err[0] = '\0';  /* soft-fail: keep going on parse errors */
    }
    /* Pass 2: func_def blocks (with leading comments). */
    for (int i = 0; i < n; i++) {
        if (kinds[i] != EXAMPLE_KIND_FUNC_BLOCK) continue;
        if (!repl_load_apply_line(body[i], err, sizeof(err), edit_line_inout))
            err[0] = '\0';
    }
    /* Pass 3: everything else (setup commands, function calls,
     * non-leading comments). */
    for (int i = 0; i < n; i++) {
        if (kinds[i] != EXAMPLE_KIND_OTHER) continue;
        if (!repl_load_apply_line(body[i], err, sizeof(err), edit_line_inout))
            err[0] = '\0';
    }
}

static int load_example_lines(const char *const *lines,
                              unsigned int tag_mask) {
    const char *const *body = lines;
    ReplCommandStore store = repl_command_store_live();

    /* Ask the host to restore tutorial-mutated cfg before the example
     * overwrites presentation cfg. The standalone demo leaves this bridge
     * unset because it never starts tutorials. */
    repl_dispatch_tutorial_teardown();
    repl_command_store_load(&store, NULL, 0);
    source_document_clear();
    repl_state_flat_program_set_count(0);
    /* Editor-input cleanup (insert mode off, input buffer wipe, cursor
     * home, pending newline clear) routes through the controller-
     * installed sink so the REPL pipeline doesn't reach into
     * EditorState (implemented in Phase 3 of
     * feature/source-document-port.md). */
    repl_dispatch_input_reset();
    /* Editor-side transient reset (camera drag / menu / picker /
     * code-panel-drag) is the controller's responsibility — see
     * cycle_example_or_user_scene in glr_ctrl.c and the
     * example-load menu handler in glr_actions.c. The call moved out
     * of this REPL-side loader (implemented in step 2 of the decouple
     * plan). */
    repl_eval_init_predef_vars();
    /* Examples use bare funcN; clear any user-aliased names from the
     * outgoing scene so funcN free-slot allocation starts fresh. */
    repl_func_alias_clear_all();
    reset_example_presentation_defaults(tag_mask);

    if (body)
        body += consume_example_cfg_header(body);
    /* Drain the @cfg accumulator: the leading example metadata is
     * parsed into the bag by parse_workspace_header_line; the bridge
     * applies it to live state. This moved out of an inline
     * glr_config_set chain (implemented in step 4 of the decouple
     * plan). */
    repl_export_apply_pending_cfg();

    if (body && body[0] && strcmp(body[0], "// camera") == 0) {
        /* Only consume the 5 header lines if the block was actually
         * validated; otherwise leave them for ordinary parsing so a
         * malformed camera header doesn't silently swallow real
         * geometry that happens to follow it (e.g., a truncated 4-line
         * header where the missing slot is filled by the first real
         * line of the example body). */
        if (try_apply_example_camera_header(body)) {
            for (int skip = 0; skip < 5 && body[0]; skip++)
                body++;
        }
    }

    /* Examples are loaded via the lean repl_load_apply_line,
     * mirroring src/repl/export.c's importer.
     *
     * Two passes match the editor's editor_try_commit_func_def reorder
     * behavior at load time: func_def blocks (with their leading
     * depth-0 comments) emit first, then everything else. The lean
     * loader auto-promotes `float X;` decls to the top of non-decl
     * code regardless of emission order, so two passes are enough
     * to produce the canonical layout the existing fixtures pin.
     *
     * Caller-owned cursor: the loader threads
     * the cursor through each per-line apply via a local int
     * starting at 0 (load policy: cursor begins at the top before
     * the body emits). The post-load value is returned to the
     * caller, which lands it on EditorState above the β boundary
     * (implemented as step 5b and in phase 3.6.4 / 3.6.5; see the
     * edit-line-ownership plan doc). */
    int loader_edit_line = 0;
    emit_example_body_two_pass(body, &loader_edit_line);

    /* Post-load editor cleanup mirrors the pre-load sink dispatch so a
     * stale input line or cursor doesn't survive the loaded body. */
    repl_dispatch_input_reset();
    repl_mark_source_dirty();
    /* Return the canonical post-load cursor: document end. The
     * caller applies this via editor_state_edit_line_set(). */
    return repl_state_document_count();
}

static int load_example(int idx) {
    int count = repl_example_count();
    const char *const *lines;
    const char *name;

    if (idx < 0 || idx >= count) return 0;
    lines = repl_example_lines(idx);
    name = repl_example_name(idx);
    if (!lines || !name) return 0;

    /* Preserve the user's work (once, into slot 0) before overwriting with
     * an example. Subsequent example loads leave the home slot untouched. */
    repl_scenes_save_active_scene_if_any();
    repl_scenes_capture_home_if_needed();
    /* Snapshot the user's pre-example presentation cfg so the next
     * transition out of example state can roll back any in-example
     * cfg toggles before applying the destination's saved cfg. */
    repl_scenes_capture_pre_example_cfg_if_entering();

    int new_edit_line = load_example_lines(lines, repl_example_tag_mask(idx));
    repl_state_scenes_set_active_example_idx(idx);
    repl_scenes_mark_example_active();
    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, count, name);
    repl_set_status(msg);
    return new_edit_line;
}

int repl_load_example(int idx) {
    return load_example(idx);
}

int repl_load_example_lines_for_test(const char *const *lines) {
    /* Test/bench harness has no example-index context, so pass a zero
     * tag mask — the controller's reset still applies global defaults,
     * just no tag-default overrides. */
    return load_example_lines(lines, 0u);
}
