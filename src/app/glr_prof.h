/*
 * glr_prof.h - gl-repl's profile-section policy surface.
 *
 * glr_prof.c owns two per-section tables: the display table behind the
 * generic prof_section_info() (declared in support/cpuprof.h), and the
 * GPU-bracketing table behind the accessors here — which prof sections
 * get GL_TIME_ELAPSED timer queries issued around them via the cpuprof
 * section hooks (see prof_install_section_hooks). Sections that emit no
 * GL (flatten, snapshot, frame restore) stay CPU-only; so do the
 * per-fade-batch sub-sections, whose one-segment-per-batch-per-accum-pass
 * shape would swamp the per-frame query budget for little signal.
 */
#ifndef GLR_PROF_H
#define GLR_PROF_H

#include "support/cpuprof.h"   /* ProfSection */

/* Nonzero when section s is in the GPU-bracketed subset. */
int glr_prof_section_is_gpu(ProfSection s);

/* How much of the GPU subset actually gets queried this frame. Every query
 * boundary has a real cost on some stacks (GL-on-Metal can split command
 * encoders at it), and on tile-deferred GPUs more segments also means more
 * overlap inflation (see gpuprof.h) — so the controller dials capture to
 * what the panel can display: Off/FPS = no queries at all, while Sections
 * and Histogram capture the full subset shown by the collapsible tree.
 * Set once per frame, before gpu_prof_frame_begin, so a frame is captured
 * at a single granularity. Default is ALL. */
typedef enum {
    GLR_PROF_GPU_CAPTURE_OFF = 0,    /* issue no timer queries */
    GLR_PROF_GPU_CAPTURE_ALL         /* the full GPU subset, children included */
} GlrProfGpuCaptureMode;

void glr_prof_set_gpu_capture_mode(GlrProfGpuCaptureMode mode);

/* Install the cpuprof begin/end hooks that route the GPU subset into
 * gpu_prof_begin/gpu_prof_end. Call after gpu_prof_init() succeeds
 * (the hooks no-op while gpu_prof is disabled, but there is no reason
 * to pay the indirection without it). */
void glr_prof_install_gpu_section_hooks(void);

#endif /* GLR_PROF_H */
