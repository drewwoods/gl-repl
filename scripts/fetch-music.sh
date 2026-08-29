#!/bin/bash
# fetch-music.sh - download the optional gl-repl music pack.
#
#   scripts/fetch-music.sh [--dir <dest>] [--tag <release-tag>] [--auto]
#
# The repository ships only one sample track to keep clones light; the rest
# of the playlist is attached as *.mp3 assets to a GitHub release (default
# tag: assets-v1). This script downloads them into the per-user music folder
# that gl-repl scans on startup, so a fresh clone stays byte-identical.
#
# Uses the `gh` CLI when available (handles auth/rate limits), otherwise
# falls back to anonymous curl against the GitHub releases API.
#
# Two modes:
#   explicit (default)  Always contacts the release; a failure is an error.
#                       This is what `make fetch-music` runs.
#   --auto              The build hook (`make gl-repl`). Asks once, remembers
#                       the answer, re-downloads nothing that is already here,
#                       and never exits non-zero - a music pack is optional and
#                       must not be able to fail a build.
#
# Consent and the identity of the installed pack are cached in .music.ini at
# the repo root (gitignored). `tag` + `manifest` (a hash of the track list)
# are what "already downloaded" means: same tag, same track list, no network.
#
# Options:
#   --dir <dest>   Destination directory (default: the per-user music folder -
#                  ~/Library/Application Support/gl-repl/Music on macOS,
#                  $XDG_DATA_HOME/gl-repl/music elsewhere)
#   --tag <tag>    Release tag carrying the assets (default: assets-v1)
#   --auto         Build-hook mode described above.
#   --forget       Drop the cached consent (the next --auto run asks again).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE="$ROOT/.music.ini"

REPO="drewwoods/gl-repl"
TAG="assets-v1"
DEST=""
AUTO=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DEST="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --auto) AUTO=1; shift ;;
        --forget) rm -f "$STATE"; echo "fetch-music: cleared $STATE"; exit 0 ;;
        -h|--help) sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "fetch-music: unknown option '$1' (try --help)" >&2; exit 1 ;;
    esac
done

if [ -z "$DEST" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        DEST="$HOME/Library/Application Support/gl-repl/Music"
    else
        DEST="${XDG_DATA_HOME:-$HOME/.local/share}/gl-repl/music"
    fi
fi

# --- state file --------------------------------------------------------------
# Flat key=value; read with a grep rather than sourcing, so a hand-edited file
# can never execute anything.
state_get() {
    [ -f "$STATE" ] || return 0
    sed -n "s/^$1 *= *//p" "$STATE" | tail -n 1
}

state_put() {
    local consent="$1" manifest="$2" tracks="$3"
    cat > "$STATE" <<EOF
# gl-repl music pack state - written by scripts/fetch-music.sh.
# consent:  answer to the build-time [Y/n] prompt; 'no' is remembered.
# tag:      release the installed pack came from.
# manifest: hash of the installed track list - the "version" of this pack.
# Delete this file (or run scripts/fetch-music.sh --forget) to be asked again.
consent = $consent
tag = $TAG
dest = $DEST
tracks = $tracks
manifest = $manifest
fetched = $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
}

# Identity of what is on disk: hash of the sorted track basenames. Cheap, and
# it changes whenever the pack gains, loses, or renames a track.
manifest_of() {
    local d="$1"
    [ -d "$d" ] || { echo "-"; return 0; }
    find "$d" -maxdepth 1 -name '*.mp3' -exec basename {} \; 2>/dev/null \
        | LC_ALL=C sort \
        | { shasum -a 256 2>/dev/null || sha256sum; } \
        | cut -d' ' -f1
}

track_count() {
    find "$1" -maxdepth 1 -name '*.mp3' 2>/dev/null | wc -l | tr -d ' '
}

# --- auto mode: consent + up-to-date short-circuits --------------------------
if [ "$AUTO" = "1" ]; then
    if [ -n "${NO_MUSIC:-}" ]; then
        exit 0
    fi

    # A remembered "no" is final until --forget.
    if [ "$(state_get consent)" = "no" ]; then
        exit 0
    fi

    # Already have this exact pack? Nothing to do, no network.
    if [ "$(state_get tag)" = "$TAG" ] \
       && [ "$(state_get manifest)" = "$(manifest_of "$DEST")" ] \
       && [ "$(track_count "$DEST")" -gt 0 ]; then
        exit 0
    fi

    if [ "$(state_get consent)" != "yes" ]; then
        # Never block a non-interactive build (CI, piped output) on input.
        # The controlling terminal - not stdin - is what decides: `make` with
        # stdin redirected is still a human at a keyboard, while CI has no
        # /dev/tty at all. Skip quietly and leave consent unrecorded, so a
        # later interactive build still gets to ask.
        # `-r /dev/tty` is not the test: the node can be present and readable
        # while opening it fails with ENXIO (no controlling terminal - a
        # daemonized or detached build). Only an actual open settles it.
        if ! { exec 3<>/dev/tty; } 2>/dev/null; then
            echo "fetch-music: skipping music download (not a terminal; run 'make fetch-music')" >&2
            exit 0
        fi
        printf 'Download the optional gl-repl music pack (~94 MB) from release %s? [Y/n] ' "$TAG" >&3
        reply=""
        if ! read -r reply <&3; then
            # EOF, not an answer. Enter (empty line, read succeeds) means yes;
            # nobody there to press it must not be read as consent.
            echo "" >&3
            exec 3>&-
            echo "fetch-music: no answer — skipping the music pack this build." >&2
            exit 0
        fi
        exec 3>&-
        case "$reply" in
            [Nn]*)
                state_put no "$(manifest_of "$DEST")" "$(track_count "$DEST")"
                echo "fetch-music: skipped and remembered — 'make fetch-music' pulls it later." >&2
                exit 0
                ;;
        esac
    fi
fi

# --- download ----------------------------------------------------------------
mkdir -p "$DEST"
echo "fetch-music: downloading *.mp3 from $REPO release '$TAG' -> $DEST"

download() {
    if command -v gh >/dev/null 2>&1; then
        gh release download "$TAG" --repo "$REPO" --pattern '*.mp3' \
            --dir "$DEST" --skip-existing
    else
        api="https://api.github.com/repos/$REPO/releases/tags/$TAG"
        urls=$(curl -fsSL "$api" \
            | grep -o '"browser_download_url": *"[^"]*\.mp3"' \
            | grep -o 'https://[^"]*')
        [ -n "$urls" ] || { echo "fetch-music: no .mp3 assets on release '$TAG'" >&2; return 1; }
        for url in $urls; do
            f="$DEST/$(basename "$url")"
            if [ -e "$f" ]; then
                echo "  skip $(basename "$url") (exists)"
                continue
            fi
            echo "  get  $(basename "$url")"
            # Download beside the target and rename only on success. Writing
            # straight to "$f" would leave a truncated mp3 behind on a dropped
            # connection, and the -e test above would then "skip (exists)" it
            # forever. `set -e` does not fire inside this function (it runs as
            # an `if !` condition), so the status has to be checked by hand.
            tmp="$f.part"
            if ! curl -fSL --progress-bar -o "$tmp" "$url"; then
                rm -f "$tmp"
                echo "fetch-music: failed to download $(basename "$url")" >&2
                return 1
            fi
            mv -f "$tmp" "$f" || { rm -f "$tmp"; return 1; }
        done
    fi
}

if ! download; then
    if [ "$AUTO" = "1" ]; then
        # Offline, rate-limited, no such release: all fine. The app runs
        # without music, and the build must not care.
        echo "fetch-music: download failed — continuing without the music pack." >&2
        echo "fetch-music: run 'make fetch-music' to retry." >&2
        exit 0
    fi
    echo "fetch-music: download failed." >&2
    exit 1
fi

count=$(track_count "$DEST")
state_put yes "$(manifest_of "$DEST")" "$count"
echo "fetch-music: done — $count track(s) in $DEST (gl-repl picks them up on next start)"
