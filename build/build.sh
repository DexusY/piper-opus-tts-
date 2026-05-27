#!/usr/bin/env bash
# Builds tts_engine.so into the project root. Idempotent — re-running skips
# anything already in place.
#
# Falls back to source builds of libopus / libogg into dependencies/local/
# when the system -dev packages aren't there (no sudo required). pybind11
# goes into system pip, or a local venv at dependencies/.venv if PEP 668
# refuses the system install.

set -euo pipefail
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$BUILD_DIR/.." && pwd)"
DEPS_DIR="$ROOT_DIR/dependencies"
LOCAL_PREFIX="$DEPS_DIR/local"

mkdir -p "$DEPS_DIR"
cd "$DEPS_DIR"

# 1. piper source
if [ ! -d "piper/src/cpp" ]; then
    echo "==> Cloning piper source..."
    git clone --depth 1 https://github.com/rhasspy/piper.git
else
    echo "==> piper/src/cpp already present, skipping clone"
fi

# 2. piper-phonemize runtime libs, headers, and espeak-ng data
PHONEMIZE_URL="https://github.com/rhasspy/piper-phonemize/releases/download/2023.11.14-4/piper-phonemize_linux_x86_64.tar.gz"
PHONEMIZE_TAR="piper-phonemize_linux_x86_64.tar.gz"

if [ ! -f "include/espeak-ng/speak_lib.h" ] || [ ! -f "espeak-ng-data/phontab" ]; then
    echo "==> Downloading piper-phonemize (libs + headers + espeak-ng-data)..."
    wget -q --show-progress -O "$PHONEMIZE_TAR" "$PHONEMIZE_URL"
    mkdir -p include espeak-ng-data
    # Headers and .so files first.
    tar -xzf "$PHONEMIZE_TAR" --strip-components=1 -C . \
        --wildcards "*/include/*" "*/lib/*.so*"
    # Then the espeak-ng phoneme data, flattened from share/espeak-ng-data/.
    tmpd="$(mktemp -d)"
    tar -xzf "$PHONEMIZE_TAR" -C "$tmpd" \
        --wildcards "*/share/espeak-ng-data/*"
    cp -rT "$tmpd"/*/share/espeak-ng-data ./espeak-ng-data
    rm -rf "$tmpd"
    rm "$PHONEMIZE_TAR"
else
    echo "==> piper-phonemize headers + espeak-ng-data already present, skipping download"
fi

# 3. onnxruntime headers
ONNX_URL="https://github.com/microsoft/onnxruntime/releases/download/v1.14.1/onnxruntime-linux-x64-1.14.1.tgz"
ONNX_TAR="onnxruntime-linux-x64-1.14.1.tgz"

if [ ! -f "include/onnxruntime_cxx_api.h" ]; then
    echo "==> Downloading onnxruntime headers..."
    wget -q --show-progress -O "$ONNX_TAR" "$ONNX_URL"
    tar -xzf "$ONNX_TAR" --strip-components=2 -C include \
        --wildcards "*/include/*"
    rm "$ONNX_TAR"
else
    echo "==> onnxruntime headers already present, skipping download"
fi

# 4. spdlog headers
SPDLOG_URL="https://github.com/gabime/spdlog/archive/refs/tags/v1.12.0.tar.gz"
SPDLOG_TAR="spdlog-1.12.0.tar.gz"

if [ ! -f "include/spdlog/spdlog.h" ]; then
    echo "==> Downloading spdlog headers..."
    wget -q --show-progress -O "$SPDLOG_TAR" "$SPDLOG_URL"
    tar -xzf "$SPDLOG_TAR" --strip-components=2 -C include \
        --wildcards "*/include/spdlog*"
    rm "$SPDLOG_TAR"
else
    echo "==> spdlog headers already present, skipping download"
fi

# 5. libogg + libopus — only build from source if the system ones are missing
need_local_audio=0
if pkg-config --exists opus 2>/dev/null && pkg-config --exists ogg 2>/dev/null; then
    echo "==> system pkg-config found opus + ogg, skipping local build"
else
    need_local_audio=1
fi

if [ "$need_local_audio" -eq 1 ]; then
    if [ ! -f "$LOCAL_PREFIX/lib/pkgconfig/ogg.pc" ]; then
        echo "==> Building libogg from source into $LOCAL_PREFIX..."
        OGG_VER="1.3.5"
        wget -q --show-progress -O "libogg-${OGG_VER}.tar.gz" \
            "https://downloads.xiph.org/releases/ogg/libogg-${OGG_VER}.tar.gz"
        tar -xzf "libogg-${OGG_VER}.tar.gz"
        (
            cd "libogg-${OGG_VER}"
            ./configure --prefix="$LOCAL_PREFIX" --disable-static --enable-shared >/dev/null
            make -j"$(nproc)" >/dev/null
            make install >/dev/null
        )
        rm -rf "libogg-${OGG_VER}" "libogg-${OGG_VER}.tar.gz"
    else
        echo "==> local libogg already built, skipping"
    fi

    if [ ! -f "$LOCAL_PREFIX/lib/pkgconfig/opus.pc" ]; then
        echo "==> Building libopus from source into $LOCAL_PREFIX..."
        OPUS_VER="1.5.2"
        wget -q --show-progress -O "opus-${OPUS_VER}.tar.gz" \
            "https://downloads.xiph.org/releases/opus/opus-${OPUS_VER}.tar.gz"
        tar -xzf "opus-${OPUS_VER}.tar.gz"
        (
            cd "opus-${OPUS_VER}"
            ./configure --prefix="$LOCAL_PREFIX" --disable-static --enable-shared \
                --disable-doc --disable-extra-programs >/dev/null
            make -j"$(nproc)" >/dev/null
            make install >/dev/null
        )
        rm -rf "opus-${OPUS_VER}" "opus-${OPUS_VER}.tar.gz"
    else
        echo "==> local libopus already built, skipping"
    fi

    export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
fi

# 6. pybind11 — prefer whatever's already importable, then system pip,
#    and only fall back to a local venv if PEP 668 blocks the install.
PYBIND11_CMAKEDIR=""
if python3 -c "import pybind11" 2>/dev/null; then
    PYBIND11_CMAKEDIR="$(python3 -m pybind11 --cmakedir)"
    PY_FOR_CMAKE=python3
else
    if pip install -q pybind11 2>/dev/null; then
        PYBIND11_CMAKEDIR="$(python3 -m pybind11 --cmakedir)"
        PY_FOR_CMAKE=python3
    else
        VENV_DIR="$DEPS_DIR/.venv"
        if [ ! -x "$VENV_DIR/bin/python" ]; then
            echo "==> Creating local venv at $VENV_DIR for pybind11..."
            python3 -m venv "$VENV_DIR"
        fi
        "$VENV_DIR/bin/pip" install -q pybind11
        PYBIND11_CMAKEDIR="$("$VENV_DIR/bin/python" -m pybind11 --cmakedir)"
        PY_FOR_CMAKE="$VENV_DIR/bin/python"
    fi
fi
echo "==> pybind11 cmakedir: $PYBIND11_CMAKEDIR"

# 7. Build the module
echo "==> Building tts_engine..."
OUT_DIR="$BUILD_DIR/out"
rm -rf "$OUT_DIR"
cmake -B "$OUT_DIR" -S "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPython3_EXECUTABLE="$PY_FOR_CMAKE" \
    -Dpybind11_DIR="$PYBIND11_CMAKEDIR"
cmake --build "$OUT_DIR" -j"$(nproc)"

echo "==> Done. tts_engine.*.so → $ROOT_DIR/"
