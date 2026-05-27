#!/bin/bash
# Check for function declarations duplicated across multiple module
# public headers. Two headers declaring the same function name is a
# header-hygiene smell: only the owning module's header should declare
# its exported API; other modules should #include that header.
#
# Heuristic shares the declaration regex with check-unused-apis.sh.
# False positives are rare — opaque-type forward decls don't match
# the function-shaped regex, and function-like macros are seldom
# named identically across headers.
#
# Exits 1 when duplicates are found so the check can gate CI.
set -u
cd "$(git rev-parse --show-toplevel)"

echo "Scanning for duplicate API declarations..."

# Same header set as check-unused-apis.sh — keep the two scripts in
# sync so a function moved out of the curated set vanishes from both
# audits together.
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

is_keyword() {
    case "$1" in
        if|for|while|switch|return|sizeof|typedef|struct|enum|union|\
        static|extern|inline|const|void|int|float|double|char|long|\
        short|unsigned|signed|size_t|bool|NULL|true|false) return 0 ;;
    esac
    return 1
}

# Collapse multi-line declarations (e.g. `void foo(\n  args\n);`) onto a
# single logical line, and skip block-comment bodies, so the declaration
# regex catches declarations whose argument list wrapped.
flatten_header() {
    awk '
        # Block-comment state machine.
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

dups=$({
    for hdr in $headers; do
        [ -f "$hdr" ] || continue
        # Skip lines whose first token is a control-flow keyword —
        # those are bodies of `static inline` helpers (e.g.
        # `return foo();` inside an inline wrapper). Without this
        # filter, the call regex would pull `foo` out of any inline
        # that delegates to another module's helper and false-flag it
        # as a duplicate declaration.
        funcs=$(flatten_header "$hdr" \
            | grep -E '^[[:space:]]*[A-Za-z_]' \
            | grep -v '^[[:space:]]*#' \
            | grep -vE '^[[:space:]]*(return|if|while|for|do|switch|case|break|continue|goto)[[:space:](]' \
            | grep -oE '[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^;{]*\)[[:space:]]*;' \
            | sed -E 's/[[:space:]]*\(.*$//' \
            | sort -u)
        for func in $funcs; do
            [ -z "$func" ] && continue
            is_keyword "$func" && continue
            printf '%s\t%s\n' "$func" "$hdr"
        done
    done
} | awk -F'\t' '
    {
        count[$1]++
        if (headers[$1] == "") headers[$1] = $2
        else                   headers[$1] = headers[$1] ", " $2
    }
    END {
        for (n in count)
            if (count[n] > 1)
                print n " (" count[n] "): " headers[n]
    }
' | sort)

if [ -n "$dups" ]; then
    echo "❌ Duplicate API declarations found:"
    printf '%s\n' "$dups" | sed 's/^/  /'
    echo ""
    echo "   Each name appears in more than one module header. Keep the"
    echo "   canonical declaration in the owning module's header and"
    echo "   delete the rest; downstream code should #include that header."
    exit 1
fi
echo "✓ No duplicate API declarations detected"
exit 0
