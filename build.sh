#!/bin/bash

set -e

echo "🚀 Building libaio..."
echo "🔧 Checking dependencies..."

command -v clang++ >/dev/null 2>&1 || { echo "❌ clang++ not found."; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "❌ cmake not found."; exit 1; }

mkdir -p build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_C_COMPILER=clang \
         -DCMAKE_CXX_COMPILER=clang++ \
         -DLIBAIO_ENABLE_TESTS=OFF \
         -DBUILD_SHARED_LIBS=ON

make -j$(nproc)

echo "✅ Build successful! Library: $(pwd)/libaio.so"