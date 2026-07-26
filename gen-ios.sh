#!/usr/bin/env bash
set -ex

# Generate the Xcode project for the device-only iOS application.
# Mirrors the configure step in .github/workflows/ios.yml.
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DVITA3K_BUILD_IOS=ON \
  -DVITA3K_IOS_DEPLOYMENT_TARGET=26.0 \
  -DVITA3K_IOS_LINK_CORE=ON
