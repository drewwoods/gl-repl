/* glr_workspaces.c - Enumerate and exclusively create managed workspaces. */
#include "app/glr_workspaces.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/glr_paths.h"
#include "config.h"
#include "repl/scenes.h"
#include "repl/workspace_io.h"

#define GLR_WORKSPACES_MAX 64

typedef struct {
    char name[WORKSPACE_IO_NAME_MAX];
    char path[GLR_PATH_MAX];
} GlrWorkspaceEntry;

static GlrWorkspaceEntry g_entries[GLR_WORKSPACES_MAX];
static int g_count;

static int entry_cmp(const void *a, const void *b) {
    const GlrWorkspaceEntry *ea = (const GlrWorkspaceEntry *)a;
    const GlrWorkspaceEntry *eb = (const GlrWorkspaceEntry *)b;
    return strcmp(ea->name, eb->name);
}

void glr_workspaces_refresh(void) {
    char root[GLR_PATH_MAX];
    DIR *d;
    g_count = 0;
    if (!glr_paths_workspaces_root(root, sizeof(root)))
        return;
    d = opendir(root);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) && g_count < GLR_WORKSPACES_MAX) {
        char path[GLR_PATH_MAX];
        struct stat st;
        WorkspaceManifest manifest;
        if (ent->d_name[0] == '.')
            continue;
        int n = snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
        if (n < 0 || n >= (int)sizeof(path) || stat(path, &st) != 0 ||
            !S_ISDIR(st.st_mode) ||
            !workspace_io_manifest_read(path, &manifest, NULL, 0))
            continue;
        snprintf(g_entries[g_count].name, sizeof(g_entries[g_count].name),
                 "%s", manifest.name[0] ? manifest.name : ent->d_name);
        snprintf(g_entries[g_count].path, sizeof(g_entries[g_count].path),
                 "%s", path);
        g_count++;
    }
    closedir(d);
    qsort(g_entries, (size_t)g_count, sizeof(g_entries[0]), entry_cmp);
}

int glr_workspaces_count(void) { return g_count; }

const char *glr_workspaces_name(int idx) {
    return idx >= 0 && idx < g_count ? g_entries[idx].name : "";
}

const char *glr_workspaces_path(int idx) {
    return idx >= 0 && idx < g_count ? g_entries[idx].path : "";
}

int glr_workspaces_active_index(void) {
    const char *active = repl_workspace_dir();
    for (int i = 0; i < g_count; i++)
        if (glr_paths_same_dir(active, g_entries[i].path))
            return i;
    return -1;
}

static void set_err(char *err, size_t err_sz, const char *msg) {
    if (err && err_sz)
        snprintf(err, err_sz, "%s", msg ? msg : "Workspace error");
}

int glr_workspaces_create(const char *name,
                          char *out_path, size_t out_sz,
                          char *err, size_t err_sz) {
    char root[GLR_PATH_MAX];
    char path[GLR_PATH_MAX];
    WorkspaceManifest manifest;
    if (!workspace_io_workspace_name_valid(name)) {
        set_err(err, err_sz, "Workspace name needs a letter or digit");
        return 0;
    }
    if (!glr_paths_workspaces_root(root, sizeof(root)) ||
        !glr_paths_ensure_dir(root, NULL) ||
        !glr_paths_workspace_dir_for_name(name, path, sizeof(path))) {
        set_err(err, err_sz, "Could not resolve workspace path");
        return 0;
    }
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            char msg[WORKSPACE_IO_NAME_MAX + 48];
            snprintf(msg, sizeof(msg), "Workspace '%s' already exists", name);
            set_err(err, err_sz, msg);
        } else {
            set_err(err, err_sz, "Could not create workspace directory");
        }
        return 0;
    }
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "%s", name);
    if (!workspace_io_manifest_write(path, &manifest, err, err_sz)) {
        rmdir(path);
        return 0;
    }
    if (out_path && out_sz)
        snprintf(out_path, out_sz, "%s", path);
    glr_workspaces_refresh();
    return 1;
}

int glr_workspaces_discard_empty(const char *path) {
    WorkspaceManifest manifest;
    char manifest_path[GLR_PATH_MAX];
    if (!workspace_io_manifest_read(path, &manifest, NULL, 0) ||
        manifest.scene_count != 0 ||
        !workspace_io_path_join(path, WORKSPACE_IO_MANIFEST_FILE,
                                manifest_path, sizeof(manifest_path)))
        return 0;
    if (unlink(manifest_path) != 0)
        return 0;
    if (rmdir(path) != 0)
        return 0;
    glr_workspaces_refresh();
    return 1;
}
