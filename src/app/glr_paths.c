/*
 * glr_paths.c - App-owned filesystem locations.
 */
#include "app/glr_paths.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

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
    return user_data_subdir("workspaces/default", buf, buflen);
}

int glr_paths_cwd_supports_relative_saves(void) {
    return access(".", W_OK) == 0;
}

const char *glr_paths_default_workspace_dir(void) {
    static char dir[GLR_PATH_MAX];
    char root[GLR_PATH_MAX];
    if (glr_paths_workspaces_root(root, sizeof(root))) {
        int n = snprintf(dir, sizeof(dir), "%s/%s", root,
                         GLR_DEFAULT_WORKSPACE_NAME);
        if (n >= 0 && n < (int)sizeof(dir))
            return dir;
    }
    return GLR_WORKSPACES_ROOT_DIR "/" GLR_DEFAULT_WORKSPACE_NAME;
}

int glr_paths_workspaces_root(char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return 0;
    if (glr_paths_cwd_supports_relative_saves()) {
        int n = snprintf(buf, buflen, "%s", GLR_WORKSPACES_ROOT_DIR);
        return n >= 0 && n < (int)buflen;
    }
    return user_data_subdir("workspaces", buf, buflen);
}

int glr_paths_workspace_dir_for_name(const char *name,
                                     char *buf, size_t buflen) {
    char root[GLR_PATH_MAX];
    char slug[64];
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\') ||
        !glr_paths_workspaces_root(root, sizeof(root)))
        return 0;
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof(slug); i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c))
            slug[j++] = (char)tolower(c);
        else if (c == ' ' || c == '-' || c == '_')
            slug[j++] = '_';
    }
    if (j == 0)
        return 0;
    slug[j] = '\0';
    int n = snprintf(buf, buflen, "%s/%s", root, slug);
    return n >= 0 && n < (int)buflen;
}

int glr_paths_resolve_output_path(const char *leaf,
                                  char *buf, size_t buflen) {
    if (!leaf || !leaf[0] || !buf || buflen == 0)
        return 0;
    if (glr_paths_cwd_supports_relative_saves()) {
        int n = snprintf(buf, buflen, "%s", leaf);
        return n >= 0 && n < (int)buflen;
    }
    const char *dir = glr_paths_default_workspace_dir();
    int n = snprintf(buf, buflen, "%s/%s", dir, leaf);
    return n >= 0 && n < (int)buflen;
}

int glr_paths_app_state_path(const char *leaf, char *buf, size_t buflen) {
    char data[GLR_PATH_MAX];
    if (!leaf || !leaf[0] || !buf || buflen == 0)
        return 0;
    if (glr_paths_cwd_supports_relative_saves()) {
        int n = snprintf(buf, buflen, "%s", leaf);
        return n >= 0 && n < (int)buflen;
    }
    if (!glr_paths_user_data_dir(data, sizeof(data)))
        return 0;
    int n = snprintf(buf, buflen, "%s/state/%s", data, leaf);
    return n >= 0 && n < (int)buflen;
}

const char *glr_paths_default_audio_state_file(void) {
    static char path[GLR_PATH_MAX];
    char dir[GLR_PATH_MAX];

    if (!glr_paths_app_state_path(GLR_AUDIO_STATE_FILE_NAME,
                                  path, sizeof(path)))
        return GLR_AUDIO_STATE_FILE_NAME;
    if (glr_paths_cwd_supports_relative_saves())
        return path;

    /* Unlike the recovery file, nothing else creates the state dir before
     * the audio worker's first save — do it here, once, at bootstrap. */
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return path;
    *slash = '\0';
    if (!glr_paths_ensure_dir(dir, NULL))
        return GLR_AUDIO_STATE_FILE_NAME;
    return path;
}

static int absolute_lexical_path(const char *path, char *buf, size_t buflen) {
    char cwd[GLR_PATH_MAX];
    char input[GLR_PATH_MAX];
    char *parts[GLR_PATH_MAX / 2];
    int part_count = 0;
    if (!path || !path[0])
        return 0;
    if (path[0] == '/') {
        if (snprintf(input, sizeof(input), "%s", path) >= (int)sizeof(input))
            return 0;
    } else {
        if (!getcwd(cwd, sizeof(cwd)))
            return 0;
        if (snprintf(input, sizeof(input), "%s/%s", cwd, path) >=
            (int)sizeof(input))
            return 0;
    }

    char *cursor = input;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        char *part = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor) *cursor++ = '\0';
        if (!strcmp(part, "."))
            continue;
        if (!strcmp(part, "..")) {
            if (part_count > 0) part_count--;
            continue;
        }
        parts[part_count++] = part;
    }

    size_t used = 0;
    if (buflen < 2) return 0;
    buf[used++] = '/';
    for (int i = 0; i < part_count; i++) {
        size_t len = strlen(parts[i]);
        if (used + len + (i + 1 < part_count ? 1u : 0u) >= buflen)
            return 0;
        memcpy(buf + used, parts[i], len);
        used += len;
        if (i + 1 < part_count) buf[used++] = '/';
    }
    buf[used] = '\0';
    return 1;
}

int glr_paths_same_dir(const char *a, const char *b) {
    char ar[GLR_PATH_MAX], br[GLR_PATH_MAX];
    if (!a || !a[0] || !b || !b[0])
        return 0;
    if (realpath(a, ar) && realpath(b, br))
        return strcmp(ar, br) == 0;
    if (!absolute_lexical_path(a, ar, sizeof(ar)) ||
        !absolute_lexical_path(b, br, sizeof(br)))
        return strcmp(a, b) == 0;
    return strcmp(ar, br) == 0;
}
