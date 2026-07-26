#!/bin/bash
# record-video.sh - record a gl-repl session to an MP4 (H.264 + AAC music),
# optionally driven by a scripted synthetic pointer (menu navigation, clicks,
# highlights). Frames stream straight from the app into ffmpeg through a fifo
# (FREEGLUT_CAPTURE_STREAM) - encoding starts on frame 1 and no raw
# framebuffer dumps ever land on disk.
#
#   scripts/record-video.sh --script scripts/video/menu-tour.pointer \
#       --duration 30 --music assets/sample.mp3 --out menu-tour
#
# Uses the NATIVE windowed backend by default (same rationale as
# docs-assets.sh: real GPU colors/MSAA; OSMesa's software rasterizer is slow
# and mis-renders the grid). Each recording opens a real window for its
# duration - run on a machine with a display. Pass --bin to point at an
# OSMesa build for fully headless capture.
#
# The pointer script (GLR_POINTER_SCRIPT) times events on the rendered-frame
# clock: frame N <-> t = N/60 s. At the default --fps 60 the video plays in
# real time; lower fps yields smooth slow motion (the simulation still
# advances 1/60 s per frame via GLR_TICK_PER_FRAME). See
# src/app/glr_pointer_script.h for the script grammar.
#
# Options:
#   --script <file>     Pointer script driving mouse/keyboard input
#   --example <name|i>  Built-in example to start on
#   --duration <secs>   Clip length in seconds            (default: 10)
#   --fps <n>           Output frames/sec                 (default: 60)
#   --time <secs>       Initial animation time t          (default: 0)
#   --window <WxH>      Window size                       (default: 1200x800)
#   --music <file>      Audio track to mux (default: assets/sample.mp3 when
#                       present; the track fades out over the last 1.5 s)
#   --no-music          Silent video
#   --music-seek <secs> Start offset into the music track (default: 0) -
#                       scrub past an intro to the part you want under the
#                       video
#
# A pointer script can carry its own soundtrack choice in header comments,
# so a tour is self-contained (CLI flags still override):
#
#   # music: assets/Beta Waves.mp3
#   # music-seek: 24.5
#   --scale <width>     Scale output to this width (even height forced)
#   --colorspace <cs>   Color tagging: srgb (default) or p3 (Display P3;
#                       matches how the un-color-managed app looks on a
#                       wide-gamut Mac display)
#   --out <base>        Output base name                  (default: out -> out.mp4)
#   --bin <path>        gl-repl binary    (default: build/release/gl-repl)
#   --splash            Keep the startup splash banner (default: skipped)
#   -h, --help          This help
#
# Requires: ffmpeg, and a native gl-repl build (make gl-repl).

set -euo pipefail

script=""
example=""
duration="10"
fps="60"
t0=""
window="1200x800"
music=""
no_music=0
music_seek="0"
scale=""
colorspace="srgb"
out="out"
bin="build/release/gl-repl"
splash=0

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'; }

need_val() { [ "$#" -ge 2 ] || { echo "record-video: option '$1' requires a value (try --help)" >&2; exit 2; }; }

while [ $# -gt 0 ]; do
	case "$1" in
		--script)     need_val "$@"; script="$2"; shift 2;;
		--example)    need_val "$@"; example="$2"; shift 2;;
		--duration)   need_val "$@"; duration="$2"; shift 2;;
		--fps)        need_val "$@"; fps="$2"; shift 2;;
		--time)       need_val "$@"; t0="$2"; shift 2;;
		--window)     need_val "$@"; window="$2"; shift 2;;
		--music)      need_val "$@"; music="$2"; shift 2;;
		--no-music)   no_music=1; shift;;
		--music-seek) need_val "$@"; music_seek="$2"; shift 2;;
		--scale)      need_val "$@"; scale="$2"; shift 2;;
		--colorspace) need_val "$@"; colorspace="$2"; shift 2;;
		--out)        need_val "$@"; out="$2"; shift 2;;
		--bin)        need_val "$@"; bin="$2"; shift 2;;
		--splash)     splash=1; shift;;
		-h|--help)    usage; exit 0;;
		*) echo "record-video: unknown option '$1' (try --help)" >&2; exit 2;;
	esac
done

# Validate numbers before interpolating them into awk (see record-gif.sh).
is_num() { case "$1" in ''|*[!0-9.]*) return 1;; *) return 0;; esac; }
is_num "$duration"   || { echo "record-video: --duration must be a number (got '$duration')" >&2; exit 2; }
is_num "$fps"        || { echo "record-video: --fps must be a number (got '$fps')" >&2; exit 2; }
is_num "$music_seek" || { echo "record-video: --music-seek must be a number (got '$music_seek')" >&2; exit 2; }
[ -z "$scale" ] || is_num "$scale" || { echo "record-video: --scale must be a number (got '$scale')" >&2; exit 2; }
case "$window" in
	*x*) :;;
	*) echo "record-video: --window wants WxH (got '$window')" >&2; exit 2;;
esac

frames="$(awk "BEGIN{ n = $duration * $fps + 0.5; if (n < 1) n = 1; printf \"%d\", n }")"

command -v ffmpeg >/dev/null 2>&1 || {
	echo "record-video: ffmpeg not found (brew install ffmpeg / apt-get install ffmpeg)" >&2
	exit 1
}
[ -x "$bin" ] || {
	echo "record-video: gl-repl binary not found at '$bin' (build it: make gl-repl)" >&2
	exit 1
}
[ -z "$script" ] || [ -r "$script" ] || {
	echo "record-video: pointer script '$script' not readable" >&2
	exit 1
}

# Music defaults, weakest first: pointer-script header directives
# ("# music: <file>" / "# music-seek: <secs>"), then the bundled sample
# track. Explicit --music / --music-seek flags win over both.
if [ -n "$script" ]; then
	script_music="$(sed -n 's/^# music:[[:space:]]*//p' "$script" | head -1)"
	script_seek="$(sed -n 's/^# music-seek:[[:space:]]*//p' "$script" | head -1)"
	[ -n "$music" ] || music="$script_music"
	if [ "$music_seek" = "0" ] && [ -n "$script_seek" ]; then
		is_num "$script_seek" || { echo "record-video: bad '# music-seek:' in $script (got '$script_seek')" >&2; exit 2; }
		music_seek="$script_seek"
	fi
fi
if [ "$no_music" = 0 ] && [ -z "$music" ] && [ -r "assets/sample.mp3" ]; then
	music="assets/sample.mp3"
fi
[ "$no_music" = 1 ] && music=""
if [ -n "$music" ] && [ ! -r "$music" ]; then
	echo "record-video: music file '$music' not readable" >&2
	exit 1
fi

out_dir="$(dirname "$out")"
[ -d "$out_dir" ] || { echo "record-video: output dir '$out_dir' does not exist" >&2; exit 1; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/record-video.XXXXXX")"
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT

fifo="$tmp/frames.ppm"
mkfifo "$fifo"

echo "record-video: duration=${duration}s fps=$fps frames=$frames window=$window${script:+ script=$script}${music:+ music=$music}"

# Color handling. The captured PPM frames are display-referred RGB (the app
# is not color-managed), so the colorspace is declared at encode time:
#  - Convert RGB -> YCbCr with the BT.709 matrix EXPLICITLY. ffmpeg's
#    default for an RGB input is the BT.601 matrix, and players assume
#    BT.709 for HD content -- the mismatch shifts colors subtly.
#  - Tag the frames (setparams -> the mp4's colr atom; on ffmpeg >= 7 the
#    encoder takes color properties from the frames, so the old
#    -color_primaries/-color_trc encoder flags no longer stick) so players
#    interpret the values as authored: "srgb" for standard displays, or
#    "p3" = Display P3 (P3-D65 primaries + sRGB transfer) to reproduce how
#    the un-color-managed app looks on a wide-gamut Mac display.
case "$colorspace" in
	srgb) primaries="bt709";;
	p3)   primaries="smpte432";;
	*) echo "record-video: --colorspace must be 'srgb' or 'p3' (got '$colorspace')" >&2; exit 2;;
esac
csp="out_color_matrix=bt709:out_range=tv"
tag="setparams=color_primaries=${primaries}:color_trc=iec61966-2-1:colorspace=bt709:range=tv"

# Video filter chain (even dimensions for H.264).
if [ -n "$scale" ]; then
	vf="scale=${scale}:-2:flags=lanczos:${csp},format=yuv420p,${tag}"
else
	vf="scale=trunc(iw/2)*2:trunc(ih/2)*2:${csp},format=yuv420p,${tag}"
fi

# Encoder first (blocks reading the fifo until the app writes frame 1).
# Music, when present, fades out over the clip's last 1.5 s; -shortest pins
# the audio to the video length.
ffpid=""
if [ -n "$music" ]; then
	fade_start="$(awk "BEGIN{ s = $duration - 1.5; if (s < 0) s = 0; printf \"%.2f\", s }")"
	ffmpeg -y -loglevel error \
		-f image2pipe -vcodec ppm -framerate "$fps" -i "$fifo" \
		-ss "$music_seek" -i "$music" \
		-map 0:v -map 1:a \
		-vf "$vf" -c:v libx264 -crf 18 -preset medium \
		-c:a aac -b:a 192k -af "afade=t=out:st=${fade_start}:d=1.5" \
		-shortest -movflags +faststart "$out.mp4" &
	ffpid=$!
else
	ffmpeg -y -loglevel error \
		-f image2pipe -vcodec ppm -framerate "$fps" -i "$fifo" \
		-vf "$vf" -c:v libx264 -crf 18 -preset medium \
		-movflags +faststart "$out.mp4" &
	ffpid=$!
fi

# The app: stream every rendered frame into the fifo and exit after $frames.
run_fail() {
	echo "record-video: gl-repl exited non-zero; log:" >&2
	cat "$tmp/run.log" >&2
	kill "$ffpid" 2>/dev/null || true
	exit 1
}
# --accum: same reasoning as record-gif.sh. This script defaults to the native
# build (unaffected by the auto probe on a hardware renderer), but --bin can
# point it at an OSMesa build, where the probe would otherwise drop the accum
# passes a scene header asked for.
set -- --no-audio --accum --window "$window"
[ -n "$example" ] && set -- "$@" --example "$example"
[ -n "$t0" ]      && set -- "$@" --time "$t0"
env GLR_TICK_PER_FRAME=1 \
	${script:+GLR_POINTER_SCRIPT="$script"} \
	$([ "$splash" = 1 ] || echo GLR_NO_SPLASH=1) \
	FREEGLUT_CAPTURE_STREAM="$fifo" FREEGLUT_CAPTURE_FRAMES="$frames" \
	"$bin" "$@" >"$tmp/run.log" 2>&1 || run_fail

wait "$ffpid" || {
	echo "record-video: ffmpeg failed" >&2
	exit 1
}
echo "record-video: wrote $out.mp4"
