#!/usr/bin/env bash
# Copy web music beside the Emscripten build and write assets/music.json.
# The browser audio backend streams these paths directly instead of relying on
# Emscripten's preload .data package.
set -euo pipefail

if [[ "$#" -ne 2 ]]; then
    echo "usage: $(basename "$0") <music-src-dir> <web-assets-dir>" >&2
    exit 2
fi

SRC_DIR=$1
OUT_DIR=$2
MANIFEST="${OUT_DIR}/music.json"

json_escape() {
    sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e 's/	/\\t/g'
}

mkdir -p "${OUT_DIR}"
rm -f "${OUT_DIR}"/*.mp3 "${MANIFEST}" "${MANIFEST}.tmp"

count=0
{
    printf '[\n'
    if [[ -d "${SRC_DIR}" ]]; then
        first=1
        while IFS= read -r file; do
            [[ -f "${file}" ]] || continue
            base=$(basename "${file}")
            cp "${file}" "${OUT_DIR}/${base}"
            path="assets/${base}"
            esc_path=$(printf '%s' "${path}" | json_escape)
            if [[ "${first}" -eq 0 ]]; then
                printf ',\n'
            fi
            printf '  {"path":"%s","group":"Music"}' "${esc_path}"
            first=0
            count=$((count + 1))
        done < <(find "${SRC_DIR}" -maxdepth 1 \( -type f -o -type l \) -iname '*.mp3' | LC_ALL=C sort)
    fi
    printf '\n]\n'
} > "${MANIFEST}.tmp"

mv "${MANIFEST}.tmp" "${MANIFEST}"
echo "web audio assets: wrote ${MANIFEST} (${count} tracks)"
