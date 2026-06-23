/*
 * src/repl/help_text.h - REPL-side producer of the F1 help overlay content.
 *
 * This module owns the actual text plus the dynamic F-key strings.
 * The app/controller layer adapts this neutral content into the
 * renderer-facing overlay shape, so neither the REPL text producer
 * nor the UI renderer includes the other.
 *
 * F-key bindings live in the controller's g_cfg_items table; this
 * module pulls labels through a controller-installed
 * ReplHelpFkeyProvider so help_text.c doesn't need to #include
 * app/glr_config.h (the layering inversion the prior baseline
 * tracked as audit #1). The standalone render3d_demo doesn't install a
 * provider — the F-Key section just renders empty there.
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

/* Look up the label bound to F<n> (where n is 2..10) in the app's
 * configuration vocabulary. Returns NULL when no binding exists.
 * The controller installs this so help_text.c does not need to
 * reach into src/app/. */
typedef struct {
    const char *(*fkey_label)(int fn);
} ReplHelpFkeyProvider;

/* Install the F-key label provider. Pass NULL to deinstall (e.g.
 * from a standalone driver that has no config menu). Cheap; safe
 * to call from app init. */
void repl_help_text_install_fkey_provider(const ReplHelpFkeyProvider *provider);

/* Returns a pointer to the assembled help content. The pointer
 * references file-static storage and stays valid until the next call;
 * callers should not retain it across frames if they expect the
 * dynamic F-key strings to refresh. */
const ReplHelpContent *repl_help_text_build(void);

#endif /* REPL_HELP_TEXT_H */
