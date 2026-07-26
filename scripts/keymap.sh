#!/bin/bash
# keymap.sh - keyboard-binding tooling over keymap.h, the single source of
# truth for `#define GLR_<ACTION>  <key>, <mods>` bindings.
#
#   keymap.sh check   (default)  Hard guard: fail if two bindings claim the
#                                same (key, mods) — a double-map.
#   keymap.sh list               Print the current bindings, reserved
#                                control-key aliases, then the free Ctrl /
#                                Ctrl+Shift / F-key slots still available
#                                to assign.
#
# Comparing the symbolic TEXT of `<key>, <mods>` is exactly right: the same
# key with different modifiers is distinct by design (KEY_CTRL_C,0 = Copy vs
# KEY_CTRL_C,GLUT_ACTIVE_SHIFT = Reset camera), and an F-key vs a Ctrl byte
# of equal numeric value are different tokens (GLUT_KEY_F3 vs KEY_CTRL_C), so
# the ASCII and special callback classes never alias — no need to resolve
# numbers or track is_special here. Every `#define GLR_*` in keymap.h is a
# binding; the file holds no other GLR_ macros.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

mode="${1:-check}"

awk -v mode="$mode" '
    BEGIN { n = 0 }
    /^[ \t]*#define[ \t]+GLR_[A-Za-z0-9_]+[ \t]/ {
        name = $2
        rest = $0
        sub(/^[ \t]*#define[ \t]+GLR_[A-Za-z0-9_]+[ \t]+/, "", rest)  # drop "#define GLR_X "
        sub(/[ \t]*\/\*.*$/, "", rest)                                # drop trailing comment
        gsub(/[ \t]+/, "", rest)                                      # "KEY, 0" == "KEY,0"
        if (rest == "")
            next
        if (rest in owner) {
            printf("keymap.h: DOUBLE-MAP %s shared by %s and %s\n",
                   rest, owner[rest], name) > "/dev/stderr"
            bad = 1
        } else {
            owner[rest] = name
            n++
        }
    }
    END {
        if (mode == "check") {
            if (bad)
                exit 1
            if (n == 0) {
                print "keymap.h: no GLR_ bindings found (format changed?)" > "/dev/stderr"
                exit 1
            }
            printf("keymap-no-dup OK (%d bindings, no (key,mods) collision)\n", n)
            exit 0
        }
        if (mode != "list") {
            print "usage: keymap.sh [check|list]" > "/dev/stderr"
            exit 2
        }

        # ---- list mode ----
        if (bad)
            print "(note: a double-map is present — run `keymap.sh check`)"
        print "Bound (" n "):"
        k = 0
        for (b in owner) bindings[k++] = b
        for (i = 1; i < k; i++) {           # insertion sort (portable)
            x = bindings[i]; j = i - 1
            while (j >= 0 && bindings[j] > x) { bindings[j+1] = bindings[j]; j-- }
            bindings[j+1] = x
        }
        for (i = 0; i < k; i++)
            printf("  %-34s %s\n", bindings[i], owner[bindings[i]])

        reserved = "HIJM"
        reserved_reason["H"] = "byte 8 = Backspace"
        reserved_reason["I"] = "byte 9 = Tab"
        reserved_reason["J"] = "byte 10 = Line Feed / Enter variant"
        reserved_reason["M"] = "byte 13 = Carriage Return / Enter"

        print ""
        print "Reserved (not assignable):"
        for (ri = 1; ri <= length(reserved); ri++) {
            c = substr(reserved, ri, 1)
            printf("  Ctrl+%-2s / Ctrl+Shift+%-2s %s\n", c, c, reserved_reason[c])
        }

        print ""
        print "Available (unbound) slots:"
        # Ctrl+<letter> aliases above arrive as editing keys at the byte
        # level, so neither their plain nor Shift-modified forms are safe
        # app shortcuts.
        L = "  Ctrl+        :"
        for (i = 1; i <= 26; i++) {
            c = substr("ABCDEFGHIJKLMNOPQRSTUVWXYZ", i, 1)
            if (index(reserved, c)) continue
            if (("KEY_CTRL_" c ",0") in owner) continue
            L = L " " c
        }
        print L
        L = "  Ctrl+Shift+  :"
        for (i = 1; i <= 26; i++) {
            c = substr("ABCDEFGHIJKLMNOPQRSTUVWXYZ", i, 1)
            if (index(reserved, c)) continue
            if (("KEY_CTRL_" c ",GLUT_ACTIVE_SHIFT") in owner) continue
            L = L " " c
        }
        print L
        L = "  F-keys       :"
        for (i = 1; i <= 12; i++) {
            if (("GLUT_KEY_F" i ",0") in owner) continue
            L = L " F" i
        }
        print L
        exit 0
    }
' keymap.h
