/*
 * call_depth_viz.h - Colour the scene by funcN call depth.
 *
 * The flat program already records, per command, the funcN nesting depth it
 * was expanded at (`GLCmd.call_depth`, 0 = top level). That number is
 * invisible in the rendered frame: a recursive scene looks like one pile of
 * geometry, and nothing on screen says which triangles came from the outer
 * call and which from four frames down. This maps depth to colour so the
 * recursion has a spatial shape.
 *
 * Same split as the buffer_viz peers: a pure, GL-free core (scan + ramp,
 * unit-tested on synthetic command arrays) with no rendering of its own.
 * Unlike those, there is no capture and no GL shell here at all - the
 * colours are handed to the executor as a table and emitted inline with the
 * geometry, so this module never touches GL.
 *
 *   call_depth_viz_scan()          (here: counts + observed max depth)
 *     -> call_depth_viz_build_table()  (here: depth -> RGB)
 *       -> ReplExecutionOptions.depth_tint_*  (executor: glColor per depth)
 *       -> the legend panel               (controller: swatches + counts)
 *
 * Conventions:
 *
 *   - **One ramp, always - never a categorical palette.** A stencil value
 *     is a tag: value 7 has no "more than" relationship to value 3, which
 *     is why buffer_viz_stencil's PALETTE mode is a fixed lookup table and
 *     view-independence is worth having. Call depth is the opposite - the
 *     whole question the overlay answers is *which of these is deeper* -
 *     and a categorical palette answers it with magenta-versus-yellow,
 *     which is no answer. The ramp keeps the ordering visible.
 *
 *   - **The ramp is normalized over the observed max depth**, not over
 *     MAX_FLATTEN_CALL_DEPTH. A scene that nests three deep gets four
 *     well-separated stops instead of four adjacent samples from the low
 *     end of a 64-stop ramp; a scene that nests twenty deep gets a smooth
 *     gradient. Contrast adapts to what is actually on screen, which is the
 *     same reason the stencil RAMP mode normalizes over its own min/max.
 *
 *   - **Cool = shallow, warm = deep**, matching the stencil ramp's
 *     convention so the two overlays do not read in opposite directions.
 *     Depth 0 is azure, the deepest observed depth is coral.
 *
 *   - **The max is recomputed per frame, not smoothed.** Depth is an
 *     integer and a change in it is information - a recursion that grew a
 *     level deeper *should* recolour, because it did something different.
 *     The stencil RAMP's EMA exists to stop continuous per-pixel extremes
 *     from jittering the range; there is no such jitter here.
 *
 * Counts are flat commands, not pixels: that is the unit of the 8192 flat
 * budget, so the legend's numbers answer "which invocation level is eating
 * the program" as well as "which colour is which depth".
 */
#ifndef CALL_DEPTH_VIZ_H
#define CALL_DEPTH_VIZ_H

#include "repl/command.h"   /* GLCmd */
#include "repl/flatten.h"   /* MAX_FLATTEN_CALL_DEPTH */

/* Depth slots the scan bins into: every depth flatten can produce, plus the
 * top level. A command carrying a depth outside this range cannot exist -
 * flatten refuses the call past max_call_depth - but the scan clamps rather
 * than trusting that, since it reads an array it did not build. */
enum { CALL_DEPTH_VIZ_SLOTS = MAX_FLATTEN_CALL_DEPTH + 1 };

typedef struct CallDepthVizStats {
    unsigned int counts[CALL_DEPTH_VIZ_SLOTS];  /* flat commands per depth */
    int          max_depth;    /* deepest depth with at least one command */
    int          distinct;     /* how many depths are occupied */
    unsigned int total;        /* commands scanned (valid ones only) */
    int          valid;        /* 0 when nothing was scanned */
} CallDepthVizStats;

/* Bin `count` flat commands by call_depth. Invalid commands are skipped -
 * they emit nothing, so counting them would inflate a depth the frame never
 * drew. `out` is fully overwritten (zeroed first); a NULL/empty program
 * yields valid = 0. */
void call_depth_viz_scan(const GLCmd *cmds, int count,
                         CallDepthVizStats *out);

/* The ramp colour for `depth` when the deepest observed depth is
 * `max_depth`. Writes 3 floats in [0,1]. `depth` is clamped into
 * [0, max_depth]; a max_depth of 0 (or less) puts everything at the cool
 * end rather than dividing by zero - a program with no funcN calls is one
 * flat colour, and the legend's single row is what says so. */
void call_depth_viz_ramp_rgb(int depth, int max_depth, float rgb_out[3]);

/* Fill `table[0..max_depth]` with the ramp, for a caller that hands the
 * whole thing to the executor once per pass instead of computing a colour
 * per command. Writes min(max_depth + 1, cap) rows and returns that count;
 * rows beyond it are untouched, and the executor clamps to the returned
 * count. */
int call_depth_viz_build_table(int max_depth, float (*table)[3], int cap);

#endif /* CALL_DEPTH_VIZ_H */
