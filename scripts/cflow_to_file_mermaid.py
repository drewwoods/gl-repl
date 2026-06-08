#!/usr/bin/env python3
"""
Convert cflow output to a file-level Mermaid flowchart.

Usage:
    cflow *.c | ./scripts/cflow_to_file_mermaid.py > callgraph-files.mmd
    cflow -m glr_ctrl_display_frame *.c | ./scripts/cflow_to_file_mermaid.py > callgraph-files.mmd
"""

import argparse
import fnmatch
import json
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

DEFAULT_EDGE_STYLES = {
    "local_group": "stroke:#94a3b8,stroke-width:1.5px,color:#64748b",
    "cross_group": "stroke:#d97706,stroke-width:3px,color:#d97706",
}

DEFAULT_GROUP_CONFIG = {
    "groups": [
        {"id": "app_shell", "label": "App Shell", "files": ["sample.c"]},
        {"id": "controller", "label": "Controller", "patterns": ["imrepl_*.c"]},
        {"id": "repl", "label": "REPL", "patterns": ["repl_*.c"]},
        {"id": "scene", "label": "Scene", "patterns": ["scene_*.c"]},
        {"id": "ui", "label": "UI", "patterns": ["ui_*.c"]},
        {
            "id": "support",
            "label": "Support",
            "files": ["cmd_format.c", "prof.c", "tests/gl-stubs/gl_stub_counts.c"],
        },
    ],
    "fallback_group": {"id": "other", "label": "Other"},
    "edge_styles": DEFAULT_EDGE_STYLES,
}


def should_filter(name, filter_stdlib=False, filter_glnums=False):
    """Determine if a function should be filtered from output."""
    if filter_glnums and (name.startswith("gl") or name.startswith("glu") or name.startswith("glut")):
        return True

    if filter_stdlib:
        for pattern in STDLIB_PATTERNS:
            if re.match(pattern, name):
                return True

    return False


def normalize_file_path(path):
    """Normalize file path to include the module path (relative path from src/ or repo root)."""
    if not path:
        return path
    path = os.path.normpath(path).replace("\\", "/")
    if path.startswith("./"):
        path = path[2:]
    if path.startswith("src/"):
        path = path[4:]
    return path


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
            file_name = normalize_file_path(file_name)
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


def sanitize_id(name):
    """Convert a label to a Mermaid-safe identifier."""
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)


def normalize_group_entry(entry):
    """Normalize a group entry from the config file."""
    label = entry["label"]
    group_id = entry.get("id", sanitize_id(label).lower())
    return {
        "id": group_id,
        "label": label,
        "files": [normalize_file_path(f) for f in entry.get("files", [])],
        "patterns": list(entry.get("patterns", [])),
    }


def normalize_group_config(config):
    """Normalize the full group config document."""
    groups = [normalize_group_entry(entry) for entry in config.get("groups", [])]
    fallback_group = None
    if config.get("fallback_group"):
        fallback_group = normalize_group_entry(config["fallback_group"])

    edge_styles = dict(DEFAULT_EDGE_STYLES)
    edge_styles.update(config.get("edge_styles", {}))

    return {
        "groups": groups,
        "fallback_group": fallback_group,
        "edge_styles": edge_styles,
    }


def default_group_config_path():
    """Return the default sibling config path for this script."""
    return os.path.join(os.path.dirname(__file__), "callgraph_file_groups.json")


def load_group_config(path=None):
    """Load a grouping config file, or fall back to the built-in prefix groups."""
    if path:
        if not os.path.exists(path):
            raise FileNotFoundError(f"group config not found: {path}")
        with open(path, "r", encoding="utf-8") as handle:
            return normalize_group_config(json.load(handle))

    candidate = default_group_config_path()
    if os.path.exists(candidate):
        with open(candidate, "r", encoding="utf-8") as handle:
            return normalize_group_config(json.load(handle))

    return normalize_group_config(DEFAULT_GROUP_CONFIG)


def group_matches_file(group, file_name):
    """Return True if a config group matches the given file."""
    if file_name in group["files"]:
        return True

    for pattern in group["patterns"]:
        if fnmatch.fnmatch(file_name, pattern) or fnmatch.fnmatch(os.path.basename(file_name), pattern):
            return True

    return False


def build_file_groups(files, group_config):
    """Resolve each file to a single group and collect ordered group membership."""
    file_to_group = {}
    grouped_files = defaultdict(list)
    present_group_ids = set()
    groups = list(group_config["groups"])
    fallback_group = group_config.get("fallback_group")

    for file_name in sorted(files):
        matches = [group for group in groups if group_matches_file(group, file_name)]
        if len(matches) > 1:
            labels = ", ".join(group["label"] for group in matches)
            raise ValueError(f"file {file_name} matches multiple subgraphs: {labels}")
        if matches:
            group = matches[0]
        elif fallback_group:
            group = fallback_group
        else:
            continue

        file_to_group[file_name] = group
        grouped_files[group["id"]].append(file_name)
        present_group_ids.add(group["id"])

    ordered_groups = [group for group in groups if group["id"] in present_group_ids]
    if fallback_group and fallback_group["id"] in present_group_ids:
        ordered_groups.append(fallback_group)

    return file_to_group, grouped_files, ordered_groups


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


def generate_mermaid(files, edges, group_config, group_by_prefix=True):
    """Generate a Mermaid flowchart from collapsed file edges."""
    lines = ["flowchart LR"]
    file_to_group = {}
    local_group_link_indexes = []
    cross_group_link_indexes = []

    if group_by_prefix:
        file_to_group, grouped_files, ordered_groups = build_file_groups(files, group_config)
        for group in ordered_groups:
            members = grouped_files[group["id"]]
            lines.append(f'    subgraph {sanitize_id(group["id"])}["{group["label"]}"]')
            for file_name in members:
                lines.append(f'        {sanitize_id(file_name)}["{file_name}"]')
            lines.append("    end")
    else:
        for file_name in sorted(files):
            lines.append(f'    {sanitize_id(file_name)}["{file_name}"]')

    edge_items = sorted(edges.items())
    for edge_index, ((caller_file, callee_file), weight) in enumerate(edge_items):
        weight = edges[(caller_file, callee_file)]
        if weight > 1:
            lines.append(
                f'    {sanitize_id(caller_file)} -->|{weight}| {sanitize_id(callee_file)}'
            )
        else:
            lines.append(f'    {sanitize_id(caller_file)} --> {sanitize_id(callee_file)}')

        if group_by_prefix:
            caller_group = file_to_group.get(caller_file)
            callee_group = file_to_group.get(callee_file)
            if caller_group and callee_group:
                if caller_group["id"] == callee_group["id"]:
                    local_group_link_indexes.append(edge_index)
                else:
                    cross_group_link_indexes.append(edge_index)

    if group_by_prefix:
        local_group_style = group_config.get("edge_styles", {}).get("local_group")
        cross_group_style = group_config.get("edge_styles", {}).get("cross_group")
        if local_group_style or cross_group_style:
            lines.append("")
        if local_group_style:
            for edge_index in local_group_link_indexes:
                lines.append(f"    linkStyle {edge_index} {local_group_style}")
        if cross_group_style:
            for edge_index in cross_group_link_indexes:
                lines.append(f"    linkStyle {edge_index} {cross_group_style}")

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
    parser.add_argument(
        "--group-config",
        help="JSON file describing file-to-subgraph mapping and cross-group edge styles",
    )
    args = parser.parse_args()

    nodes, calls, node_to_file, name_to_files = parse_cflow(sys.stdin.readlines())
    if not nodes:
        print("No functions found in cflow output", file=sys.stderr)
        sys.exit(1)

    try:
        group_config = load_group_config(args.group_config)
    except (FileNotFoundError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Error loading group config: {exc}", file=sys.stderr)
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

    try:
        print(generate_mermaid(files, edges, group_config, group_by_prefix=not args.no_groups))
    except ValueError as exc:
        print(f"Error generating Mermaid graph: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
