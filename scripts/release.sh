#!/usr/bin/env bash
# release.sh — build gl-repl release artifacts for macOS and Linux, bundle the
# music pack into each, then — after an explicit confirmation — upload them to a
# GitHub release.
#
# Each platform is built in one of three modes, chosen interactively (a
# checkbox-style plan menu) or via env vars:
#   skip    — don't build this platform
#   local   — build on this machine (macOS needs Darwin; Linux needs Linux)
#   remote  — build over ssh on <HOST>:<PATH>, copy the artifact back
# So you can build Linux-only on a local Linux box, build both on remote hosts,
# or the default (macOS local + Linux remote). All *packaging* happens locally,
# so the music pack (gitignored / often symlinked on the dev mac) is staged from
# this checkout for both platforms.
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
#   MACOS_MODE=<mode>    skip|local|remote for the macOS build (default local).
#   MACOS_HOST=<host>    ssh host when MACOS_MODE=remote (prompted if empty).
#   MACOS_PATH=<path>    repo path on MACOS_HOST (prompted if empty).
#   LINUX_MODE=<mode>    skip|local|remote for the Linux build (default remote).
#   LINUX_HOST=<host>    ssh host when LINUX_MODE=remote (default REMOTE_HOST).
#   LINUX_PATH=<path>    repo path on LINUX_HOST (default REMOTE_PATH).
#   REMOTE_HOST=<host>   Default Linux ssh host (default: gracemont).
#   REMOTE_PATH=<path>   Default Linux repo path
#                        (default: ~/code/openGL/samples/gen-ai/gl-repl).
#   REMOTE_BRANCH=<br>   Branch a remote fast-forwards to before building
#                        (default: main).
#   SKIP_MACOS=1         Back-compat alias for MACOS_MODE=skip.
#   SKIP_LINUX=1         Back-compat alias for LINUX_MODE=skip.
#   ASSUME_YES=1         Skip the plan menu AND the upload confirmation prompt.
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
ASSUME_YES=${ASSUME_YES:-0}
ALLOW_DIRTY=${ALLOW_DIRTY:-0}

# Per-platform build plan. Modes: skip | local | remote.
MACOS_MODE=${MACOS_MODE:-local}
MACOS_HOST=${MACOS_HOST:-}
MACOS_PATH=${MACOS_PATH:-}
LINUX_MODE=${LINUX_MODE:-remote}
LINUX_HOST=${LINUX_HOST:-$REMOTE_HOST}
LINUX_PATH=${LINUX_PATH:-$REMOTE_PATH}

# Back-compat: the old SKIP_* toggles map onto the mode.
[ "${SKIP_MACOS:-0}" = "1" ] && MACOS_MODE=skip
[ "${SKIP_LINUX:-0}" = "1" ] && LINUX_MODE=skip

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

say()  { printf '\033[1;36mrelease:\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mrelease: warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mrelease: error:\033[0m %s\n' "$*" >&2; exit 1; }

# True only when a controlling terminal is actually usable: the /dev/tty node
# exists even with no tty attached, so probe it with a write that fails
# ("Device not configured") when detached. Doesn't consume any input.
have_tty() { { true >/dev/tty; } 2>/dev/null; }

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
        warn "working tree is dirty — a local build reflects local changes, but"
        warn "a remote build compiles origin/$REMOTE_BRANCH on its host."
        warn "Commit + push for a matched release (or set ALLOW_DIRTY=1 to silence)."
    fi
}

# --- plan menu (checkbox-style: cycle each platform skip -> local -> remote) --
plan_desc() { # $1 = MACOS|LINUX
    local mode host path
    if [ "$1" = MACOS ]; then mode=$MACOS_MODE host=$MACOS_HOST path=$MACOS_PATH
    else                      mode=$LINUX_MODE host=$LINUX_HOST path=$LINUX_PATH; fi
    case "$mode" in
        skip)   printf '[ ] skip' ;;
        local)  printf '[x] build locally' ;;
        remote) printf '[x] build on remote %s:%s' "${host:-?}" "${path:-?}" ;;
    esac
}

ensure_remote_cfg() { # $1 = MACOS|LINUX — prompt for host/path if empty
    local p=$1 hv pv ans
    hv=${p}_HOST; pv=${p}_PATH
    if [ -z "${!hv}" ]; then
        printf '  %s remote ssh host: ' "$p" >&2; read -r ans < /dev/tty || ans=""
        printf -v "$hv" '%s' "$ans"
    fi
    if [ -z "${!pv}" ]; then
        printf '  %s repo path on %s: ' "$p" "${!hv}" >&2; read -r ans < /dev/tty || ans=""
        printf -v "$pv" '%s' "$ans"
    fi
}

cycle_mode() { # $1 = MACOS|LINUX — skip -> local -> remote -> skip
    local p=$1 mv cur
    mv=${p}_MODE; cur=${!mv}
    case "$cur" in
        skip)   printf -v "$mv" '%s' local ;;
        local)  printf -v "$mv" '%s' remote; ensure_remote_cfg "$p" ;;
        remote) printf -v "$mv" '%s' skip ;;
    esac
}

show_plan() {
    printf '\n\033[1mRelease build plan\033[0m — tag %s, repo %s\n' "$TAG" "$REPO" >&2
    printf '  1) macOS : %s\n' "$(plan_desc MACOS)" >&2
    printf '  2) Linux : %s\n' "$(plan_desc LINUX)" >&2
}

plan_menu() {
    [ "$ASSUME_YES" = "1" ] && return 0
    have_tty || return 0
    local cmd
    while true; do
        show_plan
        printf '  [1]/[2] cycle platform (skip->local->remote)  [Enter] proceed  [q] abort > ' >&2
        read -r cmd < /dev/tty || cmd=q
        case "$cmd" in
            1) cycle_mode MACOS ;;
            2) cycle_mode LINUX ;;
            ""|y|Y|yes|YES)
                if [ "$MACOS_MODE" = skip ] && [ "$LINUX_MODE" = skip ]; then
                    warn "both platforms are skipped — nothing to build."
                    continue
                fi
                return 0 ;;
            q|Q) die "aborted at plan menu." ;;
            *) warn "unknown choice '$cmd'." ;;
        esac
    done
}

# --- remote git sync before a remote build -----------------------------------
remote_sync() { # $1=host $2=path
    ssh "$1" "cd $2 && \
        git fetch --quiet origin $REMOTE_BRANCH && \
        git checkout --quiet $REMOTE_BRANCH && \
        git pull --ff-only origin $REMOTE_BRANCH" >&2
}

# --- macOS: build the .app (local or remote), swap in music, zip -------------
build_macos() {
    local appdir
    case "$MACOS_MODE" in
        skip) return 0 ;;
        local)
            [ "$(uname)" = "Darwin" ] || { warn "MACOS_MODE=local but not on Darwin — skipping macOS."; return 0; }
            say "building macOS app bundle locally (make app)"
            make -C "$ROOT" app >&2
            [ -d "$ROOT/gl-repl.app" ] || die "make app did not produce gl-repl.app"
            appdir="$ROOT/gl-repl.app"
            ;;
        remote)
            [ -n "$MACOS_HOST" ] || ensure_remote_cfg MACOS
            say "building macOS app bundle on $MACOS_HOST:$MACOS_PATH"
            remote_sync "$MACOS_HOST" "$MACOS_PATH"
            ssh "$MACOS_HOST" "cd $MACOS_PATH && make app" >&2
            appdir="$DIST/.macos-stage/gl-repl.app"
            rm -rf "$DIST/.macos-stage"; mkdir -p "$DIST/.macos-stage"
            say "copying gl-repl.app back from $MACOS_HOST"
            scp -q -r "$MACOS_HOST:$MACOS_PATH/gl-repl.app" "$DIST/.macos-stage/gl-repl.app"
            ;;
    esac

    # `make app` seeds Resources/assets with just sample.mp3; replace it with
    # the full pack so the released bundle ships the whole playlist.
    local res="$appdir/Contents/Resources/assets"
    rm -f "$res"/*.mp3
    stage_music "$res"

    mkdir -p "$DIST"
    say "zipping -> dist/$TAG/$MACOS_ZIP"
    ( cd "$(dirname "$appdir")" && rm -f "$DIST/$MACOS_ZIP" && zip -q -r -y "$DIST/$MACOS_ZIP" gl-repl.app )
    rm -rf "$DIST/.macos-stage"
}

# --- Linux: compile (local or remote), stage music + launcher, tar -----------
build_linux() {
    local arch stage stage_name tarball
    case "$LINUX_MODE" in
        skip) return 0 ;;
        local)
            case "$(uname)" in Linux) : ;; *) warn "LINUX_MODE=local but not on Linux — skipping Linux."; return 0 ;; esac
            say "building Linux binary locally (make gl-repl)"
            make -C "$ROOT" gl-repl >&2
            arch=$(uname -m)
            stage_name="gl-repl-$TAG-linux-$arch"
            stage="$DIST/$stage_name"; rm -rf "$stage"; mkdir -p "$stage"
            cp -L "$ROOT/build/release/gl-repl" "$stage/gl-repl"
            ;;
        remote)
            [ -n "$LINUX_HOST" ] || ensure_remote_cfg LINUX
            say "building Linux binary on $LINUX_HOST:$LINUX_PATH (branch $REMOTE_BRANCH)"
            remote_sync "$LINUX_HOST" "$LINUX_PATH"
            ssh "$LINUX_HOST" "cd $LINUX_PATH && make gl-repl" >&2
            arch=$(ssh "$LINUX_HOST" 'uname -m' | tr -d '[:space:]'); [ -n "$arch" ] || arch=x86_64
            stage_name="gl-repl-$TAG-linux-$arch"
            stage="$DIST/$stage_name"; rm -rf "$stage"; mkdir -p "$stage"
            say "copying binary back from $LINUX_HOST"
            scp -q "$LINUX_HOST:$LINUX_PATH/build/release/gl-repl" "$stage/gl-repl"
            ;;
    esac
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
    tarball="$DIST/$stage_name.tar.gz"
    say "packing -> dist/$TAG/$stage_name.tar.gz"
    ( cd "$DIST" && rm -f "$tarball" && tar czf "$tarball" "$stage_name" )
    rm -rf "$stage"
}

do_build() {
    plan_menu
    warn_if_dirty
    mkdir -p "$DIST"
    build_macos
    build_linux
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
    if ! have_tty; then
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
