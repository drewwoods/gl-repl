/*
 * repl_help_text.h - REPL-side producer of the F1 help overlay content.
 *
 * This module owns the actual text plus the dynamic F-key strings that
 * depend on g_cfg_items. The app/controller layer adapts this neutral
 * content into the renderer-facing overlay shape, so neither the REPL
 * text producer nor the UI renderer includes the other.
 */
#ifndef REPL_HELP_TEXT_H
#define REPL_HELP_TEXT_H

typedef struct {
    const char        *label;
    const char *const *lines;
} ReplHelpTab;

typedef struct {
    const char        *title;
    const ReplHelpTab *tabs;
    int                tab_count;
} ReplHelpContent;

/* Returns a pointer to the assembled help content. The pointer
 * references file-static storage and stays valid until the next call;
 * callers should not retain it across frames if they expect the
 * dynamic F-key strings to refresh. */
const ReplHelpContent *repl_help_text_build(void);

#endif /* REPL_HELP_TEXT_H */
