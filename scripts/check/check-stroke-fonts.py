#!/usr/bin/env python3
"""Validate the vendored high-resolution freeglut stroke fonts."""

import importlib.util
import math
import os
import subprocess
import sys
import tempfile


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "third_party", "freeglut", "src")
GENERATOR = os.path.join(FONT_DIR, "genstroke_hi.py")
EPS = 2e-4


def load_generator():
    spec = importlib.util.spec_from_file_location("genstroke_hi", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def close(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1]) <= EPS


def anchor_indices(base, high):
    """Return each base anchor's ordered index in the resampled strip."""
    result = []
    pos = 0
    for point in base:
        while pos < len(high) and not close(point, high[pos]):
            pos += 1
        if pos == len(high):
            raise AssertionError("source anchor missing or reordered")
        result.append(pos)
        pos += 1
    return result


def segment_count(chars):
    return sum(max(0, len(strip) - 1)
               for _, strips in chars.values() for strip in strips)


def assert_reproduced(generator, checked_text, regenerated_text, label):
    """Compare generated geometry without depending on Python/libm tie rounding."""
    _, _, checked_quantity, checked_height, checked = generator.parse_font(checked_text)
    _, _, regen_quantity, regen_height, regenerated = generator.parse_font(regenerated_text)
    if checked_quantity != regen_quantity or checked_height != regen_height:
        raise AssertionError("%s regenerated different font-wide metrics" % label)
    if checked.keys() != regenerated.keys():
        raise AssertionError("%s regenerated a different glyph table" % label)
    for code in checked:
        checked_right, checked_strips = checked[code]
        regen_right, regen_strips = regenerated[code]
        if abs(checked_right - regen_right) > EPS:
            raise AssertionError("%s regenerated a different 0x%02x advance" %
                                 (label, code))
        if len(checked_strips) != len(regen_strips):
            raise AssertionError("%s regenerated different 0x%02x strips" %
                                 (label, code))
        for checked_strip, regen_strip in zip(checked_strips, regen_strips):
            if len(checked_strip) != len(regen_strip):
                raise AssertionError("%s regenerated different 0x%02x vertices" %
                                     (label, code))
            if any(not close(a, b) for a, b in zip(checked_strip, regen_strip)):
                raise AssertionError("%s geometry is stale for 0x%02x" %
                                     (label, code))


def bounds(strips):
    points = [point for strip in strips for point in strip]
    if not points:
        return None
    return (min(p[0] for p in points), max(p[0] for p in points),
            min(p[1] for p in points), max(p[1] for p in points))


def assert_round(points, label):
    if len(points) < 25 or not close(points[0], points[24]):
        raise AssertionError("%s is not a closed 24-segment mark" % label)
    core = points[:24]
    cx = sum(p[0] for p in core) / len(core)
    cy = sum(p[1] for p in core) / len(core)
    radii = [math.hypot(p[0] - cx, p[1] - cy) for p in core]
    ratio = min(radii) / max(radii)
    if ratio < 0.995:
        raise AssertionError("%s remains pinched (min/max radius %.4f)" %
                             (label, ratio))


def assert_axis_segment(base, high, start, end, label):
    indices = anchor_indices(base, high)
    lo, hi = indices[start], indices[end]
    points = high[lo:hi + 1]
    a, b = base[start], base[end]
    if abs(a[0] - b[0]) <= EPS:
        error = max(abs(p[0] - a[0]) for p in points)
    elif abs(a[1] - b[1]) <= EPS:
        error = max(abs(p[1] - a[1]) for p in points)
    else:
        raise AssertionError("%s test segment is not axis-aligned" % label)
    if error > EPS:
        raise AssertionError("%s bends by %.4f font units" % (label, error))


def check_font(generator, source_name, high_name):
    source_path = os.path.join(FONT_DIR, source_name)
    high_path = os.path.join(FONT_DIR, high_name)
    source_text = open(source_path).read()
    high_text = open(high_path).read()
    _, _, source_quantity, source_height, source = generator.parse_font(source_text)
    _, _, high_quantity, high_height, high = generator.parse_font(high_text)

    if source_quantity != high_quantity or source_height != high_height:
        raise AssertionError("%s changed the font-wide metrics" % high_name)
    if source.keys() != high.keys():
        raise AssertionError("%s changed the glyph table" % high_name)

    for code in source:
        source_right, source_strips = source[code]
        high_right, high_strips = high[code]
        if source_right != high_right:
            raise AssertionError("%s changed advance width for 0x%02x" %
                                 (high_name, code))
        if len(source_strips) != len(high_strips):
            raise AssertionError("%s changed strip count for 0x%02x" %
                                 (high_name, code))
        for source_strip, high_strip in zip(source_strips, high_strips):
            anchor_indices(source_strip, high_strip)
        source_bounds = bounds(source_strips)
        high_bounds = bounds(high_strips)
        if source_bounds is not None:
            overflow = max(source_bounds[0] - high_bounds[0],
                           high_bounds[1] - source_bounds[1],
                           source_bounds[2] - high_bounds[2],
                           high_bounds[3] - source_bounds[3])
            if overflow > 2.0:
                raise AssertionError("%s lets 0x%02x overshoot by %.3f units" %
                                     (high_name, code, overflow))

    # True circular marks, including the loop joined to comma/semicolon tails.
    for char, strip in (("!", 1), (",", 0), (".", 0), (":", 0), (":", 1),
                        (";", 0), (";", 1), ("?", 1), ("i", 0), ("j", 0)):
        assert_round(high[ord(char)][1][strip], "%s %r" % (high_name, char))

    # Long source stems must not inherit curvature from the adjoining hook.
    assert_axis_segment(source[ord("f")][1][0], high[ord("f")][1][0],
                        3, 4, "%s 'f' stem" % high_name)
    assert_axis_segment(source[ord("j")][1][1], high[ord("j")][1][1],
                        0, 1, "%s 'j' stem" % high_name)
    assert_axis_segment(source[ord("t")][1][0], high[ord("t")][1][0],
                        0, 1, "%s 't' stem" % high_name)

    if source[ord("_")][1] != high[ord("_")][1]:
        raise AssertionError("%s rounded the underscore rectangle" % high_name)
    if len(high[ord("@")][1][0]) <= len(source[ord("@")][1][0]):
        raise AssertionError("%s did not resample the @ inner bowl" % high_name)

    source_segments = segment_count(source)
    high_segments = segment_count(high)
    if high_segments > source_segments * 5.2:
        raise AssertionError("%s segment budget grew beyond 5.2x" % high_name)

    with tempfile.TemporaryDirectory(prefix="glr-stroke-font-") as tmp:
        regenerated = os.path.join(tmp, high_name)
        subprocess.run([sys.executable, GENERATOR, source_path, regenerated],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert_reproduced(generator, high_text, open(regenerated).read(), high_name)

    return source_segments, high_segments


def main():
    generator = load_generator()
    pairs = (("fg_stroke_roman.c", "fg_stroke_roman_hi.c"),
             ("fg_stroke_mono_roman.c", "fg_stroke_mono_roman_hi.c"))
    for source_name, high_name in pairs:
        source_segments, high_segments = check_font(generator, source_name, high_name)
        print("%s: %d -> %d line segments (%.2fx), geometry OK" %
              (high_name, source_segments, high_segments,
               high_segments / source_segments))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print("ERROR: stroke-font check failed: %s" % exc, file=sys.stderr)
        sys.exit(1)
