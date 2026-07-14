cmake_minimum_required(VERSION 3.22)

set(VITA3K_IOS_DEPLOYMENT_TARGET "17.0" CACHE STRING "Minimum supported iOS version")
set(VITA3K_IOS_BUNDLE_ID "org.vita3k.experimental.ios" CACHE STRING "Application bundle identifier")
option(VITA3K_IOS_LINK_CORE "Link the first upstream Vita3K core slice" ON)

if(NOT VITA3K_IOS_LINK_CORE)
    message(FATAL_ERROR
        "Bootstrap-only builds are no longer supported. Set VITA3K_IOS_LINK_CORE=ON.")
endif()

add_subdirectory(core)

add_executable(Vita3KiOS MACOSX_BUNDLE
    src/AppDelegate.mm
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
    "-framework Metal"
    "-framework MetalKit"
    "-framework UIKit"
)

set_target_properties(Vita3KiOS PROPERTIES
    OUTPUT_NAME "Vita3K-iOS"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${VITA3K_IOS_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VITA3K_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos"
    XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
