/*
 * glr_paths.h - App-owned filesystem locations.
 *
 * Centralizes per-user app data paths so the controller decides where app
 * artifacts live and lower layers only receive explicit directories.
 */
#ifndef GLR_PATHS_H
#define GLR_PATHS_H

#include <stddef.h>

int         glr_paths_user_data_dir(char *buf, size_t buflen);
int         glr_paths_user_music_dir(char *buf, size_t buflen);
int         glr_paths_user_workspace_dir(char *buf, size_t buflen);
int         glr_paths_workspaces_root(char *buf, size_t buflen);
int         glr_paths_workspace_dir_for_name(const char *name,
                                              char *buf, size_t buflen);
int         glr_paths_resolve_output_path(const char *leaf,
                                           char *buf, size_t buflen);
int         glr_paths_app_state_path(const char *leaf,
                                      char *buf, size_t buflen);
int         glr_paths_same_dir(const char *a, const char *b);
int         glr_paths_cwd_supports_relative_saves(void);
const char *glr_paths_default_workspace_dir(void);

/* Path the audio resume-state file should be written to / read from.
 * Plain GLR_AUDIO_STATE_FILE_NAME (working-directory relative) for a normal
 * dev run; inside the per-user data dir when the working directory is not
 * writable - the macOS .app case, where the CWD is "/". The user data dir is
 * created on demand. Returns a pointer to static storage. */
const char *glr_paths_default_audio_state_file(void);

/* mkdir -p. Returns 1 on success, 0 on failure. If `created` is non-NULL it is
 * set to 1 only when the leaf directory was created by this call. */
int         glr_paths_ensure_dir(const char *path, int *created);

#endif /* GLR_PATHS_H */
