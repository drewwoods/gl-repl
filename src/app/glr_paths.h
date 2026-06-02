/*
 * glr_paths.h - App-owned filesystem locations.
 *
 * Centralizes per-user app data paths so the controller decides where app
 * artifacts live and lower layers only receive explicit directories.
 */
#ifndef GLR_PATHS_H
#define GLR_PATHS_H

#include <stddef.h>

#define GLR_PATH_MAX 1024

/* Default workspace directory for save/load operations when the current
 * working directory is writable. Can be overridden by workspace metadata
 * headers or explicit user selection. */
#define GLR_DEFAULT_WORKSPACE_DIR "./workspace"

int         glr_paths_user_data_dir(char *buf, size_t buflen);
int         glr_paths_user_music_dir(char *buf, size_t buflen);
int         glr_paths_user_workspace_dir(char *buf, size_t buflen);
int         glr_paths_cwd_supports_relative_saves(void);
const char *glr_paths_default_workspace_dir(void);

/* mkdir -p. Returns 1 on success, 0 on failure. If `created` is non-NULL it is
 * set to 1 only when the leaf directory was created by this call. */
int         glr_paths_ensure_dir(const char *path, int *created);

#endif /* GLR_PATHS_H */
