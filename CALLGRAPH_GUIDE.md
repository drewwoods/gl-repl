# Call Graph Generation Guide

This project includes tools to automatically generate visual call graphs of the program using two different methods: static analysis and runtime profiling.

## Quick Start

### Static Analysis (Recommended for quick exploration)

```bash
# Generate call graph from specific entry point
make callgraph-static-entry ENTRY=imrepl_ctrl_display_frame

# Generate call graph of entire program
make callgraph-static
```

This uses **cflow** to parse the source code and extract function call relationships without running the program.

### Runtime Profiling (Shows actual execution)

```bash
# Generate profile-based call graph from default execution
make callgraph-profile

# Generate profile with custom program/arguments
make callgraph-profile PROG="./sample output.c"
```

This uses **Valgrind's callgrind** to profile actual execution and shows which functions were *actually* called with real timing data.

## Visualizing the Output

The targets generate Mermaid diagram files (`.mmd` format) that can be visualized in several ways:

### Online Visualization
1. Go to [https://mermaid.live](https://mermaid.live)
2. Paste the contents of the generated `.mmd` file
3. The diagram renders instantly

### Command-line Visualization
Convert Mermaid to SVG or PNG:

```bash
# Install mermaid CLI (requires Node.js)
npm install -g @mermaid-js/mermaid-cli

# Convert to SVG
mmdc -i callgraph-static.mmd -o callgraph-static.svg

# Convert to PNG
mmdc -i callgraph-static.mmd -o callgraph-static.png
```

### Open in GUI (macOS)
```bash
open callgraph-static.mmd  # Opens in default text editor
# Copy content to https://mermaid.live in browser
```

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

## Examples

### Generate call graph from main display loop
```bash
make callgraph-static-entry ENTRY=imrepl_ctrl_display_frame
# Output: callgraph-imrepl_ctrl_display_frame.mmd
```

Result shows:
```
imrepl_ctrl_display_frame
├── imrepl_ctrl_build_scene_config
├── scene_render_3d_scene
│   └── render_3d_scene_pass
│       └── execute_fn (scene_execute_adapter)
│           └── repl_execute_program
├── ui_panels_render_code_panel
├── ui_autocomplete_panel_render
├── ui_menu_bar_render
└── ...
```

### Profile actual execution with loaded file
```bash
# Load a saved session and profile for 5 seconds
make callgraph-profile PROG="./sample output.c"

# View the actual call graph of what executed
```

## Dependencies

### For Static Analysis
- **cflow** - Parses C source code for function calls
  - Install: `brew install cflow` (macOS) or `apt-get install cflow` (Linux)

### For Runtime Profiling  
- **Valgrind** with callgrind tool - Runtime profiling
  - Install: `brew install valgrind` (macOS) or `apt-get install valgrind` (Linux)
- **callgrind_annotate** - Comes with valgrind, parses profiling output

### For Visualization
- **Mermaid Live** (online, no installation needed)
- **mermaid-cli** (optional, requires Node.js): `npm install -g @mermaid-js/mermaid-cli`

## Scripts

Two Python scripts in `scripts/` handle the conversion:

### `cflow_to_mermaid.py`
Converts cflow's hierarchical output to Mermaid graph format.

```bash
cflow *.c | scripts/cflow_to_mermaid.py > output.mmd
cflow -m function_name *.c | scripts/cflow_to_mermaid.py > output.mmd
```

### `callgrind_to_mermaid.py`
Converts Valgrind callgrind profiling data to Mermaid graph format.

```bash
callgrind_annotate callgrind.out.12345 | scripts/callgrind_to_mermaid.py > output.mmd
scripts/callgrind_to_mermaid.py callgrind.out.12345 > output.mmd  # Direct file parsing
```

## Troubleshooting

### "cflow not found"
```bash
brew install cflow
```

### "valgrind not found"
```bash
brew install valgrind
```

### Graph is too large/complex
For large programs, focus on specific entry points:
```bash
# Instead of all functions, trace just the scene rendering
make callgraph-static-entry ENTRY=scene_render_3d_scene
```

### Mermaid diagram won't render
- Check file is valid Mermaid syntax (open in browser at https://mermaid.live)
- Very large graphs may hit browser limitations; split into smaller graphs by entry point

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
make callgraph-profile PROG="./sample examples/animated_ring.c"
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

The `MODULES.md` file in the root contains hand-crafted architecture diagrams. These auto-generated call graphs complement those by showing:
- **MODULES.md**: Ownership boundaries, module relationships, responsibilities
- **Callgraph**: Actual function-level call chains, execution order

Together they provide both high-level structure and detailed execution flow.
