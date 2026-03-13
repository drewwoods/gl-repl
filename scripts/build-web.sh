#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SRC_DIR:-${HOME}/src}"
PROJECT_SRC="${SCRIPT_DIR}/../src"
PROJECT_INCLUDE="${SCRIPT_DIR}/../include"
BOOTSTRAP="./gl4es_bootstrap.c"
OUT_DIR="${SCRIPT_DIR}/out"

# -- Emscripten environment -------------------------------
STACK_SIZE=$((1024*1024)) # 1MB stack size for complex samples
INITIAL_MEMORY=$((1024 * 1024 * 128)) # 128MB initial memory for complex samples

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
    echo "Usage: $(basename "$0") [OPTIONS] <source.c>"
    echo ""
    echo "Compile OpenGL C samples to WebAssembly using Emscripten + gl4es + GLU"
    echo ""
    echo "Options:"
    echo "  --all       Build all best.c files from ../src/*/"
    echo "  --no-serve  Build only, don't start HTTP server"
    echo "  --help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") ../src/sine-spin/best.c"
    echo "  $(basename "$0") --all"
    exit 0
}

# ── Dependency Checks ────────────────────────────────────────────────────────

check_emsdk() {
    if [[ ! -f "${SRC_DIR}/emsdk/emsdk" ]]; then
        echo -e "${RED}Error: emsdk not found at ${SRC_DIR}/emsdk/${NC}"
        echo ""
        echo "Install emsdk:"
        echo "  cd ${SRC_DIR}"
        echo "  git clone https://github.com/emscripten-core/emsdk.git"
        echo "  cd emsdk"
        echo "  ./emsdk install latest"
        echo "  ./emsdk activate latest"
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
        echo "  cd ${SRC_DIR}"
        echo "  git clone https://github.com/ptitSeb/gl4es.git"
        echo "  cd gl4es"
        echo "  mkdir build_wasm && cd build_wasm"
        echo "  emcmake cmake .. -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON"
        echo "  emmake make"
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
    local src_file="$1"
    local src_file_abs
    src_file_abs="$(cd "$(dirname "$src_file")" && pwd)/$(basename "$src_file")"

    if [[ ! -f "${src_file_abs}" ]]; then
        echo -e "${RED}Error: Source file not found: ${src_file}${NC}"
        return 1
    fi

    # Derive sample name from parent directory
    local sample_dir
    sample_dir="$(dirname "${src_file_abs}")"
    # Walk up to find the directory directly under PROJECT_SRC
    local project_src_abs
    project_src_abs="$(cd "${PROJECT_SRC}" && pwd)"
    local rel_path="${sample_dir#"${project_src_abs}"/}"
    local sample_name="${rel_path%%/*}"

    if [[ -z "${sample_name}" ]]; then
        sample_name="$(basename "${sample_dir}")"
    fi

    local out_path="${OUT_DIR}/${sample_name}"
    mkdir -p "${out_path}"

    echo -e "${CYAN}Building: ${src_file} → ${out_path}/index.html${NC}"

    emcc "${src_file_abs}" "${BOOTSTRAP}" \
        -include "${GL4ES_GL_H}" \
        "${GL4ES_LIB}" \
        "${GLU_LIB}" \
        -I "${GL4ES_INCLUDE}" \
        -I "${FREEGLUT_INCLUDE}" \
        -I "${PROJECT_INCLUDE}" \
        -s USE_WEBGL2=1 \
        -s FULL_ES2=1 \
        -s INITIAL_MEMORY=${INITIAL_MEMORY} \
        -s STACK_SIZE=${STACK_SIZE} \
        -lglut \
        "${FREEGLUT_LIB}" \
        -o "${out_path}/index.html"
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

    for best in "${PROJECT_SRC}"/*/best.c; do
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
    echo -e "${GREEN}  Succeeded: ${succeeded}${NC}"
    if [[ ${failed} -gt 0 ]]; then
        echo -e "${RED}  Failed:    ${failed}${NC}"
        for f in "${failures[@]}"; do
            echo -e "${RED}    - ${f}${NC}"
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
    project_src_abs="$(cd "${PROJECT_SRC}" && pwd)"
    rel="${local_src#"${project_src_abs}"/}"
    LAST_SAMPLE="${rel%%/*}"

    build_one "${SOURCE_FILE}"
fi

if [[ ${NO_SERVE} -eq 0 ]]; then
    serve
fi
