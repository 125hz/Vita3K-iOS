#!/usr/bin/env bash
set -ex

# Generate the Xcode project for the real upstream-core iOS application
# (SwiftUI frontend + SDL3 + the actual Vita3K emulator core).
# Mirrors the configure step in .github/workflows/ios-upstream.yml.
#
# Prerequisites, all set up by the steps in that workflow:
#   * build-deps/vcpkg   bootstrapped at pinned commit 77df67c
#   * build-deps/moltenvk MoltenVK 1.4.1 ios-arm64 slice
#   * boost from Homebrew (VITA3K_FORCE_SYSTEM_BOOST)
#   * ios/patches/0001-oaknut-ios-rwx-jit.patch applied to external/dynarmic

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$REPO_ROOT/build-deps/vcpkg}"
MVK_ROOT="$REPO_ROOT/build-deps/moltenvk/MoltenVK/MoltenVK"

# tools/i18n/generate_lang_catalog.py uses an f-string containing backslashes,
# which is a syntax error before Python 3.12 (PEP 701 lifted the restriction).
# macOS ships /usr/bin/python3 3.9, so point CMake at a newer interpreter.
PYTHON3="${PYTHON3:-/usr/local/bin/python3.13}"

cmake -S . -B build-ios-upstream -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-ios \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
  -DVITA3K_BUILD_IOS_UPSTREAM_CORE=ON \
  -DVITA3K_BUILD_IOS=OFF \
  -DVITA3K_IOS_DEPLOYMENT_TARGET=26.0 \
  -DVITA3K_IOS_LINK_CORE=ON \
  -DVITA3K_IOS_MOLTENVK_LIBRARY="$MVK_ROOT/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" \
  -DVITA3K_IOS_MOLTENVK_INCLUDE_DIR="$MVK_ROOT/include" \
  -DPython3_EXECUTABLE="$PYTHON3" \
  -DVITA3K_FORCE_SYSTEM_BOOST=ON \
  -DUSE_DISCORD_RICH_PRESENCE=OFF \
  -DUSE_VITA3K_UPDATE=OFF \
  -DUSE_LTO=NEVER
