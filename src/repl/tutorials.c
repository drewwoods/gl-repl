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

#define STEP_APPEND(label, c, e) \
    { (label), (c), (e), TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Comment-less append COMMAND step: commits `e` with no locked
 * instruction row above it — the autocomplete ghost and status hint
 * still teach the command. Use for runs of related commands where a
 * narration comment per line would just be noise. */
#define STEP_CMD(label, e) \
    { (label), NULL, (e), TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Comment-only step: reveal the instruction comment, wait for an ack
 * key (Enter/Tab/Space), advance. SET's showcase flow without the cfg
 * write — narration between commands with no GL call to type. */
#define STEP_NOTE(c) \
    { NULL, (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_NOTE, NULL, 0, NULL, NULL, 0.0f }

#define STEP_AT(label, c, e, target) \
    { (label), (c), (e), TUTORIAL_STEP_LABEL, (target), \
      TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

/* Showcase step: on entry apply cfg_slug=cfg_value so the user sees the
 * effect, show a "press Enter to continue" prompt, advance on ack key.
 * `expected` is NULL — there is no command to type. */
#define STEP_SET(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_SET, (slug), (val), NULL, NULL, 0.0f }

/* Symbolic-value variant of STEP_SET. The runner passes `val_name`
 * (e.g. "GRID_THEME_RADAR") through the controller-installed bridge's
 * resolve_text at apply time so the catalog reads the enum constant
 * rather than encoding a magic number. `cfg_value` stays 0 here as
 * the back-compat fallback in case a future caller resolves the name
 * and writes the int back. */
#define STEP_SET_SYM(label, c, slug, val_name) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_SET, (slug), 0, (val_name), NULL, 0.0f }

/* Check step: advance when the user themselves makes cfg_slug == cfg_value
 * (via F-key/menu/etc.). Auto-advances if already satisfied on entry.
 * `expected` is NULL. */
#define STEP_REQUIRE(label, c, slug, val) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), (val), NULL, NULL, 0.0f }

#define STEP_REQUIRE_KEY(label, c, slug, val, key_code, mods, is_special) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), (val), NULL, NULL, 0.0f, \
      (key_code), (mods), (is_special) }

/* Symbolic-value variant of STEP_REQUIRE. The runner resolves
 * `val_name` to int via the bridge's resolve_text at compare time. */
#define STEP_REQUIRE_SYM(label, c, slug, val_name) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE, (slug), 0, (val_name), NULL, 0.0f }

/* Predef-var check step: advance when the named predefined variable's
 * live value matches `target` within TUTORIAL_VAR_EPS. Either a typed
 * `name = expr;` commit or a variable-panel slider drag satisfies it.
 * Auto-advances if already satisfied on entry. `expected` is NULL. */
#define STEP_REQUIRE_VAR(label, c, var, target) \
    { (label), (c), NULL, TUTORIAL_STEP_APPEND, NULL, \
      TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, (var), (target) }

/* Sentinel: comment AND expected both NULL (repl_tutorial_step_is_sentinel).
 * Every real step carries at least one of the two — SET/REQUIRE/NOTE have
 * NULL `expected` but a comment; comment-less COMMAND steps have NULL
 * `comment` but an expected. */
#define STEP_SENTINEL { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL, \
                        TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f }

static const TutorialStep g_tutorial_first_triangle_steps[] = {
    STEP_APPEND(NULL,
        "// Build the smallest filled shape: open a GL_TRIANGLES batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Place the first vertex near the top; this becomes the triangle tip.",
        "glVertex3f(0, 2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-left corner so the triangle has width.",
        "glVertex3f(-2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner; three vertices complete one triangle.",
        "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Close the batch and the filled triangle appears in the scene.",
        "glEnd()"),
    STEP_SENTINEL,
};

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
        "glVertex3f(-2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right corner at the same height.",
        "glVertex3f(2, -2, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-right corner so the quad has height.",
        "glVertex3f(2, 2, 0)"),
    STEP_APPEND(NULL,
        "// Add the upper-left corner; vertex order walks around the square.",
        "glVertex3f(-2, 2, 0)"),
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
 * "triangle_begin" anchors the insertion (implemented in Phase 2). */
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
    NULL,
};

/* "Feature Tour" — exercises the non-COMMAND step kinds and the
 * relaxed step shapes:
 *   1) A NOTE step opens the tour (comment-only; ack key advances).
 *   2) Five COMMAND steps draw a triangle in 3D. The two lower-corner
 *      vertex steps are comment-less (STEP_CMD) — the autocomplete
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
 * steps use symbolic cfg values so enum reordering in src/scene/themes.h
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

/* "Variable Slider" — teaches the REQUIRE_VAR step kind by driving a
 * variable that controls a drawn shape. The user declares `n` (the
 * triangle's size), draws a triangle whose vertices use `n`, then grows
 * `n` to 10 with the variable-panel slider and watches the triangle
 * scale. Both a typed commit and a slider drag flow through
 * repl_apply_predef_ops, which the editor commit path notifies after.
 * No `@cfg` block — presentation defaults are fine.
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
    STEP_REQUIRE_VAR(NULL,
        "// Now drag the n slider in the variable panel to bring n to 10.",
        "n", 10.0f),
    STEP_SENTINEL,
};

/* "Lighting Basics" — the first lit-surface tutorial. Six COMMAND steps
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

/* "First Animation" — introduces the built-in `t` clock without asking the
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
        "glutSolidCube(1)"),
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

/* "Color Interpolation" — the first tutorial that BUILDS ON prior work
 * via a preloaded `setup` scaffold instead of making the user re-type
 * it (see docs/plans/active/tutorial-setup-scaffold.md). The scaffold
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

/* Phase C catalog expansion -------------------------------------------------
 * These tutorials deliberately mix narrated STEP_APPEND rows with compact
 * STEP_CMD runs. The former explain a new idea; the latter let the ghost text
 * carry repetitive geometry so the resulting source stays readable. */

static const char *const g_tutorial_points_lines_cfg[] = {
    "// @cfg view_mode = RENDER3D_VIEW_2D",
    NULL,
};

static const TutorialStep g_tutorial_points_lines_steps[] = {
    STEP_APPEND(NULL,
        "// Make individual points large enough to read clearly.",
        "glPointSize(8)"),
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

static const char *const g_tutorial_glut_solids_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -14.00f);",
    "glRotatef(18.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(25.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "// Lighting scaffold shared by every solid in this tour.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    "glEnable(GL_COLOR_MATERIAL)",
    "glColor3f(0.8, 0.7, 0.3)",
    NULL,
};

static const TutorialStep g_tutorial_glut_solids_steps[] = {
    STEP_APPEND(NULL,
        "// Move left, then place a cube at the new local origin.",
        "glTranslatef(-8, 0, 0)"),
    STEP_CMD(NULL, "glutSolidCube(1)"),
    STEP_APPEND(NULL,
        "// Step right and compare the cube with a smooth sphere.",
        "glTranslatef(4, 0, 0)"),
    STEP_CMD(NULL, "glutSolidSphere(1, 16, 12)"),
    STEP_APPEND(NULL,
        "// Move again; a torus uses inner and outer radii plus two resolutions.",
        "glTranslatef(4, 0, 0)"),
    STEP_CMD(NULL, "glutSolidTorus(1, 2, 16, 24)"),
    STEP_APPEND(NULL,
        "// Add freeglut's classic teapot beside the torus.",
        "glTranslatef(4, 0, 0)"),
    STEP_CMD(NULL, "glutSolidTeapot(1)"),
    STEP_APPEND(NULL,
        "// Finish the lineup with a cone.",
        "glTranslatef(4, 0, 0)"),
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
        "glutSolidCube(1)"),
    STEP_CMD(NULL, "glEnable(GL_BLEND)"),
    STEP_CMD(NULL, "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)"),
    STEP_APPEND(NULL,
        "// Stop translucent fragments from writing new depth values.",
        "glDepthMask(GL_FALSE)"),
    STEP_CMD(NULL, "glColor4f(0.4, 0.8, 1, 0.4)"),
    STEP_APPEND(NULL,
        "// Draw a larger translucent sphere around the opaque cube.",
        "glutSolidSphere(1, 32, 24)"),
    STEP_APPEND(NULL,
        "// Restore depth writes for whatever is drawn next.",
        "glDepthMask(GL_TRUE)"),
    STEP_NOTE(
        "// Translucent objects usually render last, with depth testing on and depth writes off."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_fog_cfg[] = {
    "// @cfg backdrop = RENDER3D_BACKDROP_STARS",
    NULL,
};

static const char *const g_tutorial_fog_setup[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -16.00f);",
    "glRotatef(14.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(20.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "// A locked row of toruses receding away from the camera.",
    ":draw",
    "for(i, 0, 8) {",
    "  glPushMatrix()",
    "  glTranslatef(0, 0, -i * 3)",
    "  glutSolidTorus(1, 2, 16, 24)",
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
        "glFogf(GL_FOG_DENSITY, 0.1)", "draw"),
    STEP_AT(NULL,
        "// Match the fog color to the dark star-field backdrop.",
        "glFogfv(GL_FOG_COLOR, 0.05, 0.08, 0.12, 1)", "draw"),
    STEP_NOTE(
        "// The nearest torus stays clear while each deeper torus fades farther into the scene."),
    STEP_SENTINEL,
};

static const TutorialStep g_tutorial_clip_planes_steps[] = {
    STEP_APPEND(NULL,
        "// Define plane 0 as y + 1 = 0; its positive half-space is kept.",
        "glClipPlane(GL_CLIP_PLANE0, 0, 1, 0, 1)"),
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

static const char *const g_tutorial_normals_setup[] = {
    "// Lighting scaffold for the normal and shade-model demonstration.",
    "glEnable(GL_DEPTH_TEST)",
    "glEnable(GL_LIGHTING)",
    "glEnable(GL_LIGHT0)",
    "glEnable(GL_COLOR_MATERIAL)",
    NULL,
};

static const TutorialStep g_tutorial_normals_steps[] = {
    STEP_APPEND(NULL,
        "// Flat shading uses one provoking-vertex result across each polygon.",
        "glShadeModel(GL_FLAT)"),
    STEP_APPEND(NULL,
        "// Open a quad batch for one lit surface.",
        "glBegin(GL_QUADS)"),
    STEP_APPEND(NULL,
        "// Point the surface normal toward +Z so lighting can orient the face.",
        "glNormal3f(0, 0, 1)"),
    STEP_CMD(NULL, "glVertex3f(-2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, -2, 0)"),
    STEP_CMD(NULL, "glVertex3f(2, 2, 0)"),
    STEP_CMD(NULL, "glVertex3f(-2, 2, 0)"),
    STEP_CMD(NULL, "glEnd()"),
    STEP_SET(NULL,
        "// Normal-vector overlays reveal the direction supplied by glNormal3f.",
        "normal_vectors", 1),
    STEP_NOTE(
        "// GL_SMOOTH interpolates vertex lighting; Auto-normals can synthesize missing face normals."),
    STEP_SENTINEL,
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
    STEP_SET_SYM(NULL,
        "// Plain wireframe makes the submitted triangle edges easier to compare.",
        "wireframe", "WIREFRAME_PLAIN"),
    STEP_REQUIRE_KEY(NULL,
        "// Press %s to show the winding-direction overlay.",
        "winding", 1,
        KM_KEY(GLR_WINDING_VIEW), KM_MODS(GLR_WINDING_VIEW), 0),
    STEP_NOTE(
        "// Reverse the vertex order or glFrontFace mode to decide which side survives culling."),
    STEP_SENTINEL,
};

static const char *const g_tutorial_bitmap_text_cfg[] = {
    "// @cfg auto_time = 1",
    NULL,
};

static const TutorialStep g_tutorial_bitmap_text_steps[] = {
    STEP_APPEND(NULL,
        "// Draw a cube to give the label a visible scene reference.",
        "glutSolidCube(1)"),
    STEP_APPEND(NULL,
        "// Set the 3D raster anchor just above the cube.",
        "glRasterPos3f(0, 2, 0)"),
    STEP_APPEND(NULL,
        "// Format the live t clock into bitmap text at that raster position.",
        "label(\"t %f\", t)"),
    STEP_NOTE(
        "// label formats numbers with %f; its format text cannot contain parentheses, commas, or backslashes."),
    STEP_SENTINEL,
};

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
    STEP_CMD(NULL, "glutSolidSphere(1, 24, 16)"),
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
        "glutSolidCube(1)"),
    STEP_SENTINEL,
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
    STEP_CMD(NULL, "glutSolidSphere(1, 24, 16)"),
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
 * vocabulary — the menu just emits `### subheading` chrome rows when
 * the subheading changes. The current catalog uses Beginner,
 * Intermediate, and Advanced; future catalogs can use any labels that
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
         * once to Intermediate at the tail — the subheading contiguity
         * test (test_catalog_subheading_metadata) requires a single
         * Beginner→Intermediate transition across the whole catalog. */
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
         * Lighting Basics) form a single Intermediate run in that flyout —
         * test_catalog_subheading_metadata enforces both. */
        .name       = "Lighting Basics",
        .steps      = g_tutorial_lighting_basics_steps,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        /* Builds on First Triangle via a preloaded setup scaffold —
         * the learner colors the corners without re-drawing the
         * triangle. It closes the original Intermediate trio while the
         * global catalog continues with Phase C's Intermediate entries. */
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
        .tags       = TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Materials & Shininess",
        .steps      = g_tutorial_materials_steps,
        .setup      = g_tutorial_materials_setup,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING | TUTORIAL_TAG_EFFECTS,
        .subheading = "Intermediate",
    },
    {
        .name       = "Normals & Shade Model",
        .steps      = g_tutorial_normals_steps,
        .setup      = g_tutorial_normals_setup,
        .tags       = TUTORIAL_TAG_DEPTH_LIGHTING,
        .subheading = "Intermediate",
    },
    {
        .name       = "Culling & Winding",
        .steps      = g_tutorial_culling_steps,
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
 * expected_commit_line on commit. The full guarantee would require
 * driving the live parser/compile seam against a temp document
 * snapshot, which the catalog validator can't easily do without
 * dragging REPL state into Phase 1. Until that lands, this checker
 * is a best-effort syntactic filter focused on the patterns that
 * actively break label-line bookkeeping:
 *
 *   - empty / whitespace-only text;
 *   - embedded newlines (`\n`, `\r`);
 *   - statement separators (`;`) — would commit two source rows;
 *   - block punctuation (`{`/`}`) outside the block shapes the
 *     classifier recognises (see repl_tutorial_expected_shape) —
 *     the runner's bookkeeping only understands the open / branch /
 *     close commit deltas;
 *   - ORDINARY text with a `for(` / `if(` header prefix — the block
 *     kernels claim those regardless of a trailing `{`, so the
 *     commit would still insert header + auto `}` (two rows) while
 *     the runner expected one;
 *   - any `float ` declaration (single- or multi-name) — the
 *     CMD_VAR_DECLARE placement rule relocates the new decl to the
 *     top of non-decl code regardless of edit_line, so
 *     pending.commit_line would not match the actual landing row
 *     and committed_line_for_step would point at the wrong source
 *     line for any later label-targeted step.
 *
 * Known-shallow gaps (commit-time failures, not catastrophic): an
 * unknown GL call or one with wrong arity validates here but fails
 * at commit time, leaving the tutorial unrunnable at that step.
 * Catalog authors notice immediately. Promoting this to a real
 * parser-driven check is tracked as future work. */
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
     * — SET/REQUIRE/NOTE steps legitimately leave `expected` NULL, and
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
     * runner's pending.commit_line stays truthful — afterwards it is
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
            /* `comment` is optional for COMMAND — NULL/empty commits the
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

        /* Unique non-empty labels — and no shadowing of a `:name` goto
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
             * setup scaffold defines — that's how steps splice new
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

    /* Every block that opens must close before the tutorial ends —
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
     * rows (TUTORIAL_SCENE_PRELUDE_ROWS — the comment + glClear the runner
     * loads ahead of every tutorial) plus every setup line, plus up to one
     * baseline lock per step. A commented block-open needs TWO additional
     * locks for its committed header + auto-`}` (the baseline step lock is
     * its instruction comment); a comment-less open needs only one extra,
     * but budgeting two for every open is a safe upper bound. A branch
     * similarly needs one extra lock for its committed separator row.
     * Counting every setup line (metadata headers included) also
     * overcounts — safely. Check even without setup so the relationship
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
