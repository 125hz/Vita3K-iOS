#!/usr/bin/env bash
set -ex

# Generate the Xcode project for the x86_64 iOS Simulator.
# Kept in a separate binary dir so the arm64 device configuration in
# build-ios/ stays intact; see gen-ios.sh for that one.
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DVITA3K_BUILD_IOS=ON \
  -DVITA3K_IOS_DEPLOYMENT_TARGET=26.0 \
  -DVITA3K_IOS_LINK_CORE=ON
