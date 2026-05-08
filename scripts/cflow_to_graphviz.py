#!/usr/bin/env python3
"""
Convert cflow output to Graphviz DOT format (more efficient for large graphs).

Usage:
    cflow *.c | ./scripts/cflow_to_graphviz.py > callgraph.dot
    cflow -m glr_ctrl_display_frame *.c | ./scripts/cflow_to_graphviz.py > callgraph.dot

    # Render to SVG/PNG:
    dot -Tsvg callgraph.dot -o callgraph.svg
    dot -Tpng callgraph.dot -o callgraph.png

    # Using neato (better for large graphs):
    neato -Tsvg callgraph.dot -o callgraph.svg

    # Using sfdp (scalable force-directed):
    sfdp -Tsvg callgraph.dot -o callgraph.svg
"""

import sys
import re
from collections import defaultdict

# Functions to filter out (standard library, GL calls, etc.)
STDLIB_PATTERNS = [
    r'^(printf|fprintf|sprintf|snprintf|strlen|strcpy|strcat|memcpy|memmove|malloc|free|calloc|realloc)',
    r'^(sin|cos|tan|sqrt|abs|pow|floor|ceil|fmod|fmax|fmin)',
    r'^(sin|cos|tan)f$',
    r'^gl[A-Z]',  # OpenGL calls
    r'^glu[A-Z]',  # GLU calls
    r'^glut[A-Z]',  # GLUT calls
    r'^prof_',  # Profiling calls
]

def should_filter(name, filter_stdlib=False, filter_glnums=False):
    """Determine if a function should be filtered from output."""
    if not filter_stdlib and not filter_glnums:
        return False

    if filter_glnums and (name.startswith('gl') or name.startswith('glu')):
        return True

    if filter_stdlib:
        for pattern in STDLIB_PATTERNS:
            if re.match(pattern, name):
                return True

    return False

def parse_cflow(lines):
    """Parse cflow output and return (functions, calls) tuples."""
    functions = set()
    calls = []
    current_stack = []

    for line in lines:
        # Count leading tabs/spaces to determine nesting level
        match = re.match(r'^(\s*)(\w+)\(\)', line)
        if not match:
            continue

        indent = len(match.group(1))
        func_name = match.group(2)

        functions.add(func_name)

        # Determine nesting level
        level = indent // 4 if indent % 4 == 0 else (indent + 3) // 4

        # Update stack to current level
        if level < len(current_stack):
            current_stack = current_stack[:level]

        # If there's a parent, record the call
        if current_stack:
            parent = current_stack[-1]
            calls.append((parent, func_name))

        # Add current function to stack
        current_stack.append(func_name)

    return functions, calls

def generate_graphviz(functions, calls, filter_stdlib=False, filter_glnums=False, reduce_edges=False):
    """Generate Graphviz DOT format from functions and calls."""
    # Filter functions
    if filter_stdlib or filter_glnums:
        functions = {f for f in functions if not should_filter(f, filter_stdlib, filter_glnums)}

    # Filter edges
    filtered_calls = [
        (c, ce) for c, ce in calls
        if c in functions and ce in functions
    ]

    if reduce_edges:
        # Remove transitive edges (if A->B->C, remove A->C)
        filtered_calls = remove_transitive_edges(filtered_calls)

    lines = [
        "digraph CallGraph {",
        '    rankdir=LR;',
        '    overlap=false;',
        '    splines=curved;',
        '    node [shape=box, fontname="Arial"];',
    ]

    # Add edges
    edges_added = set()
    for caller, callee in filtered_calls:
        edge = (caller, callee)
        if edge not in edges_added:
            # Sanitize names for DOT (replace special chars)
            caller_safe = re.sub(r'[^a-zA-Z0-9_]', '_', caller)
            callee_safe = re.sub(r'[^a-zA-Z0-9_]', '_', callee)
            lines.append(f'    {caller_safe} -> {callee_safe};')
            edges_added.add(edge)

    lines.append("}")
    return '\n'.join(lines)

def remove_transitive_edges(calls):
    """Remove transitive edges (A->B->C means we can remove A->C)."""
    # Build adjacency lists
    direct = defaultdict(set)
    for caller, callee in calls:
        direct[caller].add(callee)

    # Find transitive edges
    transitive = set()
    for caller, direct_callees in direct.items():
        for direct_callee in direct_callees:
            # Find all nodes reachable from direct_callee
            reachable = find_reachable(direct_callee, direct)
            for final_callee in reachable:
                if final_callee != direct_callee:
                    transitive.add((caller, final_callee))

    # Filter out transitive edges
    return [(c, ce) for c, ce in calls if (c, ce) not in transitive]

def find_reachable(node, adjacency, visited=None):
    """Find all nodes reachable from a given node."""
    if visited is None:
        visited = set()

    if node in visited:
        return set()

    visited.add(node)
    reachable = {node}

    for neighbor in adjacency.get(node, set()):
        reachable.update(find_reachable(neighbor, adjacency, visited))

    return reachable

def main():
    import argparse

    parser = argparse.ArgumentParser(description='Convert cflow to Graphviz DOT format')
    parser.add_argument('--no-stdlib', action='store_true', help='Filter out standard library functions')
    parser.add_argument('--no-gl', action='store_true', help='Filter out OpenGL/GLUT functions')
    parser.add_argument('--reduce-edges', action='store_true', help='Remove transitive edges (A->B->C removes A->C)')

    args = parser.parse_args()

    lines = sys.stdin.readlines()
    functions, calls = parse_cflow(lines)

    if not functions:
        print("No functions found in cflow output", file=sys.stderr)
        sys.exit(1)

    dot = generate_graphviz(functions, calls,
                           filter_stdlib=args.no_stdlib,
                           filter_glnums=args.no_gl,
                           reduce_edges=args.reduce_edges)
    print(dot)

if __name__ == '__main__':
    main()
