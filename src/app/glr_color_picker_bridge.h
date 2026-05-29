/*
 * glr_color_picker_bridge.h - REPL/editor-backed host for the color picker.
 *
 * Installs the ColorPickerHostBridge the floating color picker reads the
 * document and screen geometry through. Keeps the color_picker subsystem
 * free of repl/editor/ui-app so it links standalone in color_picker_demo.
 */
#ifndef GLR_COLOR_PICKER_BRIDGE_H
#define GLR_COLOR_PICKER_BRIDGE_H

/* Install the host services (document read/write + viewport + code-panel
 * rect). Called once from the controller's app-service wiring. */
void glr_color_picker_install_host(void);

#endif /* GLR_COLOR_PICKER_BRIDGE_H */
