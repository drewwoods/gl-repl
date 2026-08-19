# Group the `## `-documented Make targets into families, for `make help-details`.
#
# A family is the leading dash-segment of the target name (test-, bench-,
# check-, ...) once at least MIN_FAMILY targets share it; everything else
# lands in "general". Nothing here is a hand-maintained inventory: families
# appear and disappear as target names do, which is the whole point -- the
# help that replaced this was 182 lines of prose that could not be generated.
#
# ROOT_TARGETS (passed in as `roots`) are exempt from forming or joining a
# family: they are artifact names, not a verb plus a subject. Without that,
# gl-repl and gl-repl-unchained pull gl-tests into a "gl-*" family, which is
# the plain-sight version of the collision the plan complains about -- `gl-`
# meaning both "the product" and "a real GL context". They belong under
# general, where a reader looks for something to build.
#
# Universe: source declarations. The generated test-* / run-test-* aliases are
# invisible here (they exist only after $(eval)); help-details says so in its
# epilogue rather than pretending to list them.

BEGIN {
    FS = ":.*## "
    MIN_FAMILY = 3
    n_root = split(roots, r, " ")
    for (i = 1; i <= n_root; i++) root[r[i]] = 1
}

/^[a-zA-Z0-9_.-]+:.*## / {
    name = $1
    if (name ~ /^check-/) next      # check-* guards live under `make help-check`
    if (name in desc) next          # first `## ` wins; a second one is a guard failure
    desc[name] = $2
    names[++n] = name
    if (name in root) { family[name] = "general"; next }
    fam = name
    sub(/-.*$/, "", fam)
    family[name] = fam
    members[fam]++
}

function isort(a, len,   i, j, tmp) {
    for (i = 2; i <= len; i++) {
        tmp = a[i]
        for (j = i - 1; j >= 1 && a[j] > tmp; j--) a[j + 1] = a[j]
        a[j + 1] = tmp
    }
}

END {
    for (i = 1; i <= n; i++)
        if (members[family[names[i]]] < MIN_FAMILY) family[names[i]] = "general"

    nf = 0
    for (i = 1; i <= n; i++) {
        f = family[names[i]]
        if (!(f in seen)) { seen[f] = 1; fams[++nf] = f }
    }
    isort(fams, nf)
    isort(names, n)

    # "general" is what a newcomer wants first; the rest read alphabetically.
    for (pass = 0; pass <= 1; pass++) {
        for (k = 1; k <= nf; k++) {
            f = fams[k]
            if ((f == "general") != (pass == 0)) continue
            printf "\033[1m%s\033[0m\n", (f == "general" ? "general - build, run, maintain" : f "-*")
            for (i = 1; i <= n; i++)
                if (family[names[i]] == f) printf "  %-30s %s\n", names[i], desc[names[i]]
            printf "\n"
        }
    }
}
