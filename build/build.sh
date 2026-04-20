#!/usr/bin/env bash
# Builds tts_engine.so into the project root.
# Safe to re-run: skips steps that are already done.

set -euo pipefail
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$BUILD_DIR/.." && pwd)"
DEPS_DIR="$ROOT_DIR/dependencies"

cd "$DEPS_DIR"

# ── 1. piper source ───────────────────────────────────────────────────────────
if [ ! -d "piper/src/cpp" ]; then
    echo "==> Cloning piper source..."
    git clone --depth 1 https://github.com/rhasspy/piper.git
else
    echo "==> piper/src/cpp already present, skipping clone"
fi

# ── 2. Runtime libs + headers from piper-phonemize ───────────────────────────
PHONEMIZE_URL="https://github.com/rhasspy/piper-phonemize/releases/download/2023.11.14-4/piper-phonemize_linux_x86_64.tar.gz"
PHONEMIZE_TAR="piper-phonemize_linux_x86_64.tar.gz"

if [ ! -f "include/espeak-ng/speak_lib.h" ]; then
    echo "==> Downloading piper-phonemize (libs + headers)..."
    wget -q --show-progress -O "$PHONEMIZE_TAR" "$PHONEMIZE_URL"
    mkdir -p include
    tar -xzf "$PHONEMIZE_TAR" --strip-components=1 -C . \
        --wildcards "*/include/*" "*/lib/*.so*"
    rm "$PHONEMIZE_TAR"
else
    echo "==> piper-phonemize headers already present, skipping download"
fi

# ── 3. onnxruntime headers ────────────────────────────────────────────────────
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

# ── 4. spdlog headers ─────────────────────────────────────────────────────────
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

# ── 5. pybind11 ───────────────────────────────────────────────────────────────
pip install -q pybind11

# ── 6. Build tts_engine ───────────────────────────────────────────────────────
echo "==> Building tts_engine..."
OUT_DIR="$BUILD_DIR/out"
rm -rf "$OUT_DIR"
cmake -B "$OUT_DIR" -S "$BUILD_DIR" \
    -Dpybind11_DIR="$(python3 -m pybind11 --cmakedir)"
cmake --build "$OUT_DIR" -j"$(nproc)"
