#!/usr/bin/env bash
# release.sh — build gl-repl release artifacts for macOS (locally) and Linux
# (on a remote host), bundle the music pack into each, then — after an explicit
# confirmation — upload them to a GitHub release.
#
# The macOS .app is built here with `make app`; the Linux binary is compiled on
# REMOTE_HOST (an x86 Linux box with real gcc + GL dev libs) and copied back.
# All *packaging* happens locally, so the music pack (whose files are gitignored
# and often symlinks on the dev mac) is staged from this checkout for both.
#
# Usage:
#   scripts/release.sh build     # build + stage into dist/<tag>/ (no upload)
#   scripts/release.sh upload    # upload already-staged dist/<tag>/ artifacts
#   scripts/release.sh all       # build, then confirm and upload (default)
#
# Config (env vars — the Makefile `release` target forwards these):
#   TAG=<tag>            Release tag (default: `git describe` / dev-<sha>).
#   REPO=<owner/repo>    GitHub repo (default: drewwoods/gl-repl).
#   MUSIC_SRC_DIR=<dir>  Music source dir (default: assets/favorite if it holds
#                        any .mp3s, else assets).
#   REMOTE_HOST=<host>   ssh host for the Linux build (default: gracemont).
#   REMOTE_PATH=<path>   Repo path on REMOTE_HOST
#                        (default: ~/code/openGL/samples/gen-ai/gl-repl).
#   REMOTE_BRANCH=<br>   Branch the remote fast-forwards to before building
#                        (default: main).
#   SKIP_MACOS=1         Skip the macOS build/stage.
#   SKIP_LINUX=1         Skip the Linux (remote) build/stage.
#   ASSUME_YES=1         Skip the pre-upload confirmation prompt.
#   ALLOW_DIRTY=1        Don't warn when the working tree is dirty.

set -euo pipefail

# --- resolve repo root (script lives in scripts/) ----------------------------
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

# --- config with defaults ----------------------------------------------------
REPO=${REPO:-drewwoods/gl-repl}
REMOTE_HOST=${REMOTE_HOST:-gracemont}
REMOTE_PATH=${REMOTE_PATH:-'~/code/openGL/samples/gen-ai/gl-repl'}
REMOTE_BRANCH=${REMOTE_BRANCH:-main}
SKIP_MACOS=${SKIP_MACOS:-0}
SKIP_LINUX=${SKIP_LINUX:-0}
ASSUME_YES=${ASSUME_YES:-0}
ALLOW_DIRTY=${ALLOW_DIRTY:-0}

if [ -z "${TAG:-}" ]; then
    TAG=$(git describe --tags --always --dirty 2>/dev/null || true)
    [ -n "$TAG" ] || TAG="dev-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi

# Music source: prefer assets/favorite when it holds tracks, else flat assets/.
if [ -z "${MUSIC_SRC_DIR:-}" ]; then
    if ls assets/favorite/*.mp3 >/dev/null 2>&1; then
        MUSIC_SRC_DIR="assets/favorite"
    else
        MUSIC_SRC_DIR="assets"
    fi
fi

DIST="$ROOT/dist/$TAG"
MACOS_ZIP="gl-repl-$TAG-macos.zip"
# Linux archive name/arch filled in after the remote build (needs uname -m).

say()  { printf '\033[1;36mrelease:\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mrelease: warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mrelease: error:\033[0m %s\n' "$*" >&2; exit 1; }

# --- stage the music pack into <dest>/ (a "known location" beside the binary) -
# Files under MUSIC_SRC_DIR are often symlinks on the dev mac; -L dereferences.
stage_music() {
    local dest=$1 n=0 f base
    mkdir -p "$dest"
    if [ -d "$MUSIC_SRC_DIR" ]; then
        while IFS= read -r f; do
            base=$(basename "$f")
            cp -L "$f" "$dest/$base"
            n=$((n + 1))
        done < <(find "$MUSIC_SRC_DIR" -maxdepth 1 \( -type f -o -type l \) -iname '*.mp3' | LC_ALL=C sort)
    fi
    say "staged $n track(s) from $MUSIC_SRC_DIR -> ${dest#$ROOT/}"
    [ "$n" -gt 0 ] || warn "no .mp3 tracks found in $MUSIC_SRC_DIR (run 'make fetch-music' first?)"
}

warn_if_dirty() {
    [ "$ALLOW_DIRTY" = "1" ] && return 0
    if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
        warn "working tree is dirty — the macOS build reflects local changes,"
        warn "but the Linux build compiles origin/$REMOTE_BRANCH on $REMOTE_HOST."
        warn "Commit + push for a matched release (or set ALLOW_DIRTY=1 to silence)."
    fi
}

# --- macOS: build the .app locally, swap in the full music pack, zip it -------
build_macos() {
    [ "$(uname)" = "Darwin" ] || { warn "not on macOS — skipping the .app build"; return 0; }
    say "building macOS app bundle (make app)"
    make -C "$ROOT" app >&2
    [ -d "$ROOT/gl-repl.app" ] || die "make app did not produce gl-repl.app"

    # `make app` seeds Resources/assets with just sample.mp3; replace it with
    # the full pack so the released bundle ships the whole playlist.
    local res="$ROOT/gl-repl.app/Contents/Resources/assets"
    rm -f "$res"/*.mp3
    stage_music "$res"

    mkdir -p "$DIST"
    say "zipping -> dist/$TAG/$MACOS_ZIP"
    ( cd "$ROOT" && rm -f "$DIST/$MACOS_ZIP" && zip -q -r -y "$DIST/$MACOS_ZIP" gl-repl.app )
}

# --- Linux: compile on REMOTE_HOST, copy back, stage music + launcher, tar ----
build_linux() {
    say "building Linux binary on $REMOTE_HOST:$REMOTE_PATH (branch $REMOTE_BRANCH)"
    ssh "$REMOTE_HOST" "cd $REMOTE_PATH && \
        git fetch --quiet origin $REMOTE_BRANCH && \
        git checkout --quiet $REMOTE_BRANCH && \
        git pull --ff-only origin $REMOTE_BRANCH && \
        make gl-repl" >&2

    local arch bin_remote
    arch=$(ssh "$REMOTE_HOST" 'uname -m' | tr -d '[:space:]')
    [ -n "$arch" ] || arch="x86_64"
    bin_remote="$REMOTE_PATH/build/release/gl-repl"

    local stage_name="gl-repl-$TAG-linux-$arch"
    local stage="$DIST/$stage_name"
    rm -rf "$stage"
    mkdir -p "$stage"

    say "copying binary back from $REMOTE_HOST"
    scp -q "$REMOTE_HOST:$bin_remote" "$stage/gl-repl"
    chmod +x "$stage/gl-repl"

    stage_music "$stage/assets"

    cat > "$stage/README.txt" <<EOF
gl-repl — Immediate-mode OpenGL REPL ($TAG, linux-$arch)

Run from inside this directory so the bundled music is found:

    ./gl-repl

Music lives in ./assets (gl-repl scans it on startup). Drop more *.mp3
there, or into \$XDG_DATA_HOME/gl-repl/music. Run with --no-audio to skip
audio entirely.

Requires system OpenGL + GLUT/freeglut runtime libraries
(e.g. libgl1 libglu1-mesa freeglut3 on Debian/Ubuntu).
EOF

    mkdir -p "$DIST"
    local tarball="$DIST/$stage_name.tar.gz"
    say "packing -> dist/$TAG/$stage_name.tar.gz"
    ( cd "$DIST" && rm -f "$tarball" && tar czf "$tarball" "$stage_name" )
    rm -rf "$stage"
}

do_build() {
    warn_if_dirty
    mkdir -p "$DIST"
    [ "$SKIP_MACOS" = "1" ] || build_macos
    [ "$SKIP_LINUX" = "1" ] || build_linux
    say "staged artifacts in dist/$TAG/:"
    ( cd "$DIST" && ls -lh ./*.zip ./*.tar.gz 2>/dev/null | sed 's/^/  /' ) || true
}

do_upload() {
    command -v gh >/dev/null 2>&1 || die "gh CLI not found — install it or upload dist/$TAG/ manually."
    shopt -s nullglob
    local artifacts=("$DIST"/*.zip "$DIST"/*.tar.gz)
    shopt -u nullglob
    [ "${#artifacts[@]}" -gt 0 ] || die "no artifacts in dist/$TAG/ — run 'make release-build' first."

    if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
        say "release $TAG exists on $REPO — uploading (clobber)"
    else
        say "creating release $TAG on $REPO"
        gh release create "$TAG" --repo "$REPO" --title "$TAG" \
            --notes "gl-repl $TAG — macOS .app and Linux binary, music pack bundled." \
            --draft >&2
    fi
    gh release upload "$TAG" --repo "$REPO" --clobber "${artifacts[@]}" >&2
    say "uploaded ${#artifacts[@]} artifact(s) to $REPO release $TAG (draft)."
    say "review + publish: gh release edit $TAG --repo $REPO --draft=false"
}

confirm_upload() {
    if [ "$ASSUME_YES" = "1" ]; then
        return 0
    fi
    if [ ! -t 0 ] && [ ! -e /dev/tty ]; then
        warn "not attached to a terminal — skipping upload."
        say  "artifacts are staged in dist/$TAG/. Upload with: make release-upload TAG=$TAG"
        return 1
    fi
    local ans
    printf '\033[1;35mrelease:\033[0m upload the above artifacts to GitHub release "%s" on %s? [y/N] ' "$TAG" "$REPO" >&2
    read -r ans < /dev/tty || ans=""
    case "$ans" in
        y|Y|yes|YES) return 0 ;;
        *)
            say "upload declined. Artifacts remain in dist/$TAG/."
            say "Upload later with: make release-upload TAG=$TAG"
            return 1
            ;;
    esac
}

cmd=${1:-all}
case "$cmd" in
    build)
        do_build
        ;;
    upload)
        do_upload
        ;;
    all)
        do_build
        if confirm_upload; then
            do_upload
        fi
        ;;
    *)
        die "unknown subcommand '$cmd' (want: build | upload | all)"
        ;;
esac
