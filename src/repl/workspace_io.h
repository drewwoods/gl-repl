/*
 * src/repl/workspace_io.h - Workspace filesystem + scene-file naming mechanics.
 *
 * The self-contained "how" of workspace persistence: recursive directory
 * creation and scene-file path/slug derivation. Pure filesystem + string work
 * with no REPL slot state, so it carries none of the slot state machine -
 * scenes.c keeps slot selection, promotion policy, and the save/load
 * orchestration that drives these helpers.
 */
#ifndef REPL_WORKSPACE_IO_H
#define REPL_WORKSPACE_IO_H

#include <stddef.h>

#define WORKSPACE_IO_MANIFEST_FILE ".glr-workspace"
#define WORKSPACE_IO_MAX_SCENES 8
#define WORKSPACE_IO_NAME_MAX 64
#define WORKSPACE_IO_FILE_MAX 96

typedef struct {
    int version;
    char name[WORKSPACE_IO_NAME_MAX];
    int scene_count;
    char scene_files[WORKSPACE_IO_MAX_SCENES][WORKSPACE_IO_FILE_MAX];
} WorkspaceManifest;

/* Recursively create `dir` (mkdir -p semantics). Returns 1 on success or when
 * the directory already exists, 0 on failure or empty input. */
int workspace_io_ensure_dir(const char *dir);

/* 1 if `name` ends in ".c" / ".C". */
int workspace_io_has_c_ext(const char *name);
int workspace_io_workspace_name_valid(const char *name);
int workspace_io_regular_file(const char *path);
int workspace_io_manifest_exists(const char *dir);
int workspace_io_manifest_read(const char *dir, WorkspaceManifest *out,
                               char *err, size_t err_sz);
int workspace_io_manifest_write(const char *dir,
                                const WorkspaceManifest *manifest,
                                char *err, size_t err_sz);
int workspace_io_path_join(const char *dir, const char *leaf,
                           char *out, size_t out_sz);

/* Scene display name from a file path: the basename with its extension
 * stripped, clamped to out_sz. */
void workspace_io_scene_name_from_filename(const char *path,
                                           char *out, size_t out_sz);

/* Lowercase alphanumeric slug for a scene name (space / '-' / '_' collapse to
 * '_', other punctuation dropped); falls back to "s" when the result is empty. */
void workspace_io_filename_slug(const char *name, char *out, size_t out_sz);

/* Build a collision-disambiguated slug: `base_slug` followed by
 * `collision_depth` "_0" suffixes, clamped to out_sz. The caller probes
 * increasing depths until the slug is unique within its scene set. */
void workspace_io_slug_with_collision_depth(const char *base_slug,
                                            int collision_depth,
                                            char *out, size_t out_sz);

#endif /* REPL_WORKSPACE_IO_H */
