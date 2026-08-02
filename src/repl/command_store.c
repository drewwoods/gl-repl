#include <string.h>
#include "repl/state_notify.h"
#include "repl/command_store.h"
#include "repl/state_owners.h"

ReplCommandStore repl_command_store_live(void) {
    ReplDocumentState *document = repl_state_document_mut();
    ReplCommandStore store = {
        document->cmds,
        &document->cmd_count,
        document->capacity,
    };
    return store;
}

int repl_command_store_count(const ReplCommandStore *store) {
    return store && store->count ? *store->count : 0;
}

int repl_command_store_capacity(const ReplCommandStore *store) {
    return store ? store->capacity : 0;
}

int repl_command_store_can_insert(const ReplCommandStore *store, int count) {
    if (!store || !store->cmds || !store->count || count < 0)
        return 0;
    return *store->count + count <= store->capacity;
}

int repl_command_store_normalize_range(const ReplCommandStore *store,
                                       int start, int count,
                                       int *out_start, int *out_count) {
    if (!store || !store->count || !out_start || !out_count)
        return 0;
    if (count <= 0 || start < 0 || start >= *store->count)
        return 0;
    if (start + count > *store->count)
        count = *store->count - start;
    *out_start = start;
    *out_count = count;
    return 1;
}

static int clamp_insert_pos(const ReplCommandStore *store, int pos) {
    int count = repl_command_store_count(store);
    if (pos < 0)
        return 0;
    if (pos > count)
        return count;
    return pos;
}

static void command_store_invalidate_after_mutation(void) {
    repl_state_mark_source_dirty();
}

int repl_command_store_insert_many(ReplCommandStore *store, int pos,
                                   const GLCmd *cmds, int count,
                                   const ReplStoreMutOpts *opts) {
    if (!store || !store->cmds || !store->count || !cmds || count <= 0)
        return 0;
    if (!repl_command_store_can_insert(store, count))
        return 0;

    pos = clamp_insert_pos(store, pos);
    memmove(&store->cmds[pos + count], &store->cmds[pos],
            (size_t)(*store->count - pos) * sizeof(store->cmds[0]));
    memcpy(&store->cmds[pos], cmds, (size_t)count * sizeof(store->cmds[0]));
    *store->count += count;

    if (opts && opts->cursor_inout &&
        (opts->flags & REPL_COMMAND_STORE_ADJUST_EDIT_LINE) &&
        *opts->cursor_inout >= pos) {
        *opts->cursor_inout += count;
    }

    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_insert_one(ReplCommandStore *store, int pos,
                                  const GLCmd *cmd,
                                  const ReplStoreMutOpts *opts) {
    return repl_command_store_insert_many(store, pos, cmd, 1, opts);
}

int repl_command_store_replace_one(ReplCommandStore *store, int pos,
                                   const GLCmd *cmd) {
    if (!store || !store->cmds || !store->count || !cmd)
        return 0;
    if (pos < 0 || pos >= *store->count)
        return 0;

    store->cmds[pos] = *cmd;
    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_delete_range(ReplCommandStore *store, int start,
                                    int count,
                                    const ReplStoreMutOpts *opts) {
    if (!repl_command_store_normalize_range(store, start, count,
                                           &start, &count))
        return 0;

    memmove(&store->cmds[start], &store->cmds[start + count],
            (size_t)(*store->count - start - count) * sizeof(store->cmds[0]));
    *store->count -= count;

    if (opts && opts->cursor_inout) {
        int c = *opts->cursor_inout;
        if (c < start) {
            /* before the deleted range - unchanged */
        } else if (c < start + count) {
            /* inside the deleted range - snap to start */
            c = start;
        } else {
            /* past the deleted range - shift left by count */
            c -= count;
        }
        if (c < 0) c = 0;
        if (c > *store->count) c = *store->count;
        *opts->cursor_inout = c;
    }

    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_load(ReplCommandStore *store,
                            const GLCmd *cmds, int count) {
    if (!store || !store->cmds || !store->count || count < 0)
        return 0;
    if (count > store->capacity)
        return 0;
    if (count > 0 && !cmds)
        return 0;

    if (count > 0)
        memcpy(store->cmds, cmds, (size_t)count * sizeof(store->cmds[0]));
    *store->count = count;

    command_store_invalidate_after_mutation();
    return 1;
}

void repl_command_store_clear(ReplCommandStore *store) {
    if (!store || !store->count)
        return;
    *store->count = 0;
    command_store_invalidate_after_mutation();
}
