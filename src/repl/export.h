/*
 * src/repl/export.h - Save/load of REPL sessions to/from C source files.
 *
 * Bidirectional text format for persisting complete REPL state (commands,
 * variables, camera, settings, workspace directory) as standalone C files.
 * Files round-trip cleanly: saving and re-loading preserves all state.
 *
 * Export format (save_output): Writes a complete C file with:
 *   1. Header comments with workspace metadata (@var name=value, @cfg setting=value,
 *      @scene-name <name>, @workspace-dir <path>). Used by import to restore context.
 *   2. Global variable declarations for user-defined predefined variables (float x, y, z).
 *   3. Camera state as the raw glTranslatef/glRotatef sequence the REPL uses internally
 *      (not a pose matrix — the exact command history).
 *   4. REPL function definitions converted to C function syntax (for reloading as
 *      CMD_FUNC_DEF on import).
 *   5. Geometry commands in the display() function body (user-edited commands).
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

#include "repl/eval.h"        /* REPL_SCRATCH_ARRAY_LEN */
#include "source_document.h"  /* caller-supplied SourceTextView */

/* Bag of `// @cfg` header key/value pairs used by save/load and scene snapshots.
 *
 * Export writes the bag as header lines, import rebuilds the same shape from
 * those lines, and src/repl/scenes.c uses it to snapshot the subset of
 * presentation settings that travels with a user scene. The REPL exporter and
 * importer treat keys and values as opaque strings; the controller-side config
 * owner decides what each slug means.
 *
 * The neutral bag exists so src/repl/export.c can round-trip cfg state without
 * including controller config headers. That dependency inversion came from
 * step 4 of feature/decouple-repl-from-gl-repl-alt.md. Presentation/render
 * slugs still flow through this bag while their storage finishes moving out of
 * the REPL-owned runtime state.
 *
 * Why a flat key/value bag and not a callback registry: the cfg payload is
 * ~30 slugs, all int 0..N, decimal-encoded. A registered-callback dispatch
 * would add init-order coupling, indirection on a 5-line operation, and
 * loss of inspectability. A flat collection round-trips trivially in tests
 * (feed bag in, read bag out, compare) and lets future migrations to per-TU
 * shapes happen without breaking call shapes. */
#define REPL_EXPORT_CFG_KEY_MAX     24
#define REPL_EXPORT_CFG_VALUE_MAX   16
#define REPL_EXPORT_CFG_MAX_ITEMS   32

typedef struct {
    char key[REPL_EXPORT_CFG_KEY_MAX];      /* opaque slug */
    char value[REPL_EXPORT_CFG_VALUE_MAX];  /* decimal-encoded; opaque to repl_export */
} ReplExportConfigItem;

typedef struct {
    ReplExportConfigItem items[REPL_EXPORT_CFG_MAX_ITEMS];
    int count;
} ReplExportConfig;

void        repl_export_config_clear(ReplExportConfig *cfg);
int         repl_export_config_set(ReplExportConfig *cfg,
                                   const char *key, const char *value);
int         repl_export_config_set_int(ReplExportConfig *cfg,
                                       const char *key, int value);
const char *repl_export_config_get(const ReplExportConfig *cfg, const char *key);
int         repl_export_config_get_int(const ReplExportConfig *cfg,
                                       const char *key, int fallback);
int         repl_export_config_count(const ReplExportConfig *cfg);
int         repl_export_config_at(const ReplExportConfig *cfg, int idx,
                                  const char **key_out, const char **value_out);

/* Controller-installed adapter for cfg save/load.
 *
 * App startup installs this bridge from src/app/glr_actions.c. Save paths call
 * fill_all() to emit `// @cfg` lines and get_int() to gate bootstrap lines;
 * src/repl/scenes.c calls fill_scene_subset() when capturing scene-local cfg
 * state; import and example-load paths call apply() as parsed cfg lines arrive.
 * The standalone demo leaves the bridge unset, so cfg lines are skipped and the
 * exporter/importer stay decoupled from glr_config.c. */
typedef struct {
    /* Fill bag with all current cfg values for save-file emission. */
    void (*fill_all)(ReplExportConfig *cfg);
    /* Fill bag with the per-scene-snapshot subset for repl_scenes. */
    void (*fill_scene_subset)(ReplExportConfig *cfg);
    /* Apply bag values to live state. The same apply works for both
     * full-set and scene-subset bags (it iterates whatever's there). */
    void (*apply)(const ReplExportConfig *cfg);
    /* Single-slug live read. Used by src/repl/export.c's bootstrap path
     * to gate emission of init commands (e.g. glPointParameterfv only
     * if "point_attenuation" toggle is on). Returns `fallback` when
     * the slug is unknown or the bridge isn't installed. */
    int  (*get_int)(const char *slug, int fallback);
} ReplExportConfigBridge;

/* Install or read the process-wide cfg bridge used by export/import and scene
 * snapshot code. Callers normally install once at app startup. */
void                          repl_export_install_config_bridge(const ReplExportConfigBridge *bridge);
const ReplExportConfigBridge *repl_export_config_bridge(void);

/* Apply the current batch of parsed `// @cfg` lines, then clear the accumulator.
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
 * (introduced in step 4a of the decouple plan). */
#define REPL_EXPORT_CAMERA_LINES        4
#define REPL_EXPORT_CAMERA_LINE_MAX     96
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
const ReplExportProjectionBridge *repl_export_projection_bridge(void);

/* Resolve the reshape projection lines: the installed bridge, else the
 * canonical perspective default. Fills up to REPL_EXPORT_PROJ_LINES
 * pointers into out[] and returns the count. Returned pointers reference
 * internal static storage valid until the next call — emit/copy them
 * before calling again. Main-thread only. */
int repl_export_reshape_projection_lines(const char *out[REPL_EXPORT_PROJ_LINES]);

/* Boilerplate C file segments for export. g_header_pre is the file-scope
 * preamble (includes, macros, rotation globals). g_display_header opens
 * the display() function (`void display() { ...clear/load/push...`) and
 * is shared verbatim by the code panel and emit_export_display_begin so
 * the two stay in sync. g_header_post follows the dynamic state lines
 * inside display(); g_footer_pre_init / g_footer_post_init bracket the
 * init() function. Together they form a valid C program. */
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

/* Controller-owned layout snapshot used by export and code-panel dumps.
 *
 * The exporter needs a few geometry and wrapping values to format saved output
 * and preview text, but it should not query UI or app state directly. Callers
 * compute the numbers once per export and pass them in here as opaque integers.
 * This data flow was moved out of hidden ui/glr state reads (implemented in
 * step 7c of feature/decouple-repl-from-gl-repl-alt.md).
 *
 * Callers that do not have a viewport on hand (LRU evict in
 * src/repl/scenes.c, headless tests, the standalone repl_demo which does
 * not export) may pass NULL. The export call sites then use
 * defensive defaults: scene_w / scene_h fall back to 800x600 in the
 * exported display() boilerplate, code_panel_w defaults to 0 (which
 * disables wrap), wrap_at_comma defaults to 1, and
 * show_vertex_indices defaults to 1. The defaults are deliberately
 * skewed toward "matches a typical interactive layout" so saved
 * files remain readable when re-loaded with the editor running. */
typedef struct {
    int viewport_w;          /* ui_state_viewport().window_w */
    int viewport_h;          /* ui_state_viewport().window_h */
    int scene_x;             /* ui_layout_scene_rect — geometry preserved in export */
    int scene_y;
    int scene_w;
    int scene_h;
    int code_panel_w;        /* ui_layout_code_panel_rect width — used by dump wrap */
    int wrap_at_comma;       /* glr_state_presentation().wrap_at_comma */
    int show_vertex_indices; /* glr_state_presentation().show_vertex_indices */
} ReplExportLayout;

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
void repl_export_save_output(const char *filename, SourceTextView text,
                             const ReplExportLayout *layout);

/* Code-panel dumps used by debug output and test fixtures. */
void repl_dump_code_panel_text(FILE *out, SourceTextView text);

/* Import a C source file saved by save_output(). Parses workspace header directives,
 * camera state, function definitions, and geometry commands. Feeds geometry lines
 * through repl_load_apply_line() (the lean non-editor source-load path from step
 * 5b) for normal parsing. Returns 1 on success, 0 on error (parse failure, open
 * failure). Pending scene-name and workspace-dir directives remain in import/export
 * state for the caller to consume after return. */
int  repl_export_load_from_file(const char *filename);

/* Refresh the export header text from current state. Pre-builds the header metadata
 * lines (@var, @cfg, @scene-name, @workspace-dir) from the current predefined
 * variables, render settings, and workspace directory. Called after mutations
 * (variable declare, config toggle, workspace directory change) to keep the export
 * buffer in sync. */
void repl_state_refresh_workspace_header_lines(void);

/* Parse a single workspace header directive line (e.g., "@var x=5" or "@cfg wireframe=1").
 * Updates internal state accordingly. Returns 1 if the line was a recognized directive,
 * 0 if it's not a directive (caller should process as regular code). Used during
 * load_from_file() to extract metadata. */
int  repl_state_parse_workspace_header_line(const char *line);

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

#if 0
/* Visual dump consumes the explicit layout snapshot so the REPL pipeline does
 * not reach into ui_state / glr_state for panel width or presentation flags.
 * The plain text dump above doesn't need the struct because it doesn't measure
 * or wrap. `code_margin_x` is passed by the caller so this helper still stays
 * free of UI metric ownership. Step 7c documented that dependency cut. */
void repl_dump_code_panel_visual_text(FILE *out, SourceTextView text,
                                      const ReplExportLayout *layout,
                                      int code_margin_x);
#endif

#endif
