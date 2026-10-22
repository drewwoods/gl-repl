#!/usr/bin/env python3
"""Keep tessellation transitions in repl_tess_advance_depth()."""
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
TOKEN = re.compile(r"[A-Za-z_]\w*|<<=|>>=|\+\+|--|->|[+*/%&|^!=<>-]=|[^\s]")
LITERALS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}

# Exact copies and lifecycle resets, never whole-file exemptions. Counts
# prevent another identical reset from silently growing this allowlist.
EXCEPTIONS = {
    ("src/repl/source_scope.c", "view->tess_depth_prefix[0]", "0"): 1,
    ("src/repl/source_scope.c", "tess_depth", "view->tess_depth_prefix[i]"): 1,
    ("src/repl/source_scope.c", "view->tess_depth_prefix[i+1]", "tess_depth"): 1,
    # Cursor teardown clears ownership even when GLU finalization is suppressed.
    ("src/repl/executor.c", "cursor->tess_depth", "0"): 1,
}


def helper_call(rhs):
    if rhs[:2] != ["repl_tess_advance_depth", "("]:
        return False
    depth = 0
    for idx, token in enumerate(rhs[1:], 1):
        depth += (token == "(") - (token == ")")
        if depth == 0:
            return idx == len(rhs) - 1
    return False


def check(path, counts):
    try:
        relative = path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        relative = str(path)
    if relative == "src/repl/command.h":
        return []
    source = path.read_text()
    # Mask comments and literals without moving diagnostics to another line.
    source = LITERALS.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), source)
    matches = list(TOKEN.finditer(source))
    tokens = [match.group() for match in matches]
    bad = []
    for idx, name in enumerate(tokens):
        if "tess_depth" not in name or not re.fullmatch(r"[A-Za-z_]\w*", name):
            continue
        start = idx
        while start >= 2 and tokens[start - 1] in {"->", "."}:
            start -= 2
        while start > 0 and tokens[start - 1] in {"*", "("}:
            start -= 1
        end = idx + 1
        while end < len(tokens) and tokens[end] == "[":
            nesting = 1
            end += 1
            while end < len(tokens) and nesting:
                nesting += (tokens[end] == "[") - (tokens[end] == "]")
                end += 1
        while end < len(tokens) and tokens[end] == ")":
            end += 1
        op = tokens[end] if end < len(tokens) else ""
        increment = op in {"++", "--"} or (start > 0 and tokens[start - 1] in {"++", "--"})
        if not increment and op not in ASSIGN:
            continue
        stop = end + 1
        while stop < len(tokens) and tokens[stop] != ";":
            stop += 1
        rhs = tokens[end + 1:stop]
        lhs = "".join(tokens[start:end])
        key = (relative, lhs, "".join(rhs))
        if not increment and op == "=":
            if helper_call(rhs):
                continue
            if start == idx and start > 0 and tokens[start - 1] == "int" and rhs == ["0"]:
                continue
            if key in EXCEPTIONS:
                counts[key] = counts.get(key, 0) + 1
                if counts[key] <= EXCEPTIONS[key]:
                    continue
        line = source.count("\n", 0, matches[idx].start()) + 1
        bad.append(f"{relative}:{line}: {lhs} {op}")
    return bad


paths = [Path(arg) for arg in sys.argv[1:]] if len(sys.argv) > 1 else [
    path for directory in ("src", "include", "tools")
    for path in (ROOT / directory).rglob("*") if path.suffix in {".c", ".h"}
] + [ROOT / "gl_repl.c", ROOT / "gl_repl.h"]
counts = {}
violations = [hit for path in paths for hit in check(path, counts)]
if violations:
    print("ERROR: tessellation depth writes must use repl_tess_advance_depth().", file=sys.stderr)
    print("\n".join(violations), file=sys.stderr)
    sys.exit(1)
print("tess-depth OK")
