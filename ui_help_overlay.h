/*
 * ui_help_overlay.h - Modal help overlay (F1).
 *
 * Renders a full-screen semi-transparent modal overlay showing keyboard
 * bindings, command cheat sheet, and feature descriptions. Opened by pressing
 * F1 or via the help menu item. Provides quick reference for key shortcuts,
 * GL commands, math functions, and overlays without leaving the REPL.
 *
 * Modal behavior: When open, input is routed to help overlay (keyboard/mouse
 * navigation); ESC or clicking outside closes it. Only one overlay is modal
 * at a time (help, search, etc.); opening help closes other overlays. The
 * modal blocks interaction with the scene and code panel until dismissed.
 *
 * Content: Two main sections accessible via tabs:
 *   - Commands: GL commands, control structures (for/func/if), math functions,
 *     and constant names (GL_TRIANGLES, PI, TAU, etc.)
 *   - Keys: Keyboard shortcuts dynamically populated from repl_config.h
 *     (F1-F11 for overlays, Ctrl+keys for editing, etc.), organized by
 *     function (navigation, editing, save/load, overlays).
 *
 * Integration: repl_editor.c detects F1 key and calls ui_help_overlay_render()
 * to display the help. Input dispatch (keyboard/mouse) in repl_editor.c checks
 * if help is open before routing input elsewhere. Help content is read-only;
 * no state mutations occur.
 *
 * Data source: Help text is static, compiled into the binary. Keyboard
 * shortcuts are generated from repl_config.h item descriptors (label, key_code,
 * key, state_names) so they stay in sync with actual bindings.
 */
#ifndef UI_HELP_OVERLAY_H
#define UI_HELP_OVERLAY_H

/* Render the help overlay modal once per frame. Displays a full-screen
 * semi-transparent overlay with keyboard bindings, GL command reference,
 * math functions, and feature descriptions. Renders nothing if help is not
 * active. Input dispatch (keyboard/mouse) should check help state before
 * routing input. Called by repl_core.c during display callback if help
 * overlay is active. */
void ui_help_overlay_render(void);

#endif /* UI_HELP_OVERLAY_H */
