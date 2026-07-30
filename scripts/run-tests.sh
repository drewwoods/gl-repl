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

# Detect a usable `time` once. We report the timed command's CPU time
# (user + sys), not `real`: the binaries run in parallel, so each job's
# wall clock absorbs every other job's contention and the numbers all
# converge on the suite's total. Under a shell where `time` is neither a
# keyword nor on PATH (dash with no /usr/bin/time) it exits 127 — in that
# case timing is simply dropped; the test result never depends on it.
have_time=0
if ( time -p true ) >/dev/null 2>&1; then
    have_time=1
fi

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

parse_cpu_seconds() {
    # Total CPU time (user + sys) from `time` output, as seconds.
    # `time -p` prints bare `1.234`; a shell keyword without -p may print
    # `0m1.234s`, so accept both. The trailing awk args are locals — awk
    # has no other scope, and an undeclared `s` here would clobber the
    # sys accumulator below.
    awk '
    function to_sec(str,   parts, mins, secs) {
        if (str ~ /m/) {
            split(str, parts, "m")
            mins = parts[1]
            secs = parts[2]
            sub(/s/, "", secs)
            return mins * 60 + secs
        } else {
            sub(/s/, "", str)
            return str + 0
        }
    }
    /^user/ { u = to_sec($2); seen = 1 }
    /^sys/  { s = to_sec($2); seen = 1 }
    END {
        if (seen)
            printf "%.3f\n", u + s
    }
    ' "$1"
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

# We store specifications in arrays to track status of parallel jobs
specs=( "$@" )
job_names=()
job_cmds=()
job_pids=()
job_started=()
job_completed=()

for ((i=0; i<total_bins; i++)); do
    spec="${specs[i]}"
    name=${spec%%:::*}
    cmd=${spec#*:::}

    if [ "$name" = "$spec" ] || [ -z "$name" ] || [ -z "$cmd" ]; then
        printf 'invalid test spec: %s\n' "$spec" >&2
        exit 2
    fi

    job_names[i]=$name
    job_cmds[i]=$cmd
    job_pids[i]=0
    job_started[i]=0
    job_completed[i]=0
done

concurrency=$jobs
if [ "$concurrency" -le 0 ]; then
    concurrency=$total_bins
fi

completed_count=0
started_count=0
running_count=0

while [ "$completed_count" -lt "$total_bins" ]; do
    # 1. Start new jobs up to capacity
    while [ "$running_count" -lt "$concurrency" ] && [ "$started_count" -lt "$total_bins" ]; do
        # Find first unstarted job
        for ((i=0; i<total_bins; i++)); do
            if [ "${job_started[i]}" -eq 0 ]; then
                name="${job_names[i]}"
                cmd="${job_cmds[i]}"
                log="$log_dir/$name.log"
                status="$log_dir/$name.status"
                time_file="$log_dir/$name.time"

                (
                    if [ -n "$child_force_color" ]; then
                        FORCE_COLOR=$child_force_color
                        export FORCE_COLOR
                    fi
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

                job_pids[i]=$!
                job_started[i]=1
                started_count=$((started_count + 1))
                running_count=$((running_count + 1))
                break
            fi
        done
    done

    # 2. Check for completed jobs
    any_changed=0
    for ((i=0; i<total_bins; i++)); do
        if [ "${job_started[i]}" -eq 1 ] && [ "${job_completed[i]}" -eq 0 ]; then
            name="${job_names[i]}"
            status="$log_dir/$name.status"
            pid="${job_pids[i]}"

            if [ -f "$status" ] || { [ "$pid" -ne 0 ] && ! kill -0 "$pid" 2>/dev/null; }; then
                # Reap process
                wait "$pid" 2>/dev/null

                if [ -f "$status" ]; then
                    rc=$(cat "$status")
                    if [ -z "$rc" ]; then
                        rc=127
                    fi
                else
                    rc=127
                fi

                log="$log_dir/$name.log"
                time_file="$log_dir/$name.time"

                # Extract CPU time (see parse_cpu_seconds)
                cpu_str="unknown"
                if [ -f "$time_file" ]; then
                    cpu_secs=$(parse_cpu_seconds "$time_file")
                    if [ -n "$cpu_secs" ]; then
                        printf '%s %s\n' "$cpu_secs" "$name" >>"$timings_file"
                        cpu_str="${cpu_secs}s cpu"
                    fi
                fi

                counts=$(parse_counts "$log")
                test_passed=${counts%% *}
                test_total=${counts#* }

                if [ "$rc" -eq 0 ]; then
                    passed_bins=$((passed_bins + 1))
                    printf '[%02d/%d] %bPASS%b %s' "$((completed_count + 1))" "$total_bins" "$green" "$reset" "$name"
                else
                    failed_bins=$((failed_bins + 1))
                    printf '%b════════════════════════════════════════════════════════════%b\n' "$red" "$reset"
                    printf '[%02d/%d] %bFAIL%b %s (exit %d)' "$((completed_count + 1))" "$total_bins" "$red" "$reset" "$name" "$rc"
                    printf '%s\n' "$name" >>"$failed_tests_file"
                fi

                if [ "$test_total" -ge 0 ]; then
                    passed_tests=$((passed_tests + test_passed))
                    total_tests=$((total_tests + test_total))
                    printf ' [%d/%d tests] (%s)\n' "$test_passed" "$test_total" "$cpu_str"
                    printf '%s %s %d %d %d\n' "$name" "$rc" "$test_passed" "$test_total" "$((test_total - test_passed))" >>"$summary_file"
                else
                    unknown_stats=$((unknown_stats + 1))
                    printf ' [test count unknown] (%s)\n' "$cpu_str"
                    printf '%s %s unknown unknown unknown\n' "$name" "$rc" >>"$summary_file"
                fi

                if [ "$rc" -ne 0 ] && [ -s "$log" ]; then
                    printf '%b==> %s output:%b\n' "$cyan" "$name" "$reset"
                    cat "$log"
                    printf '%b════════════════════════════════════════════════════════════%b\n' "$red" "$reset"
                fi

                job_completed[i]=1
                completed_count=$((completed_count + 1))
                running_count=$((running_count - 1))
                any_changed=1
            fi
        fi
    done

    if [ "$any_changed" -eq 0 ]; then
        sleep 0.1
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

# Show the 3 most expensive tests. CPU time, not wall clock: the jobs run
# in parallel, and time spent blocked (sleeps, waiting on a child) does not
# count here.
if [ -f "$timings_file" ] && [ -s "$timings_file" ]; then
    printf '\n%b⏱️  most expensive tests (CPU time):%b\n' "$cyan" "$reset"
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
