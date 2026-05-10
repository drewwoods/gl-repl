#!/bin/bash
# Hard guard: REPL pipeline TUs do not call load_line_to_input.
#
# load_line_to_input() rewrites the editor input buffer from the
# active source line — that is editor-input behavior and lives in
# src/editor/input.c. Step 6 of the repl_demo decoupling plan moved
# the only two pipeline call sites out: repl_reformat_commands() in
# repl_core.c (split into pure repl_reformat_program() + editor
# wrapper editor_reformat_commands()) and load_scene_from_slot() in
# repl_scenes.c (the editor-input refresh now happens at the
# controller boundary after the scene-load API returns).
#
# This guard fails the build if a REPL pipeline TU starts calling
# load_line_to_input again. It is intentionally narrow: src/editor/*
# and controller TUs (glr_ctrl.c, glr_actions.c) may still call it.

set -euo pipefail

# Pipeline TUs in REPL_DEMO_DEP_SRCS. Mirrors the Makefile list so the
# guard tracks what the demo actually links from the REPL pipeline.
files=(
    repl_core.c
    repl_state.c
    repl_parser.c
    repl_command_spec.c
    repl_command_store.c
    repl_compile.c
    repl_apply.c
    repl_flatten.c
    repl_executor.c
    repl_eval.c
    repl_source_scope.c
    repl_autonormal.c
    repl_scenes.c
    repl_example_loader.c
    repl_examples.c
    repl_export.c
    repl_autocomplete.c
    repl_help_text.c
    repl_replay_annotations.c
)

# Match call shape only; skip lines that are inside C block comments
# (leading `*`) or whose first non-whitespace tokens form a `//` prefix.
violations=$(grep -nE '\bload_line_to_input\s*\(' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "no-load-line-to-input-in-pipeline OK"
    exit 0
fi

echo "ERROR: REPL pipeline TUs must not call load_line_to_input()." >&2
echo "load_line_to_input is editor-input behavior (src/editor/input.c)." >&2
echo "Pipeline code that needs to refresh the input buffer should return" >&2
echo "an effect/result and let the controller actualize the refresh." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
