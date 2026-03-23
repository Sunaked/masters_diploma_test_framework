#!/usr/bin/env bash
# ============================================================================
# setup.sh — установка зависимостей и сборка проекта.
#
# Использование:
#   chmod +x setup.sh
#   ./setup.sh
# ============================================================================

set -euo pipefail

echo "=== parallel-containers-bench: setup ==="

# ---- 1. vcpkg (если нет) ----
if [ ! -d "vcpkg" ]; then
    echo "[1/4] Installing vcpkg..."
    git clone https://github.com/microsoft/vcpkg.git
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
else
    echo "[1/4] vcpkg already present, skipping."
fi

export VCPKG_ROOT="$(pwd)/vcpkg"
export PATH="$VCPKG_ROOT:$PATH"

# ---- 2. Установка зависимостей через vcpkg ----
echo "[2/4] Installing dependencies via vcpkg..."
echo "  (This may take 10-30 minutes on first run)"

# sol2 тянет за собой Lua
vcpkg install sol2 fmt spdlog

# libcds — может не быть в vcpkg, тогда ставим из исходников
if ! vcpkg list | grep -q "libcds"; then
    echo "  libcds not in vcpkg, attempting system install..."
    if command -v apt-get &>/dev/null; then
        echo "  Trying apt..."
        sudo apt-get install -y libcds-dev 2>/dev/null || {
            echo "  Building libcds from source..."
            if [ ! -d "third_party/libcds" ]; then
                mkdir -p third_party
                git clone https://github.com/khizmax/libcds.git third_party/libcds
                cd third_party/libcds
                cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
                cmake --build build -j$(nproc)
                sudo cmake --install build
                cd ../..
            fi
        }
    fi
fi

# Concurrency Kit — обычно нет в vcpkg
if ! pkg-config --exists ck 2>/dev/null; then
    echo "  Installing Concurrency Kit from source..."
    if [ ! -d "third_party/ck" ]; then
        mkdir -p third_party
        git clone https://github.com/concurrencykit/ck.git third_party/ck
        cd third_party/ck
        ./configure
        make -j$(nproc)
        sudo make install
        cd ../..
    fi
fi

# ---- 3. CMake configure ----
echo "[3/4] Configuring with CMake..."
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# ---- 4. Build ----
echo "[4/4] Building..."
cmake --build build -j$(nproc)

echo ""
echo "=== Build complete ==="
echo "Run: ./build/bench config/default.lua"
