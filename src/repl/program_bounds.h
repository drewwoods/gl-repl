/*
 * program_bounds.h - World-space bounding box of a flat program's geometry.
 *
 * Answers "how big is the user's scene, and where is it?" for consumers that
 * have to place something around it - today the drone backdrop's flight
 * paths, light attenuation and spotlight aim.
 *
 * This is a pure CPU walk over the flat program: it tracks the modelview in
 * software (repl/transform_utils.h's Mat4Stack) and transforms each emitted
 * vertex, plus an analytic local box per GLUT solid. The obvious alternative
 * - a glRenderMode(GL_FEEDBACK) capture like src/app/glr_mesh_export.c -
 * is deliberately not used per frame: it re-executes the whole program a
 * second time and ends in a synchronous readback, and it could not run
 * before user geometry anyway (backdrop lights are configured in the pass
 * setup phase). The walk here is O(cmd_count) with no GL calls at all, so it
 * is cheap enough to run every frame and can run before the geometry pass.
 *
 * The box is an over-estimate in two known ways, both harmless for placing
 * things around a scene: a GLUT solid contributes the AABB of its *rotated*
 * local box, and the teapot contributes its Bezier control hull rather than
 * the surface.
 */
#ifndef REPL_PROGRAM_BOUNDS_H
#define REPL_PROGRAM_BOUNDS_H

#include "repl/flatten.h"

typedef struct ReplSceneBounds {
    /* 0 when the program emitted no geometry at all, or when the modelview
     * walk overflowed its stack and the numbers cannot be trusted. min/max
     * are zeroed then - consumers must check this before reading them. */
    int   valid;
    float min[3];
    float max[3];
} ReplSceneBounds;

/* Walk `program`'s first `cmd_count` commands and return their world-space
 * bounds. `cmd_count` is clamped to the view's own count; pass the full
 * count for the whole program. */
ReplSceneBounds repl_program_bounds(FlatProgramView program, int cmd_count);

/* Centre of `b`, or the origin when it is not valid. */
void repl_scene_bounds_center(const ReplSceneBounds *b, float out[3]);

/* Radius of the sphere containing `b` (half its diagonal), or 0 when it is
 * not valid. Callers wanting a usable scale for an empty scene should
 * substitute their own fallback rather than relying on the 0. */
float repl_scene_bounds_radius(const ReplSceneBounds *b);

#endif /* REPL_PROGRAM_BOUNDS_H */
