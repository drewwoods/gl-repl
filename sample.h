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
#include "editor_limits.h"
#include "repl_export_state.h"
#include "scene_render.h"
#include "ui_layout.h"
#include "ui_metrics.h"
#include "ui_replay_hud.h"
#include "editor_search.h"

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* ========================================================================= */
/* Types                                                                      */
/* ========================================================================= */

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
