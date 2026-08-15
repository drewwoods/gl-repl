#!/bin/bash
# Hard guard: the editor plugin publishes its sidecar atomically.
#
# packaging/editor/glr-wip.vim writes the unsaved buffer to <file>.wip on every
# keystroke and every cursor motion, while gl-repl reads that file from its
# frame loop. Writing the target in place - `writefile(lines, target)` - leaves
# a window between the truncate and the last byte in which the watcher reads a
# short file. What lands then is not a crash: it is a *successful* atomic
# import of a truncated document, which replaces the live scene with whatever
# prefix happened to be on disk. The only thing standing between the user and
# that is the temp-file-and-rename in GlrWipPublish().
#
# The C side cannot check this - by the time the watcher sees the file the
# rename has either happened or not - and the test suite cannot either: a test
# drives its own publisher, so it goes on passing after the plugin regresses.
# Hence a guard over the plugin text itself.
#
# The rule: every writefile() in the plugin goes to a temp, and a rename()
# moves it into place. If the publication mechanism is ever rewritten, this
# guard should be rewritten with it rather than deleted - the property is
# load-bearing, the current spelling is not.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

plugin='packaging/editor/glr-wip.vim'
[ -f "$plugin" ] || {
    echo "ERROR: $plugin is missing - guard cannot run." >&2
    exit 1
}

fail=0

# 1. The temp must be renamed onto *the target*. Requiring merely "a rename()
#    somewhere" is not the property: rename(l:tmp, l:backup) satisfies that and
#    never installs the sidecar at all.
if ! grep -Eq 'rename\([[:space:]]*l:tmp[[:space:]]*,[[:space:]]*l:target[[:space:]]*\)' "$plugin"; then
    echo "ERROR: $plugin does not rename(l:tmp, l:target) - the temp is never" >&2
    echo "       installed as the sidecar, so publication is not atomic." >&2
    fail=1
fi

# 2. No writefile() may name the target directly. Every one must write a temp,
#    which by convention here is the variable holding the `.tmp` path.
while IFS= read -r line; do
    case "$line" in
        *writefile\(*l:tmp*) ;;   # writes the temp: fine
        *)
            echo "ERROR: $plugin writes with a non-temp destination:" >&2
            echo "         $line" >&2
            echo "       Publish to a sibling temp and rename() it over the target;" >&2
            echo "       an in-place write can be read half-finished." >&2
            fail=1
            ;;
    esac
done < <(grep -n 'writefile(' "$plugin" || true)

# 3. The temp has to be a sibling of the target, or the rename crosses a
#    filesystem and stops being atomic.
if ! grep -q "l:tmp = printf('%s\." "$plugin"; then
    echo "ERROR: $plugin's temp path is not derived from the target -" >&2
    echo "       a rename across filesystems is not atomic." >&2
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "wip-plugin-atomic OK (sidecar published via sibling temp + rename)"
