#!/bin/bash
# Hard guard: the generic CPU-profile timer compiles with NO section catalog.
#
# src/support/cpuprof.{c,h} are meant to be a host-agnostic, int-keyed wall-time
# sampler: the named-section catalog (the ProfSection enum + PROF_SECTION_COUNT)
# is supplied by the host via prof_sections.h, force-included by the Makefile
# (-include prof_sections.h). When that catalog is absent, cpuprof.h's
# `#ifndef PROF_SECTIONS_PROVIDED` fallback makes ProfSection an opaque int so
# the timer still compiles standalone.
#
# That fallback is never exercised by the normal build (which always injects the
# catalog), so it would silently rot. This guard keeps it honest by syntax-only
# compiling cpuprof.c WITHOUT any -include catalog and WITHOUT defining
# PROF_SECTIONS_PROVIDED — exactly the "lift these two files into another
# project unchanged" scenario. If someone re-couples cpuprof.h to config.h or to
# the enum's named constants, this fails.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CC="${CC:-gcc}"

# Note: deliberately no `-include config.h -include prof_sections.h` and no
# `-DPROF_SECTIONS_PROVIDED`; -Isrc lets it resolve "support/cpuprof.h".
if ! err=$("$CC" -std=c99 -Isrc -Iinclude -I. -fsyntax-only \
        src/support/cpuprof.c 2>&1); then
    echo "ERROR: src/support/cpuprof.c does not compile without a section catalog." >&2
    echo "The generic timer must stand alone via cpuprof.h's #ifndef" >&2
    echo "PROF_SECTIONS_PROVIDED fallback (ProfSection=int, PROF_SECTION_COUNT=1)." >&2
    echo "Don't reintroduce a hard dependency on config.h or on named PROF_* enum" >&2
    echo "constants in src/support/cpuprof.{c,h}." >&2
    echo "Compiler output:" >&2
    printf '%s\n' "$err" >&2
    exit 1
fi

echo "cpuprof-standalone OK (generic timer compiles with no section catalog)"
