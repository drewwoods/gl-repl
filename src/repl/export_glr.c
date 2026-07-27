/*
 * src/repl/export_glr.c -- Scene-source (.glr) writer.
 *
 * repl_export_save_output() writes a standalone C program: banner,
 * includes, globals, helper functions, display()/reshape()/main() — a
 * file you can hand to a compiler. That is the wrong shape for authoring.
 * A built-in example (a .glr under examples/scenes/) is the REPL's own
 * source text and nothing else:
 *
 *     // @cfg <slug> = <value>     (only where the scene differs from
 *     // @cfg <slug> = <value>      the presentation defaults an example
 *     // camera                     load resets to)
 *     glTranslatef(...);           (the 4-line camera block)
 *     glRotatef(...);
 *     glRotatef(...);
 *     glTranslatef(...);
 *     <the document, verbatim>
 *
 * Symmetric with src/repl/example_loader.c, which reads exactly this:
 * consume_example_cfg_header() then repl_example_consume_camera_header()
 * then the body. Round-tripping a scene through save -> catalog entry ->
 * load is the point of the format, so the two files move together.
 *
 * Only the @cfg rows the loader would actually honour are emitted (the
 * bridge's scene subset), and only where they differ from the default —
 * an example that pins every slug tells a reader nothing about which
 * ones matter to it.
 */
#include "repl/export_internal.h"

/* Document rows carry the display() body's 2-space base indent (the code
 * panel and the C export both render them inside a function). A .glr
 * scene is authored at column 0; the loader re-derives every indent level
 * from block structure as it feeds lines back in, so shedding the base
 * indent here is presentation-only and round-trips exactly. */
#define REPL_GLR_BASE_INDENT 2

static void glr_scene_write_line(FILE *f, const char *line) {
    int skip = 0;

    if (!line) {
        fprintf(f, "\n");
        return;
    }
    while (skip < REPL_GLR_BASE_INDENT && line[skip] == ' ')
        skip++;
    fprintf(f, "%s\n", line + skip);
}

/* Emit `// @cfg slug = value` for every scene-subset slug whose live value
 * differs from its default. A slug the bridge has no default for is emitted
 * unconditionally — better a redundant row than a silently dropped one. */
static void glr_scene_write_cfg_overrides(FILE *f) {
    const ReplConfigBridge *bridge = g_export_cfg_bridge;
    ReplConfigBag live;
    ReplConfigBag defaults;

    if (!bridge || !bridge->fill_scene_subset)
        return;

    repl_config_bag_clear(&live);
    repl_config_bag_clear(&defaults);
    bridge->fill_scene_subset(&live);
    if (bridge->fill_scene_defaults)
        bridge->fill_scene_defaults(&defaults);

    for (int i = 0; i < live.count; i++) {
        const char *def = repl_config_bag_get(&defaults, live.items[i].key);
        if (def && strcmp(def, live.items[i].value) == 0)
            continue;
        fprintf(f, "// @cfg %s = %s\n",
                live.items[i].key, live.items[i].value);
    }
}

/* The camera marker plus the 4-line transform block, in the numeric form the
 * loader parses (fill_display_block, not the saved-C variant that substitutes
 * g_angle). No camera bridge installed — the demo, tests — leaves the block
 * empty, and the scene simply inherits the live camera on load, which is the
 * documented no-header behaviour.
 *
 * A scene loaded from a .glr keeps whatever heading its author wrote
 * (`// --- Camera --- `, say) in camera_comment_line; the marker matcher
 * only requires the alphabetic payload to be "camera", so round-tripping the
 * original text costs nothing and keeps a hand-edited example's section
 * banners intact. Plain `// camera` for scenes that never had one. */
static void glr_scene_write_camera(FILE *f) {
    repl_refresh_camera_lines();
    if (!g_cam_lines[0][0])
        return;
    if (g_camera_comment_line[0])
        glr_scene_write_line(f, g_camera_comment_line);
    else
        fprintf(f, "// camera\n");
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        glr_scene_write_line(f, g_cam_lines[i]);
}

int repl_export_save_glr(const char *filename, SourceTextView text) {
    char msg[REPL_DIAG_TEXT_MAX];
    FILE *f;
    int had_error;
    int close_failed;

    if (!filename || !filename[0]) {
        repl_set_status_error("Error: no .glr path");
        return 0;
    }

    f = fopen(filename, "w");
    if (!f) {
        snprintf(msg, sizeof(msg), "Error: cannot write %s", filename);
        repl_set_status_error(msg);
        return 0;
    }

    export_set_source_text_view(text);

    glr_scene_write_cfg_overrides(f);
    glr_scene_write_camera(f);
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid)
            continue;
        glr_scene_write_line(f, export_document_text(cmd_idx));
    }

    had_error = ferror(f);
    close_failed = fclose(f) != 0;
    if (had_error || close_failed) {
        snprintf(msg, sizeof(msg), "Error: cannot write %s", filename);
        repl_set_status_error(msg);
        return 0;
    }

    snprintf(msg, sizeof(msg), "Saved %s (%d commands)",
             filename, repl_state_document_count());
    repl_set_status(msg);
    return 1;
}
