#!/bin/bash
# record-gif.sh - render a gl-repl animation headlessly (OSMesa) and assemble it
# into a GIF + MP4. No display required.
#
#   scripts/record-gif.sh [options]
#
# Drives a FREEGLUT_OSMESA build of gl-repl with the backend record mode
# (FREEGLUT_CAPTURE_FRAMES): it renders with no window and writes every frame to
# a numbered PPM, exiting after exactly N frames. ffmpeg then assembles those
# PPMs into <out>.gif (palette-optimized) and <out>.mp4 (H.264).
#
# The user-facing knob is DURATION, not a frame count, so the clip length is
# invariant of --fps: N = round(duration * fps), and the output is always
# `duration` seconds long at `fps` frames/sec.
#
# Animation-speed note: GLR_TICK_PER_FRAME makes the engine advance the complete
# simulation by a fixed 1/60 s per rendered frame (see GLR_FRAME_DT_SECS), so no
# animation states are dropped when rendering is slow. Playback runs at ~fps/60
# of natural speed. Use --fps 60 for real-time; lower fps gives smooth slow-mo
# (and smaller GIFs).
#
# Options:
#   --example <name|idx>  Built-in example to record   (default: 2, animated ring)
#   --duration <secs>     Clip length in seconds        (default: 2)
#   --fps <n>             Frames/sec of the output clip (default: 50)
#   --time <secs>         Initial animation time t      (default: 0; start later)
#   --scale <width>       Scale output to this width px (default: native 1200)
#   --out <base>          Output base name              (default: out -> out.gif/.mp4)
#   --bin <path>          gl-repl OSMesa binary
#                         (default: build/release-osmesa/gl-repl)
#   --gif-only            Skip the MP4
#   --mp4-only            Skip the GIF
#   --keep                Keep the intermediate PPM frames
#   -h, --help            This help
#
# Requires: ffmpeg, and a gl-repl built with `make gl-repl FREEGLUT_OSMESA=1`.

set -euo pipefail

example="2"
duration="2"
fps="50"
t0=""
scale=""
out="out"
bin="build/release-osmesa/gl-repl"
make_gif=1
make_mp4=1
keep=0

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'; }

# A value-taking flag given as the last arg (nothing after it) would trip
# `set -u` with an opaque "$2: unbound variable"; fail with a clear message
# naming the flag instead. Called as `need_val "$@"` so $# counts what remains.
need_val() { [ "$#" -ge 2 ] || { echo "record-gif: option '$1' requires a value (try --help)" >&2; exit 2; }; }

while [ $# -gt 0 ]; do
	case "$1" in
		--example)  need_val "$@"; example="$2"; shift 2;;
		--duration) need_val "$@"; duration="$2"; shift 2;;
		--fps)      need_val "$@"; fps="$2"; shift 2;;
		--time)     need_val "$@"; t0="$2"; shift 2;;
		--scale)    need_val "$@"; scale="$2"; shift 2;;
		--out)      need_val "$@"; out="$2"; shift 2;;
		--bin)      need_val "$@"; bin="$2"; shift 2;;
		--gif-only) make_mp4=0; shift;;
		--mp4-only) make_gif=0; shift;;
		--keep)     keep=1; shift;;
		-h|--help)  usage; exit 0;;
		*) echo "record-gif: unknown option '$1' (try --help)" >&2; exit 2;;
	esac
done

# Validate user-supplied numbers up front (before the environment checks, so a
# usage typo fails the same way with or without ffmpeg installed). A typo like
# `--fps 5o` otherwise reads as 0 inside awk and silently yields a 1-frame clip;
# and since the values are interpolated into the awk program, a non-numeric one
# could inject awk code. --time is not checked here: it is passed straight to
# the binary, whose atof() tolerates it (and accepts a leading minus, which this
# check would reject).
is_num() { case "$1" in ''|*[!0-9.]*) return 1;; *) return 0;; esac; }
is_num "$duration" || { echo "record-gif: --duration must be a number (got '$duration')" >&2; exit 2; }
is_num "$fps"      || { echo "record-gif: --fps must be a number (got '$fps')" >&2; exit 2; }
[ -z "$scale" ] || is_num "$scale" || { echo "record-gif: --scale must be a number (got '$scale')" >&2; exit 2; }

# N = round(duration * fps); must be >= 1.
frames="$(awk "BEGIN{ n = $duration * $fps + 0.5; if (n < 1) n = 1; printf \"%d\", n }")"

# The backend numbers PPMs with a 4-digit field (f-%04d.ppm) and ffmpeg reads
# them back with the same %04d glob, so a capture past frame 9999 would silently
# drop its tail. Cap with a clear message (9999 frames is ~200s at 50fps).
if [ "$frames" -gt 9999 ]; then
	echo "record-gif: $frames frames exceeds the 4-digit capture limit (9999)." >&2
	echo "            Reduce --duration or --fps so duration*fps <= 9999." >&2
	exit 2
fi

command -v ffmpeg >/dev/null 2>&1 || {
	echo "record-gif: ffmpeg not found (brew install ffmpeg / apt-get install ffmpeg)" >&2
	exit 1
}
[ -x "$bin" ] || {
	echo "record-gif: gl-repl binary not found at '$bin'" >&2
	echo "            build it first: make gl-repl FREEGLUT_OSMESA=1" >&2
	exit 1
}

# Stage the intermediate PPMs next to the output, not in /tmp: a snap/flatpak
# ffmpeg runs with a private /tmp and can't read frames written to the host /tmp,
# but it can reach the output directory (typically $HOME or the cwd). The dir is
# NOT hidden (no leading dot) because snap's home interface blocks dotfiles.
out_dir="$(dirname "$out")"
[ -d "$out_dir" ] || { echo "record-gif: output dir '$out_dir' does not exist" >&2; exit 1; }
tmp="$(mktemp -d "$out_dir/record-gif-frames.XXXXXX")"
cleanup() { [ "$keep" = 1 ] || rm -rf "$tmp"; }
trap cleanup EXIT

echo "record-gif: example=$example duration=${duration}s fps=$fps frames=$frames${t0:+ t0=${t0}s}"

# Record: the backend captures every frame and exit(0)s after $frames. Branch on
# --time rather than an array so it stays portable to old bash under `set -u`.
#
# --accum overrides gl-repl's auto probe, which disables accum effects on
# renderers that emulate them in software. This script drives an OSMesa build,
# which is Mesa by construction, so without the flag a scene that asks for
# multi-pass accum in its header (e.g. glr-logo's `@cfg accum_passes = 2`)
# would record without it. Offline recording can afford the CPU accumulate.
run_fail() { echo "record-gif: gl-repl exited non-zero; log:" >&2; cat "$tmp/run.log" >&2; exit 1; }
if [ -n "$t0" ]; then
	GLR_TICK_PER_FRAME=1 \
		FREEGLUT_CAPTURE_FILE="$tmp/f" FREEGLUT_CAPTURE_FRAMES="$frames" \
		"$bin" --example "$example" --time "$t0" --no-audio --accum >"$tmp/run.log" 2>&1 || run_fail
else
	GLR_TICK_PER_FRAME=1 \
		FREEGLUT_CAPTURE_FILE="$tmp/f" FREEGLUT_CAPTURE_FRAMES="$frames" \
		"$bin" --example "$example" --no-audio --accum >"$tmp/run.log" 2>&1 || run_fail
fi

got="$(ls "$tmp"/f-*.ppm 2>/dev/null | wc -l | tr -d ' ')"
[ "$got" -gt 0 ] || { echo "record-gif: no frames captured (is this an OSMesa build?)" >&2; exit 1; }
echo "record-gif: captured $got frame(s)"

# Optional scale (force even dimensions for H.264 with -2). Build the three
# filter strings explicitly so the empty-scale case stays valid.
if [ -n "$scale" ]; then
	sc="scale=${scale}:-2:flags=lanczos"
	mp4_vf="${sc},format=yuv420p"
	pgen_vf="${sc},palettegen"
	puse_lavfi="[0:v]${sc}[x];[x][1:v]paletteuse"
else
	mp4_vf="format=yuv420p"
	pgen_vf="palettegen"
	puse_lavfi="paletteuse"
fi

if [ "$make_mp4" = 1 ]; then
	ffmpeg -y -loglevel error -framerate "$fps" -start_number 0 -i "$tmp/f-%04d.ppm" \
		-vf "$mp4_vf" -c:v libx264 -movflags +faststart "$out.mp4"
	echo "record-gif: wrote $out.mp4"
fi

if [ "$make_gif" = 1 ]; then
	# Two-pass palette for clean GIF colors.
	ffmpeg -y -loglevel error -framerate "$fps" -start_number 0 -i "$tmp/f-%04d.ppm" \
		-vf "$pgen_vf" "$tmp/palette.png"
	ffmpeg -y -loglevel error -framerate "$fps" -start_number 0 -i "$tmp/f-%04d.ppm" -i "$tmp/palette.png" \
		-lavfi "$puse_lavfi" "$out.gif"
	echo "record-gif: wrote $out.gif"
fi

[ "$keep" = 1 ] && echo "record-gif: kept frames in $tmp"
exit 0
