/*
 * src/repl/workspace_io.c - Workspace filesystem + scene-file naming mechanics.
 *
 * Extracted from src/repl/scenes.c so the slot state machine keeps only the
 * policy ("which scene, when") and delegates the persistence "how" (make the
 * directory, name the file) here. No g_user_scenes / live-state access: every
 * routine works purely on its arguments.
 */
#include "repl/workspace_io.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "repl/state_views.h"   /* REPL_WORKSPACE_DIR_MAX */

static int path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_one_dir(const char *dir) {
    if (!dir || !*dir)
        return 0;
    if (mkdir(dir, 0755) == 0)
        return 1;
    return errno == EEXIST && path_is_dir(dir);
}

int workspace_io_ensure_dir(const char *dir) {
    if (!dir || !*dir)
        return 0;

    char path[REPL_WORKSPACE_DIR_MAX];
    int n = snprintf(path, sizeof(path), "%s", dir);
    if (n < 0 || n >= (int)sizeof(path))
        return 0;

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (path[0] && !mkdir_one_dir(path)) {
            *p = '/';
            return 0;
        }
        *p = '/';
    }

    return mkdir_one_dir(path);
}

int workspace_io_has_c_ext(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 3) return 0;
    return name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'C');
}

void workspace_io_scene_name_from_filename(const char *path,
                                           char *out, size_t out_sz) {
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    int n = 0;
    while (base[n] && base[n] != '.' && n < (int)out_sz - 1) {
        out[n] = base[n];
        n++;
    }
    if (out_sz > 0) out[n] = '\0';
}

void workspace_io_filename_slug(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    if (name) {
        for (size_t i = 0; name[i] && j + 1 < out_sz; i++) {
            unsigned char c = (unsigned char)name[i];
            if (isalnum(c))                            out[j++] = (char)tolower(c);
            else if (c == ' ' || c == '-' || c == '_') out[j++] = '_';
        }
    }
    if (j == 0 && out_sz > 0) out[j++] = 's';
    if (j >= out_sz) j = out_sz - 1;
    out[j] = '\0';
}

void workspace_io_slug_with_collision_depth(const char *base_slug,
                                            int collision_depth,
                                            char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!base_slug) base_slug = "s";

    size_t max_len = out_sz - 1;
    size_t suffix_len = (size_t)collision_depth * 2u;
    if (suffix_len > max_len)
        suffix_len = max_len;

    size_t prefix_len = strlen(base_slug);
    if (prefix_len > max_len - suffix_len)
        prefix_len = max_len - suffix_len;

    memcpy(out, base_slug, prefix_len);
    size_t pos = prefix_len;
    for (int i = 0; i < collision_depth && pos + 2 <= max_len; i++) {
        out[pos++] = '_';
        out[pos++] = '0';
    }
    out[pos] = '\0';
}
