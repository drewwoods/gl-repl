#ifndef GLR_HIDDEN_LINES_H
#define GLR_HIDDEN_LINES_H

#include "repl/flatten.h"
#include "scene/render_types.h"
#include "source_document.h"

typedef struct {
    FlatProgramView program;
    int             flat_cmd_count;
    SourceTextView  text;
    char           *status_out;
    int             status_out_sz;
} GlrHiddenLinesRenderContext;

void glr_hidden_lines_init_resources(void);
void glr_hidden_lines_destroy_resources(void);
void glr_hidden_lines_execute(const GlrHiddenLinesRenderContext *ctx,
                              SceneExecutePurpose purpose);

#endif /* GLR_HIDDEN_LINES_H */
