#!/usr/bin/env bash
set -ex

# Experimental: the upstream-core application built for the x86_64 iOS
# Simulator, so the SwiftUI frontend can be exercised on an Intel Mac.
#
# This is NOT a configuration upstream supports. Differences from
# gen-ios-upstream.sh (the device build that CI verifies):
#
#   * vcpkg has no x64 + iphonesimulator triplet, so one is supplied as an
#     overlay from ios/triplets/.
#   * MoltenVK-ios.tar ships only an ios-arm64 slice; the simulator needs the
#     ios-arm64_x86_64-simulator slice out of MoltenVK-all.tar.
#   * CMAKE_SYSTEM_PROCESSOR must be set explicitly: the root CMakeLists
#     forces it to arm64 when the Xcode generator leaves it empty, which would
#     select dynarmic's arm64 JIT backend instead of the x86_64 one.
#   * ios/patches/0001-oaknut-ios-rwx-jit.patch targets dynarmic's arm64
#     backend; those files are not compiled for an x86_64 target.
#
# JIT is expected to be the limiting factor at runtime, not compilation.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$REPO_ROOT/build-deps/vcpkg}"
MVK_ALL="$REPO_ROOT/build-deps/moltenvk-all/MoltenVK/MoltenVK"
PYTHON3="${PYTHON3:-/usr/local/bin/python3.13}"

cmake -S . -B build-ios-upstream-sim -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$REPO_ROOT/ios/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-ios-simulator \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
  -DVITA3K_BUILD_IOS_UPSTREAM_CORE=ON \
  -DVITA3K_BUILD_IOS=OFF \
  -DVITA3K_IOS_DEPLOYMENT_TARGET=26.0 \
  -DVITA3K_IOS_LINK_CORE=ON \
  -DVITA3K_IOS_MOLTENVK_LIBRARY="$MVK_ALL/static/MoltenVK.xcframework/ios-arm64_x86_64-simulator/libMoltenVK.a" \
  -DVITA3K_IOS_MOLTENVK_INCLUDE_DIR="$MVK_ALL/include" \
  -DPython3_EXECUTABLE="$PYTHON3" \
  -DVITA3K_FORCE_SYSTEM_BOOST=ON \
  -DUSE_DISCORD_RICH_PRESENCE=OFF \
  -DUSE_LTO=NEVER
