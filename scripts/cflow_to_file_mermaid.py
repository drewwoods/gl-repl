#!/usr/bin/env python3
"""
Convert cflow output to a file-level Mermaid flowchart.

Usage:
    cflow *.c | ./scripts/cflow_to_file_mermaid.py > callgraph-files.mmd
    cflow -m imrepl_ctrl_display_frame *.c | ./scripts/cflow_to_file_mermaid.py > callgraph-files.mmd
"""

import argparse
import os
import re
import sys
from collections import defaultdict

STDLIB_PATTERNS = [
    r"^(printf|fprintf|sprintf|snprintf|strlen|strcpy|strcat|memcpy|memmove|malloc|free|calloc|realloc)",
    r"^(sin|cos|tan|sqrt|abs|pow|floor|ceil|fmod|fmax|fmin)",
    r"^(sin|cos|tan)f$",
    r"^gl[A-Z]",
    r"^glu[A-Z]",
    r"^glut[A-Z]",
    r"^prof_",
]

FUNC_LINE_RE = re.compile(
    r"^(\s*)([A-Za-z_]\w*)\(\)(?:\s+<.*? at ([^:>]+):\d+>)?:?\s*$"
)

GROUP_ORDER = [
    "App Shell",
    "Controller",
    "REPL",
    "Scene",
    "UI",
    "Support",
    "Other",
]


def should_filter(name, filter_stdlib=False, filter_glnums=False):
    """Determine if a function should be filtered from output."""
    if filter_glnums and (name.startswith("gl") or name.startswith("glu") or name.startswith("glut")):
        return True

    if filter_stdlib:
        for pattern in STDLIB_PATTERNS:
            if re.match(pattern, name):
                return True

    return False


def make_node_key(func_name, file_name):
    """Make a stable function key, keeping file-local statics distinct when possible."""
    if file_name:
        return f"{func_name}@{file_name}"
    return func_name


def parse_cflow(lines):
    """Parse cflow output into function nodes, call edges, and file ownership metadata."""
    nodes = set()
    calls = []
    current_stack = []
    node_to_file = {}
    name_to_files = defaultdict(set)

    for raw_line in lines:
        line = raw_line.rstrip("\n")
        match = FUNC_LINE_RE.match(line)
        if not match:
            continue

        indent_text, func_name, file_name = match.groups()
        indent = len(indent_text.expandtabs(4))
        level = indent // 4 if indent % 4 == 0 else (indent + 3) // 4

        if file_name:
            file_name = os.path.basename(file_name)
            name_to_files[func_name].add(file_name)

        node_key = make_node_key(func_name, file_name)
        if file_name:
            node_to_file[node_key] = file_name

        nodes.add(node_key)

        if level < len(current_stack):
            current_stack = current_stack[:level]

        if current_stack:
            calls.append((current_stack[-1], node_key))

        current_stack.append(node_key)

    return nodes, calls, node_to_file, name_to_files


def node_name(node_key):
    """Return the bare function name from a node key."""
    return node_key.split("@", 1)[0]


def resolve_file(node_key, node_to_file, name_to_files):
    """Resolve a function node to a file when possible."""
    file_name = node_to_file.get(node_key)
    if file_name:
        return file_name

    candidates = sorted(name_to_files.get(node_name(node_key), set()))
    if len(candidates) == 1:
        return candidates[0]

    return None


def classify_file(file_name):
    """Group files by the repo's ownership prefixes."""
    stem = os.path.splitext(os.path.basename(file_name))[0]

    if stem == "sample":
        return "App Shell"
    if stem.startswith("imrepl_"):
        return "Controller"
    if stem.startswith("repl_"):
        return "REPL"
    if stem.startswith("scene_"):
        return "Scene"
    if stem.startswith("ui_"):
        return "UI"
    if stem in {"cmd_format", "prof", "gl_stub_counts"}:
        return "Support"
    return "Other"


def sanitize_id(name):
    """Convert a label to a Mermaid-safe identifier."""
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)


def collapse_to_files(
    nodes,
    calls,
    node_to_file,
    name_to_files,
    filter_stdlib=False,
    filter_glnums=False,
    include_self=False,
):
    """Collapse function-level cflow output to file-level nodes and edges."""
    files = set()
    edges = defaultdict(int)

    for node_key in nodes:
        func_name = node_name(node_key)
        if should_filter(func_name, filter_stdlib=filter_stdlib, filter_glnums=filter_glnums):
            continue

        file_name = resolve_file(node_key, node_to_file, name_to_files)
        if file_name:
            files.add(file_name)

    for caller_key, callee_key in calls:
        caller_name = node_name(caller_key)
        callee_name = node_name(callee_key)
        if should_filter(caller_name, filter_stdlib=filter_stdlib, filter_glnums=filter_glnums):
            continue
        if should_filter(callee_name, filter_stdlib=filter_stdlib, filter_glnums=filter_glnums):
            continue

        caller_file = resolve_file(caller_key, node_to_file, name_to_files)
        callee_file = resolve_file(callee_key, node_to_file, name_to_files)
        if not caller_file or not callee_file:
            continue
        if caller_file == callee_file and not include_self:
            continue

        files.add(caller_file)
        files.add(callee_file)
        edges[(caller_file, callee_file)] += 1

    return files, edges


def generate_mermaid(files, edges, group_by_prefix=True):
    """Generate a Mermaid flowchart from collapsed file edges."""
    lines = ["flowchart LR"]

    if group_by_prefix:
        grouped = defaultdict(list)
        for file_name in sorted(files):
            grouped[classify_file(file_name)].append(file_name)

        for group_name in GROUP_ORDER:
            members = grouped.get(group_name)
            if not members:
                continue
            lines.append(f'    subgraph {sanitize_id(group_name)}["{group_name}"]')
            for file_name in members:
                lines.append(f'        {sanitize_id(file_name)}["{file_name}"]')
            lines.append("    end")
    else:
        for file_name in sorted(files):
            lines.append(f'    {sanitize_id(file_name)}["{file_name}"]')

    for caller_file, callee_file in sorted(edges):
        weight = edges[(caller_file, callee_file)]
        if weight > 1:
            lines.append(
                f'    {sanitize_id(caller_file)} -->|{weight}| {sanitize_id(callee_file)}'
            )
        else:
            lines.append(f'    {sanitize_id(caller_file)} --> {sanitize_id(callee_file)}')

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Convert cflow output to a file-level Mermaid diagram"
    )
    parser.add_argument(
        "--no-stdlib",
        action="store_true",
        help="Filter out standard library and profiling helper functions",
    )
    parser.add_argument(
        "--no-gl",
        action="store_true",
        help="Filter out OpenGL/GLU/GLUT calls",
    )
    parser.add_argument(
        "--include-self",
        action="store_true",
        help="Keep same-file self edges in the collapsed graph",
    )
    parser.add_argument(
        "--no-groups",
        action="store_true",
        help="Do not group files into ownership subgraphs",
    )
    args = parser.parse_args()

    nodes, calls, node_to_file, name_to_files = parse_cflow(sys.stdin.readlines())
    if not nodes:
        print("No functions found in cflow output", file=sys.stderr)
        sys.exit(1)

    files, edges = collapse_to_files(
        nodes,
        calls,
        node_to_file,
        name_to_files,
        filter_stdlib=args.no_stdlib,
        filter_glnums=args.no_gl,
        include_self=args.include_self,
    )

    if not files:
        print("No file-level nodes found after filtering", file=sys.stderr)
        sys.exit(1)

    print(generate_mermaid(files, edges, group_by_prefix=not args.no_groups))


if __name__ == "__main__":
    main()
