#!/bin/bash
# Check for potentially unused public APIs in repl_*.h headers.
# Prints function names that are declared but not called in any .c file.

echo "Scanning for potentially unused public APIs..."

# Use a temp file to collect unused functions (workaround for subshell variable scope)
tmpfile=$(mktemp)
trap "rm -f '$tmpfile'" EXIT

for hdr in repl_parser.h repl_executor.h repl_flatten.h repl_eval.h repl_export.h \
	repl_editor.h repl_replay.h repl_undo.h repl_command_store.h repl_scenes.h; do

	[ ! -f "$hdr" ] && continue

	# Extract function declarations: find lines starting with type keywords,
	# extract function name (word followed by '(')
	grep -E '^(void|int|float|const|FlatProgramView|ReplFlattenResult|ReplPredefView)' "$hdr" 2>/dev/null | \
		grep -o '[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*(' | \
		sed 's/[[:space:]]*(//' | \
		sort -u | while read func; do

			[ -z "$func" ] && continue

			# Check if function is called anywhere in .c files
			if ! grep -q "\b$func\s*(" *.c 2>/dev/null; then
				echo "  $hdr: $func()" >> "$tmpfile"
			fi
		done
done

if [ -s "$tmpfile" ]; then
	echo "⚠️  Potentially unused APIs found:"
	cat "$tmpfile"
	echo ""
	echo "   Consider moving these to repl_core_internal.h or making them static."
	exit 0  # Don't fail the build; this is informational only
else
	echo "✓ No obviously unused public APIs detected"
	exit 0
fi
