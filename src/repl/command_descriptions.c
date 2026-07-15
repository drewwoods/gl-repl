#include "repl/command_descriptions.h"

#include <stddef.h>

typedef struct {
    const char *title;
    const char *body;
} ReplCommandDescriptionRecord;

typedef struct {
    GLenum value;
    const char *title;
    const char *body;
} ReplCapabilityDescriptionRecord;

#include "../../build/generated/repl_command_descriptions_data.inc"

int repl_command_description_lookup(const GLCmd *cmd,
                                    ReplCommandDescription *out) {
    const ReplCommandDescriptionRecord *record;
    size_t i;

    if (out) {
        out->title = NULL;
        out->body = NULL;
    }
    if (!cmd || !out || cmd->type < 0 || cmd->type >= CMD_TYPE_COUNT)
        return 0;

    if (cmd->type == CMD_ENABLE || cmd->type == CMD_DISABLE) {
        GLenum cap = (GLenum)cmd->args[0];
        for (i = 0;
             i < sizeof(g_repl_capability_description_records) /
                     sizeof(g_repl_capability_description_records[0]);
             i++) {
            const ReplCapabilityDescriptionRecord *cap_record =
                &g_repl_capability_description_records[i];
            if (cap_record->value == cap) {
                out->title = cap_record->title;
                out->body = cap_record->body;
                return 1;
            }
        }
        return 0;
    }

    record = &g_repl_command_description_records[cmd->type];
    if (!record->title || !record->body)
        return 0;
    out->title = record->title;
    out->body = record->body;
    return 1;
}
