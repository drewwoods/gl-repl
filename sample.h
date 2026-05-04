/*
 * sample.h - Shared types, macros, extern globals, and utility declarations
 *
 * Common header for the OpenGL REPL split across sample.c, scene_render.c,
 * scene_grid.c, scene_axes.c, and ui_panels.c.
 */
#ifndef SAMPLE_H
#define SAMPLE_H

#include <gl_includes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "repl_command.h"
#include "repl_command_spec.h"
#include "repl_presentation.h"
#include "replay.h"
#include "editor_search.h"

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

#define MAX_INPUT_LEN   1024
#define MAX_AC_MATCHES  10
#define MAX_WORKSPACE_HEADER_LINES 48
#define WORKSPACE_HEADER_LINE_LEN   96

#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13
#define LINE_H          18
#define CODE_MARGIN_X   10
#define CODE_MARGIN_Y   8

#define MAX_ACCUM_SAMPLES 16
#define ACCUM_STEP_COUNT  5
typedef enum {
    CODE_PANEL_LAYOUT_LEFT = 0,
    CODE_PANEL_LAYOUT_TOP,
    CODE_PANEL_LAYOUT_BOTTOM,
    CODE_PANEL_LAYOUT_HIDDEN,
    CODE_PANEL_LAYOUT_COUNT
} CodePanelLayout;

/* Default values for runtime-configurable state.
 * Used at both the variable definition site and in repl_reset_state() so the
 * two cannot drift.  Add a new entry here whenever a global's default appears
 * in more than one place. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_TOP
#define CFG_DEFAULT_PANEL_FRAC        0.45f

/* Shared by live scene overlays and generated output.c outline passes. */
#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)
#define TESS_VERT_BUF_SIZE 256
#define CAM_LINE_COUNT 4
#define RENDER_STATE_LINE_COUNT 3

/*
 * Replay HUD layout shared by scene_render.c and ui_panels.c.
 * Keep replay HUD drawing and variable-panel replay lift anchored to this
 * single geometry contract.
 */
#define REPLAY_HUD_MARGIN_X      18
#define REPLAY_HUD_MARGIN_Y      18
#define REPLAY_HUD_MIN_WIDTH     220
#define REPLAY_HUD_HEIGHT        56
/* y positions measured from hud_y (bottom edge), top-to-bottom:
 *   line1 (status)   @ 36 - icon row, above progress
 *   progress groove  @ 22
 *   line2 (kbd)      @  4 */
#define REPLAY_HUD_PROGRESS_Y    22
#define REPLAY_HUD_PROGRESS_H     6
#define REPLAY_HUD_TEXT_PAD_X    10
#define REPLAY_HUD_TEXT_LINE1_Y  36
#define REPLAY_HUD_TEXT_LINE2_Y   4
#define REPLAY_HUD_BOTTOM_Y (REPLAY_HUD_MARGIN_Y + REPLAY_HUD_HEIGHT)

/* Height of the amber status strip along the bottom of the scene - used by
 * both ui_panels.c (var panel lift, code panel statusbar) and scene_render.c
 * (replay HUD lift) so the HUD clears the strip. */
#define STATUSBAR_H 22

/* Shared UI accent palette - kept here so menubar (ui_panels.c) and HUD
 * (scene_render.c) use identical values.  #6fb36f is the design-bundle
 * green used for the Replay button, progress fill, and active example. */
#define UI_ACCENT_GREEN_R 0.435f
#define UI_ACCENT_GREEN_G 0.702f
#define UI_ACCENT_GREEN_B 0.435f

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

typedef struct {
    GLdouble pos[3];
    GLdouble normal[3]; /* per-vertex normal, default (0,0,1) */
    GLdouble color[4];  /* per-vertex RGBA,   default (1,1,1,1) */
} TessVertex;

/* ========================================================================= */
/* Types                                                                      */
/* ========================================================================= */

typedef enum {
    PROFILE_PANEL_OFF = 0,
    PROFILE_PANEL_ON,
    PROFILE_PANEL_DETAILS,
    PROFILE_PANEL_MODE_COUNT
} ProfilePanelMode;

int  init_section_line_count(void);
void init_section_line(int i, char *buf, size_t n);

void editor_state_autocomplete_clear(void);

static inline void clear_autocomplete_state(void) {
    editor_state_autocomplete_clear();
}

/* ========================================================================= */
/* Shared utility functions                                                   */
/* ========================================================================= */

void set_status(const char *msg);
const char *mode_name(GLenum mode);
GLenum current_begin_mode(void);
int  count_vertices(void);
void mark_normals_dirty(void);

void navigate_to_line(int target);
void execute_commands(void);
void flatten_commands(void);
void recompute_autonormals(void);
void update_cam_lines(void);

#endif /* SAMPLE_H */
