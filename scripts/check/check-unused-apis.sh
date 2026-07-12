#!/bin/bash
# Check for potentially unused public APIs in module headers.
#
# Heuristic: for each function declared in a scanned header, count
# word-boundary references to the name across all tracked .c and .h
# files. Two references is the floor for a *used* function:
#   1 in the header (the declaration) + 1 in a .c (the definition).
# Anything with <= 2 refs is flagged as a candidate dead API.
#
# Known false positives / negatives:
#   - Functions only referenced via function-pointer in a callback
#     registration that appears in a non-.c file (rare here).
#   - Names that double as a struct field or local variable inflate
#     the count and hide a truly-unused API.
#   - X-macro / generated callsites won't be caught lexically.
#
# Informational only — never fails the build.
set -u
cd "$(git rev-parse --show-toplevel)"

echo "Scanning for potentially unused public APIs..."

# Headers to audit. Curated to module public surfaces. Internal /
# types-only / forwarder-only headers are excluded explicitly.
headers=$(find src/repl src/editor src/subsystems -maxdepth 2 -name '*.h' \
    -not -name 'core_internal.h' \
    -not -name 'state_views.h' \
    -not -name 'state_owners.h' \
    -not -name 'state_types.h' \
    -not -name 'limits.h' \
    -not -name 'util.h' \
    -not -name 'command.h' \
    -not -name 'export_state.h' \
    -not -name 'pipeline.h' \
    | sort)

# C keywords / common return-type tokens we never want to treat as
# function names if the declaration regex picks them up.
is_keyword() {
    case "$1" in
        if|for|while|switch|return|sizeof|typedef|struct|enum|union|\
        static|extern|inline|const|void|int|float|double|char|long|\
        short|unsigned|signed|size_t|bool|NULL|true|false|STATIC_ASSERT) return 0 ;;
    esac
    return 1
}

# Collapse multi-line declarations (e.g. `void foo(\n  args\n);`) onto a
# single logical line, and skip block-comment bodies, so the declaration
# regex catches declarations whose argument list wrapped. Kept in sync
# with check-duplicate-api-decls.sh.
flatten_header() {
    awk '
        function strip_comment(s,    out, i, ja, jb) {
            out = ""
            i = 1
            while (i <= length(s)) {
                if (in_bc) {
                    ja = index(substr(s, i), "*/")
                    if (ja == 0) { i = length(s) + 1 }
                    else { in_bc = 0; i = i + ja + 1 }
                } else {
                    ja = index(substr(s, i), "/*")
                    jb = index(substr(s, i), "//")
                    if (ja == 0 && jb == 0) {
                        out = out substr(s, i); i = length(s) + 1
                    } else if (jb > 0 && (ja == 0 || jb < ja)) {
                        out = out substr(s, i, jb - 1); i = length(s) + 1
                    } else {
                        out = out substr(s, i, ja - 1); in_bc = 1; i = i + ja + 1
                    }
                }
            }
            return out
        }
        {
            line = strip_comment($0)
            if (line ~ /^[[:space:]]*#/) { print line; next }
            gsub(/[[:space:]]+/, " ", line)
            sub(/^ /, "", line); sub(/ $/, "", line)
            if (line == "") next
            acc = (acc == "" ? line : acc " " line)
            if (acc ~ /[;{}]$/) { print acc; acc = "" }
        }
        END { if (acc != "") print acc }
    ' "$1"
}

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/check-unused-apis.XXXXXX") || exit 1
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
candidates_file="$tmp_dir/candidates.tsv"
source_files="$tmp_dir/source-files.txt"
unused_file="$tmp_dir/unused.txt"

# Build the audited declaration set once. The old implementation ran a
# repository-wide `git grep` for every function found here; on this tree that
# meant hundreds of full source scans and roughly 40 seconds of process and
# I/O overhead.
for hdr in $headers; do
    [ -f "$hdr" ] || continue

    # Extract every line that looks like a function declaration:
    #   `<return-type> ... <name>( ... );`
    # Skip preprocessor lines (`#...`); block comments are already
    # stripped by flatten_header, so prose mentioning `func()` no
    # longer trips the regex.
    funcs=$(flatten_header "$hdr" \
        | grep -E '^[[:space:]]*[A-Za-z_]' \
        | grep -v '^[[:space:]]*#' \
        | grep -oE '[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^;{]*\)[[:space:]]*;' \
        | sed -E 's/[[:space:]]*\(.*$//' \
        | sort -u)

    for func in $funcs; do
        [ -z "$func" ] && continue
        is_keyword "$func" && continue
        printf '%s\t%s\n' "$hdr" "$func" >> "$candidates_file"
    done
done

# Count all candidate identifiers in one pass over the tracked source set.
# `git grep -c` counted matching *lines*, not raw occurrences, so `seen[]`
# deliberately increments a candidate at most once per source line.
git ls-files -- '*.c' '*.h' > "$source_files"
LC_ALL=C awk -F '\t' -v candidates="$candidates_file" -v files="$source_files" '
    BEGIN {
        while ((getline row < candidates) > 0) {
            split(row, field, "\t")
            candidate_count++
            candidate_header[candidate_count] = field[1]
            candidate_name[candidate_count] = field[2]
            wanted[field[2]] = 1
        }
        close(candidates)

        while ((getline path < files) > 0) {
            while ((getline source_line < path) > 0) {
                source_line_id++
                rest = source_line
                while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
                    identifier = substr(rest, RSTART, RLENGTH)
                    if ((identifier in wanted) &&
                        seen_on_line[identifier] != source_line_id) {
                        reference_count[identifier]++
                        seen_on_line[identifier] = source_line_id
                    }
                    rest = substr(rest, RSTART + RLENGTH)
                }
            }
            close(path)
        }
        close(files)

        for (i = 1; i <= candidate_count; i++) {
            name = candidate_name[i]
            count = reference_count[name] + 0
            if (count <= 2)
                printf "%s: %s() — %d refs\n",
                       candidate_header[i], name, count
        }
    }
' > "$unused_file"

if [ -s "$unused_file" ]; then
    echo "⚠️  Potentially unused APIs found:"
    sort "$unused_file"
    echo ""
    echo "   Heuristic: <= 2 refs across .c/.h (1 decl + 1 def, no callers)."
    echo "   Inspect each before acting — callback registrations, X-macros,"
    echo "   and shadowed names can cause both false positives and negatives."
    echo "   Consider moving truly-unused decls to an _internal.h or making static."
else
    echo "✓ No unused public APIs detected"
fi
exit 0
