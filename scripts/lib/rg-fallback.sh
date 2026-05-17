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
#   - falls back to GNU grep, emulating the only usage these scripts
#     need: `rg -n "REGEX" FILE...` -> `file:line:match`.
#
# GNU grep supports the `\s` / `\(` the patterns use; `-H` forces the
# `file:` prefix even for a single file (callers strip `file:line:`).
# Caller-side `2>/dev/null || true` still handles non-existent globs
# exactly as it did with ripgrep.
rg() {
    local _rg
    _rg="$(type -P rg 2>/dev/null || true)"
    if [ -n "$_rg" ]; then
        "$_rg" "$@"
    else
        # Callers always invoke: rg -n "PATTERN" FILE...
        grep -HE "$@"
    fi
}
