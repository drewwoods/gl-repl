/*
 * config.h - Project-wide compile-time configuration constants.
 *
 * Home for shared, namespace-agnostic limits and sizes used across
 * REPL / editor / UI / scene boundaries — anywhere a single number
 * needs to be visible to multiple subsystems without dragging in
 * domain-specific headers.
 *
 * NOTE: This file is for *compile-time* configuration. User-toggleable
 * runtime settings (wireframe / grid theme / etc.) live on
 * `repl_config.h` and the `g_cfg_items[]` descriptor table in
 * `glr_actions.c` — different concept, do not conflate.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

/* Status / diagnostic text buffer size. Compile entries write into
 * `err[REPL_STATUS_TEXT_MAX]`; ReplCompiledChange.commit_message and
 * EditorCommitResult.diagnostic carry the same width; the visible
 * status bar in `ReplStatusState` mirrors the size end-to-end. */
#define REPL_STATUS_TEXT_MAX 256

#endif /* CONFIG_H */