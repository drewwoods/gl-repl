#!/usr/bin/env python3
"""
Convert cflow output to interactive HTML using Cytoscape.js.

This creates a self-contained HTML file with an interactive graph viewer
that can handle thousands of edges. Supports pan, zoom, search, filtering.

Usage:
    cflow *.c | ./scripts/cflow_to_cytoscape_html.py > callgraph.html

    # With filtering:
    cflow *.c | ./scripts/cflow_to_cytoscape_html.py --no-stdlib --no-gl > callgraph.html

Then open in a web browser:
    open callgraph.html
"""

import sys
import re
import json
from collections import defaultdict

STDLIB_PATTERNS = [
    r'^(printf|fprintf|sprintf|snprintf|strlen|strcpy|strcat|memcpy|memmove|malloc|free|calloc|realloc)',
    r'^(sin|cos|tan|sqrt|abs|pow|floor|ceil|fmod|fmax|fmin)',
    r'^(sin|cos|tan)f$',
    r'^gl[A-Z]',
    r'^glu[A-Z]',
    r'^glut[A-Z]',
    r'^prof_',
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
        match = re.match(r'^(\s*)(\w+)\(\)', line)
        if not match:
            continue

        indent = len(match.group(1))
        func_name = match.group(2)

        functions.add(func_name)
        level = indent // 4 if indent % 4 == 0 else (indent + 3) // 4

        if level < len(current_stack):
            current_stack = current_stack[:level]

        if current_stack:
            parent = current_stack[-1]
            calls.append((parent, func_name))

        current_stack.append(func_name)

    return functions, calls

def generate_html(functions, calls, filter_stdlib=False, filter_glnums=False):
    """Generate interactive HTML with Cytoscape.js."""
    # Filter
    if filter_stdlib or filter_glnums:
        functions = {f for f in functions if not should_filter(f, filter_stdlib, filter_glnums)}

    calls = [(c, ce) for c, ce in calls if c in functions and ce in functions]

    # Build nodes and edges
    nodes = [{'data': {'id': f, 'label': f}} for f in sorted(functions)]

    edges = []
    seen_edges = set()
    for caller, callee in calls:
        edge_id = f"{caller}->{callee}"
        if edge_id not in seen_edges:
            edges.append({'data': {'source': caller, 'target': callee, 'id': edge_id}})
            seen_edges.add(edge_id)

    data = {'nodes': nodes, 'edges': edges}

    html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Call Graph Viewer</title>
    <script src="https://cdnjs.cloudflare.com/ajax/libs/cytoscape/3.24.0/cytoscape.min.js"></script>
    <style>
        body {{
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 0;
            background: #f5f5f5;
        }}
        #cy {{
            width: 100%;
            height: 100vh;
            background: white;
        }}
        #controls {{
            position: absolute;
            top: 10px;
            left: 10px;
            background: white;
            padding: 10px;
            border-radius: 5px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
            z-index: 100;
            max-width: 300px;
        }}
        .control-group {{
            margin: 8px 0;
        }}
        input[type="text"] {{
            width: 100%;
            padding: 5px;
            border: 1px solid #ccc;
            border-radius: 3px;
            box-sizing: border-box;
        }}
        button {{
            padding: 5px 10px;
            margin: 3px;
            background: #0066cc;
            color: white;
            border: none;
            border-radius: 3px;
            cursor: pointer;
            font-size: 12px;
        }}
        button:hover {{
            background: #0052a3;
        }}
        #stats {{
            position: absolute;
            bottom: 10px;
            right: 10px;
            background: white;
            padding: 10px;
            border-radius: 5px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
            font-size: 12px;
        }}
    </style>
</head>
<body>
    <div id="cy"></div>
    <div id="controls">
        <div class="control-group">
            <input type="text" id="search" placeholder="Search functions...">
        </div>
        <div class="control-group">
            <button onclick="cy.fit()">Fit All</button>
            <button onclick="cy.elements().show(); updateStats()">Show All</button>
            <button onclick="resetHighlight()">Clear Highlight</button>
        </div>
        <div class="control-group">
            <label><input type="checkbox" id="hideIsolated"> Hide isolated nodes</label>
        </div>
        <div style="font-size: 11px; margin-top: 10px; color: #666;">
            <strong>Tips:</strong><br>
            • Click node to highlight connections<br>
            • Shift+click to deselect<br>
            • Ctrl+click to select multiple<br>
            • Type to search<br>
            • Drag to pan, scroll to zoom
        </div>
    </div>
    <div id="stats"></div>

    <script>
        const data = {json.dumps(data)};

        const cy = cytoscape({{
            container: document.getElementById('cy'),
            elements: data.nodes.concat(data.edges),
            style: [
                {{
                    selector: 'node',
                    style: {{
                        'background-color': '#0066cc',
                        'label': 'data(label)',
                        'color': '#fff',
                        'font-size': 10,
                        'padding': '2px',
                        'text-valign': 'center',
                        'text-halign': 'center',
                        'width': 'mapData(degree, 0, 100, 20, 40)',
                        'height': 'mapData(degree, 0, 100, 20, 40)',
                    }}
                }},
                {{
                    selector: 'node.highlighted',
                    style: {{
                        'background-color': '#ff6600',
                    }}
                }},
                {{
                    selector: 'node.related',
                    style: {{
                        'background-color': '#00cc00',
                    }}
                }},
                {{
                    selector: 'node.hidden',
                    style: {{
                        'display': 'none',
                    }}
                }},
                {{
                    selector: 'edge',
                    style: {{
                        'line-color': '#ccc',
                        'target-arrow-color': '#ccc',
                        'target-arrow-shape': 'triangle',
                        'width': 1,
                    }}
                }},
                {{
                    selector: 'edge.highlighted',
                    style: {{
                        'line-color': '#ff6600',
                        'target-arrow-color': '#ff6600',
                        'width': 2,
                    }}
                }},
            ],
            layout: {{
                name: 'cose',
                directed: true,
                animate: false,
                animationDuration: 0,
            }}
        }});

        let selectedNode = null;

        // Node click to highlight
        cy.on('tap', 'node', function(e) {{
            const node = e.target;
            if (selectedNode === node.id()) {{
                resetHighlight();
            }} else {{
                resetHighlight();
                selectedNode = node.id();
                highlightConnections(node);
            }}
        }});

        function highlightConnections(node) {{
            node.addClass('highlighted');

            // Incoming
            cy.elements().connectedEdges().forEach(edge => {{
                if (edge.target().id() === node.id()) {{
                    edge.addClass('highlighted');
                    edge.source().addClass('related');
                }}
                // Outgoing
                if (edge.source().id() === node.id()) {{
                    edge.addClass('highlighted');
                    edge.target().addClass('related');
                }}
            }});
        }}

        function resetHighlight() {{
            cy.elements().removeClass('highlighted').removeClass('related');
            selectedNode = null;
        }}

        // Search
        document.getElementById('search').addEventListener('input', function(e) {{
            const query = e.target.value.toLowerCase();
            cy.nodes().forEach(node => {{
                if (query === '' || node.id().toLowerCase().includes(query)) {{
                    node.removeClass('hidden');
                }} else {{
                    node.addClass('hidden');
                }}
            }});
            cy.edges().forEach(edge => {{
                if (!edge.source().hasClass('hidden') && !edge.target().hasClass('hidden')) {{
                    edge.show();
                }} else {{
                    edge.hide();
                }}
            }});
            updateStats();
        }});

        // Hide isolated
        document.getElementById('hideIsolated').addEventListener('change', function(e) {{
            cy.nodes().forEach(node => {{
                if (e.target.checked && node.degree() === 0) {{
                    node.addClass('hidden');
                }} else {{
                    node.removeClass('hidden');
                }}
            }});
            cy.edges().forEach(edge => {{
                if (!edge.source().hasClass('hidden') && !edge.target().hasClass('hidden')) {{
                    edge.show();
                }} else {{
                    edge.hide();
                }}
            }});
            updateStats();
        }});

        function updateStats() {{
            const visible_nodes = cy.nodes(':visible').length;
            const visible_edges = cy.edges(':visible').length;
            const total_nodes = cy.nodes().length;
            const total_edges = cy.edges().length;
            document.getElementById('stats').innerHTML =
                `Nodes: ${{visible_nodes}}/${{total_nodes}}<br>Edges: ${{visible_edges}}/${{total_edges}}`;
        }}

        cy.layout(cy.elements().makeLayout({{ name: 'cose', directed: true }})).run();
        setTimeout(() => cy.fit(), 100);
        updateStats();
    </script>
</body>
</html>"""

    return html

def main():
    import argparse

    parser = argparse.ArgumentParser(description='Convert cflow to interactive Cytoscape HTML')
    parser.add_argument('--no-stdlib', action='store_true', help='Filter out standard library functions')
    parser.add_argument('--no-gl', action='store_true', help='Filter out OpenGL/GLUT functions')

    args = parser.parse_args()

    lines = sys.stdin.readlines()
    functions, calls = parse_cflow(lines)

    if not functions:
        print("No functions found in cflow output", file=sys.stderr)
        sys.exit(1)

    html = generate_html(functions, calls,
                        filter_stdlib=args.no_stdlib,
                        filter_glnums=args.no_gl)
    print(html)

if __name__ == '__main__':
    main()
