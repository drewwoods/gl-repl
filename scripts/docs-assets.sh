#!/usr/bin/env bash
# docs-assets.sh - regenerate the screenshots and GIFs under docs/images/
# (the media embedded in README.md, docs/SHOWCASE.md and docs/USER_GUIDE.md).
#
#   scripts/docs-assets.sh [options] [asset ...] # default: all assets
#   scripts/docs-assets.sh --gifs                # regenerate every GIF
#   scripts/docs-assets.sh --pngs                # regenerate every PNG
#   scripts/docs-assets.sh --list [category]
#
# -j / --jobs N regenerates up to N assets in parallel. Each gl-repl render
# is single-threaded, but on the native backend every capture opens a real
# window, so parallel runs pop N windows at once and fight for focus — keep
# the default of 1 unless you don't mind the screen churn. Default 1.
#
# Requires a NATIVE build (make gl-repl) plus ImageMagick (magick) and
# ffmpeg. Override the binary with BIN=<path>. Clips in APNG form additionally
# need apngasm + oxipng; a run that touches no APNG clip does not.
#
# Native vs OSMesa:
#   This script drives the NATIVE windowed backend, not the headless OSMesa
#   software rasterizer. The native build renders with the real GPU driver
#   (true colors, real MSAA, correct grid/theme rendering — OSMesa's swrast
#   mis-renders the adaptive-planes grid as a brown quad), so it is what the
#   docs should show. The trade-off is that it is NOT headless: each capture
#   opens a real window on screen for a moment. Run it on a machine with a
#   display.
#
# How it works:
#   - Every capture uses the backend's record mode (FREEGLUT_CAPTURE_FRAMES=N):
#     the app renders exactly N frames to numbered PPMs and exits. Stills keep
#     the LAST frame, so anything frame-based (grid/axes theme cross-fades,
#     animation time t) has deterministically settled — no wall-clock sleeps,
#     no SIGUSR1 races. Warm-up length is the uniform WARM knob every capture
#     helper reads — see the WARM_* block below. (profile-panels is the one
#     SIGUSR1 capture: its warm-up must run at the live 60 Hz cadence.)
#   - GIF captures set GLR_TICK_PER_FRAME=1, moving the complete fixed-dt
#     simulation tick from the wall-clock timer to frame presentation. Slow
#     rendering therefore takes longer without skipping animation states.
#   - Scene states are staged by loading generated snippet files whose
#     `/* @cfg slug = value */` headers set presentation state and whose
#     optional `// camera` block poses the camera. GLR_EDIT_LINE parks the
#     cursor for cursor-bound overlays (transform guides). Gallery scenes
#     load built-in examples by NAME (--example "..."), which is stable across
#     catalog reordering — never by numeric index.
#   - Antialiasing. The native context already has MSAA (the window requests
#     GLUT_MULTISAMPLE). On top of that, stills raise the accumulation-AA
#     sample count via GLR_ACCUM_PASSES: the scene is re-rendered with a
#     sub-pixel frustum jitter per pass and accumulated, which the 2D UI
#     renders OUTSIDE of — so the GLUT bitmap fonts in the code panel stay
#     crisp while 3D edges and 1px grid/axes hairlines smooth out at full
#     weight. We do NOT supersample by rendering into an oversized window:
#     a native window is clamped to the visible screen, so an over-size
#     request comes back the wrong size. 1x window + accum/MSAA AA instead.
#   - GIFs keep stock AA (just the native MSAA): raising the accum passes
#     would multiply the cost of every recorded frame.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/gl-repl}"
DEMO_BIN_DIR="${DEMO_BIN_DIR:-$ROOT/build/release}"
OUT="${OUT:-$ROOT/docs/images}"
SHOW="$OUT/showcase"
DEMOS="$OUT/demos"

W=1200          # target width/height of every asset's source window
H=800
GIF_FUZZ=4%     # magick -layers Optimize fuzz for GIF delta compression
LP_CROP=545x300+362+402       # label-placement: scene pane around the two quads
LP_CODE_CROP=1094x250+8+48    # label-placement: the code panel above it
CH_CODE_CROP=450x233+8+86     # cursor-highlight: code rows 49..61 (both tri() calls)
CH_SCENE_CROP=450x352+382+396 # cursor-highlight: scene pane spanning both triangles
VG_CODE_CROP=592x57+8+104     # vertex-guides: code rows 4..6 (glBegin + the vertex rows)
VG_SCENE_CROP=592x390+305+365 # vertex-guides: scene pane framing all four guide shapes
XG_CODE_CROP=596x112+8+46     # xform-guide-montage: code rows 1..6 (the whole program)
XG_SCENE_CROP=596x280+400+420 # xform-guide-montage: scene pane around origin + guide
XM_CODE_CROP=800x150+8+46     # xform-guide-mode: code rows 1..8 (the whole program)
XM_SCENE_CROP=800x240+28+368  # xform-guide-mode: scene pane spanning both anchors
AC_CROP=600x275+8+44          # autocomplete: the typed line, the popup and its hint
GLS_CROP=760x630+8+44         # gl-state-inspector: source rows, popup, and the teapot under it
SP_CODE_CROP=600x258+8+44     # single-polygon-scope: the whole two-quad program
SP_SCENE_CROP=600x270+280+450 # single-polygon-scope: scene pane around both quads
WV_CODE_CROP=600x190+8+44     # winding-view: the whole two-triangle program
WV_SCENE_CROP=600x220+290+465 # winding-view: scene pane around both triangles
GS_CODE_CROP=830x76+8+46      # glow-sprites: the four point-sprite setup rows
GS_SCENE_CROP=830x280+100+450 # glow-sprites: scene pane around the sprite cloud
VO_CODE_CROP=600x206+8+44     # vertex-overlays: the whole ten-line program
VO_SCENE_CROP=600x300+300+435 # vertex-overlays: scene pane around the annotated quad
GT_CODE_CROP=664x220+8+46     # glu-tess: code rows 17..28 (comment + both gluBegins)
GT_SCENE_CROP=664x265+268+448 # glu-tess: scene pane around the tessellated arrow
GB_CROP=600x420+320+250       # grid-brightness: the cube and the lines crossing it
LTS_CROP=1200x436+0+0         # light-theme-studio: indicators + teapot, no empty floor
LI_POS_SCROLL=95              # light-theme-inspect: display()'s light-position block
LI_COL_SCROLL=172             # light-theme-inspect: init()'s per-light color block
LI_POS_CROP=870x94+8+46       # light-theme-inspect: the comment + four GL_POSITION rows
LI_COL_CROP=870x184+8+46      # light-theme-inspect: the comment + GL_LIGHT0/1 color rows
VP_CROP=265x122+935+653       # variable-panel: the panel, header row to last slider
CP_CROP=320x152+872+616       # console-panel: the floating console panel
AP_CROP=270x175+933+602       # assign-plot: the bottom-right value panel, one series
APS_CROP=270x189+933+588      # assign-plot-series: same panel, three series + legend

# Warm-up frames. Every capture helper reads the WARM env var as its warm-up
# length in frames (~1/60 s of simulation each); set it in a ( subshell )
# around the call so it stays scoped to one capture, like the GLR_* hooks.
# Per helper:
#   still           WARM maps directly to FREEGLUT_CAPTURE_FRAMES: render
#                   WARM frames and keep the LAST, so the still IS the final
#                   warm-up frame. Default WARM_PLAIN; theme cross-fade shots
#                   set WARM_FADE.
#   one_shot_still  run WARM frames at the live interactive 60 Hz cadence,
#                   then snapshot once via SIGUSR1. Default WARM_PLAIN.
#   gif             render WARM extra leading frames and DISCARD them — the
#                   kept clip length is unchanged. Default 0; --example clips
#                   default to WARM_EXAMPLE, because loading an example eases
#                   the camera into place (with damping) over the first
#                   second or two of captured frames — a settle the docs
#                   assets shouldn't show (180 ≈ 3 s).
WARM_EXAMPLE=180
# Theme cross-fades settle in ~80 frames; 90 leaves margin. Plain shots
# settle immediately; 30 frames keeps t small but past startup.
WARM_FADE=90
WARM_PLAIN=30
WARM_SPLASH=190  # currently SPLASH_TOTAL_FRAMES = 150 + 36

# Categories use public asset names, not output filenames, so they flow
# through the same want()/parallel dispatch as explicitly named assets. Put
# every asset in exactly one category.
#
# GIF_ASSETS and PNG_ASSETS are the two formats captured from the gl-repl
# binary. DEMO_ASSETS is a third category rather than more PNGs because it is
# captured from a DIFFERENT set of binaries — the standalone demos under
# tools/ — so it needs its own build (`make render3d-demo` etc.) and must not
# be dragged in by --pngs, which only ever needs `make gl-repl`.
#
# The two export-c-* PNGs are the exception inside PNG_ASSETS: they still need
# only `make gl-repl` (--pngs stays honest), but the binary they photograph is
# compiled on the fly from what Ctrl+S writes — see "Exported-C stills" below.
GIF_ASSETS=(
    view-mode-2d clip-plane-sweep xform-guide replay animated-ring
    sc-torus-knot sc-snowfall sc-recursive-tree sc-spirograph sc-ripple-ring
    sc-bubble-sort sc-wave-surface sc-ringed-planet sc-grass sc-jellyfish
    sc-conditional-colors sc-sierpinski-carpet sc-sierpinski-sponge
    sc-whale sc-stress-test sc-lantern-festival sc-aurora-observatory
    sc-planar-shadows sc-fog-ring-tunnel sc-pulse-bars sc-stencil-mask
    sc-feature-time
)

PNG_ASSETS=(
    hero first-triangle window-tour vertex-overlays wireframe-hidden-line
    winding-view depth-view light-theme-studio light-theme-inspect
    grid-themes grid-brightness backdrops axes-compass
    labels-orrery glu-tess glow-sprites transform-stress variable-panel
    console-panel
    export-c-grass export-c-knobs
    motion-blur xform-guide-montage xform-guide-mode
    single-polygon-scope label-placement
    vertex-guides cursor-highlight clip-plane autocomplete
    color-picker numeric-stepper gl-state-inspector profile-panels
    assign-plot assign-plot-series assign-plot-log
    sc-parametric-torus sc-bezier sc-orbit-plot sc-gl-repl-logo sc-function-demo
    sc-function-polygons sc-feature-ply sc-feature-export-c
)

# Standalone-demo stills for tools/README.md. One per windowed demo; repl_demo
# has none because it is headless in every build (no window, no GL context) —
# its "screenshot" is the --trace transcript quoted in the README.
DEMO_ASSETS=(
    demo-render3d demo-repl-live demo-editor
    demo-variable-panel demo-color-picker demo-assign-plot
    demo-cpuprof demo-memprof
)

ALL_ASSETS=("${GIF_ASSETS[@]}" "${PNG_ASSETS[@]}" "${DEMO_ASSETS[@]}")

usage() {
    cat <<'EOF'
Usage: scripts/docs-assets.sh [options] [asset ...]

Regenerate documentation screenshots and animations. With no asset names or
format categories, all assets are regenerated. Named assets and categories may
be combined.

GIFs and profile-panels reflect live rendering performance. Regenerate them on
an otherwise unloaded machine for representative documentation output.

Options:
  --gifs             Regenerate all GIF assets (gl-repl).
  --pngs             Regenerate all PNG assets (gl-repl).
  --demos            Regenerate the standalone-demo stills (tools/ binaries).
  --list             List selected asset names and exit (all by default).
  --formats          List the selected CLIPS with their current format (GIF or
                     APNG), size and path, then exit. Stills are skipped: only
                     a clip has a format to report. With --to-apng/--to-gif it
                     also shows the target format and changes nothing, so it
                     doubles as a dry run of a migration.
  --to-apng          Write the selected clips as APNG instead of GIF: repoint
                     every Markdown reference at the .png and delete the
                     superseded .gif. On its own it selects every clip, not
                     every asset. Needs apngasm + oxipng.
  --to-gif           The reverse of --to-apng: back to GIF, delete the .png.
  -j, --jobs N       Regenerate up to N assets in parallel (default: 1).
  -h, --help         Show this help and exit.

Clip formats:
  A clip's format is per-clip state, read from what is on disk: <name>.png
  means APNG, <name>.gif means GIF, and a clip with neither (a newly added
  one) is written as GIF. So a plain `scripts/docs-assets.sh`, or --gifs, or
  a named clip, REPRODUCES EACH CLIP IN THE FORMAT IT IS ALREADY IN — an APNG
  clip is regenerated as an APNG — and never changes a format or edits a doc.
  Only --to-apng/--to-gif change a format. --formats reports the current one,
  and shown together with --to-apng/--to-gif it is a dry run of that
  migration.

Environment:
  BIN=<path>         gl-repl binary (default: build/release/gl-repl).
  DEMO_BIN_DIR=<dir> directory holding the demo binaries (default:
                     build/release). Only the demo-* assets read it.
  OUT=<path>         output directory (default: docs/images).
  WARM=<frames>      warm-up frame override for captures that don't set their
                     own (see the WARM_* block in the script).

The demo-* assets need their own binaries:
  make render3d-demo repl-live-demo editor-demo variable-panel-demo \
       color-picker-demo cpuprof-demo memprof-demo

Examples:
  scripts/docs-assets.sh --gifs
  scripts/docs-assets.sh --pngs -j 4
  scripts/docs-assets.sh --demos
  scripts/docs-assets.sh --list --gifs
  scripts/docs-assets.sh hero replay demo-render3d
  scripts/docs-assets.sh --formats               # which clips are which format
  scripts/docs-assets.sh --to-apng view-mode-2d  # migrate one clip to APNG
  scripts/docs-assets.sh --to-apng --gifs        # migrate every clip
EOF
}

contains_asset() {
    local needle=$1 a
    shift
    for a in "$@"; do [[ "$a" == "$needle" ]] && return 0; done
    return 1
}

JOBS=1
ARGS=()
SELECT_GIFS=0
SELECT_PNGS=0
SELECT_DEMOS=0
LIST=0
FORMATS=0
# Empty = each clip keeps whatever format it is already in; --to-apng /
# --to-gif force it and migrate. Deliberately NOT spelled --apng/--gif: --gif
# would sit one letter from the --gifs CATEGORY selector and mean something
# entirely different. The 'to-' prefix reads as the conversion it performs.
CLIP_FORMAT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --list) LIST=1; shift ;;
        --formats) FORMATS=1; shift ;;
        --gifs) SELECT_GIFS=1; shift ;;
        --pngs) SELECT_PNGS=1; shift ;;
        --demos) SELECT_DEMOS=1; shift ;;
        --to-apng) CLIP_FORMAT=apng; shift ;;
        --to-gif)  CLIP_FORMAT=gif;  shift ;;
        -j|--jobs)
            [[ $# -ge 2 ]] || {
                echo "docs-assets: option '$1' requires a count (try --help)" >&2
                exit 2
            }
            JOBS=$2; shift 2
            ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --) shift; while [[ $# -gt 0 ]]; do ARGS+=("$1"); shift; done ;;
        -*) echo "docs-assets: unknown option '$1' (try --help)" >&2; exit 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

case "$JOBS" in
    ''|*[!0-9]*)
        echo "docs-assets: --jobs must be a positive integer (got '$JOBS')" >&2
        exit 2
        ;;
esac
[[ "$JOBS" -gt 0 ]] || {
    echo "docs-assets: --jobs must be a positive integer (got '$JOBS')" >&2
    exit 2
}

if [[ ${#ARGS[@]} -gt 0 ]]; then
    for a in "${ARGS[@]}"; do
        contains_asset "$a" "${ALL_ASSETS[@]}" || {
            echo "docs-assets: unknown asset '$a' (try --list)" >&2
            exit 2
        }
    done
fi

WANTED=()
if [[ "$SELECT_GIFS" -eq 1 ]]; then
    WANTED+=("${GIF_ASSETS[@]}")
fi
if [[ "$SELECT_PNGS" -eq 1 ]]; then
    WANTED+=("${PNG_ASSETS[@]}")
fi
if [[ "$SELECT_DEMOS" -eq 1 ]]; then
    WANTED+=("${DEMO_ASSETS[@]}")
fi
if [[ ${#ARGS[@]} -gt 0 ]]; then
    WANTED+=("${ARGS[@]}")
fi
if [[ ${#WANTED[@]} -eq 0 ]]; then
    # A bare --to-apng / --to-gif means "migrate the clips", not "regenerate
    # every asset in the tree and migrate the clips among them". Only clips
    # have a format to convert, so selecting the stills and demos too would
    # re-render an hour of assets that the flag cannot affect.
    if [[ -n "$CLIP_FORMAT" ]]; then
        WANTED=("${GIF_ASSETS[@]}")
    else
        WANTED=("${ALL_ASSETS[@]}")
    fi
else
    # Avoid rendering an explicitly named asset twice when its category was
    # also selected (especially important for the xargs parallel path).
    UNIQUE=()
    for a in "${WANTED[@]}"; do
        if [[ ${#UNIQUE[@]} -eq 0 ]] || ! contains_asset "$a" "${UNIQUE[@]}"; then
            UNIQUE+=("$a")
        fi
    done
    WANTED=("${UNIQUE[@]}")
fi

if [[ "$LIST" -eq 1 ]]; then
    printf '%s\n' "${WANTED[@]}" | sort
    exit 0
fi

# clip_base <asset> — the extension-less output path of a clip asset, read out
# of that asset's own `clip` call site.
#
# Deliberately NOT a second table, and not derived from the name: the mapping
# is conventional (sc-torus-knot -> $SHOW/torus-knot) but nothing enforces the
# convention, so a table or a rule would be free to drift from the call site
# it claims to describe and would then misreport a clip's format in --formats.
# Reading the call site cannot be wrong. Prints nothing if the asset has none.
clip_base() {
    local body base
    body=$(sed -n "/^if want $1; then\$/,/^fi\$/p" "$0")
    base=$(printf '%s\n' "$body" | sed -nE 's/^[[:space:]]*clip "\$(OUT|SHOW)\/([a-z0-9-]+)".*/\1 \2/p' | head -1)
    [[ -n "$base" ]] || return 0
    case "${base%% *}" in
        OUT)  printf '%s/%s\n' "$OUT"  "${base##* }" ;;
        SHOW) printf '%s/%s\n' "$SHOW" "${base##* }" ;;
    esac
}

if [[ "$FORMATS" -eq 1 ]]; then
    # Target differs from current only under --to-apng/--to-gif, which makes
    # this a dry run of that migration: the column says what a real run would
    # write, and nothing is rendered or retargeted here.
    n_gif=0; n_apng=0; kb_gif=0; kb_apng=0; n_change=0
    printf '%-7s %-7s %8s  %-24s %s\n' CURRENT TARGET SIZE ASSET PATH
    while read -r a; do
        base="$(clip_base "$a")"
        [[ -n "$base" ]] || continue   # not a clip: stills have no format
        if [[ -f "$base.png" ]]; then
            cur=APNG; kb=$(( $(wc -c <"$base.png") / 1024 ))
            n_apng=$((n_apng + 1)); kb_apng=$((kb_apng + kb)); path="${base#$ROOT/}.png"
        elif [[ -f "$base.gif" ]]; then
            cur=GIF; kb=$(( $(wc -c <"$base.gif") / 1024 ))
            n_gif=$((n_gif + 1)); kb_gif=$((kb_gif + kb)); path="${base#$ROOT/}.gif"
        else
            cur='-'; kb=0; path="${base#$ROOT/}.{gif,png} (never generated)"
        fi
        # No migration flag: the target IS the current format — that is the
        # whole promise of a plain regeneration.
        if [[ -n "$CLIP_FORMAT" ]]; then
            tgt=$(printf '%s' "$CLIP_FORMAT" | tr '[:lower:]' '[:upper:]')
        elif [[ "$cur" == '-' ]]; then
            tgt=GIF
        else
            tgt=$cur
        fi
        [[ "$tgt" == "$cur" ]] || n_change=$((n_change + 1))
        printf '%-7s %-7s %5s KB  %-24s %s\n' "$cur" "$tgt" "$kb" "$a" "$path"
    done < <(printf '%s\n' "${WANTED[@]}" | sort)
    printf '\n%d clip(s): %d GIF (%d KB), %d APNG (%d KB)\n' \
        "$((n_gif + n_apng))" "$n_gif" "$kb_gif" "$n_apng" "$kb_apng"
    [[ "$n_change" -eq 0 ]] || printf '%d would change format on `--to-%s` (nothing written: --formats is read-only)\n' \
        "$n_change" "$CLIP_FORMAT"
    exit 0
fi

# Naming a still alongside --to-apng/--to-gif is legal but does nothing to it:
# it has no animation and so no format to migrate. Say so once rather than
# leaving the user to wonder why the extension never changed.
if [[ -n "$CLIP_FORMAT" ]]; then
    n_still=0
    for a in "${WANTED[@]}"; do
        contains_asset "$a" "${GIF_ASSETS[@]}" || n_still=$((n_still + 1))
    done
    [[ "$n_still" -eq 0 ]] || echo "docs-assets: note --to-$CLIP_FORMAT converts clips only;" \
        "$n_still selected asset(s) are stills and will just be re-rendered" >&2
fi

for tool in magick ffmpeg; do
    command -v "$tool" >/dev/null || {
        echo "docs-assets: '$tool' not found" >&2; exit 1; }
done
# gl-repl is required only if something outside the demo category was asked
# for: `--demos` on a tree with no gl-repl build is a legitimate run.
NEED_GL_REPL=0
for a in "${WANTED[@]}"; do
    contains_asset "$a" "${DEMO_ASSETS[@]}" || { NEED_GL_REPL=1; break; }
done
if [[ "$NEED_GL_REPL" -eq 1 ]]; then
    [[ -x "$BIN" ]] || {
        echo "docs-assets: gl-repl binary not found at '$BIN'" >&2
        echo "             build it first: make gl-repl" >&2
        exit 1; }
fi

# A format migration rewrites shared Markdown files, so it cannot run
# concurrently: two children retargeting different clips would race on the
# same doc. Captures still cost the same; only the fan-out is given up.
if [[ -n "$CLIP_FORMAT" && "$JOBS" -gt 1 ]]; then
    echo "docs-assets: --to-$CLIP_FORMAT rewrites shared docs; forcing -j 1" >&2
    JOBS=1
fi

# Parallel mode: re-exec ourselves one asset per process via xargs -P.
# (Portable to the macOS /bin/bash 3.2 era — no `wait -n`.) Each child
# gets its own work dir, so captures can't collide. Note: native captures
# each open a window, so -j N pops N at once.
if [[ "$JOBS" -gt 1 && ${#WANTED[@]} -gt 1 ]]; then
    printf '%s\n' "${WANTED[@]}" | xargs -n1 -P "$JOBS" "$0"
    echo "docs-assets: done ($JOBS jobs)."
    exit 0
fi

want() {
    local a
    for a in "${WANTED[@]}"; do [[ "$a" == "$1" ]] && return 0; done
    return 1
}

WORK="$(mktemp -d /tmp/glr-docs-assets.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$SHOW" "$DEMOS"

# render <frames> <aa> <args...> — run one capture into $FRDIR (fresh per
# call, so renders never collide), always at the 1x WxH window. aa raises the
# accumulation sample count via GLR_ACCUM_PASSES (0 = stock AA / native MSAA
# only). Extra env (GLR_EDIT_LINE etc.) is passed by exporting before the call.
render() {
    local frames=$1 aa=$2; shift 2
    FRDIR="$WORK/frames-$RANDOM$RANDOM"
    mkdir -p "$FRDIR"
    local aa_env=() aa_flag=()
    # --accum overrides the auto probe, which disables accum on renderers
    # that emulate it in software: an OSMesa capture build is Mesa by
    # construction, and a still is worth the CPU accumulate.
    [[ "$aa" != 0 ]] && { aa_env=("GLR_ACCUM_PASSES=$aa"); aa_flag=("--accum"); }
    # ${arr[@]:+...} keeps an empty array safe under `set -u` (bash 3.2).
    env ${aa_env[@]:+"${aa_env[@]}"} \
        FREEGLUT_CAPTURE_FRAMES=$frames FREEGLUT_CAPTURE_FILE="$FRDIR/f" \
        "$BIN" "$@" --window ${W}x${H} --no-audio \
        ${aa_flag[@]:+"${aa_flag[@]}"} \
        >/dev/null 2>&1
}

# write_png <in> <out> [pre-args...] — final PNG writer: apply any pre-args
# (crop/resize) and write TRUECOLOR at max lossless compression. Do NOT
# palette-quantize (png8/-colors): the smooth 3D gradients (backdrops,
# glows, accum-AA edges) band and speckle visibly under a 256-color
# dither — tried and reverted.
write_png() {
    local in=$1 out=$2; shift 2
    magick "$in" "$@" -define png:compression-level=9 \
        -define png:exclude-chunk=time "$out"
}

# still <out.png> <aa> <args...> — keep the LAST frame. WARM maps directly
# to FREEGLUT_CAPTURE_FRAMES: the still is the final warm-up frame (default
# WARM_PLAIN).
still() {
    local out=$1 aa=$2; shift 2
    render "${WARM:-$WARM_PLAIN}" "$aa" "$@"
    local last
    last="$(ls "$FRDIR"/f-*.ppm | tail -1)"
    write_png "$last" "$out"
    rm -rf "$FRDIR"
    echo "docs-assets: wrote $out"
}

# one_shot_still <out.png> <args...> — run at the normal interactive 60 Hz
# cadence for WARM frames (default WARM_PLAIN), then ask freeglut for one
# SIGUSR1 snapshot. This is for screenshots whose contents measure wall-clock
# frame cadence: record mode would read back and write every warm-up frame,
# polluting the measurement.
one_shot_still() {
    local out=$1; shift
    local warmup_frames=${WARM:-$WARM_PLAIN}
    local warmup_seconds
    warmup_seconds="$(awk -v frames="$warmup_frames" \
        'BEGIN { printf "%.6f", frames / 60.0 }')"
    local prefix="$WORK/one-shot" ppm="$WORK/one-shot-0000.ppm"
    local log="$WORK/one-shot.log"

    (
        local pid i
        trap '[[ -z "${pid:-}" ]] || kill "$pid" 2>/dev/null || true' EXIT
        FREEGLUT_CAPTURE_FILE="$prefix" \
            "$BIN" "$@" --window ${W}x${H} --no-audio \
            >"$log" 2>&1 &
        pid=$!

        sleep "$warmup_seconds"
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "docs-assets: gl-repl exited before one-shot capture" >&2
            cat "$log" >&2
            exit 1
        fi
        kill -USR1 "$pid"

        i=0
        while [[ ! -f "$ppm" && $i -lt 100 ]]; do
            sleep 0.05
            i=$((i + 1))
        done
        if [[ ! -f "$ppm" ]]; then
            echo "docs-assets: timed out waiting for one-shot capture" >&2
            cat "$log" >&2
            exit 1
        fi

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        pid=
    )

    write_png "$ppm" "$out"
    echo "docs-assets: wrote $out"
}

# --- Standalone-demo stills ------------------------------------------------
#
# The demos are separate binaries, so they do NOT go through render(): they
# take none of gl-repl's flags (no --window, no --no-audio, no --accum) and
# each opens at its own compiled-in window size. What they DO share is the
# backend, so the same FREEGLUT_CAPTURE_* record mode drives them.
#
# Frame budget is not a free knob here. render()-style record mode renders
# exactly N frames and exits, which needs the demo to keep asking for frames:
#   - render3d / cpuprof / memprof / variable_panel register glutIdleFunc and
#     repl_live_demo a glutTimerFunc, so any N works;
#   - editor_demo and color_picker_demo are event-driven (they redisplay only
#     on input), so ONLY N=1 terminates. N>1 hangs forever waiting for a
#     frame nobody will request. Their state is static anyway — one frame is
#     the whole picture.
#
# Startup staging uses each demo's own GLR_DEMO_* capture hook (the demo-side
# equivalents of the app's GLR_OPEN_COLOR_PICKER / GLR_TYPE_KEYS), so a cold
# frame grab shows the feature rather than an empty window.

# demo_bin <demo> — resolve a demo binary path, or fail with the build line.
demo_bin() {
    local p="$DEMO_BIN_DIR/$1"
    [[ -x "$p" ]] || {
        echo "docs-assets: demo binary not found at '$p'" >&2
        echo "             build it first: make $1" >&2
        return 1; }
    echo "$p"
}

# demo_still <out.png> <demo> <frames> [args...] — record <frames> and keep
# the LAST. Use frames=1 for the event-driven demos (see above).
demo_still() {
    local out=$1 demo=$2 frames=$3; shift 3
    local bin; bin="$(demo_bin "$demo")" || return 1
    FRDIR="$WORK/frames-$RANDOM$RANDOM"
    mkdir -p "$FRDIR"
    FREEGLUT_CAPTURE_FRAMES=$frames FREEGLUT_CAPTURE_FILE="$FRDIR/f" \
        "$bin" "$@" >/dev/null 2>&1
    local last
    last="$(ls "$FRDIR"/f-*.ppm | tail -1)"
    write_png "$last" "$out"
    rm -rf "$FRDIR"
    echo "docs-assets: wrote $out"
}

# demo_one_shot_still <out.png> <demo> <warm-seconds> [args...] — run at the
# live interactive cadence for <warm-seconds> of WALL CLOCK, then take one
# SIGUSR1 snapshot. Record mode can't serve a demo whose content is a function
# of real elapsed time: it renders as fast as it can, so N frames pass in
# almost no wall time. memprof is exactly that case — its ring samples every
# MEMPROF_PUSH_INTERVAL_S (~5 s) of monotonic time, so a record-mode grab
# always reads "(collecting samples)" no matter how many frames it renders.
demo_one_shot_still() {
    local out=$1 demo=$2 warm=$3; shift 3
    local bin; bin="$(demo_bin "$demo")" || return 1
    local prefix="$WORK/demo-one-shot" ppm="$WORK/demo-one-shot-0000.ppm"
    local log="$WORK/demo-one-shot.log"
    rm -f "$ppm"

    (
        local pid i
        trap '[[ -z "${pid:-}" ]] || kill "$pid" 2>/dev/null || true' EXIT
        FREEGLUT_CAPTURE_FILE="$prefix" "$bin" "$@" >"$log" 2>&1 &
        pid=$!

        sleep "$warm"
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "docs-assets: $demo exited before one-shot capture" >&2
            cat "$log" >&2
            exit 1
        fi
        kill -USR1 "$pid"

        i=0
        while [[ ! -f "$ppm" && $i -lt 100 ]]; do
            sleep 0.05
            i=$((i + 1))
        done
        [[ -f "$ppm" ]] || {
            echo "docs-assets: timed out waiting for $demo one-shot" >&2
            cat "$log" >&2
            exit 1; }

        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        pid=
    )

    write_png "$ppm" "$out"
    echo "docs-assets: wrote $out"
}

# clip_frames <frames> <step> <args...> — record a clip and subsample every
# <step>th frame into $WORK/sub as g-%04d.ppm. Shared by gif() and apng().
#
# --example clips additionally render WARM_EXAMPLE extra leading frames and discard
# them (the camera-ease warmup, see WARM_EXAMPLE), so the clip starts settled while
# keeping the exact same frames/step at the same fps — identical length, just
# past the settle. Staged scenes have no load ease, and replay's draw-by-draw
# assembly IS the content, so a clip with no --example keeps frame 0. An
# explicit WARM at the call site overrides both defaults.
clip_frames() {
    local frames=$1 step=$2; shift 2
    local skip=${WARM:-0} a
    for a in "$@"; do [[ "$a" == "--example" ]] && { skip=${WARM:-$WARM_EXAMPLE}; break; }; done
    # Offline animation clock: one fixed-dt simulation step per captured
    # frame, independent of native render/readback throughput.
    GLR_TICK_PER_FRAME=1 render "$((skip + frames))" 0 "$@"
    rm -rf "$WORK/sub"; mkdir -p "$WORK/sub"
    local n=0 k=0 f
    for f in "$FRDIR"/f-*.ppm; do
        if (( n >= skip && (n - skip) % step == 0 )); then
            cp "$f" "$WORK/sub/g-$(printf %04d $k).ppm"; k=$((k + 1))
        fi
        n=$((n + 1))
    done
    rm -rf "$FRDIR"  # subsampled into sub/; drop the full capture (still parity)
}

# --- clip quantization -------------------------------------------------------
#
# Both writers quantize to ONE palette for the whole clip, so a GIF and an
# APNG of the same capture differ only in container. The two formats do NOT
# share settings, because they fail differently:
#
#   GIF  — 128 colors, no dither. Dither was measured worse on both axes: it
#          laid a visible crosshatch over the backdrop gradients and the dark
#          UI bars (mean SSIM 0.95 against the un-encoded frames with none,
#          0.79 with bayer) and cost bytes doing it.
#   APNG — 192 colors, atkinson dither, palette weighted toward the moving
#          part of the frame. It can afford the extra colors because its
#          inter-frame deltas are exact, and the dither pays here where it
#          does not in GIF: there is no lossy delta step downstream to turn
#          the dither pattern into permanent speckle.
#
# Numbers behind these: docs/plans/done/ has none — the measurements are in
# the git history of this file's review. Re-run them with the standalone
# lab harness rather than editing values here on a hunch.
GIF_QUANT='split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=none'
APNG_QUANT='split[s0][s1];[s0]palettegen=max_colors=192:stats_mode=diff[p];[s1][p]paletteuse=dither=atkinson:diff_mode=rectangle'

# clip <out-without-extension> <frames> <step> <fps> <width> <args...> —
# record and write the clip in whichever format it is currently in. Call
# sites pass NO extension: the format is not a property of the call site, it
# is per-clip state that --to-apng / --to-gif migrate (see clip_format).
clip() {
    local base=$1; shift
    case "$(clip_format "$base")" in
        apng) write_apng "$base.png" "$@"; clip_retarget "$base" gif png ;;
        *)    write_gif  "$base.gif" "$@"; clip_retarget "$base" png gif ;;
    esac
}

# clip_format <base> — gif | apng. --to-apng/--to-gif force it (and migrate);
# otherwise it is whichever file is already on disk, defaulting to GIF so a
# newly added clip needs no ceremony.
clip_format() {
    [[ "$CLIP_FORMAT" == "apng" ]] && { echo apng; return; }
    [[ "$CLIP_FORMAT" == "gif"  ]] && { echo gif;  return; }
    [[ -f "$1.png" ]] && { echo apng; return; }
    echo gif
}

# clip_retarget <base> <old-ext> <new-ext> — drop the superseded file and
# repoint every Markdown reference at the one we just wrote. A no-op unless
# the old file is actually there, so an ordinary regeneration does nothing.
#
# Rewriting by BASENAME covers all four reference spellings in the docs
# (Markdown `images/x.gif`, README's `docs/images/x.gif`, SHOWCASE's HTML
# <img src>, and the bare filenames in the showcase README's table) without
# needing to know which one a given doc uses.
clip_retarget() {
    local base=$1 old=$2 new=$3
    [[ -f "$base.$old" ]] || return 0
    local name; name="$(basename "$base")"
    rm -f "$base.$old"
    local md
    for md in $(grep -rl "$name\.$old" "$ROOT" --include='*.md' 2>/dev/null); do
        # The literal '.' is the only regex metacharacter an asset name can
        # contain, and it is escaped; names are otherwise [a-z0-9-].
        gsed -i "s/${name}\.${old}/${name}.${new}/g" "$md" 2>/dev/null \
            || sed -i '' "s/${name}\.${old}/${name}.${new}/g" "$md"
        echo "docs-assets: retargeted $(basename "$md") -> $name.$new"
    done
    [[ "$new" == png ]] && echo "docs-assets: NOTE an APNG is a .png to Markdown — say \"Animation:\" in the alt text of $name.png"
    return 0
}

# write_gif <out.gif> <frames> <step> <fps> <width> <args...> — record, take
# every <step>th frame, assemble a palette-optimized looping GIF. The kept
# frame count (frames/step) and fps set the clip length.
write_gif() {
    local out=$1 frames=$2 step=$3 fps=$4 width=$5; shift 5
    clip_frames "$frames" "$step" "$@"
    ffmpeg -y -framerate "$fps" -i "$WORK/sub/g-%04d.ppm" \
        -vf "scale=$width:-1:flags=lanczos,$GIF_QUANT" \
        -loop 0 "$WORK/raw.gif" >/dev/null 2>&1
    # -fuzz is a LOSSY delta step: it quantizes changed pixels to win
    # compression, and it is what speckles a GIF's flat blacks. It is also
    # what makes GIF competitive on size at all here, so it stays — the clips
    # that can't afford it are the ones that should be APNG.
    magick "$WORK/raw.gif" -fuzz "$GIF_FUZZ" -layers Optimize "$out"
    echo "docs-assets: wrote $out"
}

# write_apng <out.png> <frames> <step> <fps> <width> <args...> — the APNG twin
# of write_gif(), same arguments.
#
# ffmpeg's own APNG encoder is NOT a substitute for apngasm: it barely deltas
# between frames (6 MB against apngasm's 2 MB on the same 63 frames), which is
# why apngasm is a dependency rather than another `-f apng` output.
write_apng() {
    local out=$1 frames=$2 step=$3 fps=$4 width=$5; shift 5
    for tool in apngasm oxipng; do
        command -v "$tool" >/dev/null || {
            echo "docs-assets: '$tool' not found (brew install $tool), needed for APNG clips" >&2
            exit 1; }
    done
    clip_frames "$frames" "$step" "$@"
    rm -rf "$WORK/aq"; mkdir -p "$WORK/aq"
    ffmpeg -y -i "$WORK/sub/g-%04d.ppm" \
        -vf "scale=$width:-1:flags=lanczos,$APNG_QUANT" \
        "$WORK/aq/f-%04d.png" >/dev/null 2>&1
    # apngasm takes the frame delay in MILLISECONDS (rounded to nearest);
    # -l 0 loops forever. A GIF's delay field is centiseconds, so this is the
    # finer of the two clocks — 18 fps is 56ms here against GIF's 6cs.
    local delay=$(( (1000 + fps / 2) / fps ))
    apngasm -F -o "$out" "$WORK"/aq/f-*.png -d "$delay" -l 0 >/dev/null 2>&1
    # Lossless, ~8% off, and it preserves acTL/fcTL. `--zopfli` finds another
    # 3% for 20x the time — not worth it across a whole regeneration.
    oxipng -o max "$out" >/dev/null 2>&1
    echo "docs-assets: wrote $out"
}

# stage <name> — write a staged snippet scene to $WORK/<name>.c (heredoc
# read from stdin) and echo its path.
stage() {
    cat > "$WORK/$1.c"
    echo "$WORK/$1.c"
}

# stage_scene <name> <scene> — write capture-specific headers from stdin,
# then append an existing example scene as the body. This lets an asset use a
# catalog scene as its base while overriding presentation-only @cfg state.
stage_scene() {
    local name=$1 scene=$2 out="$WORK/$1.c"
    [[ -f "$scene" ]] || {
        echo "docs-assets: example scene not found: $scene" >&2
        return 1
    }
    cat > "$out"
    cat "$scene" >> "$out"
    echo "$out"
}

# montage2x2 <out> <a> <b> <c> <d> — tile four images 2x2 with a thin black
# gutter, font-free. (We avoid `magick montage`: its default per-tile label
# needs a font, and a misconfigured ImageMagick resolves the font to '' and
# aborts the whole tiling. Plain +append/-append needs no font.)
montage2x2() {
    local out=$1 a=$2 b=$3 c=$4 d=$5
    magick \
        \( "$a" -bordercolor black -border 2 \) \
        \( "$b" -bordercolor black -border 2 \) +append "$WORK/_row1.png"
    magick \
        \( "$c" -bordercolor black -border 2 \) \
        \( "$d" -bordercolor black -border 2 \) +append "$WORK/_row2.png"
    magick "$WORK/_row1.png" "$WORK/_row2.png" -append -background black "$out"
}

# montage1x2 <out> <a> <b> — tile two images side-by-side with a thin black
# gutter, same style as montage2x2.
montage1x2() {
    local out=$1 a=$2 b=$3
    magick \
        \( "$a" -bordercolor black -border 2 \) \
        \( "$b" -bordercolor black -border 2 \) +append -background black "$out"
}

# montage2x1 <out> <a> <b> — stack two images vertically, same style. Use it
# when the pair differs along the HORIZONTAL axis (two takes of the same wide
# scene): stacking keeps a screen x in both tiles at the same image x, so the
# difference reads as a vertical offset instead of a search across the page.
montage2x1() {
    local out=$1 a=$2 b=$3
    magick \
        \( "$a" -bordercolor black -border 2 \) \
        \( "$b" -bordercolor black -border 2 \) -append -background black "$out"
}

# ---- staged scenes ------------------------------------------------------

stage_triangle() { stage triangle <<'EOF'
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(1, 0.85, 0.15);
glBegin(GL_TRIANGLES);
glVertex3f(0, 1, 0);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glEnd();
// Snippet end
EOF
}

stage_overlays() { stage overlays <<'EOF'
/* @cfg vertex_labels = OVERLAY_VERTEX_LABEL_INDEX_POS */
/* @cfg normal_vectors = 1 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 1 */
/* @cfg poly_highlight = 1 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(0.15, 0.55, 1);
glBegin(GL_QUADS);
glNormal3f(0, 0, 1);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glVertex3f(1, 1, 0);
glVertex3f(-1, 1, 0);
glEnd();
// Snippet end
EOF
}


# Two quads in one glBegin/glEnd block. GLR_EDIT_LINE parks the cursor on
# the second quad's second vertex (line 11) so Single polygon scope narrows
# the highlight/labels to just that quad, not the whole shared block.
stage_single_polygon() { stage single_polygon <<'EOF'
/* @cfg poly_highlight = 1 */
/* @cfg vertex_labels = OVERLAY_VERTEX_LABEL_INDEX */
/* @cfg overlay_scope = OVERLAY_SCOPE_SINGLE_POLYGON */
/* @cfg grid = GRID_THEME_OFF */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glBegin(GL_QUADS);
glColor3f(0.95, 0.25, 1);
glVertex3f(-1.6, -0.8, 0);
glVertex3f(-0.2, -0.8, 0);
glVertex3f(-0.2, 0.8, 0);
glVertex3f(-1.6, 0.8, 0);
glColor3f(0.15, 0.55, 1);
glVertex3f(0.2, -0.8, 0);
glVertex3f(1.6, -0.8, 0);
glVertex3f(1.6, 0.8, 0);
glVertex3f(0.2, 0.8, 0);
glEnd();
// Snippet end
EOF
}

# One quad drawn twice by a loop, so the flat program holds two unrolled
# copies of the same glBegin block. That is the case where the two label
# numbering rules diverge: a decluttered label floats off its vertex and so
# must be globally unique (v0..v7), while an at-vertex label sits on the
# vertex it names and reads as its in-block ordinal (v0..v3, twice).
# $1 = OVERLAY_LABEL_PLACEMENT_<NAME>. GLR_EDIT_LINE parks the cursor on the
# block's first glVertex3f so the cursor-bound overlays fire.
stage_label_placement() { stage "lp-$1" <<EOF
/* @cfg vertex_labels = OVERLAY_VERTEX_LABEL_INDEX */
/* @cfg overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES */
/* @cfg vertex_label_placement = $1 */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// camera
glTranslatef(0.0f, 0.0f, -4.6f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(0.15, 0.55, 1);
for(i, 0, 2) {
glPushMatrix();
glTranslatef(-1.05 + i * 2.1, 0, 0);
glBegin(GL_QUADS);
glVertex3f(-0.75, -0.85, 0);
glVertex3f(0.75, -0.85, 0);
glVertex3f(0.75, 0.85, 0);
glVertex3f(-0.75, 0.85, 0);
glEnd();
glPopMatrix();
}
// Snippet end
EOF
}

stage_torus_mesh() {
    local name=$1
    local mode=$2
    stage "$name" <<EOF
/* @cfg wireframe = $mode */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glColor3f(0.1, 0.95, 0.85);
glutSolidTorus(0.4, 1.2, 24, 36);
// Snippet end
EOF
}

stage_wireframe() {
    stage_torus_mesh wireframe 1
}

# Hidden-line render mode: same torus as stage_wireframe, wireframe = 2.
stage_hidden_line() {
    stage_torus_mesh hidden_line 2
}

# Same two render modes (wireframe/hidden-line), but on a Sierpinski sponge
# instead of the torus -- pairs with stage_wireframe/stage_hidden_line in the
# wireframe-hidden-line 2x2 montage so the reader sees both modes work on a
# deeply nested recursive mesh, not just a simple primitive.
stage_sponge_mesh() {
    local name=$1
    local mode=$2
    stage "$name" <<EOF
/* @cfg wireframe = $mode */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// from examples/scenes/sierpinski-sponge-3d-recursion.glr
sponge(depth, size) {
  // Base case: one solid cube -- the neon lights do the coloring.
  if(depth <= 0) {
    glutSolidCube(size);
  }
  // Recursive case: -1..1 on each axis -- half-open bounds, so -1..2.
  // A cell survives when at least two of its coords are nonzero.
  if(depth > 0) {
    for(i, -1, 2) {
      for(j, -1, 2) {
        for(k, -1, 2) {
          if(abs(i) + abs(j) + abs(k) > 1) {
            glPushMatrix();
            glTranslatef(i*size/3, j*size/3, k*size/3);
            sponge(depth - 1, size/3);
            glPopMatrix();
          }
        }
      }
    }
  }
}

glTranslatef(0.0f, 0.0f, -6.00f);   // @camera dist
glRotatef(28.0f, 1.0f, 0.0f, 0.0f);   // @camera rx
glRotatef(50.0f, 0.0f, 1.0f, 0.0f);   // @camera ry
glTranslatef(0.0f, -0.0f, 0.0f);   // @camera pan
glClearColor(0.05, 0.06, 0.08, 1);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (GLfloat[]){1, 1, 1, 1});
glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, (GLfloat[]){0.22, 0.12, 0.4, 1});
glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (GLfloat[]){0.4, 0.4, 0.4, 1});
glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 30);
glEnable(GL_DEPTH_TEST);

glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glEnable(GL_LIGHT3);
glRotatef(16*t, 0, 1, 0);
sponge(2, 3);
EOF
}

stage_sponge_wireframe() {
    stage_sponge_mesh sponge-wireframe 1
}

stage_sponge_hidden_line() {
    stage_sponge_mesh sponge-hidden-line 2
}


stage_lights() { stage lights <<'EOF'
/* @cfg light_theme = LIGHT_THEME_DEFAULT */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg light_indicators = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 0 */
/* @cfg variable_panel = 0 */
glTranslatef(0.0000f, 0.0000f, -10.0000f);
glRotatef(15.5000f, 1.0f, 0.0f, 0.0f);
glRotatef(-118.0000f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0000f, 1.0000f, -0.0000f);
// Snippet start
glClearColor(0.0, 0.0, 0.0, 1.0);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glEnable(GL_COLOR_MATERIAL);
glColor3f(0.8, 0.7, 0.6);
glutSolidTeapot(1);
// Snippet end
EOF
}

# Lit teapot with the code panel LEFT VISIBLE (unlike stage_lights, which hides
# it): the light-theme-inspect asset photographs generated C, so it needs the
# panel. Studio theme, because the guide names it beside the montage.
stage_light_inspect() { stage light_inspect <<'EOF'
/* @cfg light_theme = LIGHT_THEME_STUDIO */
/* @cfg grid = GRID_THEME_XZRULER */
/* @cfg light_indicators = 1 */
/* @cfg variable_panel = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glEnable(GL_COLOR_MATERIAL);
glColor3f(0.8, 0.7, 0.6);
glutSolidTeapot(1);
// Snippet end
EOF
}

stage_grid_theme() {  # $1 = GRID_THEME_<NAME>
    stage "grid-$1" <<EOF
/* @cfg grid = $1 */
/* @cfg grid_brightness = GRID_BRIGHTNESS_BOLD */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
//glColor3f(1, 0.35, 0.2);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glutSolidCube(0.8);
// Snippet end
EOF
}

# The Grid brightness levels, staged against the case that makes the setting
# visible: a near-white lit cube sunk BELOW the grid plane, so the whole
# graticule draws over its faces. Raising brightness only raises line alpha,
# and alpha converges a line toward its own color -- over a bright surface the
# contrast tops out no matter the setting, which is why Bright/Bold add the
# dark contrast casing (GRID_CASING_* in src/render3d/grid.c). The cube has to
# be under y=0, not straddling it: geometry above the plane wins the depth
# test and no line reaches it. Default XZ Ruler theme on purpose -- it is what
# a user sees before touching F2.
stage_grid_brightness() {  # $1 = GRID_BRIGHTNESS_<NAME>
    stage "gridb-$1" <<EOF
/* @cfg grid_brightness = $1 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// camera
glTranslatef(0.0f, 0.0f, -6.80f);
glRotatef(30.0f, 1.0f, 0.0f, 0.0f);
glRotatef(25.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
//glColor3f(0.97, 0.95, 0.90);
glutSolidCube(1.0);
//glutSolidCube(0.8);
// Snippet end
EOF
}

stage_backdrop() {  # $1 = RENDER3D_BACKDROP_<NAME>, $2 = flat|default camera
    local cam=""
    if [[ "${2:-}" == "flat" ]]; then
        cam='// camera
glTranslatef(0.0f, 0.0f, -6.0f);
glRotatef(2.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, -0.5f, 0.0f);'
    fi
    stage "bd-$1" <<EOF
/* @cfg backdrop = $1 */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
$cam
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(0.8, 0.8, 0.8);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT1);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_axes() { stage axes <<'EOF'
/* @cfg axes = AXES_THEME_COMPASS */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg accum_effect = 1 */
/* @cfg accum_passes = 16 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_tune() { stage tune <<'EOF'
/* @cfg variable_panel = 1 */
// Snippet start
float amp = 1.2; // @tune
float freq = 2; // @tune
float spread = 0.8;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.15, 0.55, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 64) {
glVertex3f(-1.6 + i*0.05, sin(i*0.1*freq + t)*amp*0.4, spread*cos(i*0.05));
}
glEnd();
// Snippet end
EOF
}

stage_console() { stage console <<'EOF'
/* @cfg grid = GRID_THEME_DARK */
/* @cfg variable_panel = 0 */
/* @cfg code_panel = 0 */
/* @cfg wrap_at_commas = 0 */
// Snippet start
spoke(n, rad) {
  console("spoke %f rad=%f", n, rad);
  glBegin(GL_LINES);
  glVertex3f(0, 0, 0);
  glVertex3f(rad * cos(n * 0.8), rad * sin(n * 0.8), 0);
  glEnd();
}
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.3, 0.7, 1);
for(i, 0, 4) {
  console("step %f", i);
  spoke(i, 1.2);
}
// Snippet end
EOF
}

# Motion blur: a lit torus centered on the origin, spun about +Y. The torus
# lies in the XY plane, so its ±X rim sweeps a 1.2-radius circle while the
# ±Y points sit on the rotation axis and barely move — one shot showing the
# smear scaling with distance from the axis, which a translated solid (every
# point moving at the same speed) can't show.
stage_blur2() { stage blur <<'EOF'
/* @cfg accum_effect = 2 */
/* @cfg accum_passes = 16 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_COLOR_MATERIAL);
glPushMatrix();
glRotatef(t*200, 0, 1, 0);
glColor3f(1, 0.35, 0.2);
glutSolidTorus(0.4, 1.2, 24, 36);
glPopMatrix();
// Snippet end
EOF
}

stage_blur() { stage blur <<'EOF'
/* @cfg accum_effect = 2 */
/* @cfg accum_passes = 16 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_COLOR_MATERIAL);
glPushMatrix();
glRotatef(t*600, 0, 1, 0);
glColor3f(0.9, 0.35, 0.2);
glTranslatef(1.2, 0, 0);
glutSolidCube(0.6);
glPopMatrix();
// Snippet end
EOF
}

# Cursor parks on line 6 (glTranslatef) via GLR_EDIT_LINE.
stage_guide() { stage guide <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glutSolidCube(0.5);
glPushMatrix();
glTranslatef(2, 0.8, 0);
glColor3f(0.1, 0.95, 0.85);
glutSolidCube(0.5);
glPopMatrix();
// Snippet end
EOF
}

# One transform-guide kind per tiny program, cursor on line 4 (GLR_EDIT_LINE=4
# at the call site) — the transform whose guide is on show. Four programs rather
# than four cursor stops in one, so every tile's code strip is the whole program
# at the same crop and every guide anchors at the same place: the cursor line is
# the FIRST transform in each, so the default Frame mode's anchor (the
# pre-cursor modelview's origin) is the world origin in all four tiles.
#
#   translate     arrow = the argument vector, tip where the cube lands.
#   rotate        anchored on the rotation axis, so the guide draws its
#                 synthetic dial (see draw_rotate_guide); the trailing
#                 glTranslatef is there to give the sweep something visibly
#                 rotated to point at.
#   scale         the anchor is the origin, so scale always draws the 3-axis
#                 gizmo: gray unit reference per axis, pulse arrow only on the
#                 axes whose factor differs from 1. NO trailing translate in
#                 either scale tile — offset geometry would sit off the gizmo
#                 and read as a guide that missed its own cube.
#   scale-origin  the same gizmo with all three factors off 1 (and one of them
#                 below 1, so an inward arrow is in the montage too).
#
# Colors are active accent-palette anchors (make palette-list), one per tile so
# a tile is identifiable at a glance; the guide derives its own color from the
# command's vector, not from glColor3f.
stage_xform_guide() {  # $1 = translate | rotate | scale | scale-origin
    local color body
    case "$1" in
        translate)
            color='0.1, 0.95, 0.85'
            body='glTranslatef(2, 0.8, 0);
glutSolidCube(0.4);' ;;
        rotate)
            color='1, 0.35, 0.2'
            body='glRotatef(45, 0, 1, 0);
glutSolidCube(0.5);' ;;
        scale)
            color='1, 0.85, 0.15'
            body='glScalef(2.2, 1, 1);
glutSolidCube(0.35);' ;;
        scale-origin)
            color='0.55, 0.3, 1'
            body='glScalef(1.8, 0.6, 1.4);
glutSolidCube(0.35);' ;;
        *)  echo "docs-assets: unknown xform guide case '$1'" >&2; return 1 ;;
    esac
    stage "xg-$1" <<EOF
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f($color);
$body
// Snippet end
EOF
}

# World vs Frame guide mode, same program and same cursor line both times
# (the second glTranslatef, GLR_EDIT_LINE=7) so only the anchor moves: World
# draws from the world origin, Frame from where the pre-cursor modelview left
# it — the first cube — which puts the Frame arrow's tip on the second cube and
# the World arrow's tip in empty space. That contrast is the whole asset.
#
# The vector is deliberately diagonal: an axis-aligned one would draw both
# arrows along the red world X axis line, where the two anchors are much harder
# to tell apart. transform_guides has no symbolic cfg form, so the mode is the
# raw enum ordinal — 1 = RENDER3D_XFORM_GUIDE_WORLD, 2 = _FRAME.
stage_xform_guide_mode() {  # $1 = 1 (World) | 2 (Frame)
    stage "xgm-$1" <<EOF
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
/* @cfg transform_guides = $1 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glTranslatef(1.8, 0.4, 0);
glutSolidCube(0.5);
glColor3f(0.1, 0.95, 0.85);
glTranslatef(-3.6, 1, 0);
glutSolidCube(0.5);
// Snippet end
EOF
}

# Vertex entry guides: the snippet ends on an OPEN glBegin block, so the
# cursor lands on the in-block append row after load, and GLR_TYPE_KEYS
# (exported at the call site) types a partial glVertex3f( there — the
# only way to pose the 2-DOF sheet / 1-DOF line guides, which exist only
# mid-typing (a committed line always has all three coordinates).
#
# vertex_labels needs WHOLE_SCENE to show up here. Every other scope anchors
# the labels to the cursor's block, and the cursor spends these captures on a
# half-typed line, which is not a committed vertex and so numbers nothing --
# the labels stayed silently off under the default scope. WHOLE_SCENE is not
# cursor-anchored, so the vertex committed in the fourth capture gets its
# v0 readout while the guide for the *next* vertex is still up. (The v0/v1
# gutter marks in the code panel remain cursor-bound and do not appear; only
# the in-scene labels are scope-driven.)
stage_vertex_entry() { stage vertex_entry <<'EOF'
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 1 */
/* @cfg vertex_labels = OVERLAY_VERTEX_LABEL_INDEX_POS */
/* @cfg overlay_scope = OVERLAY_SCOPE_WHOLE_SCENE */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_OFF */
// camera
// -9.5 rather than the default framing: the 2-DOF sheet is a fixed-size quad
// around the pinned coordinate, and at -6.5 the x = 1.2 one (a vertical plane,
// seen near edge-on from this iso angle) runs off the bottom-right corner. This
// distance fits all four guide shapes inside one shared crop.
glTranslatef(0.0000f, 0.0000f, -9.5000f);
glRotatef(35.2500f, 1.0f, 0.0f, 0.0f);
glRotatef(45.0000f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0000f, -0.0000f, -0.0000f);
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glBegin(GL_TRIANGLES);
// Snippet end
EOF
}

# Clip plane: cursor parks on the glClipPlane line (GLR_EDIT_LINE=6 at the
# call site) so the clip-plane guide renders — gridded disc, ghost rim,
# kept-half-space arrow with the P0 readout.
stage_clip_plane() { stage clip_plane <<'EOF'
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_COLOR_MATERIAL);
glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0.2, 1, 0.3, 0.4});
glEnable(GL_CLIP_PLANE0);
glColor3f(1, 0.35, 0.2);
glutSolidSphere(1.5, 32, 24);
// Snippet end
EOF
}

# Animated clip plane: d = sin(t*3)*1.1 sweeps one full cycle in ~126
# frames (t*3 has period TAU/3 = 2.09 s = 126 captured frames), so the
# GIF loops seamlessly. Cursor parks on the glClipPlane line so the
# guide disc sweeps with the plane.
stage_clip_sweep() { stage clip_sweep <<'EOF'
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_COLOR_MATERIAL);
glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0, 1, 0, sin(t*3)*1.1});
glEnable(GL_CLIP_PLANE0);
glColor3f(0.15, 0.55, 1);
glutSolidTorus(0.5, 1.2, 24, 36);
// Snippet end
EOF
}

stage_replay() { stage replay <<'EOF'
/* @cfg replay = 1 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_points = 0 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glBegin(GL_TRIANGLES);
for(i, 0, 12) {
glColor3f(0.67 + 0.31*cos(i*TAU/12), 0.58 - 0.12*cos(i*TAU/12), 0.67 - 0.31*cos(i*TAU/12));
glVertex3f(cos(i*TAU/12)*1.5, sin(i*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.6)*TAU/12)*1.5, sin((i+0.6)*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.3)*TAU/12)*0.7, sin((i+0.3)*TAU/12)*0.7, 0);
}
glEnd();
// Snippet end
EOF
}

# Depth view: three lit solids spread across the depth range so the
# grayscale gradient has something to show. $1 = depth_view mode
# (2 = Scene full-rect, 3 = Split right-half overlay). The grid stays on:
# in Split it renders on the normal (left) half but is absent from the
# depth (right) half — the "scene depth only, helpers excluded" contract
# in one image.
stage_depth_view() {  # $1 = depth_view mode
    stage "depth-view-$1" <<EOF
/* @cfg depth_view = $1 */
/* @cfg grid = GRID_THEME_XZRULER */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg code_panel = 3 */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_COLOR_MATERIAL);
glColor3f(1, 0.35, 0.2);
glutSolidTeapot(0.8);
glPushMatrix();
glTranslatef(-1.7, 0.4, -2.2);
glColor3f(0.15, 0.55, 1);
glutSolidCube(0.9);
glPopMatrix();
glPushMatrix();
glTranslatef(1.5, -0.2, 1.4);
glColor3f(0.1, 0.95, 0.85);
glutSolidSphere(0.5, 32, 24);
glPopMatrix();
// Snippet end
EOF
}

# Winding view: one CCW triangle (front-facing, renders green) beside one
# CW triangle (back-facing, renders red) — the diagnostic's whole point.
stage_winding() { stage winding <<'EOF'
/* @cfg winding = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glBegin(GL_TRIANGLES);
glVertex3f(-1.5, 0, 0);
glVertex3f(-0.2, 0, 0);
glVertex3f(-0.85, 0.8, 0);
glVertex3f(0.2, 0, 0);
glVertex3f(0.85, 0.8, 0);
glVertex3f(1.5, 0, 0);
glEnd();
// Snippet end
EOF
}

# Autocomplete popup: a committed backdrop line, then GLR_TYPE_KEYS (at the
# call site) types a partial glEnable(GL_LI on the fresh append row — the
# enum-slot popup lists the GL_LI* candidates with the ghost text inline.
stage_autocomplete() { stage autocomplete <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glutSolidTeapot(0.8);
// Snippet end
EOF
}

# Color picker: GLR_EDIT_LINE parks the cursor on the glColor3f line (its
# swatch shows at the panel's right edge) and GLR_OPEN_COLOR_PICKER opens
# the floating picker on it, as clicking the swatch would.
stage_color_picker() { stage color_picker <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glEnable(GL_COLOR_MATERIAL);
glColor3f(1, 0.35, 0.2);
glutSolidTeapot(0.9);
// Snippet end
EOF
}

# Numeric stepper: GLR_EDIT_LINE parks the cursor on the declaration line;
# loading it puts the cursor at end-of-line, inside the initializer's
# number, so the inline stepper appears at the panel's right edge.
stage_stepper() { stage stepper <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
float radius = 1.5;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(0.1, 0.95, 0.85);
glutSolidSphere(radius, 32, 16);
// Snippet end
EOF
}

# Assignment value plot, exec-index axis: the `wave = ...` row inside the loop
# runs once per iteration, so one captured frame carries the whole sweep. The
# row index the GLR_OPEN_ASSIGN_PLOT hook targets is counted AFTER the @cfg
# headers and the snippet markers are stripped, same as GLR_EDIT_LINE.
stage_assign_plot() { stage assign_plot <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg vertex_labels = 0 */
/* @cfg auto_normals = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
float wave;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.35, 0.8, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 160) {
wave = sin(i * 0.08 + t) * 0.7 * cos(i * 0.013);
glVertex3f(i * 0.0125 - 1, wave, 0);
}
glEnd();
// Snippet end
EOF
}

# Three series on one plot. All three rows sit in the same loop body, so they
# share an execution count and therefore an X axis (a row with a different one
# is refused at add time — that rule is what this scene has to satisfy). The
# two components and their sum are deliberately the same order of magnitude:
# the shot is about the legend and the overlay, not about the shared-Y
# flattening the prose warns of.
stage_assign_plot_series() { stage assign_plot_series <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg vertex_labels = 0 */
/* @cfg auto_normals = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
float base, ripple, wave;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.35, 0.8, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 160) {
base = sin(i * 0.05 + t) * 0.5;
ripple = cos(i * 0.17 - t * 1.3) * 0.25;
wave = base + ripple;
glVertex3f(i * 0.0125 - 1, wave, 0);
}
glEnd();
// Snippet end
EOF
}

# The symmetric-log axis, which is only legible against its linear twin: two
# sinusoids an order of magnitude apart, plotted together. Shared linear Y
# flattens `small` onto the zero line; log lifts it a decade below `big` while
# both still cross a real zero. Same scene, captured twice with
# GLR_ASSIGN_PLOT_LOG toggling the chip.
stage_assign_plot_log() { stage assign_plot_log <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg vertex_labels = 0 */
/* @cfg auto_normals = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
float big, small;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.35, 0.8, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 160) {
big = sin(i * 0.05 + t) * 0.6;
small = sin(i * 0.05 + t) * 0.006;
glVertex3f(i * 0.0125 - 1, big + small, 0);
}
glEnd();
// Snippet end
EOF
}

# Same panel on its other X axis: a top-level row runs exactly once per frame,
# so the plot becomes a time series over successive captures. That needs real
# elapsed seconds rather than a single frame, which is what the long WARM at
# the call site buys — record mode still advances GLUT_ELAPSED_TIME by wall
# clock, so the default 1 Hz capture rate ticks while the frames are written.
stage_assign_plot_frames() { stage assign_plot_frames <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg vertex_labels = 0 */
/* @cfg auto_normals = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
float angle;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_COLOR_MATERIAL);
angle = sin(t * 0.9) * 40;
glRotatef(angle, 0.2, 1, 0);
glColor3f(0.95, 0.6, 0.25);
glutSolidTeapot(0.6);
// Snippet end
EOF
}

# OpenGL state inspector: the blank row after the authored state changes is
# line 10 after @cfg headers/snippet markers are stripped and the authored
# light position is lifted into generated display setup. The GLR_OPEN_GL_STATE
# capture hook opens the same popup a right-click would.
stage_gl_state_inspector() { stage gl_state_inspector <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
glColor4f(0.15, 0.55, 1, 0.8);
glTranslatef(0.25, 0, -0.4);
glRotatef(22.5, 0, 1, 0);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glLightfv(GL_LIGHT0, GL_POSITION, (GLfloat[]){2, 3, 4, 1});

glutSolidTeapot(0.8);
// Snippet end
EOF
}

# Profile panels: Histogram mode shows the section listing (CPU/GPU/Max
# columns), the log-log section histogram, and the FPS plot at once. Use the
# real wave-surface example as the workload, with asset-specific presentation
# config prepended to it. Stock AA: raising the accum passes would multiply
# every section's cost 16x and distort the numbers the panel is there to show.
stage_profile() {
    stage_scene profile \
        "$ROOT/examples/scenes/wave-surface-analytic-normals.glr" <<'EOF'
/* @cfg compute_profile = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
EOF
}

# Window tour: a two-scene workspace so the scene-tab strip shows, with the
# variable panel on — one shot covering every piece of window chrome the
# guide's "The window" section names.
#
# The visible scene is a lit torus tumbling about two axes, captured under the
# Blur accum effect at 16 passes (GLR_ACCUM_EFFECT/GLR_ACCUM_PASSES at the call
# site), so the shot shows what the status bar's `Blur 16x` indicator means
# rather than naming it over a static triangle. Both angles are expressions in
# `t`, which buys three things at once: the blur has something to smear, the
# variable panel has live rows to list, and the two rows can be plotted
# together — a top-level assignment runs once per frame, so both share the
# captures X axis and the panel overlays them.
stage_window_tour_dir() {
    local ws="$WORK/tour-ws"
    mkdir -p "$ws"
    # A managed workspace is its manifest: a directory of .c files with no
    # .glr-workspace is rejected outright and the app falls back to a fresh
    # scene, which is how this asset quietly lost its scene tabs once the
    # manifest became mandatory.
    cat > "$ws/.glr-workspace" <<'EOF'
version=1
name=Tour
scene=torus.c
scene=ring.c
EOF
    cat > "$ws/torus.c" <<'EOF'
// @scene-name Spinning Torus
/* @cfg vertex_points = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg poly_highlight = 0 */
/* @cfg grid = GRID_THEME_CLASSIC */
/* @cfg grid_brightness = GRID_BRIGHTNESS_DIM */
// Snippet start
float ang0, ang1;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_COLOR_MATERIAL);
ang0 = rem(t * 750, 360);
ang1 = rem(t * 2000, 360);
glColor3f(1, 0.85, 0.15);
glutSolidSphere(0.4, 32, 24);
glPushMatrix();
  glRotatef(ang0, 1, 0, 0);
  glRotatef(ang1, 0, 1, 0);
  glColor3f(0.35, 0.8, 1);
  glutSolidTorus(0.2, 0.8, 24, 48);
glPopMatrix();
// Snippet end
EOF
    cat > "$ws/ring.c" <<'EOF'
// @scene-name Ring Sketch
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.1, 0.95, 0.85);
glBegin(GL_LINE_LOOP);
for(i, 0, 32) {
glVertex3f(cos(i*TAU/32), sin(i*TAU/32), 0);
}
glEnd();
// Snippet end
EOF
    echo "$ws"
}

# ---- example names (stable; never reference examples by index) ----------

EX_HERO="Ringed planet"
EX_ORRERY="Orrery (labels track 3D orbits)"
EX_GLU="GLU concave arrow"
EX_GLOW="Glow sprites (blend + point attenuation)"
EX_XFORM="Transform stress (translate/rotate/scale guides)"
EX_RING="Animated ring (for + t)"

# ---- core assets --------------------------------------------------------

if want hero; then
    (
    WARM=${WARM_SPLASH}
    still "$OUT/hero.png" 16 --example "$EX_HERO" --time 3
    )
fi

if want first-triangle; then
    still "$OUT/first-triangle.png" 16 "$(stage_triangle)"
fi

# Overlay toggles: the ten-line program above the quad it annotates, cropped to
# both and stacked. GLR_EDIT_LINE=5 parks the cursor on the glNormal3f row --
# the overlays are cursor-block bound, so with the cursor on the trailing blank
# row (the old shot) there is no block and the normals, labels and highlight
# the asset exists to show are simply not drawn. WARM outlasts the splash,
# which otherwise dims the bottom of the scene crop.
if want vertex-overlays; then
    ( WARM=$((WARM_SPLASH + 30))
      export GLR_EDIT_LINE=5
      still "$WORK/vo-full.png" 16 "$(stage_overlays)" )
    write_png "$WORK/vo-full.png" "$WORK/vo-code.png" \
        -crop "$VO_CODE_CROP" +repage
    write_png "$WORK/vo-full.png" "$WORK/vo-scene.png" \
        -crop "$VO_SCENE_CROP" +repage
    montage2x1 "$WORK/vo-pair.png" "$WORK/vo-code.png" "$WORK/vo-scene.png"
    write_png "$WORK/vo-pair.png" "$OUT/vertex-overlays.png"
    echo "docs-assets: wrote $OUT/vertex-overlays.png"
fi

# Single-polygon scope: the cursor's row in the code above the one quad it
# narrows the overlays down to. Cropped to both -- the claim is that the two
# quads share a glBegin and only one of them lights up, which needs the code
# and the scene, and nothing else in the window.
if want single-polygon-scope; then
    ( WARM=$((WARM_SPLASH + 30))
      export GLR_EDIT_LINE=11
      still "$WORK/sps-full.png" 16 "$(stage_single_polygon)" )
    write_png "$WORK/sps-full.png" "$WORK/sps-code.png" \
        -crop "$SP_CODE_CROP" +repage
    write_png "$WORK/sps-full.png" "$WORK/sps-scene.png" \
        -crop "$SP_SCENE_CROP" +repage
    montage2x1 "$WORK/sps-pair.png" "$WORK/sps-code.png" "$WORK/sps-scene.png"
    write_png "$WORK/sps-pair.png" "$OUT/single-polygon-scope.png"
    echo "docs-assets: wrote $OUT/single-polygon-scope.png"
fi

# Cursor-follow demo: the SAME asset twice, differing only in which line the
# cursor sits on. Transform stress calls one tri() function from two different
# transform stacks, so parking the cursor on each call in turn moves the
# highlight between two triangles that are far apart on screen and different
# colours -- which is the point the User Guide's overlay section is making.
# The lines are the code panel's own numbers -- 54 and 60, the two tri() calls.
#
# Each tile is a code strip above a scene strip (same width, so -append needs
# no padding), montaged at NATIVE resolution: the code rows have to stay
# legible, and a full-window 1x2 montage would need a 2x downscale to fit $W.
# GLR_NO_SPLASH keeps the startup wordmark out of the scene pane without
# paying WARM_SPLASH frames for it, and GLR_TICK_PER_FRAME pins t so both
# captures render the same pose of the spinning tri().
#
# use a long warm to wait for the camera crosshair to fade
if want cursor-highlight; then
    # Named once and reused by both the capture loop and the montage: these
    # two numbers were renumbered in the loop alone once already, leaving the
    # montage reaching for tiles nothing had written.
    CH_LINE_A=54; CH_LINE_B=60
    ( export GLR_NO_SPLASH=1 GLR_TICK_PER_FRAME=1
      for line in $CH_LINE_A $CH_LINE_B; do
          ( export GLR_EDIT_LINE=$line
            WARM=220 still "$WORK/ch-$line.png" 16 \
                --example "$EX_XFORM" --time 1 )
          magick "$WORK/ch-$line.png" -crop "$CH_CODE_CROP" +repage \
              "$WORK/ch-code-$line.png"
          magick "$WORK/ch-$line.png" -crop "$CH_SCENE_CROP" +repage \
              "$WORK/ch-scene-$line.png"
          magick "$WORK/ch-code-$line.png" "$WORK/ch-scene-$line.png" \
              -append -background black "$WORK/ch-tile-$line.png"
      done )
    montage1x2 "$WORK/ch-pair.png" \
        "$WORK/ch-tile-$CH_LINE_A.png" "$WORK/ch-tile-$CH_LINE_B.png"
    write_png "$WORK/ch-pair.png" "$OUT/cursor-highlight.png"
    echo "docs-assets: wrote $OUT/cursor-highlight.png"
fi

# Cropped to the scene pane and montaged at NATIVE resolution rather than
# resized to $W: this asset is read for its label text, and the 2x downscale
# a full-window 1x2 montage needs makes v0..v7 illegible.
#
# The program that produced the labels rides IN the image, as a code-panel
# strip above the pair, so the guide never has to quote a copy that can drift
# from what the asset actually rendered. One strip, not two: the code panel is
# identical in both captures (its gutter marks auto-normals, not label numbers,
# so it does not vary with the placement). The widths are picked to match
# montage1x2's output exactly -- (545 + 2*2) * 2 == 1094 + 2*2 -- so the
# vertical append needs no padding.
if want label-placement; then
    ( export GLR_EDIT_LINE=9
      still "$WORK/lp-declutter.png" 16 \
            "$(stage_label_placement OVERLAY_LABEL_PLACEMENT_DECLUTTERED)"
      still "$WORK/lp-at-vertex.png" 16 \
            "$(stage_label_placement OVERLAY_LABEL_PLACEMENT_AT_VERTEX)" )
    magick "$WORK/lp-declutter.png" -crop "${LP_CODE_CROP}" +repage \
        -bordercolor black -border 2 "$WORK/lp-code.png"
    magick "$WORK/lp-declutter.png" -crop "${LP_CROP}" +repage "$WORK/lp-a.png"
    magick "$WORK/lp-at-vertex.png" -crop "${LP_CROP}" +repage "$WORK/lp-b.png"
    montage1x2 "$WORK/lp-pair.png" "$WORK/lp-a.png" "$WORK/lp-b.png"
    magick "$WORK/lp-code.png" "$WORK/lp-pair.png" -append \
        -background black "$WORK/lp-all.png"
    write_png "$WORK/lp-all.png" "$OUT/label-placement.png"
    echo "docs-assets: wrote $OUT/label-placement.png"
fi

if want wireframe-hidden-line; then
    still "$WORK/wireframe.png" 16 "$(stage_wireframe)"
    still "$WORK/hidden-line.png" 16 "$(stage_hidden_line)"
    still "$WORK/sponge-wireframe.png" 16 "$(stage_sponge_wireframe)"
    still "$WORK/sponge-hidden-line.png" 16 "$(stage_sponge_hidden_line)"
    montage2x2 "$WORK/wf-hl.png" "$WORK/wireframe.png" "$WORK/hidden-line.png" \
        "$WORK/sponge-wireframe.png" "$WORK/sponge-hidden-line.png"
    write_png "$WORK/wf-hl.png" "$OUT/wireframe-hidden-line.png" -resize "$W"
    echo "docs-assets: wrote $OUT/wireframe-hidden-line.png"
fi

# Winding view: two triangles listing their vertices in opposite orders, above
# the green/red the view paints them. The point is the ORDER in the code beside
# the color in the scene, so both crops are the asset; the rest of the window
# was grid.
if want winding-view; then
    ( WARM=$((WARM_SPLASH + 30))
      still "$WORK/wv-full.png" 16 "$(stage_winding)" )
    write_png "$WORK/wv-full.png" "$WORK/wv-code.png" \
        -crop "$WV_CODE_CROP" +repage
    write_png "$WORK/wv-full.png" "$WORK/wv-scene.png" \
        -crop "$WV_SCENE_CROP" +repage
    montage2x1 "$WORK/wv-pair.png" "$WORK/wv-code.png" "$WORK/wv-scene.png"
    write_png "$WORK/wv-pair.png" "$OUT/winding-view.png"
    echo "docs-assets: wrote $OUT/winding-view.png"
fi

# Depth view montage: Scene mode (full-rect scene-normalized grayscale)
# beside Split (right half depth over the normal render). Same staged
# solids, only the depth_view mode differs.
if want depth-view; then
    still "$WORK/depth-scene.png" 16 "$(stage_depth_view 2)"
    still "$WORK/depth-split.png" 16 "$(stage_depth_view 3)"
    montage1x2 "$WORK/depth-m.png" "$WORK/depth-scene.png" "$WORK/depth-split.png"
    write_png "$WORK/depth-m.png" "$OUT/depth-view.png" -resize "$W"
    echo "docs-assets: wrote $OUT/depth-view.png"
fi

# WARM=200: past the ~186-frame splash fade, so the wordmark doesn't occlude
# the variable panel this shot is naming — and, at the frame capture rate, 200
# columns of plot history. The rate is the reason the shot is reproducible:
# 1 Hz fills the plot on wall clock, so the sample count would track how fast
# this machine renders 16 accumulation passes.
#
# GLR_OPEN_COMMAND_HELP goes last of the three popup hooks by construction, not
# by luck: right-clicking an assignment row closes the help card, so posing the
# plot after it would take the card back down (glr_capture_env.c orders the
# frame hooks the same way). Its `,330` offset slides the card clear of the
# code text: the click has to land on the glutSolidSphere row, but the card
# does not have to sit on top of the program to explain it.
if want window-tour; then
    ( WARM=200
      export GLR_ACCUM_EFFECT=blur
      export GLR_EDIT_LINE=10 GLR_OPEN_ASSIGN_PLOT=7,8
      export GLR_ASSIGN_PLOT_RATE=frame
      export GLR_OPEN_COMMAND_HELP=10,330
      still "$OUT/window-tour.png" 16 "$(stage_window_tour_dir)" )
fi

# The rig, cropped to the half of the frame it occupies: the four indicators
# and the teapot they light. Everything below the teapot was empty black.
# GLR_EDIT_LINE=6 is the glEnable(GL_LIGHT1) row, so L1 carries the
# cursor-follows-the-light halo the guide describes.
if want light-theme-studio; then
    ( export GLR_EDIT_LINE=6
      still "$WORK/lts-full.png" 16 "$(stage_lights)" )
    write_png "$WORK/lts-full.png" "$OUT/light-theme-studio.png" \
        -crop "$LTS_CROP" +repage
    echo "docs-assets: wrote $OUT/light-theme-studio.png"
fi

# Where the rig's numbers actually live. No REPL command sets a light, so the
# only place to read one is the generated C that code focus hides:
# GLR_CODE_FOCUS=0 shows it, and GLR_CODE_SCROLL parks the two blocks that
# matter -- positions in the display() prologue, colors in init(). Two runs
# because they are ~75 rows apart. Those row numbers count generated lines, so
# they move if the staged scene's own length changes; re-probe with
# GLR_CODE_SCROLL before assuming a shifted crop is a rendering bug.
if want light-theme-inspect; then
    for li_scroll in "$LI_POS_SCROLL" "$LI_COL_SCROLL"; do
        ( export GLR_NO_SPLASH=1 GLR_CODE_FOCUS=0
          export GLR_CODE_SCROLL=$li_scroll
          still "$WORK/li-$li_scroll.png" 16 "$(stage_light_inspect)" )
    done
    write_png "$WORK/li-$LI_POS_SCROLL.png" "$WORK/li-pos.png" \
        -crop "$LI_POS_CROP" +repage
    write_png "$WORK/li-$LI_COL_SCROLL.png" "$WORK/li-col.png" \
        -crop "$LI_COL_CROP" +repage
    montage2x1 "$WORK/li-pair.png" "$WORK/li-pos.png" "$WORK/li-col.png"
    write_png "$WORK/li-pair.png" "$OUT/light-theme-inspect.png"
    echo "docs-assets: wrote $OUT/light-theme-inspect.png"
fi

if want grid-themes; then
    themes=(SKETCH RADAR PLANES OCEAN)
    files=()
    for theme in "${themes[@]}"; do
        ( WARM=$WARM_FADE
          still "$WORK/grid_$theme.png" 16 \
              "$(stage_grid_theme GRID_THEME_$theme)" )
        files+=("$WORK/grid_$theme.png")
    done
    montage2x2 "$WORK/grid-m.png" "${files[@]}"
    write_png "$WORK/grid-m.png" "$OUT/grid-themes.png" -resize "$W"
    echo "docs-assets: wrote $OUT/grid-themes.png"
fi

# Reading order is the F4 cycle order. Each tile is cropped to GB_CROP and
# tiled at 1:1 with no final -resize: the whole subject is 1-3px lines and the
# casing that edges them, and even the ~0.3% downscale that would square this
# montage to $W blurs exactly the pixels the asset exists to show.
if want grid-brightness; then
    levels=(DIM NORMAL BRIGHT BOLD)
    gb_files=()
    for level in "${levels[@]}"; do
        (
          still "$WORK/gridb_$level.png" 16 \
              "$(stage_grid_brightness GRID_BRIGHTNESS_$level)" )
        write_png "$WORK/gridb_$level.png" "$WORK/gridb_${level}_c.png" \
            -crop "$GB_CROP" +repage
        gb_files+=("$WORK/gridb_${level}_c.png")
    done
    montage2x2 "$WORK/gridb-m.png" "${gb_files[@]}"
    write_png "$WORK/gridb-m.png" "$OUT/grid-brightness.png"
    echo "docs-assets: wrote $OUT/grid-brightness.png"
fi

if want backdrops; then
    backdrops=(
        "POLAR_DAY_SNOW"
        "NEBULA flat"
        "SUNSET flat"
        "AURORA flat"
    )
    files=()
    for item in "${backdrops[@]}"; do
        name="${item%% *}"
        if [[ "$item" == *" "* ]]; then
            cam="${item#* }"
        else
            cam=""
        fi
        ( WARM=$WARM_FADE
          still "$WORK/bd_$name.png" 16 \
              "$(stage_backdrop RENDER3D_BACKDROP_$name "$cam")" )
        files+=("$WORK/bd_$name.png")
    done
    montage2x2 "$WORK/bd-m.png" "${files[@]}"
    write_png "$WORK/bd-m.png" "$OUT/backdrops.png" -resize "$W"
    echo "docs-assets: wrote $OUT/backdrops.png"
fi

if want axes-compass; then
    ( WARM=$WARM_FADE
      still "$OUT/axes-compass.png" 16 "$(stage_axes)" )
fi

if want view-mode-2d; then
    # A GIF that toggles View mode (Ctrl+Shift+V) on the wave surface,
    # so the doc shows the 3D->2D->3D transition rather than a frozen 2D still.
    # GLR_VIEW_TOGGLE_AT fires on gl_repl's rendered-frame clock (t = frame/60),
    # which counts the WARM_EXAMPLE leading frames gif() renders and discards for
    # --example clips. WARM_EXAMPLE=180 => 3.0s of that clock elapses before the kept
    # clip begins, so a toggle at clock second S lands at (S*60 - WARM_EXAMPLE)/(step)
    # kept frames in, i.e. output time (S*60 - WARM_EXAMPLE)/(step*fps) s. With
    # step=2 fps=20: clock 4s -> 1.5s into the clip (3D->2D), clock 6s -> 4.5s
    # (2D->3D). The 6s clip then holds ~1.5s of 3D before it loops.
    #
    # APNG, not GIF: the wave surface is most of the frame and its shaded
    # gradient is exactly what the GIF delta path degrades. See apng().
    ( export GLR_VIEW_TOGGLE_AT=4,6
      clip "$OUT/view-mode-2d" 240 2 20 560 \
          --example "Wave surface (analytic normals)" )
fi

if want labels-orrery; then
    still "$OUT/labels-orrery.png" 16 --example "$EX_ORRERY" --time 4
fi

# GLU tessellator: the contour structure above the shape it tessellates into,
# stacked the way the app stacks them. A full-window shot spent two thirds of
# its pixels on empty grid and on the gluColor/gluVertex tail, which says
# nothing the head of the polygon doesn't. GLR_EDIT_LINE scrolls the panel to
# the `// Arrow shape` comment; parking on a gluBegin instead would light the
# cursor-block vertex labels, which belong to the overlay section, not here.
# WARM_FADE: the Tron grid theme cross-fades in, and the old shot was captured
# mid-fade.
if want glu-tess; then
    ( WARM=$WARM_FADE
      export GLR_EDIT_LINE=17
      still "$WORK/glu-full.png" 16 --example "$EX_GLU" )
    write_png "$WORK/glu-full.png" "$WORK/glu-code.png" \
        -crop "$GT_CODE_CROP" +repage
    write_png "$WORK/glu-full.png" "$WORK/glu-scene.png" \
        -crop "$GT_SCENE_CROP" +repage
    montage2x1 "$WORK/glu-pair.png" "$WORK/glu-code.png" "$WORK/glu-scene.png"
    write_png "$WORK/glu-pair.png" "$OUT/glu-tess.png"
    echo "docs-assets: wrote $OUT/glu-tess.png"
fi

# Glow sprites: the four point-sprite setup rows above the cloud they produce.
# WARM stays 12 with --time 2 -- the cloud's look is a tuned moment of the
# animation, not a settled state, so this one keeps its short warm-up and the
# scene crop stops above the splash strip instead of outlasting it.
if want glow-sprites; then
    ( WARM=12
      still "$WORK/glow-full.png" 16 --example "$EX_GLOW" --time 2 )
    write_png "$WORK/glow-full.png" "$WORK/glow-code.png" \
        -crop "$GS_CODE_CROP" +repage
    write_png "$WORK/glow-full.png" "$WORK/glow-scene.png" \
        -crop "$GS_SCENE_CROP" +repage
    montage2x1 "$WORK/glow-pair.png" "$WORK/glow-code.png" \
        "$WORK/glow-scene.png"
    write_png "$WORK/glow-pair.png" "$OUT/glow-sprites.png"
    echo "docs-assets: wrote $OUT/glow-sprites.png"
fi

if want transform-stress; then
    still "$OUT/transform-stress.png" 16 --example "$EX_XFORM" --time 1
fi

# The panel itself, cropped out of the full window: everything the guide's
# panel section and its knob section point at is inside those 265x122 px --
# `t` plus three declared rows, the two @tune ones carrying their accent mark
# against the two that don't. This crop used to be a second asset
# (tune-badges.png) taken from the same still; one image serves both sections,
# since the accent mark only means anything beside a row without one.
if want variable-panel; then
    ( WARM=$WARM_SPLASH
      still "$WORK/variable-panel-full.png" 16 "$(stage_tune)" )
    write_png "$WORK/variable-panel-full.png" "$OUT/variable-panel.png" \
        -crop "$VP_CROP" +repage
    echo "docs-assets: wrote $OUT/variable-panel.png"
fi

# The console panel: captures trace lines and formats them with call-depth auto-indentation.
if want console-panel; then
    ( WARM=$WARM_SPLASH W=980 H=400
      export GLR_EDIT_LINE=1 GLR_PANEL_FRAC=0.60
      still "$OUT/console-panel.png" 16 "$(stage_console)" )
fi

# --- Exported-C stills -----------------------------------------------------
#
# The only two assets that photograph a COMPILED EXPORT rather than the app.
# The whole point of the section in USER_GUIDE.md is that Ctrl+S emits a real
# program, so a gl-repl screenshot cannot make the claim: these run the export
# through `cc` and capture the resulting binary's own window (no code panel,
# no grid, no menu bar — just the scene and the generated @tune HUD).
#
# The pipeline is: GLR_TYPE_KEYS=Ctrl+S on the grass example writes output.c ->
# sed the two @tune initializers (exactly what q/a and w/s do at runtime) and
# the window size -> cc -> FREEGLUT_CAPTURE_FRAMES. Nothing else in the export
# is touched, so what the stills show is the generator's own output.
#
# Capture needs the record mode, which lives in the VENDORED freeglut — so the
# export links against third_party/freeglut's static archive rather than the
# system GLUT the doc's build line uses. Same C, different -l.
EXPORT_C_SCENE="Swaying grass field (rand + t)"
EXPORT_C_DIR="$WORK/export-c"
EXPORT_C_A="$ROOT/third_party/freeglut/build/lib/libglut.a"

# export_c_source — drive Ctrl+S on EXPORT_C_SCENE and echo the standalone C
# it writes. Save Scene on an example (no workspace bound) targets ./output.c,
# so the app runs in its own cwd. FREEGLUT_CAPTURE_FRAMES is just the exit
# path: the keystroke lands on frame 1 and the frames are thrown away.
export_c_source() {
    rm -rf "$EXPORT_C_DIR"; mkdir -p "$EXPORT_C_DIR"
    ( cd "$EXPORT_C_DIR" &&
      GLR_NO_SPLASH=1 GLR_TYPE_KEYS=$'\023' \
      FREEGLUT_CAPTURE_FRAMES=2 FREEGLUT_CAPTURE_FILE="$EXPORT_C_DIR/save" \
          "$BIN" --example "$EXPORT_C_SCENE" --no-audio >/dev/null 2>&1 )
    [[ -f "$EXPORT_C_DIR/output.c" ]] || {
        echo "docs-assets: Ctrl+S wrote no output.c for '$EXPORT_C_SCENE'" >&2
        return 1; }
    echo "$EXPORT_C_DIR/output.c"
}

# export_c_build <out-bin> <src.c> <blades> <field> <WxH> [q-presses] — knob
# values + window size are the only edits. -w because the export targets C89
# against macOS's deprecated-since-10.14 GL headers: hundreds of warnings,
# zero news.
#
# q-presses drives the FIRST @tune knob through the export's own keyboard()
# handler before the main loop, so a still can show a knob that has actually
# been turned rather than one whose initializer we rewrote. The keystrokes
# route through the generated tuning_step() ladder exactly as a held key does
# — glutGetModifiers() outside an input callback warns and reports no
# modifiers, which is the unmodified (x1) step we want anyway.
export_c_build() {
    local bin_out=$1 src=$2 blades=$3 half_width=$4 size=$5 presses=${6:-0}
    local patched="$EXPORT_C_DIR/$(basename "$bin_out").c"
    local libs press_edit=()
    [[ -f "$EXPORT_C_A" ]] || {
        echo "docs-assets: vendored freeglut archive not built: $EXPORT_C_A" >&2
        echo "             build it first: make gl-repl" >&2
        return 1; }
    if [[ "$(uname -s)" == Darwin ]]; then
        libs="-framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo"
    else
        libs="-lGL -lGLU -lX11"
    fi
    [[ "$presses" == 0 ]] || press_edit=(-e \
        "s/  glutTimerFunc(16, tick, 0);/  { int i; for (i = 0; i < $presses; i++) keyboard('q', 0, 0); }\\
  glutTimerFunc(16, tick, 0);/")
    sed -e "s/^static float bladeCount = .*/static float bladeCount = $blades;/" \
        -e "s/^static float field = .*/static float field = $half_width;/" \
        -e "s/glutInitWindowSize([0-9]*, [0-9]*);/glutInitWindowSize(${size%x*}, ${size#*x});/" \
        ${press_edit[@]:+"${press_edit[@]}"} \
        "$src" > "$patched"
    cc -std=c89 -O2 -w -DGL_SILENCE_DEPRECATION -o "$bin_out" "$patched" \
        -I"$ROOT/third_party/freeglut/include" "$EXPORT_C_A" \
        -lm -lpthread $libs
}

# export_c_frame <bin> <frames> — record <frames>, echo the LAST ppm. The
# export advances t on a 60 Hz glutTimerFunc, so the frame count is also the
# settle: 90 frames ≈ 1.5 s of sway, past the upright t = 0 pose.
#
# Unlike render(), the frame dir is FIXED (recreated per call, dropped with
# $WORK) rather than $RANDOM: callers read this through $( ), and a subshell
# can echo a path back but cannot hand out a variable to clean up later.
export_c_frame() {
    local bin=$1 frames=$2 dir="$WORK/export-c-frames"
    rm -rf "$dir"; mkdir -p "$dir"
    FREEGLUT_CAPTURE_FRAMES=$frames FREEGLUT_CAPTURE_FILE="$dir/f" \
        "$bin" >/dev/null 2>&1
    ls "$dir"/f-*.ppm | tail -1
}

if want export-c-grass || want export-c-knobs; then
    EXPORT_C_SRC="$(export_c_source)"
fi

# 9600 blades over a wider field. In the REPL this scene is ALREADY at the
# ceiling — 135 blades flatten to 8113 of 8192 commands (60 per blade), and
# 137 blades flatten to nothing at all. The exported `for` loop has no such
# budget, which is the entire point of the shot. 9600 (not 10000) keeps the
# HUD's %.4g on a plain integer instead of 1e+04.
#
# The export has no accum-AA hook, so this one supersamples the old-fashioned
# way: render at 1.5x and let the downscale do the antialiasing. 1800x1200 is
# also the largest 3:2 window that fits on a 2560x1440 display — a native
# window is clamped to the visible screen and comes back the wrong size.
if want export-c-grass; then
    export_c_build "$EXPORT_C_DIR/grass-dense" "$EXPORT_C_SRC" 9600 4.5 1800x1200
    write_png "$(export_c_frame "$EXPORT_C_DIR/grass-dense" 90)" \
        "$OUT/export-c-grass.png" -resize ${W}x${H}
    echo "docs-assets: wrote $OUT/export-c-grass.png"
fi

# The first @tune knob, before and after. Both panels are the SAME binary at
# the same exported bladeCount = 135; the right one has had `q` pressed 177
# times through the export's own keyboard handler (see export_c_build). The
# tuning_step ladder puts that at exactly 1200: 5 per press up to 1000, then
# 50 per press above it. The generated HUD labels both panels for us, so this
# montage needs no font.
#
# Captured 1:1 at WxH and NOT downscaled: the HUD is GLUT_BITMAP_9_BY_15, and
# any resample turns 9px glyphs to mush. Crop is the left half at full height
# — the HUD is anchored to the top edge, the grass to the bottom.
if want export-c-knobs; then
    for presses in 0 177; do
        export_c_build "$EXPORT_C_DIR/grass-q$presses" "$EXPORT_C_SRC" \
            135 3.65 ${W}x${H} "$presses"
        magick "$(export_c_frame "$EXPORT_C_DIR/grass-q$presses" 90)" \
            -crop $((W / 2))x${H}+0+0 +repage "$WORK/knob-q$presses.png"
    done
    montage1x2 "$WORK/knobs.png" "$WORK/knob-q0.png" "$WORK/knob-q177.png"
    write_png "$WORK/knobs.png" "$OUT/export-c-knobs.png"
    echo "docs-assets: wrote $OUT/export-c-knobs.png"
fi

if want motion-blur; then
    # The blur itself is the scene's @cfg accum effect; no extra AA passes.
    # The status bar's "Blur 16x" indicator is part of the shot. WARM=12
    # captures just past startup, while t is small.
    ( WARM=24
      still "$OUT/motion-blur.png" 0 "$(stage_blur)" )
fi

# Subshells: WARM and the exported GLR_* capture hooks are set inside ( ) so
# they stay scoped to one capture — explicit, and safe even under shells
# (POSIX mode) where a VAR=x prefix on a function call persists after it.

# Transform guides, one tile per guide kind (see stage_xform_guide). Each tile
# is a code strip above a scene strip at NATIVE resolution, same construction as
# vertex-guides: the program that produced the guide rides in the image with the
# cursor row highlighted, so the montage needs no font-drawn captions and the
# User Guide never has to quote a copy of the code that can drift from it.
if want xform-guide-montage; then
    xg_tiles=()
    for kind in translate rotate scale scale-origin; do
        ( export GLR_NO_SPLASH=1 GLR_EDIT_LINE=4
          still "$WORK/xg-$kind.png" 16 "$(stage_xform_guide "$kind")" )
        magick "$WORK/xg-$kind.png" -crop "$XG_CODE_CROP" +repage \
            "$WORK/xg-c-$kind.png"
        magick "$WORK/xg-$kind.png" -crop "$XG_SCENE_CROP" +repage \
            "$WORK/xg-s-$kind.png"
        magick "$WORK/xg-c-$kind.png" "$WORK/xg-s-$kind.png" -append \
            -background black "$WORK/xg-t-$kind.png"
        xg_tiles+=("$WORK/xg-t-$kind.png")
    done
    montage2x2 "$WORK/xg-2x2.png" "${xg_tiles[@]}"
    write_png "$WORK/xg-2x2.png" "$OUT/xform-guide-montage.png"
    echo "docs-assets: wrote $OUT/xform-guide-montage.png"
fi

# Guide mode: the SAME program and cursor twice, differing only in the
# transform_guides cfg, stacked so the two anchors line up vertically. One
# shared code strip (the two captures render an identical code panel), and the
# widths are picked to match montage2x1's output exactly -- 800 + 2*2 both ways
# -- so the vertical append needs no padding.
if want xform-guide-mode; then
    ( export GLR_NO_SPLASH=1 GLR_EDIT_LINE=7
      still "$WORK/xgm-world.png" 16 "$(stage_xform_guide_mode 1)"
      still "$WORK/xgm-frame.png" 16 "$(stage_xform_guide_mode 2)" )
    magick "$WORK/xgm-world.png" -crop "$XM_CODE_CROP" +repage \
        -bordercolor black -border 2 "$WORK/xgm-code.png"
    magick "$WORK/xgm-world.png" -crop "$XM_SCENE_CROP" +repage "$WORK/xgm-a.png"
    magick "$WORK/xgm-frame.png" -crop "$XM_SCENE_CROP" +repage "$WORK/xgm-b.png"
    montage2x1 "$WORK/xgm-pair.png" "$WORK/xgm-a.png" "$WORK/xgm-b.png"
    magick "$WORK/xgm-code.png" "$WORK/xgm-pair.png" -append \
        -background black "$WORK/xgm-all.png"
    write_png "$WORK/xgm-all.png" "$OUT/xform-guide-mode.png"
    echo "docs-assets: wrote $OUT/xform-guide-mode.png"
fi

# Mid-typing states: GLR_TYPE_KEYS feeds the partial line through the real
# keyboard dispatch after load (see stage_vertex_entry), so the guides pose
# without anyone touching a keyboard. One 2x2 montage walks the whole
# degrees-of-freedom ladder in reading order:
#
#   one coordinate typed  -> 2-DOF graph-paper sheet   | two -> 1-DOF tick line
#   all three             -> 0-DOF point marker        | next vertex -> sheet again
#
# The fourth tile is the one that needs the semicolon: it commits the first
# vertex and starts a second, showing the ladder restart against geometry that
# is already in the block. Each tile is a code strip (rows 4..6, so the typed
# text that produced the guide rides in the image) above a scene strip, both
# $VG_* crops, montaged at NATIVE resolution -- the typed line is the caption,
# and a 2x downscale would blur it. GLR_NO_SPLASH keeps the startup wordmark
# out of the scene pane; WARM_PLAIN is far short of the ~186-frame fade.
if want vertex-guides; then
    vg_keys=(
        'glVertex3f(1.2'
        'glVertex3f(1.2, 0.8'
        'glVertex3f(1.2, 0.8, 0)'
        'glVertex3f(1.2, 0.8, 0);glVertex3f(,1'
    )
    vg_tiles=()
    for i in 0 1 2 3; do
        ( export GLR_NO_SPLASH=1 GLR_TYPE_KEYS="${vg_keys[$i]}"
          still "$WORK/vg-$i.png" 16 "$(stage_vertex_entry)" )
        magick "$WORK/vg-$i.png" -crop "$VG_CODE_CROP" +repage "$WORK/vg-c$i.png"
        magick "$WORK/vg-$i.png" -crop "$VG_SCENE_CROP" +repage "$WORK/vg-s$i.png"
        magick "$WORK/vg-c$i.png" "$WORK/vg-s$i.png" -append \
            -background black "$WORK/vg-t$i.png"
        vg_tiles+=("$WORK/vg-t$i.png")
    done
    montage2x2 "$WORK/vg-2x2.png" "${vg_tiles[@]}"
    write_png "$WORK/vg-2x2.png" "$OUT/vertex-guides.png"
    echo "docs-assets: wrote $OUT/vertex-guides.png"
fi

# Autocomplete: the popup + inline ghost exist only mid-typing, so
# GLR_TYPE_KEYS poses them (same trick as the vertex-entry guides). Cropped to
# the code panel: the popup hangs off the typed line and the scene behind it
# has nothing to do with completing an enum.
if want autocomplete; then
    ( export GLR_TYPE_KEYS='glEnable(GL_LI'
      still "$WORK/ac-full.png" 16 "$(stage_autocomplete)" )
    write_png "$WORK/ac-full.png" "$OUT/autocomplete.png" \
        -crop "$AC_CROP" +repage
    echo "docs-assets: wrote $OUT/autocomplete.png"
fi

# Color picker: opened via the GLR_OPEN_COLOR_PICKER capture hook (the
# picker otherwise needs a swatch click). Line 7 = the glColor3f.
if want color-picker; then
    ( export GLR_EDIT_LINE=8 GLR_OPEN_COLOR_PICKER=8
      still "$OUT/color-picker.png" 16 "$(stage_color_picker)" )
fi

# Numeric stepper: parking the cursor on the decl line loads it with the
# cursor at end-of-line — inside the initializer's number — so the
# stepper shows at the panel's right edge. Cropped to the code-panel top:
# the widget is 16px, a full-window shot would reduce it to a speck.
if want numeric-stepper; then
    ( export GLR_EDIT_LINE=1
      still "$WORK/numeric-stepper-full.png" 16 "$(stage_stepper)" )
    write_png "$WORK/numeric-stepper-full.png" "$OUT/numeric-stepper.png" \
        -crop 1200x110+0+28 +repage
    echo "docs-assets: wrote $OUT/numeric-stepper.png"
fi

# Open the state inspector on the committed blank row following the staged
# state changes. Cropped to the popup plus the source rows that wrote the state
# it lists -- and far enough down to keep the teapot those rows are drawing,
# since the table describes the GL that renders it.
if want gl-state-inspector; then
    ( export GLR_EDIT_LINE=10 GLR_OPEN_GL_STATE=10
      still "$WORK/gls-full.png" 16 "$(stage_gl_state_inspector)" )
    write_png "$WORK/gls-full.png" "$OUT/gl-state-inspector.png" \
        -crop "$GLS_CROP" +repage
    echo "docs-assets: wrote $OUT/gl-state-inspector.png"
fi

# Right-click an assignment row to plot its values. The asset is a 1x2 montage
# of the panel on its two X axes -- a loop row (exec index within one frame)
# beside a top-level row (successive captures) -- because the only thing that
# differs is the axis caption and the trace under it; two full-window stills
# would be two near-identical 1200x800 pages of mostly grid.
#
# Left tile: line 6 is the `wave = ...` assignment inside the loop (the code
# panel's own numbering, counted after the @cfg headers and the snippet markers
# are stripped, same as GLR_EDIT_LINE). WARM outlasts the splash (WARM_SPLASH)
# rather than taking the house-default 30: this panel lands bottom-right,
# exactly where the splash strip dims the frame, and the min/max/mean/sd rows
# the shot exists to show are unreadable underneath it.
#
# Right tile: the frames axis needs several capture *instants*, not one frame:
# the 1 Hz capture rate is gated on wall clock, not on frame count, so what it
# needs is ~20 s of elapsed time. WARM is therefore a frame budget standing in
# for a duration — which means, like profile-panels, the sample count in the
# shot reflects the machine that generated it. Regenerate on an otherwise idle
# one, and expect n= to move. 600 frames also clears the splash.
# Line 7 is the `angle = ...` row — the four glEnable rows above it are part
# of the scene.
if want assign-plot; then
    ( WARM=$((WARM_SPLASH + 30))
      export GLR_EDIT_LINE=6 GLR_OPEN_ASSIGN_PLOT=6
      still "$WORK/ap-exec-full.png" 16 "$(stage_assign_plot)" )
    ( WARM=600
      export GLR_EDIT_LINE=7 GLR_OPEN_ASSIGN_PLOT=7
      still "$WORK/ap-frames-full.png" 16 "$(stage_assign_plot_frames)" )
    write_png "$WORK/ap-exec-full.png" "$WORK/ap-exec.png" \
        -crop "$AP_CROP" +repage
    write_png "$WORK/ap-frames-full.png" "$WORK/ap-frames.png" \
        -crop "$AP_CROP" +repage
    montage1x2 "$WORK/ap-pair.png" "$WORK/ap-exec.png" "$WORK/ap-frames.png"
    write_png "$WORK/ap-pair.png" "$OUT/assign-plot.png"
    echo "docs-assets: wrote $OUT/assign-plot.png"
fi

# Several rows on one plot: Shift+right-click adds a series, and the hook's
# comma list is that same add path. Line 8 (`wave = ...`) leads, so it owns the
# X axis; 6 and 7 are its two components. Cropped like the pair above, one row
# taller because the legend band only exists past one series -- and wider,
# since the legend spreads the three names across the panel.
if want assign-plot-series; then
    ( WARM=$((WARM_SPLASH + 30))
      export GLR_EDIT_LINE=8 GLR_OPEN_ASSIGN_PLOT=8,6,7
      still "$WORK/aps-full.png" 16 "$(stage_assign_plot_series)" )
    write_png "$WORK/aps-full.png" "$OUT/assign-plot-series.png" \
        -crop "$APS_CROP" +repage
    echo "docs-assets: wrote $OUT/assign-plot-series.png"
fi

# lin | log on the same two traces, side by side -- the claim the prose used to
# make in three paragraphs. GLR_ASSIGN_PLOT_LOG flips the chip that is
# otherwise mouse-only. Lines 6 and 7 are `big` and `small`; `big` leads.
if want assign-plot-log; then
    for ap_log in 0 1; do
        ( WARM=$((WARM_SPLASH + 30))
          export GLR_EDIT_LINE=6 GLR_OPEN_ASSIGN_PLOT=6,7
          [ "$ap_log" = 1 ] && export GLR_ASSIGN_PLOT_LOG=1
          still "$WORK/apl-$ap_log-full.png" 16 "$(stage_assign_plot_log)" )
        write_png "$WORK/apl-$ap_log-full.png" "$WORK/apl-$ap_log.png" \
            -crop "$APS_CROP" +repage
    done
    montage1x2 "$WORK/apl-pair.png" "$WORK/apl-0.png" "$WORK/apl-1.png"
    write_png "$WORK/apl-pair.png" "$OUT/assign-plot-log.png"
    echo "docs-assets: wrote $OUT/assign-plot-log.png"
fi

# Profile panels reflect live performance, so generate this on an otherwise
# unloaded machine for representative output. Run normally while the FPS
# history and per-section histograms fill, then take one SIGUSR1 snapshot.
# Capturing every warmup frame would make the FPS plot measure PPM
# readback/write throughput instead.
if want profile-panels; then
    ( WARM=720
      one_shot_still "$OUT/profile-panels.png" "$(stage_profile)" )
fi

if want clip-plane; then
    ( export GLR_EDIT_LINE=6
      still "$OUT/clip-plane.png" 16 "$(stage_clip_plane)" )
fi

if want clip-plane-sweep; then
    ( export GLR_EDIT_LINE=6
      clip "$OUT/clip-plane-sweep" 126 2 20 560 "$(stage_clip_sweep)" )
fi

if want xform-guide; then
    ( export GLR_EDIT_LINE=6
      clip "$OUT/xform-guide" 120 1 20 720 "$(stage_guide)" )
fi

if want replay; then
    # Replay advances a few commands per second; 500 frames subsampled
    # 3x shows the whole pinwheel assembling at a watchable pace.
    clip "$OUT/replay" 575 3 24 720 "$(stage_replay)"
fi

if want animated-ring; then
    clip "$OUT/animated-ring" 126 2 18 560 --example "$EX_RING"
fi

# ---- showcase assets (docs/images/showcase/) ----------------------------
#
# The SHOWCASE gallery shows each scene WITH its code panel (the whole point:
# "the geometry lives in the code"), so these use the default full-UI render.
# Animated examples become GIFs; static ones become stills.

# Featured.
if want sc-torus-knot; then
    clip "$SHOW/torus-knot" 200 2 20 720 --example "Torus knot"
fi
if want sc-snowfall; then
    clip "$SHOW/snowfall" 220 2 22 720 --example "Snowfall particles"
fi
if want sc-parametric-torus; then
    # Static geometry (nested for, no t) — a still, not a frozen GIF.
    still "$SHOW/parametric-torus.png" 16 \
        --example "Parametric torus (nested for)"
fi
if want sc-recursive-tree; then
    clip "$SHOW/recursive-tree" 200 2 20 720 \
        --example "3D tree (func + recursion)"
fi

# Curves & line art.
if want sc-spirograph; then
    clip "$SHOW/spirograph" 200 2 20 560 --example "Spirograph curve"
fi
if want sc-ripple-ring; then
    clip "$SHOW/ripple-ring" 200 2 20 560 --example "Traveling ripple ring"
fi
if want sc-bezier; then
    still "$SHOW/bezier.png" 16 --example "Bezier curve with guides"
fi
if want sc-bubble-sort; then
    ( WARM=20
      clip "$SHOW/bubble-sort" 160 2 20 560 \
          --example "Bubble sort (scratch arrays)" )
fi

if want sc-orbit-plot; then
    still "$SHOW/orbit-plot.png" 16 \
        --example "Annotated orbit plot (labels)"
fi

if want sc-wave-surface; then
    clip "$SHOW/wave-surface" 200 2 20 560 \
        --example "Wave surface (analytic normals)"
fi

# Surfaces. (The old "Procedural terrain" example is gone; the new "Ringed
# planet" stands in for the third surface tile.)
if want sc-ringed-planet; then
    clip "$SHOW/ringed-planet" 200 2 20 560 --example "Ringed planet (nebula skies)"
fi
if want sc-gl-repl-logo; then
    (
    WARM=60
    still "$SHOW/gl-repl-logo.png" 16 --example "gl-repl logo"
    )
fi

# Particles & effects.
if want sc-grass; then
    clip "$SHOW/grass" 200 2 20 560 --example "Swaying grass field (rand + t)"
fi
if want sc-jellyfish; then
    clip "$SHOW/jellyfish" 220 2 22 560 --example "Jellyfish (glDepthMask translucency)"
fi

# Functions, branching & recursion.
if want sc-function-demo; then
    still "$SHOW/function-demo.png" 16 --example "Function demo (named func)"
fi
if want sc-function-polygons; then
    still "$SHOW/function-polygons.png" 16 \
        --example "Function polygons (args + for)"
fi
if want sc-conditional-colors; then
    clip "$SHOW/conditional-colors" 200 2 20 560 --example "Conditional colors (if + t)"
fi
if want sc-sierpinski-carpet; then
    clip "$SHOW/sierpinski-carpet" 200 2 20 560 \
        --example "Sierpinski carpet (2D recursion)"
fi
if want sc-sierpinski-sponge; then
    clip "$SHOW/sierpinski-sponge" 200 2 20 560 \
        --example "Sierpinski sponge (3D recursion)"
fi

# Big scenes.
if want sc-whale; then
    clip "$SHOW/whale" 240 2 22 560 --example "Whale (particle system + lit model)"
fi
if want sc-stress-test; then
    clip "$SHOW/stress-test" 240 2 22 560 \
        --example "Dusk lighthouse atoll (stress test)"
fi
if want sc-lantern-festival; then
    # The lanterns rise on a 15-unit wrap, so a 240-frame clip catches the
    # flock mid-ascent with reflections and glints already running.
    clip "$SHOW/lantern-festival" 240 2 22 560 \
        --example "Lantern festival (additive glow + reflections)"
fi
if want sc-aurora-observatory; then
    # Post-warm clip spans t in [3, 6.3]: one coral beacon blink (period
    # ~3.9 s) rides mid-clip while the dish drifts and the pulses cycle.
    clip "$SHOW/aurora-observatory" 200 2 20 560 \
        --example "Aurora observatory (dish tracks the sky)"
fi

# Transforms & GL state.
if want sc-planar-shadows; then
    ( export GLR_EDIT_LINE=3
    clip "$SHOW/planar-shadows" 200 2 20 560 \
        --example "Planar shadows (glMultMatrixf)" )
fi
if want sc-fog-ring-tunnel; then
    clip "$SHOW/fog-ring-tunnel" 200 2 20 560 \
        --example "Fog ring tunnel"
fi
if want sc-pulse-bars; then
    clip "$SHOW/pulse-bars" 200 2 20 560 \
        --example "Pulse bars (easing)"
fi
if want sc-stencil-mask; then
    clip "$SHOW/stencil-mask" 200 2 20 560 \
        --example "Stencil mask window (glStencilOp)"
fi

# "Beyond the still image" — interactive features. feature-time is a real
# t-driven clip (the narrative "freeze then play" can't be scripted headless,
# but the motion is honest). feature-ply / feature-export-c want an external
# tool (MeshLab / a text editor) we can't drive headlessly, so they reuse a
# representative scene still as a stand-in; the SHOWCASE comments document the
# ideal shot.
if want sc-feature-time; then
    clip "$SHOW/feature-time" 200 2 20 560 --example "Conditional colors (if + t)"
fi
if want sc-feature-ply; then
    # Stand-in: the parametric torus is the canonical --export-ply scene.
    still "$SHOW/feature-ply.png" 16 \
        --example "Parametric torus (nested for)"
fi
if want sc-feature-export-c; then
    # Stand-in: any full-UI shot shows "it's all code in the panel".
    still "$SHOW/feature-export-c.png" 16 --example "Function demo (named func)"
fi

# --- Standalone demos (tools/README.md) ------------------------------------
# One still per windowed demo. repl_demo has no entry: it is headless in every
# build, so there is no frame to grab — tools/README.md quotes its --trace
# output instead.
if want demo-render3d; then
    demo_still "$DEMOS/render3d.png" render3d_demo 60
fi
if want demo-repl-live; then
    # Scene passed as argv (bypassing the INI): the whale is the bundled scene
    # with the most declared variables, so the slider panel — the thing this
    # demo composes onto the pipeline — is full rather than showing just `t`.
    demo_still "$DEMOS/repl-live.png" repl_live_demo 90 \
        "$ROOT/tools/repl_live_demo/scenes/whale-full-c.c"
fi
if want demo-editor; then
    # Keep every line inside the panel width: this demo's text_panel clips
    # long rows rather than wrapping them, and a screenshot should not show
    # a sentence running off the edge.
    cat > "$WORK/editor-demo.txt" <<'EOF'
editor_demo -- generic plain-text editing on src/editor.

No REPL, no app shell. This window is EditorState plus
ui/core/text_panel, driven by the demo's own dispatcher
in tools/editor_demo/input.c.

Type to insert. Enter splits the row at the cursor;
backspace at column 0 merges it back into the line above.
Arrows, Home and End move within the row. Up and Down
commit the input row and load the neighbouring one.

The File menu above is the demo's own, not the app's
menu bar. Ctrl+F opens the find bar. Esc quits.
EOF
    ( export GLR_DEMO_TEXT="$WORK/editor-demo.txt"
    # Event-driven demo: exactly one frame, or the capture never terminates.
    demo_still "$DEMOS/editor.png" editor_demo 1 )
fi
if want demo-variable-panel; then
    demo_still "$DEMOS/variable-panel.png" variable_panel_demo 60
fi
if want demo-color-picker; then
    # Open on the torus (index 2): the picker anchors at the left edge (the
    # demo's host bridge reports no code panel), so an active shape further
    # right stays visible next to the popup that is editing it.
    ( export GLR_DEMO_OPEN_PICKER=2
    demo_still "$DEMOS/color-picker.png" color_picker_demo 1 )
fi
if want demo-assign-plot; then
    # All three rows plotted (wave leads, so it owns the X axis) with the
    # program counter parked mid-loop: the shot has to show the PC rules and
    # their value readouts, which is the half of this panel the full app can
    # only reach under a running replay. GLR_DEMO_PC also pauses, so the frame
    # grabbed is the frame posed. 30 frames at the FRAME capture rate fills the
    # statistics with more than one pass.
    ( export GLR_DEMO_PLOT_ROWS=2,0,1 GLR_DEMO_PC=76
    demo_still "$DEMOS/assign-plot.png" assign_plot_demo 30 )
fi
if want demo-cpuprof; then
    # Live cadence, not record mode: the panel's content IS per-frame timing,
    # and record mode's per-frame readback would be measured as app time.
    demo_one_shot_still "$DEMOS/cpuprof.png" cpuprof_demo 5
fi
if want demo-memprof; then
    # 12 s: memprof pushes a ring sample every ~5 s and defers its baseline to
    # the first push, so this lands after two samples with the staged blocks
    # allocated between them — the panel shows a real init/delta pair and the
    # graph a step. Much later and macOS's memory compressor reclaims the
    # untouched blocks, walking the delta back toward zero.
    ( export GLR_DEMO_ALLOC=12
    demo_one_shot_still "$DEMOS/memprof.png" memprof_demo 12 )
fi

echo "docs-assets: done."
