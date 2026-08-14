# 7 · Replay & export

> **Goal:** step through how a scene is drawn, then take your work with you.
> **Time:** ~4 minutes. &nbsp; **Prereq:** [Tutorial 6](06-lighting-and-material.md).

---

## Replay — watch the scene draw itself

Replay walks the flat command stream one call at a time, so you can *see* each
GL call paint its piece. Build something with a few parts first:

```c
glTranslatef(0, 0, -7);
for(i, 0, 6) {
  glPushMatrix();
  glRotatef(i * 60, 0, 1, 0);
  glTranslatef(2, 0, 0);
  glColor3f(i / 6, 0.4, 1 - i / 6);
  glutSolidSphere(0.4, 20, 20);
  glPopMatrix();
}
```

Press **`Ctrl+R`** to start replay. Geometry up to the current step renders;
older geometry fades as new calls appear, and a HUD shows where you are in the
program. Press `Ctrl+R` again to stop.

<!-- TODO: GIF — Ctrl+R replay running over the ring-of-spheres: spheres appearing one at a time with the fade trail and the replay HUD visible. -->
<div align="center">
<img src="assets/07-replay.gif" alt="Replay stepping through the draw calls of a scene" width="80%">
<br><sub><i>Capture: build the sphere ring → <code>Ctrl+R</code> → spheres paint in one at a time with the HUD.</i></sub>
</div>

> [!TIP]
> Replay is the best way to debug an unexpected scene — if a transform is in the
> wrong place, you'll spot the exact call where things go sideways.

---

## Save & reload — `output.c`

Press **`Ctrl+S`** to save the current scene to `output.c`: a **standalone,
compilable C file**. The export embeds your variables, camera, and any `funcN`
definitions, and round-trips back in:

```bash
./sample output.c        # reload exactly what you saved
```

Because it's plain C against vanilla freeglut, you can also lift the `display()`
body straight into your own engine — independence is a first-class goal.

<!-- TODO: GIF (optional) — Ctrl+S, then a quick cut to a terminal showing `./sample output.c` reloading the same scene. Or just a still of the saved file. -->
<div align="center">
<img src="assets/07-save-reload.gif" alt="Saving to output.c and reloading it" width="80%">
<br><sub><i>Capture: <code>Ctrl+S</code> → show <code>output.c</code> → relaunch with it as an argument.</i></sub>
</div>

---

## Export geometry — `.ply` mesh

Press **`F11`** (or **File → Export .ply**) to capture the current scene as an
ASCII PLY mesh — your `glVertex` polygons, GLU-tessellated shapes, and the GLUT
solids, all through one capture pass. Authored normals are preserved; the rest
are smoothly synthesized. Drop the `.ply` into Blender, MeshLab, or any viewer.

Headless capture, for scripting:

```bash
./sample --example 8 --export-ply out.ply                 # capture frame 1, then exit
./sample --example 8 --export-ply out.ply --export-ply-srgb   # sRGB → linear colors
```

> [!NOTE]
> On macOS, `F11` may be claimed by "Show Desktop" — use **File → Export .ply**
> instead if the key does nothing.

<!-- TODO: GIF (optional) — pressing F11 and a toast/status line confirming the .ply was written; optionally a cut to the mesh opened in a viewer. -->
<div align="center">
<img src="assets/07-ply-export.gif" alt="Exporting the scene to a PLY mesh" width="80%">
<br><sub><i>Capture: <code>F11</code> → status line confirms the file; optional cut to the mesh in MeshLab/Blender.</i></sub>
</div>

---

## Where to go next

- Cycle the built-in examples with **`F12`** and read their source — they're the
  best reference for what's possible.
- Press **`F1`** for the full command + key-binding reference.
- Skim `ARCHITECTURE.md` for how the pieces fit together.

---

**↩ Back to** [the tutorial index](README.md)
