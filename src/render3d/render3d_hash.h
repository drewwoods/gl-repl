/*
 * render3d_hash.h - Shared deterministic value hash for procedural scene
 * decoration.
 *
 * The GLSL-classic fract(sin(dot)*k) hash. Renderers use it to place
 * decoration that must be stable frame to frame without carrying any
 * state: grid.c freezes crack joints, frost heave and star-chart nodes in
 * place with it; axes.c scatters the Fountain theme's droplets with it.
 * Callers pass whatever coordinates identify the thing being decorated
 * (a grid line and joint index, an axis and droplet index), so the value
 * is a pure function of identity and the look never swims.
 *
 * Keep sin's argument small and take the fract of the big multiply
 * afterwards. Feeding sin a pre-multiplied argument instead (~1e8 rad and
 * up) reduces to noise no libm agrees on: one ULP at that magnitude is
 * tens of radians, so the pattern stops matching between the native and
 * emscripten builds.
 *
 * Deliberately NOT shared with the language evaluator's `rand`/`rand2`,
 * which is the same formula plus a 0.5 seed offset:
 *   - this renderer helper must stay independent of evaluator state, so
 *     it cannot include the evaluator;
 *   - the evaluator's body is pinned character-for-character to the repl_randf
 *     helper that export_prologue.c writes into exported C, and that
 *     parity contract shouldn't be entangled with renderer decoration.
 * The seed offset is also why the constants differ: it was tuned for the
 * evaluator's distribution (see `randdist` in tests/test_eval.c), while
 * this one must stay bit-stable to keep existing grid themes rendering
 * exactly as they do today.
 */
#ifndef RENDER3D_HASH_H
#define RENDER3D_HASH_H

#include <math.h>

/* Deterministic hash of (a, b) in [0, 1). */
static inline float render3d_hash01(float a, float b) {
    float s = sinf(a * 12.9898f + b * 78.233f) * 43758.5453f;
    return s - floorf(s);
}

#endif /* RENDER3D_HASH_H */
