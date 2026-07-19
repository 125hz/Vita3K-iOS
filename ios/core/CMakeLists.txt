cmake_minimum_required(VERSION 3.22)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(Vita3KiOSCore LANGUAGES C CXX)
endif()

add_library(vita3k_ios_miniz STATIC ../../external/miniz/miniz.c)
target_include_directories(vita3k_ios_miniz PUBLIC ../../external/miniz)
if(MSVC)
    target_compile_definitions(vita3k_ios_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(vita3k_ios_core STATIC
    ../src/ArmExecution.cpp
    ../src/CoreBridge.cpp
    ../src/ExecutableLoader.cpp
    ../src/ExecutableProbe.cpp
    ../src/GuestHeap.cpp
    ../src/GuestMemory.cpp
    ../src/GuestThread.cpp
    ../src/HostDisplay.cpp
    ../src/HostFilesystem.cpp
    ../src/HostInput.cpp
    ../src/ImportBinder.cpp
    ../src/ModuleTableParser.cpp
    ../src/RelocationEngine.cpp
    ../src/VitaAppMetadata.cpp
    ../src/VitaAppArchive.cpp
    ../../vita3k/nids/src/nids.cpp
    ../../vita3k/packages/src/sfo.cpp
    ../../vita3k/packages/src/archive.cpp
    ../../vita3k/util/src/arm.cpp
    ../../external/boost/libs/filesystem/src/codecvt_error_category.cpp
    ../../external/boost/libs/filesystem/src/exception.cpp
    ../../external/boost/libs/filesystem/src/operations.cpp
    ../../external/boost/libs/filesystem/src/directory.cpp
    ../../external/boost/libs/filesystem/src/path.cpp
    ../../external/boost/libs/filesystem/src/path_traits.cpp
    ../../external/boost/libs/filesystem/src/portability.cpp
    ../../external/boost/libs/filesystem/src/unique_path.cpp
    ../../external/boost/libs/filesystem/src/utf8_codecvt_facet.cpp
)
if(WIN32)
    target_sources(vita3k_ios_core PRIVATE ../../external/boost/libs/filesystem/src/windows_file_codecvt.cpp)
endif()

target_compile_features(vita3k_ios_core PUBLIC cxx_std_23)
target_compile_definitions(vita3k_ios_core
    PUBLIC VITA3K_PLATFORM_IOS=1 SPDLOG_FMT_EXTERNAL=1 FMT_HEADER_ONLY=1 BOOST_FILESYSTEM_NO_LIB=1
    PRIVATE BOOST_FILESYSTEM_SOURCE=1
)
target_link_libraries(vita3k_ios_core PRIVATE vita3k_ios_miniz)
target_include_directories(vita3k_ios_core
    PUBLIC
        ../include
    PRIVATE
        ../../vita3k/nids/include
        ../../vita3k/packages/include
        ../../vita3k/util/include
        ../../external/boost
        ../../external/boost/libs/filesystem/src
        ../../external/fmt/include
        ../../external/spdlog/include
)
if(MSVC)
    target_compile_options(vita3k_ios_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(vita3k_ios_core PRIVATE -Wall -Wextra)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
    add_executable(vita3k_ios_core_smoke_tests tests/CoreSmokeTests.cpp)
    target_include_directories(vita3k_ios_core_smoke_tests PRIVATE ../../vita3k/packages/include)
    target_link_libraries(vita3k_ios_core_smoke_tests PRIVATE vita3k_ios_core)
    add_test(NAME vita3k_ios_core_smoke_tests COMMAND vita3k_ios_core_smoke_tests)
endif()
