#!/usr/bin/env bash
# build-web.sh - cold-start entry point for the Emscripten/wasm web build:
# sources emsdk, fetches/builds gl4es + GLU (scripts/web-deps.sh), then
# builds via `make web`. If emsdk is already activated in your shell, you
# can skip this and just run `make web` / `make web-serve` directly.
#
#   scripts/build-web.sh [--serve]
#
# --serve   Run `make web-serve` after a successful build (opt-in; a plain
#           build never blocks on a server).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EMSDK="${EMSDK:-${HOME}/src/emsdk}"

SERVE=0
for arg in "$@"; do
	case "${arg}" in
		--serve) SERVE=1 ;;
		-h|--help)
			echo "Usage: $(basename "$0") [--serve]"
			echo "  --serve   Run 'make web-serve' after a successful build."
			exit 0
			;;
		*)
			echo "Unknown argument: ${arg}" >&2
			exit 1
			;;
	esac
done

if ! command -v emcc >/dev/null 2>&1; then
	if [[ ! -f "${EMSDK}/emsdk_env.sh" ]]; then
		echo "Error: emsdk not found at ${EMSDK}" >&2
		echo "" >&2
		echo "Install emsdk:" >&2
		echo "  git clone https://github.com/emscripten-core/emsdk.git ${EMSDK}" >&2
		echo "  ${EMSDK}/emsdk install latest" >&2
		echo "  ${EMSDK}/emsdk activate latest" >&2
		echo "" >&2
		echo "Or set EMSDK=<path> if it's installed somewhere else." >&2
		exit 1
	fi
	# shellcheck disable=SC1091
	source "${EMSDK}/emsdk_env.sh"
fi

cd "${ROOT_DIR}"
"${SCRIPT_DIR}/web-deps.sh"
emmake make web

if [[ "${SERVE}" -eq 1 ]]; then
	make web-serve
fi
