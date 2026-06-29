#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SRC_DIR:-${HOME}/src}"
PROJECT_SRC="${SCRIPT_DIR}/../src"
PROJECT_INCLUDE="${SCRIPT_DIR}/../include"
BOOTSTRAP="${SCRIPT_DIR}/gl4es_bootstrap.c"
OUT_DIR="${SCRIPT_DIR}/out"

# -- Emscripten environment -------------------------------
STACK_SIZE=$((8*1024*1024)) # 8MB stack size for complex samples
GL_MAX_TEMP_BUFFER_SIZE=$((1024*1024*64)) # 64MB temp buffer for complex samples
INITIAL_MEMORY=$((1024 * 1024 * 768)) # 768 initial memory for REPL/Complex samples

# ── Library Paths (edit these if your layout differs) ────────────────────────
GL4ES_INCLUDE="${SRC_DIR}/gl4es/include"
GL4ES_LIB="${SRC_DIR}/gl4es/lib/libGL.a"
GL4ES_GL_H="${GL4ES_INCLUDE}/GL/gl.h"
GLU_DIR="${SCRIPT_DIR}/GLU"
GLU_LIB="${GLU_DIR}/.libs/libGLU.a"
FREEGLUT_DIR="${SCRIPT_DIR}/freeglut"
FREEGLUT_LIB="${FREEGLUT_DIR}/build_wasm/lib/libglut.a"
FREEGLUT_INCLUDE="${FREEGLUT_DIR}/include"
FREEGLUT_PATCH="${SCRIPT_DIR}/0001-feat-add-Emscripten-WebAssembly-platform-support.patch"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "Usage: $(basename "$0") [OPTIONS] <source.c | Makefile>"
    echo ""
    echo "Compile OpenGL C samples to WebAssembly using Emscripten + gl4es + GLU"
    echo ""
    echo "Options:"
    echo "  --all       Build all best.c and sample.c files from ../src/** (direct emcc)"
    echo "  --no-serve  Build only, don't start HTTP server"
    echo "  --help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") ../src/sine-spin/best.c"
    echo "  $(basename "$0") ../src/immediate-mode-repl/claude4.6-opus-thinking/Makefile"
    echo "  $(basename "$0") --all"
    exit 0
}

# ── Dependency Checks ────────────────────────────────────────────────────────

check_emsdk() {
    if [[ ! -f "${SRC_DIR}/emsdk/emsdk" ]]; then
        echo -e "${RED}Error: emsdk not found at ${SRC_DIR}/emsdk/${NC}"
        echo ""
        echo "Install emsdk:"
        echo "  pushd ${SRC_DIR}"
        echo "  git clone https://github.com/emscripten-core/emsdk.git"
        echo "  cd emsdk"
        echo "  ./emsdk install latest"
        echo "  ./emsdk activate latest"
        echo "  popd"
        exit 1
    fi
    # shellcheck disable=SC1091
    source "${SRC_DIR}/emsdk/emsdk_env.sh" 2>/dev/null
}

check_gl4es() {
    if [[ ! -f "${GL4ES_LIB}" ]]; then
        echo -e "${RED}Error: gl4es not found at ${GL4ES_LIB}${NC}"
        echo ""
        echo "Install gl4es:"
        echo "  pushd ${SRC_DIR}"
        echo "  git clone https://github.com/ptitSeb/gl4es.git"
        echo "  cd gl4es"
        echo "  mkdir build_wasm && cd build_wasm"
        echo "  emcmake cmake .. -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON"
        echo "  emmake make"
        echo "  popd"
        exit 1
    fi
}

check_glu() {
    if [[ ! -f "${GLU_LIB}" ]]; then
        echo -e "${YELLOW}GLU not found locally at ${GLU_DIR}/${NC}"
        read -rp "Build GLU now? [y/N] " answer
        if [[ "${answer}" != [yY] ]]; then
            echo "GLU is required. Exiting."
            exit 1
        fi
        build_glu
    fi
}

build_glu() {
    echo -e "${CYAN}── Building GLU ──${NC}"

    if [[ ! -d "${GLU_DIR}" ]]; then
        echo "Cloning GLU..."
        git clone https://github.com/ptitSeb/GLU.git "${GLU_DIR}"
    fi

    pushd "${GLU_DIR}" > /dev/null

    if [[ ! -f configure ]]; then
        echo "Running autoreconf..."
        autoreconf -fi
    fi

    echo "Configuring GLU..."
    emconfigure ./configure --disable-shared --enable-static

    echo "Building GLU..."
    emmake make \
        CFLAGS="-include ${GL4ES_GL_H} -I${GL4ES_INCLUDE} -D__EMSCRIPTEN__ -DUSE_MGL_NAMESPACE" \
        CXXFLAGS="-include ${GL4ES_GL_H} -I${GL4ES_INCLUDE} -D__EMSCRIPTEN__ -DUSE_MGL_NAMESPACE -Wno-register" \
        V=1

    popd > /dev/null

    if [[ -f "${GLU_LIB}" ]]; then
        echo -e "${GREEN}GLU built successfully.${NC}"
    else
        echo -e "${RED}GLU build failed.${NC}"
        exit 1
    fi
}

check_freeglut() {
    if [[ ! -f "${FREEGLUT_LIB}" ]]; then
        echo -e "${YELLOW}freeglut not found locally at ${FREEGLUT_DIR}/${NC}"
        read -rp "Build freeglut now? [y/N] " answer
        if [[ "${answer}" != [yY] ]]; then
            echo "freeglut is required for glutSolid*/glutWire* geometry. Exiting."
            exit 1
        fi
        build_freeglut
    fi
}

build_freeglut() {
    echo -e "${CYAN}── Building freeglut ──${NC}"

    if [[ ! -d "${FREEGLUT_DIR}" ]]; then
        echo "Cloning freeglut..."
        git clone https://github.com/freeglut/freeglut.git "${FREEGLUT_DIR}"
    fi

    echo "Applying Emscripten support patch..."
    pushd "${FREEGLUT_DIR}" > /dev/null
    if ! git apply --check "${FREEGLUT_PATCH}" 2>/dev/null; then
        echo "Patch already applied or not needed, skipping."
    else
        git apply "${FREEGLUT_PATCH}"
    fi
    popd > /dev/null

    mkdir -p "${FREEGLUT_DIR}/build_wasm"
    pushd "${FREEGLUT_DIR}/build_wasm" > /dev/null

    echo "Configuring freeglut..."
    emcmake cmake .. \
        -DFREEGLUT_BUILD_DEMOS=OFF \
        -DFREEGLUT_BUILD_SHARED_LIBS=OFF \
        -DFREEGLUT_BUILD_STATIC_LIBS=ON \
        -DFREEGLUT_REPLACE_GLUT=ON \
        -DCMAKE_C_FLAGS="-include ${GL4ES_GL_H} -I${GL4ES_INCLUDE}" \
        -DCMAKE_INSTALL_PREFIX="${FREEGLUT_DIR}/install"

    echo "Building freeglut..."
    emmake make

    popd > /dev/null

    if [[ -f "${FREEGLUT_LIB}" ]]; then
        echo -e "${GREEN}freeglut built successfully.${NC}"
    else
        echo -e "${RED}freeglut build failed.${NC}"
        exit 1
    fi
}

# ── Build ────────────────────────────────────────────────────────────────────

build_one() {
    local input_file="$1"
    local input_file_abs
    input_file_abs="$(cd "$(dirname "$input_file")" && pwd)/$(basename "$input_file")"

    if [[ ! -f "${input_file_abs}" ]]; then
        echo -e "${RED}Error: File not found: ${input_file}${NC}"
        return 1
    fi

    # Derive sample name from parent directory
    local sample_dir
    sample_dir="$(dirname "${input_file_abs}")"
    local project_src_abs
    project_src_abs="$(cd "${PROJECT_SRC}" && pwd)"
    local rel_path="${sample_dir#"${project_src_abs}"/}"
    # Use nested path as name (replace / with -); for projects outside
    # ${PROJECT_SRC} (e.g. the gl-repl repo) the prefix-strip is a no-op
    # and would mangle the absolute path, so fall back to the dir name.
    local sample_name
    if [[ "${rel_path}" == "${sample_dir}" ]]; then
        sample_name="$(basename "${sample_dir}")"
    else
        sample_name="${rel_path//\//-}"
    fi
    if [[ -z "${sample_name}" || "${sample_name}" == "." ]]; then
        sample_name="$(basename "${sample_dir}")"
    fi

    local out_path="${OUT_DIR}/${sample_name}"
    mkdir -p "${out_path}"

    local out_html="${out_path}/index.html"

    # If the sample ships an assets/ folder (e.g. background music for
    # samples using repl_audio / miniaudio), bundle it into the .data
    # package under /assets so relative paths used by the native build
    # resolve the same way under Emscripten's MEMFS. Silently skipped
    # when the folder doesn't exist.
    local preload_args=()
    local preload_flag=""
    if [[ -d "${sample_dir}/assets" ]]; then
        preload_args+=(--preload-file "${sample_dir}/assets@/assets")
        preload_flag="--preload-file ${sample_dir}/assets@/assets"
        echo -e "${CYAN}  bundling ${sample_dir}/assets → /assets${NC}"
    fi

    # --- Makefile Project Handling ---
    if [[ "$(basename "${input_file_abs}")" == "Makefile" ]]; then
        echo -e "${CYAN}Building via Makefile: ${input_file} → ${out_html}${NC}"

        # --- New-layout gl-repl repo (src/ tree, `gl-repl:` target) ---
        # Its Makefile already carries the project -I flags and the
        # `-include config.h` force-includes in OBJ_CFLAGS, so we override
        # only the GL header/link knobs it exposes:
        #   GL_HEADER_CFLAGS — swap native GL headers for gl4es + the
        #     patched freeglut + GLU. `-std=gnu99` is appended because it
        #     comes after the Makefile's -std=c99 on the compile line and
        #     miniaudio's WebAudio backend needs EM_ASM (a gnu-mode
        #     extension); native builds keep their C99 ratchet untouched.
        #   GL_LDFLAGS — gl4es/GLU/freeglut static archives + emcc opts.
        #   SAMPLE_BIN — point the link rule's -o straight at index.html.
        #   OBJDIR — keep wasm objects out of the native build/<cfg> dirs.
        # The windowing layer at runtime is Emscripten's JS GLUT (the
        # patched freeglut renames its own windowing to fg_glut* and only
        # contributes solids/fonts/extension queries).
        if grep -q "^gl-repl:" "${input_file_abs}"; then
            local em_flags="-include ${GL4ES_GL_H} -I${GL4ES_INCLUDE} -I${GLU_DIR}/include -I${FREEGLUT_INCLUDE} -DUSE_MGL_NAMESPACE -std=gnu99"
            # Bundle only sample.mp3 (the `make app` small-download policy)
            # — gl-repl's assets/ can symlink a full multi-hundred-MB
            # playlist that file_packager would happily follow.
            local repl_preload=""
            if [[ -f "${sample_dir}/assets/sample.mp3" ]]; then
                repl_preload="--preload-file ${sample_dir}/assets/sample.mp3@/assets/sample.mp3"
                echo -e "${CYAN}  bundling assets/sample.mp3 only → /assets${NC}"
            elif [[ -n "${preload_flag}" ]]; then
                repl_preload="${preload_flag}"
            fi

            pushd "${sample_dir}" > /dev/null
            emmake make "${out_html}" \
                CC=emcc \
                OBJDIR=build/emscripten \
                SAMPLE_BIN="${out_html}" \
                GL_HEADER_CFLAGS="${em_flags}" \
                GL_LDFLAGS="${BOOTSTRAP} ${GL4ES_LIB} ${GLU_LIB} ${FREEGLUT_LIB} -s USE_WEBGL2=1 -s FULL_ES2=1 -s INITIAL_MEMORY=${INITIAL_MEMORY} -s STACK_SIZE=${STACK_SIZE} -s GL_MAX_TEMP_BUFFER_SIZE=${GL_MAX_TEMP_BUFFER_SIZE} ${repl_preload}"
            local res=$?
            popd > /dev/null

            if [[ ${res} -eq 0 && -f "${out_html}" ]]; then
                echo -e "${GREEN}  ✓ ${sample_name}${NC}"
                return 0
            fi
            echo -e "${RED}  ✗ ${sample_name} (gl-repl Makefile build failed)${NC}"
            return 1
        fi

        # --- Legacy flat-layout Makefiles (claude4.6-opus-thinking era) ---
        # Common variables that Makefiles use for libraries
        local em_flags="-include ${GL4ES_GL_H} -I${GL4ES_INCLUDE} -I${GLU_DIR}/include -I${FREEGLUT_INCLUDE} -I${PROJECT_INCLUDE} -DGL_SILENCE_DEPRECATION -DUSE_MGL_NAMESPACE"
        local em_libs="${BOOTSTRAP} ${GL4ES_LIB} ${GLU_LIB} ${FREEGLUT_LIB} -lglut -s USE_WEBGL2=1 -s FULL_ES2=1 -s INITIAL_MEMORY=${INITIAL_MEMORY} -s STACK_SIZE=${STACK_SIZE} -s GL_MAX_TEMP_BUFFER_SIZE=${GL_MAX_TEMP_BUFFER_SIZE} ${preload_flag}"

        pushd "${sample_dir}" > /dev/null
        # Attempt to build 'sample' or 'best' target if they exist, else just default 'make'
        local make_target=""
        if grep -q "^sample:" Makefile; then
            make_target="sample"
        elif grep -q "^best:" Makefile; then
            make_target="best"
        fi

        emmake make ${make_target} \
            CC=emcc \
            CXX=em++ \
            COMMON_CFLAGS="-O2 ${em_flags}" \
            BUILD_CFLAGS="-O2 ${em_flags}" \
            CFLAGS="-O2 ${em_flags}" \
            LDFLAGS="${em_libs} -o ${out_html}" \
            LIBS="${em_libs} -o ${out_html}" \
            LDLIBS="${em_libs} -o ${out_html}" \
            GL_LDFLAGS="${em_libs} -o ${out_html}" \
            GLUT_GL_LDFLAGS="${em_libs} -o ${out_html}"

        local res=$?
        popd > /dev/null

        if [[ ${res} -eq 0 && -f "${out_html}" ]]; then
            echo -e "${GREEN}  ✓ ${sample_name}${NC}"
            return 0
        fi
        echo -e "${RED}  ✗ ${sample_name} (Makefile build failed)${NC}"
        return 1
    fi

    # --- Standard emcc Build (Single-File or multi-file discovery) ---
    echo -e "${CYAN}Building: ${input_file} → ${out_html}${NC}"

    local all_srcs=("${input_file_abs}")
    if ls "${sample_dir}"/*.c >/dev/null 2>&1; then
        local count
        count=$(ls "${sample_dir}"/*.c | wc -l)
        if [[ ${count} -gt 1 ]]; then
             echo -e "${YELLOW}  Multiple .c files found, including all except test_...${NC}"
             all_srcs=()
             for f in "${sample_dir}"/*.c; do
                 local b
                 b=$(basename "$f")
                 if [[ "${b}" != test_* ]]; then
                     all_srcs+=("$f")
                 fi
             done
        fi
    fi

    emcc "${all_srcs[@]}" "${BOOTSTRAP}" \
        -include "${GL4ES_GL_H}" \
        "${GL4ES_LIB}" \
        "${GLU_LIB}" \
        -I "${GL4ES_INCLUDE}" \
        -I "${GLU_DIR}/include" \
        -I "${FREEGLUT_INCLUDE}" \
        -I "${PROJECT_INCLUDE}" \
        -I "${sample_dir}" \
        -DUSE_MGL_NAMESPACE \
        -s USE_WEBGL2=1 \
        -s FULL_ES2=1 \
        -s INITIAL_MEMORY=${INITIAL_MEMORY} \
        -s STACK_SIZE=${STACK_SIZE} \
        -s GL_MAX_TEMP_BUFFER_SIZE=16777216 \
        -lglut \
        "${FREEGLUT_LIB}" \
        "${preload_args[@]}" \
        -o "${out_html}"

    if [[ $? -eq 0 ]]; then
        echo -e "${GREEN}  ✓ ${sample_name}${NC}"
        return 0
    else
        echo -e "${RED}  ✗ ${sample_name}${NC}"
        return 1
    fi
}

build_all() {
    local succeeded=0
    local failed=0
    local failures=()

    # Find all best.c and sample.c files up to 3 levels deep
    find "${PROJECT_SRC}" -maxdepth 3 \( -name "best.c" -o -name "sample.c" \) | while read -r best; do
        if [[ ! -f "${best}" ]]; then
            continue
        fi
        if build_one "${best}"; then
            ((succeeded++))
        else
            ((failed++))
            failures+=("${best}")
        fi
    done

    echo ""
    echo -e "${CYAN}── Build Summary ──${NC}"
    echo -e "  Succeeded: ${succeeded}"
    if [[ ${failed} -gt 0 ]]; then
        echo -e "  Failed:    ${failed}"
        for f in "${failures[@]}"; do
            echo -e "    - ${f}"
        done
    fi
}

serve() {
    echo ""
    echo -e "${CYAN}── Starting HTTP Server ──${NC}"
    echo -e "Serving at: ${GREEN}http://localhost:8080/${NC}"
    if [[ -n "${LAST_SAMPLE:-}" ]]; then
        echo -e "Direct link: ${GREEN}http://localhost:8080/${LAST_SAMPLE}/${NC}"
    fi
    echo "Press Ctrl+C to stop."
    echo ""
    cd "${OUT_DIR}"
    python3 -m http.server 8080
}

# ── Main ─────────────────────────────────────────────────────────────────────

NO_SERVE=0
BUILD_ALL=0
SOURCE_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            ;;
        --all)
            BUILD_ALL=1
            shift
            ;;
        --no-serve)
            NO_SERVE=1
            shift
            ;;
        *)
            SOURCE_FILE="$1"
            shift
            ;;
    esac
done

if [[ ${BUILD_ALL} -eq 0 && -z "${SOURCE_FILE}" ]]; then
    usage
fi

# Check all dependencies
check_emsdk
check_gl4es
check_glu
check_freeglut

LAST_SAMPLE=""

if [[ ${BUILD_ALL} -eq 1 ]]; then
    build_all
else
    # Derive sample name for serve URL
    local_src="$(cd "$(dirname "${SOURCE_FILE}")" && pwd)/$(basename "${SOURCE_FILE}")"
    local_dir="$(dirname "${local_src}")"
    project_src_abs="$(cd "${PROJECT_SRC}" && pwd)"
    rel_dir="${local_dir#"${project_src_abs}"/}"
    if [[ "${rel_dir}" == "${local_dir}" ]]; then
        LAST_SAMPLE="$(basename "${local_dir}")"
    else
        LAST_SAMPLE="${rel_dir//\//-}"
    fi
    if [[ -z "${LAST_SAMPLE}" || "${LAST_SAMPLE}" == "." ]]; then
        LAST_SAMPLE="$(basename "${local_dir}")"
    fi

    build_one "${SOURCE_FILE}"
fi

if [[ ${NO_SERVE} -eq 0 ]]; then
    serve
fi
