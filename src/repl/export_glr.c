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
 *     display() {                   (required in every scene)
 *     glTranslatef(...);  // @camera dist
 *     glRotatef(...);     // @camera rx
 *     glRotatef(...);     // @camera ry
 *     glTranslatef(...);  // @camera pan
 *     <the body>
 *     }
 *
 * That order - declarations, then function definitions, then camera and
 * body inside display - is the exported C's own order and the one shape the
 * loader accepts;
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

/* Body rows carry display()'s 2-space base indent in memory; declarations
 * and user functions do not. A .glr scene is authored at column 0 even when
 * the .glr writer always writes the explicit display wrapper. The loader
 * re-derives every indent level from block structure as it feeds lines back in,
 * so shedding the base indent here is presentation-only and round-trips
 * exactly. */
#define REPL_GLR_BASE_INDENT 2

/* `strip` shears the display() body's base indent off a line. It is NOT
 * unconditional: only rows that render inside display() carry that base.
 * Declarations and function definitions are file-scope and already sit at
 * column 0 in the document, so stripping them again would eat a real
 * nesting level - a function body would flatten onto its header. The
 * generated camera block is not a document row at all; its lines are
 * built with two spaces and always strip. */
static void glr_scene_write_line(FILE *f, const char *line, int strip) {
    int skip = 0;

    if (!line) {
        fprintf(f, "\n");
        return;
    }
    if (strip) {
        while (skip < REPL_GLR_BASE_INDENT && line[skip] == ' ')
            skip++;
    }
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
static int glr_scene_write_camera(FILE *f, int leading_blank) {
    repl_refresh_camera_lines();
    if (!IMPORT_EXPORT_VIEW.cam_lines[0][0])
        return 0;
    if (leading_blank)
        fprintf(f, "\n");
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        if (IMPORT_EXPORT_VIEW.cam_lines[i][0])
            glr_scene_write_line(f, IMPORT_EXPORT_VIEW.cam_lines[i], 1);
    return 1;
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

/* Camera rows never enter the editor document, so blank rows authored on
 * either side of them become one leading run on the first body row. Return
 * the size of that run so the writer can split it back around the camera. */
static int glr_scene_body_boundary_blanks(void) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int pending_start = -1;
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
        if (phase == GLR_ROW_PHASE_BODY) {
            int blanks = 0;
            for (int run = pending_start; run >= 0 && run < cmd_idx; run++)
                if (cmds[run].valid && cmds[run].type == CMD_EMPTY)
                    blanks++;
            return blanks;
        }
        pending_start = -1;
    }
    return 0;
}

/* Emit every document row whose phase is `want`, carrying each row's leading
 * run of comments and blank lines with it. */
static void glr_scene_write_phase(FILE *f, GlrRowPhase want,
                                  int skip_leading_blanks) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int pending_start = -1;   /* first row of the current comment/blank run */
    int func_depth = 0;
    /* Only body rows carry the display() base indent - see
     * glr_scene_write_line. */
    int strip = (want == GLR_ROW_PHASE_BODY);

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
            for (int run = pending_start; run >= 0 && run < cmd_idx; run++) {
                if (!cmds[run].valid)
                    continue;
                if (skip_leading_blanks > 0 &&
                    cmds[run].type == CMD_EMPTY) {
                    skip_leading_blanks--;
                    continue;
                }
                glr_scene_write_line(f, export_document_text(run), strip);
            }
            glr_scene_write_line(f, export_document_text(cmd_idx), strip);
            skip_leading_blanks = 0;
        }
        pending_start = -1;
    }

    /* A trailing comment/blank run belongs to no row, so it lands at the end
     * of the body - the only phase a reader will accept it after. */
    if (want == GLR_ROW_PHASE_BODY && pending_start >= 0) {
        for (int run = pending_start; run < count; run++)
            if (cmds[run].valid)
                glr_scene_write_line(f, export_document_text(run), strip);
    }
}

int repl_export_save_glr(const char *filename, SourceTextView text) {
    char msg[REPL_DIAG_TEXT_MAX];
    FILE *f;
    int had_error;
    int close_failed;
    int boundary_blanks;
    int split_blanks;

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
    boundary_blanks = glr_scene_body_boundary_blanks();
    split_blanks = boundary_blanks >= 2 ? 2 : 0;
    glr_scene_write_cfg_overrides(f);
    glr_scene_write_phase(f, GLR_ROW_PHASE_DECLS, 0);
    glr_scene_write_phase(f, GLR_ROW_PHASE_FUNCS, 0);
    fprintf(f, "display() {\n");
    if (glr_scene_write_camera(f, split_blanks > 0)) {
        /* Camera rows are consumed by the loader instead of entering the
         * document, so the blank rows on either side become adjacent there.
         * Split the first two back around the generated camera block; any
         * additional authored spacing remains with the body. */
        if (split_blanks > 0)
            fprintf(f, "\n");
        glr_scene_write_phase(f, GLR_ROW_PHASE_BODY, split_blanks);
    } else {
        glr_scene_write_phase(f, GLR_ROW_PHASE_BODY, 0);
    }
    fprintf(f, "}\n");

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
