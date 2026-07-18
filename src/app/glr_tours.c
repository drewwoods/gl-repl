/*
 * glr_tours.c - the built-in guided-tour catalog (see glr_tours.h).
 *
 * Each tour is an array of pointer-script grammar lines (the same format
 * the .pointer files under scripts/video/ use — src/app/glr_pointer_script.h).
 * Times are seconds on the rendered-frame clock. Keep events time-sorted;
 * glr_pointer_script_start_lines rejects out-of-order lines and the catalog
 * test parses every tour, so an authoring slip fails fast.
 *
 * Every point below is a SYMBOLIC target (menu:/item:/subenter:/sub:/pin:/
 * scene: — grammar in glr_pointer_script.h), resolved against the live
 * layout when the event fires, so tours work at any window size and track
 * catalog/label reordering. The caption band anchors at scene:0.25,0.76:
 * inside the scene viewport, above the replay HUD / status bar, and (at
 * <= 40 caption chars) clear of the variable panel's right-hand column.
 */
#include "app/glr_tours.h"
#include "app/glr_pointer_script.h"
#include "repl/host_effects.h"

#include <stdio.h>

/* -- Menus & Examples ---------------------------------------------------- */
/* Menu walk mirroring scripts/video/menu-tour.pointer: browse the Scene
 * example flyouts, load the Whale showcase, peek at Tutorials, toggle a
 * Config overlay (and toggle it back), then run Replay on the result. */
static const char *const k_tour_menus[] = {
    "0.2   move scene:0.55,0.30",
    "0.3   echo scene:0.25,0.76 18 3.2 Welcome! This tour drives gl-repl.",
    "3.6   echo scene:0.25,0.76 18 2.8 Press any key or click to stop it.",
    "6.6   echo scene:0.25,0.76 18 3.6 The Scene menu holds the examples.",
    "6.7   glide menu:scene 0.9",  /* up to the Scene menu */
    "7.9   click",
    "8.5   glide item:all 0.7",    /* hover All (every example, grouped) */
    "9.5   glide item:3d 0.6",     /* hover 3D -> examples flyout opens */
    "10.5  glide subenter:3d 0.5", /* straight right into the flyout */
    "10.6  echo scene:0.25,0.76 18 3.2 Hover a tag; slide into its flyout.",
    "11.2  glide sub:3d:animated_ring 0.5",  /* hover highlight follows */
    "12.0  glide sub:3d:torus_knot 0.7",
    "12.8  ring sub:3d:torus_knot 1.2",      /* spotlight it */
    "14.2  glide sub:3d:whale 1.1",          /* down to the showcase */
    "15.4  echo scene:0.25,0.76 18 2.6 Click loads the example.",
    "15.6  click",                 /* load it; camera eases in */
    "19.6  echo scene:0.25,0.76 18 3.4 Tutorials teach step by step.",
    "19.8  glide menu:tutorials 1.0",
    "21.0  click",
    "21.6  glide item:geometry 0.7",        /* hover -> tutorial flyout */
    "22.6  glide subenter:geometry 0.4",
    "23.2  glide sub:geometry:first_triangle 0.5",
    "23.8  ring sub:geometry:first_triangle 1.6",  /* don't start it */
    "25.8  echo scene:0.25,0.76 18 3.6 Config: grids, themes, overlays.",
    "26.0  glide menu:config 0.8",
    "27.0  click",
    "27.6  glide item:rendering 0.6",
    "28.6  glide item:overlays 0.7",        /* walk down to Overlays */
    "29.6  glide subenter:overlays 0.4",
    "30.2  glide sub:overlays:vertex_points 0.5",
    "31.0  ring sub:overlays:vertex_points 1.2",
    "32.4  click",                 /* Vertex points ON (whale lights up) */
    "32.6  echo scene:0.25,0.76 18 2.8 Toggles apply live.",
    "35.4  click",                 /* ...and back OFF */
    "36.2  click scene:0.55,0.30", /* dismiss the menu */
    "37.2  echo scene:0.25,0.76 18 3.2 Replay rebuilds the scene call by call.",
    "37.4  glide pin:replay 1.2",
    "38.8  click",                 /* start replay */
    "39.6  glide scene:0.70,0.40 1.0",  /* park; watch it build */
    "46.0  key \\cr",              /* Ctrl+R exits the replay (the pin
                                    * click would only pause it) */
    "47.0  glide scene:0.55,0.30 0.8",
    "47.8  echo scene:0.25,0.76 18 3.6 Tour finished - it's all yours!",
};

/* -- Editing Basics ------------------------------------------------------ */
/* File > New Scene, then type a spinning triangle ringed by a torus,
 * committing each line with `;` — the same keystrokes a user would make. */
static const char *const k_tour_editing[] = {
    "0.2   move scene:0.55,0.30",
    "0.3   echo scene:0.25,0.76 18 3.0 Let's build a scene from scratch.",
    "3.4   glide menu:file 0.8",       /* the File menu */
    "4.4   click",
    "5.0   glide item:new_scene 0.5",       /* New Scene row */
    "5.8   click",
    "6.6   echo scene:0.25,0.76 18 3.4 A new scene seeds the GL boilerplate.",
    "10.2  echo scene:0.25,0.76 18 3.4 Type a command; the ; key commits it.",
    "10.6  key glRotatef(t * 40, 0, 1, 0)",
    "12.2  key ;",
    "12.6  echo scene:0.25,0.76 18 3.0 t is the animation clock (Ctrl+T).",
    "13.4  key glBegin(GL_TRIANGLES)",
    "14.6  key ;",
    "15.2  key glColor3f(1, 0.42, 0.3)",
    "16.4  key ;",
    "16.8  key glVertex3f(-1, -0.7, 0)",
    "18.0  key ;",
    "18.4  key glColor3f(0.35, 0.85, 0.55)",
    "19.6  key ;",
    "20.0  key glVertex3f(1, -0.7, 0)",
    "21.2  key ;",
    "21.6  key glColor3f(0.45, 0.6, 1)",
    "22.8  key ;",
    "23.2  key glVertex3f(0, 1.1, 0)",
    "24.4  key ;",
    "24.8  key glEnd()",
    "25.6  key ;",
    "26.4  echo scene:0.25,0.76 18 3.6 The gray ghost is autocomplete (Tab).",
    "26.8  key glutSolidT",        /* pause here so the ghost shows */
    "29.0  key orus(0.25, 1.4, 16, 32)",
    "30.4  key ;",
    "31.4  echo scene:0.25,0.76 18 4.0 Lines re-run every frame. Undo: Ctrl+Z.",
    "35.6  echo scene:0.25,0.76 18 3.6 Tour finished - it's all yours!",
};

/* -- Camera & Views ------------------------------------------------------ */
/* Orbit drags in the scene (scripted press + glide routes through the real
 * drag handlers), wheel zoom, and the focus-origin ease. */
static const char *const k_tour_camera[] = {
    "0.2   move scene:0.58,0.30",
    "0.3   echo scene:0.25,0.76 18 3.2 Drag anywhere in the scene to orbit.",
    "3.6   down scene:0.58,0.30",
    "3.8   glide scene:0.80,0.12 1.6",
    "5.6   up",
    "6.2   down",
    "6.4   glide scene:0.50,0.42 1.2",
    "7.8   up",
    "8.4   echo scene:0.25,0.76 18 2.8 The mouse wheel zooms.",
    "8.8   wheel 1",
    "9.2   wheel 1",
    "9.6   wheel 1",
    "10.0  wheel 1",
    "11.0  wheel -1",
    "11.4  wheel -1",
    "11.8  wheel -1",
    "12.2  wheel -1",
    "13.0  echo scene:0.25,0.76 18 3.4 Ctrl+O focuses the origin.",
    "13.6  key \\co",
    "16.6  echo scene:0.25,0.76 18 3.6 Ctrl+Shift+C resets the camera.",
    "20.4  echo scene:0.25,0.76 18 3.6 Tour finished - it's all yours!",
};

typedef struct {
    const char        *name;
    const char *const *lines;
    int                line_count;
} TourEntry;

#define TOUR(name, arr) { name, arr, (int)(sizeof(arr) / sizeof((arr)[0])) }

static const TourEntry g_tours[] = {
    TOUR("Menus & Examples", k_tour_menus),
    TOUR("Editing Basics",   k_tour_editing),
    TOUR("Camera & Views",   k_tour_camera),
};

int glr_tours_count(void) {
    return (int)(sizeof(g_tours) / sizeof(g_tours[0]));
}

const char *glr_tours_name(int idx) {
    if (idx < 0 || idx >= glr_tours_count()) return NULL;
    return g_tours[idx].name;
}

int glr_tours_start(int idx) {
    if (idx < 0 || idx >= glr_tours_count()) return 0;
    if (!glr_pointer_script_start_lines(g_tours[idx].lines,
                                        g_tours[idx].line_count)) {
        repl_set_status_error("Tour script failed to load");
        return 0;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Tour: %s  (any key or click stops it)",
             g_tours[idx].name);
    repl_set_status(msg);
    return 1;
}
