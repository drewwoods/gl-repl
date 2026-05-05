#!/bin/bash
# Hard guard: ui_panels.c is hit-test-only.
#
# After Phase J2.2 every code-panel input side effect (cursor / blink
# reset / autocomplete clear / clipboard selection / replay pin /
# search / menu open-close-activate / color picker open-close-press-
# motion-release / undo push) is dispatched by imrepl_ctrl on the
# UiHit returned from ui_panels_hit_test. ui_panels.c carries no
# imperative input forwarding bodies — only rendering, the right-press
# config-menu wrapper, the menu close/open-config helpers, the
# scroll-follow test helper, and the pure hit-test composer.
#
# This guard greps ui_panels.c for any of the J2 acceptance symbols.
# Zero matches expected. The press wrapper, click helper, drag helper,
# release helper, scene-press, motion, and mouse-release forwarders
# were deleted outright in J2.2; if any of them reappear (or a new
# mutator slips in), the build fails.

set -euo pipefail

violations=$(grep -nE \
    'ui_panels_handle_code_panel_(press|click|drag|release)\b|ui_panels_handle_(scene_press|motion|mouse_release|escape)\b|color_picker_(open|close|handle_press|handle_motion|handle_release)\b|replay_handle_pin_clicked\b|handle_search_key\b|ui_menu_bar_(close|set_open_menu|activate_dropdown_item|note_search_opened)\b' \
    ui_panels.c 2>/dev/null \
    | grep -vE '^[0-9]+:[[:space:]]*(/\*|\*|//)' \
    || true)

if [ -n "$violations" ]; then
    echo "ERROR: ui_panels.c references a deleted input-dispatch symbol." >&2
    echo "ui_panels.c is hit-test only after Phase J2.2; the controller" >&2
    echo "(imrepl_ctrl) dispatches code-panel UiHits to the owning" >&2
    echo "subsystem. Press / click / drag / release / scene-press /" >&2
    echo "motion / mouse-release / escape forwarders + color picker" >&2
    echo "open/close/press/motion/release + replay pin + search +" >&2
    echo "menu open/close/activate calls must not reappear in" >&2
    echo "ui_panels.c." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "ui-panels-no-mutators OK"
