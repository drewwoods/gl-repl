#!/bin/sh
# build-historical.sh — build the REPL at an old SHA without OpenGL-Vibe present.
# See ARCHITECTURE.md § "Building Historical Checkouts" for the full story.
# Run with `--help` for inline usage.

set -eu

show_help() {
    cat <<'EOF'
build-historical.sh — make `make` work at any old SHA in this repo.

WHY THIS EXISTS

  This repo was hoisted out of OpenGL-Vibe in April 2026. Before the hoist,
  the REPL lived at OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/
  and its Makefile resolved
      PROJECT_ROOT := $(abspath ../../..)         # OpenGL-Vibe root
      REPO_INCLUDE := $(PROJECT_ROOT)/include     # OpenGL-Vibe/include
  That's where OpenGL-Vibe's project-wide gl_includes.h lived (and miniaudio.h
  via the same convention). After the hoist those paths point nowhere useful,
  so any historical SHA fails to compile with `make`.

  Modern HEAD vendors a slim include/gl_includes.h and include/miniaudio.h
  at the repo root and rewires the Makefile to use them via -Iinclude. This
  script does the equivalent for any older SHA without modifying its tree.

WHAT IT DOES

  1. Materialises two compat headers into a scratch directory at
     ./.compat-scratch/include/  :

       - gl_includes.h   — pulled from <ref>:compat/legacy-include/gl_includes.h
                            (a fat compat header that also transitively
                            includes <stdlib.h>/<stdio.h>/<string.h>/<math.h>;
                            old export templates relied on those being
                            pulled in by OpenGL-Vibe's bundled utilities).
       - miniaudio.h     — pulled from <ref>:compat/legacy-include/miniaudio.h
                            if present, else from <ref>:include/miniaudio.h.

     <ref> defaults to "main" and can be overridden via COMPAT_REF=… (any
     branch, tag, or SHA that has the compat layer tracked).

  2. Invokes `make` in the repo root with PROJECT_ROOT and REPO_INCLUDE
     overridden so the historical Makefile finds the headers in the scratch
     dir instead of the long-gone OpenGL-Vibe layout. Any extra arguments
     after the script name are forwarded to make verbatim.

TWO REFS — DON'T CONFUSE THEM

  This script juggles two distinct refs:

    a. The *checked-out* ref: the version of the code you want to build.
       Set this with `git checkout <sha-or-branch>` before running.

    b. COMPAT_REF (env var, default "main"): where the script reads the
       compat headers from. Must be a ref that tracks
       compat/legacy-include/  — i.e. `main` or the bootstrap commit and
       later. **Old SHAs do NOT have compat/ tracked**, so passing an
       old SHA as COMPAT_REF will fail with "compat/legacy-include/
       gl_includes.h not found".

  In normal use you only set (a). Leave COMPAT_REF unset (or =main) and
  the script will splice main's compat headers into whatever old SHA
  you're sitting on. Setting COMPAT_REF only makes sense if you've
  forked the compat layer onto a branch or tag.

USAGE

  From inside the repo, on any SHA (modern or historical):

    git checkout <old-sha>                                    # ref (a)
    git show main:scripts/build-historical.sh | sh -s -- <make-args>

    # Or, if the script is checked out (modern SHAs only):
    ./scripts/build-historical.sh <make-args>

EXAMPLES

  # Build sample at old SHA 041ff95:
  git checkout 041ff95
  git show main:scripts/build-historical.sh | sh -s -- sample

  # Modern SHA — script is in tree:
  ./scripts/build-historical.sh sample
  ./scripts/build-historical.sh test
  ./scripts/build-historical.sh test USE_GL_STUBS=1
  ./scripts/build-historical.sh sample USE_GLUT=1 BUILD=debug

  # Reading compat from a branch/tag other than `main`
  # (only useful if you've maintained the compat layer there):
  COMPAT_REF=compat-branch ./scripts/build-historical.sh sample

  # WRONG — passing an old SHA as COMPAT_REF tries to read compat
  # headers from a SHA that doesn't have them:
  COMPAT_REF=041ff95 ./scripts/build-historical.sh sample
  # error: 041ff95:compat/legacy-include/gl_includes.h not found
  # Fix: drop COMPAT_REF and `git checkout 041ff95` instead.

ENVIRONMENT

  COMPAT_REF   git ref to read compat/legacy-include/gl_includes.h and
               miniaudio.h from. Defaults to "main". This is NOT the
               version you want to build — see "TWO REFS" above. Only
               override this if you've published the compat layer on
               another branch or tag.

LIMITATIONS

  - The very first commit in the repo (the original
    "rename displaylist-dynamic-rendering --> immediate-mode-repl") used
    `#include "gl_includes.h"` (quoted form, source-dir lookup) rather
    than the bracket form expected by the -I path. That single commit
    will still fail; everything from the next commit onward should
    build.

  - This script only fixes the include/header layout. Other pre-existing
    bugs at older SHAs (e.g. moments where bench_repl.c referenced
    renamed symbols, or examples broke after a refactor) are not
    repaired — the goal is "this is what an old SHA looked like at the
    time," not "this builds bug-free."

CLEANUP

  The scratch dir at ./.compat-scratch/ is not auto-deleted. Modern HEAD
  has it in .gitignore; old SHAs may show it as untracked in `git status`.
  Remove it with `rm -rf .compat-scratch` whenever you like.
EOF
}

# Argument parsing: only --help / -h are recognized as script flags;
# everything else is forwarded to make.
case "${1:-}" in
    -h|--help)
        show_help
        exit 0
        ;;
esac

# Sanity: must be inside a git work tree.
if ! REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
    echo "error: build-historical.sh must be run from inside a git repo" >&2
    echo "       cd into a gl-repl checkout first." >&2
    exit 2
fi

COMPAT_REF="${COMPAT_REF:-main}"
SCRATCH="$REPO_ROOT/.compat-scratch"
INCLUDE_DIR="$SCRATCH/include"

mkdir -p "$INCLUDE_DIR"

# Pull the compat gl_includes.h from the named ref. This is the fat
# version under compat/legacy-include/, NOT the slim include/gl_includes.h
# at HEAD — historical export templates rely on transitive stdlib.
if ! git show "$COMPAT_REF:compat/legacy-include/gl_includes.h" \
        > "$INCLUDE_DIR/gl_includes.h" 2>/dev/null; then
    rm -f "$INCLUDE_DIR/gl_includes.h"
    echo "error: $COMPAT_REF:compat/legacy-include/gl_includes.h not found" >&2
    echo "" >&2
    echo "  COMPAT_REF tells this script where to *read the compat headers from*," >&2
    echo "  not which version of the code to build. The default 'main' is almost" >&2
    echo "  always what you want." >&2
    echo "" >&2
    echo "  If you're trying to build at an old SHA, do this instead:" >&2
    echo "      git checkout <old-sha>" >&2
    echo "      ./scripts/build-historical.sh <make-args>      # leaves COMPAT_REF=main" >&2
    echo "" >&2
    echo "  Run with --help for the full distinction." >&2
    exit 1
fi

# Pull miniaudio.h from the same ref. Try compat/ first, fall back to
# include/ (HEAD has it under include/, no separate compat copy).
if ! git show "$COMPAT_REF:compat/legacy-include/miniaudio.h" \
        > "$INCLUDE_DIR/miniaudio.h" 2>/dev/null; then
    if ! git show "$COMPAT_REF:include/miniaudio.h" \
            > "$INCLUDE_DIR/miniaudio.h" 2>/dev/null; then
        echo "error: miniaudio.h not found at $COMPAT_REF" >&2
        echo "       Tried <ref>:compat/legacy-include/miniaudio.h and" >&2
        echo "             <ref>:include/miniaudio.h — neither exists." >&2
        rm -f "$INCLUDE_DIR/miniaudio.h"
        exit 1
    fi
fi

echo "build-historical: using headers from $COMPAT_REF -> $INCLUDE_DIR"
echo "build-historical: invoking make with REPO_INCLUDE=$INCLUDE_DIR"

cd "$REPO_ROOT"
exec make \
    REPO_INCLUDE="$INCLUDE_DIR" \
    PROJECT_ROOT="$SCRATCH" \
    "$@"
