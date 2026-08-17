/*
 * console.h - Console trace line capture and state for the REPL console panel.
 *
 * A debugging surface for `console("fmt", ...)` trace lines.
 * Captured once per frame via a read-only scan of the flat program over
 * [0, g_frame_replay_exec_limit).
 */
#ifndef REPL_SUBSYSTEMS_CONSOLE_H
#define REPL_SUBSYSTEMS_CONSOLE_H

#include "repl/command.h"

#ifndef MAX_CONSOLE_LINES
#define MAX_CONSOLE_LINES 256
#endif

#ifndef CONSOLE_LINE_TEXT_MAX
#define CONSOLE_LINE_TEXT_MAX 256
#endif

typedef struct {
    char text[CONSOLE_LINE_TEXT_MAX];
    int  call_depth;
    int  src_cmd_idx;
} ConsoleLine;

typedef struct {
    int                open;
    int                count;           /* Number of valid lines in lines[] (<= MAX_CONSOLE_LINES) */
    int                total_count;     /* Total number of console commands encountered in frame */
    int                overflow_count;  /* Lines dropped due to MAX_CONSOLE_LINES cap */
    const ConsoleLine *lines;
} ConsoleView;

/* Visibility / State */
void        console_open(void);
void        console_close(void);
void        console_toggle(void);
int         console_is_open(void);
void        console_set_open(int open);
void        console_clear(void);
void        console_reset(void);

/* View accessor */
ConsoleView console_view(void);

/* Frame Capture: scans [0, exec_limit) of the flat command buffer and formats
 * each CMD_CONSOLE into the line buffer, auto-indented by call_depth. */
void        console_capture(const GLCmd *flat_cmds, int flat_count, int exec_limit);

#endif /* REPL_SUBSYSTEMS_CONSOLE_H */
