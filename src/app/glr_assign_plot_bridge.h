/*
 * glr_assign_plot_bridge.h - REPL-backed AssignPlotHostBridge installer.
 */
#ifndef GLR_ASSIGN_PLOT_BRIDGE_H
#define GLR_ASSIGN_PLOT_BRIDGE_H

/* Point the assignment plot at the live flat program + document. Idempotent;
 * called from glr_ctrl_install_app_services. */
void glr_assign_plot_install_host(void);

/* Re-target the plot from the document's `// @plot` tags after a wholesale
 * document replacement (example / scene / workspace / file load, reset); a
 * no-op on every other call, so the frame path can call it unconditionally.
 *
 * Call it with the flat program current: whether a second tagged row can join
 * the first is judged against that program's execution counts, and a stale one
 * would refuse a compatible pair. */
void glr_assign_plot_sync_tags(void);

#endif /* GLR_ASSIGN_PLOT_BRIDGE_H */
