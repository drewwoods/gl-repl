#!/bin/bash
set -euo pipefail

header="${1:-repl_state.h}"

python3 - "$header" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

m = re.search(r"typedef\s+struct\s*\{(?P<body>.*?)\}\s*ReplRuntimeState\s*;", text, re.S)
if not m:
    print(f"ERROR: ReplRuntimeState typedef not found in {path}", file=sys.stderr)
    sys.exit(1)

bad = []
for offset, raw in enumerate(m.group("body").splitlines(), start=1):
    line = raw.split("//", 1)[0]
    line = re.sub(r"/\*.*?\*/", "", line).strip()
    if not line:
        continue
    if "(*" in line:
        continue
    if "*" in line:
        bad.append(line)

if bad:
    print("ERROR: ReplRuntimeState contains pointer fields:", file=sys.stderr)
    for line in bad:
        print(f"  {line}", file=sys.stderr)
    sys.exit(1)

print("runtime-state-value-fields OK")
PY
