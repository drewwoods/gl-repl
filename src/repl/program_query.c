/*
 * src/repl/program_query.c - Read-only queries over source and flat programs.
 */

#include "source_document.h"
#include "repl/program_query.h"

#include "repl/command_spec.h"
#include "repl/eval.h"
#include "repl/flatten.h"
#include "repl/state_views.h"

const char *repl_mode_name(GLenum mode) {
    return repl_begin_mode_name(mode);
}

GLenum repl_current_begin_mode(void) {
    GLenum mode = GL_TRIANGLES;
    const GLCmd *document_cmds = repl_state_document_cmds();
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++)
        if (document_cmds[cmd_idx].valid && document_cmds[cmd_idx].type == CMD_BEGIN)
            mode = (GLenum)document_cmds[cmd_idx].args[0];
    return mode;
}

int repl_count_vertices(void) {
    int n = 0;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;

    for (int flat_idx = 0; flat_idx < g_num_flat_cmds; flat_idx++)
        if (g_flat_cmds[flat_idx].valid &&
            repl_cmd_emits_vertex(g_flat_cmds[flat_idx].type)) n++;
    return n;
}

int repl_collect_tuned_vars(const GLCmd *cmds, int count, SourceTextView text,
                            const char **out, int max, int *total_out) {
    int written = 0;
    int total = 0;
    if (!cmds || count < 0)
        count = 0;
    for (int i = 0; i < count; i++) {
        if (cmds[i].type != CMD_VAR_DECLARE)
            continue;
        if (!repl_eval_line_has_tune_tag(source_text_line(text, i)))
            continue;
        for (int n = 0; n < cmds[i].payload.decl.count; n++) {
            total++;
            if (out && written < max)
                out[written++] = cmds[i].payload.decl.names[n];
        }
    }
    if (total_out)
        *total_out = total;
    return written;
}
