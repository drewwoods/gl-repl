/*
 * src/repl/example_loader.c -- Built-in example loading and metadata handling.
 */
#include "repl/load.h"           /* repl_load_apply_line */
#include "repl/export.h"         /* ReplExportCameraBridge */
#include "repl/command_store.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "repl/text_helpers.h"
#include "source_document.h"     /* source_document_clear */
#include "support/cpuprof.h"     /* prof_histogram_reset on example switch */
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

static int example_line_is_blank_only(const char *line) {
    if (!line)
        return 0;
    while (*line && isspace((unsigned char)*line))
        line++;
    return *line == '\0';
}

static int example_line_is_camera_marker(const char *line) {
    /* Treat punctuation as presentation, not syntax, while requiring the
     * complete alphabetic payload to be exactly "camera". */
    return repl_comment_alpha_payload_equals(line, "camera");
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

    if (!lines || !lines[0] || !example_line_is_camera_marker(lines[0]))
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

    /* Stream the same four transform lines an import would see. A saved
     * file's camera block is four lines too - its `glRotatef(g_angle, 0,1,0)`
     * animation hook *substitutes for* the numeric Y-rotate rather than
     * adding a line (cam_format_block_impl in src/app/glr_camera_export.c) -
     * so an example's block needs no padding to match the parser's shape.
     * Feeding a synthetic hook line here used to desynchronise the parser at
     * the target translate, which failed the whole header and dropped the
     * camera transforms into the document as geometry. */
    if (bridge->reset_import) bridge->reset_import();
    for (int i = 1; i <= REPL_EXPORT_CAMERA_LINES; i++)
        if (!bridge->try_consume_import_line(lines[i])) return 0;
    return 1;
}

int repl_example_consume_camera_header(const char *const *lines) {
    if (!lines || !lines[0] || !example_line_is_camera_marker(lines[0]))
        return 0;
    if (!try_apply_example_camera_header(lines))
        return 0;

    /* The camera transforms are metadata, but their authored marker is useful
     * as a section heading in the expanded (Code Focus off) code panel. Keep
     * its punctuation/case verbatim and indent it to match display() body
     * boilerplate. */
    {
        const char *marker = example_cam_skip_ws(lines[0]);
        ReplImportExportState *io = repl_state_import_export_writable();
        snprintf(io->camera_comment_line, sizeof(io->camera_comment_line),
                 "  %s", marker);
    }
    return 5;
}

static void reset_example_presentation_defaults(int example_idx) {
    repl_dispatch_example_presentation_reset(example_idx);
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

/* ----- Body emission ----- */

static void example_load_error(const char *msg) {
    repl_set_status_error(msg);
    fprintf(stderr, "%s\n", msg);
}

static int example_body_count(const char *const *body, int *count_out) {
    int n = 0;

    if (!count_out)
        return 0;
    while (body && body[n]) {
        if (n >= EXAMPLE_BODY_LINES_MAX) {
            char msg[REPL_DIAG_TEXT_MAX];
            snprintf(msg, sizeof(msg),
                     "Example load failed: body exceeds EXAMPLE_BODY_LINES_MAX=%d",
                     EXAMPLE_BODY_LINES_MAX);
            example_load_error(msg);
            return 0;
        }
        n++;
    }
    *count_out = n;
    return 1;
}

static int apply_example_body_line(const char *line,
                                   int body_line_idx,
                                   int *edit_line_inout) {
    char err[REPL_DIAG_TEXT_MAX] = "";

    if (!repl_load_apply_line(line, err, sizeof(err), edit_line_inout)) {
        char msg[REPL_DIAG_TEXT_MAX];
        snprintf(msg, sizeof(msg),
                 "Example load failed at body line %d: %s",
                 body_line_idx + 1, err[0] ? err : "parse error");
        example_load_error(msg);
        return 0;
    }
    return 1;
}

/* Emit the body one line at a time, in the order its author wrote it.
 *
 * This used to bucket lines into three passes - declarations, then
 * func_def blocks with their leading comments, then everything else - to
 * reproduce a canonical layout. That layout was the loader's own
 * invention: it reordered the file, so the same scene read one way from
 * `./gl-repl scene.glr` (which goes through src/repl/import.c, line by
 * line) and another way from the Scene menu, F12, or --example. The
 * bucketing also defeated the declaration-prologue rule, because
 * declarations were applied into an empty document where no comment
 * existed to keep them below.
 *
 * repl_load_apply_line places each row itself - a declaration lands at the
 * end of the declaration prologue (compile_decl_prologue_end), a func_def
 * keeps its leading comment run - so source order in, source order out.
 * The one thing the decl-first pass bought was registering top-level
 * predef vars before a func body that reads them compiles; in source order
 * that holds by construction for any scene the editor could have produced,
 * since a declaration always precedes its references. A hand-written
 * example that forward-references a global now fails to load with the same
 * diagnostic the REPL gives for typing that line. */
static int emit_example_body_in_source_order(const char *const *body,
                                             int *edit_line_inout) {
    int n = 0;

    if (!body) return 1;
    if (!example_body_count(body, &n))
        return 0;

    for (int i = 0; i < n; i++)
        if (!apply_example_body_line(body[i], i, edit_line_inout))
            return 0;
    return 1;
}

static void reset_example_load_state(int example_idx) {
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
     * EditorState. */
    repl_dispatch_input_reset();
    /* Editor-side transient reset (camera drag / menu / picker /
     * code-panel-drag) remains the controller's responsibility; the
     * example-cycle and example-load handlers perform that reset. */
    repl_eval_init_predef_vars();
    /* Examples use bare funcN; clear any user-aliased names from the
     * outgoing scene so funcN free-slot allocation starts fresh. */
    repl_func_alias_clear_all();
    repl_state_import_export_writable()->camera_comment_line[0] = '\0';
    reset_example_presentation_defaults(example_idx);
}

static int load_example_lines(const char *const *lines,
                              int example_idx) {
    const char *const *body = lines;

    reset_example_load_state(example_idx);

    int cfg_count = 0;
    if (body) {
        cfg_count = consume_example_cfg_header(body);
        body += cfg_count;
    }
    /* Drain the @cfg accumulator: the leading example metadata is
     * parsed into the bag by parse_workspace_header_line; the bridge
     * applies it to live state. */
    repl_export_apply_pending_cfg();

    int metadata_blank_count = 0;
    if (cfg_count > 0) {
        while (body && body[metadata_blank_count] &&
               example_line_is_blank_only(body[metadata_blank_count]))
            metadata_blank_count++;
    }

    if (body && body[metadata_blank_count]) {
        /* Only consume the 5 header lines if the block was actually
         * validated; otherwise leave them for ordinary parsing so a
         * malformed camera header doesn't silently swallow real
         * geometry that happens to follow it (e.g., a truncated 4-line
         * header where the missing slot is filled by the first real
         * line of the example body). */
        int camera_line_count =
            repl_example_consume_camera_header(body + metadata_blank_count);
        if (camera_line_count > 0) {
            body += metadata_blank_count;
            for (int skip = 0; skip < camera_line_count && body[0]; skip++)
                body++;
        }
    }

    /* Examples are loaded via the lean repl_load_apply_line,
     * mirroring src/repl/export.c's importer.
     *
     * Lines emit in the order the .glr author wrote them, so a scene
     * loaded from the catalog reads exactly like the same scene opened
     * as a file (see emit_example_body_in_source_order).
     *
     * Caller-owned cursor: the loader threads
     * the cursor through each per-line apply via a local int
     * starting at 0 (load policy: cursor begins at the top before
     * the body emits). The post-load value is returned to the
     * caller, which applies it to EditorState at the editor boundary. */
    int loader_edit_line = 0;
    if (!emit_example_body_in_source_order(body, &loader_edit_line)) {
        reset_example_load_state(example_idx);
        repl_dispatch_input_reset();
        repl_mark_source_dirty();
        return 0;
    }

    /* Post-load editor cleanup mirrors the pre-load sink dispatch so a
     * stale input line or cursor doesn't survive the loaded body. */
    repl_dispatch_input_reset();
    repl_mark_source_dirty();
    /* Return the canonical post-load cursor: document end. The
     * caller applies this via editor_state_edit_line_set(). */
    return repl_state_document_count();
}

static int load_example_c_source(const char *const *lines,
                                 int example_idx,
                                 const char *name) {
    reset_example_load_state(example_idx);

    int ok = repl_export_load_from_lines(lines, name ? name : "example.c", NULL);
    repl_dispatch_input_reset();
    repl_mark_source_dirty();
    return ok ? repl_state_document_count() : 0;
}

static int load_example(int idx) {
    int count = repl_example_count();
    const char *const *lines;
    const char *name;

    if (idx < 0 || idx >= count) return 0;
    lines = repl_example_lines(idx);
    name = repl_example_name(idx);
    if (!lines || !name) return 0;

    /* Preserve the active user scene, if any, before overwriting live state
     * with an example. Browsing examples does not create a user-scene slot;
     * editing an example promotes it later through repl_promote_transient_if_needed. */
    repl_scenes_save_active_scene_if_any();
    /* Snapshot the user's pre-example presentation cfg so the next
     * transition out of example state can roll back any in-example
     * cfg toggles before applying the destination's saved cfg. */
    repl_scenes_capture_pre_example_cfg_if_entering();

    int new_edit_line;
    if (repl_example_source_format(idx) == REPL_EXAMPLE_SOURCE_C)
        new_edit_line = load_example_c_source(lines, idx, name);
    else
        new_edit_line = load_example_lines(lines, idx);
    if (new_edit_line <= 0)
        return 0;
    repl_state_scenes_set_active_example_idx(idx);
    repl_scenes_mark_example_active();
    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, count, name);
    repl_set_status(msg);
    return new_edit_line;
}

int repl_load_example(int idx) {
    int edit_line = load_example(idx);
    if (edit_line > 0) {
        /* The timing histograms are cumulative and describe the *previous*
         * example's geometry - a heavy scene's spread would haunt a light one
         * for the rest of the session. The EMAs re-converge on their own, so
         * only the histograms need clearing. Done here rather than in the
         * callers because every example path (menu, F12 cycle, --example,
         * startup bootstrap) funnels through this entry point. */
        prof_histogram_reset();
    }
    return edit_line;
}

int repl_load_example_lines(const char *const *lines) {
    /* A caller holding only scene text (test, bench, repl_live_demo) has no
     * example-index context, so pass -1 - the controller's reset
     * still applies global defaults, just no tag-default overrides. */
    return load_example_lines(lines, -1);
}
