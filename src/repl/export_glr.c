/*
 * src/repl/export_glr.c -- Scene-source (.glr) writer.
 *
 * repl_export_save_output() writes a standalone C program: banner,
 * includes, globals, helper functions, display()/reshape()/main() - a
 * file you can hand to a compiler. That is the wrong shape for authoring.
 * A built-in example (a .glr under examples/scenes/) is the REPL's own
 * source text and nothing else:
 *
 *     // @cfg <slug> = <value>     (only where the scene differs from
 *     // @cfg <slug> = <value>      the presentation defaults an example
 *                                   load resets to)
 *     float a, b;                  (declarations)
 *     func0(x) { ... }             (function definitions)
 *     glTranslatef(...);  // @camera dist
 *     glRotatef(...);     // @camera rx
 *     glRotatef(...);     // @camera ry
 *     glTranslatef(...);  // @camera pan
 *     <the body>
 *
 * That order - declarations, then function definitions, then camera and
 * body - is the exported C's own order and the one shape the loader accepts;
 * see the phase machine in src/repl/doc_order.c. Symmetric with
 * src/repl/example_loader.c, which reads exactly this: the @cfg header, then
 * every line offered to the shared camera reader. Round-tripping a scene
 * through save -> catalog entry -> load is the point of the format, so the
 * two files move together.
 *
 * Only the @cfg rows the loader would actually honour are emitted (the
 * bridge's scene subset), and only where they differ from the default -
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
 * unconditionally - better a redundant row than a silently dropped one. */
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

/* The tagged camera transform rows, in the hook-less projection (no
 * `@camera spin`: that row is exported C's animation hook and never appears
 * in a .glr). No camera bridge installed - the demo, tests - leaves the block
 * empty, and the scene simply inherits the live camera on load, which is the
 * documented no-header behaviour.
 *
 * No heading of its own: `// camera` is an ordinary comment now, so whatever
 * marker the author wrote is a document row and is re-emitted by the document
 * loop below. Writing one here too would append a second marker on every
 * save - three on the next - and filtering the row back out would resurrect
 * the marker matcher this format deleted. The rows are self-describing,
 * which is the whole argument for tagging them. */
static void glr_scene_write_camera(FILE *f) {
    repl_refresh_camera_lines();
    if (!g_cam_lines[0][0])
        return;
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        if (g_cam_lines[i][0])
            glr_scene_write_line(f, g_cam_lines[i]);
}

/* Which of the canonical document phases a row belongs to. The .glr format
 * admits exactly one order - declarations, then function definitions, then
 * camera and body - and it is the exported C's own order, so the two halves
 * of one format cannot disagree about line placement. The loader rejects a
 * file that violates it, so the writer must not produce one: the document
 * emits in three passes rather than verbatim.
 *
 * A comment or blank row carries no phase and would be legal anywhere, but a
 * *writer* has to put it somewhere - it stays with the row it precedes, which
 * is what keeps a declaration's or a function's explanatory comment attached
 * to it. */
typedef enum {
    GLR_ROW_PHASE_DECLS = 0,
    GLR_ROW_PHASE_FUNCS,
    GLR_ROW_PHASE_BODY,
    GLR_ROW_PHASE_COUNT
} GlrRowPhase;

static GlrRowPhase glr_row_phase(const GLCmd *cmd, int in_func_body) {
    if (in_func_body || cmd->type == CMD_FUNC_DEF || cmd->type == CMD_FUNC_END)
        return GLR_ROW_PHASE_FUNCS;
    if (cmd->type == CMD_VAR_DECLARE)
        return GLR_ROW_PHASE_DECLS;
    return GLR_ROW_PHASE_BODY;
}

/* Emit every document row whose phase is `want`, carrying each row's leading
 * run of comments and blank lines with it. */
static void glr_scene_write_phase(FILE *f, GlrRowPhase want) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int pending_start = -1;   /* first row of the current comment/blank run */
    int func_depth = 0;

    for (int cmd_idx = 0; cmd_idx < count; cmd_idx++) {
        GlrRowPhase phase;
        int was_in_func;

        if (!cmds[cmd_idx].valid)
            continue;
        if (cmds[cmd_idx].type == CMD_COMMENT ||
            cmds[cmd_idx].type == CMD_EMPTY) {
            if (pending_start < 0)
                pending_start = cmd_idx;
            continue;
        }

        was_in_func = func_depth > 0;
        if (cmds[cmd_idx].type == CMD_FUNC_DEF)
            func_depth++;
        else if (cmds[cmd_idx].type == CMD_FUNC_END && func_depth > 0)
            func_depth--;

        phase = glr_row_phase(&cmds[cmd_idx], was_in_func);
        if (phase == want) {
            for (int run = pending_start; run >= 0 && run < cmd_idx; run++)
                if (cmds[run].valid)
                    glr_scene_write_line(f, export_document_text(run));
            glr_scene_write_line(f, export_document_text(cmd_idx));
        }
        pending_start = -1;
    }

    /* A trailing comment/blank run belongs to no row, so it lands at the end
     * of the body - the only phase a reader will accept it after. */
    if (want == GLR_ROW_PHASE_BODY && pending_start >= 0) {
        for (int run = pending_start; run < count; run++)
            if (cmds[run].valid)
                glr_scene_write_line(f, export_document_text(run));
    }
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
    glr_scene_write_phase(f, GLR_ROW_PHASE_DECLS);
    glr_scene_write_phase(f, GLR_ROW_PHASE_FUNCS);
    glr_scene_write_camera(f);
    glr_scene_write_phase(f, GLR_ROW_PHASE_BODY);

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
