#!/bin/bash
# convert-video.sh - re-encode a recording into a distributable clip: GIF,
# APNG, MP4 or WebM, with explicit control over frame rate, resolution, color
# depth and dithering.
#
#   scripts/convert-video.sh --in out.mkv --format gif --scale 900 --fps 15
#
# The companion to scripts/record-video.sh, which captures. Point --in at that
# script's --lossless FFV1 master when you can: a palette derived from 4:2:0
# H.264 inherits its chroma bleed, and on this app's thin UI text and colored
# code that shows.
#
# Palette formats (gif, apng-pal8) run ffmpeg's two-pass palette: pass 1 reads
# the whole clip and picks one global palette (stats_mode=diff weights it
# toward what actually changes, which is what a mostly-static UI wants), pass 2
# maps frames onto it. Both passes must see identical fps/scale filters or the
# palette describes different pixels than the ones being mapped, so this script
# builds the chain once and uses it twice.
#
# Options:
#   --in <file>         Source video (required)
#   --format <fmt>      gif | apng | mp4 | webm            (default: gif)
#   --fps <n>           Output frame rate                  (default: source)
#   --scale <width>     Output width in px, height auto    (default: source)
#   --colors <n>        Palette size, 2..256               (default: 128)
#   --dither <mode>     none | bayer | bayer:<0-5> | floyd_steinberg |
#                       sierra2 | sierra2_4a               (default: bayer:4)
#   --truecolor         APNG only: skip quantization, 24-bit color
#   --start <secs>      Trim: skip this much of the source (default: 0)
#   --duration <secs>   Trim: encode only this many seconds
#   --loop <n>          GIF/APNG loop count, 0 = forever   (default: 0)
#   --crf <n>           MP4/WebM quality, lower is better  (default: 18)
#   --out <base>        Output base name; the extension follows --format
#   --keep-palette      Keep the generated palette PNG next to the output
#   -h, --help          This help
#
# Color depth and dithering only mean something for a palette format, so
# passing --colors/--dither/--truecolor with mp4/webm is an error rather than a
# silent no-op.
#
# Choosing between GIF and APNG: APNG's advantage is 24-bit color, and it costs
# roughly 20x the bytes on animated content (its per-frame zlib cannot exploit
# a shared palette the way GIF's LZW does, and inter-frame diffing wins nothing
# when most of the frame repaints). At matched palette settings APNG output is
# pixel-identical to the GIF and about 20% larger. Reach for --format apng
# --truecolor only for short clips where gradient banding is unacceptable.
#
# Requires: ffmpeg.

set -euo pipefail

src=""
format="gif"
fps=""
scale=""
colors="128"
dither="bayer:4"
truecolor=0
start=""
duration=""
loop="0"
crf="18"
out=""
keep_palette=0

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; /^set -euo/d'; }

need_val() { [ "$#" -ge 2 ] || { echo "convert-video: option '$1' requires a value (try --help)" >&2; exit 2; }; }

# Track whether a palette knob was given explicitly, so the mp4/webm rejection
# can fire on "you asked for this" rather than on the default value.
colors_set=0
dither_set=0

while [ $# -gt 0 ]; do
	case "$1" in
		--in)           need_val "$@"; src="$2"; shift 2;;
		--format)       need_val "$@"; format="$2"; shift 2;;
		--fps)          need_val "$@"; fps="$2"; shift 2;;
		--scale)        need_val "$@"; scale="$2"; shift 2;;
		--colors)       need_val "$@"; colors="$2"; colors_set=1; shift 2;;
		--dither)       need_val "$@"; dither="$2"; dither_set=1; shift 2;;
		--truecolor)    truecolor=1; shift;;
		--start)        need_val "$@"; start="$2"; shift 2;;
		--duration)     need_val "$@"; duration="$2"; shift 2;;
		--loop)         need_val "$@"; loop="$2"; shift 2;;
		--crf)          need_val "$@"; crf="$2"; shift 2;;
		--out)          need_val "$@"; out="$2"; shift 2;;
		--keep-palette) keep_palette=1; shift;;
		-h|--help)      usage; exit 0;;
		*) echo "convert-video: unknown option '$1' (try --help)" >&2; exit 2;;
	esac
done

# Validate numbers before they reach a filter string (same rationale as
# record-gif.sh: a typo like `--fps 5o` reads as 0 inside ffmpeg and yields a
# one-frame clip, and these values are interpolated into filter graphs).
is_num() { case "$1" in ''|*[!0-9.]*) return 1;; *) return 0;; esac; }
chk_num() { [ -z "$2" ] || is_num "$2" || { echo "convert-video: $1 must be a number (got '$2')" >&2; exit 2; }; }
chk_num --fps "$fps"
chk_num --scale "$scale"
chk_num --start "$start"
chk_num --duration "$duration"
chk_num --crf "$crf"

[ -n "$src" ] || { echo "convert-video: --in is required (try --help)" >&2; exit 2; }
[ -r "$src" ] || { echo "convert-video: source '$src' not readable" >&2; exit 1; }

command -v ffmpeg >/dev/null 2>&1 || {
	echo "convert-video: ffmpeg not found (brew install ffmpeg / apt-get install ffmpeg)" >&2
	exit 1
}

case "$format" in
	gif|apng|mp4|webm) :;;
	*) echo "convert-video: --format must be gif, apng, mp4 or webm (got '$format')" >&2; exit 2;;
esac

# A palette format is one that quantizes. APNG is only a palette format until
# --truecolor turns it into a 24-bit one.
palette=0
case "$format" in
	gif)  palette=1;;
	apng) [ "$truecolor" = 1 ] || palette=1;;
esac

if [ "$format" = mp4 ] || [ "$format" = webm ]; then
	[ "$colors_set" = 0 ] || { echo "convert-video: --colors has no meaning for $format" >&2; exit 2; }
	[ "$dither_set" = 0 ] || { echo "convert-video: --dither has no meaning for $format" >&2; exit 2; }
	[ "$truecolor" = 0 ]  || { echo "convert-video: --truecolor has no meaning for $format ($format is already truecolor)" >&2; exit 2; }
fi
if [ "$format" = gif ] && [ "$truecolor" = 1 ]; then
	echo "convert-video: --truecolor is not possible for gif (GIF is 256 colors per frame by format)" >&2
	echo "               use --colors 256, or --format apng --truecolor" >&2
	exit 2
fi

if [ "$palette" = 1 ]; then
	case "$colors" in
		''|*[!0-9]*) echo "convert-video: --colors must be a whole number 2..256 (got '$colors')" >&2; exit 2;;
	esac
	[ "$colors" -ge 2 ] && [ "$colors" -le 256 ] || {
		echo "convert-video: --colors must be 2..256 (got '$colors')" >&2; exit 2; }
fi

# paletteuse spells bayer's strength as a separate option, so accept it as
# `bayer:<scale>` and split it here. A higher scale is a coarser pattern: it
# compresses better and looks noisier.
dither_arg=""
if [ "$palette" = 1 ]; then
	case "$dither" in
		none|floyd_steinberg|sierra2|sierra2_4a|heckbert)
			dither_arg="dither=$dither";;
		bayer)
			dither_arg="dither=bayer";;
		bayer:*)
			bs="${dither#bayer:}"
			case "$bs" in
				[0-5]) :;;
				*) echo "convert-video: bayer scale must be 0..5 (got '$bs')" >&2; exit 2;;
			esac
			dither_arg="dither=bayer:bayer_scale=$bs";;
		*)
			echo "convert-video: unknown --dither '$dither' (try --help)" >&2; exit 2;;
	esac
fi

[ -n "$out" ] || out="$(basename "${src%.*}")"
out_dir="$(dirname "$out")"
[ -d "$out_dir" ] || { echo "convert-video: output dir '$out_dir' does not exist" >&2; exit 1; }

# Trim flags go before -i so ffmpeg seeks rather than decodes-and-discards.
# They live in the positional params, not an array: macOS ships bash 3.2,
# where "${arr[@]}" on an empty array trips `set -u`. "$@" is exempt from that.
set --
[ -z "$start" ]    || set -- "$@" -ss "$start"
[ -z "$duration" ] || set -- "$@" -t "$duration"

# One filter chain, shared by both palette passes. Order matters: drop frames
# first, then scale, so scaling only touches frames that survive.
chain=""
[ -z "$fps" ]   || chain="fps=$fps"
if [ -n "$scale" ]; then
	[ -z "$chain" ] || chain="$chain,"
	chain="${chain}scale=${scale}:-2:flags=lanczos"
fi
[ -n "$chain" ] || chain="null"

echo "convert-video: $src -> $format${fps:+ fps=$fps}${scale:+ scale=${scale}px}$([ "$palette" = 1 ] && echo " colors=$colors $dither" || true)${start:+ start=${start}s}${duration:+ dur=${duration}s}"

case "$format" in
	mp4)
		ffmpeg -y -loglevel error "$@" -i "$src" \
			-vf "${chain},format=yuv420p" \
			-c:v libx264 -crf "$crf" -preset medium -movflags +faststart \
			-an "$out.mp4"
		echo "convert-video: wrote $out.mp4"
		;;
	webm)
		ffmpeg -y -loglevel error "$@" -i "$src" \
			-vf "${chain},format=yuv420p" \
			-c:v libvpx-vp9 -crf "$crf" -b:v 0 -row-mt 1 \
			-an "$out.webm"
		echo "convert-video: wrote $out.webm"
		;;
	gif|apng)
		if [ "$palette" = 1 ]; then
			pal="$out.palette.png"
			ffmpeg -y -loglevel error "$@" -i "$src" \
				-vf "${chain},palettegen=max_colors=${colors}:stats_mode=diff" "$pal"
			# diff_mode=rectangle limits each written frame to the changed
			# region, which is most of the saving on a static-chrome UI clip.
			puse="paletteuse=${dither_arg}:diff_mode=rectangle"
			if [ "$format" = gif ]; then
				ffmpeg -y -loglevel error "$@" -i "$src" -i "$pal" \
					-lavfi "[0:v]${chain}[x];[x][1:v]${puse}" \
					-loop "$loop" "$out.gif"
			else
				# -f apng is required: a .png filename otherwise selects the
				# image2 muxer, which refuses a second frame.
				ffmpeg -y -loglevel error "$@" -i "$src" -i "$pal" \
					-lavfi "[0:v]${chain}[x];[x][1:v]${puse}" \
					-c:v apng -pix_fmt pal8 -pred mixed -plays "$loop" \
					-f apng "$out.apng"
			fi
			[ "$keep_palette" = 1 ] || rm -f "$pal"
			[ "$keep_palette" = 0 ] || echo "convert-video: kept palette $pal"
		else
			# Truecolor APNG. -pred mixed lets the encoder pick a PNG filter
			# per row; it is a few percent smaller than the paeth default and
			# costs only encode time.
			ffmpeg -y -loglevel error "$@" -i "$src" \
				-vf "$chain" \
				-c:v apng -pix_fmt rgb24 -pred mixed -plays "$loop" \
				-f apng "$out.apng"
		fi
		echo "convert-video: wrote $out.$format"
		;;
esac
