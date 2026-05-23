# Call Graph Generation Guide

This project includes tools to automatically generate visual call graphs of the program using multiple formats and methods.

## Quick Start - Choose Your Format

### 📊 **Interactive HTML** (Recommended for large graphs - NO size limits!)

```bash
# Interactive, searchable, zoomable - handles unlimited edges
make callgraph-html ENTRY=imrepl_ctrl_display_frame
open callgraph-imrepl_ctrl_display_frame.html
```

**Why choose this:**
- ✅ No size limits (Mermaid's 500-edge limit doesn't apply)
- ✅ Searchable functions, clickable connections
- ✅ Pan/zoom, hide isolated nodes
- ✅ Works with thousands of edges
- ✅ Self-contained HTML file (no server needed)

### 📈 **Graphviz DOT** (Best for rendering to high-quality images)

```bash
# Generate DOT format, render to SVG/PNG with Graphviz
make callgraph-graphviz ENTRY=imrepl_ctrl_display_frame

# Render to SVG (better for complex graphs than Mermaid)
dot -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o callgraph.svg

# Or use better layouts for large graphs:
neato -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o callgraph.svg   # spring layout
sfdp -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o callgraph.svg    # scalable force-directed
```

**Why choose this:**
- ✅ Handles large graphs better than Mermaid
- ✅ Better layout algorithms (neato, sfdp)
- ✅ Renders to any format (SVG, PNG, PDF)
- ✅ No browser text size limits

### 📋 **Mermaid** (Best for small, focused graphs)

```bash
# Generate Mermaid diagram for specific entry point
make callgraph-static-entry ENTRY=scene_render_3d_scene
```

**Why choose this:**
- ✅ Beautiful, clean diagrams
- ✅ Works great for small graphs (< 200 edges)
- ✅ Can paste to https://mermaid.live
- ⚠️ Has 500-edge limit

### 🧱 **File-Level Mermaid** (Best for module boundaries)

```bash
# Collapse function calls to file -> file edges
make callgraph-files

# Or focus on one entry path
make callgraph-files ENTRY=imrepl_ctrl_display_frame
```

**Why choose this:**
- ✅ Much smaller than the raw function graph
- ✅ Better match for `MODULES.md`-style architecture views
- ✅ Groups files using a JSON subgraph config, with a default layout that mirrors `MODULES.md`
- ✅ Edge labels show how many function-level calls collapsed into each file edge
- ✅ Cross-subgraph edges are highlighted automatically

### 📊 **Runtime Profiling** (Shows actual execution)

```bash
# Profile execution and generate call graph
make callgraph-profile PROG="./gl-repl output.c"
```

**Why choose this:**
- ✅ Shows what *actually* executes (not all possible calls)
- ✅ Includes performance data
- ✅ Useful for understanding real execution flow

## Solving the "Maximum text size" Problem

If you hit `Maximum text size in diagram exceeded`:

```bash
# Option 1: Use interactive HTML (NO limits)
make callgraph-html ENTRY=repl_eval_expr
open callgraph-repl_eval_expr.html

# Option 2: Use Graphviz (better scaling)
make callgraph-graphviz
sfdp -Tsvg callgraph-full.dot -o callgraph.svg

# Option 3: Focus on smaller entry points
make callgraph-static-entry ENTRY=flatten_range
```

## Output Formats Comparison

| Format | Max Edges | Rendering | Interaction | Best For |
|--------|-----------|-----------|-------------|----------|
| **Interactive HTML** | Unlimited | Browser | Search, click, zoom | Large graphs, exploration |
| **Graphviz (DOT)** | Unlimited | dot/neato/sfdp | None (static) | Publication, precise layout |
| **Mermaid** | ~500 | Browser/CLI | None (static) | Documentation, small graphs |
| **File-Level Mermaid** | Usually small | Browser/CLI | None (static) | Module/file interactions |
| **Profile** | Varies | Mermaid/Graphviz | None (static) | Performance analysis |

## Visualizing the Output

### Interactive HTML (Recommended)
```bash
make callgraph-html ENTRY=imrepl_ctrl_display_frame
open callgraph-imrepl_ctrl_display_frame.html
```
- Click nodes to highlight connections
- Search for functions in the input box
- Toggle "Hide isolated nodes" checkbox
- Pan with mouse drag, zoom with scroll
- No rendering needed (pure browser)

### Graphviz to SVG/PNG
```bash
# Generate DOT
make callgraph-graphviz ENTRY=imrepl_ctrl_display_frame

# Render to SVG (recommended for large graphs)
sfdp -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o graph.svg

# Render to PNG
sfdp -Tpng callgraph-imrepl_ctrl_display_frame.dot -o graph.png

# Alternative layouts
dot -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o graph.svg    # left-right layout
neato -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o graph.svg   # spring layout
```

### Mermaid Online
1. Generate: `make callgraph-static-entry ENTRY=scene_render_3d_scene`
2. Go to [https://mermaid.live](https://mermaid.live)
3. Paste contents of `.mmd` file
4. (Only works for graphs with < 500 edges)

## Understanding the Output

### Static Call Graphs (`callgraph-static*`)
- **Nodes** = Functions in the code
- **Edges** = Direct function calls
- **Nodes include**: library functions (strlen, printf, etc.), GL calls, internal functions
- **Useful for**: Understanding overall architecture, finding call chains, documentation

### Profile-Based Call Graphs (`callgraph-profile`)
- **Shows actual execution**: Only includes functions that were called during the run
- **Timing data available**: Can see hot paths (frequently called functions)
- **Smaller graphs**: Easier to understand what *actually* executes vs what *could* execute
- **Useful for**: Performance analysis, understanding real execution flow, finding bottlenecks

## Practical Examples

### Example 1: Explore Large Program Interactively
```bash
# Generate interactive HTML for the entire program (all 400+ functions)
make callgraph-html

# Open in browser - click nodes to highlight, search for functions
open callgraph-full.html
```

### Example 2: Understand a Large Entry Point
```bash
# Main display loop - huge graph with 200+ edges
make callgraph-graphviz ENTRY=imrepl_ctrl_display_frame

# Render with good layout algorithm for large graphs
sfdp -Tsvg callgraph-imrepl_ctrl_display_frame.dot -o display-frame.svg
```

### Example 3: Focus on Core Rendering
```bash
# Scene rendering pipeline
make callgraph-static-entry ENTRY=scene_render_3d_scene

# This has ~50 edges, works fine in Mermaid
cat callgraph-scene_render_3d_scene.mmd
# Copy to https://mermaid.live
```

### Example 3b: Match the Module Guide More Closely
```bash
# File-level architecture from the main frame controller
make callgraph-files ENTRY=imrepl_ctrl_display_frame

# Or the broader reachable file graph
make callgraph-files
```

### Example 3c: Try a Different Subgraph Layout
```bash
# Override the default MODULES.md-oriented grouping
make callgraph-files \
  CALLGRAPH_FILES_GROUP_CONFIG=path/to/your-groups.json
```

The default subgraph config lives at `scripts/callgraph_file_groups.json`.

### Example 4: Parser Deep-Dive
```bash
# Expression evaluator (focused, manageable size)
make callgraph-html ENTRY=repl_eval_expr
open callgraph-repl_eval_expr.html

# Or render to publication-quality SVG
make callgraph-graphviz ENTRY=repl_eval_expr
neato -Tsvg callgraph-repl_eval_expr.dot -o expr-eval.svg
```

### Example 5: Performance Analysis
```bash
# Profile actual execution with a specific test
make callgraph-profile PROG="./test_scene_render"

# Shows only what actually ran (smaller than static analysis)
```

### Example 6: Remove Noise for Documentation
```bash
# Full graph, no stdlib/GL functions
make callgraph-graphviz

# Generate SVG with filtering applied
cflow $(SRCS) | python3 scripts/cflow_to_graphviz.py --no-stdlib --no-gl > clean.dot
sfdp -Tsvg clean.dot -o callgraph-clean.svg
```

## Dependencies

### Required
- **cflow** - Parses C source code for function calls
  - Install: `brew install cflow` (macOS) or `apt-get install cflow` (Linux)
  - Needed for: `callgraph-static*`, `callgraph-graphviz`, `callgraph-html`

### Optional but Recommended
- **Graphviz** - Graph rendering engine
  - Install: `brew install graphviz` (macOS) or `apt-get install graphviz` (Linux)
  - Needed for: rendering `.dot` files to SVG/PNG
  - Tools: `dot`, `neato`, `sfdp` (each uses different layout algorithm)

### For Runtime Profiling  
- **Valgrind** with callgrind tool
  - Install: `brew install valgrind` (macOS) or `apt-get install valgrind` (Linux)
  - Needed for: `callgraph-profile`

### For Visualization
- **Browser** (for interactive HTML) - no install needed
- **Mermaid Live** - online at https://mermaid.live (for `.mmd` files)
- **mermaid-cli** (optional) - requires Node.js: `npm install -g @mermaid-js/mermaid-cli`

## Advanced: Filtering Options

All call graph generators support filtering to reduce noise:

```bash
# Remove standard library functions (strlen, printf, etc.)
# Automatically done in most targets, but can be explicit:
cflow *.c | python3 scripts/cflow_to_graphviz.py --no-stdlib

# Remove OpenGL/GLUT functions for REPL-focused graphs
cflow *.c | python3 scripts/cflow_to_graphviz.py --no-gl

# Combine both filters
cflow *.c | python3 scripts/cflow_to_graphviz.py --no-stdlib --no-gl

# Reduce transitive edges (A->B->C removes A->C if both exist)
cflow *.c | python3 scripts/cflow_to_graphviz.py --reduce-edges
```

## Scripts

Four Python scripts in `scripts/` handle the conversion:

### `cflow_to_mermaid.py`
Converts cflow's hierarchical output to Mermaid graph format.
```bash
cflow -m imrepl_ctrl_display_frame *.c | python3 scripts/cflow_to_mermaid.py > output.mmd
```

### `cflow_to_graphviz.py`
Converts to Graphviz DOT format (better for large graphs).
```bash
cflow *.c | python3 scripts/cflow_to_graphviz.py --no-stdlib > output.dot
cflow *.c | python3 scripts/cflow_to_graphviz.py --reduce-edges > output.dot
```

### `cflow_to_cytoscape_html.py`
Generates self-contained interactive HTML with Cytoscape.js (no size limits).
```bash
cflow *.c | python3 scripts/cflow_to_cytoscape_html.py --no-stdlib --no-gl > output.html
```

### `callgrind_to_mermaid.py`
Converts Valgrind callgrind profiling data to Mermaid.
```bash
callgrind_annotate callgrind.out.12345 | python3 scripts/callgrind_to_mermaid.py > output.mmd
```

## Troubleshooting

### "cflow not found"
```bash
brew install cflow        # macOS
apt-get install cflow     # Linux
```

### "graphviz not found" (for rendering DOT to SVG/PNG)
```bash
brew install graphviz        # macOS
apt-get install graphviz     # Linux
```

### "valgrind not found" (for `make callgraph-profile`)
```bash
brew install valgrind        # macOS
apt-get install valgrind     # Linux
```

### "Maximum text size in diagram exceeded" with Mermaid
**Solution:** Use interactive HTML instead - it has no size limits:
```bash
make callgraph-html ENTRY=your_function
open callgraph-*.html
```

Or use Graphviz for high-quality rendering:
```bash
make callgraph-graphviz ENTRY=your_function
sfdp -Tsvg callgraph-*.dot -o graph.svg
```

### Mermaid diagram shows all GL/stdlib functions (too noisy)
The scripts filter these out by default in most targets. If you're using the raw commands:
```bash
cflow *.c | python3 scripts/cflow_to_mermaid.py --no-stdlib --no-gl > output.mmd
```

### SVG/PNG is too large or has bad layout
Try different layout algorithms:
```bash
dot -Tsvg ...    # left-right, hierarchical
neato -Tsvg ...  # spring layout, good for general graphs
sfdp -Tsvg ...   # scalable force-directed, best for large graphs
```

### HTML file is slow to load/interact
The HTML file has all ~400+ nodes cached. If it's slow:
- Use a specific entry point instead of the full graph: `make callgraph-html ENTRY=func_name`
- Or use a filtered DOT: `cflow *.c | python3 scripts/cflow_to_graphviz.py --no-stdlib > small.dot`

### Graph visualization changes layout every time
Graphviz uses physics simulation for layout. Use specific seeds for reproducibility:
```bash
# Neato with seed for consistent layout
neato -Tsvg -Gseed=42 callgraph.dot -o graph.svg
```

## Use Cases

### Architecture Documentation
Generate call graphs of key entry points to document module interactions:
```bash
make callgraph-static-entry ENTRY=repl_parse_and_normalize  # Parser pipeline
make callgraph-static-entry ENTRY=flatten_commands          # Flatten/expand
make callgraph-static-entry ENTRY=repl_execute_program      # Execution engine
```

### Performance Analysis
Profile a specific scenario to find hot paths:
```bash
make callgraph-profile PROG="./gl-repl examples/animated_ring.c"
```

### Code Review
Understand what a change affects:
```bash
# Before refactoring scene_render_3d_scene
make callgraph-static-entry ENTRY=scene_render_3d_scene
```

### Module Testing
Trace which functions are actually exercised by tests:
```bash
make callgraph-profile PROG="./test_scene_render"
```

## Integration with MODULES.md

The `MODULES.md` file in the root contains hand-crafted architecture diagrams. These auto-generated graphs complement those by showing:
- **MODULES.md**: Ownership boundaries, module relationships, responsibilities
- **Callgraph**: Actual function-level call chains, execution order
- **File-level Mermaid**: Collapsed `file.c -> file.c` interactions grouped by configurable subgraphs, with cross-boundary edges highlighted

Together they provide both high-level structure and detailed execution flow.
