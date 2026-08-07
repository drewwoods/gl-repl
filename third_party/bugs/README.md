# Third-party bug reports

Reduced reproducers for bugs traced to something *outside* this tree — a GL
driver, a system library, a compiler. Each bug gets a `<slug>.md` write-up and,
where possible, a self-contained `<slug>.c` that depends on nothing from this
project.

Kept in-repo because the reductions are expensive to rediscover and because a
scene here may have to carry a workaround until the upstream fix reaches the
distros we build on. When a report is filed upstream, add the issue link to the
top of its `.md`.

Nothing here is compiled by the build. These are standalone; each file's header
comment carries its own build line.

| Report | Component | Affects |
|---|---|---|
| [`mesa-colormaterial-face-switch`](mesa-colormaterial-face-switch.md) | Mesa core (`iris` + `llvmpipe`) | `glColorMaterial` face switch drops the tracked color; renders the `glr-logo` example's exterior faces black |
