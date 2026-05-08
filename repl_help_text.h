/*
 * repl_help_text.h - REPL-side producer of the F1 help overlay content.
 *
 * The renderer (ui_tabbed_overlay.c) consumes a UiOverlayContent;
 * this module owns the actual text plus the dynamic F-key strings
 * that depend on g_cfg_items, so the UI side carries no REPL knowledge.
 */
#ifndef REPL_HELP_TEXT_H
#define REPL_HELP_TEXT_H

#include "ui/tabbed_overlay.h"

/* Returns a pointer to the assembled help content. The pointer
 * references file-static storage and stays valid until the next call;
 * callers should not retain it across frames if they expect the
 * dynamic F-key strings to refresh. */
const UiOverlayContent *repl_help_text_build(void);

#endif /* REPL_HELP_TEXT_H */
