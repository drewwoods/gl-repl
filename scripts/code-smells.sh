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
CHURN_SINCE="2026-05-23"

CLANGD_BIN="${CLANGD_BIN:-clangd}"

CLANG_TIDY_BIN="${CLANG_TIDY_BIN:-/opt/homebrew/opt/llvm/bin/clang-tidy}"

# Broad bugprone/misc/readability globs minus the checks that are noise *in this
# tree*.  Each exclusion is here for one of four reasons:
#
#   already covered  - the build or another scan in this script reports it, so a
#                      second copy is pure duplication.
#   wrong standard   - the check assumes C11/C23/C++; everything here is -std=c99.
#   house style      - the check disagrees with a convention used consistently
#                      across the codebase, so every hit is a false positive.
#   heuristic        - the check guesses from names/shapes and was wrong on every
#                      hit when last audited (2026-08-07).
CLANG_TIDY_CHECKS="${CLANG_TIDY_CHECKS:-bugprone-*,misc-*,readability-*\
,-bugprone-assignment-in-if-condition\
,-bugprone-branch-clone\
,-bugprone-easily-swappable-parameters\
,-bugprone-float-loop-counter\
,-bugprone-multi-level-implicit-pointer-conversion\
,-bugprone-narrowing-conversions\
,-bugprone-suspicious-missing-comma\
,-bugprone-switch-missing-default-case\
,-misc-include-cleaner\
,-misc-no-recursion\
,-readability-avoid-nested-conditional-operator\
,-readability-braces-around-statements\
,-readability-else-after-return\
,-readability-function-cognitive-complexity\
,-readability-identifier-length\
,-readability-inconsistent-ifelse-braces\
,-readability-isolate-declaration\
,-readability-magic-numbers\
,-readability-math-missing-parentheses\
,-readability-non-const-parameter\
,-readability-redundant-declaration\
,-readability-suspicious-call-argument\
,-readability-uppercase-literal-suffix\
,-readability-use-concise-preprocessor-directives}"

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
  CLANG_TIDY_CHECKS  clang-tidy checks (comma-separated).
                     Default: bugprone-*,misc-*,readability-* minus ~24 checks
                     that are noise in this tree - ones the build already
                     reports (narrowing conversions vs -Wfloat-conversion),
                     ones lizard already covers (cognitive complexity), ones
                     assuming a newer standard than -std=c99 (concise
                     preprocessor directives), and ones that disagree with
                     house style (isolate-declaration, ifelse-braces).
                     See the comment above the assignment for the full
                     rationale.  Pass the variable to widen it back out:
                       CLANG_TIDY_CHECKS='bugprone-*,readability-*' $0 --clang-tidy

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

# A0: Verify that every .c file scanned by clangd/clang-tidy has its own
# compile-database entry.  Counts alone are insufficient because a database
# can contain duplicate configurations for one TU while omitting another.
verify_compile_commands() {
  local db_path="$1"

  if ! have python3; then
    printf 'python3 is required to validate %s\n' "$db_path" >&2
    return 1
  fi

  python3 - "$ROOT" "$SRC_DIR" "$db_path" <<'PY'
import json
import os
import sys

root = os.path.realpath(sys.argv[1])
src_dir = sys.argv[2]
db_path = sys.argv[3]

if not os.path.isabs(src_dir):
    src_dir = os.path.join(root, src_dir)
src_dir = os.path.realpath(src_dir)

expected = set()
for dirpath, _, filenames in os.walk(src_dir):
    for filename in filenames:
        if filename.endswith(".c"):
            expected.add(os.path.realpath(os.path.join(dirpath, filename)))
expected.add(os.path.realpath(os.path.join(root, "gl_repl.c")))

try:
    with open(db_path, "r", encoding="utf-8") as handle:
        database = json.load(handle)
except (OSError, json.JSONDecodeError) as exc:
    print(f"ERROR: cannot read {db_path}: {exc}", file=sys.stderr)
    raise SystemExit(1)

if not isinstance(database, list):
    print(f"ERROR: {db_path} is not a JSON array", file=sys.stderr)
    raise SystemExit(1)

actual = set()
for entry in database:
    if not isinstance(entry, dict) or not entry.get("file"):
        continue
    directory = entry.get("directory") or root
    if not os.path.isabs(directory):
        directory = os.path.join(root, directory)
    filename = entry["file"]
    if not os.path.isabs(filename):
        filename = os.path.join(directory, filename)
    actual.add(os.path.realpath(filename))

missing = sorted(expected - actual)
if missing:
    print(
        f"ERROR: {db_path} is missing {len(missing)} of "
        f"{len(expected)} scanned translation units:",
        file=sys.stderr,
    )
    for filename in missing[:20]:
        print(f"  {os.path.relpath(filename, root)}", file=sys.stderr)
    if len(missing) > 20:
        print(f"  ... and {len(missing) - 20} more", file=sys.stderr)
    raise SystemExit(1)

print(
    f"compile_commands.json coverage: {len(expected)} scanned files "
    f"present ({len(actual)} unique entries)"
)
PY
}

ensure_compile_commands() {
  say "checking compile_commands.json"

  if [[ -f compile_commands.json ]]; then
    printf 'found compile_commands.json\n'
    if verify_compile_commands compile_commands.json; then
      return 0
    fi
    printf 'compile_commands.json is incomplete; regeneration required\n' >&2
  fi

  if [[ "$ENSURE_COMPILE_DB" -eq 0 ]]; then
    printf 'compile_commands.json unavailable or incomplete; not generating because --no-compile-db was passed\n'
    return 1
  fi

  if ! have bear; then
    cat > "$OUT_DIR/compile_commands.missing.txt" <<EOF
compile_commands.json is missing or incomplete and bear is not installed.

Install bear:

  brew install bear

Then run:

  bear -- make gl-repl USE_GL_STUBS=1

clangd and clang-tidy diagnostics require compile_commands.json to understand
include paths, defines, and force-includes.
EOF

    printf 'compile_commands.json unavailable; see %s\n' "$OUT_DIR/compile_commands.missing.txt"
    return 1
  fi

  say "generating compile_commands.json with bear + stub build"
  # Use make -B so bear records every sample TU even when objects are current.
  # Keep the candidate beside the destination so the final rename is atomic;
  # validate it before replacing an existing database.
  local tmp_db
  tmp_db="$(mktemp "$ROOT/.compile_commands.XXXXXX.json")"

  # The `gl-repl` make target ends in `ln -sfn $(SAMPLE_BIN) gl-repl`, so a
  # USE_GL_STUBS=1 build repoints ./gl-repl at the windowless stub binary and
  # leaves it there.  Anyone who ran a scan and then ran the app would get a
  # binary that renders nothing.  Snapshot the link and put it back.
  local prev_link=""
  [[ -L gl-repl ]] && prev_link="$(readlink gl-repl)"

  local bear_rc=0
  bear --output "$tmp_db" -- make -B gl-repl USE_GL_STUBS=1 || bear_rc=$?

  if [[ -n "$prev_link" ]]; then
    ln -sfn "$prev_link" gl-repl
  elif [[ -L gl-repl ]]; then
    rm -f gl-repl          # no link before the build; leave none behind
  fi

  if [[ "$bear_rc" -ne 0 ]]; then
    rm -f "$tmp_db"
    printf 'bear build failed; compile_commands.json not generated\n' >&2
    return 1
  fi

  if ! verify_compile_commands "$tmp_db"; then
    rm -f "$tmp_db"
    printf 'bear produced an incomplete compile database; existing file preserved\n' >&2
    return 1
  fi

  mv "$tmp_db" compile_commands.json
  printf 'generated and validated compile_commands.json\n'
  return 0
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
    printf 'skipping clangd: compile_commands.json unavailable or incomplete\n' | tee -a "$clangd_log"
    return 1
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
    printf 'skipping clang-tidy: compile_commands.json unavailable or incomplete\n' | tee -a "$tidy_log"
    return 1
  fi

  # A1: Resolve macOS sysroot so Homebrew clang-tidy finds system headers
  local sysroot_arg=""
  if have xcrun; then
    local sdk_path
    sdk_path="$(xcrun --show-sdk-path 2>/dev/null)" || true
    if [[ -n "$sdk_path" ]]; then
      sysroot_arg="--extra-arg=-isysroot${sdk_path}"
    fi
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
        '"${sysroot_arg:+\"$sysroot_arg\"}"' \
        "$file" \
        >> "$tmpfile" 2>&1 || true
      cat "$tmpfile"
      rm -f "$tmpfile"
    ' _ {} >> "$tidy_log"

  # A2: Filter vendored-path diagnostics (miniaudio.h, freeglut) from summary.
  # A3: Collapse duplicates.  A header is re-diagnosed once per TU that includes
  # it, and clang-tidy spells the path absolutely or relatively depending on how
  # the TU reached the file - so strip the repo prefix before sort -u, or the two
  # spellings of one diagnostic both survive.  On the last full run this took
  # 2448 lines to 1088.
  grep -E "warning:|error:" "$tidy_log" | \
    grep -vE 'miniaudio\.h|third_party/freeglut' | \
    sed -e "s|^${ROOT}/||" | \
    sort -u > "$tidy_diag" || true

  printf 'wrote %s\n' "$tidy_log"
  printf 'summary %s\n' "$tidy_diag"
}

run_lizard() {
  say "lizard complexity / size"

  local lizard_log="$OUT_DIR/lizard.txt"
  local lizard_summary="$OUT_DIR/lizard-summary.txt"

  if have lizard; then
    # A5: include root gl_repl.c alongside src/
    lizard "$SRC_DIR" gl_repl.c \
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
    # A3: -Isrc matches the build's include path so cppcheck resolves
    # project headers (repl/catalog_tags.h, repl/export.h).  The -D stubs
    # for REPL_EXPORT_STRINGIFY{,2} let the #ifndef guard in export.h skip
    # the stringification-based definition that cppcheck can't expand.
    #
    # A4: -D__APPLE__ on macOS so cppcheck analyzes the correct
    # platform-gated branches (without it, e.g. clipboard always-zero
    # false positives appear from the non-macOS stub branch).
    local cppcheck_platform_defs=""
    local cppcheck_excludes=()
    if [[ "$(uname -s)" = "Darwin" ]]; then
      cppcheck_platform_defs="-D__APPLE__"
    fi
    # repl_live_demo/scenes contains staged REPL snippets (including top-level
    # for-loops), not standalone C translation units.  Feeding them to
    # cppcheck produces syntaxError aborts and contaminates whole-program
    # unusedFunction analysis.
    if [[ -d "$TOOLS_DIR/repl_live_demo/scenes" ]]; then
      cppcheck_excludes=(-i "$TOOLS_DIR/repl_live_demo/scenes")
    fi

    cppcheck \
      --quiet \
      --enable=style,unusedFunction \
      --inline-suppr \
      --suppress=missingIncludeSystem \
      -i src/app/glr_audio.c \
      --suppress='*:*/miniaudio.h' \
      -I. \
      -Iinclude \
      -Isrc \
      '-DREPL_EXPORT_STRINGIFY2(x)="0"' \
      '-DREPL_EXPORT_STRINGIFY(x)="0"' \
      $cppcheck_platform_defs \
      "${cppcheck_excludes[@]}" \
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

  # A5: use --file-list so we can include root gl_repl.c alongside src/
  local cpd_filelist
  cpd_filelist="$(mktemp "${TMPDIR:-/tmp}/cpd-files.XXXXXX.txt")"
  { find "$SRC_DIR" -name '*.c' -o -name '*.h'; printf 'gl_repl.c\n'; } > "$cpd_filelist"

  if have pmd; then
    say "PMD CPD duplication scan (local)"
    pmd cpd \
      --minimum-tokens "$MIN_TOKENS" \
      --language cpp \
      --file-list "$cpd_filelist" \
      --format text \
      > "$cpd_log" 2>&1 || true

    CPD_STATUS="ok"
  elif have docker && docker info >/dev/null 2>&1; then
    say "PMD CPD duplication scan (Docker)"
    docker run --rm \
      -v "$ROOT:/src" \
      -v "$cpd_filelist:/tmp/cpd-files.txt:ro" \
      -w /src \
      "$PMD_IMAGE" \
      cpd \
        --minimum-tokens "$MIN_TOKENS" \
        --language cpp \
        --file-list /tmp/cpd-files.txt \
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

  rm -f "$cpd_filelist"

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
    printf '# Note: churn counts since src/ reorg on %s\n' "$CHURN_SINCE"
    printf '%8s %8s %8s  %s\n' "churn" "lines" "score" "file"

    # A5: include root gl_repl.c alongside src/
    git log --since="$CHURN_SINCE" --format= --name-only -- "$SRC_DIR" gl_repl.c |
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
    # A5: count trailing [checkName] tags, not note: continuation lines
    grep -cE '\[[a-zA-Z][a-zA-Z0-9]*\]$' "$OUT_DIR/cppcheck.txt" || true
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

# Each runner is independent: a failed compile-db / docker probe must not
# abort later checks under set -e (the previous `&& run_foo` form did).
if [[ "$RUN_CLANGD" -eq 1 ]]; then run_clangd || true; fi
if [[ "$RUN_CLANG_TIDY" -eq 1 ]]; then run_clang_tidy || true; fi
if [[ "$RUN_LIZARD" -eq 1 ]]; then run_lizard || true; fi
if [[ "$RUN_CPPCHECK" -eq 1 ]]; then run_cppcheck || true; fi
if [[ "$RUN_CPD" -eq 1 ]]; then run_cpd || true; fi
if [[ "$RUN_CHURN" -eq 1 ]]; then run_churn || true; fi

print_summary
