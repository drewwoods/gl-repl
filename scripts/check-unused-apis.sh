#!/bin/bash
# Check for potentially unused public APIs in repl_*.h headers.
# Prints function names that are declared but not called in any .c file.
cd "$(git rev-parse --show-toplevel)"

echo "Scanning for potentially unused public APIs..."

# Collect all unused functions first (avoid subshell variable scope issues)
# by processing output directly without intermediate temp file
unused_count=0
output=""

for hdr in src/repl/parser.h src/repl/executor.h src/repl/flatten.h src/repl/eval.h src/repl/export.h \
	repl_editor.h repl_replay.h repl_undo.h src/repl/command_store.h src/repl/scenes.h; do

	[ ! -f "$hdr" ] && continue

	# Extract function declarations and check each one
	grep -E '^(void|int|float|const|FlatProgramView|ReplFlattenResult|ReplPredefView)' "$hdr" 2>/dev/null | \
		grep -o '[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*(' | \
		sed 's/[[:space:]]*(//' | \
		sort -u | while read func; do
			[ -z "$func" ] && continue
			# Check if function is called anywhere in .c files
			if ! grep -q "\b$func\s*(" *.c 2>/dev/null; then
				echo "  $hdr: $func()"
			fi
		done >> /tmp/check_unused_apis_$$.txt
done

# Now check if we found any unused APIs (safer than checking tmpfile)
if [ -f "/tmp/check_unused_apis_$$.txt" ] && [ -s "/tmp/check_unused_apis_$$.txt" ]; then
	echo "⚠️  Potentially unused APIs found:"
	cat "/tmp/check_unused_apis_$$.txt"
	echo ""
	echo "   Consider moving these to src/repl/core_internal.h or making them static."
	rm -f "/tmp/check_unused_apis_$$.txt"
	exit 0  # Don't fail the build; this is informational only
else
	echo "✓ No unused public APIs detected"
	rm -f "/tmp/check_unused_apis_$$.txt" 2>/dev/null
	exit 0
fi
