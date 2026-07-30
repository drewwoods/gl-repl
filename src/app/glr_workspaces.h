/* glr_workspaces.h - Managed-workspace catalog under the app workspace root. */
#ifndef GLR_WORKSPACES_H
#define GLR_WORKSPACES_H

#include <stddef.h>

void        glr_workspaces_refresh(void);
int         glr_workspaces_count(void);
const char *glr_workspaces_name(int idx);
const char *glr_workspaces_path(int idx);
int         glr_workspaces_active_index(void);

/* Display name of the workspace the live scene set is bound to, or "" when
 * no managed workspace is bound (a plain collection of scenes). Answers the
 * catalog first; falls back to the manifest name — then the directory
 * basename — for a workspace opened through "Other folder...", which has no
 * catalog entry. A bound directory without a manifest is not a workspace and
 * reads as "".
 *
 * Memoized on the bound directory (invalidated by glr_workspaces_refresh(),
 * which every workspace mutation already calls) so per-frame callers such as
 * the window title do no filesystem work. Returns static storage. */
const char *glr_workspaces_active_name(void);

int         glr_workspaces_create(const char *name,
                                  char *out_path, size_t out_sz,
                                  char *err, size_t err_sz);
int         glr_workspaces_discard_empty(const char *path);

#endif
