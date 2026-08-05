/*
 * glr_lint_scenes.c - `--lint-scenes <dir>` : validate .glr files, no window.
 *
 * See glr_lint_scenes.h. This runs the *same* two checkers the loaders run -
 * src/repl/camera_header.c and src/repl/doc_order.c - over raw file text, so
 * a file that lints clean is a file that loads clean, and there is no second
 * opinion to drift.
 */
#include "app/boot/glr_lint_scenes.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "repl/camera_header.h"
#include "repl/doc_order.h"

typedef struct {
    const char *path;
    FILE       *out;
    int         count;
} LintFileCtx;

static void lint_camera_sink(void *userdata, const ReplCameraDiag *diag,
                             const char *rule_text) {
    LintFileCtx *ctx = (LintFileCtx *)userdata;

    /* A missing pose role is a note, not a violation: a hand-authored scene
     * that sets only `dist` and is content with the defaults for the rest is
     * a legitimate file. */
    if (repl_camera_rule_severity(diag->rule) != REPL_CAMERA_SEVERITY_REJECTION)
        return;
    fprintf(ctx->out, "%s:%d: %s.\n", ctx->path, diag->line_no, rule_text);
    ctx->count++;
}

static void lint_order_sink(void *userdata, ReplDocOrderRule rule,
                            int line_no, int conflict_line_no,
                            const char *message) {
    LintFileCtx *ctx = (LintFileCtx *)userdata;

    (void)rule;
    (void)conflict_line_no;
    fprintf(ctx->out, "%s:%d: %s\n", ctx->path, line_no, message);
    ctx->count++;
}

static int lint_one_file(const char *path, FILE *out) {
    ReplCameraHeader camera;
    ReplDocOrder     order;
    LintFileCtx      ctx;
    char             line[MAX_LINE_LEN];
    int              line_no = 0;
    FILE            *f;

    f = fopen(path, "r");
    if (!f) {
        fprintf(out, "%s: cannot open\n", path);
        return 1;
    }

    ctx.path  = path;
    ctx.out   = out;
    ctx.count = 0;

    repl_camera_header_init(&camera);
    repl_camera_header_set_sink(&camera, lint_camera_sink, &ctx);
    repl_doc_order_init(&order);
    repl_doc_order_set_sink(&order, lint_order_sink, &ctx);

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        ReplCameraLineResult result;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        line_no++;
        result = repl_camera_header_offer(&camera, line, line_no);
        (void)repl_doc_order_offer(&order, line, line_no,
                                   result != REPL_CAMERA_LINE_NOT_CAMERA);
    }
    fclose(f);

    /* No bridge is installed here, so finish() validates and diagnoses but
     * applies nothing - which is exactly what a linter wants. */
    (void)repl_camera_header_finish(&camera, REPL_CAMERA_APPLY_IMPORT);
    return ctx.count > 0;
}

static int lint_name_is_glr(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".glr") == 0;
}

static int lint_name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int glr_lint_scenes_dir(const char *dir, FILE *out) {
    DIR *d;
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0, cap = 0;
    size_t i;
    int bad_files = 0;

    d = opendir(dir);
    if (!d) {
        fprintf(out, "%s: cannot open directory\n", dir);
        return 1;
    }
    while ((entry = readdir(d)) != NULL) {
        if (!lint_name_is_glr(entry->d_name))
            continue;
        if (count == cap) {
            size_t next = cap ? cap * 2 : 32;
            char **grown = (char **)realloc(names, next * sizeof(*names));
            if (!grown)
                break;
            names = grown;
            cap = next;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count])
            break;
        count++;
    }
    closedir(d);

    /* Sorted, so the worklist is stable between runs and diffable. */
    qsort(names, count, sizeof(*names), lint_name_cmp);

    for (i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        bad_files += lint_one_file(path, out);
        free(names[i]);
    }
    free(names);

    fprintf(out, "%s: %d of %d files need migration\n",
            dir, bad_files, (int)count);
    return bad_files;
}
