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
# ffmpeg. Override the binary with BIN=<path>.
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
OUT="${OUT:-$ROOT/docs/images}"
SHOW="$OUT/showcase"

W=1200          # target width/height of every asset's source window
H=800
GIF_FUZZ=4%     # magick -layers Optimize fuzz for GIF delta compression

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

# Format categories use public asset names, not output filenames, so they flow
# through the same want()/parallel dispatch as explicitly named assets. Put
# every asset in exactly one format category.
GIF_ASSETS=(
    view-mode-2d clip-plane-sweep xform-guide replay animated-ring
    sc-torus-knot sc-snowfall sc-recursive-tree sc-spirograph sc-ripple-ring
    sc-bubble-sort sc-wave-surface sc-ringed-planet sc-grass sc-jellyfish
    sc-conditional-colors sc-sierpinski-carpet sc-sierpinski-sponge
    sc-whale sc-stress-test sc-aurora-observatory
    sc-planar-shadows sc-fog-ring-tunnel sc-pulse-bars sc-stencil-mask
    sc-feature-time
)

PNG_ASSETS=(
    hero first-triangle window-tour vertex-overlays wireframe-hidden-line
    winding-view depth-view light-theme-studio grid-themes backdrops axes-compass
    labels-orrery glu-tess glow-sprites transform-stress variable-panel
    tune-badges motion-blur xform-guide-still single-polygon-scope
    vertex-guide-plane vertex-guide-line clip-plane autocomplete color-picker
    numeric-stepper gl-state-inspector profile-panels
    sc-parametric-torus sc-bezier sc-orbit-plot sc-gl-repl-logo sc-function-demo
    sc-function-polygons sc-feature-ply sc-feature-export-c
)

ALL_ASSETS=("${GIF_ASSETS[@]}" "${PNG_ASSETS[@]}")

usage() {
    cat <<'EOF'
Usage: scripts/docs-assets.sh [options] [asset ...]

Regenerate documentation screenshots and animations. With no asset names or
format categories, all assets are regenerated. Named assets and categories may
be combined.

GIFs and profile-panels reflect live rendering performance. Regenerate them on
an otherwise unloaded machine for representative documentation output.

Options:
  --gifs             Regenerate all GIF assets.
  --pngs             Regenerate all PNG assets.
  --list             List selected asset names and exit (all by default).
  -j, --jobs N       Regenerate up to N assets in parallel (default: 1).
  -h, --help         Show this help and exit.

Environment:
  BIN=<path>         gl-repl binary (default: build/release/gl-repl).
  OUT=<path>         output directory (default: docs/images).
  WARM=<frames>      warm-up frame override for captures that don't set their
                     own (see the WARM_* block in the script).

Examples:
  scripts/docs-assets.sh --gifs
  scripts/docs-assets.sh --pngs -j 4
  scripts/docs-assets.sh --list --gifs
  scripts/docs-assets.sh hero replay
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
LIST=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --list) LIST=1; shift ;;
        --gifs) SELECT_GIFS=1; shift ;;
        --pngs) SELECT_PNGS=1; shift ;;
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
if [[ ${#ARGS[@]} -gt 0 ]]; then
    WANTED+=("${ARGS[@]}")
fi
if [[ ${#WANTED[@]} -eq 0 ]]; then
    WANTED=("${ALL_ASSETS[@]}")
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

for tool in magick ffmpeg; do
    command -v "$tool" >/dev/null || {
        echo "docs-assets: '$tool' not found" >&2; exit 1; }
done
[[ -x "$BIN" ]] || {
    echo "docs-assets: gl-repl binary not found at '$BIN'" >&2
    echo "             build it first: make gl-repl" >&2
    exit 1; }

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
mkdir -p "$OUT" "$SHOW"

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

# gif <out.gif> <frames> <step> <fps> <width> <args...> — record, take
# every <step>th frame, assemble a palette-optimized looping GIF. The kept
# frame count (frames/step) and fps set the clip length.
#
# --example clips additionally render WARM_EXAMPLE extra leading frames and discard
# them (the camera-ease warmup, see WARM_EXAMPLE), so the clip starts settled while
# keeping the exact same frames/step at the same fps — identical length, just
# past the settle. Staged scenes have no load ease, and replay's draw-by-draw
# assembly IS the content, so a clip with no --example keeps frame 0. An
# explicit WARM at the call site overrides both defaults.
gif() {
    local out=$1 frames=$2 step=$3 fps=$4 width=$5; shift 5
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
    ffmpeg -y -framerate "$fps" -i "$WORK/sub/g-%04d.ppm" \
        -vf "scale=$width:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer" \
        -loop 0 "$WORK/raw.gif" >/dev/null 2>&1
    magick "$WORK/raw.gif" -fuzz "$GIF_FUZZ" -layers Optimize "$out"
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
/* @cfg vertex_labels = 2 */
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
# the second quad's second vertex (line 10) so Single polygon scope narrows
# the highlight/labels to just that quad, not the whole shared block.
stage_single_polygon() { stage single_polygon <<'EOF'
/* @cfg poly_highlight = 1 */
/* @cfg vertex_labels = 1 */
/* @cfg label_highlight_scope = OVERLAY_SCOPE_SINGLE_POLYGON */
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


stage_lights() { stage lights <<'EOF'
/* @cfg light_theme = LIGHT_THEME_DEFAULT */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg light_indicators = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
glTranslatef(0.0000f, 0.0000f, -7.0409f);
glRotatef(15.5000f, 1.0f, 0.0f, 0.0f);
glRotatef(-118.0000f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0000f, -0.0000f, -0.0000f);
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

# Cursor parks on line 5 (glTranslatef) via GLR_EDIT_LINE.
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

# Vertex entry guides: the snippet ends on an OPEN glBegin block, so the
# cursor lands on the in-block append row after load, and GLR_TYPE_KEYS
# (exported at the call site) types a partial glVertex3f( there — the
# only way to pose the 2-DOF sheet / 1-DOF line guides, which exist only
# mid-typing (a committed line always has all three coordinates).
stage_vertex_entry() { stage vertex_entry <<'EOF'
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_XZRULER */
// Snippet start
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST);
glColor3f(1, 0.35, 0.2);
glBegin(GL_TRIANGLES);
glVertex3f(-1, 0, 0);
glVertex3f(1, 0, 0);
glVertex3f(0, 1.4, 0);
// Snippet end
EOF
}

# Clip plane: cursor parks on the glClipPlane line (GLR_EDIT_LINE=5 at the
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

# OpenGL state inspector: the blank row after the authored state changes is
# line 9 after @cfg headers/snippet markers are stripped and the authored
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
# real animated-wave example as the workload, with asset-specific presentation
# config prepended to it. Stock AA: raising the accum passes would multiply
# every section's cost 16x and distort the numbers the panel is there to show.
stage_profile() {
    stage_scene profile \
        "$ROOT/examples/scenes/animated-wave-surface-analytic-normals.glr" <<'EOF'
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
stage_window_tour_dir() {
    local ws="$WORK/tour-ws"
    mkdir -p "$ws"
    cat > "$ws/triangle.c" <<'EOF'
// @scene-name First Triangle
// Snippet start
float lift = 1;
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(1, 0.85, 0.15);
glBegin(GL_TRIANGLES);
glVertex3f(0, lift, 0);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glEnd();
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

EX_HERO="Teapot carousel (transform stacks + glow points)"
EX_ORRERY="Orrery (labels track 3D orbits)"
EX_GLU="GLU concave arrow"
EX_GLOW="Glow sprites (blend + point attenuation)"
EX_XFORM="Transform stress (translate/rotate/scale guides)"
EX_RING="Animated ring (for + t)"

# ---- core assets --------------------------------------------------------

if want hero; then
    still "$OUT/hero.png" 16 --example "$EX_HERO" --time 3
fi

if want first-triangle; then
    still "$OUT/first-triangle.png" 16 "$(stage_triangle)"
fi

if want vertex-overlays; then
    still "$OUT/vertex-overlays.png" 16 "$(stage_overlays)"
fi

if want single-polygon-scope; then
    ( export GLR_EDIT_LINE=10
      still "$OUT/single-polygon-scope.png" 16 "$(stage_single_polygon)" )
fi

if want wireframe-hidden-line; then
    still "$WORK/wireframe.png" 16 "$(stage_wireframe)"
    still "$WORK/hidden-line.png" 16 "$(stage_hidden_line)"
    montage1x2 "$WORK/wf-hl.png" "$WORK/wireframe.png" "$WORK/hidden-line.png"
    write_png "$WORK/wf-hl.png" "$OUT/wireframe-hidden-line.png" -resize "$W"
    echo "docs-assets: wrote $OUT/wireframe-hidden-line.png"
fi

if want winding-view; then
    still "$OUT/winding-view.png" 16 "$(stage_winding)"
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

# WARM=200: past the ~186-frame splash fade, so the wordmark doesn't
# occlude the variable panel this shot is naming.
if want window-tour; then
    ( WARM=200
      still "$OUT/window-tour.png" 16 "$(stage_window_tour_dir)" )
fi

if want light-theme-studio; then
    still "$OUT/light-theme-studio.png" 16 "$(stage_lights)"
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
    # A GIF that toggles View mode (Ctrl+Shift+V) on the animated wave surface,
    # so the doc shows the 3D->2D->3D transition rather than a frozen 2D still.
    # GLR_VIEW_TOGGLE_AT fires on gl_repl's rendered-frame clock (t = frame/60),
    # which counts the WARM_EXAMPLE leading frames gif() renders and discards for
    # --example clips. WARM_EXAMPLE=180 => 3.0s of that clock elapses before the kept
    # clip begins, so a toggle at clock second S lands at (S*60 - WARM_EXAMPLE)/(step)
    # kept frames in, i.e. output time (S*60 - WARM_EXAMPLE)/(step*fps) s. With
    # step=2 fps=20: clock 4s -> 1.5s into the clip (3D->2D), clock 6s -> 4.5s
    # (2D->3D). The 6s clip then holds ~1.5s of 3D before it loops.
    ( export GLR_VIEW_TOGGLE_AT=4,6
      gif "$OUT/view-mode-2d.gif" 240 2 20 560 \
          --example "Animated wave surface (analytic normals)" )
fi

if want labels-orrery; then
    still "$OUT/labels-orrery.png" 16 --example "$EX_ORRERY" --time 4
fi

if want glu-tess; then
    still "$OUT/glu-tess.png" 16 --example "$EX_GLU"
fi

if want glow-sprites; then
    (
    WARM=12
    still "$OUT/glow-sprites.png" 16 --example "$EX_GLOW" --time 2
    )
fi

if want transform-stress; then
    still "$OUT/transform-stress.png" 16 --example "$EX_XFORM" --time 1
fi

if want variable-panel || want tune-badges; then
    (
    WARM=$WARM_SPLASH
    still "$WORK/variable-panel.png" 16 "$(stage_tune)"
    if want variable-panel; then
        cp "$WORK/variable-panel.png" "$OUT/variable-panel.png"
        echo "docs-assets: wrote $OUT/variable-panel.png"
    fi
    if want tune-badges; then
        magick "$WORK/variable-panel.png" -crop 265x122+935+653 +repage \
            "$OUT/tune-badges.png"
        echo "docs-assets: wrote $OUT/tune-badges.png"
    fi
    )
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
if want xform-guide-still; then
    ( export GLR_EDIT_LINE=5
      still "$OUT/xform-guide-still.png" 16 "$(stage_guide)" )
fi

# Mid-typing states: GLR_TYPE_KEYS feeds the partial line through the
# real keyboard dispatch after load (see stage_vertex_entry). One typed
# coordinate -> the 2-DOF graph-paper sheet; two -> the 1-DOF tick line.
if want vertex-guide-plane; then
    ( export GLR_TYPE_KEYS='glVertex3f(1.2'
      still "$OUT/vertex-guide-plane.png" 16 "$(stage_vertex_entry)" )
fi

if want vertex-guide-line; then
    ( export GLR_TYPE_KEYS='glVertex3f(1.2, 0.8'
      still "$OUT/vertex-guide-line.png" 16 "$(stage_vertex_entry)" )
fi

# Autocomplete: the popup + inline ghost exist only mid-typing, so
# GLR_TYPE_KEYS poses them (same trick as the vertex-entry guides). The
# full window keeps the popup in context beside the scene.
if want autocomplete; then
    ( export GLR_TYPE_KEYS='glEnable(GL_LI'
      still "$OUT/autocomplete.png" 16 "$(stage_autocomplete)" )
fi

# Color picker: opened via the GLR_OPEN_COLOR_PICKER capture hook (the
# picker otherwise needs a swatch click). Line 7 = the glColor3f.
if want color-picker; then
    ( export GLR_EDIT_LINE=7 GLR_OPEN_COLOR_PICKER=7
      still "$OUT/color-picker.png" 16 "$(stage_color_picker)" )
fi

# Numeric stepper: parking the cursor on the decl line loads it with the
# cursor at end-of-line — inside the initializer's number — so the
# stepper shows at the panel's right edge. Cropped to the code-panel top:
# the widget is 16px, a full-window shot would reduce it to a speck.
if want numeric-stepper; then
    ( export GLR_EDIT_LINE=0
      still "$WORK/numeric-stepper-full.png" 16 "$(stage_stepper)" )
    write_png "$WORK/numeric-stepper-full.png" "$OUT/numeric-stepper.png" \
        -crop 1200x110+0+28 +repage
    echo "docs-assets: wrote $OUT/numeric-stepper.png"
fi

# Open the state inspector on the committed blank row following the staged
# state changes. The full-window shot keeps both the source boundary and the
# floating comparison table in view.
if want gl-state-inspector; then
    ( export GLR_EDIT_LINE=9 GLR_OPEN_GL_STATE=9
      still "$OUT/gl-state-inspector.png" 16 \
          "$(stage_gl_state_inspector)" )
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
    ( export GLR_EDIT_LINE=5
      still "$OUT/clip-plane.png" 16 "$(stage_clip_plane)" )
fi

if want clip-plane-sweep; then
    ( export GLR_EDIT_LINE=5
      gif "$OUT/clip-plane-sweep.gif" 126 1 20 720 "$(stage_clip_sweep)" )
fi

if want xform-guide; then
    ( export GLR_EDIT_LINE=5
      gif "$OUT/xform-guide.gif" 120 1 20 720 "$(stage_guide)" )
fi

if want replay; then
    # Replay advances a few commands per second; 500 frames subsampled
    # 3x shows the whole pinwheel assembling at a watchable pace.
    gif "$OUT/replay.gif" 500 3 24 720 "$(stage_replay)"
fi

if want animated-ring; then
    gif "$OUT/animated-ring.gif" 126 2 18 560 --example "$EX_RING"
fi

# ---- showcase assets (docs/images/showcase/) ----------------------------
#
# The SHOWCASE gallery shows each scene WITH its code panel (the whole point:
# "the geometry lives in the code"), so these use the default full-UI render.
# Animated examples become GIFs; static ones become stills.

# Featured.
if want sc-torus-knot; then
    gif "$SHOW/torus-knot.gif" 200 2 20 720 --example "Torus knot (animated)"
fi
if want sc-snowfall; then
    gif "$SHOW/snowfall.gif" 220 2 22 720 --example "Snowfall particles"
fi
if want sc-parametric-torus; then
    # Static geometry (nested for, no t) — a still, not a frozen GIF.
    still "$SHOW/parametric-torus.png" 16 \
        --example "Parametric torus (nested for)"
fi
if want sc-recursive-tree; then
    gif "$SHOW/recursive-tree.gif" 200 2 20 720 \
        --example "Recursive triangle tree (func + recursion)"
fi

# Curves & line art.
if want sc-spirograph; then
    gif "$SHOW/spirograph.gif" 200 2 20 560 --example "Animated spirograph curve"
fi
if want sc-ripple-ring; then
    gif "$SHOW/ripple-ring.gif" 200 2 20 560 --example "Traveling ripple ring"
fi
if want sc-bezier; then
    still "$SHOW/bezier.png" 16 --example "Bezier curve with guides"
fi
if want sc-bubble-sort; then
    ( WARM=20
      gif "$SHOW/bubble-sort.gif" 160 2 20 560 \
          --example "Bubble sort (scratch arrays)" )
fi

if want sc-orbit-plot; then
    still "$SHOW/orbit-plot.png" 16 \
        --example "Annotated orbit plot (labels)"
fi

if want sc-wave-surface; then
    gif "$SHOW/wave-surface.gif" 200 2 20 560 \
        --example "Animated wave surface (analytic normals)"
fi

# Surfaces. (The old "Procedural terrain" example is gone; the new "Ringed
# planet" stands in for the third surface tile.)
if want sc-ringed-planet; then
    gif "$SHOW/ringed-planet.gif" 200 2 20 560 --example "Ringed planet (nebula skies)"
fi
if want sc-gl-repl-logo; then
    (
    WARM=60
    still "$SHOW/gl-repl-logo.png" 16 --example "gl-repl logo"
    )
fi

# Particles & effects.
if want sc-grass; then
    gif "$SHOW/grass.gif" 200 2 20 560 --example "Swaying grass field (rand + t)"
fi
if want sc-jellyfish; then
    gif "$SHOW/jellyfish.gif" 220 2 22 560 --example "Jellyfish (glDepthMask translucency)"
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
    gif "$SHOW/conditional-colors.gif" 200 2 20 560 --example "Conditional colors (if + t)"
fi
if want sc-sierpinski-carpet; then
    gif "$SHOW/sierpinski-carpet.gif" 200 2 20 560 \
        --example "Sierpinski carpet (2D recursion)"
fi
if want sc-sierpinski-sponge; then
    gif "$SHOW/sierpinski-sponge.gif" 200 2 20 560 \
        --example "Sierpinski sponge (3D recursion)"
fi

# Big scenes.
if want sc-whale; then
    gif "$SHOW/whale.gif" 240 2 22 560 --example "Whale (particle system + lit model)"
fi
if want sc-stress-test; then
    gif "$SHOW/stress-test.gif" 240 2 22 560 \
        --example "Dusk lighthouse atoll (stress test)"
fi
if want sc-aurora-observatory; then
    # Post-warm clip spans t in [3, 6.3]: one coral beacon blink (period
    # ~3.9 s) rides mid-clip while the dish drifts and the pulses cycle.
    gif "$SHOW/aurora-observatory.gif" 200 2 20 560 \
        --example "Aurora observatory (dish tracks the sky)"
fi

# Transforms & GL state.
if want sc-planar-shadows; then
    ( export GLR_EDIT_LINE=2
    gif "$SHOW/planar-shadows.gif" 200 2 20 560 \
        --example "Planar shadows (glMultMatrixf)" )
fi
if want sc-fog-ring-tunnel; then
    gif "$SHOW/fog-ring-tunnel.gif" 200 2 20 560 \
        --example "Fog ring tunnel (glFog)"
fi
if want sc-pulse-bars; then
    gif "$SHOW/pulse-bars.gif" 200 2 20 560 \
        --example "Pulse bars (easing)"
fi
if want sc-stencil-mask; then
    gif "$SHOW/stencil-mask.gif" 200 2 20 560 \
        --example "Stencil mask window (glStencilOp)"
fi

# "Beyond the still image" — interactive features. feature-time is a real
# t-driven clip (the narrative "freeze then play" can't be scripted headless,
# but the motion is honest). feature-ply / feature-export-c want an external
# tool (MeshLab / a text editor) we can't drive headlessly, so they reuse a
# representative scene still as a stand-in; the SHOWCASE comments document the
# ideal shot.
if want sc-feature-time; then
    gif "$SHOW/feature-time.gif" 200 2 20 560 --example "Conditional colors (if + t)"
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

echo "docs-assets: done."
