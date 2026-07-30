/* glr_workspaces.h - Managed-workspace catalog under the app workspace root. */
#ifndef GLR_WORKSPACES_H
#define GLR_WORKSPACES_H

#include <stddef.h>

void        glr_workspaces_refresh(void);
int         glr_workspaces_count(void);
const char *glr_workspaces_name(int idx);
const char *glr_workspaces_path(int idx);
int         glr_workspaces_active_index(void);
int         glr_workspaces_create(const char *name,
                                  char *out_path, size_t out_sz,
                                  char *err, size_t err_sz);
int         glr_workspaces_discard_empty(const char *path);

#endif
