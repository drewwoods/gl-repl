#!/bin/sh
# build-historical.sh — build the REPL at an old SHA without OpenGL-Vibe present.
# See docs/ARCHITECTURE.md § "Building Historical Checkouts" for the full story.
# Run with `--help` for inline usage.

set -eu

show_help() {
    cat <<'EOF'
build-historical.sh — make `make` work at any old SHA in this repo.

THE 30-SECOND VERSION

  From a modern checkout (where this script and compat/ exist), tell
  the script which historical SHA you want to build:

      ./scripts/build-historical.sh --at <old-sha> sample

  The script does the checkout for you, builds via a private worktree
  under .compat-scratch/worktrees/<sha>/, then leaves your HEAD where
  it was. The build's `sample` / test binaries land inside that
  worktree (path is printed at the end so you can run them).

  Without `--at`, the script builds the *currently* checked-out SHA
  in place. That's the right mode if you're already on the old SHA
  (perhaps streamed from main: `git show main:scripts/build-
  historical.sh | sh -s -- sample`).

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

  Two modes, picked by whether `--at` is given:

    A. Worktree mode (recommended, run from main or any modern SHA):
         ./scripts/build-historical.sh --at <sha> [make-args...]
       Adds a private git worktree under
         .compat-scratch/worktrees/<sha>/
       checks out <sha> in it, splices the compat headers in, and runs
       `make` there. Your main checkout is untouched. To re-clean a
       worktree, pass `--clean` along with `--at`.

    B. In-place mode (run from inside the SHA you want to build):
         ./scripts/build-historical.sh [make-args...]
       Useful if you've already `git checkout`'d the old SHA, or are
       streaming the script from main:
         git checkout <old-sha>
         git show main:scripts/build-historical.sh | sh -s -- sample

EXAMPLES

  # Worktree mode — build sample at old SHA 041ff95 from main:
  ./scripts/build-historical.sh --at 041ff95 sample

  # Worktree mode — run a single test at an old SHA:
  ./scripts/build-historical.sh --at 041ff95 test_eval USE_GL_STUBS=1

  # Wipe a worktree before reusing it:
  ./scripts/build-historical.sh --at 041ff95 --clean sample

  # In-place mode — script is in tree, current checkout is what builds:
  ./scripts/build-historical.sh sample
  ./scripts/build-historical.sh test
  ./scripts/build-historical.sh test USE_GL_STUBS=1
  ./scripts/build-historical.sh sample USE_GLUT=1 BUILD=debug

  # Reading compat from a branch/tag other than `main`
  # (only useful if you've maintained the compat layer there):
  COMPAT_REF=compat-branch ./scripts/build-historical.sh --at 041ff95 sample

  # WRONG — passing an old SHA as COMPAT_REF tries to read compat
  # headers from a SHA that doesn't have them:
  COMPAT_REF=041ff95 ./scripts/build-historical.sh sample
  # error: 041ff95:compat/legacy-include/gl_includes.h not found
  # Fix: use `--at 041ff95` and let COMPAT_REF default to main.

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

# --- Argument parsing -------------------------------------------------------
# Recognised script flags: --help / -h, --at <ref> / --at=<ref>, --clean.
# Everything else is forwarded to make verbatim.

target_ref=""
clean_worktree=0

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            show_help
            exit 0
            ;;
        --at)
            shift
            if [ $# -eq 0 ]; then
                echo "error: --at requires a git ref argument" >&2
                exit 2
            fi
            target_ref="$1"
            shift
            ;;
        --at=*)
            target_ref="${1#--at=}"
            shift
            ;;
        --clean)
            clean_worktree=1
            shift
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

# Sanity: must be inside a git work tree.
if ! REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
    echo "error: build-historical.sh must be run from inside a git repo" >&2
    echo "       cd into a gl-repl checkout first." >&2
    exit 2
fi

COMPAT_REF="${COMPAT_REF:-main}"

# --- Worktree mode (--at <ref>) --------------------------------------------
# When --at is given, build inside a private worktree at
# .compat-scratch/worktrees/<sha>/ instead of mutating the user's HEAD.
if [ -n "$target_ref" ]; then
    if ! resolved_sha="$(git rev-parse --verify "$target_ref^{commit}" 2>/dev/null)"; then
        echo "error: '$target_ref' is not a valid git ref or commit" >&2
        exit 1
    fi
    short_sha="$(git rev-parse --short=12 "$resolved_sha")"
    worktree_dir="$REPO_ROOT/.compat-scratch/worktrees/$short_sha"

    if [ "$clean_worktree" -eq 1 ] && [ -d "$worktree_dir" ]; then
        echo "build-historical: removing stale worktree at $worktree_dir"
        git worktree remove --force "$worktree_dir" 2>/dev/null || rm -rf "$worktree_dir"
    fi

    if [ ! -d "$worktree_dir" ]; then
        echo "build-historical: adding worktree at $worktree_dir for $short_sha"
        mkdir -p "$REPO_ROOT/.compat-scratch/worktrees"
        git worktree add --detach "$worktree_dir" "$resolved_sha"
    else
        echo "build-historical: reusing worktree at $worktree_dir"
    fi

    # Populate compat headers into the worktree's local .compat-scratch dir,
    # then run make from inside it. Save the host repo root so the post-build
    # cleanup hint can reference the right path for `git worktree remove`.
    HOST_REPO_ROOT="$REPO_ROOT"
    cd "$worktree_dir"
    REPO_ROOT="$worktree_dir"
fi

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
if [ -n "${target_ref:-}" ]; then
    echo "build-historical: build artifacts will land in $REPO_ROOT"
fi

cd "$REPO_ROOT"
make \
    REPO_INCLUDE="$INCLUDE_DIR" \
    PROJECT_ROOT="$SCRATCH" \
    "$@"
make_status=$?

if [ -n "${target_ref:-}" ] && [ $make_status -eq 0 ]; then
    echo ""
    echo "build-historical: ok. Worktree preserved at:"
    echo "    $REPO_ROOT"
    echo "  cd there to run the binary, e.g. ./sample"
    echo "  remove the worktree with:"
    echo "    git -C $HOST_REPO_ROOT worktree remove --force $REPO_ROOT"
fi

exit $make_status
