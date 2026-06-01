# Headless GIF/MP4 generator (OSMesa)

**Status:** DONE on branch `osmesa-gif` (off `osmesa-support`). Verified headless
on macOS (Apple-Silicon Mesa) and gracemont (Linux). Output: **GIF + MP4** via
`scripts/record-gif.sh`.

**As-built note:** the per-frame capture trigger landed in the backend's
main-loop tick (`fgPlatformProcessSingleEvent`), *not* the swap path — the
generic `glutSwapBuffers()` short-circuits before the platform swap for a
single-buffered window, and `fghRedrawWindow()` exposes no per-display hook, so
the per-iteration tick (1:1 with the display under the software renderer) is the
only per-frame hook available without editing core freeglut. A cheap colour-
buffer content signature skips the pre-first-render frame. An earlier attempt
that edited core `fg_display.c` was reverted to keep the feature backend-only
(cleaner to upstream). The script stages frames in a non-hidden dir beside the
output (not `/tmp`) so a snap/flatpak-confined `ffmpeg` — private `/tmp`, home
interface blocks dotfiles — can read them.

## Context

The `osmesa-support` work added a headless **OSMesa** build of gl-repl (renders
with no display), a **SIGUSR1** single-frame PPM capture in the vendored
freeglut backend, and a `--time`/`GLR_TIME` knob for the initial animation
clock. Animation runs headless: `t` is a *fixed-timestep* clock advancing
exactly `1/60 s` per **rendered frame** (decoupled from wall-clock —
`GLR_FRAME_DT_SECS` in `src/app/glr_ctrl.c`), so capturing every frame and
playing back at ~50–60 fps gives smooth motion regardless of the software
renderer's speed (Mac ~2.7 fps, gracemont ~21 fps generation).

The missing piece for animations is a reliable way to grab a *sequence* of
frames. `SIGUSR1`-bursting is unusable (POSIX signals coalesce). The clean
solution: a backend "record N frames then exit" mode plus a script that runs it
and assembles the PPMs with `ffmpeg` (present on Mac and gracemont).

## Step 1 — backend record mode (freeglut fork)

Repo `~/src/freeglut-fork`, branch `osmesa-backend` (pushed to
`github.com/drewwoods/freeglut`). In **`src/osmesa/fg_display_osmesa.c`**,
`fgPlatformGlutSwapBuffers` (after the existing `glFinish()` + alongside the
`SIGUSR1` check):

- Read `FREEGLUT_CAPTURE_FRAMES` once (cached static `N`, like the existing
  capture-prefix read).
- When `N > 0`: call `fghOSMesaCaptureFrame()` **every** frame (no signal),
  increment a counter, `exit(0)` once it reaches `N`. The already-fixed atexit
  teardown makes the clean exit safe.
- Reuses `fghOSMesaCaptureFrame()` + the `FREEGLUT_CAPTURE_FILE` prefix verbatim
  — only the trigger is new. Each frame is a clean `+1/60 s` step, so output is
  deterministic in `t` (frame `i` = `t0 + i/60`), machine-independent.

Commit on the fork (`osmesa: FREEGLUT_CAPTURE_FRAMES record-N-then-exit`), push.

## Step 2 — re-vendor + the script (gl-repl)

- **Re-vendor:** `FREEGLUT_REPO=~/src/freeglut-fork scripts/vendor-freeglut.sh
  osmesa-backend` then `make freeglut-clean FREEGLUT_OSMESA=1`. Updates
  `third_party/freeglut/` + `VENDORED.txt` + the `THIRD_PARTY_LICENSES.md` pin.

- **`scripts/record-gif.sh`** (new; `#!/bin/bash`, header-comment style like
  `scripts/vendor-freeglut.sh`). User-facing knob is **duration** (clip length
  invariant of fps). Flags: `--example <name|idx>` (default 2), `--duration
  <secs>` (default 2), `--fps F` (default 50), `--time <t0>`, `--scale W`
  (optional GIF downscale; native MP4), `--out <base>` (default `out` →
  `out.gif` + `out.mp4`), `--bin <path>` (default `build/release-osmesa/gl-repl`),
  `--keep`. The script computes backend frame count `N = round(duration * fps)`
  and feeds `FREEGLUT_CAPTURE_FRAMES`; clip is always `duration` s at `F` fps.
  (Engine steps `t` by `1/60 s`/frame, so playback is ~`F/60`× natural speed —
  `--fps 60` for real-time; documented in `--help` + README.) Pipeline:
  1. `mktemp -d`; run `FREEGLUT_CAPTURE_FILE=<tmp>/f FREEGLUT_CAPTURE_FRAMES=N
     <bin> --example E [--time t0] --no-audio` (self-exits after N).
  2. **MP4**: `ffmpeg -y -framerate F -i <tmp>/f-%04d.ppm -c:v libx264
     -pix_fmt yuv420p -movflags +faststart out.mp4`.
  3. **GIF** (two-pass palette): `palettegen` → `paletteuse` (optional
     `scale=W:-1:flags=lanczos`).
  4. Clean tmp unless `--keep`; clear error if `ffmpeg`/binary missing (hint:
     `make gl-repl FREEGLUT_OSMESA=1`). Mac binary has Mesa rpaths baked — no
     `DYLD_LIBRARY_PATH` needed.

- **Docs:** `README.md` (Headless — GIF/MP4 subsection),
  `ARCHITECTURE.md` (Headless Rendering — record mode + determinism note),
  `CLAUDE.md` (`FREEGLUT_CAPTURE_FRAMES` env + `scripts/record-gif.sh`).
- Commit (`tools: headless GIF/MP4 generator (scripts/record-gif.sh)`).

## Verification

- `make gl-repl FREEGLUT_OSMESA=1` on Mac; `make test-stubs` stays green (no
  gl-repl C source change in step 2 — only vendored tree + script + docs).
- Generate on gracemont (fast) in the `gl-repl-osmesa` worktree:
  `scripts/record-gif.sh --example 2 --duration 3 --out /tmp/ring` → expect
  `/tmp/ring.gif` + `/tmp/ring.mp4`, a 3-second clip; pull back + view (ring
  rotating smoothly).
- Spot-check `--time` offset and `--duration`/`--fps` independence (clip length
  tracks `--duration` at fps 50 vs 25). Cross-check on Mac.

## Reuse (no new code where infra exists)

- `fghOSMesaCaptureFrame()` / `FREEGLUT_CAPTURE_FILE` — capture writer, as-is.
- `repl_set_time()` + `--time`/`GLR_TIME` — start-offset, already shipped.
- Fixed-timestep `t` (`GLR_FRAME_DT_SECS`, `repl_state_time_advance`) — free
  deterministic frames.
- `scripts/vendor-freeglut.sh` (`FREEGLUT_REPO=`) — re-vendor.
- `ffmpeg` — assembly (GIF palette + H.264), on Mac and gracemont.
