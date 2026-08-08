# shellcheck shell=bash
# Portable `rg` for the check scripts.
#
# The check guards were written against ripgrep. ripgrep is not always
# installed (e.g. a stock Ubuntu box / CI), and the call sites swallow
# its absence with `2>/dev/null || true`, which silently turns a
# missing tool into a vacuous pass or a false failure. Source this to
# get an `rg` that:
#
#   - uses the real ripgrep binary when present (`type -P rg` resolves
#     the on-PATH executable, never this function — so macOS, which
#     ships ripgrep, behaves exactly as before); else
#   - falls back to the_silver_searcher (`ag`) when present, which is
#     faster than plain grep on the same tree-wide sweeps; else
#   - falls back to GNU grep, emulating the only usage these scripts
#     need: `rg -n "REGEX" FILE...` -> `file:line:match`.
#
# `-n`/`-o`/`-v`/`-w` and the ERE syntax these patterns use (alternation,
# `\s`, `\(`, `\b`) mean the same thing across rg and GNU grep -E, so
# callers don't need to branch on which tool actually ran. ag needs flags
# to get there - it is neither line-oriented nor `file:`-prefixed by
# default; see the ag tier below. `-H` forces the `file:` prefix even for
# a single file (callers strip `file:line:`).
# Caller-side `2>/dev/null || true` still handles non-existent globs
# exactly as it did with ripgrep.
#
# `-g '*.ext'` (ripgrep's file-type glob) is the one flag that means
# something different in every backend, so it needs translating rather than
# forwarding: ag has no glob filter, only a filename *regex* (`-G`), and
# grep has neither, only `--include`. Every caller in this tree only ever
# passes the plain `-g '*.ext'` shape (never a directory glob or `!`
# exclusion), so that's the only shape handled here.
rg() {
    local _rg
    _rg="$(type -P rg 2>/dev/null || true)"
    if [ -n "$_rg" ]; then
        "$_rg" "$@"
        return
    fi

    local -a rest=() exts=()
    while [ "$#" -gt 0 ]; do
        case "$1" in
            -g)
                shift
                case "${1:-}" in
                    '*.'*) exts+=("${1#\*.}") ;;
                    *) rest+=(-g "$1") ;;   # unrecognized glob shape: pass through as-is
                esac
                ;;
            *) rest+=("$1") ;;
        esac
        shift
    done

    local _ag
    _ag="$(type -P ag 2>/dev/null || true)"
    if [ -n "$_ag" ]; then
        # Three ag defaults diverge from rg and all three break callers that
        # do `... | head -n1` and strip a `file:line:` prefix:
        #
        #   --nogroup/--noheading/--nobreak: ag otherwise prints a per-file
        #     heading plus a blank separator line and omits the `file:`
        #     prefix on the match lines. `--nogroup` alone does NOT cover
        #     the single-file case - ag still groups under a heading there,
        #     so the caller's prefix strip eats the line content.
        #   --nomultiline: ag matches its PCRE against the whole buffer, so
        #     the `\s` every caller's pattern uses also matches newlines and
        #     `^\s*void\s+f\s*\(` reports the *blank line above* the
        #     declaration as the first hit. rg and grep are line-oriented.
        local -a ag_flags=(--nogroup --noheading --nobreak --nomultiline)
        if [ "${#exts[@]}" -gt 0 ]; then
            local joined
            joined="$(IFS='|'; echo "${exts[*]}")"
            "$_ag" "${ag_flags[@]}" -G "\\.(${joined})\$" "${rest[@]}"
        else
            "$_ag" "${ag_flags[@]}" "${rest[@]}"
        fi
        return
    fi

    # Callers always invoke: rg -n "PATTERN" FILE...; `-r` matters here even
    # though rg/ag recurse without being asked, because several callers pass
    # a directory (not a file list) - plain `grep -HE` on a directory just
    # errors "Is a directory" per-path (swallowed by the caller-side
    # `2>/dev/null || true`), silently producing zero hits.
    if [ "${#exts[@]}" -gt 0 ]; then
        local -a includes=()
        local e
        for e in "${exts[@]}"; do includes+=(--include="*.${e}"); done
        grep -rHE "${includes[@]}" "${rest[@]}"
    else
        grep -rHE "${rest[@]}"
    fi
}
