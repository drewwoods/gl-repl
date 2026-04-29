#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_state.h"

ReplCommandStore repl_command_store_live(void) {
    ReplDocumentState *document = repl_state_document_mut();
    ReplCommandStore store = {
        document->cmds,
        &document->cmd_count,
        document->capacity,
        &document->edit_line_idx
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

int repl_command_store_first_non_decl(const ReplCommandStore *store) {
    if (!store || !store->cmds || !store->count)
        return 0;

    int pos = 0;
    while (pos < *store->count &&
           store->cmds[pos].type == CMD_VAR_DECLARE)
        pos++;
    return pos;
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

static int clamp_edit_line_to_count(int edit_line, int count) {
    if (edit_line < 0)
        return 0;
    if (edit_line > count)
        return count;
    return edit_line;
}

static void command_store_invalidate_after_mutation(void) {
    repl_state_mark_normals_dirty();
}

static int repl_command_store_write_color_source(GLCmd *cmd,
                                                 float r, float g, float b,
                                                 float a, int has_alpha,
                                                 int is_clear_color) {
    int indent_len = 0;
    char indent_prefix[sizeof(cmd->source)];
    char new_source[sizeof(cmd->source)];
    float out_r = r;
    float out_g = g;
    float out_b = b;
    int formatted;

    while (cmd->source[indent_len] == ' ' || cmd->source[indent_len] == '\t')
        indent_len++;
    if ((size_t)indent_len >= sizeof(indent_prefix))
        indent_len = (int)sizeof(indent_prefix) - 1;
    for (int i = 0; i < indent_len; i++)
        indent_prefix[i] = cmd->source[i];
    indent_prefix[indent_len] = '\0';

    if (is_clear_color) {
        if (out_r > CP_CLEAR_MAX_V) out_r = CP_CLEAR_MAX_V;
        if (out_g > CP_CLEAR_MAX_V) out_g = CP_CLEAR_MAX_V;
        if (out_b > CP_CLEAR_MAX_V) out_b = CP_CLEAR_MAX_V;
        formatted = repl_format_fits(new_source, sizeof(new_source),
                                     "%sglClearColor(%g, %g, %g, %g);",
                                     indent_prefix, out_r, out_g, out_b, a);
    } else if (cmd->type == CMD_TESS_COLOR) {
        if (has_alpha) {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sgluColor(%g, %g, %g, %g);",
                                         indent_prefix, out_r, out_g, out_b, a);
        } else {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sgluColor(%g, %g, %g);",
                                         indent_prefix, out_r, out_g, out_b);
        }
    } else if (has_alpha) {
        formatted = repl_format_fits(new_source, sizeof(new_source),
                                     "%sglColor4f(%g, %g, %g, %g);",
                                     indent_prefix, out_r, out_g, out_b, a);
    } else {
        formatted = repl_format_fits(new_source, sizeof(new_source),
                                     "%sglColor3f(%g, %g, %g);",
                                     indent_prefix, out_r, out_g, out_b);
    }

    if (!formatted) {
        set_status("Command too long");
        return 0;
    }

    cmd->args[0] = out_r;
    cmd->args[1] = out_g;
    cmd->args[2] = out_b;
    cmd->num_args = has_alpha ? 4 : 3;
    if (has_alpha)
        cmd->args[3] = a;
    memcpy(cmd->source, new_source, strlen(new_source) + 1);
    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_set_color(int cmd_idx,
                                 float r, float g, float b, float a,
                                 int has_alpha) {
    GLCmd *cmd = repl_state_document_cmd_at_mut(cmd_idx);
    if (!cmd)
        return 0;
    if (cmd->type != CMD_COLOR3F && cmd->type != CMD_COLOR4F &&
        cmd->type != CMD_TESS_COLOR)
        return 0;
    return repl_command_store_write_color_source(cmd, r, g, b, a, has_alpha, 0);
}

int repl_command_store_set_clear_color(int cmd_idx,
                                       float r, float g, float b, float a) {
    GLCmd *cmd = repl_state_document_cmd_at_mut(cmd_idx);
    if (!cmd || cmd->type != CMD_CLEAR_COLOR)
        return 0;
    return repl_command_store_write_color_source(cmd, r, g, b, a, 1, 1);
}

int repl_command_store_insert_many(ReplCommandStore *store, int pos,
                                   const GLCmd *cmds, int count, int flags) {
    if (!store || !store->cmds || !store->count || !cmds || count <= 0)
        return 0;
    if (!repl_command_store_can_insert(store, count))
        return 0;

    pos = clamp_insert_pos(store, pos);
    memmove(&store->cmds[pos + count], &store->cmds[pos],
            (size_t)(*store->count - pos) * sizeof(store->cmds[0]));
    memcpy(&store->cmds[pos], cmds, (size_t)count * sizeof(store->cmds[0]));
    *store->count += count;

    if ((flags & REPL_COMMAND_STORE_ADJUST_EDIT_LINE) &&
        store->edit_line && *store->edit_line >= pos) {
        *store->edit_line += count;
    }

    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_insert_one(ReplCommandStore *store, int pos,
                                  const GLCmd *cmd, int flags) {
    return repl_command_store_insert_many(store, pos, cmd, 1, flags);
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
                                    int count) {
    if (!repl_command_store_normalize_range(store, start, count,
                                           &start, &count))
        return 0;

    memmove(&store->cmds[start], &store->cmds[start + count],
            (size_t)(*store->count - start - count) * sizeof(store->cmds[0]));
    *store->count -= count;
    command_store_invalidate_after_mutation();
    return 1;
}

int repl_command_store_load(ReplCommandStore *store, const GLCmd *cmds,
                            int count, int edit_line) {
    if (!store || !store->cmds || !store->count || count < 0)
        return 0;
    if (count > store->capacity)
        return 0;
    if (count > 0 && !cmds)
        return 0;

    if (count > 0)
        memcpy(store->cmds, cmds, (size_t)count * sizeof(store->cmds[0]));
    *store->count = count;
    if (store->edit_line)
        *store->edit_line = clamp_edit_line_to_count(edit_line, count);

    command_store_invalidate_after_mutation();
    return 1;
}

void repl_command_store_clear(ReplCommandStore *store) {
    if (!store || !store->count)
        return;
    *store->count = 0;
    command_store_invalidate_after_mutation();
}
