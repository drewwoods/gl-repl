#!/bin/sh

green=
red=
cyan=
yellow=
reset=

if [ -t 1 ] && [ -z "$NO_COLOR" ]; then
    green=$(printf '\033[32m')
    red=$(printf '\033[31m')
    cyan=$(printf '\033[36m')
    yellow=$(printf '\033[33m')
    reset=$(printf '\033[0m')
fi

child_force_color=
if [ -z "$NO_COLOR" ]; then
    if [ -n "$FORCE_COLOR" ]; then
        child_force_color=$FORCE_COLOR
    elif [ -t 1 ]; then
        child_force_color=1
    fi
fi

if [ "$#" -eq 0 ]; then
    printf '%s\n' 'usage: run-tests.sh name:::command [...]' >&2
    exit 2
fi

jobs=${TEST_JOBS:-0}
case "$jobs" in
    ''|*[!0-9]*)
        printf 'invalid TEST_JOBS value: %s\n' "$jobs" >&2
        exit 2
        ;;
esac

log_dir=${TEST_LOG_DIR:-build/test-logs/run-$$}
mkdir -p "$log_dir" || exit 2

total_bins=$#
printf '%b==> running %d test binaries' "$cyan" "$total_bins"
if [ "$jobs" -gt 0 ]; then
    printf ' (%d at a time)' "$jobs"
else
    printf ' (parallel)'
fi
printf '%b\n' "$reset"

active=0
for spec do
    name=${spec%%:::*}
    cmd=${spec#*:::}

    if [ "$name" = "$spec" ] || [ -z "$name" ] || [ -z "$cmd" ]; then
        printf 'invalid test spec: %s\n' "$spec" >&2
        exit 2
    fi

    log="$log_dir/$name.log"
    status="$log_dir/$name.status"

    (
        if [ -n "$child_force_color" ]; then
            FORCE_COLOR=$child_force_color
            export FORCE_COLOR
        fi
        sh -c "$cmd" >"$log" 2>&1
        rc=$?
        printf '%d\n' "$rc" >"$status"
        exit 0
    ) &

    active=$((active + 1))
    if [ "$jobs" -gt 0 ] && [ "$active" -ge "$jobs" ]; then
        wait
        active=0
    fi
done

wait

parse_counts() {
    awk '
    {
        line = $0
        if (match(line, /[0-9][0-9]*[[:space:]]*\/[[:space:]]*[0-9][0-9]*[[:space:]]*(tests[[:space:]]*)?passed/)) {
            snippet = substr(line, RSTART, RLENGTH)
            split(snippet, nums, /[^0-9]+/)
            passed = nums[1]
            total = nums[2]
        }
    }
    END {
        if (total != "")
            printf "%d %d\n", passed, total
        else
            printf "-1 -1\n"
    }
    ' "$1"
}

passed_bins=0
failed_bins=0
passed_tests=0
total_tests=0
unknown_stats=0
summary_file="$log_dir/summary.txt"
: >"$summary_file"

for spec do
    name=${spec%%:::*}
    log="$log_dir/$name.log"
    status="$log_dir/$name.status"

    if [ -f "$status" ]; then
        rc=$(cat "$status")
    else
        rc=127
    fi

    counts=$(parse_counts "$log")
    test_passed=${counts%% *}
    test_total=${counts#* }

    printf '\n%b==> %s%b\n' "$cyan" "$name" "$reset"
    if [ -s "$log" ]; then
        cat "$log"
    fi

    if [ "$rc" -eq 0 ]; then
        passed_bins=$((passed_bins + 1))
        printf '%bPASS%b %s' "$green" "$reset" "$name"
    else
        failed_bins=$((failed_bins + 1))
        printf '%bFAIL%b %s (exit %d)' "$red" "$reset" "$name" "$rc"
    fi

    if [ "$test_total" -ge 0 ]; then
        passed_tests=$((passed_tests + test_passed))
        total_tests=$((total_tests + test_total))
        printf ' [%d/%d tests]\n' "$test_passed" "$test_total"
        printf '%s %s %d %d %d\n' "$name" "$rc" "$test_passed" "$test_total" "$((test_total - test_passed))" >>"$summary_file"
    else
        unknown_stats=$((unknown_stats + 1))
        printf ' [test count unknown]\n'
        printf '%s %s unknown unknown unknown\n' "$name" "$rc" >>"$summary_file"
    fi
done

failed_tests=$((total_tests - passed_tests))

printf '\n%b==> global summary%b\n' "$cyan" "$reset"
printf 'test binaries: %d total, %d passed, %d failed\n' "$total_bins" "$passed_bins" "$failed_bins"
printf 'tests:         %d total, %d passed, %d failed' "$total_tests" "$passed_tests" "$failed_tests"
if [ "$unknown_stats" -gt 0 ]; then
    printf ' (%d binaries had unknown test counts)' "$unknown_stats"
fi
printf '\n'

if [ "$failed_bins" -gt 0 ]; then
    printf '%bfailure logs kept in %s%b\n' "$yellow" "$log_dir" "$reset"
    exit 1
fi

exit 0
