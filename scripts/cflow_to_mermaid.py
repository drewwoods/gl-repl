#!/usr/bin/env python3
"""
Convert cflow output to Mermaid flowchart format.

Usage:
    cflow *.c | ./scripts/cflow_to_mermaid.py > callgraph.mmd
    cflow -m imrepl_ctrl_display_frame *.c | ./scripts/cflow_to_mermaid.py > callgraph.mmd
"""

import sys
import re
from collections import defaultdict

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

        # Determine nesting level (each indent level = 4 spaces or 1 tab)
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

def sanitize_id(name):
    """Convert function name to valid Mermaid ID."""
    return re.sub(r'[^a-zA-Z0-9_]', '_', name)

def generate_mermaid(functions, calls):
    """Generate Mermaid flowchart from functions and calls."""
    lines = ["graph TD"]

    # Add node definitions with sanitized IDs
    seen = set()
    for func in sorted(functions):
        func_id = sanitize_id(func)
        if func_id not in seen:
            lines.append(f'    {func_id}["{func}"]')
            seen.add(func_id)

    # Add edges (calls)
    for caller, callee in calls:
        caller_id = sanitize_id(caller)
        callee_id = sanitize_id(callee)
        # Deduplicate edges
        edge = f'    {caller_id} --> {callee_id}'
        if edge not in lines:
            lines.append(edge)

    return '\n'.join(lines)

def main():
    lines = sys.stdin.readlines()
    functions, calls = parse_cflow(lines)

    if not functions:
        print("No functions found in cflow output", file=sys.stderr)
        sys.exit(1)

    mermaid = generate_mermaid(functions, calls)
    print(mermaid)

if __name__ == '__main__':
    main()
