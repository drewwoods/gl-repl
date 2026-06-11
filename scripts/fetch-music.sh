#!/bin/bash
# fetch-music.sh - download the optional gl-repl music pack.
#
#   scripts/fetch-music.sh [--dir <dest>] [--tag <release-tag>]
#
# The repository ships only one sample track to keep clones light; the rest
# of the playlist is attached as *.mp3 assets to a GitHub release (default
# tag: assets-v1). This script downloads them into the per-user music folder
# that gl-repl scans on startup, so a fresh clone stays byte-identical.
#
# Uses the `gh` CLI when available (handles auth/rate limits), otherwise
# falls back to anonymous curl against the GitHub releases API.
#
# Options:
#   --dir <dest>   Destination directory (default: the per-user music folder —
#                  ~/Library/Application Support/gl-repl/Music on macOS,
#                  $XDG_DATA_HOME/gl-repl/music elsewhere)
#   --tag <tag>    Release tag carrying the assets (default: assets-v1)

set -euo pipefail

REPO="drewwoods/gl-repl"
TAG="assets-v1"
DEST=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DEST="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "fetch-music.sh: unknown option '$1' (try --help)" >&2; exit 1 ;;
    esac
done

if [ -z "$DEST" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        DEST="$HOME/Library/Application Support/gl-repl/Music"
    else
        DEST="${XDG_DATA_HOME:-$HOME/.local/share}/gl-repl/music"
    fi
fi

mkdir -p "$DEST"
echo "fetch-music: downloading *.mp3 from $REPO release '$TAG' -> $DEST"

if command -v gh >/dev/null 2>&1; then
    gh release download "$TAG" --repo "$REPO" --pattern '*.mp3' \
        --dir "$DEST" --skip-existing
else
    api="https://api.github.com/repos/$REPO/releases/tags/$TAG"
    urls=$(curl -fsSL "$api" \
        | grep -o '"browser_download_url": *"[^"]*\.mp3"' \
        | grep -o 'https://[^"]*')
    [ -n "$urls" ] || { echo "fetch-music: no .mp3 assets on release '$TAG'" >&2; exit 1; }
    for url in $urls; do
        f="$DEST/$(basename "$url")"
        if [ -e "$f" ]; then
            echo "  skip $(basename "$url") (exists)"
        else
            echo "  get  $(basename "$url")"
            curl -fSL --progress-bar -o "$f" "$url"
        fi
    done
fi

count=$(find "$DEST" -name '*.mp3' | wc -l | tr -d ' ')
echo "fetch-music: done — $count track(s) in $DEST (gl-repl picks them up on next start)"
