/*
 * src/repl/export.h - Save/load of REPL sessions to/from C source files.
 *
 * Bidirectional text format for persisting complete REPL state (commands,
 * variables, camera, settings, workspace directory) as standalone C files.
 * Files round-trip cleanly: saving and re-loading preserves all state.
 *
 * Export format (save_output): Writes a complete C file with:
 *   1. A file-only generated banner with standalone compile hints.
 *   2. Header comments with workspace metadata (@var name=value, @cfg setting=value,
 *      @scene-name <name>, @workspace-dir <path>). Used by import to restore context.
 *   3. Flattened system GL/GLUT includes, without depending on gl_includes.h.
 *   4. Global variable declarations for user-defined predefined variables (float x, y, z).
 *   5. Camera state as the raw glTranslatef/glRotatef sequence the REPL uses internally
 *      (not a pose matrix — the exact command history).
 *   6. REPL function definitions converted to C function syntax (for reloading as
 *      CMD_FUNC_DEF on import).
 *   7. Geometry commands in the display() function body (user-edited commands).
 *
 * Import format (load_from_file): Line-by-line scan that:
 *   1. Parses leading workspace header directives (@var, @cfg, @scene-name, @workspace-dir).
 *   2. Extracts camera state (raw glTranslatef/glRotatef lines) via the
 *      controller-installed camera bridge.
 *   3. Detects function definitions (lines matching C function syntax).
 *   4. Feeds remaining geometry lines through repl_load_apply_line() — the
 *      lean non-editor source-load path — for normal parsing.
 *   5. Stores pending scene-name and workspace-dir for the caller to apply after loading.
 *
 * Header templates: g_header_pre/post and g_footer_pre/post_init are boilerplate
 * segments (includes, setup, display() signature, cleanup). Inserted around the
 * exported code to create a valid C program.
 *
 * Workspace integration: repl_state_refresh_workspace_header_lines() rebuilds the
 * export header text from current state. repl_state_parse_workspace_header_line()
 * consumes a single header directive during import and stores pending scene/workspace
 * metadata for the caller to apply after load_from_file() returns.
 *
 * Multi-scene export lives in src/repl/scenes.c: workspace saves iterate user-scene
 * slots, set the export scene-name hint, and call repl_export_save_output() for
 * each scene.
 */
#ifndef REPL_EXPORT_H
#define REPL_EXPORT_H

#include <stdio.h>
#include "source_document.h"
#include "repl/cfg_baseline.h"
#include "repl/export_state.h"  /* REPL_EXPORT_CAMERA_LINES / _LINE_MAX */
#include "repl/state_views.h"   /* REPL_WORKSPACE_DIR_MAX, USER_SCENE_NAME_MAX */

/* Apply the current batch of parsed @cfg lines, then clear the accumulator.
 * Importers call this after finishing a header batch; example loading uses the
 * same drain after consuming leading example metadata. The per-line parser may
 * already have applied each cfg immediately, so this is primarily the
 * batch-shaped handoff for callers that want the full bag at once. */
void repl_export_apply_pending_cfg(void);

/* Opaque 4-line camera transform block used by export, import, and examples.
 *
 * Export writes these raw GL lines into saved files and the code-panel preview;
 * import and example loading feed the same text back to the controller bridge.
 * src/repl/export.c treats the block as opaque strings, while the bridge in
 * src/app/glr_camera_export.c owns the actual camera parsing and mutation
 * (introduced in step 4a of the decouple plan).
 *
 * REPL_EXPORT_CAMERA_LINES / _LINE_MAX live in export_state.h (included
 * above) so the same constants dimension the cam_lines[] preview storage
 * in state_views.h. */
#define REPL_EXPORT_CAMERA_PREAMBLE_MAX 64

typedef struct {
    char lines[REPL_EXPORT_CAMERA_LINES][REPL_EXPORT_CAMERA_LINE_MAX];
    int  present;
} ReplExportCameraBlock;

/* Controller-installed adapter for camera block save/load.
 *
 * Save paths call fill_save_block() and fill_save_preamble(); the code-panel
 * preview calls fill_display_block(); file import uses reset_import() plus
 * try_consume_import_line() to stream camera lines back into live state; the
 * example loader can call apply_example_block() after validating a `// camera`
 * header. The bridge is installed from src/app/glr_camera_export.c and is
 * intentionally optional so non-rendering hosts can leave camera handling out. */
typedef struct {
    /* Fill block for saved-file emission. Line 3 uses the literal
     * "glRotatef(g_angle, 0,1,0)" so the saved file animates via
     * the file-scope g_angle variable. */
    void (*fill_save_block)(ReplExportCameraBlock *block);
    /* Fill block for the in-app code-panel preview. Line 3 uses the
     * numeric ry value (no g_angle placeholder — the preview shows
     * the current state, not the animation hook). */
    void (*fill_display_block)(ReplExportCameraBlock *block);
    /* Build the "static float g_angle = N.NNNNf;" preamble line for
     * the saved file's header. */
    void (*fill_save_preamble)(char *out, int out_sz);
    /* Try to consume a single import line as part of the camera
     * block (or its g_angle preamble). Returns 1 if consumed
     * (applied to camera state), 0 otherwise. The bridge owns the
     * stateful per-line parser. */
    int  (*try_consume_import_line)(const char *line);
    /* Reset import-side parser state at the start of each load. */
    void (*reset_import)(void);
    /* Apply a validated example camera block. This is separate from
     * import-line consumption so app bridges can animate example
     * switches while save/workspace imports still restore immediately. */
    void (*apply_example_block)(const ReplExportCameraBlock *block);
    /* Snap-apply a captured camera block to live state — used by the
     * workspace-save iteration to stage each slot's saved camera
     * before the export bridge reads the live state, and by
     * stash/restore around the iteration. Unlike apply_example_block
     * this does NOT ease, does NOT set the scene default, and does
     * NOT record an external 3D pose — it's the symmetric inverse of
     * fill_display_block. */
    void (*apply_capture_block_snap)(const ReplExportCameraBlock *block);
} ReplExportCameraBridge;

/* Install or read the process-wide camera bridge used by export/import and
 * example camera headers. Callers normally install once at app startup. */
void                          repl_export_install_camera_bridge(const ReplExportCameraBridge *bridge);
const ReplExportCameraBridge *repl_export_camera_bridge(void);

/* Neutral reshape-projection seam (same shape as the camera bridge).
 *
 * The exported reshape() body between glLoadIdentity() and
 * glMatrixMode(GL_MODELVIEW) is dynamic: it must reproduce whatever
 * projection the scene is currently applying (perspective in 3D, ortho
 * in 2D). src/repl/export.c is GL-free, so the controller installs a
 * bridge whose implementation reads scene_get_active_projection() and
 * formats the lines. The g_footer_pre_init slot for those lines holds
 * REPL_EXPORT_RESHAPE_PROJ_SENTINEL; every consumer (file writer and the
 * live code panel) expands it through repl_export_reshape_projection_lines()
 * so the saved file and the panel always agree. No bridge installed (the
 * scene_demo, tests) => the canonical perspective default. */
#define REPL_EXPORT_PROJ_LINES    4
#define REPL_EXPORT_PROJ_LINE_MAX 96
#define REPL_EXPORT_RESHAPE_PROJ_SENTINEL "\x01@reshape-projection"

typedef struct {
    char lines[REPL_EXPORT_PROJ_LINES][REPL_EXPORT_PROJ_LINE_MAX];
    int  count;
} ReplExportProjectionBlock;

typedef struct {
    void (*fill_reshape_block)(ReplExportProjectionBlock *block);
} ReplExportProjectionBridge;

void                              repl_export_install_projection_bridge(const ReplExportProjectionBridge *bridge);

/* Neutral light seam (same install shape as the camera/projection bridges).
 *
 * The exported init()/display() bodies emit one glLightfv block per light
 * slot, but the dimensional light data (positions / colors / eye-space) is
 * presentation state owned by the app shell (GlrRenderState.lights, seeded
 * from a scene light theme). src/repl/export.c is scene/app-free, so the
 * controller installs a bridge that copies the live per-slot values into this
 * neutral float struct. No bridge installed (scene_demo, tests) => the
 * exporter emits zeroed/disabled lights. This carries only the dimensional
 * fields; whether a slot is *enabled* is decided by the program's own
 * glEnable(GL_LIGHTn) in display(), so the export bootstrap disables every
 * slot regardless and never consults this. */
typedef struct {
    float pos[4];
    float diffuse[4];
    float ambient[4];
    float specular[4];
    int   pos_is_eye_space;
} ReplExportLightInfo;

typedef struct {
    void (*fill_slot)(int slot, ReplExportLightInfo *out);
} ReplExportLightBridge;

void                         repl_export_install_light_bridge(const ReplExportLightBridge *bridge);
const ReplExportLightBridge *repl_export_light_bridge(void);

/* Resolve the reshape projection lines: the installed bridge, else the
 * canonical perspective default. Fills up to REPL_EXPORT_PROJ_LINES
 * pointers into out[] and returns the count. Returned pointers reference
 * internal static storage valid until the next call — emit/copy them
 * before calling again. Main-thread only. */
int repl_export_reshape_projection_lines(const char *out[REPL_EXPORT_PROJ_LINES]);

/* Boilerplate C file segments for export. g_header_pre is the file-scope
 * preamble (includes, macros, rotation globals). g_display_header opens
 * the display() function (`void display(void) { ...clear/load/push...`) and
 * is shared verbatim by the code panel and emit_export_display_begin so
 * the two stay in sync. g_header_post follows the dynamic state lines
 * inside display(); g_footer_pre_init / g_footer_post_init bracket the
 * init() function. Together they form a valid C program. */
/* The literal that opens the exported display() function. Used as
 * g_display_header[0] and as the search needle from core.c's
 * scroll_to_display_function — exposed via this macro so both sides
 * stay in sync if the line text ever changes. */
#define REPL_EXPORT_DISPLAY_OPEN_LINE "void display(void) {"
extern const char  *g_header_pre[];
extern const char  *g_display_header[];
extern const char  *g_header_post[];
extern const char  *g_footer_pre_init[];
extern const char  *g_footer_post_init[];

/* Stringify a macro value (compose-time only). Used to inject
 * REPL_SCRATCH_ARRAY_LEN into the literal scratch-decl line shown in
 * the live editor + exported file, so changing the array length
 * doesn't require hand-editing this string. */
#ifndef REPL_EXPORT_STRINGIFY
#define REPL_EXPORT_STRINGIFY2(x) #x
#define REPL_EXPORT_STRINGIFY(x)  REPL_EXPORT_STRINGIFY2(x)
#endif
#define REPL_CODE_PANEL_SCRATCH_DECL_LINE \
    "  float " \
    "A[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "], " \
    "B[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "], " \
    "C[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "];"

/* Controller-owned layout snapshot used by export.
 *
 * The exporter needs scene geometry to preserve aspect ratio in saved output,
 * but it should not query UI or app state directly. Callers compute the
 * numbers once per export and pass them in here as opaque integers. This data
 * flow was moved out of hidden ui/glr state reads (implemented in step 7c of
 * feature/decouple-repl-from-gl-repl-alt.md).
 *
 * Callers that do not have a viewport on hand (LRU evict in
 * src/repl/scenes.c, headless tests, the standalone repl_demo which does
 * not export) may pass NULL. The export call sites then use
 * defensive defaults: scene_w / scene_h fall back to 800x600 in the
 * exported display() boilerplate. */
struct ReplExportLayout {
    int scene_w;
    int scene_h;
};
#ifndef REPL_EXPORT_LAYOUT_DECLARED
#define REPL_EXPORT_LAYOUT_DECLARED
typedef struct ReplExportLayout ReplExportLayout;
#endif

/* Export current REPL state to a C source file. Writes header metadata (@var, @cfg,
 * @scene-name, @workspace-dir), global variable declarations, camera state, function
 * definitions, and geometry commands to filename. The file is a complete, standalone
 * C program that can be reloaded via load_from_file(). Called by save-to-output and
 * workspace export routines.
 *
 * `text` is the source-text view the caller built; the export
 * pipeline reads source text exclusively through that view rather
 * than reaching into globals. `layout` carries the viewport /
 * scene rect / code-panel width / wrap toggle the controller built;
 * `src/repl/export.c` consumes it as opaque integers. */
int repl_export_save_output(const char *filename, SourceTextView text,
                            const ReplExportLayout *layout);

/* Code-panel dumps used by debug output and test fixtures. */
void repl_dump_code_panel_text(FILE *out, SourceTextView text);

/* Caller-supplied scratch for repl_export_load_from_file to populate
 * with the metadata it parsed (or empty strings if the file didn't
 * carry the corresponding directives). The buffers stay valid for the
 * caller; nothing inside the import path retains a pointer past
 * return. Pass NULL to drop the metadata. */
typedef struct {
    char scene_name[USER_SCENE_NAME_MAX];
    char workspace_dir[REPL_WORKSPACE_DIR_MAX];
} ReplImportResult;

/* Import a C source file saved by save_output(). Parses workspace header directives,
 * camera state, function definitions, and geometry commands. Feeds geometry lines
 * through repl_load_apply_line() (the lean non-editor source-load path from step
 * 5b) for normal parsing. Returns 1 on success, 0 on error (parse failure, open
 * failure). When `result` is non-NULL, the parsed scene-name and workspace-dir
 * directives (if any) are copied into it before return; pass NULL to drop them. */
int  repl_export_load_from_file(const char *filename, ReplImportResult *result);

/* Refresh the export header text from current state. Pre-builds the header metadata
 * lines (@var, @cfg, @scene-name, @workspace-dir) from the current predefined
 * variables, render settings, and workspace directory. Called after mutations
 * (variable declare, config toggle, workspace directory change) to keep the export
 * buffer in sync. */
void repl_state_refresh_workspace_header_lines(void);

/* Access the init-section boilerplate lines rendered into the code panel and
 * exported output. */
int  repl_export_init_section_line_count(void);
void repl_export_init_section_line(int i, char *buf, size_t n);

/* Light-setup text lines. Init lines (per-light DIFFUSE/AMBIENT/SPECULAR +
 * baseline glDisable) belong in the init() section. Display lines (POSITION)
 * belong in display() after the camera transforms — glLightfv(GL_POSITION)
 * snapshots the active modelview, so positions set before the camera would
 * not orbit with the scene. The editor's code panel renders these verbatim
 * and the exported C file writes the same text, so a single generator
 * keeps the two surfaces in lockstep. */
int  repl_export_lights_init_line_count(void);
void repl_export_lights_init_line(int i, char *buf, size_t n);
int  repl_export_lights_display_line_count(void);
void repl_export_lights_display_line(int i, char *buf, size_t n);

#endif
