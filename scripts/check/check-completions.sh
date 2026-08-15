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
usage_text=$(sed -n '/^static void print_usage/,/^}/p' "$CLI_SRC")

help_opts=$(printf '%s\n' "$usage_text" \
    | grep -E '^ *"  -' \
    | grep -oE -- '--[a-z][a-z0-9-]*' \
    | sort -u)

# -h is the only short option; it is spelled inline with --help above.
#
# Matched with a bash glob rather than `… | grep -q`: grep -q exits at the
# first match, the writer upstream takes SIGPIPE, and `set -o pipefail` then
# reports the whole pipeline as failed even though the pattern WAS found — so
# the branch silently never ran and -h dropped out of help_opts. It survived
# on macOS only because BSD sed's output fits the pipe buffer before grep
# leaves; GNU sed on CI is slower to finish and fails loudly
# ("sed: couldn't write N items to stdout: Broken pipe").
case $usage_text in
    *'"  -h, --help'*) help_opts=$(printf '%s\n-h\n' "$help_opts" | sort -u) ;;
esac

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

echo "completions OK ($(printf '%s\n' "$help_opts" | grep -c .) gl-repl options; help = parser = zsh = bash)"

# ===========================================================================
# docs-assets.sh — the same four-way agreement, different sources. This
# script's flags are a user-facing surface too, and its completions went
# stale silently before this block existed.
# ===========================================================================

DA_SRC=scripts/docs-assets.sh
DA_ZSH=scripts/completions/_docs-assets.sh
DA_BASH=scripts/completions/docs-assets.bash

for f in "$DA_SRC" "$DA_ZSH" "$DA_BASH"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: missing $f" >&2
        exit 1
    fi
done

# --- help ------------------------------------------------------------------
# Option lines in the usage heredoc start with two spaces and a dash. Read
# ONLY the option field — everything up to the first run of 2+ spaces — so a
# flag named inside another's description ("The reverse of --to-apng: …")
# isn't mistaken for a declaration of its own.
da_help_opts=$(sed -n '/^usage() {/,/^}/p' "$DA_SRC" \
    | grep -E '^  -' \
    | sed -E 's/^  //; s/   +.*//' \
    | grep -oE -- '-{1,2}[a-z][a-z0-9-]*' \
    | sort -u)

# --- parser ----------------------------------------------------------------
# The case arms of the option loop. Three arms are shell syntax rather than
# documented options and are excluded by name: `-j*` (the glued -j4 form of
# --jobs), `--` (end-of-options), and `-*` (the unknown-option catch-all).
da_parser_opts=$(sed -n '/^while \[\[ \$# -gt 0 \]\]; do/,/^done/p' "$DA_SRC" \
    | grep -E '^ +-[^)]*\)' \
    | sed -E 's/^ +//; s/\).*//' \
    | tr '|' '\n' \
    | grep -E '^-{1,2}[a-z][a-z0-9-]*$' \
    | sort -u)

# --- zsh -------------------------------------------------------------------
da_zsh_opts=$(sed -n '/_arguments/,/^$/p' "$DA_ZSH" \
    | grep -E "^[[:space:]]*'[-(]" \
    | sed -E "s/^[[:space:]]*'//; s/\[.*//; s/^\([^)]*\)//" \
    | tr -d "'{}" | tr ', ' '\n\n' \
    | grep -E '^-' \
    | sed -E 's/\+$//' \
    | sort -u)

# --- bash ------------------------------------------------------------------
da_bash_opts=$(grep -oE "compgen -W '[^']*'" "$DA_BASH" \
    | sed -E "s/compgen -W '//; s/'$//" \
    | tr ' ' '\n' \
    | grep -E '^-' \
    | sort -u)

report "docs-assets help vs parser" "$da_help_opts" "$da_parser_opts"
report "docs-assets help vs zsh"    "$da_help_opts" "$da_zsh_opts"
report "docs-assets help vs bash"   "$da_help_opts" "$da_bash_opts"

if [ "$fail" -ne 0 ]; then
    cat >&2 <<EOF

A docs-assets option must appear in all four places:
  $DA_SRC   usage() text + the case arm
  $DA_ZSH     _arguments spec
  $DA_BASH   the compgen -W word list
EOF
    exit 1
fi

echo "completions OK ($(printf '%s\n' "$da_help_opts" | grep -c .) docs-assets options; help = parser = zsh = bash)"
