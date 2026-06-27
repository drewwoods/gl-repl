#!/bin/bash
# Isolation guard for the subsystem demos (memprof / variable_panel /
# color_picker — tools/*_demo).
#
# Each demo must link ONLY from {src/subsystems, src/support,
# src/ui/support, src/ui/subsystems, src/ui/core} (+ the
# tests/gl-stubs/gl_stub_counts.c link stub) — never src/ui/app,
# src/app, src/repl, or src/editor. That is the whole point of the
# demos: they prove the subsystem is decoupled from the controller /
# REPL / editor layers (the same role render3d_demo / repl_demo /
# editor_demo play for their subtrees).
#
# Three checks:
#   1. The Makefile <DEP_VAR> list contains no forbidden-layer paths.
#   2. The demo driver's own .c/.h include no forbidden-layer headers.
#   3. nm <binary> exposes no repl_/editor_/glr_ symbols (defined or
#      referenced). Skipped when the binary is not built.
#
# Usage: check-subsystem-demo-isolation.sh <MAKE_DEP_VAR> <demo_dir> <binary>
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

dep_var="${1:?usage: <MAKE_DEP_VAR> <demo_dir> <binary>}"
demo_dir="${2:?missing demo_dir}"
binary="${3:?missing binary}"

forbidden='src/ui/app/|src/app/|src/repl/|src/editor/'

# --- Check 1: the Makefile dependency list -----------------------------------
deps="$(awk -v v="^${dep_var}[[:space:]]*=" '
    $0 ~ v { in_block = 1 }
    in_block {
        print
        if ($0 !~ /\\$/) in_block = 0
    }
' Makefile)"

if [ -z "$deps" ]; then
  echo "ERROR: $dep_var not found in Makefile" >&2
  exit 1
fi

bad_srcs="$(printf '%s\n' "$deps" | grep -E "$forbidden" || true)"
if [ -n "$bad_srcs" ]; then
  echo "ERROR: $dep_var references forbidden layers (ui/app, app, repl, editor):" >&2
  echo "$bad_srcs" >&2
  exit 1
fi

# --- Check 2: the demo driver's own includes ---------------------------------
bad_inc="$(grep -rEn '#[[:space:]]*include[[:space:]]*"(ui/app|app|repl|editor)/' "$demo_dir" 2>/dev/null || true)"
if [ -n "$bad_inc" ]; then
  echo "ERROR: $demo_dir includes forbidden-layer headers:" >&2
  echo "$bad_inc" >&2
  exit 1
fi

# --- Check 3: nm negative test (when the binary is built) --------------------
if [ -x "$binary" ]; then
  hits="$(nm "$binary" 2>/dev/null | awk '{print $NF}' \
          | grep -E '^_?(repl|editor|glr)_' | sort -u || true)"
  if [ -n "$hits" ]; then
    echo "ERROR: nm $binary exposes forbidden-layer symbols:" >&2
    printf '%s\n' "$hits" >&2
    exit 1
  fi
  nm_note="$binary symbol-clean"
else
  nm_note="$binary not built — nm check skipped"
fi

echo "subsystem-demo-isolation OK ($dep_var: allowed dirs only; $demo_dir clean; $nm_note)"
