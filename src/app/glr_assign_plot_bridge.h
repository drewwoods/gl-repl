/*
 * glr_assign_plot_bridge.h - REPL-backed AssignPlotHostBridge installer.
 */
#ifndef GLR_ASSIGN_PLOT_BRIDGE_H
#define GLR_ASSIGN_PLOT_BRIDGE_H

/* Point the assignment plot at the live flat program + document. Idempotent;
 * called from glr_ctrl_install_app_services. */
void glr_assign_plot_install_host(void);

#endif /* GLR_ASSIGN_PLOT_BRIDGE_H */
