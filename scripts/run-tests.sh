#!/bin/bash

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

# Detect a usable `time` once. The shell keyword (bash/ksh) yields
# sub-second `real 0mX.XXXs` that parse_time_output already handles.
# Under a shell where it is neither a keyword nor on PATH (dash with
# no /usr/bin/time) it exits 127 — in that case timing is simply
# dropped; the test result never depends on it.
have_time=0
if ( time -p true ) >/dev/null 2>&1; then
    have_time=1
fi

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
    time_file="$log_dir/$name.time"

    (
        if [ -n "$child_force_color" ]; then
            FORCE_COLOR=$child_force_color
            export FORCE_COLOR
        fi
        # rc must come from the test command, never from `time`.
        if [ "$have_time" -eq 1 ]; then
            { time -p sh -c "$cmd" >"$log" 2>&1; } 2>"$time_file"
            rc=$?
        else
            sh -c "$cmd" >"$log" 2>&1
            rc=$?
            : >"$time_file"
        fi
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

parse_time_output() {
    # Extract "real" time from time command output (format: real 0m1.234s)
    awk '/^real/ { print $2 }' "$1"
}

time_to_seconds() {
    # Convert time format (0m1.234s, 1m2.345s, or 1.234) to seconds as float
    local time_str=$1
    if [ -z "$time_str" ]; then
        printf '0.000\n'
        return
    fi
    case "$time_str" in
        *m*)
            local mins=$(printf '%s' "$time_str" | cut -d'm' -f1)
            local secs=$(printf '%s' "$time_str" | cut -d'm' -f2 | cut -d's' -f1)
            printf '%s\n' "$mins $secs" | awk '{printf "%.3f\n", $1 * 60 + $2}'
            ;;
        *)
            local secs=$(printf '%s' "$time_str" | cut -d's' -f1)
            printf '%s\n' "$secs" | awk '{printf "%.3f\n", $1}'
            ;;
    esac
}

passed_bins=0
failed_bins=0
passed_tests=0
total_tests=0
unknown_stats=0
summary_file="$log_dir/summary.txt"
failed_tests_file="$log_dir/failed_tests.txt"
timings_file="$log_dir/timings.txt"
: >"$summary_file"
: >"$failed_tests_file"
: >"$timings_file"

for spec do
    name=${spec%%:::*}
    log="$log_dir/$name.log"
    status="$log_dir/$name.status"
    time_file="$log_dir/$name.time"

    if [ -f "$status" ]; then
        rc=$(cat "$status")
    else
        rc=127
    fi

    # Extract elapsed time from time output
    if [ -f "$time_file" ]; then
        elapsed_str=$(parse_time_output "$time_file")
        if [ -z "$elapsed_str" ]; then
            elapsed_str="unknown"
        else
            # Record timing for top 3 analysis (in seconds for sorting)
            elapsed_secs=$(time_to_seconds "$elapsed_str")
            printf '%s %s\n' "$elapsed_secs" "$name" >>"$timings_file"
            elapsed_str="${elapsed_secs}s"
        fi
    else
        elapsed_str="unknown"
    fi

    counts=$(parse_counts "$log")
    test_passed=${counts%% *}
    test_total=${counts#* }

    if [ "$rc" -eq 0 ]; then
        passed_bins=$((passed_bins + 1))
        printf '%bPASS%b %s' "$green" "$reset" "$name"
    else
        failed_bins=$((failed_bins + 1))
        printf '%b════════════════════════════════════════════════════════════%b\n' "$red" "$reset"
        printf '%bFAIL%b %s (exit %d)' "$red" "$reset" "$name" "$rc"
        printf '%s\n' "$name" >>"$failed_tests_file"
    fi

    if [ "$test_total" -ge 0 ]; then
        passed_tests=$((passed_tests + test_passed))
        total_tests=$((total_tests + test_total))
        printf ' [%d/%d tests] (%s)\n' "$test_passed" "$test_total" "$elapsed_str"
        printf '%s %s %d %d %d\n' "$name" "$rc" "$test_passed" "$test_total" "$((test_total - test_passed))" >>"$summary_file"
    else
        unknown_stats=$((unknown_stats + 1))
        printf ' [test count unknown] (%s)\n' "$elapsed_str"
        printf '%s %s unknown unknown unknown\n' "$name" "$rc" >>"$summary_file"
    fi

    # Show test output only for failed tests
    if [ "$rc" -ne 0 ] && [ -s "$log" ]; then
        printf '%b==> %s output:%b\n' "$cyan" "$name" "$reset"
        cat "$log"
        printf '%b════════════════════════════════════════════════════════════%b\n' "$red" "$reset"
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

# Show top 3 longest tests
if [ -f "$timings_file" ] && [ -s "$timings_file" ]; then
    printf '\n%b⏱️  longest tests:%b\n' "$cyan" "$reset"
    sort -rn "$timings_file" | head -3 | awk '{
        secs = $1
        name = $2
        mins = int(secs / 60)
        remaining = secs - (mins * 60)
        if (mins > 0) {
            printf "  %s: %dm%.1fs\n", name, mins, remaining
        } else {
            printf "  %s: %.3fs\n", name, secs
        }
    }'
fi

if [ "$failed_bins" -gt 0 ]; then
    printf '\n%b❌ FAILED TEST BINARIES:%b\n' "$red" "$reset"
    while IFS= read -r failed_name; do
        failed_log="$log_dir/$failed_name.log"
        failed_status="$log_dir/$failed_name.status"
        if [ -f "$failed_status" ]; then
            exit_code=$(cat "$failed_status")
        else
            exit_code=127
        fi
        counts=$(parse_counts "$failed_log")
        test_passed=${counts%% *}
        test_total=${counts#* }

        printf '  %b%s%b (exit code: %d)\n' "$red" "$failed_name" "$reset" "$exit_code"
        if [ "$test_total" -ge 0 ] && [ "$test_total" -gt 0 ]; then
            printf '    %d/%d tests passed\n' "$test_passed" "$test_total"
        fi
        printf '    log: %s\n' "$failed_log"
    done <"$failed_tests_file"
    printf '\n%bfailure logs kept in %s%b\n' "$yellow" "$log_dir" "$reset"
    exit 1
fi

exit 0
