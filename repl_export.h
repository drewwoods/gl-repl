/*
 * repl_export.h - Save/load of REPL sessions to/from C source files.
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
 *   2. Extracts camera state (raw glTranslatef/glRotatef lines).
 *   3. Detects function definitions (lines matching C function syntax).
 *   4. Feeds remaining geometry lines through feed_line() for normal parsing.
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
 * Multi-scene export lives in repl_scenes.c: workspace saves iterate user-scene
 * slots, set the export scene-name hint, and call repl_export_save_output() for
 * each scene.
 */
#ifndef REPL_EXPORT_H
#define REPL_EXPORT_H

#include "editor/state.h"  /* EditorBufferView */
#include "repl_eval.h"     /* REPL_SCRATCH_ARRAY_LEN */

/* Neutral header-config bag (step 4 of feature/decouple-repl-from-gl-repl-alt.md).
 *
 * The REPL pipeline historically called glr_config_get/set/items directly to
 * emit and parse `// @cfg slug = value` header lines. That dragged glr_config.c
 * (and transitively audio, camera, profile, variable_panel) into the demo
 * link set. The neutral bag inverts the dependency: owners (controller-side)
 * fill a ReplExportConfig before save and read one after load; repl_export.c
 * iterates the bag without knowing what the slugs mean.
 *
 * Step 4 is the temporary catalog split: presentation/render slugs land in
 * the bag because their backing storage is still in g_repl_state. Step 7
 * relocates that storage to glr_state.c and reassigns slugs.
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

/* Bridge installed by the controller. The bridge knows how to project app/REPL
 * cfg state into a bag and apply a bag back. repl_export.c uses it for @cfg
 * emission (save) and apply (load); repl_scenes.c uses it for per-scene
 * snapshots. The demo deliberately leaves the bridge unset so repl_export.c
 * doesn't reach for cfg-bound state, which is what keeps glr_config.c out of
 * the demo link set. */
typedef struct {
    /* Fill bag with all current cfg values for save-file emission. */
    void (*fill_all)(ReplExportConfig *cfg);
    /* Fill bag with the per-scene-snapshot subset for repl_scenes. */
    void (*fill_scene_subset)(ReplExportConfig *cfg);
    /* Apply bag values to live state. The same apply works for both
     * full-set and scene-subset bags (it iterates whatever's there). */
    void (*apply)(const ReplExportConfig *cfg);
    /* Single-slug live read. Used by repl_export.c's bootstrap path
     * to gate emission of init commands (e.g. glPointParameterfv only
     * if "point_attenuation" toggle is on). Returns `fallback` when
     * the slug is unknown or the bridge isn't installed. */
    int  (*get_int)(const char *slug, int fallback);
} ReplExportConfigBridge;

void                          repl_export_install_config_bridge(const ReplExportConfigBridge *bridge);
const ReplExportConfigBridge *repl_export_config_bridge(void);

/* Drain the pending-import-cfg accumulator (populated by header-line parsing).
 * Used by import paths after they finish handing all `// @cfg` lines to
 * `repl_state_parse_workspace_header_line`. The drain calls the installed
 * bridge's apply() with the accumulated bag and resets the accumulator. */
void repl_export_apply_pending_cfg(void);

/* Boilerplate C file segments for export. g_header_pre is the initial includes
 * and setup; g_header_post follows the metadata comments; g_footer_pre_init is
 * before the display() function; g_footer_post_init follows the function body.
 * Together they bracket the exported code to create a valid C program. */
extern const char  *g_header_pre[];
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

/* Export current REPL state to a C source file. Writes header metadata (@var, @cfg,
 * @scene-name, @workspace-dir), global variable declarations, camera state, function
 * definitions, and geometry commands to filename. The file is a complete, standalone
 * C program that can be reloaded via load_from_file(). Called by save-to-output and
 * workspace export routines.
 *
 * `text` is the editor buffer view the caller built; the export
 * pipeline reads source text exclusively through that view rather
 * than reaching into editor globals. */
void repl_export_save_output(const char *filename, EditorBufferView text);

/* Import a C source file saved by save_output(). Parses workspace header directives,
 * camera state, function definitions, and geometry commands. Feeds geometry lines
 * through feed_line() for normal parsing. Returns 1 on success, 0 on error (parse
 * failure, open failure). Pending scene-name and workspace-dir directives remain
 * in import/export state for the caller to consume after return. */
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
int  init_section_line_count(void);
void init_section_line(int i, char *buf, size_t n);

#endif
