/*
 * console.c - Implementation of REPL console trace line capture and state.
 */
#include "subsystems/console/console.h"
#include "repl/text_helpers.h"

#include <stdio.h>
#include <string.h>

static int g_console_open = 0;
static ConsoleLine g_console_lines[MAX_CONSOLE_LINES];
static int g_console_line_count = 0;
static int g_console_total_count = 0;
static int g_console_overflow_count = 0;

void console_open(void) {
    g_console_open = 1;
}

void console_close(void) {
    g_console_open = 0;
}

void console_toggle(void) {
    g_console_open = !g_console_open;
}

int console_is_open(void) {
    return g_console_open;
}

void console_set_open(int open) {
    g_console_open = open ? 1 : 0;
}

void console_clear(void) {
    g_console_line_count = 0;
    g_console_total_count = 0;
    g_console_overflow_count = 0;
}

void console_reset(void) {
    g_console_open = 0;
    console_clear();
}

ConsoleView console_view(void) {
    ConsoleView view;
    view.open = g_console_open;
    view.count = g_console_line_count;
    view.total_count = g_console_total_count;
    view.overflow_count = g_console_overflow_count;
    view.lines = g_console_lines;
    return view;
}

void console_capture(const GLCmd *flat_cmds, int flat_count, int exec_limit) {
    if (!g_console_open) {
        return;
    }

    g_console_line_count = 0;
    g_console_total_count = 0;
    g_console_overflow_count = 0;

    if (!flat_cmds || flat_count <= 0) {
        return;
    }

    int limit = exec_limit;
    if (limit < 0) limit = 0;
    if (limit > flat_count) limit = flat_count;

    for (int i = 0; i < limit; i++) {
        const GLCmd *cmd = &flat_cmds[i];
        if (!cmd->valid || cmd->type != CMD_CONSOLE) {
            continue;
        }

        g_console_total_count++;

        if (g_console_line_count < MAX_CONSOLE_LINES) {
            ConsoleLine *line = &g_console_lines[g_console_line_count];
            int depth = cmd->call_depth;
            if (depth < 0) depth = 0;
            if (depth > 20) depth = 20;

            int indent_spaces = depth * 2;
            int off = 0;
            for (int s = 0; s < indent_spaces && off < (int)sizeof(line->text) - 1; s++) {
                line->text[off++] = ' ';
            }
            line->text[off] = '\0';

            char formatted[CONSOLE_LINE_TEXT_MAX];
            repl_format_label_string(formatted, (int)sizeof(formatted),
                                     cmd->payload.label.fmt,
                                     cmd->args, cmd->num_args);

            snprintf(line->text + off, sizeof(line->text) - (size_t)off,
                     "%s", formatted);

            line->call_depth = depth;
            line->src_cmd_idx = i;
            g_console_line_count++;
        } else {
            g_console_overflow_count++;
        }
    }
}
