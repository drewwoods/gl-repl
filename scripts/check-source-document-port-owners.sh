#!/bin/bash
# Hard guard: source_document_* implementations live only in approved
# host adapters.
#
# Approved owner files:
#   src/app/glr_source_document.c          — full-app host (forwards to EditorState)
#   tools/repl_demo/source_document.c      — standalone demo backend
#   tests/support/source_document_fixture.c — optional test fixture (if added)
#
# This guard refuses any *.c outside the allowlist that defines (not just
# calls) a source_document_* symbol. A rogue REPL TU that implemented one
# of these would silently shadow the host adapter; the guard surfaces it.
#
# Phase 7 of feature/source-document-port.md.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

allowed_files=(
  "src/app/glr_source_document.c"
  "tools/repl_demo/source_document.c"
  "tests/support/source_document_fixture.c"
)

# Symbol names we own. Matches function definitions (signature on a line
# starting at column 0) — function CALLS in pipeline TUs are fine.
symbols='source_document_view|source_document_apply_change|source_document_insert_line|source_document_replace_line|source_document_load_lines|source_document_clear'

# Collect linked-worktree roots so we can exclude them from the scan.
# `git worktree list` prints one line per worktree; the first field is the
# absolute path.  We skip the main worktree (first line) and convert each
# linked-worktree absolute path to a path relative to the repo root so we can
# pass it as --exclude-dir to grep.
worktree_excludes=()
current_worktree="$(git rev-parse --show-toplevel)"
while IFS= read -r wt_path; do
  # Skip the current worktree — we want to scan that.
  [ "$wt_path" = "$current_worktree" ] && continue
  # Convert to a path relative to the repo root (handles sibling worktrees).
  rel="$(realpath --relative-to="$(pwd)" "$wt_path" 2>/dev/null || python3 -c \
    "import os,sys; print(os.path.relpath(sys.argv[1]))" "$wt_path")"

  # grep --exclude-dir matches against the basename, and passing '.' or '..'
  # or a path with '/' doesn't work as expected. But for nested worktrees,
  # excluding the basename is usually sufficient to prevent traversal.
  base="$(basename "$rel")"
  if [ "$base" != "." ] && [ "$base" != ".." ]; then
    worktree_excludes+=("--exclude-dir=$base")
  fi
done < <(git worktree list --porcelain | awk '/^worktree /{print $2}')

# Find every .c that defines (not just declares) one of the symbols.
# A definition has the symbol name followed by `(` at column 0.
offenders="$(
  grep -rlE "^[A-Za-z_].*[[:space:]]($symbols)[[:space:]]*\(" \
    --exclude-dir=".git" \
    "${worktree_excludes[@]+"${worktree_excludes[@]}"}" \
    --include="*.c" --include="*.cc" --include="*.cpp" . 2>/dev/null \
  | sed 's|^\./||' \
  | sort -u
)"

bad=""
approved_count=0
for f in $offenders; do
  approved=0
  for ok in "${allowed_files[@]}"; do
    if [ "$f" = "$ok" ]; then
      approved=1
      break
    fi
  done
  if [ "$approved" -eq 1 ]; then
    approved_count=$((approved_count + 1))
  else
    bad="$bad  $f
"
  fi
done

if [ -n "$bad" ]; then
  echo "ERROR: source_document_* symbol(s) defined outside the allowlist:" >&2
  printf "%s" "$bad" >&2
  echo "       Approved owners:" >&2
  for ok in "${allowed_files[@]}"; do
    echo "         $ok" >&2
  done
  exit 1
fi

echo "source-document-port-owners OK ($approved_count approved owner(s) defining source_document_*)"
