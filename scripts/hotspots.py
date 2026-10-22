#!/usr/bin/env python3
"""Rework signals from git history and the source tree.

Three reports, each a pointer for a human to read, not a verdict:

  fixes     Fix commits per code file and per function. A fix commit is one
            whose subject starts with `fix:` / `fix(area):` (explicit), or,
            failing that, whose subject or first body paragraph uses fix-like
            words (inferred - noisy both ways; read the commits before acting).
  cochange  Code files that keep changing in the same commits, across and
            within modules. `conf` = commits with both / commits touching the
            rarer file. High conf between unrelated modules usually means
            duplicated logic or a boundary in the wrong place - or a wiring
            hub every feature must touch.
  mirrors   Comments that declare two pieces of code must agree ("keep in
            sync", "must match", "mirrors", "lockstep", "twin of", ...) and
            tests that exist to keep two implementations agreeing. A mirror
            comment with no test behind it is agreement held by memory.

Usage (from anywhere in the repo):
  scripts/hotspots.py [--since DATE] [--top N] [--report fixes,cochange,mirrors]
  make show-hotspots [SINCE=2026-07-01]

DATE is anything `git log --since` accepts; default "3 months ago".
"""
import argparse
import collections
import itertools
import os
import re
import subprocess

EXPLICIT_FIX_RE = re.compile(r"^fix(\([^)]*\))?:", re.I)
INFERRED_FIX_RE = re.compile(
    r"\b(fix(es|ed)?|bug|regress\w*|broke(n)?|wrong|crash\w*|stale|leak\w*|"
    r"mismatch\w*|desync\w*|drift\w*|misses|dropped|lost|incorrect|"
    r"off-by-one)\b", re.I)

NON_CODE_PREFIXES = ("build/", "third_party/", "docs/", "tests/testdata/",
                     "examples/", "bench/bench-data/", "tours/", "packaging/")
VENDORED = ("include/miniaudio.h",)

MIRROR_RE = re.compile(
    r"keep (this|these|it|them|both)? ?(in sync|identical|aligned|in step|"
    r"in lockstep)|must (match|mirror|stay in sync|agree)|kept in sync|"
    r"in lockstep|keep .{0,30} identical|mirrors? (the|that|this|exactly)|"
    r"same (rule|logic|walk|order) as|twin of|duplicat(e|ed|es) (of|from|in)|"
    r"copy of the", re.I)
PARITY_TEST_RE = re.compile(
    r"static (?:void|int) (test_[a-z0-9_]*(?:parity|agree|drift|consisten|"
    r"lockstep|mirror|in_sync|_matches_)[a-z0-9_]*)\s*\(")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True,
                          check=True).stdout


def is_code(path):
    if path.startswith(NON_CODE_PREFIXES) or path in VENDORED:
        return False
    return path.endswith((".c", ".h"))


def load_commits(since):
    raw = git("log", f"--since={since}", "--no-merges",
              "--format=%x1e%H%x1f%s%x1f%b%x1d", "--name-only")
    commits = []
    for rec in raw.split("\x1e")[1:]:
        head, _, files = rec.partition("\x1d")
        h, subj, body = (head.split("\x1f") + ["", ""])[:3]
        first_para = body.strip().split("\n\n")[0] if body.strip() else ""
        if EXPLICIT_FIX_RE.match(subj):
            kind = "explicit"
        elif INFERRED_FIX_RE.search(subj) or INFERRED_FIX_RE.search(first_para):
            kind = "inferred"
        else:
            kind = None
        files = [f for f in files.split("\n") if f.strip()]
        commits.append((h, subj, kind, files))
    return commits


def report_fixes(commits, top):
    touch = collections.Counter()
    fixes = collections.Counter()
    explicit = collections.Counter()
    for _h, _s, kind, files in commits:
        for f in set(files):
            if not is_code(f):
                continue
            touch[f] += 1
            if kind:
                fixes[f] += 1
            if kind == "explicit":
                explicit[f] += 1
    n_exp = sum(1 for c in commits if c[2] == "explicit")
    n_inf = sum(1 for c in commits if c[2] == "inferred")
    print(f"## Fix commits: {n_exp} explicit (fix: / fix(area):), "
          f"{n_inf} inferred from wording\n")
    print("### Per code file")
    print(f"{'fixes':>5} {'expl':>4} {'all':>5} {'fix%':>5}  file")
    for f, n in fixes.most_common(top):
        print(f"{n:5d} {explicit[f]:4d} {touch[f]:5d} "
              f"{100 * n / touch[f]:4.0f}%  {f}")

    func_re = re.compile(r"^@@ [^@]* @@\s*(.*)$")
    name_re = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    keywords = {"if", "for", "while", "switch", "return", "sizeof"}
    func_fix = collections.Counter()
    for h, _s, kind, files in commits:
        if not kind:
            continue
        code = [f for f in files if is_code(f) and f.endswith(".c")
                and not f.startswith("tests/")]
        if not code or len(code) > 25:
            continue
        cur = None
        seen = set()
        for line in git("show", "--format=", "-U0", "--no-color", h,
                        "--", *code).split("\n"):
            if line.startswith("+++ "):
                cur = line[6:] if line.startswith("+++ b/") else None
                continue
            m = func_re.match(line)
            if not (m and cur):
                continue
            nm = name_re.search(m.group(1))
            if nm and nm.group(1) not in keywords:
                key = (cur, nm.group(1))
                if key not in seen:
                    seen.add(key)
                    func_fix[key] += 1
    print("\n### Per function (git hunk-header context: the enclosing "
          "function, sometimes the previous one for edits between functions)")
    for (f, fn), n in func_fix.most_common(top):
        print(f"{n:4d}  {fn}  ({f})")
    return touch


def report_cochange(commits, touch, top):
    def module(path):
        return os.path.dirname(path) or "."

    def stem(path):
        return re.sub(r"\.[ch]$", "", path)

    pair = collections.Counter()
    for _h, _s, _k, files in commits:
        code = sorted({f for f in files
                       if is_code(f) and not f.startswith("tests/")})
        if not 2 <= len(code) <= 12:      # skip sweeping refactors
            continue
        for a, b in itertools.combinations(code, 2):
            if stem(a) != stem(b):        # foo.c + foo.h is expected
                pair[(a, b)] += 1

    def rows(same_module, min_both, min_conf):
        out = []
        for (a, b), n in pair.items():
            if n < min_both or (module(a) == module(b)) != same_module:
                continue
            conf = n / min(touch[a], touch[b])
            if conf >= min_conf:
                out.append((conf, n, a, b))
        return sorted(out, key=lambda r: (-r[0], -r[1]))[:top]

    print("\n## Co-change pairs (code files only, commits touching 2-12 of "
          "them)\n")
    print("### Across modules (both >= 6)")
    print(f"{'both':>4} {'conf':>5}  pair")
    for conf, n, a, b in rows(False, 6, 0.0):
        print(f"{n:4d} {100 * conf:4.0f}%  {a}  <->  {b}")
    print("\n### Within a module (both >= 8, conf >= 50%)")
    for conf, n, a, b in rows(True, 8, 0.5):
        print(f"{n:4d} {100 * conf:4.0f}%  {a}  <->  {b}")


def report_mirrors(top):
    files = [f for f in git("ls-files").split("\n")
             if is_code(f) and not f.startswith("tests/")]
    hits = collections.defaultdict(list)
    for f in files:
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                for i, line in enumerate(fh, 1):
                    if MIRROR_RE.search(line):
                        hits[f].append((i, line.strip()[:160]))
        except OSError:
            continue
    total = sum(len(v) for v in hits.values())
    print(f"\n## Mirror comments: {total} in {len(hits)} files\n")
    print("### Files with the most")
    for f, lst in sorted(hits.items(), key=lambda kv: -len(kv[1]))[:top]:
        print(f"{len(lst):4d}  {f}")
    print("\n### All hits")
    for f in sorted(hits):
        for i, text in hits[f]:
            print(f"{f}:{i}: {text}")

    tests = []
    for f in git("ls-files", "tests").split("\n"):
        if not f.endswith(".c"):
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            for m in PARITY_TEST_RE.finditer(fh.read()):
                tests.append((f, m.group(1)))
    print(f"\n## Agreement tests (two implementations kept in step): "
          f"{len(tests)}\n")
    for f, name in sorted(tests):
        print(f"{name}  ({f})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--since", default="3 months ago")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--report", default="fixes,cochange,mirrors")
    args = ap.parse_args()
    reports = set(args.report.split(","))

    os.chdir(git("rev-parse", "--show-toplevel").strip())
    commits = load_commits(args.since)
    print(f"# Rework signals - {len(commits)} commits since {args.since}\n")
    touch = None
    if reports & {"fixes", "cochange"}:
        touch = report_fixes(commits, args.top) if "fixes" in reports else None
    if "cochange" in reports:
        if touch is None:
            touch = collections.Counter(
                f for _h, _s, _k, fs in commits for f in set(fs) if is_code(f))
        report_cochange(commits, touch, args.top)
    if "mirrors" in reports:
        report_mirrors(args.top)


if __name__ == "__main__":
    main()
