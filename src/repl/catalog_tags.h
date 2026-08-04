#ifndef REPL_CATALOG_TAGS_H
#define REPL_CATALOG_TAGS_H

#include <stddef.h>

/* Dynamic Tag Linked List Node for registered tags in a catalog */
typedef struct ReplTagNode {
    char name[64];
    int id;                        /* 0-based tag index in catalog */
    struct ReplTagNode *next;
} ReplTagNode;

/* Node for per-item linked list of attached tags */
typedef struct ReplItemTagNode {
    const ReplTagNode *tag;
    struct ReplItemTagNode *next;
} ReplItemTagNode;

typedef struct {
    int (*entry_count)(void);
    int (*tag_count)(void);
    const char *(*tag_label)(int tag_idx);
    int (*entry_has_tag)(int entry_idx, int tag_idx);
} ReplCatalogTagOps;

#define REPL_DEFINE_CATALOG_TAG_WRAPPERS(prefix, ops_ptr)                \
    const char *repl_##prefix##_tag_label(int tag_idx) {                 \
        return repl_catalog_tag_label((ops_ptr), tag_idx);               \
    }                                                                    \
    int repl_##prefix##_has_tag(int entry_idx, int tag_idx) {            \
        return repl_catalog_has_tag((ops_ptr), entry_idx, tag_idx);      \
    }                                                                    \
    int repl_##prefix##_count_for_tag(int tag_idx) {                     \
        return repl_catalog_count_for_tag((ops_ptr), tag_idx);           \
    }                                                                    \
    int repl_##prefix##_index_for_tag(int tag_idx, int ordinal) {        \
        return repl_catalog_index_for_tag((ops_ptr), tag_idx, ordinal);  \
    }                                                                    \
    int repl_##prefix##_visible_tag_count(void) {                        \
        return repl_catalog_visible_tag_count(ops_ptr);                  \
    }                                                                    \
    int repl_##prefix##_visible_tag_at(int dense_idx) {                  \
        return repl_catalog_visible_tag_at((ops_ptr), dense_idx);        \
    }

static inline const char *repl_catalog_tag_label(const ReplCatalogTagOps *ops,
                                                 int tag_idx) {
    if (!ops || !ops->tag_count || !ops->tag_label)
        return NULL;
    if (tag_idx < 0 || tag_idx >= ops->tag_count())
        return NULL;
    return ops->tag_label(tag_idx);
}

static inline int repl_catalog_has_tag(const ReplCatalogTagOps *ops,
                                       int entry_idx,
                                       int tag_idx) {
    if (!ops || !ops->entry_count || !ops->entry_has_tag || !ops->tag_count)
        return 0;
    if (entry_idx < 0 || entry_idx >= ops->entry_count())
        return 0;
    if (tag_idx < 0 || tag_idx >= ops->tag_count())
        return 0;
    return ops->entry_has_tag(entry_idx, tag_idx);
}

static inline int repl_catalog_count_for_tag(const ReplCatalogTagOps *ops,
                                             int tag_idx) {
    int count = 0;

    if (!ops || !ops->entry_count)
        return 0;

    for (int idx = 0; idx < ops->entry_count(); idx++)
        if (repl_catalog_has_tag(ops, idx, tag_idx))
            count++;
    return count;
}

static inline int repl_catalog_index_for_tag(const ReplCatalogTagOps *ops,
                                             int tag_idx,
                                             int ordinal) {
    int seen = 0;

    if (!ops || !ops->entry_count || ordinal < 0)
        return -1;

    for (int idx = 0; idx < ops->entry_count(); idx++) {
        if (!repl_catalog_has_tag(ops, idx, tag_idx))
            continue;
        if (seen == ordinal)
            return idx;
        seen++;
    }
    return -1;
}

static inline int repl_catalog_visible_tag_count(const ReplCatalogTagOps *ops) {
    int count = 0;

    if (!ops || !ops->tag_count)
        return 0;

    for (int tag_idx = 0; tag_idx < ops->tag_count(); tag_idx++)
        if (repl_catalog_count_for_tag(ops, tag_idx) > 0)
            count++;
    return count;
}

static inline int repl_catalog_visible_tag_at(const ReplCatalogTagOps *ops,
                                              int dense_idx) {
    int seen = 0;

    if (!ops || !ops->tag_count || dense_idx < 0)
        return -1;

    for (int tag_idx = 0; tag_idx < ops->tag_count(); tag_idx++) {
        if (repl_catalog_count_for_tag(ops, tag_idx) <= 0)
            continue;
        if (seen == dense_idx)
            return tag_idx;
        seen++;
    }
    return -1;
}

#endif
