#!/bin/bash
# Hard guard: the frame-time controller band must not include boot headers.
#
# src/app/ holds two bands (see src/app/README.md):
#
#   - boot band  — src/app/boot/*   : process/startup lifecycle that runs
#                  before or without a live GL context (CLI parse, init
#                  trace, capture-env hooks, frame pacer, splash, the
#                  CLI dump-and-exit path). Reached only from the host
#                  gl_repl.c (and tests / sibling boot modules).
#   - controller band — src/app/*   : the frame-time controller and its
#                  app-level services (glr_ctrl, router, camera, config,
#                  actions, audio, completion, ...). Assume a live GL
#                  context and a running frame loop.
#
# Direction rule: host -> boot -> controller -> subsystems. Boot modules
# may call DOWN into the controller band (that is how the dump path reuses
# glr_debug's formatters), but the controller band must never reach back
# UP into boot. A controller file that #includes "app/boot/..." has taken
# a dependency on the startup lifecycle it is supposed to run underneath —
# the conflation this split exists to prevent.
#
# Comment-only references are filtered out — they describe the boundary
# but do not link.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

forbidden='#include[[:space:]]+"app/boot/'

# Controller band = files directly in src/app/, excluding the boot/ subdir.
files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/app -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/app/ contains no controller-band .c / .h files — guard cannot run." >&2
  exit 1
fi

hits="$( { grep -nE "$forbidden" "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

if [ -n "$hits" ]; then
  echo "ERROR: controller-band src/app/ files include boot headers:" >&2
  echo "$hits" >&2
  echo >&2
  echo "       The frame-time controller must not depend on src/app/boot/" >&2
  echo "       (CLI parse, init trace, capture-env, frame pacer, splash," >&2
  echo "       the dump-and-exit path). Boot runs the controller, not the" >&2
  echo "       other way round. If a controller TU needs a boot symbol," >&2
  echo "       the boundary is misplaced: move the caller into boot, or" >&2
  echo "       lift the shared primitive into the controller band (as the" >&2
  echo "       glr_debug dump formatters are)." >&2
  exit 1
fi

echo "app-boot-band OK (no controller-band includes of app/boot/)"
