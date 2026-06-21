#!/bin/bash
# Hard guard: REPL pipeline TUs do not call editor_load_line_to_input.
#
# editor_load_line_to_input() rewrites the editor input buffer from the
# active source line — that is editor-input behavior and lives in
# src/editor/input.c. Step 6 of the repl_demo decoupling plan moved
# the only two pipeline call sites out: repl_reformat_commands() in
# src/repl/core.c (split into pure repl_reformat_program() + editor
# wrapper editor_reformat_commands()) and load_scene_from_slot() in
# src/repl/scenes.c (the editor-input refresh now happens at the
# controller boundary after the scene-load API returns).
#
# This guard fails the build if a REPL pipeline TU starts calling
# editor_load_line_to_input again. It is intentionally narrow: src/editor/*
# and controller TUs (glr_ctrl.c, glr_actions.c) may still call it.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Pipeline TUs in REPL_DEMO_DEP_SRCS. Mirrors the Makefile list so the
# guard tracks what the demo actually links from the REPL pipeline.
files=(
    src/repl/core.c
    src/repl/state.c
    src/repl/parser.c
    src/repl/command_spec.c
    src/repl/command_store.c
    src/repl/compile.c
    src/repl/apply.c
    src/repl/flatten.c
    src/repl/flatten_query.c
    src/repl/executor.c
    src/repl/eval.c
    src/repl/source_scope.c
    src/repl/autonormal.c
    src/repl/scenes.c
    src/repl/scene_snapshot.c
    src/repl/example_loader.c
    src/repl/examples.c
    src/repl/export.c
    src/repl/export_cmd_writer.c
    src/repl/export_display.c
    src/repl/export_prologue.c
    src/repl/export_setup.c
    repl_autocomplete.c
    src/repl/help_text.c
    src/subsystems/replay/replay_annotations.c
)

# Match call shape only; skip lines that are inside C block comments
# (leading `*`) or whose first non-whitespace tokens form a `//` prefix.
violations=$(grep -nE '\beditor_load_line_to_input\s*\(' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "no-load-line-to-input-in-pipeline OK"
    exit 0
fi

echo "ERROR: REPL pipeline TUs must not call editor_load_line_to_input()." >&2
echo "editor_load_line_to_input is editor-input behavior (src/editor/input.c)." >&2
echo "Pipeline code that needs to refresh the input buffer should return" >&2
echo "an effect/result and let the controller actualize the refresh." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
