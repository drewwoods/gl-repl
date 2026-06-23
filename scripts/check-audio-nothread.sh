#!/bin/bash
# Hard guard: the single-threaded audio build stays compilable.
#
# src/app/glr_audio.c selects its threading model at compile time
# (GLR_AUDIO_NO_THREAD). The default native build uses a pthread worker;
# Emscripten — and any -DGLR_AUDIO_NO_THREAD=1 build — drops the worker
# thread entirely and instead services the lifecycle request mailbox
# synchronously from glr_audio_tick(), with the lock helpers collapsed to
# no-ops. The normal build only ever exercises the threaded path, so the
# thread-OFF arms of those #if guards would silently rot.
#
# This guard syntax-checks glr_audio.c with the thread OFF, mirroring how
# the real build compiles it (-std=c99, -D_GNU_SOURCE, the
# config.h/prof_sections.h force-includes), so a broken no-thread arm
# fails CI here instead of only surfacing in an Emscripten build.
#
# Fast: -fsyntax-only, no codegen, no link (same shape as
# check-cpuprof-standalone.sh).

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CC=${CC:-gcc}
ROOT=$(pwd)

if ! err=$("$CC" -std=c99 -fsyntax-only \
        -Wall -Wno-deprecated-declarations -Wfloat-conversion \
        -Werror=implicit-function-declaration \
        -D_GNU_SOURCE -DGL_SILENCE_DEPRECATION -DGLR_AUDIO_NO_THREAD=1 \
        -include "$ROOT/config.h" -include "$ROOT/prof_sections.h" \
        -I"$ROOT" -I"$ROOT/src" -I"$ROOT/include" \
        src/app/glr_audio.c 2>&1); then
    echo "ERROR: src/app/glr_audio.c does not compile with GLR_AUDIO_NO_THREAD=1." >&2
    echo "The single-threaded audio path (used by Emscripten builds, where the" >&2
    echo "worker thread is dropped and the mailbox is drained from" >&2
    echo "glr_audio_tick()) is broken. Keep the #if GLR_AUDIO_NO_THREAD arms in" >&2
    echo "src/app/glr_audio.c in sync with the threaded path." >&2
    echo "Compiler output:" >&2
    printf '%s\n' "$err" >&2
    exit 1
fi

echo "audio-nothread OK (glr_audio.c compiles with GLR_AUDIO_NO_THREAD=1)"
