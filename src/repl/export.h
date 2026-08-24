/*
 * src/repl/export.h - Save/load of REPL sessions to/from C source files.
 *
 * Bidirectional text format for persisting complete REPL state (commands,
 * variables, camera, settings, workspace directory) as standalone C files.
 * Files round-trip cleanly: saving and re-loading preserves all state.
 *
 * Export format (save_output): Writes a complete C file with:
 *   1. A file-only generated banner with standalone compile hints.
 *   2. Header comments with workspace metadata (@cfg setting=value,
 *      @scene-name <name>, @workspace-dir <path>). Used by import to restore context.
 *   3. Flattened system GL/GLUT includes, without depending on gl_includes.h.
 *   4. Global variable declarations for user-defined predefined variables (float x, y, z).
 *   5. Camera state as the raw glTranslatef/glRotatef sequence the REPL uses
 *      internally (not a pose matrix - the exact command history), each row
 *      tagged with its `@camera` role so import can recognise it by tag
 *      rather than by position.
 *   6. REPL function definitions converted to C function syntax (for reloading as
 *      CMD_FUNC_DEF on import).
 *   7. Geometry commands in the display() function body (user-edited commands).
 *
 * Import format (load_from_file): Line-by-line scan that:
 *   1. Parses leading workspace header directives (@cfg, @scene-name, @workspace-dir).
 *   2. Extracts camera state from the `@camera`-tagged transform rows via the
 *      shared reader in src/repl/camera_header.c, and applies one resolved
 *      pose through the controller-installed camera bridge at end of load.
 *   3. Detects function definitions (lines matching C function syntax).
 *   4. Feeds remaining geometry lines through repl_load_apply_line() - the
 *      lean non-editor source-load path - for normal parsing.
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
#include "repl/camera_header.h"  /* ReplCameraPose / ReplCameraApplyMode */
#include "repl/cfg_baseline.h"
#include "repl/export_state.h"  /* REPL_EXPORT_CAMERA_LINES / _LINE_MAX */
#include "repl/state_views.h"   /* REPL_WORKSPACE_DIR_MAX, USER_SCENE_NAME_MAX */

/* Apply the current batch of parsed @cfg lines, then clear the accumulator.
 * Importers call this after finishing a header batch; example loading uses the
 * same drain after consuming leading example metadata. The per-line parser may
 * already have applied each cfg immediately, so this is primarily the
 * batch-shaped handoff for callers that want the full bag at once. */
void repl_export_apply_pending_cfg(void);

/* Opaque camera transform block used by export and the code-panel preview.
 *
 * The bridge in src/app/glr_camera_export.c formats these raw GL lines from
 * live camera state; src/repl/export.c treats them as opaque strings. Nothing
 * parses them any more - reading a camera back is src/repl/camera_header.c's
 * job, from the file's own tagged lines.
 *
 * REPL_EXPORT_CAMERA_LINES / _LINE_MAX live in export_state.h (included
 * above) so the same constants dimension the cam_lines[] preview storage
 * in state_views.h. Slot 3 is the `spin` animation hook and is empty in the
 * hook-less projection; every consumer skips empty slots. */
#define REPL_EXPORT_CAMERA_PREAMBLE_MAX 64

typedef struct {
    char lines[REPL_EXPORT_CAMERA_LINES][REPL_EXPORT_CAMERA_LINE_MAX];
    int  present;
} ReplExportCameraBlock;

/* Controller-installed camera adapter. It formats and it applies; it does
 * not parse.
 *
 * `fill_block` is one formatter with two projections that differ by one
 * optional row: `with_anim_hook` emits the `@camera spin` row carrying the
 * exported C's g_angle animation hook, and is set only for exported C. The
 * `.glr` writer and the code panel pass 0 - a g_angle reference is not a
 * REPL identifier and has no business in either.
 *
 * The bridge is installed from src/app/glr_camera_export.c and is
 * intentionally optional so non-rendering hosts can leave camera handling
 * out; a header then loads, validates and diagnoses as usual but applies
 * nothing, and the scene inherits the live camera. */
typedef struct {
    void (*fill_block)(ReplExportCameraBlock *block, int with_anim_hook);
    /* Read the camera's *destination* pose - the inverse of fill_block, and
     * deliberately not the live pose: a load can land mid-ease, and merging
     * a partial header against an interpolated frame would bake an arbitrary
     * step of an animation into the result. */
    void (*capture_pose)(ReplCameraPose *out);
    /* Apply a fully-resolved pose. The reader merges partial headers before
     * calling, so no bridge implementation ever reasons about a mask. The
     * mode carries both the transition and the scene-default decision -
     * see ReplCameraApplyMode. */
    void (*apply_pose)(const ReplCameraPose *pose, ReplCameraApplyMode mode);
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
 * bridge whose implementation reads render3d_get_active_projection() and
 * formats the lines. The g_footer_pre_init slot for those lines holds
 * REPL_EXPORT_RESHAPE_PROJ_SENTINEL; every consumer (file writer and the
 * live code panel) expands it through repl_export_reshape_projection_lines()
 * so the saved file and the panel always agree. No bridge installed (the
 * render3d_demo, tests) => the canonical perspective default. */
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
 * from a render3d light theme). src/repl/export.c is render3d/app-free, so the
 * controller installs a bridge that copies the live per-slot values into this
 * neutral float struct. No bridge installed (render3d_demo, tests) => the
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
 * internal static storage valid until the next call - emit/copy them
 * before calling again. Main-thread only. */
int repl_export_reshape_projection_lines(const char *out[REPL_EXPORT_PROJ_LINES]);

/* Boilerplate C file segments for export. g_header_pre is the file-scope
 * preamble (includes, macros, rotation globals). g_display_header opens
 * the display() function (`void display(void) { ...clear/load/push...`) and
 * is shared verbatim by the code panel and emit_export_display_begin so
 * the two stay in sync. g_header_post follows the camera/light setup inside
 * display(); g_footer_pre_init / g_footer_post_init bracket the init()
 * function. Together they form a valid C program. */
/* The literal that opens the exported display() function. Used as
 * g_display_header[0] and as the search needle from bootstrap.c's
 * scroll_to_display_function - exposed via this macro so both sides
 * stay in sync if the line text ever changes. */
#define REPL_EXPORT_DISPLAY_OPEN_LINE "void display(void) {"
/* The line that closes display(). Like the opener, the code panel draws
 * this one unconditionally - the two together frame the user's body, so
 * code focus hides what is inside them but never the frame itself. */
#define REPL_EXPORT_DISPLAY_CLOSE_LINE "}"

/* Code-focus spelling of the display() opener. Focus mode strips derived-C
 * boilerplate down to what frames the user's own code, and the C return
 * type and `(void)` parameter list are boilerplate; the name and the brace
 * are the frame. The full C spelling is g_display_header[0]. */
#define REPL_EXPORT_DISPLAY_OPEN_FOCUS_LINE "display() {"
/* The same line without its brace. The importer matches on this to signal the
 * camera reader's display region, because a hand-formatted file may put the
 * `{` on its own line - so the signature, not the whole opener, is the part
 * that identifies the function. */
#define REPL_EXPORT_DISPLAY_OPEN_SIGNATURE "void display(void)"
extern const char  *g_header_pre[];
extern const char  *g_display_header[];
extern const char  *g_header_post[];
extern const char  *g_display_footer[];
extern const char  *g_footer_pre_init[];
extern const char  *g_footer_post_init[];

/* <math.h> exposes a few short POSIX function names that are also natural
 * scene-variable names. The header template contains paired wrappers for
 * those names; consumers use this mask + predicate to show only the pairs
 * required by the current predefined-variable table. */
unsigned repl_export_math_collision_mask(void);
int      repl_export_header_pre_line_visible(int line_idx,
                                             unsigned collision_mask);

/* Conditional C89 stand-ins for compound GL vector literals. The mask and
 * line API are shared by standalone export, the expanded live code panel,
 * and --dump-code so all three surfaces show the same helper definitions. */
enum {
    REPL_EXPORT_GL_VECTOR_FLOAT1  = 1u << 0,
    REPL_EXPORT_GL_VECTOR_FLOAT3  = 1u << 1,
    REPL_EXPORT_GL_VECTOR_FLOAT4  = 1u << 2,
    REPL_EXPORT_GL_VECTOR_DOUBLE4 = 1u << 3,
    REPL_EXPORT_GL_VECTOR_FLOAT16 = 1u << 4
};
enum { REPL_EXPORT_GL_VECTOR_HELPER_MAX_LINES = 49 };
unsigned repl_export_gl_vector_helper_mask(void);
int      repl_export_gl_vector_helper_line_count(unsigned mask);
int      repl_export_gl_vector_helper_line(unsigned mask, int line_idx,
                                           char *out, size_t out_size);

/* Stringify a macro value (compose-time only). Used to inject
 * REPL_SCRATCH_ARRAY_LEN into the literal scratch-decl line shown in
 * the live editor + exported file, so changing the array length
 * doesn't require hand-editing this string. */
#ifndef REPL_EXPORT_STRINGIFY
#define REPL_EXPORT_STRINGIFY2(x) #x
#define REPL_EXPORT_STRINGIFY(x)  REPL_EXPORT_STRINGIFY2(x)
#endif
#define REPL_CODE_PANEL_SCRATCH_DECL_LINE \
    "float " \
    "A[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "], " \
    "B[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "], " \
    "C[" REPL_EXPORT_STRINGIFY(REPL_SCRATCH_ARRAY_LEN) "];"

/* Controller-owned layout snapshot used by export.
 *
 * The exporter needs scene geometry to preserve aspect ratio in saved output,
 * but it should not query UI or app state directly. Callers compute the
 * numbers once per export and pass them in here as opaque integers. The
 * exporter therefore has no direct UI or app-state dependency.
 *
 * Callers that do not have a viewport on hand (workspace persistence in
 * src/repl/scenes.c, headless tests, the standalone repl_demo which does
 * not export) may pass NULL. The export call sites then use
 * defensive defaults: render3d_w / render3d_h fall back to 800x600 in the
 * exported display() boilerplate. */
struct ReplExportLayout {
    int render3d_w;
    int render3d_h;
};
#ifndef REPL_EXPORT_LAYOUT_DECLARED
#define REPL_EXPORT_LAYOUT_DECLARED
typedef struct ReplExportLayout ReplExportLayout;
#endif

/* Export current REPL state to a C source file. Writes header metadata (@cfg,
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

/* Write the active user scene (or current example/transient buffer) to
 * ./output.c. This is the default single-file export path behind Ctrl+S when no
 * named scene-specific save target takes over. `layout` is the controller-built
 * ReplExportLayout passed through to the exporter as opaque integers. */
int repl_save_default_output(const ReplExportLayout *layout);

/* Write the scene as a `.glr` source file - the authoring format built-in
 * examples ship in (see src/repl/export_glr.c), not a compilable C program.
 * Content is the non-default `@cfg` rows, the tagged `@camera` transform
 * rows, and the document text in canonical order - declarations, then
 * function definitions, then camera and body. Function-bearing scenes wrap
 * camera and body in explicit `display() { ... }` format syntax; scenes with
 * no functions keep the implicit body. Symmetric with the example loader, so the
 * output can be dropped into examples/scenes/ and referenced from
 * examples/catalog.ini as-is. Returns 1 on success, 0 on write failure
 * (status message set either way). No ReplExportLayout: the format carries
 * no viewport-dependent scaffold. */
int repl_export_save_glr(const char *filename, SourceTextView text);

/* Code-panel dumps used by debug output and test fixtures. */
void repl_dump_code_panel_text(FILE *out, SourceTextView text);

/* --- Reader half (implemented in import.c) ---------------------------------
 *
 * DEFERRED (repl-clarity-review.md finding 8, in docs/plans/partial/): these
 * are the *loader's* entry points, declared in the writer's header under a
 * "Save/load of REPL sessions" banner, with the writer's `repl_export_`
 * prefix and no import.h to find them in. Callers looking for the loader
 * open the wrong file. Renaming to repl_import_load_* behind a thin import.h
 * is explicitly optional and should ride a change that is already opening
 * import.c - it must not lead one. */

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

/* Import from an arbitrary stream by first copying it to an anonymous,
 * seekable temporary file, then running the exact same multi-pass parser as
 * repl_export_load_from_file(). The input stream remains owned by the caller
 * and is left at EOF. `source_name` is used only for diagnostics. */
int  repl_export_load_from_stream(FILE *input, const char *source_name,
                                  ReplImportResult *result);

/* Import an in-memory, NULL-terminated source line array using the same parser
 * as repl_export_load_from_file(). `source_name` is used only for diagnostics.
 * This lets compiled-in/runtime example catalogs carry full exported-C sources
 * without writing them to a temporary file first. */
int  repl_export_load_from_lines(const char *const *lines,
                                 const char *source_name,
                                 ReplImportResult *result);

/* Refresh the export header text from current state. Pre-builds the header metadata
 * lines (@cfg, @scene-name, @workspace-dir) from the current predefined
 * variables, render settings, and workspace directory. Called after mutations
 * (variable declare, config toggle, workspace directory change) to keep the export
 * buffer in sync. */
void repl_state_refresh_workspace_header_lines(void);

/* Access the init-section boilerplate lines rendered into the code panel and
 * exported output. */
int  repl_export_init_section_line_count(void);
void repl_export_init_section_line(int i, char *buf, size_t n);

/* Light-setup text lines. Init lines contain per-light
 * DIFFUSE/AMBIENT/SPECULAR + baseline glDisable. POSITION lines belong in
 * display() because glLightfv snapshots the active modelview: eye-space slots
 * are emitted before the camera transforms and world-space slots after them.
 * The editor's code panel and exported C share these generators. */
int  repl_export_lights_init_line_count(void);
void repl_export_lights_init_line(int i, char *buf, size_t n);
int  repl_export_lights_pre_camera_line_count(void);
void repl_export_lights_pre_camera_line(int i, char *buf, size_t n);
int  repl_export_lights_display_line_count(void);
void repl_export_lights_display_line(int i, char *buf, size_t n);

#endif
