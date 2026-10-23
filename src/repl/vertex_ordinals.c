/*
 * src/repl/vertex_ordinals.c - Vertex numbering of a flat command stream.
 * See vertex_ordinals.h.
 */
#include "repl/vertex_ordinals.h"

void repl_flat_number_vertices(const GLCmd *cmds, int count,
                               ReplVertexOrdinal *out) {
    int run = 0;              /* display run of the innermost open head */
    int imm = 0;              /* glVertex ordinal in the current glBegin */
    int tess = 0;             /* gluVertex ordinal in the current polygon */
    int tess_depth = 0;
    int in_begin_block = 0;
    int polygon_run = 0;      /* the polygon's run while a nested strip owns it */

    if (!cmds || !out)
        return;

    for (int i = 0; i < count; i++) {
        const GLCmd *cmd = &cmds[i];

        out[i].run = out[i].imm = out[i].tess = REPL_VERTEX_ORDINAL_NONE;
        if (!cmd->valid)
            continue;

        switch (cmd->type) {
        case CMD_BEGIN:
            /* The first strip opened inside a polygon takes the run over;
             * the polygon's index is kept for its glEnd(). */
            if (tess_depth > 0 && !in_begin_block)
                polygon_run = run;
            in_begin_block = 1;
            run = imm = 0;
            break;
        case CMD_END:
            /* glEnd() closes only the strip: the enclosing polygon resumes. */
            if (tess_depth > 0 && in_begin_block)
                run = polygon_run;
            in_begin_block = 0;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            run = imm = tess = 0;
            polygon_run = 0;
            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            break;
        case CMD_TESS_END: {
            int old_depth = tess_depth;

            tess_depth = repl_tess_advance_depth(cmd->type, tess_depth);
            if (old_depth == 1 && tess_depth == 0)
                polygon_run = 0;
            break;
        }
        case CMD_VERTEX2F:
        case CMD_VERTEX3F:
        case CMD_VERTEX4F:
            out[i].run = run++;
            out[i].imm = imm++;
            break;
        case CMD_TESS_VERTEX:
            out[i].run = run++;
            out[i].tess = tess++;
            break;
        default:
            break;
        }
    }
}
