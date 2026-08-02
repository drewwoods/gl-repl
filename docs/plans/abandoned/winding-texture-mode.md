# Winding view: add a "Color + Texture" mode (design brief)

## Status - ABANDONED (2026-07-29)

Not implemented and not planned. `GLR_CONFIG_WINDING_VIEW` stays the shipped
2-state Off/Color toggle (`src/app/glr_actions.c`, `.state_count = 2`); nothing
in the tree references a texture mode.

Kept for the design content, not the schedule: the two-pass-cull vs. single-pass
multitexture-combiner comparison below (including a working reference
implementation of the rejected combiner) is the reusable part if a textured
face-orientation mode is ever revisited. The open questions at the end were
never resolved. Move back to `not-started/` if it is.

## Context

The winding-visualization view shipped as a 2-state toggle
(`GLR_CONFIG_WINDING_VIEW`, "Winding" in Config → GEOMETRY): **Off** or
**Color**. In Color mode `render3d_pass_winding` in
[`src/render3d/render.c`](../../../src/render3d/render.c) draws one
two-sided-lighting pass - front material green, back material red, cull
off - so flipped / inside-out polygons read red against green. The
controller suppresses the program's own material/lighting/cull commands via
`winding_state_filter` ([`src/app/glr_ctrl.c`](../../../src/app/glr_ctrl.c))
through the generic `ReplExecutionOptions.state_filter` hook (see
ARCHITECTURE.md → *Filtering The REPL Program*).

This brief extends the toggle into a **3-state cycle**:

> **Off → Color → Color + Texture**

In "Color + Texture", front and back faces are additionally textured with
distinct, labelled images (the words **FRONT** / **BACK**), so a face's
orientation is unambiguous even in a still frame - the color tells you
winding at a glance, the text confirms it. Texture coordinates are generated
automatically with **`glTexGen` eye-linear**, because REPL geometry (user
`glVertex`, GLU tess, GLUT solids) carries no texcoords.

## Decisions (already made)

- **Apply method: two-pass cull** (not the single-pass multitexture
  combiner - see *Alternative* below for why it was considered and rejected
  for this codebase).
- **Texture content: literal `FRONT` / `BACK` text** rasterized (tiled) into
  the two textures, tinted by the green/red material so it still reads as
  winding.

## Chosen approach - two-pass cull + eye-linear texgen

`render3d_pass_winding` branches on the mode. Color stays one pass. Color +
Texture renders the program **twice**, letting `glCullFace` split front from
back and binding the matching texture per pass:

```c
// shared setup: two-sided lighting OFF is fine here - per-pass culling +
// per-pass material already separate front/back. Keep GL_LIGHTING on with a
// headlight for shading; GL_MODULATE lets the green/red material tint the
// FRONT/BACK text texture.
glEnable(GL_TEXTURE_2D);
glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
setup_eye_linear_texgen();           // GL_S/GL_T, GL_EYE_LINEAR, enable GEN_S/T
glEnable(GL_CULL_FACE);

// pass A: front faces only, green-tinted FRONT texture
glCullFace(GL_BACK);
glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
glBindTexture(GL_TEXTURE_2D, g_winding_front_tex);
render3d_execute_user_geometry(config, RENDER3D_EXEC_WINDING);

// pass B: back faces only, red-tinted BACK texture
glCullFace(GL_FRONT);
glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, red);    // back material lights the
glBindTexture(GL_TEXTURE_2D, g_winding_back_tex);      // reversed-normal face
render3d_execute_user_geometry(config, RENDER3D_EXEC_WINDING);
```

Why this fits **this** codebase: we already re-walk the flat program cheaply
per pass (the hidden-line wireframe walks it three times). A second walk is
negligible, and in return we get:

- **Core GL 1.1 only** - one texture unit, `glTexGen` (1.0), single
  `glBindTexture`. No `glActiveTexture`, no `ARB_multitexture`, no
  `ARB_texture_env_combine`, no runtime proc-loading, no stub-header
  additions, no capability gate / fallback path. Works identically on the
  Apple-legacy GL 2.1, Linux/Mesa, and OSMesa-swrast paths the project
  targets.
- Per-pass `glCullFace` + per-pass `glBindTexture` is trivially correct -
  no material-alpha-as-switch trick, no driver-specific combiner quirks.

`winding_state_filter` is reused unchanged (still suppresses user material /
lighting / cull / color-material so the pass owns that state). The pass is a
side-effecting fill replacement, so `scene_execute_adapter`'s
snapshot/restore must run the program once per visible frame, not per pass -
i.e. the predef/scratch/render restore that already brackets auxiliary
passes needs care here, since Color + Texture issues **two** geometry walks.
Resolve by either (a) treating only the *first* of the two winding walks as
side-effecting and bracketing the second, or (b) keeping both walks
non-mutating and advancing animation once outside them. (Open question 3.)

### Eye-linear texgen

```c
static const GLfloat s_plane[4] = { 0.5f, 0.0f, 0.0f, 0.5f };
static const GLfloat t_plane[4] = { 0.0f, 0.5f, 0.0f, 0.5f };
glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
glTexGenfv(GL_S, GL_EYE_PLANE, s_plane);   // planes are captured in eye space
glTexGenfv(GL_T, GL_EYE_PLANE, t_plane);   // at the moment GEN is enabled
glEnable(GL_TEXTURE_GEN_S);
glEnable(GL_TEXTURE_GEN_T);
```

Eye-linear projects the texture in eye space, so the FRONT/BACK lettering
stays fixed relative to the camera and the geometry slides through it as it
orbits - a stable, readable label rather than a per-triangle smear. Plane
scale sets how many times the word tiles across the view; tune so a few
repeats are visible. (Object-linear is the alternative if we'd rather pin the
text to the geometry - open question 4.)

### The FRONT / BACK textures

Built once (lazily on first use, freed on GL teardown alongside the other
render3d GL resources):

- Two small RGBA textures (e.g. 128×64), white text on transparent/!text so
  `GL_MODULATE` lets the green/red material tint the lettering and the
  background.
- Rasterize the strings by stamping a tiny built-in 5×7 bitmap glyph set into
  the RGBA buffer (we don't have FreeType; a minimal A-Z/0-9 stamp covers
  "FRONT"/"BACK"). Keep the glyph table local to the winding module.
- `GL_LINEAR` min/mag; `GL_REPEAT` wrap so eye-linear tiling reads cleanly.
- Lives in `src/render3d/` (pure GL, REPL-agnostic). Likely a small
  `winding_textures.{c,h}` or static helpers in `render.c`.

## Config / plumbing changes

- `GLR_CONFIG_WINDING_VIEW` becomes a **3-state cycle**: add
  `state_count = 3` + `state_names = { "Off", "Color", "Color + Texture" }`
  to the `g_cfg_items[]` row in
  [`src/app/glr_actions.c`](../../../src/app/glr_actions.c). The cycle
  plumbing in `glr_config.c` already handles N-state items; the backing
  `GlrPresentationState.winding_view` is already an `int` (0/1/2 now).
- `render3d_pass_fill` already branches on `config->winding_view` truthiness;
  `render3d_pass_winding` gains the `== 2` texture branch.
- The config is config-backed: it is emitted in full-workspace `@cfg`
  headers (`glr_export_cfg_fill_all`) but stays **out** of the per-example
  scene subset (`cfg_key_in_scene_subset`) so F12 example switches don't
  change it - unchanged from the shipped toggle. The full-export `@cfg`
  value string becomes `0` / `1` / `2`; no symbol table needed (numeric is
  fine, like other small cycles).

## Tests

- `test_glr_actions` cfg twin: bump the winding row's expected `state_count`
  to 3.
- Example UI goldens: the full-export dump already carries
  `/* @cfg winding = 0 */`; value stays `0` by default, so **no golden churn
  expected** from the cycle change itself (only if defaults change).
- Headless OSMesa capture (`scripts/docs-assets.sh` staging) to eyeball the
  FRONT/BACK lettering and the green/red tint on a mis-wound example.
- `make check-c99`, `make check-state-ownership`, full `make test`.

## Alternative considered - single-pass multitexture combiner

The originally-proposed method (kept here for the record) does it in **one**
walk by encoding the front/back distinction in the **material alpha** and
letting a fixed-function combiner pick the texture:

- Two-sided lighting on; front material alpha = 1.0, back material alpha =
  0.0. Fixed-function lighting takes the final alpha from the diffuse
  material, so the lit **primary color alpha** is 1 on front faces, 0 on
  back faces.
- Three texture units via `glActiveTexture`:
  - **Unit 0** samples the FRONT texture, `GL_REPLACE` → passes it down.
  - **Unit 1** samples the BACK texture, `GL_COMBINE` / `GL_INTERPOLATE`
    with `SOURCE0 = GL_PREVIOUS` (front), `SOURCE1 = GL_TEXTURE` (back),
    `SOURCE2 = GL_PRIMARY_COLOR` operand `GL_SRC_ALPHA` - so the per-face
    material alpha selects front-vs-back texture (`out = front*α +
    back*(1-α)`).
  - **Unit 2** `GL_COMBINE` / `GL_MODULATE` of `GL_PREVIOUS` × the
    `GL_PRIMARY_COLOR` lighting RGB, tinting the chosen texture green/red.
- One geometry walk; `glTexCoord`/texgen coords broadcast to all active
  units.

A self-contained reference implementation (spinning textured quad) is
preserved at the end of this file.

**Why not chosen for this codebase:** the combiner needs `ARB_multitexture`
+ `ARB_texture_env_combine`, runtime-loaded `glActiveTexture` /
`glMultiTexCoord` (the proc-loader pattern used for `glPointParameterfv` /
gpuprof), matching additions to the GL stub headers
(`tests/gl-stubs/include/GL/`), per-unit texgen, and a capability-gated
**fallback to plain Color** when any of that is missing - across the three
GL backends the project supports. The two-pass cull version reaches the
identical result with core GL 1.1 and none of that surface area, and the
extra program walk is free in an engine that already re-walks per pass. If a
future need makes a single pass mandatory (e.g. a per-sample accumulation
effect where re-walking is costly), revisit the combiner then.

### Reference: single-pass combiner (spinning quad)

```c
#include <GL/glew.h>
#include <GL/glut.h>
#include <stdio.h>

// Rotation angle for the quad
float rotationAngle = 0.0f;

// Texture handles
GLuint frontTextureID;
GLuint backTextureID;

// Procedural texture generator (Grayscale patterns)
void generateTextures() {
    GLubyte frontData[64][64][4];
    GLubyte backData[64][64][4];

    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++) {
            // Front Texture: Grayscale Checkerboard
            int checker = ((((i & 0x8) == 0) ^ ((j & 0x8) == 0))) ? 255 : 128;
            frontData[i][j][0] = (GLubyte)checker;
            frontData[i][j][1] = (GLubyte)checker;
            frontData[i][j][2] = (GLubyte)checker;
            frontData[i][j][3] = 255;

            // Back Texture: Grayscale Horizontal Stripes
            int stripes = (i % 16 < 8) ? 255 : 100;
            backData[i][j][0] = (GLubyte)stripes;
            backData[i][j][1] = (GLubyte)stripes;
            backData[i][j][2] = (GLubyte)stripes;
            backData[i][j][3] = 255;
        }
    }

    // Upload Front Texture
    glGenTextures(1, &frontTextureID);
    glBindTexture(GL_TEXTURE_2D, frontTextureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, frontData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Upload Back Texture
    glGenTextures(1, &backTextureID);
    glBindTexture(GL_TEXTURE_2D, backTextureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, backData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void init() {
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    // Initialize procedural textures
    generateTextures();

    // Setup Lighting
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE); // CRITICAL: Enables back-face lighting calculations

    // Position light slightly in front of the camera path
    GLfloat lightPos[] = { 0.0f, 0.0f, 2.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);

    // Front Material: Pure Green Shading, Alpha = 1.0 (The Front Flag)
    GLfloat frontMat[] = { 0.0f, 1.0f, 0.0f, 1.0f };
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, frontMat);

    // Back Material: Pure Red Shading, Alpha = 0.0 (The Back Flag)
    GLfloat backMat[] = { 1.0f, 0.0f, 0.0f, 0.0f };
    glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, backMat);
}

void display() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.0f);
    glRotatef(rotationAngle, 0.0f, 1.0f, 0.0f); // Spin around Y-axis to show front/back

    // ----------------------------------------------------
    // CONFIGURE FIXED FUNCTION PIPELINE COMBINERS
    // ----------------------------------------------------

    // UNIT 0: Sample Front Texture
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, frontTextureID);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); // Pass raw front texture to next unit

    // UNIT 1: Sample Back Texture & Interpolate based on Face Material Alpha
    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, backTextureID);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);

    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);      // Arg 0: Front Texture (From Unit 0)
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);       // Arg 1: Back Texture (Bound to Unit 1)
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR); // Arg 2: Switch determined by lighting Alpha
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);

    // UNIT 2: Modulate the chosen texture with the computed Lighting Color
    glActiveTexture(GL_TEXTURE2);
    glEnable(GL_TEXTURE_2D);
    // Note: We must bind a dummy texture here so the driver treats the unit as active,
    // but we won't sample from it.
    glBindTexture(GL_TEXTURE_2D, frontTextureID);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);

    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);      // Arg 0: Mixed Texture out of Unit 1
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR); // Arg 1: Lighting RGB (Green or Red)
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

    // ----------------------------------------------------
    // RENDER GEOMETRY
    // ----------------------------------------------------
    glBegin(GL_QUADS);
        // Geometric Normal handles lighting orientation automatically
        glNormal3f(0.0f, 0.0f, 1.0f);

        // Immediate-mode texture coordinates apply cleanly to all active units simultaneously
        glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f, -1.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.0f,  1.0f, 0.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f,  1.0f, 0.0f);
    glEnd();

    // Reset multitexture state to avoid leaks
    glActiveTexture(GL_TEXTURE2); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0); glDisable(GL_TEXTURE_2D);

    glutSwapBuffers();
}

void idle() {
    rotationAngle += 0.2f; // Smoothly spin the quad
    if (rotationAngle > 360.0f) rotationAngle -= 360.0f;
    glutPostRedisplay();
}

void reshape(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
}

int main(int argc, char** argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(800, 600);
    glutCreateWindow("Fixed-Function One-Pass Face Texturing");

    // Initialize GLEW to load glActiveTexture extension pointers safely
    if (glewInit() != GLEW_OK) {
        printf("Failed to initialize GLEW\n");
        return -1;
    }

    init();

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);

    glutMainLoop();
    return 0;
}
```

## Open questions

1. **Texture lifecycle owner.** Lazily create on first Color + Texture frame
   and free in the render3d GL teardown, mirroring how the executor's shared
   quadric/tessellator are owned? Or eagerly at `glr_ctrl_init_gl`? Lazy
   avoids paying for textures users never enable.
2. **Glyph source.** Ship a minimal embedded 5×7 bitmap font for the
   FRONT/BACK stamp (self-contained, no dependency), vs. reusing any existing
   bitmap-font asset. The REPL's on-screen text is GLUT bitmap fonts (not
   sampleable into a texture), so a tiny local glyph table is the likely
   answer.
3. **Animation side effects across two walks.** Color + Texture issues two
   geometry walks per frame; `scene_execute_adapter` must ensure assignment
   animation (`t = t + 1`, scratch writes) advances exactly once per visible
   frame, not twice. Decide between bracketing the second walk
   (snapshot/restore) or making both walks non-mutating with a single
   external advance.
4. **Texgen space.** Eye-linear (decided - text fixed to camera, geometry
   slides through) vs. object-linear (text pinned to the surface). Revisit
   only if eye-linear reads poorly on real scenes.
5. **Interaction with the existing `winding_state_filter`.** Confirm the
   filter still suppresses exactly the right set when a texture is bound
   (it should - texturing is pass-owned GL state, and the program emits no
   texture commands today).
