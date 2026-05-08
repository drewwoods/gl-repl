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
#include "repl_core.h"
#include "repl_executor.h"
#include "repl_export.h"
#include "repl_pipeline.h"
#include "scene/render.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "replay_ui_hud.h"
#include "editor_input.h"
#include "editor_search.h"

#endif /* SAMPLE_H */
