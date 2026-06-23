#!/usr/bin/env bash
# docs-assets.sh - regenerate the screenshots and GIFs under docs/images/
# (the media embedded in README.md and docs/USER_GUIDE.md), fully headless.
#
#   scripts/docs-assets.sh [-j N] [asset ...]   # default: all assets
#   scripts/docs-assets.sh --list               # print asset names and exit
#
# -j / --jobs N regenerates up to N assets in parallel (each gl-repl
# render is single-threaded, so this scales well). Default 1.
#
# Requires a FREEGLUT_OSMESA build (make gl-repl FREEGLUT_OSMESA=1) plus
# ImageMagick (magick) and ffmpeg. Override the binary with BIN=<path>.
#
# How it works:
#   - Every capture uses the OSMesa backend's record mode
#     (FREEGLUT_CAPTURE_FRAMES=N): the app renders exactly N frames to
#     numbered PPMs and exits. Stills keep the LAST frame, so anything
#     frame-based (grid/axes theme cross-fades, animation time t) has
#     deterministically settled — no wall-clock sleeps, no SIGUSR1 races.
#   - Scene states are staged by loading generated snippet files whose
#     `/* @cfg slug = value */` headers set presentation state and whose
#     optional `// camera` block poses the camera. GLR_EDIT_LINE parks
#     the cursor for cursor-bound overlays (transform guides).
#   - Antialiasing (the software rasterizer has no MSAA), two tools:
#       * Scene-only shots (code panel hidden) render at 2x the target
#         size via --window and downscale 50% — 4x supersampling.
#       * Full-UI shots stay 1x — the GLUT bitmap fonts don't scale, so
#         downscaling would halve the code-panel text — and instead
#         raise the accumulation-AA sample count via GLR_ACCUM_PASSES
#         (the 2D UI renders outside the accumulation loop, so text
#         stays crisp while 3D edges smooth out).
#       * This also suits the grid/axes theme shots: their subject is
#         1px hairlines, which supersampling washes out to half
#         intensity but jitter AA keeps at full weight.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release-osmesa/gl-repl}"
OUT="${OUT:-$ROOT/docs/images}"

W=1200          # target width/height of every asset's source window
H=800
GIF_FUZZ=8%     # magick -layers Optimize fuzz for GIF delta compression

ALL_ASSETS=(
    hero first-triangle vertex-overlays wireframe light-theme-studio
    grid-themes backdrops axes-compass view-mode-2d labels-orrery
    glu-tess glow-sprites transform-stress variable-panel tune-badges
    motion-blur xform-guide-still xform-guide replay animated-ring
)

JOBS=1
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) printf '%s\n' "${ALL_ASSETS[@]}"; exit 0 ;;
        -j|--jobs) JOBS="${2:?docs-assets: $1 needs a count}"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

for tool in magick ffmpeg; do
    command -v "$tool" >/dev/null || {
        echo "docs-assets: '$tool' not found" >&2; exit 1; }
done
[[ -x "$BIN" ]] || {
    echo "docs-assets: gl-repl binary not found at '$BIN'" >&2
    echo "             build it first: make gl-repl FREEGLUT_OSMESA=1" >&2
    exit 1; }

if [[ ${#ARGS[@]} -gt 0 ]]; then
    WANTED=("${ARGS[@]}")
else
    WANTED=("${ALL_ASSETS[@]}")
fi

# Parallel mode: re-exec ourselves one asset per process via xargs -P.
# (Portable to the macOS /bin/bash 3.2 era — no `wait -n`.) Each child
# gets its own work dir, so captures can't collide.
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
mkdir -p "$OUT"

# render <frames> <ss> <aa> <args...> — run one capture into $FRDIR
# (fresh per call, so renders never collide). ss=1 renders at WxH; ss=2
# renders at 2x for the 4x-supersampled shots. aa raises the
# accumulation sample count via GLR_ACCUM_PASSES (0 = stock AA 2x).
# Extra env (GLR_EDIT_LINE etc.) is passed by exporting before the call.
render() {
    local frames=$1 ss=$2 aa=$3; shift 3
    FRDIR="$WORK/frames-$RANDOM$RANDOM"
    mkdir -p "$FRDIR"
    local aa_env=()
    [[ "$aa" != 0 ]] && aa_env=("GLR_ACCUM_PASSES=$aa")
    env "${aa_env[@]}" \
        FREEGLUT_CAPTURE_FRAMES=$frames FREEGLUT_CAPTURE_FILE="$FRDIR/f" \
        "$BIN" "$@" --window $((W * ss))x$((H * ss)) --no-audio \
        >/dev/null 2>&1
}

# still <out.png> <frames> <ss> <aa> <args...> — keep the LAST frame.
still() {
    local out=$1 frames=$2 ss=$3 aa=$4; shift 4
    render "$frames" "$ss" "$aa" "$@"
    local last
    last="$(ls "$FRDIR"/f-*.ppm | tail -1)"
    if [[ "$ss" -gt 1 ]]; then
        magick "$last" -resize 50% "$out"
    else
        magick "$last" "$out"
    fi
    rm -rf "$FRDIR"
    echo "docs-assets: wrote $out"
}

# gif <out.gif> <frames> <step> <fps> <width> <args...> — record, take
# every <step>th frame, assemble a palette-optimized looping GIF.
# GIFs keep stock AA: raising the accum passes multiplies the cost of
# every recorded frame.
gif() {
    local out=$1 frames=$2 step=$3 fps=$4 width=$5; shift 5
    render "$frames" 1 0 "$@"
    rm -rf "$WORK/sub"; mkdir -p "$WORK/sub"
    local n=0 k=0 f
    for f in "$FRDIR"/f-*.ppm; do
        if (( n % step == 0 )); then
            cp "$f" "$WORK/sub/g-$(printf %04d $k).ppm"; k=$((k + 1))
        fi
        n=$((n + 1))
    done
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

# ---- staged scenes ------------------------------------------------------

stage_triangle() { stage triangle <<'EOF'
// Snippet start
glColor3f(1, 0.6, 0.1);
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
/* @cfg grid = GRID_THEME_FAINT */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.2, 0.7, 1);
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

stage_wireframe() { stage wireframe <<'EOF'
/* @cfg wireframe = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.3, 0.9, 1);
glutSolidTorus(0.4, 1.2, 24, 36);
// Snippet end
EOF
}

stage_lights() { stage lights <<'EOF'
/* @cfg light_theme = LIGHT_THEME_STUDIO */
/* @cfg light_indicators = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
// Snippet start
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
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_backdrop() {  # $1 = SCENE_BACKDROP_<NAME>, $2 = flat|default camera
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
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
$cam
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_axes() { stage axes <<'EOF'
/* @cfg axes = AXES_THEME_COMPASS */
/* @cfg grid = GRID_THEME_FAINT */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
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
glColor3f(0.4, 0.8, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 64) {
glVertex3f(-1.6 + i*0.05, sin(i*0.1*freq + t)*amp*0.4, spread*cos(i*0.05));
}
glEnd();
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
glEnable(GL_DEPTH_TEST);
glPushMatrix();
glRotatef(t*200, 0, 1, 0);
glColor3f(0.9, 0.55, 0.15);
glTranslatef(1.2, 0, 0);
glutSolidCube(0.6);
glPopMatrix();
// Snippet end
EOF
}

# Cursor parks on line 4 (glTranslatef) via GLR_EDIT_LINE.
stage_guide() { stage guide <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_FAINT */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
glutSolidCube(0.5);
glPushMatrix();
glTranslatef(2, 0.8, 0);
glColor3f(0.3, 0.8, 0.9);
glutSolidCube(0.5);
glPopMatrix();
// Snippet end
EOF
}

stage_replay() { stage replay <<'EOF'
/* @cfg replay = 1 */
/* @cfg variable_panel = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glBegin(GL_TRIANGLES);
for(i, 0, 12) {
glColor3f(0.5 + 0.5*cos(i*TAU/12), 0.6, 0.5 + 0.5*sin(i*TAU/12));
glVertex3f(cos(i*TAU/12)*1.5, sin(i*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.6)*TAU/12)*1.5, sin((i+0.6)*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.3)*TAU/12)*0.7, sin((i+0.3)*TAU/12)*0.7, 0);
}
glEnd();
// Snippet end
EOF
}

# ---- assets -------------------------------------------------------------

# Theme cross-fades settle in ~80 frames; 90 leaves margin. Plain shots
# settle immediately; 30 frames keeps t small but past startup.
FADE_FRAMES=90
PLAIN_FRAMES=30

if want hero; then
    still "$OUT/hero.png" $PLAIN_FRAMES 1 16 --example 26 --time 3
fi

if want first-triangle; then
    still "$OUT/first-triangle.png" $PLAIN_FRAMES 1 16 "$(stage_triangle)"
fi

if want vertex-overlays; then
    still "$OUT/vertex-overlays.png" $PLAIN_FRAMES 1 16 "$(stage_overlays)"
fi

if want wireframe; then
    still "$OUT/wireframe.png" $PLAIN_FRAMES 2 0 "$(stage_wireframe)"
fi

if want light-theme-studio; then
    still "$OUT/light-theme-studio.png" $PLAIN_FRAMES 2 0 "$(stage_lights)"
fi

# Grid and axes themes are hairline content: at 2x the 1px lines stay
# 1px, so the 50% downscale halves their intensity and the theme reads
# washed out. These two stay 1x with accum AA 16x — the lines ARE the
# subject, and jitter AA smooths them without dimming.
if want grid-themes; then
    for theme in TRON RADAR AURORA SYNTHWAVE; do
        still "$WORK/grid_$theme.png" $FADE_FRAMES 1 16 \
            "$(stage_grid_theme GRID_THEME_$theme)"
    done
    magick montage "$WORK"/grid_{TRON,RADAR,AURORA,SYNTHWAVE}.png \
        -tile 2x2 -geometry +2+2 -background black "$WORK/grid-m.png"
    magick "$WORK/grid-m.png" -resize "$W" "$OUT/grid-themes.png"
    echo "docs-assets: wrote $OUT/grid-themes.png"
fi

if want backdrops; then
    still "$WORK/bd_CITYSCAPE.png" $FADE_FRAMES 2 0 \
        "$(stage_backdrop SCENE_BACKDROP_CITYSCAPE flat)"
    still "$WORK/bd_STARS.png" $FADE_FRAMES 2 0 \
        "$(stage_backdrop SCENE_BACKDROP_STARS)"
    still "$WORK/bd_SUNSET.png" $FADE_FRAMES 2 0 \
        "$(stage_backdrop SCENE_BACKDROP_SUNSET)"
    still "$WORK/bd_AURORA.png" $FADE_FRAMES 2 0 \
        "$(stage_backdrop SCENE_BACKDROP_AURORA flat)"
    magick montage "$WORK"/bd_{CITYSCAPE,STARS,SUNSET,AURORA}.png \
        -tile 2x2 -geometry +2+2 -background black "$WORK/bd-m.png"
    magick "$WORK/bd-m.png" -resize "$W" "$OUT/backdrops.png"
    echo "docs-assets: wrote $OUT/backdrops.png"
fi

if want axes-compass; then
    still "$OUT/axes-compass.png" $FADE_FRAMES 1 16 "$(stage_axes)"
fi

if want view-mode-2d; then
    still "$OUT/view-mode-2d.png" $PLAIN_FRAMES 1 16 --example 1
fi

if want labels-orrery; then
    still "$OUT/labels-orrery.png" $PLAIN_FRAMES 1 16 --example 25 --time 4
fi

if want glu-tess; then
    still "$OUT/glu-tess.png" $PLAIN_FRAMES 1 16 --example 22
fi

if want glow-sprites; then
    still "$OUT/glow-sprites.png" $PLAIN_FRAMES 1 16 --example 16 --time 2
fi

if want transform-stress; then
    still "$OUT/transform-stress.png" $PLAIN_FRAMES 1 16 --example 20 --time 1
fi

if want variable-panel || want tune-badges; then
    still "$WORK/variable-panel.png" $PLAIN_FRAMES 1 16 "$(stage_tune)"
    if want variable-panel; then
        cp "$WORK/variable-panel.png" "$OUT/variable-panel.png"
        echo "docs-assets: wrote $OUT/variable-panel.png"
    fi
    if want tune-badges; then
        magick "$WORK/variable-panel.png" -crop 260x180+940+680 +repage \
            "$OUT/tune-badges.png"
        echo "docs-assets: wrote $OUT/tune-badges.png"
    fi
fi

if want motion-blur; then
    # 16 accumulation passes per frame: few frames, each expensive. The
    # status bar's "Blur 16x" indicator is part of the shot, so 1x.
    still "$OUT/motion-blur.png" 12 1 0 "$(stage_blur)"
fi

# Subshells: a VAR=x prefix on a shell *function* call persists after the
# call in bash, so export GLR_EDIT_LINE inside ( ) to keep it scoped.
if want xform-guide-still; then
    ( export GLR_EDIT_LINE=4
      still "$OUT/xform-guide-still.png" $PLAIN_FRAMES 1 16 "$(stage_guide)" )
fi

if want xform-guide; then
    ( export GLR_EDIT_LINE=4
      gif "$OUT/xform-guide.gif" 120 1 20 720 "$(stage_guide)" )
fi

if want replay; then
    # Replay advances a few commands per second; 500 frames subsampled
    # 3x shows the whole pinwheel assembling at a watchable pace.
    gif "$OUT/replay.gif" 500 3 24 720 "$(stage_replay)"
fi

if want animated-ring; then
    gif "$OUT/animated-ring.gif" 126 2 18 560 --example 2
fi

echo "docs-assets: done."
