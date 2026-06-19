#ifndef HIDDEN_LINES_H
#define HIDDEN_LINES_H

#include "repl/flatten.h"
#include "scene/render_types.h"
#include "source_document.h"

typedef struct {
    FlatProgramView program;
    int             flat_cmd_count;
    SourceTextView  text;
    char           *status_out;
    int             status_out_sz;
} HiddenLinesRenderContext;

void hidden_lines_init_resources(void);
void hidden_lines_destroy_resources(void);
void hidden_lines_execute(const HiddenLinesRenderContext *ctx,
                          SceneExecutePurpose purpose);

#endif /* HIDDEN_LINES_H */
