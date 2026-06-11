#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-build/code-smells}"
SRC_DIR="${SRC_DIR:-src}"
TEST_DIR="${TEST_DIR:-tests}"
TOOLS_DIR="${TOOLS_DIR:-tools}"
BENCH_DIR="${BENCH_DIR:-bench}"

MIN_TOKENS="${MIN_TOKENS:-80}"

LIZARD_CCN="${LIZARD_CCN:-15}"
LIZARD_LEN="${LIZARD_LEN:-150}"

CLANGD_BIN="${CLANGD_BIN:-clangd}"

CLANG_TIDY_BIN="${CLANG_TIDY_BIN:-/opt/homebrew/opt/llvm/bin/clang-tidy}"
CLANG_TIDY_CHECKS="${CLANG_TIDY_CHECKS:-readability-*,bugprone-*,misc-*,-readability-magic-numbers}"

PMD_IMAGE="${PMD_IMAGE:-pmdcode/pmd:latest}"
JOBS="${JOBS:-4}"

RUN_ALL=0
RUN_CLANGD=0
RUN_CLANG_TIDY=0
RUN_LIZARD=0
RUN_CPPCHECK=0
RUN_CPD=0
RUN_CHURN=0
ENSURE_COMPILE_DB=1
CPD_STATUS=""

say() {
  printf '\n==> %s\n' "$*"
}

have() {
  command -v "$1" >/dev/null 2>&1
}

usage() {
  cat <<EOF
Usage:
  $0 [options]

Checks:
  --all              Run every check
  --clangd           Run clangd --check over C files
  --clang-tidy       Run clang-tidy over C files
  --lizard           Run lizard complexity/size scan
  --cppcheck         Run cppcheck style/unusedFunction scan
  --cpd              Run PMD CPD duplication scan through Docker
  --churn            Generate churn x file-size hotspot list

Compile database:
  --no-compile-db    Do not try to generate compile_commands.json with bear

Other:
  -h, --help         Show this help

Environment knobs:
  OUT_DIR            Output directory. Default: build/code-smells
  SRC_DIR            Source directory. Default: src
  JOBS               Parallel jobs for clangd/clang-tidy. Default: 4

  CLANGD_BIN         clangd executable. Default: clangd

  CLANG_TIDY_BIN     clang-tidy executable.
                     Default: /opt/homebrew/opt/llvm/bin/clang-tidy
  CLANG_TIDY_CHECKS  clang-tidy checks.
                     Default: readability-*,bugprone-*,misc-*,-readability-magic-numbers

  MIN_TOKENS         PMD CPD minimum duplicate token count. Default: 80
  PMD_IMAGE          Docker image for PMD. Default: pmdcode/pmd:latest

  LIZARD_CCN         Lizard cyclomatic complexity threshold. Default: 15
  LIZARD_LEN         Lizard function length threshold. Default: 150

Examples:
  $0 --all
  $0 --lizard
  $0 --cpd
  $0 --clangd --cppcheck
  $0 --clang-tidy

  MIN_TOKENS=120 $0 --cpd
  LIZARD_CCN=40 LIZARD_LEN=180 $0 --lizard
  CLANGD_BIN=/opt/homebrew/opt/llvm/bin/clangd $0 --clangd
  CLANG_TIDY_BIN=/opt/homebrew/opt/llvm/bin/clang-tidy $0 --clang-tidy
EOF
}

if [[ $# -eq 0 ]]; then
  usage
  exit 2
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      RUN_ALL=1
      ;;
    --clangd)
      RUN_CLANGD=1
      ;;
    --clang-tidy)
      RUN_CLANG_TIDY=1
      ;;
    --lizard)
      RUN_LIZARD=1
      ;;
    --cppcheck)
      RUN_CPPCHECK=1
      ;;
    --cpd)
      RUN_CPD=1
      ;;
    --churn)
      RUN_CHURN=1
      ;;
    --no-compile-db)
      ENSURE_COMPILE_DB=0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$RUN_ALL" -eq 1 ]]; then
  RUN_CLANGD=1
  RUN_CLANG_TIDY=1
  RUN_LIZARD=1
  RUN_CPPCHECK=1
  RUN_CPD=1
  RUN_CHURN=1
fi

if [[ "$RUN_CLANGD" -eq 0 &&
      "$RUN_CLANG_TIDY" -eq 0 &&
      "$RUN_LIZARD" -eq 0 &&
      "$RUN_CPPCHECK" -eq 0 &&
      "$RUN_CPD" -eq 0 &&
      "$RUN_CHURN" -eq 0 ]]; then
  printf 'no checks selected\n\n' >&2
  usage >&2
  exit 2
fi

mkdir -p "$OUT_DIR"

say "code smell scan"
printf 'repo: %s\n' "$ROOT"
printf 'src:  %s\n' "$SRC_DIR"
printf 'out:  %s\n' "$OUT_DIR"

ensure_compile_commands() {
  say "checking compile_commands.json"

  if [[ -f compile_commands.json ]]; then
    printf 'found compile_commands.json\n'
    return 0
  fi

  if [[ "$ENSURE_COMPILE_DB" -eq 0 ]]; then
    printf 'compile_commands.json missing; not generating because --no-compile-db was passed\n'
    return 1
  fi

  if have bear; then
    say "compile_commands.json missing; generating with bear + stub build"
    # NOTE: We use make -B to force a full rebuild under bear so that it records all
    # compiler flags, even if the build is already up-to-date. We build with GL stubs
    # (-DGL_STUBS) so the scan can run on machines without real GL development headers.
    bear -- make -B gl-repl USE_GL_STUBS=1 || true
    [[ -f compile_commands.json ]] || return 1
    return 0
  fi

  cat > "$OUT_DIR/compile_commands.missing.txt" <<EOF
compile_commands.json is missing and bear is not installed.

Install bear:

  brew install bear

Then run:

  bear -- make gl-repl USE_GL_STUBS=1

clangd and clang-tidy diagnostics require compile_commands.json to understand
include paths, defines, and force-includes.
EOF

  printf 'missing compile_commands.json; see %s\n' "$OUT_DIR/compile_commands.missing.txt"
  return 1
}

run_clangd() {
  say "clangd diagnostics"

  local clangd_log="$OUT_DIR/clangd-check.txt"
  local clangd_diag="$OUT_DIR/clangd-diagnostics.txt"

  : > "$clangd_log"
  : > "$clangd_diag"

  if ! have "$CLANGD_BIN"; then
    cat > "$clangd_log" <<EOF
clangd not found.

Try one of:

  brew install llvm
  export PATH="/opt/homebrew/opt/llvm/bin:\$PATH"

or run with:

  CLANGD_BIN=/opt/homebrew/opt/llvm/bin/clangd $0 --clangd
EOF
    printf 'skipping clangd; see %s\n' "$clangd_log"
    return 0
  fi

  if ! ensure_compile_commands; then
    printf 'skipping clangd: compile_commands.json missing\n' | tee -a "$clangd_log"
    return 0
  fi

  # Run clangd --check in parallel using xargs to speed up execution
  { find "$SRC_DIR" -name '*.c' -print0; printf 'gl_repl.c\0'; } |
    xargs -0 -P "$JOBS" -I {} sh -c '
      file="$1"
      tmpfile=$(mktemp)
      printf -- "--- %s ---\n" "$file" > "$tmpfile"
      "'"$CLANGD_BIN"'" --check="$file" >> "$tmpfile" 2>&1 || true
      cat "$tmpfile"
      rm -f "$tmpfile"
    ' _ {} >> "$clangd_log"

  grep -E "warning:|error:" "$clangd_log" > "$clangd_diag" || true

  printf 'wrote %s\n' "$clangd_log"
  printf 'summary %s\n' "$clangd_diag"
}

run_clang_tidy() {
  say "clang-tidy readability / bugprone / misc checks"

  local tidy_log="$OUT_DIR/clang-tidy.txt"
  local tidy_diag="$OUT_DIR/clang-tidy-diagnostics.txt"

  : > "$tidy_log"
  : > "$tidy_diag"

  if [[ ! -x "$CLANG_TIDY_BIN" ]] && ! have "$CLANG_TIDY_BIN"; then
    cat > "$tidy_log" <<EOF
clang-tidy not found.

Expected Homebrew LLVM path on Apple Silicon:

  /opt/homebrew/opt/llvm/bin/clang-tidy

On Intel Homebrew, try:

  /usr/local/opt/llvm/bin/clang-tidy

Install LLVM:

  brew install llvm

or run with:

  CLANG_TIDY_BIN=/opt/homebrew/opt/llvm/bin/clang-tidy $0 --clang-tidy
EOF
    printf 'skipping clang-tidy; see %s\n' "$tidy_log"
    return 0
  fi

  if ! ensure_compile_commands; then
    printf 'skipping clang-tidy: compile_commands.json missing\n' | tee -a "$tidy_log"
    return 0
  fi

  # Run clang-tidy in parallel using xargs to speed up execution
  { find "$SRC_DIR" -name '*.c' -print0; printf 'gl_repl.c\0'; } |
    xargs -0 -P "$JOBS" -I {} sh -c '
      file="$1"
      tmpfile=$(mktemp)
      printf -- "--- %s ---\n" "$file" > "$tmpfile"
      "'"$CLANG_TIDY_BIN"'" \
        -p . \
        --checks="'"$CLANG_TIDY_CHECKS"'" \
        "$file" \
        >> "$tmpfile" 2>&1 || true
      cat "$tmpfile"
      rm -f "$tmpfile"
    ' _ {} >> "$tidy_log"

  grep -E "warning:|error:" "$tidy_log" > "$tidy_diag" || true

  printf 'wrote %s\n' "$tidy_log"
  printf 'summary %s\n' "$tidy_diag"
}

run_lizard() {
  say "lizard complexity / size"

  local lizard_log="$OUT_DIR/lizard.txt"
  local lizard_summary="$OUT_DIR/lizard-summary.txt"

  if have lizard; then
    lizard "$SRC_DIR" \
      -C "$LIZARD_CCN" \
      -L "$LIZARD_LEN" \
      --warnings_only \
      > "$lizard_log" 2>&1 || true

    {
      printf 'ccn threshold: %s\n' "$LIZARD_CCN"
      printf 'length threshold: %s\n' "$LIZARD_LEN"
      printf 'lizard warning count: '
      grep -c 'warning:' "$lizard_log" || true
    } > "$lizard_summary"

    printf 'wrote %s\n' "$lizard_log"
    cat "$lizard_summary"
  else
    cat > "$lizard_log" <<EOF
lizard not found.

Install without touching Homebrew Python:

  brew install pipx
  pipx install lizard

or in a local venv:

  python3 -m venv .venv
  . .venv/bin/activate
  python3 -m pip install lizard
EOF
    printf 'skipping lizard; see %s\n' "$lizard_log"
  fi
}

run_cppcheck() {
  say "cppcheck style / dead-code-ish checks"

  local cppcheck_log="$OUT_DIR/cppcheck.txt"

  if have cppcheck; then
    # Prepend note explaining the glr_audio.c exclusion to prevent false-positive triaging
    cat << 'EOF' > "$cppcheck_log"
# NOTE: src/app/glr_audio.c is excluded from this scan to avoid parsing miniaudio.h (extremely slow).
# Consequently:
# 1. Functions defined in src/app/glr_audio.c are not analyzed.
# 2. Functions only called by src/app/glr_audio.c may be falsely flagged as unused (unusedFunction).
# Always verify unusedFunction findings against src/app/glr_audio.c before removing code.
--------------------------------------------------------------------------------
EOF

    # NOTE: Do NOT add -j (parallel execution) here.
    # cppcheck's unusedFunction check is incompatible with -j and will be silently
    # disabled or cause an error if -j is used, losing the dead-code check.
    cppcheck \
      --quiet \
      --enable=style,unusedFunction \
      --inline-suppr \
      --suppress=missingIncludeSystem \
      -i src/app/glr_audio.c \
      --suppress='*:*/miniaudio.h' \
      -I. \
      -Iinclude \
      "$SRC_DIR" \
      "$TEST_DIR" \
      "$TOOLS_DIR" \
      "$BENCH_DIR" \
      "gl_repl.c" \
      >> "$cppcheck_log" 2>&1 || true

    printf 'wrote %s\n' "$cppcheck_log"
  else
    cat > "$cppcheck_log" <<EOF
cppcheck not found.

Install:

  brew install cppcheck
EOF
    printf 'skipping cppcheck; see %s\n' "$cppcheck_log"
  fi
}

run_cpd() {
  local cpd_log="$OUT_DIR/cpd.txt"
  local cpd_summary="$OUT_DIR/cpd-summary.txt"

  if have pmd; then
    say "PMD CPD duplication scan (local)"
    pmd cpd \
      --minimum-tokens "$MIN_TOKENS" \
      --language cpp \
      --dir "$SRC_DIR" \
      --format text \
      > "$cpd_log" 2>&1 || true

    CPD_STATUS="ok"
  elif have docker && docker info >/dev/null 2>&1; then
    say "PMD CPD duplication scan (Docker)"
    docker run --rm \
      -v "$ROOT:/src" \
      -w /src \
      "$PMD_IMAGE" \
      cpd \
        --minimum-tokens "$MIN_TOKENS" \
        --language cpp \
        --dir "$SRC_DIR" \
        --format text \
      > "$cpd_log" 2>&1 || true

    CPD_STATUS="ok"
  else
    say "PMD CPD duplication scan"
    cat > "$cpd_log" <<EOF
PMD CPD could not be run.

To run CPD, please do one of the following:

1. Install PMD locally (so that the 'pmd' command is available in your PATH):
   On macOS:
     brew install pmd
   Or download from https://pmd.github.io/

2. Start the Docker daemon (if Docker is installed but not running).

Manual command (Docker):
  docker run --rm \\
    -v "\$PWD:/src" \\
    -w /src \\
    pmdcode/pmd:latest \\
    cpd --minimum-tokens 80 --language cpp --dir src
EOF
    printf 'skipping PMD CPD; see %s\n' "$cpd_log"
    CPD_STATUS="skipped"
  fi

  if [[ "$CPD_STATUS" = "ok" ]]; then
    {
      printf 'minimum tokens: %s\n' "$MIN_TOKENS"
      printf 'duplication blocks: '
      grep -c '^Found a' "$cpd_log" || true
    } > "$cpd_summary"

    printf 'wrote %s\n' "$cpd_log"
    cat "$cpd_summary"
  else
    : > "$cpd_summary"
  fi
}

run_churn() {
  say "churn × file size hotspots"

  local churn_log="$OUT_DIR/churn-size.txt"

  {
    printf '# Note: churn counts since src/ reorg on 2026-05-23 (pre-reorg paths filtered out)\n'
    printf '%8s %8s %8s  %s\n' "churn" "lines" "score" "file"

    git log --format= --name-only -- "$SRC_DIR" |
      sed '/^$/d' |
      sort |
      uniq -c |
      while read -r churn file; do
        [[ -f "$file" ]] || continue
        lines="$(wc -l < "$file" | tr -d ' ')"
        score=$((churn * lines))
        printf '%8d %8d %8d  %s\n' "$churn" "$lines" "$score" "$file"
      done |
      sort -nr -k3 |
      head -30
  } > "$churn_log"

  printf 'wrote %s\n' "$churn_log"
}

print_summary() {
  say "summary"

  printf 'outputs:\n'

  [[ "$RUN_CLANGD" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/clangd-diagnostics.txt"
  [[ "$RUN_CLANG_TIDY" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/clang-tidy-diagnostics.txt"
  [[ "$RUN_LIZARD" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/lizard.txt"
  [[ "$RUN_CPPCHECK" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/cppcheck.txt"
  [[ "$RUN_CPD" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/cpd.txt"
  [[ "$RUN_CHURN" -eq 1 ]] && printf '  %s\n' "$OUT_DIR/churn-size.txt"

  printf '\nquick counts:\n'

  if [[ "$RUN_CLANGD" -eq 1 && -f "$OUT_DIR/clangd-diagnostics.txt" ]]; then
    printf '  clangd diagnostics:      '
    grep -cE 'warning:|error:' "$OUT_DIR/clangd-diagnostics.txt" || true
  fi

  if [[ "$RUN_CLANG_TIDY" -eq 1 && -f "$OUT_DIR/clang-tidy-diagnostics.txt" ]]; then
    printf '  clang-tidy diagnostics:  '
    grep -cE 'warning:|error:' "$OUT_DIR/clang-tidy-diagnostics.txt" || true
  fi

  if [[ "$RUN_LIZARD" -eq 1 && -f "$OUT_DIR/lizard.txt" ]]; then
    printf '  lizard warnings:         '
    grep -c 'warning:' "$OUT_DIR/lizard.txt" || true
  fi

  if [[ "$RUN_CPPCHECK" -eq 1 && -f "$OUT_DIR/cppcheck.txt" ]]; then
    printf '  cppcheck findings:       '
    grep -cE '^\[|:[0-9]+:' "$OUT_DIR/cppcheck.txt" || true
  fi

  if [[ "$RUN_CPD" -eq 1 && -f "$OUT_DIR/cpd.txt" ]]; then
    printf '  CPD blocks:              '
    if [[ "$CPD_STATUS" = "skipped" ]]; then
      printf 'skipped (pmd/docker missing or daemon not running)\n'
    else
      grep -c '^Found a' "$OUT_DIR/cpd.txt" || true
    fi
  fi

  printf '\nDone.\n'
}

[[ "$RUN_CLANGD" -eq 1 ]] && run_clangd
[[ "$RUN_CLANG_TIDY" -eq 1 ]] && run_clang_tidy
[[ "$RUN_LIZARD" -eq 1 ]] && run_lizard
[[ "$RUN_CPPCHECK" -eq 1 ]] && run_cppcheck
[[ "$RUN_CPD" -eq 1 ]] && run_cpd
[[ "$RUN_CHURN" -eq 1 ]] && run_churn

print_summary
