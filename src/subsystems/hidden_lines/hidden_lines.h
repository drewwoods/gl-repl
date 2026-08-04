#ifndef HIDDEN_LINES_H
#define HIDDEN_LINES_H

#include "repl/executor.h"   /* ReplBackgroundObservation */
#include "repl/flatten.h"
#include "render3d/render_types.h"

typedef struct {
    FlatProgramView program;
    int             flat_cmd_count;
    /* Forwarded to the cursor this subsystem drives, same contract as
     * ReplExecutionOptions: NULL publishes nothing. Only the depth-fill pass
     * emits a clear, so only that purpose can produce a known background -
     * but the caller still passes NULL explicitly for the other two. */
    ReplBackgroundObservation *observation_out;
    float                      baseline_clear_rgba[4];
} HiddenLinesRenderContext;

void hidden_lines_init_resources(void);
void hidden_lines_destroy_resources(void);
void hidden_lines_execute(const HiddenLinesRenderContext *ctx,
                          Render3dExecutePurpose purpose);

#endif /* HIDDEN_LINES_H */
