#ifndef REPL_CATALOG_TAGS_H
#define REPL_CATALOG_TAGS_H

#include <limits.h>  /* CHAR_BIT */
#include <stddef.h>

typedef struct {
    int (*entry_count)(void);
    int (*tag_count)(void);
    const char *const *tag_labels;
    unsigned int (*entry_tag_mask)(int entry_idx);
    unsigned int (*tag_bit)(int tag_idx);
} ReplCatalogTagOps;

/* Catalog-side tag-bit helper. Both per-catalog tag_bit() functions
 * (repl_example_tag_bit, repl_tutorial_tag_bit) reduce to this exact
 * body — caller passes the catalog's tag_count to keep the helper
 * free of any catalog-specific lookups. */
static inline unsigned int repl_catalog_tag_bit_for_count(int tag_idx,
                                                          int tag_count) {
    if (tag_idx < 0 || tag_idx >= tag_count ||
        tag_idx >= (int)(sizeof(unsigned int) * CHAR_BIT))
        return 0u;
    return 1u << (unsigned int)tag_idx;
}

/* Define the six standard per-catalog tag wrappers in one go. Each
 * delegates to the corresponding `repl_catalog_*` helper bound to the
 * caller's ops struct. Use as
 *
 *   REPL_DEFINE_CATALOG_TAG_WRAPPERS(example,  &g_example_tag_ops)
 *   REPL_DEFINE_CATALOG_TAG_WRAPPERS(tutorial, &g_tutorial_tag_ops)
 *
 * Function-pointer arguments must match the prototypes declared in
 * the catalog's header (examples.h, tutorials.h). The macro's
 * synthesized parameter names are deliberately generic — C only
 * matches signatures at the function level, not by parameter
 * name. */
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
    if (!ops || !ops->tag_count || !ops->tag_labels)
        return NULL;
    if (tag_idx < 0 || tag_idx >= ops->tag_count())
        return NULL;
    return ops->tag_labels[tag_idx];
}

static inline int repl_catalog_has_tag(const ReplCatalogTagOps *ops,
                                       int entry_idx,
                                       int tag_idx) {
    unsigned int bit;

    if (!ops || !ops->entry_count || !ops->entry_tag_mask || !ops->tag_bit)
        return 0;
    if (entry_idx < 0 || entry_idx >= ops->entry_count())
        return 0;

    bit = ops->tag_bit(tag_idx);
    if (!bit)
        return 0;
    return (ops->entry_tag_mask(entry_idx) & bit) != 0u;
}

static inline int repl_catalog_count_for_tag(const ReplCatalogTagOps *ops,
                                             int tag_idx) {
    int count = 0;

    if (!ops || !ops->entry_count || !ops->tag_bit)
        return 0;
    if (!ops->tag_bit(tag_idx))
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

    if (!ops || !ops->entry_count || !ops->tag_bit)
        return -1;
    if (ordinal < 0 || !ops->tag_bit(tag_idx))
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

    if (!ops || !ops->tag_count)
        return -1;
    if (dense_idx < 0)
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