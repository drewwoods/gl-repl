#include "repl/examples.h"
#include "repl/catalog_tags.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c_compat.h"   /* STATIC_ASSERT (C99/C11 portable) */
#include "config.h"     /* MAX_LINE_LEN */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    const char *name;
    const char *const *lines;
    const char *const *tag_names;   /* Null-terminated string list for built-in entries */
    ReplItemTagNode *tags;           /* Dynamic linked list for runtime catalog entries */
    ReplExampleSourceFormat format;
    const char *subheading;
} ReplExampleEntry;

static ReplTagNode *g_example_tags_head = NULL;
static int g_example_tag_count = 0;

static void ensure_default_tags_initialized(void);

const ReplTagNode *repl_example_find_or_register_tag(const char *name) {
    if (!name || !*name)
        return NULL;
    ensure_default_tags_initialized();

    for (ReplTagNode *curr = g_example_tags_head; curr; curr = curr->next) {
        if (strcmp(curr->name, name) == 0)
            return curr;
    }

    ReplTagNode *node = (ReplTagNode *)calloc(1, sizeof(ReplTagNode));
    if (!node)
        return NULL;
    snprintf(node->name, sizeof(node->name), "%s", name);
    node->id = g_example_tag_count++;
    node->next = NULL;

    if (!g_example_tags_head) {
        g_example_tags_head = node;
    } else {
        ReplTagNode *tail = g_example_tags_head;
        while (tail->next)
            tail = tail->next;
        tail->next = node;
    }
    return node;
}

static int g_default_tags_initializing = 0;

static void ensure_default_tags_initialized(void) {
    if (g_example_tags_head != NULL || g_default_tags_initializing)
        return;

    g_default_tags_initializing = 1;
    repl_example_find_or_register_tag("All");      /* id 0 */
    repl_example_find_or_register_tag("2D");       /* id 1 */
    repl_example_find_or_register_tag("3D");       /* id 2 */
    repl_example_find_or_register_tag("Polygons"); /* id 3 */
    repl_example_find_or_register_tag("Lines");    /* id 4 */
    g_default_tags_initializing = 0;
}

static void clear_dynamic_tags_list(void) {
    ReplTagNode *curr = g_example_tags_head;
    while (curr) {
        ReplTagNode *next = curr->next;
        free(curr);
        curr = next;
    }
    g_example_tags_head = NULL;
    g_example_tag_count = 0;
}

int repl_example_attach_tag(ReplItemTagNode **head, const ReplTagNode *tag) {
    if (!head || !tag)
        return 0;
    for (ReplItemTagNode *curr = *head; curr; curr = curr->next) {
        if (curr->tag == tag)
            return 1;
    }
    ReplItemTagNode *node = (ReplItemTagNode *)calloc(1, sizeof(ReplItemTagNode));
    if (!node)
        return 0;
    node->tag = tag;
    node->next = NULL;
    if (*head == NULL) {
        *head = node;
    } else {
        ReplItemTagNode *tail = *head;
        while (tail->next)
            tail = tail->next;
        tail->next = node;
    }
    return 1;
}

void repl_example_free_item_tags(ReplItemTagNode *head) {
    ReplItemTagNode *curr = head;
    while (curr) {
        ReplItemTagNode *next = curr->next;
        free(curr);
        curr = next;
    }
}

#include "../../build/generated/repl_examples_data.inc"

static ReplExampleEntry *g_runtime_example_entries = NULL;
static int g_runtime_example_count = 0;

static void examples_set_error(char *err_buf, int err_sz,
                               const char *fmt, ...) {
    if (!err_buf || err_sz <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, (size_t)err_sz, fmt, ap);
    va_end(ap);
}

static char *examples_strdup_range(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out)
        return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *examples_strdup(const char *s) {
    return examples_strdup_range(s ? s : "", strlen(s ? s : ""));
}

static char *examples_trim(char *s) {
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int examples_streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int examples_has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t sn = strlen(suffix);
    return n >= sn && strcmp(s + n - sn, suffix) == 0;
}

static void examples_free_entry(ReplExampleEntry *entry) {
    if (!entry)
        return;
    free((char *)entry->name);
    free((char *)entry->subheading);
    repl_example_free_item_tags(entry->tags);
    if (entry->lines) {
        char **lines = (char **)entry->lines;
        for (int i = 0; lines[i]; i++)
            free(lines[i]);
        free(lines);
    }
    memset(entry, 0, sizeof(*entry));
}

static void examples_free_entries(ReplExampleEntry *entries, int count) {
    if (!entries)
        return;
    for (int i = 0; i < count; i++)
        examples_free_entry(&entries[i]);
    free(entries);
}

void repl_examples_clear_runtime_catalog(void) {
    examples_free_entries(g_runtime_example_entries, g_runtime_example_count);
    g_runtime_example_entries = NULL;
    g_runtime_example_count = 0;
    clear_dynamic_tags_list();
    ensure_default_tags_initialized();
}

typedef struct {
    ReplExampleEntry *entries;
    int count;
    int cap;
    char **sections;
    int section_count;
    int section_cap;
    char **files;
    int file_count;
    int file_cap;
} RuntimeCatalogBuild;

typedef struct {
    char *section;
    char *file;
    char *name;
    char *tags;
    char *group;
} RuntimeCatalogDraft;

static void catalog_draft_clear(RuntimeCatalogDraft *draft) {
    if (!draft)
        return;
    free(draft->section);
    free(draft->file);
    free(draft->name);
    free(draft->tags);
    free(draft->group);
    memset(draft, 0, sizeof(*draft));
}

static void string_list_clear(char **list, int count) {
    if (!list)
        return;
    for (int i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

static void catalog_build_clear(RuntimeCatalogBuild *build) {
    if (!build)
        return;
    examples_free_entries(build->entries, build->count);
    string_list_clear(build->sections, build->section_count);
    string_list_clear(build->files, build->file_count);
    memset(build, 0, sizeof(*build));
}

static int string_list_append(char ***list, int *count, int *cap, char *item) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 8 : (*cap * 2);
        char **next = (char **)realloc(*list, (size_t)new_cap * sizeof(char *));
        if (!next)
            return 0;
        *list = next;
        *cap = new_cap;
    }
    (*list)[(*count)++] = item;
    return 1;
}

static int catalog_build_append_entry(RuntimeCatalogBuild *build,
                                       const ReplExampleEntry *entry) {
    if (build->count >= build->cap) {
        int new_cap = (build->cap == 0) ? 8 : (build->cap * 2);
        ReplExampleEntry *next = (ReplExampleEntry *)realloc(
            build->entries, (size_t)new_cap * sizeof(ReplExampleEntry));
        if (!next)
            return 0;
        build->entries = next;
        build->cap = new_cap;
    }
    build->entries[build->count++] = *entry;
    return 1;
}

static int catalog_section_seen(const RuntimeCatalogBuild *build,
                                const char *section) {
    for (int i = 0; i < build->section_count; i++)
        if (strcmp(build->sections[i], section) == 0)
            return 1;
    return 0;
}

static int catalog_name_seen(const RuntimeCatalogBuild *build,
                             const char *name) {
    for (int i = 0; i < build->count; i++)
        if (examples_streq_ci(build->entries[i].name, name))
            return 1;
    return 0;
}

static int catalog_file_seen(const RuntimeCatalogBuild *build,
                             const char *file_real) {
    for (int i = 0; i < build->file_count; i++)
        if (strcmp(build->files[i], file_real) == 0)
            return 1;
    return 0;
}

static int examples_parse_tags(const char *section,
                               const char *tags_str,
                               ReplItemTagNode **item_tags_out,
                               char *err_buf,
                               int err_sz) {
    if (!tags_str || !*tags_str) {
        examples_set_error(err_buf, err_sz, "[%s] tags must not be empty", section);
        return 0;
    }

    char *scratch = examples_strdup(tags_str);
    if (!scratch) {
        examples_set_error(err_buf, err_sz, "out of memory");
        return 0;
    }

    ReplItemTagNode *item_tags = NULL;
    int tag_count = 0;
    char *p = scratch;

    while (p && *p) {
        char *comma = strchr(p, ',');
        if (comma)
            *comma = '\0';
        char *tag_name = examples_trim(p);
        if (*tag_name) {
            if (strcmp(tag_name, "All") == 0) {
                examples_set_error(err_buf, err_sz,
                                   "[%s] must not list synthetic tag All",
                                   section);
                repl_example_free_item_tags(item_tags);
                free(scratch);
                return 0;
            }
            const ReplTagNode *tag = repl_example_find_or_register_tag(tag_name);
            if (!tag) {
                examples_set_error(err_buf, err_sz, "out of memory");
                repl_example_free_item_tags(item_tags);
                free(scratch);
                return 0;
            }
            if (!repl_example_attach_tag(&item_tags, tag)) {
                examples_set_error(err_buf, err_sz, "out of memory");
                repl_example_free_item_tags(item_tags);
                free(scratch);
                return 0;
            }
            tag_count++;
        }
        p = comma ? comma + 1 : NULL;
    }
    free(scratch);

    if (tag_count == 0) {
        examples_set_error(err_buf, err_sz, "[%s] tags must not be empty", section);
        return 0;
    }
    *item_tags_out = item_tags;
    return 1;
}

static char **examples_read_source_lines(const char *path,
                                         char *err_buf,
                                         int err_sz) {
    FILE *f = fopen(path, "r");
    if (!f) {
        examples_set_error(err_buf, err_sz, "cannot read %s: %s",
                           path, strerror(errno));
        return NULL;
    }

    char **lines = NULL;
    int count = 0;
    int cap = 0;
    char line[MAX_LINE_LEN];
    int line_no = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        size_t raw_len = strlen(line);
        if (raw_len > 0 &&
            line[raw_len - 1] != '\n' &&
            line[raw_len - 1] != '\r' &&
            !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            examples_set_error(err_buf, err_sz,
                               "%s:%d line exceeds %d chars",
                               path, line_no, (int)MAX_LINE_LEN - 1);
            fclose(f);
            for (int i = 0; i < count; i++)
                free(lines[i]);
            free(lines);
            return NULL;
        }

        int len = (int)raw_len;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *copy = examples_strdup(line);
        if (!copy) {
            examples_set_error(err_buf, err_sz, "out of memory");
            fclose(f);
            for (int i = 0; i < count; i++)
                free(lines[i]);
            free(lines);
            return NULL;
        }
        if (!string_list_append(&lines, &count, &cap, copy)) {
            free(copy);
            examples_set_error(err_buf, err_sz, "out of memory");
            fclose(f);
            for (int i = 0; i < count; i++)
                free(lines[i]);
            free(lines);
            return NULL;
        }
    }

    int had_read_err = ferror(f);
    int close_failed = fclose(f) != 0;
    if (had_read_err || close_failed) {
        examples_set_error(err_buf, err_sz, "cannot read %s", path);
        for (int i = 0; i < count; i++)
            free(lines[i]);
        free(lines);
        return NULL;
    }
    if (count == 0) {
        examples_set_error(err_buf, err_sz, "%s must not be empty", path);
        free(lines);
        return NULL;
    }

    char *sentinel = NULL;
    if (!string_list_append(&lines, &count, &cap, sentinel)) {
        examples_set_error(err_buf, err_sz, "out of memory");
        for (int i = 0; i < count; i++)
            free(lines[i]);
        free(lines);
        return NULL;
    }
    return lines;
}

static int catalog_file_under_base(const char *file_real,
                                   const char *base_real) {
    size_t n = strlen(base_real);
    return strncmp(file_real, base_real, n) == 0 &&
           file_real[n] == '/';
}

static int catalog_finalize_draft(RuntimeCatalogBuild *build,
                                   RuntimeCatalogDraft *draft,
                                   const char *base_dir,
                                   char *err_buf,
                                   int err_sz) {
    if (!draft->section)
        return 1;
    if (!draft->file || !draft->name || !draft->tags || !draft->group) {
        examples_set_error(err_buf, err_sz,
                           "[%s] missing required key(s)", draft->section);
        return 0;
    }
    char *name = examples_trim(draft->name);
    char *group = examples_trim(draft->group);
    char *file = examples_trim(draft->file);
    char *tags = examples_trim(draft->tags);
    if (!*name || !*group || !*file || !*tags) {
        examples_set_error(err_buf, err_sz,
                           "[%s] file/name/tags/group must not be empty",
                           draft->section);
        return 0;
    }
    if (catalog_name_seen(build, name)) {
        examples_set_error(err_buf, err_sz,
                           "[%s] duplicate example name '%s'",
                           draft->section, name);
        return 0;
    }
    if (file[0] == '/') {
        examples_set_error(err_buf, err_sz,
                           "[%s] file must be relative to the examples dir",
                           draft->section);
        return 0;
    }

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", base_dir, file) >=
        (int)sizeof(joined)) {
        examples_set_error(err_buf, err_sz, "[%s] file path too long",
                           draft->section);
        return 0;
    }
    char file_real[PATH_MAX];
    if (!realpath(joined, file_real)) {
        examples_set_error(err_buf, err_sz,
                           "[%s] missing scene file: %s",
                           draft->section, file);
        return 0;
    }
    if (!catalog_file_under_base(file_real, base_dir)) {
        examples_set_error(err_buf, err_sz,
                           "[%s] file must live under the examples dir",
                           draft->section);
        return 0;
    }
    if (catalog_file_seen(build, file_real)) {
        examples_set_error(err_buf, err_sz,
                           "[%s] duplicate scene file: %s",
                           draft->section, file);
        return 0;
    }

    ReplExampleSourceFormat format;
    if (examples_has_suffix(file_real, ".glr")) {
        format = REPL_EXAMPLE_SOURCE_GLR;
    } else if (examples_has_suffix(file_real, ".c")) {
        format = REPL_EXAMPLE_SOURCE_C;
    } else {
        examples_set_error(err_buf, err_sz,
                           "[%s] file must have a .glr or .c extension",
                           draft->section);
        return 0;
    }

    ReplItemTagNode *item_tags = NULL;
    if (!examples_parse_tags(draft->section, tags, &item_tags, err_buf, err_sz))
        return 0;

    char **lines = examples_read_source_lines(file_real, err_buf, err_sz);
    if (!lines) {
        repl_example_free_item_tags(item_tags);
        return 0;
    }

    ReplExampleEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = examples_strdup(name);
    entry.subheading = examples_strdup(group);
    entry.lines = (const char *const *)lines;
    entry.tag_names = NULL;
    entry.tags = item_tags;
    entry.format = format;
    if (!entry.name || !entry.subheading) {
        examples_set_error(err_buf, err_sz, "out of memory");
        examples_free_entry(&entry);
        return 0;
    }

    char *file_real_owned = examples_strdup(file_real);
    if (!file_real_owned ||
        !string_list_append(&build->files, &build->file_count,
                            &build->file_cap, file_real_owned)) {
        free(file_real_owned);
        examples_set_error(err_buf, err_sz, "out of memory");
        examples_free_entry(&entry);
        return 0;
    }
    if (!catalog_build_append_entry(build, &entry)) {
        examples_set_error(err_buf, err_sz, "out of memory");
        examples_free_entry(&entry);
        return 0;
    }
    return 1;
}

static int catalog_key_known(const char *key) {
    return strcmp(key, "file") == 0 ||
           strcmp(key, "name") == 0 ||
           strcmp(key, "tags") == 0 ||
           strcmp(key, "group") == 0;
}

static char **catalog_draft_key_slot(RuntimeCatalogDraft *draft,
                                     const char *key) {
    if (strcmp(key, "file") == 0)
        return &draft->file;
    if (strcmp(key, "name") == 0)
        return &draft->name;
    if (strcmp(key, "tags") == 0)
        return &draft->tags;
    if (strcmp(key, "group") == 0)
        return &draft->group;
    return NULL;
}

int repl_examples_load_dir(const char *dir, char *err_buf, int err_sz) {
    if (err_buf && err_sz > 0)
        err_buf[0] = '\0';
    if (!dir || !*dir) {
        examples_set_error(err_buf, err_sz, "examples dir is empty");
        return 0;
    }

    char base_real[PATH_MAX];
    if (!realpath(dir, base_real)) {
        examples_set_error(err_buf, err_sz, "cannot read examples dir %s: %s",
                           dir, strerror(errno));
        return 0;
    }

    char catalog_path[PATH_MAX];
    if (snprintf(catalog_path, sizeof(catalog_path), "%s/catalog.ini",
                  base_real) >= (int)sizeof(catalog_path)) {
        examples_set_error(err_buf, err_sz, "catalog path too long");
        return 0;
    }
    FILE *f = fopen(catalog_path, "r");
    if (!f) {
        examples_set_error(err_buf, err_sz, "cannot read %s: %s",
                           catalog_path, strerror(errno));
        return 0;
    }

    RuntimeCatalogBuild build;
    RuntimeCatalogDraft draft;
    memset(&build, 0, sizeof(build));
    memset(&draft, 0, sizeof(draft));

    /* Reset dynamic tags before loading new catalog */
    clear_dynamic_tags_list();
    ensure_default_tags_initialized();

    char line[MAX_LINE_LEN];
    int line_no = 0;
    int ok = 1;
    while (ok && fgets(line, sizeof(line), f)) {
        line_no++;
        size_t raw_len = strlen(line);
        if (raw_len > 0 &&
            line[raw_len - 1] != '\n' &&
            line[raw_len - 1] != '\r' &&
            !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            examples_set_error(err_buf, err_sz,
                               "%s:%d line exceeds %d chars",
                               catalog_path, line_no, (int)MAX_LINE_LEN - 1);
            ok = 0;
            break;
        }
        int len = (int)raw_len;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *p = examples_trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) {
                examples_set_error(err_buf, err_sz,
                                   "%s:%d missing closing ]",
                                   catalog_path, line_no);
                ok = 0;
                break;
            }
            *close = '\0';
            char *tail = examples_trim(close + 1);
            char *section = examples_trim(p + 1);
            if (*tail || !*section) {
                examples_set_error(err_buf, err_sz,
                                   "%s:%d invalid section header",
                                   catalog_path, line_no);
                ok = 0;
                break;
            }
            if (!catalog_finalize_draft(&build, &draft, base_real,
                                        err_buf, err_sz)) {
                ok = 0;
                break;
            }
            catalog_draft_clear(&draft);
            if (catalog_section_seen(&build, section)) {
                examples_set_error(err_buf, err_sz,
                                   "duplicate section [%s]", section);
                ok = 0;
                break;
            }
            draft.section = examples_strdup(section);
            char *section_owned = examples_strdup(section);
            if (!draft.section || !section_owned ||
                !string_list_append(&build.sections, &build.section_count,
                                    &build.section_cap, section_owned)) {
                free(section_owned);
                examples_set_error(err_buf, err_sz, "out of memory");
                ok = 0;
                break;
            }
            continue;
        }

        if (!draft.section) {
            examples_set_error(err_buf, err_sz,
                               "%s:%d key outside section",
                               catalog_path, line_no);
            ok = 0;
            break;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            examples_set_error(err_buf, err_sz,
                               "%s:%d expected key = value",
                               catalog_path, line_no);
            ok = 0;
            break;
        }
        *eq = '\0';
        char *key = examples_trim(p);
        char *value = examples_trim(eq + 1);
        if (!catalog_key_known(key)) {
            examples_set_error(err_buf, err_sz,
                               "[%s] unknown key '%s'", draft.section, key);
            ok = 0;
            break;
        }
        char **slot = catalog_draft_key_slot(&draft, key);
        if (*slot) {
            examples_set_error(err_buf, err_sz,
                               "[%s] duplicate key '%s'", draft.section, key);
            ok = 0;
            break;
        }
        *slot = examples_strdup(value);
        if (!*slot) {
            examples_set_error(err_buf, err_sz, "out of memory");
            ok = 0;
            break;
        }
    }

    int had_read_err = ferror(f);
    int close_failed = fclose(f) != 0;
    if (ok && (had_read_err || close_failed)) {
        examples_set_error(err_buf, err_sz, "cannot read %s", catalog_path);
        ok = 0;
    }
    if (ok && !catalog_finalize_draft(&build, &draft, base_real,
                                      err_buf, err_sz))
        ok = 0;
    catalog_draft_clear(&draft);
    if (ok && build.count == 0) {
        examples_set_error(err_buf, err_sz, "catalog must contain examples");
        ok = 0;
    }

    if (!ok) {
        catalog_build_clear(&build);
        return 0;
    }

    examples_free_entries(g_runtime_example_entries, g_runtime_example_count);
    g_runtime_example_entries = build.entries;
    g_runtime_example_count = build.count;
    build.entries = NULL;
    build.count = 0;
    catalog_build_clear(&build);
    return 1;
}

static int active_example_count(void) {
    return g_runtime_example_entries
        ? g_runtime_example_count
        : (int)(sizeof(g_example_entries) / sizeof(g_example_entries[0]));
}

static const ReplExampleEntry *active_example_entry(int idx) {
    int count = active_example_count();
    if (idx < 0 || idx >= count)
        return NULL;
    return g_runtime_example_entries
        ? &g_runtime_example_entries[idx]
        : &g_example_entries[idx];
}

static int example_catalog_entry_count(void) {
    return repl_example_count();
}

static int example_catalog_tag_count(void) {
    return repl_example_tag_count();
}

static const char *example_catalog_tag_label(int tag_idx) {
    ensure_default_tags_initialized();
    if (tag_idx < 0 || tag_idx >= g_example_tag_count)
        return NULL;
    for (const ReplTagNode *curr = g_example_tags_head; curr; curr = curr->next) {
        if (curr->id == tag_idx)
            return curr->name;
    }
    return NULL;
}

static int example_catalog_entry_has_tag(int example_idx, int tag_idx) {
    if (tag_idx == 0) /* "All" */
        return 1;
    const ReplExampleEntry *entry = active_example_entry(example_idx);
    if (!entry)
        return 0;

    if (entry->tags) {
        for (const ReplItemTagNode *curr = entry->tags; curr; curr = curr->next) {
            if (curr->tag && curr->tag->id == tag_idx)
                return 1;
        }
        return 0;
    }

    if (entry->tag_names) {
        const char *label = repl_example_tag_label(tag_idx);
        if (!label)
            return 0;
        for (int i = 0; entry->tag_names[i]; i++) {
            if (strcmp(entry->tag_names[i], label) == 0)
                return 1;
        }
    }
    return 0;
}

static const ReplCatalogTagOps g_example_tag_ops = {
    example_catalog_entry_count,
    example_catalog_tag_count,
    example_catalog_tag_label,
    example_catalog_entry_has_tag,
};

int repl_example_count(void) {
    return active_example_count();
}

const char *repl_example_name(int idx) {
    const ReplExampleEntry *entry = active_example_entry(idx);
    return entry ? entry->name : NULL;
}

const char *const *repl_example_lines(int idx) {
    const ReplExampleEntry *entry = active_example_entry(idx);
    return entry ? entry->lines : NULL;
}

ReplExampleSourceFormat repl_example_source_format(int idx) {
    const ReplExampleEntry *entry = active_example_entry(idx);
    return entry ? entry->format : REPL_EXAMPLE_SOURCE_GLR;
}

int repl_example_tag_count(void) {
    ensure_default_tags_initialized();
    return g_example_tag_count;
}

REPL_DEFINE_CATALOG_TAG_WRAPPERS(example, &g_example_tag_ops)

const char *repl_example_subheading(int example_idx) {
    const ReplExampleEntry *entry = active_example_entry(example_idx);
    return entry ? entry->subheading : NULL;
}
