#include "subsystems/replay/replay_render.h"
#include "gl_includes.h"
#include "repl/state_views.h"
#include "repl/executor.h"
#include "repl/eval.h"
#include "repl/core.h"
#include "subsystems/replay/replay.h"
#include "support/prof.h"
#include "scene/palette.h"
#include "source_document.h"
#include <string.h>

static void tess_preview_begin_contour(void *ud) { (void)ud; glBegin(GL_LINE_STRIP); }
static void tess_preview_vertex(float x, float y, float z, void *ud) {
    (void)ud;
    glVertex3f(x, y, z);
}
static void tess_preview_end_contour(void *ud) { (void)ud; glEnd(); }

static const ReplayTessPreviewCallbacks g_tess_preview_cb = {
    .begin_contour = tess_preview_begin_contour,
    .vertex        = tess_preview_vertex,
    .end_contour   = tess_preview_end_contour,
};

static void replay_render_restore_baseline(const ReplayFadePlan *fade_plan) {
    if (!fade_plan) return;
    repl_restore_predef_values(fade_plan->baseline_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(fade_plan->baseline_scratch_arrays);
}

void replay_render_fade_batches(const ReplayFadePlan *plan) {
    int batch_count = plan->batch_count;
    if (batch_count <= 0) return;

    prof_begin(PROF_SCENE_3D_FADE);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    static const GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    static const GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    FlatProgramView program = repl_state_flat_program_view();
    SourceTextView text = source_document_view();

    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &plan->batches[batch_idx];
        float alpha = plan->batch_alpha[batch_idx];
        if (alpha <= 0.0f) continue;

        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
        replay_render_restore_baseline(plan);
        scene_clr_a(SCENE_CLR_REPLAY_FADE, alpha);
        glPushMatrix();
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_PREP);

        prof_begin(PROF_SCENE_3D_FADE_BATCH_EXEC);
        repl_execute_set_fade_context(alpha, plan->skip_limits[batch_idx]);
        repl_execute_program(&(ReplExecutionOptions){
            .flat_cmd_count = batch->new_pc,
            .program        = program,
            .text           = text,
        });
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_EXEC);

        glPopMatrix();
    }

    prof_begin(PROF_SCENE_3D_FADE_BATCH_POST);
    repl_execute_set_fade_context(1.0f, 0);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_FADE_BATCH_POST);

    prof_accum_end(PROF_SCENE_3D_FADE);
}

void replay_render_tess_preview(const ReplayFadePlan *plan) {
    (void)plan;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    scene_clr_a(SCENE_CLR_TESS_PREVIEW, 0.80f);
    glLineWidth(2.0f);

    replay_walk_tess_preview(&g_tess_preview_cb, NULL);

    glLineWidth(1.0f);
    glPopAttrib();
}

void replay_render_post_fill(void *user_data) {
    const ReplayFadePlan *plan = (const ReplayFadePlan *)user_data;
    if (!plan) return;
    if (plan->active)
        replay_render_fade_batches(plan);
    if (plan->tess_preview_active)
        replay_render_tess_preview(plan);
}
