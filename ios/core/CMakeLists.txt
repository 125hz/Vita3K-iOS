cmake_minimum_required(VERSION 3.22)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(Vita3KiOSCore LANGUAGES CXX)
endif()

add_library(vita3k_ios_core STATIC
    ../src/ArmExecution.cpp
    ../src/CoreBridge.cpp
    ../src/ExecutableLoader.cpp
    ../src/ExecutableProbe.cpp
    ../src/GuestMemory.cpp
    ../src/HostFilesystem.cpp
    ../src/ModuleTableParser.cpp
    ../src/RelocationEngine.cpp
    ../../vita3k/nids/src/nids.cpp
    ../../vita3k/util/src/arm.cpp
)

target_compile_features(vita3k_ios_core PUBLIC cxx_std_23)
target_compile_definitions(vita3k_ios_core PUBLIC VITA3K_PLATFORM_IOS=1)
target_include_directories(vita3k_ios_core
    PUBLIC
        ../include
    PRIVATE
        ../../vita3k/nids/include
        ../../vita3k/util/include
)
if(MSVC)
    target_compile_options(vita3k_ios_core PRIVATE /W4)
else()
    target_compile_options(vita3k_ios_core PRIVATE -Wall -Wextra)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
    add_executable(vita3k_ios_core_smoke_tests tests/CoreSmokeTests.cpp)
    target_link_libraries(vita3k_ios_core_smoke_tests PRIVATE vita3k_ios_core)
    add_test(NAME vita3k_ios_core_smoke_tests COMMAND vita3k_ios_core_smoke_tests)
endif()
