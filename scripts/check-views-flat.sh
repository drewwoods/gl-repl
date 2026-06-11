#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline="${1:-scripts/baselines/views-flat-violations.txt}"

python3 - "$baseline" <<'PY'
import glob
import re
import sys
from pathlib import Path

baseline_path = Path(sys.argv[1])

patterns = [
    "src/repl/state_views.h",
    "ui_*.h",
    "src/scene/render_types.h",
    "src/scene/guides/guides_shared.h",
]

files = []
for pat in patterns:
    files.extend(sorted(glob.glob(pat)))

if not files:
    print("ERROR: no headers found for flat-view scan", file=sys.stderr)
    sys.exit(1)

scan_names = {"FrameRenderContext", "ReplayFadePlan"}

violations = {}

for path in files:
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    in_struct = False
    brace_depth = 0
    block = []
    start_line = 0

    for idx, line in enumerate(lines, start=1):
        if not in_struct:
            if re.match(r"^\s*typedef\s+struct\b", line):
                in_struct = True
                brace_depth = line.count("{") - line.count("}")
                block = [(idx, line)]
                start_line = idx
            continue

        block.append((idx, line))
        brace_depth += line.count("{") - line.count("}")

        if brace_depth > 0:
            continue

        # End of typedef struct block.
        end_line = line
        m = re.search(r"}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;", end_line)
        struct_name = m.group(1) if m else f"<anonymous@{path}:{start_line}>"

        should_scan = False
        if path == "src/repl/state_views.h":
            should_scan = True
        elif struct_name.startswith("Ui") and struct_name.endswith(("View", "State", "Output")):
            should_scan = True
        elif struct_name.startswith("Scene") and struct_name.endswith(("View", "State", "Config", "Context", "Output")):
            should_scan = True
        elif struct_name in scan_names:
            should_scan = True

        if should_scan:
            bad = 0
            for ln, raw in block:
                s = raw.strip()
                if not s:
                    continue
                if s.startswith("/*") or s.startswith("*") or s.startswith("//"):
                    continue
                if s.startswith("typedef") or s.startswith("}"):
                    continue

                # Drop trailing line comments before checks.
                s = s.split("//", 1)[0].strip()
                # Drop inline block comments on the same line.
                s = re.sub(r"/\\*.*?\\*/", "", s).strip()
                # Drop a block comment that opens here but closes on a
                # later line (trailing field comment that wraps); the
                # continuation lines start with '*' and are skipped above.
                s = re.sub(r"/\\*.*$", "", s).strip()
                if not s:
                    continue

                # Ignore function pointers.
                if "(*" in s:
                    continue

                if "*" not in s:
                    continue

                # Any const in the declaration line is treated as allowed.
                if re.search(r"\bconst\b", s):
                    continue

                # Pointer field to mutable data.
                bad += 1

            if bad > 0:
                violations[f"{path}:{struct_name}"] = bad

        in_struct = False
        brace_depth = 0
        block = []

# Parse baseline (key count)
baseline = {}
if baseline_path.exists():
    for raw in baseline_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.rsplit(" ", 1)
        if len(parts) != 2:
            print(f"ERROR: malformed baseline line: {raw}", file=sys.stderr)
            sys.exit(1)
        key, val = parts
        try:
            baseline[key] = int(val)
        except ValueError:
            print(f"ERROR: malformed baseline count: {raw}", file=sys.stderr)
            sys.exit(1)

errors = []
for key, count in sorted(violations.items()):
    limit = baseline.get(key)
    if limit is None:
        errors.append(f"NEW violation bucket {key} count={count} (no baseline)")
    elif count > limit:
        errors.append(f"INCREASED {key} count={count} baseline={limit}")

if errors:
    print("ERROR: flat-view mutable-pointer violations exceeded baseline:", file=sys.stderr)
    for e in errors:
        print(f"  {e}", file=sys.stderr)
    print("Current buckets:", file=sys.stderr)
    for key in sorted(violations):
        print(f"  {key} {violations[key]}", file=sys.stderr)
    sys.exit(1)

current_total = sum(violations.values())
baseline_total = sum(baseline.values())
print(f"views-flat-types OK (current mutable-pointer fields: {current_total}, baseline: {baseline_total})")
PY
