#include "repl/tutorials.h"
#include "repl/catalog_tags.h"
#include "repl/eval.h"   /* repl_eval_is_reserved_ident for REQUIRE_VAR validation */
#include "gl_includes.h"
#include "keymap.h"
#include "keys.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "c_compat.h"   /* STATIC_ASSERT for the tag-label table */

#define STEP_APPEND(step_label, c, e) \
    { .label = (step_label), .comment = (c), .expected = (e), \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_COMMAND, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Comment-less append COMMAND step: commits `e` with no locked
 * instruction row above it - the autocomplete ghost and status hint
 * still teach the command. Use for runs of related commands where a
 * narration comment per line would just be noise. */
#define STEP_CMD(step_label, e) \
    { .label = (step_label), .comment = NULL, .expected = (e), \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_COMMAND, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Comment-only step: reveal the instruction comment, wait for an ack
 * key (Enter/Tab/Space), advance. SET's showcase flow without the cfg
 * write - narration between commands with no GL call to type. */
#define STEP_NOTE(c) \
    { .label = NULL, .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_NOTE, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

#define STEP_AT(step_label, c, e, target) \
    { .label = (step_label), .comment = (c), .expected = (e), \
      .placement = TUTORIAL_STEP_LABEL, .target_label = (target), \
      .kind = TUTORIAL_STEP_KIND_COMMAND, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Showcase step: on entry apply cfg_slug=cfg_value so the user sees the
 * effect, show a "press Enter to continue" prompt, advance on ack key.
 * `expected` is NULL - there is no command to type. */
#define STEP_SET(step_label, c, slug, val) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_SET, .cfg_slug = (slug), \
      .cfg_value = (val), .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Symbolic-value variant of STEP_SET. The runner passes `val_name`
 * (e.g. "GRID_THEME_RADAR") through the controller-installed bridge's
 * resolve_text at apply time so the catalog reads the enum constant
 * rather than encoding a magic number. `cfg_value` stays 0 here as
 * the back-compat fallback if a caller writes the resolved integer back. */
#define STEP_SET_SYM(step_label, c, slug, val_name) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_SET, .cfg_slug = (slug), \
      .cfg_value = 0, .cfg_value_name = (val_name), .var_name = NULL, \
      .var_target = 0.0f }

/* Staging step: apply cfg_slug=val and advance immediately - no
 * instruction row, no ack key, no pause. Comment-less on purpose (the
 * validator rejects a comment here), so a run of them stacks without an
 * Enter apiece:
 *
 *     STEP_SET_QUIET("vertex_points", 0),
 *     STEP_SET_QUIET_SYM("overlay_scope", "OVERLAY_SCOPE_WHOLE_SCENE"),
 *     STEP_NOTE("// Overlays are staged; here is what they show."),
 *
 * `cfg_slug` doubles as the discriminator that keeps these out of the
 * step-array sentinel (repl_tutorial_step_is_sentinel). Reach for
 * TutorialEntry.cfg instead when the settings can be applied at
 * tutorial start - this is for staging that must happen mid-lesson. */
#define STEP_SET_QUIET(slug, val) \
    { .label = NULL, .comment = NULL, .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_SET_QUIET, .cfg_slug = (slug), \
      .cfg_value = (val), .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Symbolic-value variant of STEP_SET_QUIET - see STEP_SET_SYM. */
#define STEP_SET_QUIET_SYM(slug, val_name) \
    { .label = NULL, .comment = NULL, .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_SET_QUIET, .cfg_slug = (slug), \
      .cfg_value = 0, .cfg_value_name = (val_name), .var_name = NULL, \
      .var_target = 0.0f }

/* Check step: advance when the user themselves makes cfg_slug == cfg_value
 * (via F-key/menu/etc.). Auto-advances if already satisfied on entry.
 * `expected` is NULL. */
#define STEP_REQUIRE(step_label, c, slug, val) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_REQUIRE, .cfg_slug = (slug), \
      .cfg_value = (val), .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

#define STEP_REQUIRE_KEY(step_label, c, slug, val, key_code, mods, is_special) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_REQUIRE, .cfg_slug = (slug), \
      .cfg_value = (val), .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f, .comment_binding_key = (key_code), \
      .comment_binding_mods = (mods), .comment_binding_is_special = (is_special) }

/* Symbolic-value variant of STEP_REQUIRE. The runner resolves
 * `val_name` to int via the bridge's resolve_text at compare time. */
#define STEP_REQUIRE_SYM(step_label, c, slug, val_name) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_REQUIRE, .cfg_slug = (slug), \
      .cfg_value = 0, .cfg_value_name = (val_name), .var_name = NULL, \
      .var_target = 0.0f }

/* Predef-var check step: advance when the named predefined variable's
 * live value matches `target` within TUTORIAL_VAR_EPS. Either a typed
 * `name = expr;` commit or a variable-panel slider drag satisfies it.
 * Auto-advances if already satisfied on entry. `expected` is NULL. */
#define STEP_REQUIRE_VAR(step_label, c, var, target) \
    { .label = (step_label), .comment = (c), .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_REQUIRE_VAR, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = (var), \
      .var_target = (target) }

/* Sentinel: comment, expected AND cfg_slug all NULL
 * (repl_tutorial_step_is_sentinel). Every real step carries at least one of
 * the three - SET/REQUIRE/NOTE have NULL `expected` but a comment;
 * comment-less COMMAND steps have NULL `comment` but an expected; SET_QUIET
 * has neither but always a cfg_slug. */
#define STEP_SENTINEL \
    { .label = NULL, .comment = NULL, .expected = NULL, \
      .placement = TUTORIAL_STEP_APPEND, .target_label = NULL, \
      .kind = TUTORIAL_STEP_KIND_COMMAND, .cfg_slug = NULL, \
      .cfg_value = 0, .cfg_value_name = NULL, .var_name = NULL, \
      .var_target = 0.0f }

/* Staging conventions -------------------------------------------------------
 * Every tutorial is framed by the same camera: tutorial_baseline_apply resets
 * the pose and re-aims it at CFG_DEFAULT_TUTORIAL_CAMERA_DIST (8) with the
 * built-in orbit (rx 20, ry 30). At that distance the 45-degree vertical FOV
 * shows roughly +/-3.3 world units of height about the origin, in 2D and 3D
 * view alike, so the catalog stages geometry to one shared scale:
 *
 *   - Flat figures (triangles, quads, line loops) live on the +/-2 motif:
 *     ~4 units tall, about 60% of the frame height.
 *   - A solid presented on its own is ~2 units across, matching
 *     glutSolidSphere(1, ...). The other GLUT solids do NOT agree at their
 *     nominal size-1 arguments - measured extents are cube(1) 1 unit,
 *     torus(1, 2) 6 units, teapot(1) 3.2 units, cone(1, 2) 2 units but
 *     growing along +Z from its base rather than centered. Match them
 *     explicitly: cube(2), torus(0.3, 0.7), teapot(0.7), and stand the cone
 *     up with glRotatef(-90, 1, 0, 0). In a composition (a ring, a plot) the
 *     pitch sets the size instead.
 *   - The shared pose is yawed 30 degrees, which foreshortens anything spread
 *     along X: equal shapes render unequal and equal pitch reads uneven. A
 *     tutorial that asks the learner to COMPARE shapes side by side overrides
 *     the yaw to 0 through a `// camera` block in its setup scaffold.
 *
 * Deviations are what the reader will read as a bug in the lesson, so keep new
 * entries on the scale above unless the step text explains the difference. */

/* Tutorial chrome turns vertex points AND vertex outlines on
 * (CFG_DEFAULT_TUTORIAL_VERTEX_*), which is the right default while a learner
 * is placing individual vertices: on a triangle or a cube the overlays name
 * exactly what was typed. On a high-tessellation GLUT solid they do the
 * opposite - a 48x32 sphere is ~1600 vertices and a size-1 teapot ~1600, so
 * every surface turns into speckle with its silhouette buried. Any entry whose
 * geometry is a dense solid (sphere/torus/teapot at these resolutions) shares
 * this block; entries that stay on cubes and flat figures keep the overlays. */
static const char *const g_tutorial_dense_solid_cfg[] = {
    "// @cfg vertex_points = 0",
    "// @cfg vertex_outlines = 0",
    NULL,
};

/* Same problem, different answer, for the two REPL-language tutorials that draw
 * solids with no lighting scaffold (Functions, Scratch Arrays). There the
 * shading gradient is not available as a shape cue, so dropping the outlines
 * too leaves a flat white disc that reads as a 2D sprite. Instead those entries
 * drop the vertex points and lower their sphere tessellation to 8x6, where the
 * outline pass reads as a low-poly globe rather than a scribble. */
static const char *const g_tutorial_unlit_solid_cfg[] = {
    "// @cfg vertex_points = 0",
    NULL,
};

static const TutorialStep g_tutorial_first_triangle_steps[] = {
    STEP_APPEND(NULL,
        "// Build the smallest filled shape: open a GL_TRIANGLES batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Place the first vertex near the top; this becomes the triangle tip.",
        "glVertex2f(0, 2)"),
    STEP_APPEND(NULL,
        "// Add the lower-left corner so the triangle has width.",
        "glVertex2f(-2, -2)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner; three vertices complete one triangle.",
        "glVertex2f(2, -2)"),
    STEP_APPEND(NULL,
        "// Close the batch and the filled triangle appears in the scene.",
        "glEnd()"),
    STEP_SENTINEL,
};

/* The square is 2 units, not the flat-figure +/-2 motif: it is the one flat
 * tutorial whose figure gets translated, and at 4 units wide a 2-unit shift
 * barely separated it from the origin - it just read as an oversized quad
 * hanging off to the right. Half the size makes the displacement legible. */
static const TutorialStep g_tutorial_color_transform_steps[] = {
    STEP_APPEND(NULL,
        "// Save the current matrix so this example can clean up after itself.",
        "glPushMatrix()"),
    STEP_APPEND(NULL,
        "// Set the drawing color; OpenGL keeps using it for later vertices.",
        "glColor3f(0.2, 0.8, 1)"),
    STEP_APPEND(NULL,
        "// Move the local coordinate system to the right before drawing.",
        "glTranslatef(2, 0, 0)"),
    STEP_APPEND(NULL,
        "// Rotate those local axes 30 degrees around Z, the screen-facing axis.",
        "glRotatef(30, 0, 0, 1)"),
    STEP_APPEND(NULL,
        "// Open a GL_QUADS batch; the next four corners make one filled square.",
        "glBegin(GL_QUADS)"),
    STEP_APPEND(NULL,
        "// Give the square its lower-left corner in the transformed space.",
        "glVertex3f(-1, -1, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner at the same height.",
        "glVertex3f(1, -1, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-right corner so the quad has height.",
        "glVertex3f(1, 1, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-left corner; vertex order walks around the square.",
        "glVertex3f(-1, 1, 0)"),
    STEP_APPEND(NULL,
        "// Close the quad batch to draw the rotated cyan square.",
        "glEnd()"),
    STEP_APPEND(NULL,
        "// Restore the saved matrix so future commands are not moved or rotated.",
        "glPopMatrix()"),
    STEP_SENTINEL,
};

/* This label-targeted tutorial first has the user draw a
 * triangle (five append steps), then a sixth step splices
 * glEnable(GL_DEPTH_TEST) above the original glBegin so the
 * batch renders with depth testing already enabled. The label
 * "triangle_begin" anchors the insertion above the original glBegin. */
static const TutorialStep g_tutorial_depth_triangle_steps[] = {
    STEP_APPEND("triangle_begin",
        "// Start the triangle batch; the label here anchors a later insert.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Add the top vertex.",
        "glVertex3f(0, 2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-left vertex.",
        "glVertex3f(-2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right vertex.",
        "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Close the triangle batch.",
        "glEnd()"),
    STEP_AT(NULL,
        "// Enable depth testing before the triangle is submitted.",
        "glEnable(GL_DEPTH_TEST)",
        "triangle_begin"),
    STEP_SENTINEL,
};

/* A flat triangle in the z=0 plane: present it in true 2D so a
 * first-time user sees the shape head-on without perspective
 * foreshortening or accidental orbit. */
static const char *const g_tutorial_first_triangle_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    "// @cfg vertex_labels = OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE",
    "// @cfg grid_brightness = GRID_BRIGHTNESS_DIM",
    "// @cfg grid = GRID_THEME_PLANES",
    NULL,
};

/* "Feature Tour" - exercises the non-COMMAND step kinds and the
 * relaxed step shapes:
 *   1) A NOTE step opens the tour (comment-only; ack key advances).
 *   2) Five COMMAND steps draw a triangle in 3D. The two lower-corner
 *      vertex steps are comment-less (STEP_CMD) - the autocomplete
 *      ghost and status hint carry the instruction, demonstrating
 *      that a narration comment per command is not required.
 *   3) One REQUIRE step asks the user to enable vertex outlines (using
 *      the GLR_VERTEX_OUTLINES binding) so they see how the feature
 *      changes the rendering.
 *   4) Two SET steps showcase Radar and Aurora grid themes;
 *      the user presses Enter/Tab/Space to advance through them.
 *
 * The entry-level `@cfg` block guarantees a known baseline (3D view,
 * grid off, vertex outlines off) so the REQUIRE step has the intended
 * teaching effect rather than auto-advancing immediately. The SET
 * steps use symbolic cfg values so enum reordering in src/render3d/themes.h
 * does not silently retarget the showcase. */
static const TutorialStep g_tutorial_feature_tour_steps[] = {
    STEP_NOTE(
        "// A quick tour of the REPL's scene features."),
    STEP_APPEND(NULL,
        "// Open a triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Add the three corners, starting at the top.",
        "glVertex3f(0, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(-2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch - the filled triangle appears.",
        "glEnd()"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to turn on vertex outlines; they trace each edge.",
        "vertex_outlines", 1,
        KM_KEY(GLR_VERTEX_OUTLINES), KM_MODS(GLR_VERTEX_OUTLINES), 0),
    STEP_SET_SYM(NULL,
        "// The Radar grid backdrop looks like this.",
        "grid", "GRID_THEME_RADAR"),
    STEP_SET_SYM(NULL,
        "// And the Aurora grid backdrop looks like this.",
        "grid", "GRID_THEME_AURORA"),
    STEP_SENTINEL,
};

static const char *const g_tutorial_feature_tour_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_3D",  /* depth gives the grid themes context */
    "// @cfg vertex_outlines = 0",  /* baseline: REQUIRE will ask the user to turn this on */
    "// @cfg grid = GRID_THEME_OFF", /* baseline: SET steps will showcase Radar then Aurora */
    NULL,
};

/* "Variable Slider" - teaches the REQUIRE_VAR step kind by driving a
 * variable that controls a drawn shape. The user declares `n` (the
 * triangle's size), draws a triangle whose vertices use `n`, then grows
 * `n` to 10 with the variable-panel slider and watches the triangle
 * scale. Both a typed commit and a slider drag flow through
 * repl_apply_predef_ops, which the editor commit path notifies after.
 * No `@cfg` block - presentation defaults are fine.
 *
 * Step 0 is a DECLARATION step: `n` does not exist when the tutorial
 * starts, so the runner detects the undeclared var (see
 * tutorial_enter_step) and treats it specially. The satisfying
 * `float n = 1;` is a declaration, which the compiler relocates to the
 * TOP of the document, so a separate locked instruction comment line
 * above it would be stranded (and would desync locked-line tracking).
 * Instead the instruction rides the autocomplete ghost as
 * `float n = 1; <this comment>` (synthesized in tutorial_shadow_suffix):
 * the comment below commits as a TRAILING comment on the decl line and
 * travels with it to the top. So this catalog string is worded to read
 * as a trailing description of `n`, not as a standalone instruction.
 *
 * The middle COMMAND steps draw the triangle (its vertices reference
 * `n`). The final step's `n` already exists, so its slider is live and
 * the instruction is an ordinary locked comment; a slider drag or a
 * typed `n = 10;` advances it. */
static const TutorialStep g_tutorial_variable_slider_steps[] = {
    STEP_REQUIRE_VAR(NULL,
        "// @config the triangle's size; the slider will grow it",
        "n", 1.0f),
    STEP_APPEND(NULL,
        "// Open a triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Top vertex.",
        "glVertex3f(0, n, 0)"),
    STEP_APPEND(NULL,
        "// Lower-left vertex.",
        "glVertex3f(-n, -1, 0)"),
    STEP_APPEND(NULL,
        "// Lower-right vertex.",
        "glVertex3f(n, -1, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch - the filled triangle appears.",
        "glEnd()"),
    /* 2.5, not 10: the triangle's apex is at y = n, and the shared tutorial
     * camera only shows about +/-3.3 units of height, so a target of 10 ended
     * the lesson on a white slab filling the whole scene rect. Reachable by
     * drag either way - the slider scrubs a flat 0.1 units per pixel from the
     * declared value, so 2.5 is 15 px of travel. */
    STEP_REQUIRE_VAR(NULL,
        "// Now drag the n slider in the variable panel to bring n to 2.5.",
        "n", 2.5f),
    STEP_SENTINEL,
};

/* "Lighting Basics" - the first lit-surface tutorial. Six COMMAND steps
 * stand up the minimal lighting pipeline (depth test, lighting, one
 * light, color-material) and then draw a GLUT sphere so the shading
 * gradient across a curved surface is obvious. No entry `@cfg` is needed:
 * tutorials reset presentation to defaults on start, and the default
 * view is 3D, so the sphere's falloff reads correctly. Each expected
 * command is a single call with no braces or declarations, satisfying
 * the COMMAND placement rule. */
static const TutorialStep g_tutorial_lighting_basics_steps[] = {
    STEP_APPEND(NULL,
        "// Turn on depth testing so nearer surfaces hide the ones behind them.",
        "glEnable(GL_DEPTH_TEST)"),
    STEP_APPEND(NULL,
        "// Enable lighting; OpenGL now shades surfaces instead of using flat color.",
        "glEnable(GL_LIGHTING)"),
    STEP_APPEND(NULL,
        "// Switch on light 0, the default light positioned near the camera.",
        "glEnable(GL_LIGHT0)"),
    STEP_APPEND(NULL,
        "// Let glColor drive the surface material so the next color tints the shape.",
        "glEnable(GL_COLOR_MATERIAL)"),
    STEP_APPEND(NULL,
        "// Choose a warm amber material color for the sphere.",
        "glColor3f(0.9, 0.6, 0.2)"),
    STEP_APPEND(NULL,
        "// Draw a lit sphere; watch the shading fall off from the lit side to the dark side.",
        "glutSolidSphere(1, 32, 24)"),
    STEP_SENTINEL,
};

/* "First Animation" - introduces the built-in `t` clock without asking the
 * learner to declare or edit it. The entry cfg pauses Auto time before the
 * first step, so the cube is visibly frozen until the REQUIRE step asks the
 * learner to press Ctrl+T. The closing NOTE leaves the motion on-screen
 * before teardown. `t` is reserved (rather than a user predef variable), so
 * this deliberately watches the app-level auto_time configuration instead of
 * using REQUIRE_VAR. */
static const TutorialStep g_tutorial_first_animation_steps[] = {
    STEP_NOTE(
        "// The built-in t value is a clock in seconds; use it inside a number to animate that number."),
    STEP_APPEND(NULL,
        "// Rotate later geometry 45 degrees per second around the vertical Y axis.",
        "glRotatef(t * 45, 0, 1, 0)"),
    STEP_APPEND(NULL,
        "// Draw a cube. It stays frozen until you start the t clock.",
        "glutSolidCube(2)"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to start the clock and watch the cube spin.",
        "auto_time", 1,
        KM_KEY(GLR_AUTO_TIME), KM_MODS(GLR_AUTO_TIME), 0),
    STEP_NOTE(
        "// t now advances every frame. Watch a turn, then press Enter to finish."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_first_animation_cfg[] = {
    "// @cfg auto_time = 0",  /* Ctrl+T has a visible start-the-motion payoff. */
    NULL,
};

/* "Color Interpolation" - the first tutorial that BUILDS ON prior work
 * via a preloaded `setup` scaffold instead of making the user re-type
 * it. The scaffold
 * is the flat triangle from "First Triangle" drawn in one solid color,
 * loaded locked before step 0; its leading `// @cfg` header uses the
 * example metadata vocabulary. The `:left` / `:right` goto-label rows
 * are the anchors: the two label-targeted steps splice per-vertex
 * glColor3f calls above them, so the user only types what's new and
 * watches the gradient appear. */
static const char *const g_tutorial_color_interp_setup[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    "// A triangle drawn in a single warm red.",
    "glBegin(GL_TRIANGLES)",
    "glColor3f(1, 0.3, 0.2)",
    "glVertex3f(0, 2, 0)",
    ":left",
    "glVertex3f(-2, -2, 0)",
    ":right",
    "glVertex3f(2, -2, 0)",
    "glEnd()",
    NULL,
};

static const TutorialStep g_tutorial_color_interp_steps[] = {
    STEP_NOTE(
        "// Every vertex uses the same color. Give each corner its own."),
    STEP_AT(NULL,
        "// Set green before the lower-left vertex; OpenGL blends between vertex colors.",
        "glColor3f(0.2, 1, 0.3)",
        "left"),
    STEP_AT(NULL,
        "// Set blue before the lower-right vertex to complete the gradient.",
        "glColor3f(0.2, 0.3, 1)",
        "right"),
    STEP_SENTINEL,
};

/* Additional catalog entries -----------------------------------------------
 * These tutorials deliberately mix narrated STEP_APPEND rows with compact
 * STEP_CMD runs. The former explain a new idea; the latter let the ghost text
 * carry repetitive geometry so the resulting source stays readable. */

static const char *const g_tutorial_points_lines_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    NULL,
};

static const TutorialStep g_tutorial_points_lines_steps[] = {
    /* 12, not 8: the GL_LINE_STRIP built in the second half of this tutorial
     * runs through the SAME three vertices as the point batch, and a 3-unit
     * line width swallowed 8-pixel points almost entirely - the first half's
     * result vanished as the second half was typed. At 12 the dots still read
     * at each joint of the finished strip. */
    STEP_APPEND(NULL,
        "// Make individual points large enough to read clearly.",
        "glPointSize(12)"),
    STEP_APPEND(NULL,
        "// Open a GL_POINTS batch; every vertex becomes one dot.",
        "glBegin(GL_POINTS)"),
    STEP_APPEND(NULL,
        "// Place the first point on the left.",
        "glVertex3f(-2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(0, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Close the point batch.",
        "glEnd()"),
    STEP_APPEND(NULL,
        "// Give the line that follows a bold, visible stroke.",
        "glLineWidth(3)"),
    STEP_APPEND(NULL,
        "// GL_LINE_STRIP connects each new vertex to the previous one.",
        "glBegin(GL_LINE_STRIP)"),
    STEP_CMD(NULL, "glVertex3f(-2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(0, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Close the strip to reveal the connected path.",
        "glEnd()"),
    STEP_NOTE(
        "// GL_LINES draws separate vertex pairs; GL_LINE_STRIP keeps one connected path."),
    STEP_SENTINEL,
};

/* The lineup is a side-by-side comparison, so the camera drops the shared
 * pose's 30-degree yaw: with any yaw the row recedes from the viewer and the
 * far solids render smaller on an even pitch. Straight-on, a 3-unit pitch is
 * a 3-unit gap for every pair; the 16-degree pitch angle still shows each
 * solid's top face, so the cube reads as a box rather than a square. Distance
 * 11 keeps the +/-7 row inside the frame with room to spare at the default
 * window size. */
static const char *const g_tutorial_glut_solids_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -11.00f);",
    "glRotatef(16.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "// Lighting scaffold shared by every solid in this tour.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    "glEnable(GL_COLOR_MATERIAL)",
    "glColor3f(0.8, 0.7, 0.3)",
    NULL,
};

/* Every argument here is chosen so the five solids come out the same 2 units
 * across on a 3-unit pitch - see the measured extents in the staging comment
 * at the top of this file. The nominal "size 1" call for each shape does NOT
 * agree: cube(1) is half the sphere's width and torus(1, 2) is three times it.
 * The cone is the one shape the learner has to reorient, because it grows
 * along +Z from its base instead of about its center; that step earns its
 * place by teaching the asymmetry rather than hiding it. */
static const TutorialStep g_tutorial_glut_solids_steps[] = {
    STEP_APPEND(NULL,
        "// Move to the left end of the row, then place a cube there.",
        "glTranslatef(-6, 0, 0)"),
    STEP_CMD(NULL, "glutSolidCube(2)"),
    STEP_APPEND(NULL,
        "// Step right and compare the cube with a smooth sphere.",
        "glTranslatef(3, 0, 0)"),
    STEP_CMD(NULL, "glutSolidSphere(1, 16, 12)"),
    STEP_NOTE(
        "// A radius-1 sphere is 2 units wide, so the cube needs size 2 to match."),
    STEP_APPEND(NULL,
        "// Move again; a torus uses inner and outer radii plus two resolutions.",
        "glTranslatef(3, 0, 0)"),
    STEP_CMD(NULL, "glutSolidTorus(0.3, 0.7, 16, 24)"),
    STEP_APPEND(NULL,
        "// Add freeglut's classic teapot beside the torus.",
        "glTranslatef(3, 0, 0)"),
    STEP_CMD(NULL, "glutSolidTeapot(0.7)"),
    STEP_APPEND(NULL,
        "// Step right once more, and down, to seat the last solid on the row.",
        "glTranslatef(3, -1, 0)"),
    STEP_APPEND(NULL,
        "// A cone grows along +Z from its base, so stand this one upright.",
        "glRotatef(-90, 1, 0, 0)"),
    STEP_CMD(NULL, "glutSolidCone(1, 2, 24, 8)"),
    STEP_SENTINEL,
};

static const TutorialStep g_tutorial_first_loop_steps[] = {
    STEP_NOTE(
        "// A for block repeats its body while i walks through a numeric range."),
    STEP_APPEND(NULL,
        "// Repeat the body eight times with i taking values 0 through 7.",
        "for(i, 0, 8) {"),
    STEP_APPEND(NULL,
        "// Give each iteration its own transform stack.",
        "glPushMatrix()"),
    STEP_CMD(NULL, "glRotatef(i * 45, 0, 0, 1)"),
    STEP_CMD(NULL, "glTranslatef(2, 0, 0)"),
    STEP_CMD(NULL, "glutSolidCube(1)"),
    STEP_CMD(NULL, "glPopMatrix()"),
    STEP_APPEND(NULL,
        "// Close the loop; the body now produces a ring of eight cubes.",
        "}"),
    STEP_NOTE(
        "// Loop expressions can also use sin(i), cos(i), and the other math built-ins."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_line_stipple_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    NULL,
};

static const TutorialStep g_tutorial_line_stipple_steps[] = {
    STEP_APPEND(NULL,
        "// Enable patterned line rasterization.",
        "glEnable(GL_LINE_STIPPLE)"),
    STEP_APPEND(NULL,
        "// Thicken the line so the pattern is easy to see.",
        "glLineWidth(2)"),
    STEP_APPEND(NULL,
        "// Repeat each pattern bit once; 255 supplies the 16-bit mask.",
        "glLineStipple(1, 255)"),
    STEP_APPEND(NULL,
        "// Draw a closed outline whose edges all use the stipple mask.",
        "glBegin(GL_LINE_LOOP)"),
    STEP_CMD(NULL, "glVertex3f(-2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(-2, 2, 0)"),
    STEP_APPEND(NULL,
        "// Close the loop and inspect the repeated dash pattern.",
        "glEnd()"),
    STEP_NOTE(
        "// The mask is 16 bits: 255 reads as dashes, while 43690 produces dots."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_blending_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    NULL,
};

static const TutorialStep g_tutorial_blending_steps[] = {
    STEP_APPEND(NULL,
        "// Enable blending so alpha can mix new fragments with the image behind.",
        "glEnable(GL_BLEND)"),
    STEP_APPEND(NULL,
        "// Use the standard source-alpha transparency equation.",
        "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)"),
    STEP_APPEND(NULL,
        "// Choose a half-transparent warm red for the first quad.",
        "glColor4f(1, 0.3, 0.2, 0.5)"),
    STEP_APPEND(NULL,
        "// Draw the first translucent square.",
        "glBegin(GL_QUADS)"),
    STEP_CMD(NULL, "glVertex3f(-3, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(1, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(1, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(-3, 2, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_APPEND(NULL,
        "// Switch to half-transparent blue for an overlapping quad.",
        "glColor4f(0.2, 0.4, 1, 0.5)"),
    STEP_CMD(NULL, "glBegin(GL_QUADS)"),
    STEP_CMD(NULL, "glVertex3f(-1, -1, 0)"),
    STEP_CMD(NULL, "glVertex3f(3, -1, 0)"),
    STEP_CMD(NULL, "glVertex3f(3, 3, 0)"),
    STEP_CMD(NULL, "glVertex3f(-1, 3, 0)"),
    STEP_APPEND(NULL,
        "// Close the second quad and inspect the mixed overlap.",
        "glEnd()"),
    STEP_NOTE(
        "// glBlendFunc chooses how source and destination colors contribute to each pixel."),
    STEP_SENTINEL,
};

static const TutorialStep g_tutorial_depth_mask_steps[] = {
    STEP_NOTE(
        "// Draw opaque geometry first, then translucent geometry after it."),
    STEP_APPEND(NULL,
        "// Keep ordinary depth testing active for both objects.",
        "glEnable(GL_DEPTH_TEST)"),
    STEP_APPEND(NULL,
        "// Draw the opaque cube first so it establishes solid depth.",
        "glutSolidCube(2)"),
    STEP_CMD(NULL, "glEnable(GL_BLEND)"),
    STEP_CMD(NULL, "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)"),
    STEP_APPEND(NULL,
        "// Stop translucent fragments from writing new depth values.",
        "glDepthMask(GL_FALSE)"),
    STEP_CMD(NULL, "glColor4f(0.4, 0.8, 1, 0.4)"),
    STEP_APPEND(NULL,
        "// Draw a larger translucent sphere around the opaque cube.",
        "glutSolidSphere(1.4, 32, 24)"),
    STEP_APPEND(NULL,
        "// Restore depth writes for whatever is drawn next.",
        "glDepthMask(GL_TRUE)"),
    STEP_NOTE(
        "// Translucent objects usually render last, with depth testing on and depth writes off."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_fog_cfg[] = {
    "// @cfg backdrop = RENDER3D_BACKDROP_STARS",
    /* Eight dense toruses - see g_tutorial_dense_solid_cfg. */
    "// @cfg vertex_points = 0",
    "// @cfg vertex_outlines = 0",
    NULL,
};

/* Fog strength is measured from the EYE, so the camera distance and the fog
 * density are one setting: at the old 16 units with density 0.1, EXP2 left even
 * the NEAREST ring ~8% visible, so the whole tunnel came out uniformly fogged
 * and the closing NOTE described a gradient nobody could see. From 11 units at
 * density 0.05 the near ring stays ~74% visible and the deepest ~9%. The yaw
 * stays oblique on purpose - this is the one tutorial whose geometry recedes
 * along Z, and looking straight down the tunnel stacks the rings concentrically
 * instead of showing them as a receding row. */
static const char *const g_tutorial_fog_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -11.00f);",
    "glRotatef(10.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(22.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "// Depth testing so the near rings occlude the ones behind them.",
    "glEnable(GL_DEPTH_TEST)",
    "// A locked row of toruses receding away from the camera.",
    ":draw",
    "for(i, 0, 8) {",
    "  glPushMatrix()",
    "  glTranslatef(0, 0, -i * 3)",
    "  glutSolidTorus(0.5, 1.5, 16, 24)",
    "  glPopMatrix()",
    "}",
    NULL,
};

static const TutorialStep g_tutorial_fog_steps[] = {
    STEP_AT(NULL,
        "// Enable fixed-function fog before the drawing scaffold.",
        "glEnable(GL_FOG)", "draw"),
    STEP_AT(NULL,
        "// EXP2 fog grows smoothly with the square of eye-space distance.",
        "glFogi(GL_FOG_MODE, GL_EXP2)", "draw"),
    STEP_AT(NULL,
        "// Set how quickly distant geometry disappears into the fog.",
        "glFogf(GL_FOG_DENSITY, 0.05)", "draw"),
    STEP_AT(NULL,
        "// Match the fog color to the dark star-field backdrop.",
        "glFogfv(GL_FOG_COLOR, 0.05, 0.08, 0.12, 1)", "draw"),
    STEP_NOTE(
        "// The nearest torus stays clear while each deeper torus fades farther into the scene."),
    STEP_SENTINEL,
};

/* d MUST stay 0 here: the sphere below has radius 1, so the plane y + 1 = 0
 * this tutorial used to define is exactly tangent to its south pole and clips
 * nothing at all - the lesson rendered as an untouched sphere. y = 0 cuts
 * through the center, which is what the step text promises. */
static const TutorialStep g_tutorial_clip_planes_steps[] = {
    STEP_APPEND(NULL,
        "// Define plane 0 as y = 0; the half-space above it is kept.",
        "glClipPlane(GL_CLIP_PLANE0, 0, 1, 0, 0)"),
    STEP_APPEND(NULL,
        "// Enable that plane so it clips every later primitive.",
        "glEnable(GL_CLIP_PLANE0)"),
    STEP_APPEND(NULL,
        "// Draw a sphere and watch the plane slice away its lower half.",
        "glutSolidSphere(1, 48, 32)"),
    STEP_NOTE(
        "// The kept side satisfies ax + by + cz + d >= 0; editing the plane shows its guide disc."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_materials_setup[] = {
    "// Minimal lighting scaffold for explicit material properties.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    NULL,
};

static const TutorialStep g_tutorial_materials_steps[] = {
    STEP_APPEND(NULL,
        "// Set the front face's diffuse response to a warm red.",
        "glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1)"),
    STEP_APPEND(NULL,
        "// Give highlights a bright white specular color.",
        "glMaterialfv(GL_FRONT, GL_SPECULAR, 1, 1, 1, 1)"),
    STEP_APPEND(NULL,
        "// A higher shininess exponent makes the highlight tighter.",
        "glMaterialf(GL_FRONT, GL_SHININESS, 40)"),
    STEP_APPEND(NULL,
        "// Draw a dense sphere so its specular highlight reads smoothly.",
        "glutSolidSphere(1, 48, 32)"),
    STEP_NOTE(
        "// glColorMaterial is convenient; glMaterial gives diffuse and specular independent control."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_shade_model_setup[] = {
    "// Lighting scaffold for the shade-model comparison.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    "glEnable(GL_COLOR_MATERIAL)",
    "glColor3f(0.75, 0.78, 0.85)",
    NULL,
};

/* A flat quad cannot show the difference between GL_FLAT and GL_SMOOTH - the
 * two agree exactly on a single-normal polygon, which is why the old combined
 * tutorial demonstrated nothing. The comparison needs curvature the shading
 * can either hide or expose, so this draws a COARSE sphere (16x12: few enough
 * facets that flat shading reads as an obvious faceted hull, dense enough that
 * smooth shading reads as round). The lesson is the before/after on the SAME
 * geometry: the sphere is drawn first under the default GL_SMOOTH, then a
 * label-placement step splices glShadeModel(GL_FLAT) in above it. */
static const TutorialStep g_tutorial_shade_model_steps[] = {
    STEP_APPEND("sphere_draw",
        "// Draw a coarse sphere - few enough facets to count, under the default GL_SMOOTH.",
        "glutSolidSphere(1.4, 16, 12)"),
    STEP_NOTE(
        "// GL_SMOOTH interpolates the per-vertex lighting across each facet, so the seams disappear."),
    STEP_AT(NULL,
        "// Now go back above the sphere and switch the shade model to flat.",
        "glShadeModel(GL_FLAT)", "sphere_draw"),
    STEP_NOTE(
        "// One lighting result per facet: identical geometry, but the hull's facets are now visible."),
    STEP_NOTE(
        "// GL_FLAT takes that result from the polygon's provoking vertex - shading, not tessellation, changed."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_normals_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -9.00f);",
    "glRotatef(25.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "// Lighting scaffold for the surface-normal demonstration.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    "glEnable(GL_COLOR_MATERIAL)",
    "glColor3f(0.75, 0.78, 0.85)",
    NULL,
};

/* Normals alone, no shade model in sight. The point a single lit quad cannot
 * make is that a normal is SUPPLIED data rather than something derived from
 * the vertices, so the lesson draws two geometrically identical quads side by
 * side and gives them different normals. GL_LIGHT0's default position is the
 * directional (0, 0, 1, 0), so the +Z quad lights fully and the tilted one
 * ends up at roughly 0.6 of that - visibly dimmer without going black.
 *
 * The overlay scope MUST be widened to WHOLE_SCENE before the normal-vector
 * overlay goes on. edit_overlays' normal-arrow pass honors cursor_overlay_scope
 * like every other overlay, so under the CFG_DEFAULT_OVERLAY_SCOPE
 * (LAST_INSTANCE) it would arrow only the cursor's own block - and the whole
 * lesson is the SIDE-BY-SIDE comparison of two blocks' normals. Turning the
 * overlay on is a REQUIRE_KEY rather than a SET so the learner presses the
 * binding themselves; the comment interpolates it from keymap.h via
 * KM_KEY/KM_MODS instead of spelling out a key that a rebind would falsify.
 *
 * The camera's PITCH is 25 and its YAW is deliberately 0. Head-on, both normals
 * point at the eye and their overlay arrows foreshorten to dots; pitching tilts
 * them into legible arrows. Yaw would too - and would also break the lesson.
 * GL_LIGHT0's default position is never re-specified here, so it stays fixed in
 * EYE space while the camera rotates the geometry under it: at yaw 35 the
 * tilted quad's eye-space n.z reaches 0.95 against the flat quad's 0.82 and the
 * dim quad becomes the BRIGHT one. Pitch is safe only because both normals have
 * n.y = 0, so a rotation about X scales both dots by cos(pitch) and leaves
 * their ratio at 0.6. Change either normal's Y and this camera stops being
 * neutral. */
static const TutorialStep g_tutorial_normals_steps[] = {
    STEP_APPEND(NULL,
        "// Open a quad batch for the first lit surface.",
        "glBegin(GL_QUADS)"),
    STEP_APPEND(NULL,
        "// Point this face's normal straight at the light, toward +Z.",
        "glNormal3f(0, 0, 1)"),
    STEP_CMD(NULL, "glVertex3f(-3.2, -1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(-0.2, -1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(-0.2, 1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(-3.2, 1.5, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_NOTE(
        "// A normal is per-vertex state: every vertex after it uses it until the next glNormal3f."),
    STEP_APPEND(NULL,
        "// Start a second quad, the same size and in the same plane as the first.",
        "glBegin(GL_QUADS)"),
    STEP_APPEND(NULL,
        "// Tilt only its normal away from the light - the geometry stays flat-on.",
        "glNormal3f(-0.8, 0, 0.6)"),
    STEP_CMD(NULL, "glVertex3f(0.2, -1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(3.2, -1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(3.2, 1.5, 0)"),
    STEP_CMD(NULL, "glVertex3f(0.2, 1.5, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_SET_QUIET_SYM("vertex_labels", "OVERLAY_VERTEX_LABEL_OFF"),
    STEP_SET_QUIET_SYM("overlay_scope", "OVERLAY_SCOPE_WHOLE_SCENE"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to draw the normal-vector overlay for every face in the scene.",
        "normal_vectors", 1,
        KM_KEY(GLR_NORMAL_VECTORS), KM_MODS(GLR_NORMAL_VECTORS), 0),
    STEP_NOTE(
        "// Same geometry, different normals, different brightness - lighting reads the normal, not the vertices."),
    STEP_NOTE(
        "// Omit glNormal3f entirely and the Auto-normals setting synthesizes a face normal for you."),
    STEP_SENTINEL,
};

/* Camera-only scaffold: the whole lesson is "compare these two triangles", and
 * the shared pose's 30-degree yaw pushed the right-hand one further from the
 * eye, so two identical triangles rendered visibly different sizes. Yaw 0
 * makes the pair symmetric on screen; nothing is preloaded into the document. */
static const char *const g_tutorial_culling_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -9.00f);",
    "glRotatef(20.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    NULL,
};

static const TutorialStep g_tutorial_culling_steps[] = {
    STEP_APPEND(NULL,
        "// Enable face culling so back-facing polygons are discarded.",
        "glEnable(GL_CULL_FACE)"),
    STEP_CMD(NULL, "glCullFace(GL_BACK)"),
    STEP_APPEND(NULL,
        "// Declare counter-clockwise vertices to be the front-facing order.",
        "glFrontFace(GL_CCW)"),
    STEP_APPEND(NULL,
        "// Draw a counter-clockwise triangle on the left.",
        "glBegin(GL_TRIANGLES)"),
    STEP_CMD(NULL, "glVertex3f(-4, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(0, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(-2, 2, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_APPEND(NULL,
        "// Draw the right triangle clockwise; back-face culling hides it.",
        "glBegin(GL_TRIANGLES)"),
    STEP_CMD(NULL, "glVertex3f(0, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(4, -2, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to show the winding-direction overlay.",
        "winding", 1,
        KM_KEY(GLR_WINDING_VIEW), KM_MODS(GLR_WINDING_VIEW), 0),
    STEP_NOTE(
        "// With winding enabled the green shaded faces are front facing and the red are back."),
    STEP_NOTE(
        "// Reverse the vertex order or glFrontFace mode to decide which side survives culling."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_bitmap_text_cfg[] = {
    "// @cfg auto_time = 1",
    NULL,
};

/* The anchor sits 1.5 units up so the text clears the size-2 cube's top face
 * (y = 1) by half a unit and still lands well inside the frame; at y = 2 the
 * label rendered against the top edge of the scene rect, detached from the
 * cube it annotates. */
static const TutorialStep g_tutorial_bitmap_text_steps[] = {
    STEP_APPEND(NULL,
        "// Draw a cube to give the label a visible scene reference.",
        "glutSolidCube(2)"),
    STEP_APPEND(NULL,
        "// Set the 3D raster anchor just above the cube.",
        "glRasterPos3f(0, 1.5, 0)"),
    STEP_APPEND(NULL,
        "// Format the live t clock into bitmap text at that raster position.",
        "label(\"t %f\", t)"),
    STEP_NOTE(
        "// label formats numbers with %f; its format text cannot contain parentheses, commas, or backslashes."),
    STEP_SENTINEL,
};

/* No camera-only scaffold here, unlike the other comparison tutorials: a
 * func-open step and a `setup` array are mutually exclusive (the validator
 * rejects the pair - func relocation would desync locked rows). So the three
 * spokes keep the shared yawed pose and render at slightly unequal sizes; the
 * ring radius stays at 2 so the spread stays small enough for that to read as
 * perspective rather than as three different spheres. */
static const TutorialStep g_tutorial_functions_steps[] = {
    STEP_NOTE(
        "// A named function packages reusable drawing commands and receives numeric arguments."),
    STEP_APPEND(NULL,
        "// Define spoke with one argument named a; function definitions come before scene calls.",
        "spoke(a) {"),
    STEP_APPEND(NULL,
        "// Isolate every spoke's local transforms.",
        "glPushMatrix()"),
    STEP_CMD(NULL, "glRotatef(a, 0, 0, 1)"),
    STEP_CMD(NULL, "glTranslatef(2, 0, 0)"),
    STEP_CMD(NULL, "glutSolidSphere(1, 8, 6)"),
    STEP_CMD(NULL, "glPopMatrix()"),
    STEP_APPEND(NULL,
        "// Close the function body.",
        "}"),
    STEP_APPEND(NULL,
        "// Call spoke at zero degrees.",
        "spoke(0)"),
    STEP_CMD(NULL, "spoke(120)"),
    STEP_CMD(NULL, "spoke(240)"),
    STEP_NOTE(
        "// Named calls compile to the REPL's funcN slots while the source keeps the readable name."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_conditionals_cfg[] = {
    "// @cfg auto_time = 1",
    NULL,
};

static const TutorialStep g_tutorial_conditionals_steps[] = {
    STEP_NOTE(
        "// An if block can select drawing state from a live expression such as sin(t)."),
    STEP_APPEND(NULL,
        "// Use green while the sine wave is positive.",
        "if(sin(t) > 0) {"),
    STEP_CMD(NULL, "glColor3f(0.2, 1, 0.4)"),
    STEP_APPEND(NULL,
        "// Otherwise switch to a warm red until the condition becomes true again.",
        "} else {"),
    STEP_CMD(NULL, "glColor3f(1, 0.3, 0.2)"),
    STEP_APPEND(NULL,
        "// Close the conditional; its selected color remains current.",
        "}"),
    STEP_APPEND(NULL,
        "// Draw one cube whose color changes as sin(t) crosses zero.",
        "glutSolidCube(2)"),
    STEP_SENTINEL,
};

/* Camera-only scaffold (see g_tutorial_culling_setup): the three spheres are a
 * plot of A[0..2], so their sizes have to read as equal and their 3-unit pitch
 * as even - exactly what the shared pose's yaw destroys. */
static const char *const g_tutorial_scratch_arrays_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -10.00f);",
    "glRotatef(18.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    NULL,
};

static const TutorialStep g_tutorial_scratch_arrays_steps[] = {
    STEP_NOTE(
        "// Scratch arrays A, B, and C provide temporary numeric storage without declarations."),
    STEP_APPEND(NULL,
        "// Store three heights in the first slots of A.",
        "A[0] = 0"),
    STEP_CMD(NULL, "A[1] = 2"),
    STEP_CMD(NULL, "A[2] = 1"),
    STEP_APPEND(NULL,
        "// Loop over the three stored heights.",
        "for(i, 0, 3) {"),
    STEP_CMD(NULL, "glPushMatrix()"),
    STEP_CMD(NULL, "glTranslatef((i - 1) * 3, A[i], 0)"),
    STEP_CMD(NULL, "glutSolidSphere(1, 8, 6)"),
    STEP_CMD(NULL, "glPopMatrix()"),
    STEP_APPEND(NULL,
        "// Close the loop to reveal one sphere for each array element.",
        "}"),
    STEP_NOTE(
        "// Arrays pair naturally with loops; rand(i) can generate a repeatable value for each index."),
    STEP_SENTINEL,
};

/* REPL_TUTORIAL_TAG_ALL is a synthetic tag: every tutorial is a member.
 * It is not listed in any g_tutorials[] mask literal; instead
 * repl_tutorial_tag_mask() ORs its bit into every entry's mask, so the
 * whole tag query API picks it up with no per-entry bookkeeping. Kept at
 * index 0 so "All" sorts first in the Tutorials menu. Enum lives in
 * tutorials.h. */

#define TUTORIAL_TAG_BIT(tag)         (1u << (tag))
#define TUTORIAL_TAG_ALL              TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_ALL)
#define TUTORIAL_TAG_GEOMETRY         TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_GEOMETRY)
#define TUTORIAL_TAG_COLOR_TRANSFORMS TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_COLOR_TRANSFORMS)
#define TUTORIAL_TAG_DEPTH_LIGHTING   TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_DEPTH_LIGHTING)
#define TUTORIAL_TAG_ANIMATION        TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_ANIMATION)
#define TUTORIAL_TAG_REPL_LANGUAGE    TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_REPL_LANGUAGE)
#define TUTORIAL_TAG_EFFECTS          TUTORIAL_TAG_BIT(REPL_TUTORIAL_TAG_EFFECTS)

static const char *const g_tutorial_tag_labels[] = {
    "All",
    "Geometry",
    "Color & Transforms",
    "Depth & Lighting",
    "Animation",
    "REPL Language",
    "Effects",
};
/* Must stay 1:1 with the REPL_TUTORIAL_TAG_* enum: repl_tutorial_tag_count()
 * returns REPL_TUTORIAL_TAG_COUNT but repl_tutorial_tag_label() indexes this
 * table, so any drift is an out-of-bounds read. */
STATIC_ASSERT((int)(sizeof(g_tutorial_tag_labels) /
                    sizeof(g_tutorial_tag_labels[0])) == REPL_TUTORIAL_TAG_COUNT,
              "g_tutorial_tag_labels[] out of sync with REPL_TUTORIAL_TAG_COUNT");

/* Subheading labels here are catalog-author choices, not a fixed
 * vocabulary - the menu just emits `### subheading` chrome rows when
 * the subheading changes. The current catalog uses Beginner,
 * Intermediate, and Advanced; catalogs can use any labels that
 * group sensibly within each tag flyout.
 *
 * Catalog order matters: entries sharing a subheading should be
 * contiguous within every tag they share, so the per-tag flyout walker
 * emits each header exactly once. test_catalog_subheading_metadata
 * enforces this. Global order is one Beginner run, one Intermediate run,
 * then one Advanced run, which also keeps every filtered tag view free of
 * duplicate section headers. */

static const TutorialEntry g_tutorials[] = {
    {
        .name       = "First Triangle",
        .steps      = g_tutorial_first_triangle_steps,
        .cfg        = g_tutorial_first_triangle_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Beginner",
    },
    {
        .name       = "Color & Transform",
        .steps      = g_tutorial_color_transform_steps,
        .tags       = TUTORIAL_TAG_COLOR_TRANSFORMS,
        .subheading = "Beginner",
    },
    {
        .name       = "Feature Tour",
        .steps      = g_tutorial_feature_tour_steps,
        .cfg        = g_tutorial_feature_tour_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Beginner",
    },
    {
        /* Placed before "Depth Test Triangle" (Intermediate) so the ALL
         * flyout walks Beginner-only entries first, then transitions
         * once to Intermediate at the tail - the subheading contiguity
         * test (test_catalog_subheading_metadata) requires a single
         * Beginner->Intermediate transition across the whole catalog. */
        .name       = "Variable Slider",
        .steps      = g_tutorial_variable_slider_steps,
        .tags       = TUTORIAL_TAG_COLOR_TRANSFORMS,
        .subheading = "Beginner",
    },
    {
        /* Keep this Beginner entry before the first Intermediate entry so
         * the All flyout's section headers remain a single contiguous
         * Beginner run followed by Intermediate. This is the first entry
         * carrying ANIMATION, so that tag becomes visible in the menu. */
        .name       = "First Animation",
        .steps      = g_tutorial_first_animation_steps,
        .cfg        = g_tutorial_first_animation_cfg,
        .tags       = TUTORIAL_TAG_ANIMATION,
        .subheading = "Beginner",
    },
    {
        .name       = "Points & Lines",
        .steps      = g_tutorial_points_lines_steps,
        .cfg        = g_tutorial_points_lines_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Beginner",
    },
    {
        .name       = "GLUT Solids Tour",
        .steps      = g_tutorial_glut_solids_steps,
        .setup      = g_tutorial_glut_solids_setup,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY | TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Beginner",
    },
    {
        .name       = "First Loop",
        .steps      = g_tutorial_first_loop_steps,
        .tags       = TUTORIAL_TAG_REPL_LANGUAGE,
        .subheading = "Beginner",
    },
    {
        .name       = "Depth Test Triangle",
        .steps      = g_tutorial_depth_triangle_steps,
        .tags       = TUTORIAL_TAG_GEOMETRY | TUTORIAL_TAG_DEPTH_LIGHTING,
        /* Intermediate: introduces depth-testing as a new GL concept
         * (the previous tutorials were pure geometry/color/transform). */
        .subheading = "Intermediate",
    },
    {
        /* Appended after Depth Test Triangle so both Intermediate entries
         * stay contiguous (Beginner run, then Intermediate run) in the ALL
         * flyout, and the two DEPTH_LIGHTING entries (Depth Test Triangle,
         * Lighting Basics) form a single Intermediate run in that flyout -
         * test_catalog_subheading_metadata enforces both. */
        .name       = "Lighting Basics",
        .steps      = g_tutorial_lighting_basics_steps,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        /* Builds on First Triangle via a preloaded setup scaffold -
         * the learner colors the corners without re-drawing the
         * triangle. It closes the original Intermediate trio; the catalog
         * continues with more Intermediate entries. */
        .name       = "Color Interpolation",
        .steps      = g_tutorial_color_interp_steps,
        .setup      = g_tutorial_color_interp_setup,
        .tags       = TUTORIAL_TAG_COLOR_TRANSFORMS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Line Stipple",
        .steps      = g_tutorial_line_stipple_steps,
        .cfg        = g_tutorial_line_stipple_cfg,
        .tags       = TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Blending & Transparency",
        .steps      = g_tutorial_blending_steps,
        .cfg        = g_tutorial_blending_cfg,
        .tags       = TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Depth Mask & Draw Order",
        .steps      = g_tutorial_depth_mask_steps,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_EFFECTS | TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        .name       = "Fog",
        .steps      = g_tutorial_fog_steps,
        .cfg        = g_tutorial_fog_cfg,
        .setup      = g_tutorial_fog_setup,
        .tags       = TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Clip Planes",
        .steps      = g_tutorial_clip_planes_steps,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Materials & Shininess",
        .steps      = g_tutorial_materials_steps,
        .setup      = g_tutorial_materials_setup,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING | TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Flat & Smooth Shading",
        .steps      = g_tutorial_shade_model_steps,
        .setup      = g_tutorial_shade_model_setup,
        .cfg        = g_tutorial_dense_solid_cfg,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        .name       = "Normals",
        .steps      = g_tutorial_normals_steps,
        .setup      = g_tutorial_normals_setup,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        .name       = "Culling & Winding",
        .steps      = g_tutorial_culling_steps,
        .setup      = g_tutorial_culling_setup,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Intermediate",
    },
    {
        .name       = "Bitmap Text",
        .steps      = g_tutorial_bitmap_text_steps,
        .cfg        = g_tutorial_bitmap_text_cfg,
        .tags       = TUTORIAL_TAG_GEOMETRY,
        .subheading = "Intermediate",
    },
    {
        .name       = "Functions",
        .steps      = g_tutorial_functions_steps,
        .cfg        = g_tutorial_unlit_solid_cfg,
        .tags       = TUTORIAL_TAG_REPL_LANGUAGE,
        .subheading = "Advanced",
    },
    {
        .name       = "If & Conditionals",
        .steps      = g_tutorial_conditionals_steps,
        .cfg        = g_tutorial_conditionals_cfg,
        .tags       = TUTORIAL_TAG_REPL_LANGUAGE | TUTORIAL_TAG_ANIMATION,
        .subheading = "Advanced",
    },
    {
        .name       = "Scratch Arrays",
        .steps      = g_tutorial_scratch_arrays_steps,
        .setup      = g_tutorial_scratch_arrays_setup,
        .cfg        = g_tutorial_unlit_solid_cfg,
        .tags       = TUTORIAL_TAG_REPL_LANGUAGE,
        .subheading = "Advanced",
    },
};

static int tutorial_catalog_entry_count(void) {
    return repl_tutorial_count();
}

static int tutorial_catalog_tag_count(void) {
    return REPL_TUTORIAL_TAG_COUNT;
}

static unsigned int tutorial_catalog_tag_bit(int tag_idx) {
    return repl_tutorial_tag_bit(tag_idx);
}

static const ReplCatalogTagOps g_tutorial_tag_ops = {
    tutorial_catalog_entry_count,
    tutorial_catalog_tag_count,
    g_tutorial_tag_labels,
    repl_tutorial_tag_mask,
    tutorial_catalog_tag_bit,
};

static const TutorialEntry *tutorial_entry_at(int idx) {
    int count = (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
    if (idx < 0 || idx >= count)
        return NULL;
    return &g_tutorials[idx];
}

const TutorialEntry *repl_tutorial_entry(int idx) {
    return tutorial_entry_at(idx);
}

const TutorialStep *repl_tutorial_step_get(int idx, int step_idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || !entry->steps || step_idx < 0)
        return NULL;

    for (int i = 0; i <= step_idx; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (repl_tutorial_step_is_sentinel(step))
            return NULL;
        if (i == step_idx)
            return step;
    }
    return NULL;
}

const char *repl_tutorial_step_comment(int idx, int step_idx) {
    static char comment_buf[256];
    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
    const TutorialStep *step = repl_tutorial_step_get(idx, step_idx);
    if (!step)
        return NULL;
    if (!step->comment_binding_key)
        return step->comment;

    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             step->comment_binding_key,
                             step->comment_binding_mods,
                             step->comment_binding_is_special);
    snprintf(comment_buf, sizeof(comment_buf), step->comment, shortcut);
    return comment_buf;
}

int repl_tutorial_count(void) {
    return (int)(sizeof(g_tutorials) / sizeof(g_tutorials[0]));
}

const char *repl_tutorial_name(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->name : NULL;
}

int repl_tutorial_step_count(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry || !entry->steps)
        return 0;

    int count = 0;
    while (!repl_tutorial_step_is_sentinel(&entry->steps[count]))
        count++;
    return count;
}

/* The per-field step accessors mostly live as `static inline` helpers in
 * tutorials.h and call repl_tutorial_step_get() once each. Callers
 * that need more than one field per step (e.g. the tutorial-menu
 * row renderer) should call repl_tutorial_step_get directly and
 * walk fields off the returned pointer to avoid O(N) walks per
 * field. */

const char *const *repl_tutorial_cfg_lines(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->cfg : NULL;
}

const char *const *repl_tutorial_setup_lines(int idx) {
    const TutorialEntry *entry = tutorial_entry_at(idx);
    return entry ? entry->setup : NULL;
}

int repl_tutorial_tag_count(void) {
    return REPL_TUTORIAL_TAG_COUNT;
}

unsigned int repl_tutorial_tag_mask(int tutorial_idx) {
    if (tutorial_idx < 0 || tutorial_idx >= repl_tutorial_count())
        return 0u;
    /* Fold in the synthetic "All" membership so every tag query derives
     * it uniformly; entry literals stay free of an explicit ALL bit. */
    return g_tutorials[tutorial_idx].tags | TUTORIAL_TAG_ALL;
}

REPL_DEFINE_CATALOG_TAG_WRAPPERS(tutorial, &g_tutorial_tag_ops)

const char *repl_tutorial_subheading(int tutorial_idx) {
    const TutorialEntry *entry = tutorial_entry_at(tutorial_idx);
    return entry ? entry->subheading : NULL;
}

static int label_is_empty(const char *s) {
    return !s || s[0] == '\0';
}

static int shape_token_boundary(char ch) {
    return !(isalnum((unsigned char)ch) || ch == '_');
}

/* Syntactic shape classifier for COMMAND-step `expected` text. Mirrors
 * (loosely) what the editor's block commit kernels accept:
 *   CLOSE   trimmed text exactly "}" (repl_compile_close_brace_kernel's
 *           matched-existing branch when the cursor sits on the auto
 *           end row).
 *   BRANCH  "}" then an `else` token (compile_parse_if_branch_header;
 *           the kernel allows the trailing "{" to be omitted, so the
 *           classifier keys on the leading "} else" alone).
 *   OPEN    "ident(...) {": exactly one "{" as the last character, no
 *           "}", the char before the "{" (mod whitespace) is ")", and
 *           the text starts with an identifier followed by "(".
 * Anything else is ORDINARY. The classifier is deliberately permissive
 * about the interior (the runtime kernels do the real parsing); its
 * job is to make the validator's depth walk and the runner's
 * locked-line bookkeeping agree on which commits change block depth. */
TutorialExpectedShape repl_tutorial_expected_shape(const char *expected) {
    const char *p = expected;
    const char *end;

    if (!p)
        return TUTORIAL_EXPECTED_ORDINARY;
    while (*p && isspace((unsigned char)*p))
        p++;
    end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1]))
        end--;
    if (end == p)
        return TUTORIAL_EXPECTED_ORDINARY;

    if (*p == '}') {
        const char *q = p + 1;
        if (q == end)
            return TUTORIAL_EXPECTED_BLOCK_CLOSE;
        while (q < end && isspace((unsigned char)*q))
            q++;
        if (end - q >= 4 && strncmp(q, "else", 4) == 0 &&
            (q + 4 == end || shape_token_boundary(q[4])))
            return TUTORIAL_EXPECTED_BLOCK_BRANCH;
        return TUTORIAL_EXPECTED_ORDINARY;
    }

    if (end[-1] == '{') {
        int opens = 0, closes = 0;
        const char *q;
        for (q = p; q < end; q++) {
            if (*q == '{') opens++;
            if (*q == '}') closes++;
        }
        if (opens != 1 || closes != 0)
            return TUTORIAL_EXPECTED_ORDINARY;
        if (!(isalpha((unsigned char)*p) || *p == '_'))
            return TUTORIAL_EXPECTED_ORDINARY;
        /* ")" must immediately precede the "{" (mod whitespace). */
        q = end - 1;
        while (q > p && isspace((unsigned char)q[-1]))
            q--;
        if (q == p || q[-1] != ')')
            return TUTORIAL_EXPECTED_ORDINARY;
        /* Leading identifier followed by "(". */
        q = p;
        while (q < end && !shape_token_boundary(*q))
            q++;
        while (q < end && isspace((unsigned char)*q))
            q++;
        if (q >= end || *q != '(')
            return TUTORIAL_EXPECTED_ORDINARY;
        return TUTORIAL_EXPECTED_BLOCK_OPEN;
    }
    return TUTORIAL_EXPECTED_ORDINARY;
}

/* Does `expected` start with the given keyword token (after leading
 * whitespace)? Token-boundary aware so "form(" doesn't read as "for". */
static int expected_starts_with_keyword(const char *expected,
                                        const char *keyword) {
    const char *p = expected;
    size_t n = strlen(keyword);
    if (!p)
        return 0;
    while (*p && isspace((unsigned char)*p))
        p++;
    return strncmp(p, keyword, n) == 0 && shape_token_boundary(p[n]);
}

int repl_tutorial_expected_is_func_open(const char *expected) {
    if (repl_tutorial_expected_shape(expected) != TUTORIAL_EXPECTED_BLOCK_OPEN)
        return 0;
    return !expected_starts_with_keyword(expected, "for") &&
           !expected_starts_with_keyword(expected, "if");
}

/* Syntactic mirror of the parser's goto-label rule (src/repl/parser.c):
 * a setup line whose trimmed text is `:name` or `name:` (optionally
 * followed by a `;`) defines the goto label `name`. Used to validate
 * step target_labels against the setup scaffold without running the
 * live parser; the runtime resolves the actual row by walking the
 * loaded document's CMD_GOTO_LABEL commands. */
static int setup_line_goto_label(const char *line,
                                 char *name, int name_sz) {
    const char *p = line;
    int n = 0;

    if (!line || name_sz <= 1)
        return 0;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == ':') {
        p++;
        while ((isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
            name[n++] = *p++;
    } else {
        while ((isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
            name[n++] = *p++;
        if (*p != ':') {
            name[0] = '\0';
            return 0;
        }
        p++;
    }
    name[n] = '\0';
    if (n == 0)
        return 0;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == ';')
        p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return *p == '\0';
}

/* Does the entry's setup scaffold define goto label `name`? */
static int setup_defines_goto_label(const TutorialEntry *entry,
                                    const char *name) {
    char setup_label[REPL_GOTO_LABEL_MAX];

    if (!entry || !entry->setup || label_is_empty(name))
        return 0;
    for (int i = 0; entry->setup[i]; i++) {
        if (setup_line_goto_label(entry->setup[i], setup_label,
                                  (int)sizeof(setup_label)) &&
            strcmp(setup_label, name) == 0)
            return 1;
    }
    return 0;
}

/* v1 catalog rule: each `expected` must parse to exactly one
 * source-document change AND land at the runner's chosen
 * expected_commit_line on commit. The validator stays independent of
 * live parser state, so it applies a best-effort syntactic filter focused
 * on the patterns that actively break label-line bookkeeping:
 *
 *   - empty / whitespace-only text;
 *   - embedded newlines (`\n`, `\r`);
 *   - statement separators (`;`) - would commit two source rows;
 *   - block punctuation (`{`/`}`) outside the block shapes the
 *     classifier recognises (see repl_tutorial_expected_shape) -
 *     the runner's bookkeeping only understands the open / branch /
 *     close commit deltas;
 *   - ORDINARY text with a `for(` / `if(` header prefix - the block
 *     kernels claim those regardless of a trailing `{`, so the
 *     commit would still insert header + auto `}` (two rows) while
 *     the runner expected one;
 *   - any `float ` declaration (single- or multi-name) - the
 *     CMD_VAR_DECLARE placement rule relocates the new decl to the
 *     top of non-decl code regardless of edit_line, so
 *     pending.commit_line would not match the actual landing row
 *     and committed_line_for_step would point at the wrong source
 *     line for any later label-targeted step.
 *
 * Known-shallow gaps (commit-time failures, not catastrophic): an
 * unknown GL call or one with wrong arity validates here but fails
 * at commit time, leaving the tutorial unrunnable at that step.
 * Catalog authors see those failures the first time they step
 * through the tutorial. */
static int expected_is_single_command(const char *expected,
                                      TutorialExpectedShape shape,
                                      char *err, int err_size) {
    if (!expected) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "expected is NULL");
        return 0;
    }

    int saw_non_ws = 0;
    for (const char *p = expected; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n' || ch == '\r') {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has embedded newline: %s", expected);
            return 0;
        }
        if (ch == ';') {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has statement separator ';': %s",
                         expected);
            return 0;
        }
        if ((ch == '{' || ch == '}') &&
            shape == TUTORIAL_EXPECTED_ORDINARY) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "expected has block punctuation: %s", expected);
            return 0;
        }
        if (!isspace(ch))
            saw_non_ws = 1;
    }
    if (!saw_non_ws) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "expected is empty");
        return 0;
    }

    /* A `for(...)` / `if(...)` header without the trailing `{` still
     * commits as a two-row block (the kernels key on the prefix, not
     * the brace), so an ORDINARY-shaped header is a bookkeeping trap:
     * the runner would expect a one-row commit and desync. Authors
     * must spell block heads with the `{` so they classify as OPEN. */
    if (shape == TUTORIAL_EXPECTED_ORDINARY &&
        (expected_starts_with_keyword(expected, "for") ||
         expected_starts_with_keyword(expected, "if"))) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size,
                     "expected block header must end in '{': %s", expected);
        return 0;
    }

    /* Reject every `float ` declaration. Single-name decls like
     * `float x` parse fine but the commit path relocates them to
     * the top of non-decl code (CLAUDE.md: "new CMD_VAR_DECLARE
     * lines are inserted at the top of non-decl code [...]
     * regardless of cursor position"), which means
     * pending.commit_line drifts away from the actual landing row
     * and any later label-targeted step that targets this decl
     * would resolve to the wrong source line. Multi-name decls
     * additionally expand into one CMD_VAR_DECLARE per name. */
    if (expected_starts_with_keyword(expected, "float")) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size,
                     "expected `float` declarations are not allowed in "
                     "tutorial steps (placement rule relocates them): %s",
                     expected);
        return 0;
    }
    return 1;
}

int repl_tutorial_validate(int idx, char *err, int err_size) {
    if (err && err_size > 0) err[0] = '\0';
    const TutorialEntry *entry = tutorial_entry_at(idx);
    if (!entry) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial idx %d out of range", idx);
        return 0;
    }
    return repl_tutorial_validate_entry(entry, err, err_size);
}

int repl_tutorial_validate_entry(const TutorialEntry *entry,
                                 char *err, int err_size) {
    if (err && err_size > 0) err[0] = '\0';
    if (!entry) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial entry is NULL");
        return 0;
    }
    if (!entry->steps) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size, "tutorial '%s' has no steps",
                     entry->name ? entry->name : "?");
        return 0;
    }

    /* Walk steps, validating each as we go. Forward references are
     * rejected naturally because target_label can only match a label
     * we have already seen. Sentinel is comment AND expected both NULL
     * - SET/REQUIRE/NOTE steps legitimately leave `expected` NULL, and
     * comment-less COMMAND steps legitimately leave `comment` NULL.
     *
     * Block-shape COMMAND steps are tracked with a depth counter (and
     * a per-level is-it-an-if flag so BRANCH steps can be pinned to an
     * enclosing if-open). The walk mirrors what the editor's block
     * commits do to the live document, so the runner's block_depth /
     * locked-line bookkeeping is guaranteed to see only sequences it
     * understands. */
    int step_count = 0;
    int depth = 0;
    int open_count = 0;
    int branch_count = 0;
    /* Set once any non-func content exists at the top level (an
     * ORDINARY command, or a for/if block). Func defs relocate to the
     * top of non-decl code on commit; while only comments / decls /
     * completed funcs precede, that relocation is an identity and the
     * runner's pending.commit_line stays truthful - afterwards it is
     * not, so later func-opens are rejected. */
    int saw_topline_nonfunc = 0;
    unsigned char open_is_if[TUTORIAL_MAX_STEPS];
    for (int i = 0;; i++) {
        const TutorialStep *step = &entry->steps[i];
        if (repl_tutorial_step_is_sentinel(step)) {
            break;
        }
        if (step_count >= TUTORIAL_MAX_STEPS) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' has more than %d steps",
                         entry->name ? entry->name : "?",
                         TUTORIAL_MAX_STEPS);
            return 0;
        }
        /* Inside an open block, the cursor lives on the auto-inserted
         * `}` row: label placement (which splices elsewhere) and
         * REQUIRE_VAR steps (whose satisfying decl/assign commits at a
         * free line of the user's choosing) would both fight the
         * in-block insertion point, so they are rejected until the
         * block closes. */
        if (depth > 0) {
            if (step->placement == TUTORIAL_STEP_LABEL) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d label placement not "
                             "allowed inside an open block",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (step->kind == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR not "
                             "allowed inside an open block",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        }
        /* Kind-aware shape check. Slug *validity* (is the bridge aware
         * of this slug?) is a runtime check at tutorial_start because
         * it depends on the controller-installed config bridge. */
        if (step->kind == TUTORIAL_STEP_KIND_COMMAND) {
            /* `comment` is optional for COMMAND - NULL/empty commits the
             * expected command with no locked instruction row. */
            if (!step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d COMMAND missing expected",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            TutorialExpectedShape shape =
                repl_tutorial_expected_shape(step->expected);
            if (!expected_is_single_command(step->expected, shape,
                                            err, err_size))
                return 0;
            if (shape == TUTORIAL_EXPECTED_BLOCK_OPEN) {
                if (step->placement != TUTORIAL_STEP_APPEND) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d block-open step "
                                 "must use append placement",
                                 entry->name ? entry->name : "?", i);
                    return 0;
                }
                if (repl_tutorial_expected_is_func_open(step->expected)) {
                    if (depth > 0) {
                        if (err_size > 0)
                            snprintf(err, (size_t)err_size,
                                     "tutorial '%s' step %d func def cannot "
                                     "nest inside a block",
                                     entry->name ? entry->name : "?", i);
                        return 0;
                    }
                    if (entry->setup) {
                        if (err_size > 0)
                            snprintf(err, (size_t)err_size,
                                     "tutorial '%s' step %d func-open steps "
                                     "cannot combine with a setup scaffold "
                                     "(func relocation would desync rows)",
                                     entry->name ? entry->name : "?", i);
                        return 0;
                    }
                    if (saw_topline_nonfunc) {
                        if (err_size > 0)
                            snprintf(err, (size_t)err_size,
                                     "tutorial '%s' step %d func def must "
                                     "precede other top-level commands "
                                     "(relocation would desync rows)",
                                     entry->name ? entry->name : "?", i);
                        return 0;
                    }
                } else if (depth == 0) {
                    saw_topline_nonfunc = 1;
                }
                open_is_if[depth] = (unsigned char)
                    expected_starts_with_keyword(step->expected, "if");
                depth++;
                open_count++;
            } else if (shape == TUTORIAL_EXPECTED_BLOCK_BRANCH) {
                if (step->placement != TUTORIAL_STEP_APPEND) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d block-branch step "
                                 "must use append placement",
                                 entry->name ? entry->name : "?", i);
                    return 0;
                }
                if (depth < 1 || !open_is_if[depth - 1]) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d else branch outside "
                                 "an open if block",
                                 entry->name ? entry->name : "?", i);
                    return 0;
                }
                branch_count++;
            } else if (shape == TUTORIAL_EXPECTED_BLOCK_CLOSE) {
                if (step->placement != TUTORIAL_STEP_APPEND) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d close-brace step "
                                 "must use append placement",
                                 entry->name ? entry->name : "?", i);
                    return 0;
                }
                if (depth < 1) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d unmatched close "
                                 "brace",
                                 entry->name ? entry->name : "?", i);
                    return 0;
                }
                depth--;
            } else if (depth == 0) {
                saw_topline_nonfunc = 1;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_NOTE) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d NOTE must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d NOTE needs non-empty "
                             "comment",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_SET ||
                   step->kind == TUTORIAL_STEP_KIND_REQUIRE) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
            if (label_is_empty(step->cfg_slug)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s needs non-empty "
                             "cfg_slug",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d %s needs non-empty "
                             "comment",
                             entry->name ? entry->name : "?", i,
                             step->kind == TUTORIAL_STEP_KIND_SET
                                 ? "SET" : "REQUIRE");
                return 0;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_SET_QUIET) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d SET_QUIET must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (label_is_empty(step->cfg_slug)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d SET_QUIET needs non-empty "
                             "cfg_slug",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            /* A SET_QUIET never renders an instruction row, so a comment
             * on one is dead text the author expected to see. Reject it
             * rather than swallow it - the fix is a SET step. */
            if (step->comment) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d SET_QUIET must leave "
                             "comment NULL (use SET to show one)",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else if (step->kind == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
            if (step->expected) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR must leave "
                             "expected NULL",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (label_is_empty(step->var_name)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR needs "
                             "non-empty var_name",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            if (repl_eval_is_reserved_ident(step->var_name)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR var_name "
                             "'%s' is reserved",
                             entry->name ? entry->name : "?", i,
                             step->var_name);
                return 0;
            }
            if (label_is_empty(step->comment)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d REQUIRE_VAR needs "
                             "non-empty comment",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d unknown kind",
                         entry->name ? entry->name : "?", i);
            return 0;
        }

        /* Unique non-empty labels - and no shadowing of a `:name` goto
         * label the setup scaffold defines, so a target_label always
         * resolves unambiguously to one anchor. */
        if (!label_is_empty(step->label)) {
            for (int j = 0; j < i; j++) {
                if (!label_is_empty(entry->steps[j].label) &&
                    strcmp(entry->steps[j].label, step->label) == 0) {
                    if (err_size > 0)
                        snprintf(err, (size_t)err_size,
                                 "tutorial '%s' step %d duplicate label '%s'",
                                 entry->name ? entry->name : "?", i,
                                 step->label);
                    return 0;
                }
            }
            if (setup_defines_goto_label(entry, step->label)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d label '%s' collides "
                             "with a setup goto label",
                             entry->name ? entry->name : "?", i,
                             step->label);
                return 0;
            }
        }

        if (step->placement == TUTORIAL_STEP_APPEND) {
            if (!label_is_empty(step->target_label)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d append placement must not "
                             "set target_label",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
        } else if (step->placement == TUTORIAL_STEP_LABEL) {
            if (label_is_empty(step->target_label)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d label placement needs "
                             "non-empty target_label",
                             entry->name ? entry->name : "?", i);
                return 0;
            }
            int found = 0;
            for (int j = 0; j < i; j++) {
                if (!label_is_empty(entry->steps[j].label) &&
                    strcmp(entry->steps[j].label, step->target_label) == 0) {
                    found = 1;
                    break;
                }
            }
            /* A target can also anchor on a `:name` goto label the
             * setup scaffold defines - that's how steps splice new
             * commands INTO preloaded code (resolved at step entry by
             * walking the live document's CMD_GOTO_LABEL rows). */
            if (!found && setup_defines_goto_label(entry, step->target_label))
                found = 1;
            if (!found) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d target_label '%s' is "
                             "missing or forward-referenced",
                             entry->name ? entry->name : "?", i,
                             step->target_label);
                return 0;
            }
        } else {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d unknown placement",
                         entry->name ? entry->name : "?", i);
            return 0;
        }
        step_count++;
    }

    /* Every block that opens must close before the tutorial ends -
     * otherwise teardown leaves the learner inside a half-typed block
     * and the trailing auto-`}` never gets its matching step. */
    if (depth != 0) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size,
                     "tutorial '%s' ends inside an open block "
                     "(missing '}' step)",
                     entry->name ? entry->name : "?");
        return 0;
    }

    /* Every preloaded prelude row is locked: the injected scene-clear
     * rows (TUTORIAL_SCENE_PRELUDE_ROWS - the comment + glClear the runner
     * loads ahead of every tutorial) plus every setup line, plus up to one
     * baseline lock per step. A commented block-open needs TWO additional
     * locks for its committed header + auto-`}` (the baseline step lock is
     * its instruction comment); a comment-less open needs only one extra,
     * but budgeting two for every open is a safe upper bound. A branch
     * similarly needs one extra lock for its committed separator row.
     * Counting every setup line (metadata headers included) also
     * overcounts - safely. Check even without setup so the relationship
     * remains sound if either capacity constant changes. */
    {
        int setup_count = 0;
        if (entry->setup) {
            while (entry->setup[setup_count])
                setup_count++;
        }
        if (TUTORIAL_SCENE_PRELUDE_ROWS + setup_count + step_count +
            2 * open_count + branch_count > TUTORIAL_LOCKED_LINE_MAX) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' prelude + setup lines (%d) + steps (%d) "
                         "+ block-shape extra locks (%d) exceed the locked-line "
                         "capacity (%d)",
                         entry->name ? entry->name : "?",
                         setup_count, step_count,
                         2 * open_count + branch_count,
                         TUTORIAL_LOCKED_LINE_MAX);
            return 0;
        }
    }
    return 1;
}
