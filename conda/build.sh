#!/bin/bash
set -euxo pipefail

# GDAL のプラグイン置き場へ直接入れる（環境の gdal-config ではなく conda の $PREFIX を明示）
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DGDAL_PLUGIN_DIR="$PREFIX/lib/gdalplugins" \
    "$SRC_DIR"

cmake --build build --parallel "${CPU_COUNT}"
cmake --install build
