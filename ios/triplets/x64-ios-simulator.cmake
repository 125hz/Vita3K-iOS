# x86_64 iOS Simulator triplet.
# vcpkg ships arm64-ios-simulator (Apple silicon) and x64-ios (which omits the
# simulator sysroot, so it resolves to the device SDK). Intel Macs need the
# x64 + iphonesimulator combination, which upstream does not provide.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
