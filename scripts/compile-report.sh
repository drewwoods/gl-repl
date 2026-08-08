#!/bin/bash

# Keep normal Make output useful when many C files compile in parallel:
# successful compiles produce one timed line, while failures retain the
# compiler diagnostics. `V=1` / `VERBOSE=1` passes the compiler output through
# as well as retaining the timing line.

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
    printf '       compile-report.sh summary REPORT_DIR\n' >&2
    exit 2
}

now_seconds() {
    # macOS bash is old enough that EPOCHREALTIME is unavailable. BSD and GNU
    # date both accept this form; on BSD versions without %N, awk keeps the
    # integer prefix, which gives a coarse but still correct interval.
    date +%s.%N 2>/dev/null | awk '{ printf "%.3f\n", $0 + 0 }'
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
    if [ "$1" != "--" ]; then
        usage
    fi
    shift
    if [ "$#" -eq 0 ]; then
        usage
    fi

    # Object paths are repository-relative and therefore make a safe,
    # deterministic filename once slashes are replaced. The report directory
    # itself is unique to this Make process, so parallel jobs never collide.
    key=${object//\//__}
    time_file="$report_dir/times/$key.time"
    log_file="$report_dir/logs/$key.log"
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

    printf '%s\t%s\n' "$elapsed" "$source" >"$time_file"

    if [ "$rc" -eq 0 ]; then
        rm -f "$log_file"
        printf '%bcompile%b %-64s %8ss\n' "$cyan" "$reset" "$source" "$elapsed"
        return 0
    fi

    printf '%bcompile%b %-64s %8ss %bFAILED%b (exit %d)\n' \
        "$cyan" "$reset" "$source" "$elapsed" "$red" "$reset" "$rc" >&2
    if [ "$verbose" != 1 ] && [ -s "$log_file" ]; then
        cat "$log_file" >&2
    fi
    return "$rc"
}

summary() {
    report_dir=$1
    time_dir="$report_dir/times"
    mkdir -p "$report_dir"
    count=0
    sum=0
    rows=''

    for time_file in "$time_dir"/*.time; do
        [ -f "$time_file" ] || continue
        count=$((count + 1))
        row=$(cat "$time_file")
        rows+="$row\n"
        elapsed=${row%%$'\t'*}
        sum=$(awk -v sum="$sum" -v elapsed="$elapsed" 'BEGIN { printf "%.3f", sum + elapsed }')
    done

    [ "$count" -gt 0 ] || return 0

    printf '\n%bcompile summary:%b %d files, %ss summed compile time\n' "$cyan" "$reset" "$count" "$sum"
    printf '%blongest compile steps:%b\n' "$yellow" "$reset"
    printf '%b' "$rows" |
        LC_ALL=C sort -t $'\t' -k1,1nr |
        head -3 |
        awk -F $'\t' '{
            mins = int($1 / 60)
            if (mins > 0)
                duration = sprintf("%dm%.1fs", mins, $1 - mins * 60)
            else
                duration = sprintf("%.3fs", $1)
            printf "  %-64s %8s\n", $2, duration
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
