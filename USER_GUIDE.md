# gl-repl User Guide

This guide collects usage notes for authoring, tuning, and exporting scenes
after the first-run basics are out of the way.

## Tunable Variables

Use `// @tune` on a `float` declaration when a scene parameter should become a
keyboard-adjustable knob in the exported standalone C program.

```c
float amp = 1.5; // @tune
float freq = 2;  // @tune

glBegin(GL_TRIANGLES);
glVertex3f(amp, 0, 0);
glVertex3f(0, freq, 0);
glVertex3f(0, 0, amp);
glEnd();
```

The tag is a bare trailing comment token. It matches `// @tune`, not names like
`// @tuned=5`. If a declaration line contains multiple names, every name on
that line is tagged:

```c
float amp = 1, freq = 2; // @tune
```

Tagged variables are still normal REPL variables while you are authoring. In the
variable panel, tagged rows get an accent mark so you can see which values will
export as knobs.

### Exported Controls

When you save/export C, each tagged variable becomes a keyboard knob in the
standalone program. The generated program also draws a small HUD listing each
knob, its current value, and its keys.

Knobs are assigned in declaration order:

| Knob | Raise | Lower |
|---|---:|---:|
| 1 | `q` | `a` |
| 2 | `w` | `s` |
| 3 | `e` | `d` |
| 4 | `r` | `f` |
| 5 | `t` | `g` |
| 6 | `y` | `h` |
| 7 | `u` | `j` |
| 8 | `i` | `k` |
| 9 | `o` | `l` |

Only the first 9 tagged variables get keyboard controls. If more are tagged,
the export keeps the first 9 and writes a note in the generated C that the rest
were capped.

### Step Size

The exported knobs use the same step size as the in-app numeric swatch:

| Current value magnitude | Step |
|---:|---:|
| `< 10` | `0.05` |
| `10..99.999` | `0.5` |
| `100..999.999` | `5` |

The same modifier keys apply in the exported program:

| Modifier | Effect |
|---|---:|
| Shift | fine step, `x0.2` |
| Ctrl | coarse step, `x10` |
| Shift+Ctrl | fine then coarse, `x2` total |

There is no range clamp. A tunable can go negative or very large if you keep
pressing its keys.

### Values That Stick

A tunable only persists if your display body does not overwrite it every frame.
This works:

```c
float radius = 2; // @tune

glutSolidSphere(radius, 32, 16);
```

This appears inert in the exported program because the assignment runs again on
every display call:

```c
float radius = 2; // @tune

radius = 2;
glutSolidSphere(radius, 32, 16);
```

Use tunables for parameter-style values that the scene reads, not values the
scene recomputes unconditionally each frame.

### Export And Reload

`// @tune` survives export/import round trips. The exported C carries a marker
for each tagged declaration, and reloading that file reconstructs the original
declaration with `// @tune` so the variable-panel badge and future exports keep
working.

The generated keyboard controls live only in the standalone exported C. Inside
gl-repl, adjust values through the variable panel or inline numeric swatches.
