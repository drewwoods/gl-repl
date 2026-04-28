#!/bin/bash
set -euo pipefail

strict="${CHECK_OUTPUT_ACTUALIZATION_STRICT:-0}"

python3 - "$strict" <<'PY'
import glob
import re
import sys
from pathlib import Path

strict = sys.argv[1] == "1"

ctrl_path = Path("imrepl_ctrl.c")
if not ctrl_path.exists():
    print("ERROR: imrepl_ctrl.c missing", file=sys.stderr)
    sys.exit(1)
ctrl = ctrl_path.read_text(encoding="utf-8")

missing = []
output_structs = 0

for hdr in sorted(glob.glob("ui_*.h")):
    text = Path(hdr).read_text(encoding="utf-8")
    for m in re.finditer(r"typedef\s+struct\s+([A-Za-z_][A-Za-z0-9_]*)?\s*\{(.*?)\}\s*(Ui[A-Za-z0-9_]*Output)\s*;", text, re.S):
        body = m.group(2)
        name = m.group(3)
        output_structs += 1

        fields = []
        for raw in body.splitlines():
            line = raw.strip()
            if not line or line.startswith("/*") or line.startswith("*") or line.startswith("//"):
                continue
            line = line.split("//", 1)[0].strip()
            fm = re.match(r"^[A-Za-z_][A-Za-z0-9_\s\*]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]+\])?\s*;", line)
            if fm:
                fields.append(fm.group(1))

        for field in fields:
            if re.search(r"\.(%s)\b" % re.escape(field), ctrl) is None and re.search(r"->(%s)\b" % re.escape(field), ctrl) is None:
                missing.append(f"{name}.{field}")

if output_structs == 0:
    print("output-actualization OK (no Ui*Output structs yet)")
    sys.exit(0)

if missing:
    msg = "Missing controller actualization references for: " + ", ".join(missing)
    if strict:
        print("ERROR: " + msg, file=sys.stderr)
        sys.exit(1)
    print("WARN: " + msg)
    print("output-actualization WARN (non-strict mode)")
    sys.exit(0)

print("output-actualization OK")
PY
