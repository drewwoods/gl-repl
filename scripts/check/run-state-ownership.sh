#!/bin/bash
set -e -o pipefail

# ANSI color codes
RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[0;33m'
NC=$'\033[0m'

# File lists passed from Makefile
REPL_SRCS="${REPL_SRCS}"
RENDER3D_SRCS="${RENDER3D_SRCS}"
UI_SRCS="${UI_SRCS}"
STATE_NEUTRAL_SRCS="${STATE_NEUTRAL_SRCS}"
REPL_LIVE_DEMO_DEP_SRCS="${REPL_LIVE_DEMO_DEP_SRCS}"
MEMPROF_DEMO_DEP_SRCS="${MEMPROF_DEMO_DEP_SRCS}"
CPUPROF_DEMO_DEP_SRCS="${CPUPROF_DEMO_DEP_SRCS}"
VARIABLE_PANEL_DEMO_DEP_SRCS="${VARIABLE_PANEL_DEMO_DEP_SRCS}"
COLOR_PICKER_DEMO_DEP_SRCS="${COLOR_PICKER_DEMO_DEP_SRCS}"
ASSIGN_PLOT_DEMO_DEP_SRCS="${ASSIGN_PLOT_DEMO_DEP_SRCS}"

# 1. In-place checks implemented as bash functions
check_controller_boundaries() {
    bad=$(grep -lE '#[[:space:]]*include[[:space:]]+"render3d/' $REPL_SRCS src/app/glr_ctrl.c src/app/glr_ctrl_router.c \
        | grep -v '^src/app/glr_ctrl\.c$' || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d headers included outside src/app/glr_ctrl.c:${NC}"
        echo "$bad"; exit 1
    fi
    bad=$(grep -lE '#[[:space:]]*include[[:space:]]+"ui/' $REPL_SRCS src/app/glr_ctrl.c src/app/glr_ctrl_router.c \
        | grep -vE '^src/app/(glr_ctrl|glr_ctrl_router|glr_actions)\.c$|^repl_editor\.c$' || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: new ui headers included outside approved exceptions:${NC}"
        echo "$bad"; exit 1
    fi
    echo "Controller boundaries ${GREEN}OK${NC}"
}

check_render3d_no_repl_state_mut() {
    bad=$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $RENDER3D_SRCS || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d files mutate REPL state:${NC}"
        echo "$bad"; exit 1
    fi
    echo "Render3d mutation boundary ${GREEN}OK${NC}"
}

check_pure_render3d_no_repl_state() {
    bad=$(grep -nE 'repl_(state|replay)_' $RENDER3D_SRCS || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d files reach into REPL state/replay APIs:${NC}"
        echo "$bad"; exit 1
    fi
    echo "Pure-render3d boundary ${GREEN}OK${NC}"
}

check_state_boundaries() {
    bad=$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/state\.h"' $RENDER3D_SRCS $STATE_NEUTRAL_SRCS 2>/dev/null || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d or state-neutral files include src/repl/state.h:${NC}"
        echo "$bad"; exit 1
    fi
    bad=$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/core_internal\.h"' \
        src/app/glr_ctrl.c $RENDER3D_SRCS $UI_SRCS $STATE_NEUTRAL_SRCS 2>/dev/null \
        | grep -vE '^ui_(color_picker|panels)\.c$' || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: unapproved view/utility files include src/repl/core_internal.h:${NC}"
        echo "$bad"; exit 1
    fi
    bad=$(grep -lE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $UI_SRCS 2>/dev/null \
        | grep -vE '^(ui_(color_picker|help_overlay|panels))\.c$' || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: unapproved UI files mutate REPL state directly:${NC}"
        echo "$bad"; exit 1
    fi
    bad=$(grep -nE 'repl_(state|replay)_' $RENDER3D_SRCS 2>/dev/null || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d files reach into REPL state/replay APIs:${NC}"
        echo "$bad"; exit 1
    fi
    echo "State facade boundaries ${GREEN}OK${NC}"
}

check_views_no_owners() {
    bad=$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/state_owners\.h"' $RENDER3D_SRCS $UI_SRCS 2>/dev/null || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: render3d/UI view files include src/repl/state_owners.h:${NC}"
        echo "$bad"; exit 1
    fi
    echo "View-file ownership boundary ${GREEN}OK${NC}"
}

check_ui_no_repl_state_mut() {
    bad=$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $UI_SRCS || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: UI files mutate REPL state:${NC}"
        echo "$bad"; exit 1
    fi
    echo "UI mutation boundary ${GREEN}OK${NC}"
}

# 2. Delegated script checks implemented as functions
check_no_write_through_view() {
    bash scripts/check/check-no-write-through-view.sh scripts/allowlists/write-through-view.txt $UI_SRCS $RENDER3D_SRCS
}

check_runtime_state_value_fields() {
    bash scripts/check/check-runtime-state-value-fields.sh src/repl/state.h
}

check_public_state_no_writable_pointers() {
    bash scripts/check/check-views-flat.sh scripts/baselines/views-flat-violations.txt
}

check_state_read_getters_return_values() {
    bash scripts/check/check-views-by-value-snapshot.sh scripts/baselines/by-value-snapshot-pointer-returns.txt
}

check_ui_renderer_takes_view() {
    bash scripts/check/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt
}

check_renderer_no_direct_mutators() {
    bash scripts/check/check-renderer-purity.sh scripts/allowlists/renderer-purity.txt
}

check_output_actualization() {
    bash scripts/check/check-output-actualization.sh
}

check_state_c_shrinking() {
    bash scripts/check/check-state-c-shrinking.sh scripts/baselines/state-c-lines.txt src/repl/state.c
}

check_no_facade_include_in_views() {
    bash scripts/check/check-no-facade-include-in-views.sh scripts/allowlists/facade-includes-in-views.txt
}

check_domain_owner_encapsulation() {
    bash scripts/check/check-domain-encapsulation.sh scripts/allowlists/domain-owner-encapsulation.txt
}

check_ui_no_repl_state_read() {
    bash scripts/check/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt
}

check_editor_ownership_budget() {
    bash scripts/check/check-editor-ownership-budget.sh scripts/baselines/editor-ownership-budget.txt
}

check_no_store_text_api() {
    bash scripts/check/check-no-store-text-api.sh
}

check_repl_no_direct_buffer_read() {
    bash scripts/check/check-repl-no-direct-buffer-read.sh scripts/allowlists/repl-no-direct-buffer-read.txt
}

check_glr_ctrl_not_editor-mirror() {
    bash scripts/check/check-glr-ctrl-not-editor-mirror.sh
}

check_ui_returns_hits_only() {
    bash scripts/check/check-ui-returns-hits-only.sh scripts/baselines/ui-returns-hits-only.txt
}

check_ui_panels_no_mutators() {
    bash scripts/check/check-ui-panels-no-mutators.sh
}

check_replay_ui_isolation() {
    bash scripts/check/check-replay-ui-isolation.sh
}

check_color_picker_ui_isolation() {
    bash scripts/check/check-color-picker-ui-isolation.sh
}

check_variable_panel_forwarders() {
    bash scripts/check/check-variable-panel-forwarders.sh scripts/baselines/variable-panel-forwarders.txt
}

check_replay_forwarders() {
    bash scripts/check/check-replay-forwarders.sh scripts/baselines/replay-forwarders.txt
}

check_no_repl_commit() {
    bash scripts/check/check-no-repl-commit.sh
}

check_no_repl_editor_input_shim() {
    bash scripts/check/check-no-repl-editor-input-shim.sh
}

check_no_set_status_in_repl_parser() {
    bash scripts/check/check-no-set-status-in-repl-parser.sh scripts/baselines/repl-parser-set-status.txt
}

check_no_set_status_in_compile_apply() {
    bash scripts/check/check-no-set-status-in-compile-apply.sh
}

check_no_load_line_to_input_in_pipeline() {
    bash scripts/check/check-no-load-line-to-input-in-pipeline.sh
}

check_repl_state_no_glr_state() {
    bash scripts/check/check-repl-state-no-glr-state.sh
}

check_glr_state_no_repl_mutators() {
    bash scripts/check/check-glr-state-no-repl-mutators.sh
}

check_repl_scenes_cfg_clear_paired() {
    bash scripts/check/check-repl-scenes-cfg-clear-paired.sh
}

check_repl_export_no_ui_layout() {
    bash scripts/check/check-repl-export-no-ui-layout.sh
}

check_repl_export_via_bridge() {
    bash scripts/check/check-repl-export-via-bridge.sh
}

check_ui_no_export_resolver() {
    bash scripts/check/check-ui-no-export-resolver.sh
}

check_no_feed_line_in_pipeline() {
    bash scripts/check/check-no-feed-line-in-pipeline.sh
}

check_repl_no_direct_tutorial_runner() {
    bash scripts/check/check-repl-no-direct-tutorial-runner.sh
}

check_repl_demo_stubs_shrinking() {
    bash scripts/check/check-repl-demo-stubs-shrinking.sh
}

check_repl_no_direct_editor() {
    bash scripts/check/check-repl-no-direct-editor.sh
}

check_repl_demo_no_editor() {
    bash scripts/check/check-repl-demo-no-editor.sh
}

check_repl_live_demo_no_editor() {
    bash scripts/check/check-repl-demo-no-editor.sh repl_live_demo REPL_LIVE_DEMO_DEP_SRCS
}

check_memprof_demo_isolation() {
    bash scripts/check/check-subsystem-demo-isolation.sh "MEMPROF_DEMO_DEP_SRCS" tools/memprof_demo memprof_demo
}

check_cpuprof_demo_isolation() {
    bash scripts/check/check-subsystem-demo-isolation.sh "CPUPROF_DEMO_DEP_SRCS" tools/cpuprof_demo cpuprof_demo
}

check_cpuprof_standalone() {
    bash scripts/check/check-cpuprof-standalone.sh
}

check_audio_nothread() {
    bash scripts/check/check-audio-nothread.sh
}

check_variable_panel_demo_isolation() {
    bash scripts/check/check-subsystem-demo-isolation.sh "VARIABLE_PANEL_DEMO_DEP_SRCS" tools/variable_panel_demo variable_panel_demo
}

check_color_picker_demo_isolation() {
    bash scripts/check/check-subsystem-demo-isolation.sh "COLOR_PICKER_DEMO_DEP_SRCS" tools/color_picker_demo color_picker_demo
}

check_assign_plot_demo_isolation() {
    bash scripts/check/check-subsystem-demo-isolation.sh "ASSIGN_PLOT_DEMO_DEP_SRCS" tools/assign_plot_demo assign_plot_demo
}

check_editor_repl_surface() {
    bash scripts/check/check-editor-repl-surface.sh scripts/baselines/editor-repl-surface.txt
}

check_edit_ops_pure() {
    bash scripts/check/check-edit-ops-pure.sh
}

check_no_raw_undo_clear() {
    bash scripts/check/check-no-raw-undo-clear.sh
}

check_source_document_port_owners() {
    bash scripts/check/check-source-document-port-owners.sh
}

check_editor_no_app() {
    bash scripts/check/check-editor-no-app.sh
}

check_repl_no_app() {
    bash scripts/check/check-repl-no-app.sh
}

check_repl_no_mut_reads() {
    bash scripts/check/check-repl-no-mut-reads.sh
}

check_no_retired_repl_state_apis() {
    # Broad array mutators and symmetry-only facade entries were retired once
    # every real mutation had a narrower owner surface. Keep them gone: a new
    # caller should use command_store, a targeted setter, or the flat-program
    # owner's explicit writable slice.
    local pattern
    pattern='repl_state_(document_cmds_mut|document_cmd_at_mut|document_capacity|flat_program_mut|flat_program_cmds_mut|flat_program_local_vars_mut|flat_program_reset|scenes_mut|import_export_mut|import_export_reset)[[:space:]]*\('
    local bad
    bad=$(git grep -nE "$pattern" -- '*.c' '*.h' 2>/dev/null || true)
    if [ -n "$bad" ]; then
        echo "${RED}ERROR: retired broad REPL state API resurfaced:${NC}"
        echo "$bad"
        exit 1
    fi
    echo "Retired REPL state APIs ${GREEN}OK${NC}"
}

check_render3d_no_upper_layers() {
    bash scripts/check/check-render3d-no-upper-layers.sh
}

check_ui_core_no_upper_layers() {
    bash scripts/check/check-ui-core-no-upper-layers.sh
}

check_app_boot_band() {
    bash scripts/check/check-app-boot-band.sh
}

check_module_prefixes() {
    bash scripts/check/check-module-prefixes.sh
}

check_include_style() {
    bash scripts/check/check-include-style.sh
}

check_completions() {
    bash scripts/check/check-completions.sh
}

check_tier_c_function_size() {
    bash scripts/check/check-tier-c-function-size.sh scripts/baselines/tier-c-function-size.txt
}

check_no_test_default_output() {
    bash scripts/check/check-no-test-default-output.sh
}

check_prof_sections_instrumented() {
    bash scripts/check/check-prof-sections-instrumented.sh
}

check_keymap_no_dup() {
    bash scripts/keymap.sh check
}

check_palette() {
    python3 scripts/check/check-palette.py
}

# Emit a check's captured output with the same header + coloring the serial
# path used. Reads $target / $out / already-run status from the caller.
emit_check_output() {
    local target="$1"
    local out="$2"
    printf "  ${YELLOW}▶${NC} %s\n" "$target"
    if [ -n "$out" ]; then
        echo "$out" | sed 's/^/    /' | sed $'s/WARNING:/\033[0;33mWARNING:\033[0m/g; s/ OK / \033[0;32mOK\033[0m /g; s/ OK$/ \033[0;32mOK\033[0m/'
    fi
}

# In "run all" mode (PARALLEL_COLLECT=1) run_check only records the target+func
# into a queue; dispatch_queue then runs them concurrently. In single-target
# mode it runs the check synchronously, exactly as before.
Q_TARGET=()
Q_FUNC=()
run_check() {
    local target="$1"
    local func="$2"
    shift 2

    if [ -n "${PARALLEL_COLLECT:-}" ]; then
        Q_TARGET[${#Q_TARGET[@]}]="$target"
        Q_FUNC[${#Q_FUNC[@]}]="$func"
        return 0
    fi

    local out
    local rc=0
    out=$( "$func" "$@" 2>&1 ) || rc=$?
    emit_check_output "$target" "$out"
    if [ "$rc" -ne 0 ]; then
        exit "$rc"
    fi
}

# Run one queued check, writing its formatted output and exit code to $dir.
run_queue_worker() {
    local idx="$1"
    local dir="$2"
    local out rc=0
    out=$( "${Q_FUNC[$idx]}" 2>&1 ) || rc=$?
    emit_check_output "${Q_TARGET[$idx]}" "$out" > "$dir/$idx.out" 2>&1
    printf '%s' "$rc" > "$dir/$idx.rc"
}

# Run the collected queue concurrently (bounded), then replay each check's
# output in the original order so the log stays deterministic. Exits non-zero
# if any check failed (all checks run — failures are reported together rather
# than fail-fast on the first one).
dispatch_queue() {
    local n=${#Q_TARGET[@]}
    local jobs="${STATE_OWNERSHIP_JOBS:-}"
    if [ -z "$jobs" ]; then
        jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    fi
    [ "$jobs" -ge 1 ] 2>/dev/null || jobs=1

    local dir progress_pid= cleaned=0
    dir=$(mktemp -d "${TMPDIR:-/tmp}/state-ownership.XXXXXX")

    cleanup() {
        [ "$cleaned" -eq 1 ] && return 0
        cleaned=1
        if [ -t 2 ] && [ -n "${progress_pid:-}" ]; then
            printf '\r\033[K' >&2
        fi
        if [ -n "${progress_pid:-}" ]; then
            kill "$progress_pid" 2>/dev/null || true
        fi
        local pids
        pids=$(jobs -p 2>/dev/null || true)
        if [ -n "$pids" ]; then
            kill $pids 2>/dev/null || true
        fi
        if [ -n "${dir:-}" ] && [ -d "$dir" ]; then
            rm -rf "$dir"
        fi
    }
    trap 'cleanup; exit 130' INT
    trap 'cleanup; exit 143' TERM
    trap 'cleanup; exit 129' HUP
    trap 'cleanup' EXIT

    # FIFO semaphore: bash 3.2 has no `wait -n`, so bound concurrency with a
    # token bucket. Seed $jobs tokens; each worker acquires one before it runs
    # and returns it on finish, so the next queued check starts the instant any
    # running one completes (true streaming, not fixed batches).
    local fifo="$dir/tokens"
    mkfifo "$fifo"
    exec 9<>"$fifo"
    local k
    for (( k=0; k<jobs; k++ )); do printf '\n' >&9; done

    # Parallel mode buffers each check's output and only replays it once every
    # check has finished, so an interactive run would sit silent for a couple of
    # seconds and look like a stall. When stderr is a terminal, run a background
    # ticker that reports how many checks have completed (one .rc file each) on a
    # single self-erasing line. Skipped when stderr is not a tty so piped/CI logs
    # stay byte-identical to the serial run.
    if [ -t 2 ] && [ "$n" -gt 1 ]; then
        (
            while :; do
                [ -d "$dir" ] || break
                # Count completed checks by their .rc files. A glob loop (not
                # `ls`) stays safe under set -e/pipefail when none exist yet: the
                # unmatched pattern is skipped by the -e test rather than erroring.
                done=0
                for f in "$dir"/*.rc; do [ -e "$f" ] && done=$((done+1)); done
                printf '\r  %s▶%s running %d ownership guards… %d/%d' \
                    "$YELLOW" "$NC" "$n" "$done" "$n" >&2
                [ "$done" -ge "$n" ] && break
                sleep 0.1
            done
        ) &
        progress_pid=$!
    fi

    local i
    for (( i=0; i<n; i++ )); do
        read -r -u 9                                          # acquire a token
        { run_queue_worker "$i" "$dir"; printf '\n' >&9; } &  # release when done
    done
    wait
    exec 9>&-
    if [ -n "$progress_pid" ]; then
        wait "$progress_pid" 2>/dev/null || true
        printf '\r\033[K' >&2   # erase the progress line before the report
    fi

    local rc=0 crc
    for (( i=0; i<n; i++ )); do
        cat "$dir/$i.out"
        crc=$(cat "$dir/$i.rc" 2>/dev/null || echo 1)
        [ "$crc" -eq 0 ] 2>/dev/null || rc="$crc"
    done
    exit "$rc"
}

# Run a single target if passed, otherwise run all in order
if [ -n "$1" ]; then
    # Map target name dashes to underscores for function lookup
    func_name=$(echo "$1" | tr '-' '_')
    if declare -f "$func_name" >/dev/null; then
        run_check "$1" "$func_name"
    else
        echo "${RED}ERROR: check target '$1' (function '$func_name') not found in run-state-ownership.sh${NC}" >&2
        exit 1
    fi
else
    PARALLEL_COLLECT=1
    run_check check-controller-boundaries check_controller_boundaries
    run_check check-render3d-no-repl-state-mut check_render3d_no_repl_state_mut
    run_check check-pure-render3d-no-repl-state check_pure_render3d_no_repl_state
    run_check check-state-boundaries check_state_boundaries
    run_check check-views-no-owners check_views_no_owners
    run_check check-ui-no-repl-state-mut check_ui_no_repl_state_mut
    run_check check-no-write-through-view check_no_write_through_view
    run_check check-runtime-state-value-fields check_runtime_state_value_fields
    run_check check-public-state-no-writable-pointers check_public_state_no_writable_pointers
    run_check check-state-read-getters-return-values check_state_read_getters_return_values
    run_check check-ui-renderer-takes-view check_ui_renderer_takes_view
    run_check check-renderer-no-direct-mutators check_renderer_no_direct_mutators
    run_check check-output-actualization check_output_actualization
    run_check check-state-c-shrinking check_state_c_shrinking
    run_check check-no-facade-include-in-views check_no_facade_include_in_views
    run_check check-domain-owner-encapsulation check_domain_owner_encapsulation
    run_check check-ui-no-repl-state-read check_ui_no_repl_state_read
    run_check check-editor-ownership-budget check_editor_ownership_budget
    run_check check-no-store-text-api check_no_store_text_api
    run_check check-repl-no-direct-buffer-read check_repl_no_direct_buffer_read
    run_check check-glr-ctrl-not-editor-mirror check_glr_ctrl_not_editor-mirror
    run_check check-ui-returns-hits-only check_ui_returns_hits_only
    run_check check-ui-panels-no-mutators check_ui_panels_no_mutators
    run_check check-replay-ui-isolation check_replay_ui_isolation
    run_check check-color-picker-ui-isolation check_color_picker_ui_isolation
    run_check check-variable-panel-forwarders check_variable_panel_forwarders
    run_check check-replay-forwarders check_replay_forwarders
    run_check check-no-repl-commit check_no_repl_commit
    run_check check-no-repl-editor-input-shim check_no_repl_editor_input_shim
    run_check check-no-set-status-in-repl-parser check_no_set_status_in_repl_parser
    run_check check-no-set-status-in-compile-apply check_no_set_status_in_compile_apply
    run_check check-no-load-line-to-input-in-pipeline check_no_load_line_to_input_in_pipeline
    run_check check-repl-state-no-glr-state check_repl_state_no_glr_state
    run_check check-glr-state-no-repl-mutators check_glr_state_no_repl_mutators
    run_check check-repl-scenes-cfg-clear-paired check_repl_scenes_cfg_clear_paired
    run_check check-repl-export-no-ui-layout check_repl_export_no_ui_layout
    run_check check-repl-export-via-bridge check_repl_export_via_bridge
    run_check check-ui-no-export-resolver check_ui_no_export_resolver
    run_check check-no-feed-line-in-pipeline check_no_feed_line_in_pipeline
    run_check check-repl-no-direct-tutorial-runner check_repl_no_direct_tutorial_runner
    run_check check-repl-demo-stubs-shrinking check_repl_demo_stubs_shrinking
    run_check check-repl-no-direct-editor check_repl_no_direct_editor
    run_check check-repl-demo-no-editor check_repl_demo_no_editor
    run_check check-repl-live-demo-no-editor check_repl_live_demo_no_editor
    run_check check-memprof-demo-isolation check_memprof_demo_isolation
    run_check check-cpuprof-demo-isolation check_cpuprof_demo_isolation
    run_check check-cpuprof-standalone check_cpuprof_standalone
    run_check check-audio-nothread check_audio_nothread
    run_check check-variable-panel-demo-isolation check_variable_panel_demo_isolation
    run_check check-color-picker-demo-isolation check_color_picker_demo_isolation
    run_check check-assign-plot-demo-isolation check_assign_plot_demo_isolation
    run_check check-editor-repl-surface check_editor_repl_surface
    run_check check-edit-ops-pure check_edit_ops_pure
    run_check check-no-raw-undo-clear check_no_raw_undo_clear
    run_check check-source-document-port-owners check_source_document_port_owners
    run_check check-editor-no-app check_editor_no_app
    run_check check-repl-no-app check_repl_no_app
    run_check check-repl-no-mut-reads check_repl_no_mut_reads
    run_check check-no-retired-repl-state-apis check_no_retired_repl_state_apis
    run_check check-render3d-no-upper-layers check_render3d_no_upper_layers
    run_check check-ui-core-no-upper-layers check_ui_core_no_upper_layers
    run_check check-app-boot-band check_app_boot_band
    run_check check-module-prefixes check_module_prefixes
    run_check check-include-style check_include_style
    run_check check-completions check_completions
    run_check check-tier-c-function-size check_tier_c_function_size
    run_check check-no-test-default-output check_no_test_default_output
    run_check check-prof-sections-instrumented check_prof_sections_instrumented
    run_check check-keymap-no-dup check_keymap_no_dup
    run_check check-palette check_palette
    PARALLEL_COLLECT=
    dispatch_queue
fi
