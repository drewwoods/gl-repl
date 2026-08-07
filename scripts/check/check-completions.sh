#!/bin/bash
# Hard guard: the shipped shell completions offer exactly the options the
# binary documents and accepts.
#
# Three lists have to agree, and all three live in src/app/boot/glr_cli.c or
# scripts/completions/:
#
#   help    - the option lines of print_usage() (what `gl-repl --help` prints)
#   parser  - the strcmp(argv[i], "--…") arms of glr_cli_parse()
#   zsh     - the _arguments spec in scripts/completions/_gl-repl
#   bash    - the $_gl_repl_opts word list in scripts/completions/gl-repl.bash
#
# Read from source, not from a built binary: the guard has to run in a tree
# that has never been built (and headless, and before GL libs exist).
#
# Adding an option therefore means four edits. That is the point - a new flag
# that never reaches the completions is invisible to every user who tab-
# completes instead of reading --help.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# comm compares bytes; a locale that collates "-h" and "--help" as if the
# dashes weren't there would desync it from sort's output.
export LC_ALL=C

CLI_SRC=src/app/boot/glr_cli.c
ZSH_SRC=scripts/completions/_gl-repl
BASH_SRC=scripts/completions/gl-repl.bash

for f in "$CLI_SRC" "$ZSH_SRC" "$BASH_SRC"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: missing $f" >&2
        exit 1
    fi
done

# --- help ------------------------------------------------------------------
# Option lines of the usage text start a string literal with two spaces and a
# dash ("  --window <WxH>  …"); continuation lines are indented far deeper, and
# the Environment section that follows names GLR_* vars, never options. Take
# every long option on such a line, which also picks up the "-h, --help" pair.
help_opts=$(sed -n '/^static void print_usage/,/^}/p' "$CLI_SRC" \
    | grep -E '^ *"  -' \
    | grep -oE -- '--[a-z][a-z0-9-]*' \
    | sort -u)

# -h is the only short option; it is spelled inline with --help above.
if sed -n '/^static void print_usage/,/^}/p' "$CLI_SRC" | grep -q -- '"  -h, --help'; then
    help_opts=$(printf '%s\n-h\n' "$help_opts" | sort -u)
fi

# --- parser ----------------------------------------------------------------
parser_opts=$(grep -oE 'strcmp\(argv\[i\], "-[^"]*"\)' "$CLI_SRC" \
    | sed -E 's/.*"(-[^"]*)".*/\1/' \
    | sort -u)

# --- zsh -------------------------------------------------------------------
# Spec lines are quoted and carry an optional leading exclusion group, whose
# contents repeat options declared on their own lines:
#   '(-h --help)'{-h,--help}'[…]'
#   '(--no-accum)--accum[…]'
#   '--example[…]:example:->examples'
# Drop everything past the first '[' (prose), then the group, and read what is
# left: a bare option or a {-h,--help} alternation.
# The `_arguments -s -S` line and the trailing positional spec ('*:file:…') are
# not option specs; requiring a quoted '-' or '(' first drops both.
zsh_opts=$(sed -n '/_arguments/,/^$/p' "$ZSH_SRC" \
    | grep -E "^[[:space:]]*'[-(]" \
    | sed -E "s/^[[:space:]]*'//; s/\[.*//; s/^\([^)]*\)//" \
    | tr -d "'{}" | tr ', ' '\n\n' \
    | grep -E '^-' \
    | sort -u)

# --- bash ------------------------------------------------------------------
bash_opts=$(sed -n "/^_gl_repl_opts='/,/'\$/p" "$BASH_SRC" \
    | sed -E "s/^_gl_repl_opts='//; s/'\$//" \
    | tr ' ' '\n' \
    | grep -E '^-' \
    | sort -u)

fail=0
report() {
    local label=$1 lhs=$2 rhs=$3
    local only_lhs only_rhs
    only_lhs=$(comm -23 <(printf '%s\n' "$lhs") <(printf '%s\n' "$rhs"))
    only_rhs=$(comm -13 <(printf '%s\n' "$lhs") <(printf '%s\n' "$rhs"))
    [ -z "$only_lhs" ] && [ -z "$only_rhs" ] && return 0
    fail=1
    echo "ERROR: $label" >&2
    [ -n "$only_lhs" ] && printf '  only in %s: %s\n' \
        "${label%% vs *}" "$(printf '%s' "$only_lhs" | tr '\n' ' ')" >&2
    [ -n "$only_rhs" ] && printf '  only in %s: %s\n' \
        "${label##* vs }" "$(printf '%s' "$only_rhs" | tr '\n' ' ')" >&2
    return 0   # a reported difference is not a failed command (set -e)
}

report "help vs parser" "$help_opts" "$parser_opts"
report "help vs zsh"    "$help_opts" "$zsh_opts"
report "help vs bash"   "$help_opts" "$bash_opts"

if [ "$fail" -ne 0 ]; then
    cat >&2 <<EOF

An option must appear in all four places:
  $CLI_SRC   print_usage() text + the strcmp() arm
  $ZSH_SRC       _arguments spec
  $BASH_SRC   \$_gl_repl_opts (plus a case arm if it takes an argument)
EOF
    exit 1
fi

echo "completions OK ($(printf '%s\n' "$help_opts" | grep -c .) options; help = parser = zsh = bash)"
