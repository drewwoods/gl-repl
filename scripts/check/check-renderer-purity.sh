#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

allowlist="${1:-scripts/allowlists/renderer-purity.txt}"

python3 - "$allowlist" <<'PY'
import glob
import re
import sys
from pathlib import Path

allowlist = Path(sys.argv[1])
if not allowlist.exists():
    print(f"ERROR: missing allowlist {allowlist}", file=sys.stderr)
    sys.exit(1)

funcs = []
for raw in allowlist.read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line or line.startswith("#"):
        continue
    funcs.append(line)

sources = {}
for path in glob.glob("src/ui/**/*.c", recursive=True):
    sources[path] = Path(path).read_text(encoding="utf-8")

forbidden = [
    re.compile(r"\brepl_[A-Za-z0-9_]*_set_[A-Za-z0-9_]*\s*\("),
    re.compile(r"\brepl_action_[A-Za-z0-9_]*\s*\("),
    re.compile(r"\brepl_state_[A-Za-z0-9_]*_mut\s*\("),
    re.compile(r"^\s*\*(?!out->|output->)[A-Za-z_][A-Za-z0-9_]*->[A-Za-z0-9_]+\s*(\+\+|--|[+\-*/]?=)", re.M),
]


def extract_body(text: str, fn: str):
    m = re.search(r"^\s*void\s+" + re.escape(fn) + r"\s*\([^\)]*\)\s*\{", text, re.M)
    if not m:
        return None, None
    start = text.find("{", m.start())
    i = start
    depth = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == '/' and nxt == '/':
                state = "line_comment"
                i += 1
            elif ch == '/' and nxt == '*':
                state = "block_comment"
                i += 1
            elif ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0:
                    return text[start + 1:i], m.start()
        elif state == "string":
            if ch == '\\':
                i += 1
            elif ch == '"':
                state = "code"
        elif state == "char":
            if ch == '\\':
                i += 1
            elif ch == "'":
                state = "code"
        elif state == "line_comment":
            if ch == '\n':
                state = "code"
        elif state == "block_comment":
            if ch == '*' and nxt == '/':
                state = "code"
                i += 1

        i += 1

    return None, None

errors = []
for fn in funcs:
    found = False
    for path, text in sources.items():
        body, fn_pos = extract_body(text, fn)
        if body is None:
            continue
        found = True
        for pat in forbidden:
            m = pat.search(body)
            if m:
                line = text.count("\n", 0, fn_pos + m.start()) + 1
                errors.append(f"{path}:{line}: {fn}: forbidden render-time mutation/pattern: {m.group(0).strip()}")
        break
    if not found:
        errors.append(f"renderer function not found: {fn}")

if errors:
    print("ERROR: renderer purity check failed:", file=sys.stderr)
    for e in errors:
        print(f"  {e}", file=sys.stderr)
    sys.exit(1)

print("renderer-purity OK")
PY
