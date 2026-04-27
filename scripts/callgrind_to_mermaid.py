#!/usr/bin/env python3
"""
Convert callgrind output to Mermaid flowchart format.

Usage:
    callgrind_annotate callgrind.out.12345 | ./scripts/callgrind_to_mermaid.py > callgraph.mmd

Or with Valgrind:
    valgrind --tool=callgrind --callgrind-out-file=trace.out ./sample
    callgrind_annotate trace.out | ./scripts/callgrind_to_mermaid.py > callgraph.mmd
"""

import sys
import re
from collections import defaultdict

def parse_callgrind_annotate(lines):
    """
    Parse callgrind_annotate output.

    Format:
    ---profile---
    file:line function calls

    Or from callgrind file directly with --tree=caller format:

    function_name [calls]
        -> called_function [calls]
    """
    functions = set()
    calls = defaultdict(set)

    current_func = None
    in_calls_section = False

    for line in lines:
        line = line.rstrip()
        if not line or line.startswith('---'):
            continue

        # Look for function definitions (may have call count in brackets)
        func_match = re.match(r'^(\w+)\s*(?:\[\d+\])?', line)
        if func_match and not line.startswith(' '):
            current_func = func_match.group(1)
            functions.add(current_func)
            in_calls_section = False
            continue

        # Look for called functions (indented with ->)
        called_match = re.match(r'^\s*->\s*(\w+)', line)
        if called_match and current_func:
            called_func = called_match.group(1)
            functions.add(called_func)
            calls[current_func].add(called_func)
            continue

        # Also handle format: "function\n  called_function [count]"
        if line.startswith(' ') and current_func:
            indent_match = re.match(r'^\s+(\w+)', line)
            if indent_match:
                called_func = indent_match.group(1)
                if called_func not in ['file:', 'function', 'calls']:
                    functions.add(called_func)
                    calls[current_func].add(called_func)

    return functions, calls

def parse_callgrind_file(filepath):
    """
    Parse a callgrind.out file directly.

    Format (simplified):
    fn=function_name
    calls=N target_file:target_line
    ...
    """
    functions = set()
    calls = defaultdict(set)

    current_func = None

    try:
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip()

                if line.startswith('fn='):
                    current_func = line[3:].strip()
                    functions.add(current_func)

                elif line.startswith('calls=') and current_func:
                    # Format: calls=count target_file:target_line
                    match = re.match(r'calls=\d+\s+.*\s+(\w+)', line)
                    if match:
                        called_func = match.group(1)
                        functions.add(called_func)
                        calls[current_func].add(called_func)
    except IOError as e:
        print(f"Error reading {filepath}: {e}", file=sys.stderr)

    return functions, calls

def sanitize_id(name):
    """Convert function name to valid Mermaid ID."""
    return re.sub(r'[^a-zA-Z0-9_]', '_', name)

def generate_mermaid(functions, calls):
    """Generate Mermaid flowchart from functions and calls."""
    lines = ["graph TD"]

    # Add node definitions
    seen = set()
    for func in sorted(functions):
        func_id = sanitize_id(func)
        if func_id not in seen and func not in ['file', 'function', 'calls']:
            lines.append(f'    {func_id}["{func}"]')
            seen.add(func_id)

    # Add edges
    edges_seen = set()
    for caller, callees in sorted(calls.items()):
        caller_id = sanitize_id(caller)
        for callee in sorted(callees):
            if callee not in ['file', 'function', 'calls']:
                callee_id = sanitize_id(callee)
                edge = (caller_id, callee_id)
                if edge not in edges_seen:
                    lines.append(f'    {caller_id} --> {callee_id}')
                    edges_seen.add(edge)

    return '\n'.join(lines)

def main():
    if len(sys.argv) > 1:
        # Read from callgrind file
        filepath = sys.argv[1]
        functions, calls = parse_callgrind_file(filepath)
    else:
        # Read from stdin (callgrind_annotate output)
        stdin_lines = sys.stdin.readlines()
        functions, calls = parse_callgrind_annotate(stdin_lines)

    if not functions:
        print("No functions found in callgrind output", file=sys.stderr)
        sys.exit(1)

    mermaid = generate_mermaid(functions, calls)
    print(mermaid)

if __name__ == '__main__':
    main()
