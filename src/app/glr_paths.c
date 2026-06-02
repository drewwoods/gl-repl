/*
 * glr_paths.c - App-owned filesystem locations.
 */
#include "app/glr_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GLR_APP_NAME "gl-repl"

static int path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_one_dir(const char *dir, int *created) {
    if (!dir || !*dir)
        return 0;
    if (mkdir(dir, 0755) == 0) {
        if (created)
            *created = 1;
        return 1;
    }
    return errno == EEXIST && path_is_dir(dir);
}

int glr_paths_ensure_dir(const char *path, int *created) {
    if (created)
        *created = 0;
    if (!path || !*path)
        return 0;

    char tmp[GLR_PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || n >= (int)sizeof(tmp))
        return 0;

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (tmp[0] && !mkdir_one_dir(tmp, NULL)) {
            *p = '/';
            return 0;
        }
        *p = '/';
    }

    return mkdir_one_dir(tmp, created);
}

int glr_paths_user_data_dir(char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return 0;

#if defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    int n = snprintf(buf, buflen,
                     "%s/Library/Application Support/%s",
                     home, GLR_APP_NAME);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    int n;
    if (xdg && xdg[0]) {
        n = snprintf(buf, buflen, "%s/%s", xdg, GLR_APP_NAME);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return 0;
        n = snprintf(buf, buflen, "%s/.local/share/%s",
                     home, GLR_APP_NAME);
    }
#endif
    return n >= 0 && n < (int)buflen;
}

static int user_data_subdir(const char *leaf, char *buf, size_t buflen) {
    char root[GLR_PATH_MAX];
    if (!leaf || !leaf[0] || !glr_paths_user_data_dir(root, sizeof(root)))
        return 0;
    int n = snprintf(buf, buflen, "%s/%s", root, leaf);
    return n >= 0 && n < (int)buflen;
}

int glr_paths_user_music_dir(char *buf, size_t buflen) {
#if defined(__APPLE__)
    return user_data_subdir("Music", buf, buflen);
#else
    return user_data_subdir("music", buf, buflen);
#endif
}

int glr_paths_user_workspace_dir(char *buf, size_t buflen) {
    return user_data_subdir("workspace", buf, buflen);
}

int glr_paths_cwd_supports_relative_saves(void) {
    return access(".", W_OK) == 0;
}

const char *glr_paths_default_workspace_dir(void) {
    static char dir[GLR_PATH_MAX];

    if (glr_paths_cwd_supports_relative_saves())
        return GLR_DEFAULT_WORKSPACE_DIR;
    if (glr_paths_user_workspace_dir(dir, sizeof(dir)))
        return dir;
    return GLR_DEFAULT_WORKSPACE_DIR;
}
