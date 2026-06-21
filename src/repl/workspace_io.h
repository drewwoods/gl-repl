/*
 * src/repl/workspace_io.h - Workspace filesystem + scene-file naming mechanics.
 *
 * The self-contained "how" of workspace persistence: recursive directory
 * creation and scene-file path/slug derivation. Pure filesystem + string work
 * with no REPL/scene state, so it carries none of the slot state machine —
 * scenes.c keeps slot selection, promotion policy, and the save/load
 * orchestration that drives these helpers.
 */
#ifndef REPL_WORKSPACE_IO_H
#define REPL_WORKSPACE_IO_H

#include <stddef.h>

/* Recursively create `dir` (mkdir -p semantics). Returns 1 on success or when
 * the directory already exists, 0 on failure or empty input. */
int workspace_io_ensure_dir(const char *dir);

/* 1 if `name` ends in ".c" / ".C". */
int workspace_io_has_c_ext(const char *name);

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
