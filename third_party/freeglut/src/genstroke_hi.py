#!/usr/bin/env python3
"""genstroke_hi.py - generate a higher-resolution GLUT stroke Roman font.

Reads freeglut's machine-generated src/fg_stroke_roman.c and emits
src/fg_stroke_roman_hi.c, a drop-in font with the SAME glyph anchor points,
metrics and advance widths, but with curved strokes resampled at much higher
resolution so they render smooth at large sizes.

Method: per stroke polyline, classify each interior vertex as a hard CORNER
(turn angle above a threshold) or a smooth curve point. Runs of smooth points
between hard points are replaced by a centripetal Catmull-Rom spline that
passes exactly through every original vertex, sampled at SUBDIV points per
segment. Straight 2-point runs and corners are left untouched, so E/L/T stay
crisp while O/S/C/e get smooth. Closed loops (first vertex == last) are
resampled periodically for a seamless seam.

Usage: genstroke_hi.py <in fg_stroke_roman.c> <out fg_stroke_roman_hi.c>
"""
import math
import re
import sys

# ---- tunables -------------------------------------------------------------
CORNER_DEG = 60.0   # turn sharper than this at a vertex => hard corner (break)
SUBDIV     = 6      # samples per segment inside a smooth run (>=1; 1 == no-op)
CLOSE_EPS  = 1e-3   # first/last vertex closer than this => closed loop


def parse_roman(text):
    """Return (name, quantity, height, {code: (right, [strip,...])})
    where strip is a list of (x, y) float pairs, ordered by ASCII code."""
    # Font header: fgStrokeRoman = {"Roman",128,152.381f,StrokeRoman_chars};
    m = re.search(r'SFG_StrokeFont\s+fgStrokeRoman\s*=\s*\{\s*"([^"]*)"\s*,\s*'
                  r'(\d+)\s*,\s*([0-9.eEf+-]+)\s*,', text)
    if not m:
        raise SystemExit("could not find fgStrokeRoman font header")
    name, quantity, height = m.group(1), int(m.group(2)), m.group(3).rstrip('f')

    num = r'[-+0-9.eEf]+'
    # Vertex arrays: StrokeRoman_ch33st0[] = { {x,y}, ... };
    verts = {}  # (code, strip_idx) -> [(x,y), ...]
    vre = re.compile(r'StrokeRoman_ch(\d+)st(\d+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;',
                     re.DOTALL)
    pair = re.compile(r'\{\s*(' + num + r')\s*,\s*(' + num + r')\s*\}')
    for vm in vre.finditer(text):
        code, sidx = int(vm.group(1)), int(vm.group(2))
        pts = [(float(a.rstrip('f')), float(b.rstrip('f')))
               for a, b in pair.findall(vm.group(3))]
        verts[(code, sidx)] = pts

    # Char defs: StrokeRoman_ch33 = {26.6238f,2,StrokeRoman_ch33st};
    chars = {}
    cre = re.compile(r'StrokeRoman_ch(\d+)\s*=\s*\{\s*(' + num +
                     r')\s*,\s*(\d+)\s*,\s*StrokeRoman_ch\d+st\s*\}\s*;')
    for cm in cre.finditer(text):
        code = int(cm.group(1))
        right = float(cm.group(2).rstrip('f'))
        nstrips = int(cm.group(3))
        strips = [verts.get((code, k), []) for k in range(nstrips)]
        chars[code] = (right, strips)
    return name, quantity, height, chars


def turn_angle(a, b, c):
    """Direction change (degrees) of the path a->b->c at b. 0 == straight."""
    v1 = (b[0] - a[0], b[1] - a[1])
    v2 = (c[0] - b[0], c[1] - b[1])
    n1 = math.hypot(*v1)
    n2 = math.hypot(*v2)
    if n1 < 1e-9 or n2 < 1e-9:
        return 0.0
    dot = (v1[0] * v2[0] + v1[1] * v2[1]) / (n1 * n2)
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(math.acos(dot))


def arc_extrapolate(a, b, c):
    """Continue the arc a->b->c to a 4th control point, preserving the last
    turn angle and edge length (so a terminal keeps curving along its arc
    instead of poking out straight). Falls back to linear reflection when the
    run is degenerate or near-straight, so straight terminals stay straight."""
    e1x, e1y = b[0] - a[0], b[1] - a[1]
    e2x, e2y = c[0] - b[0], c[1] - b[1]
    lin = (2 * c[0] - b[0], 2 * c[1] - b[1])
    if math.hypot(e1x, e1y) < 1e-9 or math.hypot(e2x, e2y) < 1e-9:
        return lin
    ang = math.atan2(e1x * e2y - e1y * e2x, e1x * e2x + e1y * e2y)
    if abs(ang) < math.radians(3.0):          # essentially straight
        return lin
    ca, sa = math.cos(ang), math.sin(ang)     # rotate e2 by the same turn
    return (c[0] + e2x * ca - e2y * sa, c[1] + e2x * sa + e2y * ca)


def catmull_rom(p0, p1, p2, p3, tsteps):
    """Centripetal Catmull-Rom points strictly BETWEEN p1 and p2 (exclusive),
    at tsteps-1 interior samples."""
    def tj(ti, a, b):
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        return ti + math.sqrt(d) if d > 1e-12 else ti + 1e-6
    t0 = 0.0
    t1 = tj(t0, p0, p1)
    t2 = tj(t1, p1, p2)
    t3 = tj(t2, p2, p3)
    out = []
    for s in range(1, tsteps):
        t = t1 + (t2 - t1) * (s / tsteps)
        def lerp(a, b, ta, tb):
            if abs(tb - ta) < 1e-12:
                return a
            w = (t - ta) / (tb - ta)
            return (a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w)
        A1 = lerp(p0, p1, t0, t1)
        A2 = lerp(p1, p2, t1, t2)
        A3 = lerp(p2, p3, t2, t3)
        B1 = lerp(A1, A2, t0, t2)
        B2 = lerp(A2, A3, t1, t3)
        out.append(lerp(B1, B2, t1, t2))
    return out


def smooth_strip(pts):
    """Resample one stroke polyline, preserving anchors and corners."""
    n = len(pts)
    if n < 3 or SUBDIV < 2:
        return pts
    closed = math.hypot(pts[0][0] - pts[-1][0], pts[0][1] - pts[-1][1]) < CLOSE_EPS

    if closed:
        # periodic ring; core is pts[0..n-2] (drop duplicated closing vertex)
        core = pts[:-1]
        m = len(core)
        if m < 3:
            return pts
        hard = [turn_angle(core[(i - 1) % m], core[i], core[(i + 1) % m]) > CORNER_DEG
                for i in range(m)]
        out = []
        for i in range(m):
            p1 = core[i]
            p2 = core[(i + 1) % m]
            out.append(p1)
            # smooth this segment only if neither endpoint is a hard corner
            if not hard[i] and not hard[(i + 1) % m]:
                p0 = core[(i - 1) % m]
                p3 = core[(i + 2) % m]
                out.extend(catmull_rom(p0, p1, p2, p3, SUBDIV))
        out.append(out[0])  # re-close
        return out

    # open polyline. The two free terminals are smooth anchors, NOT corners:
    # a stroke end has no turn to measure, and forcing it hard would leave the
    # first/last segment straight (visible flat tips on c, s, e, ...). Only
    # genuine interior sharp turns break the spline; terminal segments curve
    # into the tip via a reflected phantom control point below.
    hard = [False] * n
    for i in range(1, n - 1):
        hard[i] = turn_angle(pts[i - 1], pts[i], pts[i + 1]) > CORNER_DEG
    out = []
    for i in range(n - 1):
        p1, p2 = pts[i], pts[i + 1]
        out.append(p1)
        if not hard[i] and not hard[i + 1]:
            # Phantom controls past a free terminal follow the arc (preserve
            # curvature into the tip), not a straight reflection.
            p0 = pts[i - 1] if i - 1 >= 0 else arc_extrapolate(pts[2], pts[1], pts[0])
            p3 = pts[i + 2] if i + 2 < n else arc_extrapolate(pts[n - 3], pts[n - 2], pts[n - 1])
            out.extend(catmull_rom(p0, p1, p2, p3, SUBDIV))
    out.append(pts[-1])
    return out


def fnum(v):
    """Match the source formatting: trimmed float with an 'f' suffix.
    Always keep a decimal point so integers stay valid C float literals
    (e.g. 100.0f, not the invalid 100f)."""
    s = '{:.4f}'.format(v).rstrip('0')
    if s.endswith('.'):
        s += '0'
    return s + 'f'


HEADER = '''/*
 * Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
 * Written by Pawel W. Olszta, <olszta@sourceforge.net>
 * Creation date: Thu Dec 16 1999
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* freeglut high-resolution Roman stroke font definition */
/* This file has been automatically generated by the genstroke_hi utility from
 * fg_stroke_roman.c: curved strokes are resampled with a corner-preserving
 * centripetal Catmull-Rom spline (same anchor points, metrics and advance
 * widths as GLUT_STROKE_ROMAN; only the sampling density of curves differs).
 * Do not edit by hand -- re-run genstroke_hi.py. */

#include <GL/freeglut.h>
#ifdef FREEGLUT_GEOMETRY_STANDALONE
#  include "standalone/fg_glutshapes_shim.h"
#else
#  include "fg_internal.h"
#endif
'''


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        text = f.read()
    name, quantity, height, chars = parse_roman(text)

    out = [HEADER]
    codes = sorted(chars)
    base_v = sum(len(s) for _, ss in chars.values() for s in ss)
    for code in codes:
        right, strips = chars[code]
        out.append('\n/* char: 0x%02x */\n' % code)
        strip_names = []
        real = [s for s in strips if s]  # NULL strips (space) carry no verts
        for k, pts in enumerate(strips):
            if not pts:
                continue
            hp = smooth_strip(pts)
            nm = 'StrokeRomanHi_ch%dst%d' % (code, k)
            strip_names.append((nm, len(hp)))
            out.append('static const SFG_StrokeVertex %s[] =\n{\n' % nm)
            out.append(''.join(' {%s,%s},\n' % (fnum(x), fnum(y)) for x, y in hp))
            out.append('};\n\n')
        stripnm = 'StrokeRomanHi_ch%dst' % code
        out.append('static const SFG_StrokeStrip %s[] =\n{\n' % stripnm)
        if strip_names:
            out.append(''.join(' {%d,%s},\n' % (cnt, nm) for nm, cnt in strip_names))
        else:
            out.append('  { 0, NULL }\n')
        out.append('};\n\n')
        out.append('static const SFG_StrokeChar StrokeRomanHi_ch%d = {%s,%d,%s};\n'
                   % (code, fnum(right), len(strip_names), stripnm))

    # chars[] table: 0..quantity-1, NULL below the first present code.
    out.append('\nstatic const SFG_StrokeChar *StrokeRomanHi_chars[] =\n{\n')
    row = []
    for i in range(quantity):
        cell = '&StrokeRomanHi_ch%d' % i if i in chars else '0'
        row.append(cell)
        if len(row) == 8:
            out.append(' ' + ', '.join(row) + ',\n')
            row = []
    if row:
        out.append(' ' + ', '.join(row) + '\n')
    out.append('};\n\n')
    out.append('SFG_StrokeFont fgStrokeRomanHi = {"%s",%d,%sf,StrokeRomanHi_chars};\n'
               % ('RomanHi', quantity, height))

    with open(dst, 'w') as f:
        f.write(''.join(out))

    # stats
    with open(dst) as f:
        hi_v = f.read()
    hi_count = hi_v.count('{', hi_v.index('char: 0x'))
    sys.stderr.write('genstroke_hi: %d glyphs, ~%d base vertices -> resampled '
                     '(SUBDIV=%d, CORNER=%.0fdeg)\n'
                     % (len(codes), base_v, SUBDIV, CORNER_DEG))


if __name__ == '__main__':
    main()
