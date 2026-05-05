#!/bin/bash
# Strict feature-UI guard for the floating color picker's UI surface
# (`color_picker_ui*.c`).
#
# Stricter than `check-replay-ui-isolation.sh`: where the replay HUD
# legitimately reads replay snapshots and routes via replay_handle_*,
# the picker UI is purely a renderer + hit-test over a frame view
# (`ColorPickerView`). It should not read live REPL/editor state
# directly, should not invoke parser/compile/apply or editor commit,
# and should not write status. The peer `color_picker.c` owns the
# state, lifecycle, and writeback.
#
# Forbidden in `color_picker_ui*.c`:
#   - repl_command_store_*( / editor_buffer_*(   (mutators)
#   - editor_commit_*( / editor_commit_apply_external_change(
#   - repl_compile_*( / repl_apply_*( / repl_parser_*(
#   - repl_state_*_mut( / editor_state_*_mut( / ui_state_*_mut(
#   - set_status(
#   - repl_state_*( / editor_state_*(            (live state reads)
#
# Allowed: GL / gl2d_* drawing, ColorPickerView field access,
# EditorTransformer field access, color_picker_hsv_to_rgb, ui_hit_*,
# ui_metrics.h / ui_layout.h constants, math / string helpers.

set -euo pipefail

shopt -s nullglob

targets=()
for candidate in color_picker_ui.c color_picker_ui_*.c; do
    [ -f "$candidate" ] && targets+=("$candidate")
done

if [ ${#targets[@]} -eq 0 ]; then
    echo "color-picker-ui-isolation OK (no target files)"
    exit 0
fi

violations=""
for f in "${targets[@]}"; do
    hits=$(grep -nE \
        'repl_command_store_[a-zA-Z_]+\(|editor_buffer_[a-zA-Z_]+\(|editor_commit_[a-zA-Z_]+\(|repl_compile_[a-zA-Z_]+\(|repl_apply_[a-zA-Z_]+\(|repl_parser_[a-zA-Z_]+\(|repl_state_[a-zA-Z_]+_mut\(|editor_state_[a-zA-Z_]+_mut\(|ui_state_[a-zA-Z_]+_mut\(|set_status\(|repl_state_[a-zA-Z_]+\(|editor_state_[a-zA-Z_]+\(' \
        "$f" 2>/dev/null \
        | grep -vE '^[0-9]+:[[:space:]]*(/\*|\*|//)' \
        || true)
    if [ -n "$hits" ]; then
        violations+="$f:"$'\n'"$hits"$'\n'
    fi
done

if [ -n "$violations" ]; then
    echo "ERROR: color_picker_ui*.c references a forbidden mutator or live state read." >&2
    echo "The picker UI is a pure renderer + hit-test over ColorPickerView." >&2
    echo "Mutations belong on the peer (color_picker.c); live state reads" >&2
    echo "should flow through the snapshot field UiRenderSnapshot.color_picker." >&2
    echo "Hits:" >&2
    printf '%s' "$violations" >&2
    exit 1
fi

# Renderer signature shape: each color_picker_ui_*_render must take a
# const ColorPickerView * (or Ui*State/View/Snapshot for
# legacy-shaped signatures, mirroring replay_ui).
sig_bad=0
for hdr in color_picker_ui.h color_picker_ui_*.h; do
    [ -e "$hdr" ] || continue
    while IFS= read -r line; do
        fn=$(printf '%s\n' "$line" \
             | grep -oE 'color_picker_ui_[a-zA-Z0-9_]+_render(_swatch)?' \
             | head -n1 || true)
        [ -z "$fn" ] && continue
        sig_line=$(grep -nE "^\s*void\s+${fn}\s*\(" color_picker_ui.h color_picker_ui.c color_picker_ui_*.h color_picker_ui_*.c 2>/dev/null | head -n1 || true)
        [ -z "$sig_line" ] && continue
        sig=${sig_line#*:*:}
        if ! printf '%s\n' "$sig" \
             | grep -Eq '^\s*void\s+'"$fn"'\s*\(\s*const\s+(ColorPickerView|EditorTransformer|Ui[A-Za-z0-9_]+(View|State|Snapshot))\s*\*'; then
            echo "ERROR: color-picker-ui renderer signature shape invalid for $fn" >&2
            echo "  found: $sig" >&2
            sig_bad=1
        fi
    done < <(grep -E '^\s*void\s+color_picker_ui_[a-zA-Z0-9_]+_render(_swatch)?\s*\(' "$hdr" || true)
done

if [ "$sig_bad" -ne 0 ]; then
    exit 1
fi

echo "color-picker-ui-isolation OK"
