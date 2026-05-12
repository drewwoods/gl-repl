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
headers=$(find src/repl src/editor src/widgets -maxdepth 2 -name '*.h' \
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
        short|unsigned|signed|size_t|bool|NULL|true|false) return 0 ;;
    esac
    return 1
}

unused=""
for hdr in $headers; do
    [ -f "$hdr" ] || continue

    # Extract every line that looks like a function declaration:
    #   `<return-type> ... <name>( ... );`
    # Skip preprocessor lines (`#...`) and block-comment continuation
    # lines (` * doc`) so prose mentioning `func()` doesn't get picked
    # up as a declaration. Require the line to begin with an identifier
    # so single-line `/* ... */` comments are filtered too.
    funcs=$(grep -E '^[[:space:]]*[A-Za-z_]' "$hdr" \
        | grep -v '^[[:space:]]*#' \
        | grep -oE '[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^;{]*\)[[:space:]]*;' \
        | sed -E 's/[[:space:]]*\(.*$//' \
        | sort -u)

    for func in $funcs; do
        [ -z "$func" ] && continue
        is_keyword "$func" && continue

        # Count word-boundary references in all tracked source files.
        count=$(git grep -c "\\b$func\\b" -- '*.c' '*.h' 2>/dev/null \
                | awk -F: '{s+=$NF} END {print s+0}')

        if [ "$count" -le 2 ]; then
            unused="$unused$hdr: $func() — $count refs\n"
        fi
    done
done

if [ -n "$unused" ]; then
    echo "⚠️  Potentially unused APIs found:"
    printf "%b" "$unused" | sort
    echo ""
    echo "   Heuristic: <= 2 refs across .c/.h (1 decl + 1 def, no callers)."
    echo "   Inspect each before acting — callback registrations, X-macros,"
    echo "   and shadowed names can cause both false positives and negatives."
    echo "   Consider moving truly-unused decls to an _internal.h or making static."
else
    echo "✓ No unused public APIs detected"
fi
exit 0
