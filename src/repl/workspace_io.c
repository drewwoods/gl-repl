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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

int workspace_io_workspace_name_valid(const char *name) {
    int has_alnum = 0;
    size_t len;
    if (!name || !name[0])
        return 0;
    len = strlen(name);
    if (len >= WORKSPACE_IO_NAME_MAX ||
        isspace((unsigned char)name[0]) ||
        isspace((unsigned char)name[len - 1]))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c < 32 || c == 127)
            return 0;
        if (isalnum(c))
            has_alnum = 1;
    }
    return has_alnum;
}

int workspace_io_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int workspace_io_path_join(const char *dir, const char *leaf,
                           char *out, size_t out_sz) {
    if (!dir || !dir[0] || !leaf || !leaf[0] || !out || out_sz == 0 ||
        strchr(leaf, '/') || strchr(leaf, '\\') || !strcmp(leaf, ".") ||
        !strcmp(leaf, ".."))
        return 0;
    int n = snprintf(out, out_sz, "%s/%s", dir, leaf);
    return n >= 0 && n < (int)out_sz;
}

int workspace_io_manifest_exists(const char *dir) {
    char path[REPL_WORKSPACE_DIR_MAX + 32];
    struct stat st;
    return workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                                  path, sizeof(path)) &&
           stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void manifest_err(char *err, size_t err_sz, const char *msg) {
    if (err && err_sz > 0)
        snprintf(err, err_sz, "%s", msg ? msg : "Workspace manifest error");
}

static int manifest_scene_file_valid(const char *leaf) {
    return leaf && leaf[0] && !strchr(leaf, '/') && !strchr(leaf, '\\') &&
           leaf[0] != '.' && workspace_io_has_c_ext(leaf);
}

static int manifest_has_scene(const WorkspaceManifest *manifest,
                              const char *leaf) {
    for (int i = 0; i < manifest->scene_count; i++)
        if (strcmp(manifest->scene_files[i], leaf) == 0)
            return 1;
    return 0;
}

static int parse_manifest_version(const char *text, int *out) {
    char *end = NULL;
    long value;
    if (!text || !text[0] || !out)
        return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || end == text || *end != '\0' || value != 1)
        return 0;
    *out = (int)value;
    return 1;
}

int workspace_io_manifest_read(const char *dir, WorkspaceManifest *out,
                               char *err, size_t err_sz) {
    char path[REPL_WORKSPACE_DIR_MAX + 32];
    char line[WORKSPACE_IO_FILE_MAX + 32];
    FILE *f;
    int saw_version = 0;
    int saw_name = 0;
    if (!out || !workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                                        path, sizeof(path))) {
        manifest_err(err, err_sz, "Invalid workspace path");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    f = fopen(path, "r");
    if (!f) {
        manifest_err(err, err_sz, "Workspace manifest is missing");
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n' && !feof(f)) {
            fclose(f);
            manifest_err(err, err_sz, "Workspace manifest line is too long");
            return 0;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!strncmp(line, "version=", 8)) {
            if (saw_version ||
                !parse_manifest_version(line + 8, &out->version)) {
                fclose(f);
                manifest_err(err, err_sz,
                             "Unsupported or duplicate workspace version");
                return 0;
            }
            saw_version = 1;
        } else if (!strncmp(line, "name=", 5)) {
            if (saw_name || !workspace_io_workspace_name_valid(line + 5)) {
                fclose(f);
                manifest_err(err, err_sz,
                             "Workspace manifest has an invalid name");
                return 0;
            }
            snprintf(out->name, sizeof(out->name), "%s", line + 5);
            saw_name = 1;
        } else if (!strncmp(line, "scene=", 6)) {
            const char *leaf = line + 6;
            if (!manifest_scene_file_valid(leaf)) {
                fclose(f);
                manifest_err(err, err_sz, "Workspace manifest has an invalid scene path");
                return 0;
            }
            if (out->scene_count >= WORKSPACE_IO_MAX_SCENES) {
                fclose(f);
                manifest_err(err, err_sz, "Workspace has more than 8 scenes");
                return 0;
            }
            if (manifest_has_scene(out, leaf)) {
                fclose(f);
                manifest_err(err, err_sz,
                             "Workspace manifest lists a scene twice");
                return 0;
            }
            snprintf(out->scene_files[out->scene_count], WORKSPACE_IO_FILE_MAX,
                     "%s", leaf);
            out->scene_count++;
        } else if (line[0] && line[0] != '#') {
            fclose(f);
            manifest_err(err, err_sz, "Workspace manifest has an unknown field");
            return 0;
        }
    }
    if (ferror(f)) {
        fclose(f);
        manifest_err(err, err_sz, "Could not read workspace manifest");
        return 0;
    }
    fclose(f);
    if (!saw_version || out->version != 1) {
        manifest_err(err, err_sz, "Unsupported workspace manifest version");
        return 0;
    }
    if (!saw_name) {
        manifest_err(err, err_sz, "Workspace manifest is missing a name");
        return 0;
    }
    return 1;
}

int workspace_io_manifest_write(const char *dir,
                                const WorkspaceManifest *manifest,
                                char *err, size_t err_sz) {
    char path[REPL_WORKSPACE_DIR_MAX + 32];
    char tmp[REPL_WORKSPACE_DIR_MAX + 40];
    FILE *f;
    if (!manifest || manifest->version != 1 ||
        !workspace_io_workspace_name_valid(manifest->name) ||
        manifest->scene_count < 0 ||
        manifest->scene_count > WORKSPACE_IO_MAX_SCENES ||
        !workspace_io_ensure_dir(dir) ||
        !workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                                path, sizeof(path))) {
        manifest_err(err, err_sz, "Invalid workspace manifest or directory");
        return 0;
    }
    for (int i = 0; i < manifest->scene_count; i++) {
        if (!manifest_scene_file_valid(manifest->scene_files[i])) {
            manifest_err(err, err_sz, "Workspace manifest has an invalid scene path");
            return 0;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(manifest->scene_files[i], manifest->scene_files[j]) == 0) {
                manifest_err(err, err_sz, "Workspace manifest lists a scene twice");
                return 0;
            }
        }
    }
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(tmp)) {
        manifest_err(err, err_sz, "Workspace manifest path is too long");
        return 0;
    }
    f = fopen(tmp, "w");
    if (!f) {
        manifest_err(err, err_sz, "Could not write workspace manifest");
        return 0;
    }
    fprintf(f, "version=1\nname=%s\n", manifest->name);
    for (int i = 0; i < manifest->scene_count; i++)
        fprintf(f, "scene=%s\n", manifest->scene_files[i]);
    int write_failed = fflush(f) != 0;
    if (!write_failed && fsync(fileno(f)) != 0)
        write_failed = 1;
    if (fclose(f) != 0)
        write_failed = 1;
    if (write_failed || rename(tmp, path) != 0) {
        unlink(tmp);
        manifest_err(err, err_sz, "Could not commit workspace manifest");
        return 0;
    }
    return 1;
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
