#!/bin/bash
# Lighter feature-UI guard for the replay subsystem's UI surface
# (`replay_ui_*.c`).
#
# Per the prefix discipline rule in docs/MODULES.md ("2D UI rendering and
# hit-test"), feature-owned UI modules may legitimately know their
# feature's semantics and route hits to that feature's controller —
# but they may not own unrelated editor / REPL / app-router state.
#
# Generic `ui_*` checks (`check-ui-no-repl-state-read`,
# `check-ui-renderer-signatures`, `check-renderer-purity`,
# `check-no-facade-include-in-views`, `check-ui-returns-hits-only`)
# don't apply to feature-UI prefixes — they would either over-restrict
# (forbidding the `replay_state.h` snapshot read) or skip the file
# entirely (because the glob doesn't match). This guard fills the gap.
#
# Forbidden in `replay_ui_*.c`:
#   - editor_state_*_mut(            owns no editor state
#   - editor_buffer_(replace_line|insert_line|delete_range|set_input|apply_compiled_change)(
#   - editor_undo_(push_snapshot|pop_snapshot|do_redo)(
#   - repl_command_store_(insert|delete|replace|load|move)[a-zA-Z_]*(
#                                    no direct command-store mutation
#   - repl_compile_*(                 no parser/compile/apply
#   - repl_apply_*(
#   - repl_parser_*(
#   - editor_commit_*(                no commit orchestration
#   - repl_state_*_mut(               no REPL state mutation
#                                    (replay state mutation flows through
#                                    replay_handle_* / replay_state_mut)
#
# Allowed: replay snapshots / replay_handle_* / replay_state_view /
# UI metrics / gl2d / GL drawing.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

shopt -s nullglob

violations=""
for f in replay_ui_*.c; do
    [ -e "$f" ] || continue
    hits=$(grep -nE \
        'editor_state_[a-zA-Z_]+_mut\(|editor_buffer_(replace_line|insert_line|delete_range|set_input|apply_compiled_change)\(|editor_undo_(push_snapshot|pop_snapshot|do_redo)\(|repl_command_store_(insert|delete|replace|load|move)[a-zA-Z_]*\(|repl_compile_[a-zA-Z_]+\(|repl_apply_[a-zA-Z_]+\(|repl_parser_[a-zA-Z_]+\(|editor_commit_[a-zA-Z_]+\(|repl_state_[a-zA-Z_]+_mut\(' \
        "$f" 2>/dev/null \
        | grep -vE '^[0-9]+:[[:space:]]*(/\*|\*|//)' \
        || true)
    if [ -n "$hits" ]; then
        violations+="$hits"$'\n'
    fi
done

if [ -n "$violations" ]; then
    echo "ERROR: replay_ui_*.c references a forbidden mutator." >&2
    echo "Feature-owned UI under the replay_ui_* prefix may render the" >&2
    echo "replay HUD/buttons, hit-test replay-specific controls, route" >&2
    echo "hits via replay_handle_*, and read replay snapshots — but it" >&2
    echo "must not mutate editor / REPL state directly or call" >&2
    echo "parser / compile / apply. See docs/MODULES.md '2D UI rendering" >&2
    echo "and hit-test'." >&2
    echo "Hits:" >&2
    printf '%s' "$violations" >&2
    exit 1
fi

# Renderer signature shape: each replay_ui_*_render must take a
# const Ui*State / Ui*View / Ui*Snapshot pointer (matching the generic
# ui-renderer contract). This keeps feature-UI renderers snapshot-only
# even though they live under the feature prefix.
sig_bad=0
for hdr in replay_ui_*.h; do
    [ -e "$hdr" ] || continue
    while IFS= read -r line; do
        fn=$(printf '%s\n' "$line" \
             | grep -oE 'replay_ui_[a-zA-Z0-9_]+_render' \
             | head -n1 || true)
        [ -z "$fn" ] && continue
        sig_line=$(grep -nE "^\s*void\s+${fn}\s*\(" replay_ui_*.h replay_ui_*.c 2>/dev/null | head -n1 || true)
        [ -z "$sig_line" ] && continue
        sig=${sig_line#*:*:}
        if ! printf '%s\n' "$sig" \
             | grep -Eq '^\s*void\s+'"$fn"'\s*\(\s*const\s+Ui[A-Za-z0-9_]+(View|State|Snapshot)\s*\*'; then
            echo "ERROR: replay-ui renderer signature shape invalid for $fn" >&2
            echo "  found: $sig" >&2
            sig_bad=1
        fi
    done < <(grep -E '^\s*void\s+replay_ui_[a-zA-Z0-9_]+_render\s*\(' "$hdr" || true)
done

if [ "$sig_bad" -ne 0 ]; then
    exit 1
fi

echo "replay-ui-isolation OK"
