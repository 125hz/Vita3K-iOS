cmake_minimum_required(VERSION 3.22)

# iOS 26 minimum. The frontend is SwiftUI built around Liquid Glass, which is
# iOS 26 and later; supporting anything earlier meant carrying a parallel
# UIBlurEffect presentation of every surface and availability-guarding each
# API, for devices that cannot run the design the app is built around.
set(VITA3K_IOS_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum supported iOS version")
set(VITA3K_IOS_BUNDLE_ID "org.vita3k.experimental.ios" CACHE STRING "Application bundle identifier")
option(VITA3K_IOS_LINK_CORE "Link the first upstream Vita3K core slice" ON)

if(NOT VITA3K_IOS_LINK_CORE)
    message(FATAL_ERROR
        "Bootstrap-only builds are no longer supported. Set VITA3K_IOS_LINK_CORE=ON.")
endif()

if(VITA3K_BUILD_IOS_UPSTREAM_CORE)
    # Stage-2 upstream-core application: SDL3 owns the UIKit application
    # lifecycle and the frontend boots titles through the real emulator.
    # src/Swift/*.swift is the SwiftUI frontend; TsubomiBridge.mm is the only
    # thing it talks to (a plain Objective-C facade - see the bridging header
    # below for why Swift never sees the C++ core directly).
    file(GLOB VITA3K_IOS_SWIFT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Swift/*.swift")
    set(VITA3K_IOS_CUELUME_LICENSE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cuelume-LICENSE.txt")
    add_executable(Vita3KiOS MACOSX_BUNDLE
        src/NativeFrontend.mm
        src/TsubomiBridge.mm
        src/UpstreamMain.cpp
        src/VirtualController.mm
        ${VITA3K_IOS_SWIFT_SOURCES}
        ${VITA3K_IOS_CUELUME_LICENSE}
        ${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets
    )
    # Compile the app-icon asset catalog (Xcode generator runs actool) so the
    # Tsubomi flower is the home-screen icon.
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${VITA3K_IOS_CUELUME_LICENSE}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/Licenses")
    target_include_directories(Vita3KiOS PRIVATE include)
    target_compile_features(Vita3KiOS PRIVATE cxx_std_23)
    # -Wall/-Wextra are C-family flags; passing them to swiftc is an error, so
    # scope them to the languages that understand them.
    target_compile_options(Vita3KiOS PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wall>
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wextra>)
    target_compile_definitions(Vita3KiOS PRIVATE VITA3K_IOS_UPSTREAM_CORE=1)
    # Swift <-> Objective-C bridging.
    #   * The bridging header is what Swift sees: it exposes ONLY
    #     TsubomiBridge.h, a pure Objective-C facade. Swift deliberately does
    #     not get C++ interop here - the core's headers are heavy C++23 and
    #     importing them into every Swift file is both slow and fragile.
    #   * PRODUCT_MODULE_NAME is pinned so the generated header the
    #     Objective-C++ side imports has a stable name ("Tsubomi-Swift.h").
    #     Left to default it would be derived from OUTPUT_NAME ("Vita3K-iOS")
    #     with the hyphen mangled to an underscore.
    set_target_properties(Vita3KiOS PROPERTIES
        XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
        XCODE_ATTRIBUTE_PRODUCT_MODULE_NAME "Tsubomi"
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Swift/Tsubomi-Bridging-Header.h"
        XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL[variant=Release] "-O"
        XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL[variant=Debug] "-Onone"
        # Whole-module in Release lets the optimizer specialize across files,
        # which matters for SwiftUI view bodies.
        XCODE_ATTRIBUTE_SWIFT_COMPILATION_MODE[variant=Release] "wholemodule")
    target_link_libraries(Vita3KiOS PRIVATE
        vita3k
        audio
        io
        miniz
        np
        packages
        SDL3::SDL3
        "-framework SwiftUI"
        "-framework AVFoundation"
        "-framework CoreGraphics"
        "-framework Foundation"
        "-framework GameController"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework UIKit"
        "-framework UniformTypeIdentifiers"
    )
    # The renderer loads its screen-filter SPIR-V from
    # <static assets>/shaders-builtin at runtime; ship it inside the bundle.
    add_custom_command(TARGET Vita3KiOS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/vita3k/shaders-builtin"
            "$<TARGET_BUNDLE_CONTENT_DIR:Vita3KiOS>/shaders-builtin")
else()
    add_subdirectory(core)

    add_executable(Vita3KiOS MACOSX_BUNDLE
        src/AppDelegate.mm
        src/IOSInputAdapter.mm
        src/IOSLogger.mm
        src/IOSMetalRenderer.mm
        src/main.mm
    )

    target_include_directories(Vita3KiOS PRIVATE include)
    target_compile_features(Vita3KiOS PRIVATE cxx_std_23)
    target_compile_options(Vita3KiOS PRIVATE
        $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>
        -Wall
        -Wextra
    )
    target_compile_definitions(Vita3KiOS PRIVATE VITA3K_PLATFORM_IOS=1)
    target_link_libraries(Vita3KiOS PRIVATE
        vita3k_ios_core
        "-framework CoreGraphics"
        "-framework Foundation"
        "-framework GameController"
        "-framework Metal"
        "-framework MetalKit"
        "-framework UIKit"
        "-framework UniformTypeIdentifiers"
    )
endif()

if(VITA3K_BUILD_IOS_UPSTREAM_CORE)
    set_target_properties(Vita3KiOS PROPERTIES
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
endif()

set_target_properties(Vita3KiOS PROPERTIES
    OUTPUT_NAME "Vita3K-iOS"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${VITA3K_IOS_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VITA3K_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos"
    XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
