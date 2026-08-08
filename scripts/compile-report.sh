#!/bin/bash

# Keep normal Make output useful when many C files compile and link in
# parallel: successful steps produce one timed line, while failures retain
# the command's diagnostics. `V=1` / `VERBOSE=1` passes the command's own
# output through as well as retaining the timing line.

set -u

# Same convention as run-tests.sh: color only to an interactive terminal,
# and honor NO_COLOR. Parallel `make -j` jobs inherit make's own stdout, so
# `-t 1` reflects whether the top-level invocation is a terminal.
cyan=
red=
yellow=
reset=
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    cyan=$(printf '\033[36m')
    red=$(printf '\033[31m')
    yellow=$(printf '\033[33m')
    reset=$(printf '\033[0m')
fi

usage() {
    printf 'usage: compile-report.sh compile REPORT_DIR SOURCE OBJECT VERBOSE -- COMMAND [ARGS...]\n' >&2
    printf '       compile-report.sh link REPORT_DIR TARGET VERBOSE -- COMMAND [ARGS...]\n' >&2
    printf '       compile-report.sh summary REPORT_DIR\n' >&2
    exit 2
}

now_seconds() {
    # macOS bash is old enough that EPOCHREALTIME is unavailable. BSD and GNU
    # date both accept this form; on BSD versions without %N, awk keeps the
    # integer prefix, which gives a coarse but still correct interval.
    date +%s.%N 2>/dev/null | awk '{ printf "%.3f\n", $0 + 0 }'
}

# Shared by compile_one/link_one: runs COMMAND, times it, records a
# "kind\telapsed\tlabel" row under $report_dir/times/, and prints one line
# (or the full command + its output, under VERBOSE).
#
# KEY (an object or binary path) only needs to be unique per report dir --
# it names the .time/.log files -- while LABEL is what gets printed and
# summarized, so a compile step can key on the object path but still
# display its (possibly build-mode-shared) source path.
run_step() {
    kind=$1
    report_dir=$2
    key=$3
    label=$4
    verbose=$5
    shift 5
    if [ "$1" != "--" ]; then
        usage
    fi
    shift
    if [ "$#" -eq 0 ]; then
        usage
    fi

    # Report-relative filenames are derived from $key, which is
    # repository-relative and therefore safe once slashes are replaced. The
    # report directory itself is unique to this Make process, so parallel
    # jobs never collide. Compile and link steps are prefixed so a
    # same-named object/binary pair can't collide either.
    filekey="$kind-${key//\//__}"
    time_file="$report_dir/times/$filekey.time"
    log_file="$report_dir/logs/$filekey.log"
    mkdir -p "$report_dir/times" "$report_dir/logs"

    start=$(now_seconds)
    if [ "$verbose" = 1 ]; then
        printf '+'
        printf ' %q' "$@"
        printf '\n'
        "$@"
        rc=$?
    else
        "$@" >"$log_file" 2>&1
        rc=$?
    fi
    end=$(now_seconds)
    elapsed=$(awk -v start="$start" -v end="$end" 'BEGIN {
        value = end - start
        if (value < 0) value = 0
        printf "%.3f", value
    }')

    printf '%s\t%s\t%s\n' "$kind" "$elapsed" "$label" >"$time_file"

    if [ "$rc" -eq 0 ]; then
        rm -f "$log_file"
        printf '%b%-7s%b %-64s %8ss\n' "$cyan" "$kind" "$reset" "$label" "$elapsed"
        return 0
    fi

    printf '%b%-7s%b %-64s %8ss %bFAILED%b (exit %d)\n' \
        "$cyan" "$kind" "$reset" "$label" "$elapsed" "$red" "$reset" "$rc" >&2
    if [ "$verbose" != 1 ] && [ -s "$log_file" ]; then
        cat "$log_file" >&2
    fi
    return "$rc"
}

compile_one() {
    if [ "$#" -lt 6 ]; then
        usage
    fi
    report_dir=$1
    source=$2
    object=$3
    verbose=$4
    shift 4
    run_step compile "$report_dir" "$object" "$source" "$verbose" "$@"
}

link_one() {
    if [ "$#" -lt 5 ]; then
        usage
    fi
    report_dir=$1
    target=$2
    verbose=$3
    shift 3
    run_step link "$report_dir" "$target" "$target" "$verbose" "$@"
}

summary() {
    report_dir=$1
    time_dir="$report_dir/times"
    mkdir -p "$report_dir"
    compile_count=0
    compile_sum=0
    link_count=0
    link_sum=0
    rows=''

    for time_file in "$time_dir"/*.time; do
        [ -f "$time_file" ] || continue
        row=$(cat "$time_file")
        kind=${row%%$'\t'*}
        rest=${row#*$'\t'}
        elapsed=${rest%%$'\t'*}
        rows+="$row\n"
        if [ "$kind" = link ]; then
            link_count=$((link_count + 1))
            link_sum=$(awk -v sum="$link_sum" -v elapsed="$elapsed" 'BEGIN { printf "%.3f", sum + elapsed }')
        else
            compile_count=$((compile_count + 1))
            compile_sum=$(awk -v sum="$compile_sum" -v elapsed="$elapsed" 'BEGIN { printf "%.3f", sum + elapsed }')
        fi
    done

    [ "$((compile_count + link_count))" -gt 0 ] || return 0

    printf '\n%bcompile summary:%b %d files compiled (%ss), %d binaries linked (%ss)\n' \
        "$cyan" "$reset" "$compile_count" "$compile_sum" "$link_count" "$link_sum"
    printf '%blongest build steps:%b\n' "$yellow" "$reset"
    printf '%b' "$rows" |
        LC_ALL=C sort -t $'\t' -k2,2nr |
        head -3 |
        awk -F $'\t' '{
            mins = int($2 / 60)
            if (mins > 0)
                duration = sprintf("%dm%.1fs", mins, $2 - mins * 60)
            else
                duration = sprintf("%.3fs", $2)
            printf "  %-7s %-56s %8s\n", $1, $3, duration
        }'
}

if [ "$#" -lt 2 ]; then
    usage
fi

case "$1" in
    compile)
        shift
        compile_one "$@"
        ;;
    link)
        shift
        link_one "$@"
        ;;
    summary)
        if [ "$#" -ne 2 ]; then
            usage
        fi
        summary "$2"
        ;;
    *)
        usage
        ;;
esac
