#!/usr/bin/env bash
# gl4es-ab.sh - compare two gl4es builds through gl-repl's web build.
#
#   scripts/gl4es-ab.sh [--a <gl-repl rev>] [--b <gl4es dir>] [--out <dir>]
#                       [--lanes oracles,render,catalog] [--examples 1,5,9]
#                       [--runs N]
#
# A is the gl4es a gl-repl revision ships: that revision's GL4ES_SHA plus its
# packaging/web/patches, applied in its GL4ES_PATCHES order and built into
# <out>/gl4es-A (cached by pin + patch hash). Default: HEAD, i.e. the tested
# pin. B is any built gl4es tree - by default the managed third_party/web/gl4es
# (the working tree's pin + patches), or e.g. a PR branch checkout in a gl4es
# fork, which is how a PR gets exercised in gl-repl before it is opened.
#
# gl-repl's web objects are compiled once and linked twice, so the two trees
# must ship identical include/ (checked below). Three lanes, each A vs B:
#
#   oracles  `make test-gl-web` against each tree; failures compared one by
#            one. A failure only B has is a REGRESSION (non-zero exit); one
#            only A has is a gap B closed - drop it from
#            packaging/web/gl4es-known-gaps.txt.
#   render   bench/bench_render.c's fixed workloads, alternating A/B runs:
#            per-case ms/frame against run-to-run noise, plus pixel oracles.
#   catalog  every web example at a frozen t, screenshotted and pixel-diffed.
#            A diff is not automatically wrong (a fix *should* change pixels);
#            the report puts A, B and the diff side by side to judge.
#
# Everything lands in <out> (default build/gl4es-ab); <out>/index.html links
# the lane reports. Needs emcc (or emsdk at $EMSDK, default ~/src/emsdk),
# node >= 22, Chrome, and ImageMagick for the catalog lane.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

A_REV=HEAD
B_DIR=third_party/web/gl4es
OUT=build/gl4es-ab
LANES=oracles,render,catalog
EXAMPLES=
RUNS=

usage() {
	sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}
while [ $# -gt 0 ]; do
	case "$1" in
	--a) A_REV="$2"; shift 2 ;;
	--b) B_DIR="${2%/}"; shift 2 ;;
	--out) OUT="${2%/}"; shift 2 ;;
	--lanes) LANES="$2"; shift 2 ;;
	--examples) EXAMPLES="$2"; shift 2 ;;
	--runs) RUNS="$2"; shift 2 ;;
	-h|--help) usage ;;
	*) echo "gl4es-ab: unknown argument $1" >&2; usage ;;
	esac
done
lane() { case ",$LANES," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

if ! command -v emcc >/dev/null 2>&1; then
	EMSDK="${EMSDK:-$HOME/src/emsdk}"
	[ -f "$EMSDK/emsdk_env.sh" ] || { echo "gl4es-ab: emcc not on PATH and no emsdk at $EMSDK" >&2; exit 1; }
	# shellcheck disable=SC1091
	source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"   # absolute: some steps run from other directories
log() { printf '\n==> %s\n' "$*"; }

# --- A: the gl4es that revision $A_REV ships ---------------------------------
A_DIR="$OUT/gl4es-A"
a_deps="$(git show "$A_REV:scripts/web-deps.sh")"
a_pin="$(printf '%s\n' "$a_deps" | sed -n 's/^GL4ES_SHA="\(.*\)"$/\1/p')"
a_patches="$(printf '%s\n' "$a_deps" | sed -n 's|.*/patches/\(gl4es-[^"]*\.patch\)".*|\1|p')"
[ -n "$a_pin" ] || { echo "gl4es-ab: no GL4ES_SHA in $A_REV:scripts/web-deps.sh" >&2; exit 1; }
a_stamp="$a_pin:$(for p in $a_patches; do git show "$A_REV:packaging/web/patches/$p"; done | shasum -a 256 | cut -d' ' -f1)"
if [ -f "$A_DIR/lib/libGL.a" ] && [ "$(cat "$A_DIR/.ab-stamp" 2>/dev/null)" = "$a_stamp" ]; then
	log "A: gl4es for $A_REV already built ($a_pin + $(printf '%s\n' $a_patches | wc -l | tr -d ' ') patches)"
else
	log "A: building gl4es for $A_REV ($a_pin + $(printf '%s\n' $a_patches | wc -l | tr -d ' ') patches)"
	rm -rf "$A_DIR"
	# Clone from the managed checkout when it has the pin (fast, offline).
	src="$ROOT/third_party/web/gl4es"
	if ! git -C "$src" cat-file -e "$a_pin^{commit}" 2>/dev/null; then
		src="https://github.com/ptitSeb/gl4es.git"
	fi
	git clone -q --no-checkout "$src" "$A_DIR"
	git -C "$A_DIR" checkout -q "$a_pin"
	for p in $a_patches; do
		git show "$A_REV:packaging/web/patches/$p" | git -C "$A_DIR" apply
	done
	mkdir -p "$A_DIR/build_wasm"
	( cd "$A_DIR/build_wasm" \
	  && emcmake cmake .. -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON >/dev/null \
	  && emmake make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >"$OUT/gl4es-A-build.log" 2>&1 ) \
		|| { echo "gl4es-ab: A build failed, see $OUT/gl4es-A-build.log" >&2; exit 1; }
	printf '%s\n' "$a_stamp" >"$A_DIR/.ab-stamp"
fi

# --- B -----------------------------------------------------------------------
if [ ! -f "$B_DIR/lib/libGL.a" ]; then
	if [ "$B_DIR" = third_party/web/gl4es ]; then
		scripts/web-deps.sh
	else
		echo "gl4es-ab: $B_DIR/lib/libGL.a missing - build that gl4es tree first" >&2
		exit 1
	fi
fi
log "B: $B_DIR ($(git -C "$B_DIR" describe --always --dirty 2>/dev/null || echo 'not a git tree'))"

# The app objects are compiled once, against one tree's headers.
if ! diff -rq "$A_DIR/include" "$B_DIR/include" >/dev/null; then
	echo "gl4es-ab: A and B ship different include/ - one set of app objects cannot serve both" >&2
	diff -rq "$A_DIR/include" "$B_DIR/include" >&2 || true
	exit 1
fi

# --- link the web app and the render bench against each tree -----------------
for side in A B; do
	dir="$A_DIR"; [ "$side" = B ] && dir="$B_DIR"
	web="$OUT/$side-web"
	targets=()
	lane catalog && targets+=("$web/index.html")
	lane render && targets+=("$web/gl4es-render.html")
	[ ${#targets[@]} -eq 0 ] && continue
	log "$side: linking ${targets[*]}"
	make --no-print-directory WEB=1 BUILD=release GL4ES_DIR="$dir" \
		SAMPLE_BIN="$web/index.html" WEB_BINDIR="$web" "${targets[@]}" >"$OUT/$side-link.log" 2>&1 \
		|| { echo "gl4es-ab: $side link failed, see $OUT/$side-link.log" >&2; exit 1; }
done

status=0
# --- lane: oracles -----------------------------------------------------------
if lane oracles; then
	for side in A B; do
		dir="$A_DIR"; [ "$side" = B ] && dir="$B_DIR"
		log "oracles: $side"
		make --no-print-directory test-gl-web GL4ES_DIR="$dir" \
			GL_TESTS_WEB_BINDIR="$OUT/$side-gl-tests" GL_TESTS_WEB_JSON="$OUT/oracles-$side.json" \
			>"$OUT/oracles-$side.log" 2>&1 || true
		[ -f "$OUT/oracles-$side.json" ] || { echo "gl4es-ab: oracle run $side failed, see $OUT/oracles-$side.log" >&2; exit 1; }
	done
	log "oracles: A vs B"
	node - "$OUT/oracles-A.json" "$OUT/oracles-B.json" "$OUT/oracles.txt" <<'EOF' || status=1
const [fa, fb, out] = process.argv.slice(2);
const fs = require('node:fs');
const A = JSON.parse(fs.readFileSync(fa)), B = JSON.parse(fs.readFileSync(fb));
// Failure identity is the label without its measured values' line suffix.
const key = f => f.replace(/ \(line \d+\)$/, '');
const set = r => new Set([...r.failures.map(key), ...(['CRASH', 'TIMEOUT'].includes(r.status) ? [r.status] : [])]);
const lines = []; let regressions = 0;
for (const b of B) {
  const a = A.find(x => x.page === b.page) || { failures: [], status: 'MISSING' };
  const sa = set(a), sb = set(b);
  const onlyB = [...sb].filter(x => !sa.has(x)), onlyA = [...sa].filter(x => !sb.has(x));
  regressions += onlyB.length;
  lines.push(`${b.page}: A ${a.status} ${a.passed ?? '-'}/${a.run ?? '-'}  B ${b.status} ${b.passed}/${b.run}` +
             `  regressions ${onlyB.length}  fixed ${onlyA.length}`);
  onlyB.forEach(x => lines.push(`  REGRESSION ${x}`));
  onlyA.forEach(x => lines.push(`  fixed in B ${x}`));
}
lines.push(regressions ? `${regressions} oracle regression(s) in B` : 'no oracle regressions in B');
fs.writeFileSync(out, lines.join('\n') + '\n');
console.log(lines.join('\n'));
process.exit(regressions ? 1 : 0);
EOF
fi

# --- lane: render ------------------------------------------------------------
if lane render; then
	log "render bench"
	node scripts/gl4es-ab-render.mjs --a "$OUT/A-web" --b "$OUT/B-web" --out "$OUT/render" ${RUNS:+--runs "$RUNS"}
fi

# --- lane: catalog -----------------------------------------------------------
if lane catalog; then
	# Web example names, in --example index order (section order).
	sed -n 's/^name *= *//p' examples/catalog-emscripten.ini | awk '{ printf "%d\t%s\n", NR, $0 }' >"$OUT/names.tsv"
	log "catalog: $(wc -l <"$OUT/names.tsv" | tr -d ' ') examples"
	node scripts/gl4es-ab-catalog.mjs --a "$OUT/A-web" --b "$OUT/B-web" --out "$OUT/catalog" \
		--names "$OUT/names.tsv" ${EXAMPLES:+--examples "$EXAMPLES"}
fi

# --- index -------------------------------------------------------------------
{
	echo '<!doctype html><meta charset="utf-8"><title>gl4es A/B</title>'
	echo '<style>:root{color-scheme:light dark}body{font:14px/1.5 system-ui,sans-serif;margin:24px;max-width:960px}pre{white-space:pre-wrap}</style>'
	echo "<h1>gl4es A/B</h1><p>A = gl-repl $(git rev-parse --short "$A_REV") ($a_pin + patches) &middot; B = $B_DIR</p><ul>"
	lane catalog && echo '<li><a href="catalog/report.html">example catalog (pixels)</a></li>'
	lane render && echo '<li><a href="render/report.html">render bench (time + pixel oracles)</a></li>'
	echo '</ul>'
	if [ -f "$OUT/oracles.txt" ]; then
		echo '<h2>oracles (make test-gl-web)</h2><pre>'
		sed 's/&/\&amp;/g; s/</\&lt;/g' "$OUT/oracles.txt"
		echo '</pre>'
	fi
} >"$OUT/index.html"
log "report: $OUT/index.html"
exit $status
